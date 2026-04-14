# Proton 写入架构深度解读

> 目标读者：熟悉 Vespa / Proton 的工程师。  
> 目标：把 FeedView 的三层纵向继承、三条横向 Sub-DB、以及下游的 DocumentStore / AttributeVector / IndexMaintainer / DocumentMetaStore / FlushEngine / TLS / Distributor bucket 管理串成一张完整的地图，并对不同 `字段类型 × 存储模式` 的组合拆解出真实的写入代码路径，最后分析写入优化空间、读写互相影响、以及节点故障恢复与空节点冷启动。  
> 所有代码位置都以 `文件:行号` 形式给出，便于对照。

## 目录

- [§1 三层 FeedView 继承](#1-三层-feedview-继承)
- [§2 横向 Sub-DB 架构](#2-横向-sub-db-架构ready--notready--removed)
- [§3 核心组件矩阵与线程模型](#3-核心组件矩阵与线程模型)
- [§4 字段类型 × 存储模式 组合矩阵](#4-字段类型--存储模式-组合矩阵)
- [§5 写入时序：Put / Update / Remove / Move](#5-写入时序put--update--remove--move)
- [§6 Bug 解读：attribute 字段与 docstore 的纠缠](#6-bug-解读attribute-字段与-docstore-的纠缠)
- [§7 Flush / Fusion / Compact 与写入的耦合](#7-flush--fusion--compact-与写入的耦合)
- [§8 读写互相影响：RCU、Generation、可见性延迟](#8-读写互相影响rcugeneration可见性延迟)
- [§9 TLS 回放、冷启动、空节点恢复](#9-tls-回放冷启动空节点恢复)
- [§10 Distributor / Bucket 管理接口](#10-distributor--bucket-管理接口)
- [§11 写入优化分析](#11-写入优化分析)

---

## §1 三层 FeedView 继承

Proton 的写入入口 `IFeedView` 位于 `searchcore/proton/server/ifeedview.h:26-69`。它把"一次写操作"抽象为两阶段：

- **prepare 阶段**（在 master 线程执行）——分配 LID、校验 GID、登记到 BucketDB；  
- **handle 阶段**（在 master 线程下发，但会 fan-out 到 summary/attribute/index 三个工作线程）——把数据真正落到组件里。

三层继承实现 `StoreOnlyFeedView → FastAccessFeedView → SearchableFeedView`，每层只给**自己多出来的职责**加代码，下面是职责叠加图：

```mermaid
classDiagram
    class IFeedView {
        <<interface>>
        +preparePut(PutOperation)
        +handlePut(token, PutOperation)
        +prepareUpdate(UpdateOperation)
        +handleUpdate(token, UpdateOperation)
        +prepareRemove(RemoveOperation)
        +handleRemove(token, RemoveOperation)
        +prepareMove(MoveOperation)
        +handleMove(MoveOperation, doneCtx)
        +handleDeleteBucket(DeleteBucketOperation)
        +heartBeat(SerialNum)
        +forceCommit(CommitParam, doneCtx)
        +handlePruneRemovedDocuments(...)
        +handleCompactLidSpace(...)
    }

    class StoreOnlyFeedView {
        -IDocumentMetaStoreContext _dmsCtx
        -ISummaryAdapter _summaryAdapter
        -PendingLidTracker _pendingLidsForDocStore
        -PendingLidTracker _pendingLidsForCommit
        +internalPut()
        +internalUpdate()
        +internalRemove()
        +putSummary()  : final
        +removeSummary(): final
        +makeUpdatedDocument()
        +putAttributes() : virtual, empty
        +putIndexedFields(): virtual, empty
        +updateAttributes(): virtual, empty
        +updateIndexedFields(): virtual, empty
        +removeAttributes(): virtual, empty
        +removeIndexedFields(): virtual, empty
    }

    class FastAccessFeedView {
        -IAttributeWriter _attributeWriter
        -DocIdLimit _docIdLimit
        +putAttributes()  : override
        +updateAttributes(): override
        +removeAttributes(): override
        +heartBeatAttributes(): override
        +internalForceCommit(): override
    }

    class SearchableFeedView {
        -IIndexWriter _indexWriter
        +putIndexedFields()   : override
        +updateIndexedFields(): override
        +removeIndexedFields(): override
        +heartBeatIndexedFields(): override
        +internalForceCommit(): override
    }

    IFeedView <|.. StoreOnlyFeedView
    StoreOnlyFeedView <|-- FastAccessFeedView
    FastAccessFeedView <|-- SearchableFeedView
```

### §1.1 StoreOnlyFeedView — 地基

文件：`searchcore/proton/server/storeonlyfeedview.{h,cpp}`（`h:44-248`，`cpp` 整文件）

它持有**所有写入都需要的最小集合**：

| 成员 | 职责 |
|---|---|
| `IDocumentMetaStoreContext` | GID↔LID 映射、bucket 归属、timestamp |
| `ISummaryAdapter` | 指向 `LogDocumentStore`，负责整行 doc 的序列化落盘 |
| `PendingLidTracker _pendingLidsForDocStore` | 保证 docstore 写入顺序、flush 可见性 |
| `PendingLidTracker _pendingLidsForCommit` | 保证 force-commit 时所有组件都 drain 干净 |
| `_writeService`, `_repo`, `_schema`, `_params` | 线程服务、文档类型库、schema、subdb 参数 |

关键入口：

- `internalPut` (`cpp:230-274`) 顺序调用 `putMetaData → putSummary → putAttributes → putIndexedFields`，后两个在 base 是空实现；
- `internalRemove` (`cpp` 中 `handleRemove`) 做 tombstone 登记 + `removeSummary`；
- `internalUpdate` (`cpp:303-464`) 更复杂，后面 §5.3 专门讲。

它还定义一个小而关键的辅助类 `UpdateScope`（`h:122-135`）：

```cpp
class UpdateScope : public IFieldUpdateCallback {
    const search::index::Schema *_schema;
    bool _indexedFields;
    bool _nonAttributeFields;
    void onUpdateField(stringref name, const AttributeVector *attr) override {
        if (!_nonAttributeFields && (attr == nullptr || !attr->isUpdateableInMemoryOnly()))
            _nonAttributeFields = true;
        if (!_indexedFields && _schema->isIndexField(name))
            _indexedFields = true;
    }
};
```

这块是 §6 bug 分析的起点——它决定了 update 是"纯 attribute 快路径"还是"重建 doc 慢路径"。

### §1.2 FastAccessFeedView — 加入 attribute 写入

文件：`searchcore/proton/server/fast_access_feed_view.{h,cpp}`（`h:17-66`）

在 `StoreOnlyFeedView` 之上新增一个 `IAttributeWriter`。它只 override **attribute 相关**的四个虚函数：

```cpp
// fast_access_feed_view.cpp:22-25
void FastAccessFeedView::putAttributes(SerialNum s, Lid lid, const Document &doc,
                                       OnPutDoneType onWriteDone) {
    _attributeWriter->put(s, doc, lid, onWriteDone);
    _docIdLimit.bumpUpLimit(lid + 1);
}
```

`updateAttributes` 有两个重载：纯 `DocumentUpdate` 形式（常见的 partial update）和 `FutureDoc` 形式（当某些 struct field 需要完整重建 doc 才能拿到新值时）。

`DocIdLimit` 是只增的高水位，搜索侧以此界定"哪些 LID 现在已经有 attribute 数据"。

### §1.3 SearchableFeedView — 加入倒排索引

文件：`searchcore/proton/server/searchable_feed_view.{h,cpp}`（`h:17-64`）

再加一个 `IIndexWriter`，并 override `putIndexedFields / updateIndexedFields / removeIndexedFields / heartBeatIndexedFields`。注意 `putIndexedFields` 的实现（`cpp:43-80`）会把真实工作 `execute()` 到 **index 线程**上：

```cpp
void SearchableFeedView::putIndexedFields(SerialNum serialNum, Lid lid,
                                          const FutureDoc &futureDoc,
                                          OnOperationDoneType onWriteDone) {
    _writeService.index().execute(makeLambdaTask([=, this]() {
        performIndexPut(serialNum, lid, futureDoc, onWriteDone);
    }));
}
```

— 即便调用方在 master 线程，索引的实际插入永远在 index 线程里串行发生（避免锁）。

### §1.4 三层叠加的语义总览

一次 put 经过三层的职责分布：

```mermaid
flowchart LR
    A[PutOperation] --> B[StoreOnlyFeedView::internalPut]
    B --> M[putMetaData<br/>DocumentMetaStore]
    B --> S[putSummary<br/>LogDocumentStore]
    B --> AA[putAttributes<br/>虚函数]
    B --> IX[putIndexedFields<br/>虚函数]
    AA --> FA[FastAccessFeedView::putAttributes<br/>AttributeWriter]
    IX --> SF[SearchableFeedView::putIndexedFields<br/>IndexWriter]

    classDef base fill:#FFE4B5,stroke:#333;
    classDef fa fill:#B5E7FF,stroke:#333;
    classDef sf fill:#C8FFC8,stroke:#333;
    class B,M,S base;
    class AA,FA fa;
    class IX,SF sf;
```

---

## §2 横向 Sub-DB 架构（Ready / NotReady / Removed）

一个 `DocumentDB` 持有三个 `IDocumentSubDB`（`searchcore/proton/server/idocumentsubdb.h:55-126`），分别对应三个文档"状态域"：

| Sub-DB | 目录名 | FeedView | 含义 |
|---|---|---|---|
| Ready | `0.ready` | **SearchableFeedView** | 可搜索、attribute 已加载、index 已构建 |
| Removed | `1.removed` | **StoreOnlyFeedView** | 墓碑，仅存 meta+summary，用于防止误重建 |
| NotReady | `2.notready` | **FastAccessFeedView** | 已写入、但尚未进 ready（冷 attribute、无 index） |

这三者由 `CombiningFeedView` 统一路由（`searchcore/proton/server/combiningfeedview.{h,cpp}`）。它根据 `IBucketStateCalculator` 判定一个 bucket 该进哪个 sub-db，并在 `handleMove` 里把一条 doc 从源 sub-db 迁移到目的 sub-db（例如一个原本 NotReady 的 bucket 变成 Ready 时，就逐条 Move）。

### §2.1 Sub-DB 组件对比

```mermaid
flowchart TB
    subgraph Ready[0.ready - SearchableDocSubDB]
        R_fv[SearchableFeedView]
        R_dms[DocumentMetaStore]
        R_ds[SummaryManager / DocStore]
        R_am[AttributeManager]
        R_im[IndexMaintainer<br/>Memory+Disk Index]
    end
    subgraph NotReady[2.notready - FastAccessDocSubDB]
        N_fv[FastAccessFeedView]
        N_dms[DocumentMetaStore]
        N_ds[SummaryManager / DocStore]
        N_am[AttributeManager<br/>仅 fast-access]
    end
    subgraph Removed[1.removed - StoreOnlyDocSubDB]
        X_fv[StoreOnlyFeedView]
        X_dms[DocumentMetaStore]
        X_ds[SummaryManager / DocStore]
    end

    CFV[CombiningFeedView] --> Ready
    CFV --> NotReady
    CFV --> Removed
```

注意到**每个 sub-db 都有自己独立的 DocumentMetaStore 和 DocumentStore**——这是 Vespa 选择"三域物理隔离"的架构决定，带来的好处：

- Ready 和 NotReady 的 LID 空间完全独立，不会因为一个 doc 从 NotReady 迁到 Ready 而引起 LID 空洞；
- Removed 单独留墓碑，不会占用 Ready 的内存 attribute 空间；
- 每个 sub-db 可以独立 flush/compact/fusion。

坏处：一个 put/update 路由错 sub-db 时表现非常诡异（比如把本应 Ready 的文档写进 NotReady，会导致搜索不到却可以 visit），这也是 CombiningFeedView 路由逻辑必须严格测试的原因。

### §2.2 Sub-DB 类的继承

```mermaid
classDiagram
    class IDocumentSubDB {
        <<interface>>
        +getFeedView()
        +getAttributeManager()
        +getIndexManager()
        +getSummaryAdapter()
        +getDocumentMetaStoreContext()
        +createInitializer()
        +setup() / initViews()
        +getFlushTargets()
    }
    class StoreOnlyDocSubDB {
        -DocumentMetaStore _dms
        -SummaryManager _rSummaryMgr
        -ISummaryAdapter _summaryAdapter
        +initFeedView() creates StoreOnlyFeedView
    }
    class FastAccessDocSubDB {
        -AttributeManager _initAttrMgr
        -DocIdLimit _docIdLimit
        +initFeedView() creates FastAccessFeedView
    }
    class SearchableDocSubDB {
        -IIndexManager _indexMgr
        -IIndexWriter _indexWriter
        -SearchableFeedView _rFeedView
        +initFeedView() creates SearchableFeedView
    }

    IDocumentSubDB <|.. StoreOnlyDocSubDB
    StoreOnlyDocSubDB <|-- FastAccessDocSubDB
    FastAccessDocSubDB <|-- SearchableDocSubDB
```

**关键对齐**：Sub-DB 继承关系与 FeedView 继承关系是**一一对应**的。`SearchableDocSubDB::initFeedView()` 会构造一个 `SearchableFeedView`，并把自己持有的 attribute manager / index manager / summary adapter 注入进去。

### §2.3 Move 操作：跨 Sub-DB 的数据搬迁

`MoveOperation` 表达"把 LID X 从 source subdb 迁到 target subdb"。`CombiningFeedView::handleMove`（`combiningfeedview.cpp`）会让**源 subdb 做 remove，目标 subdb 做 put**——两者共享同一个 `onDone` 回调以保证原子性。

典型触发点：

- BucketMoveJob 检测某 bucket 从 not-ready→ready 应该发生（例如 attribute 已预热）；
- Distributor 通知 bucket 从本节点迁出（把数据写进 Removed 作为墓碑）。

---

## §3 核心组件矩阵与线程模型

### §3.1 组件职责矩阵

| 组件 | 文件 | 存什么 | 写入入口 | 持久化 |
|---|---|---|---|---|
| DocumentMetaStore | `proton/documentmetastore/documentmetastore.{h,cpp}` (`h:35-146+`) | LID↔GID、BucketId、Timestamp、DocSize | `put / remove / move` | attribute-style flush + bucketdb |
| LogDocumentStore | `searchlib/docstore/logdocumentstore.h:18-57` | 整行 serialized Document | `put / remove / get` | 顺序追加的 chunk 文件 + bloat/spread compact |
| SummaryAdapter | `proton/server/summaryadapter.{h,cpp}` | LogDocumentStore 的 thin wrapper | `put / remove / get / heartBeat` | 委托给 SummaryManager |
| AttributeManager | `proton/attribute/attributemanager.h:31+` | 全部 AttributeVector 集合 | `getWritableAttribute()` | per-attribute `.dat`+`.idx` |
| IAttributeWriter | `proton/attribute/i_attribute_writer.h:24-68` | 批量路由到每个 attribute field | `put / remove / update / heartBeat / forceCommit` | 委托 AttributeVector |
| IndexManager | `proton/index/indexmanager.h:32+` | MemoryIndex + DiskIndex 集合 + 进行 fusion | `putDocument / removeDocuments / commit / runFusion` | disk index 目录 + fusion |
| IIndexWriter | `proton/index/i_index_writer.h` | MemoryIndex 写入 | `put / removeDocs / commit / heartBeat` | 委托 IndexMaintainer |

### §3.2 线程模型：5 类执行器

文件：`searchcore/proton/server/executorthreadingservice.h:19-91`

| 执行器 | 用途 | 并发度 |
|---|---|---|
| `_masterExecutor` | 所有 prepare/handle 调度入口；单线程保证 serialNum 顺序 | 1 |
| `_indexExecutor` | MemoryIndex 写入；单线程避免词典锁竞争 | 1 |
| `_summaryExecutor` | LogDocumentStore 写入 | 1（可配） |
| `_attributeFieldWriter` | **per-field** 串行，ISequencedTaskExecutor | N（~ CPU 核） |
| `_sharedExecutor` | Doc 重建、makeUpdatedDocument 等可并行任务 | 共享线程池 |

fork-join 图：

```mermaid
sequenceDiagram
    participant FH as FeedHandler (master)
    participant FV as FeedView (master)
    participant SUM as SummaryExecutor
    participant ATT as AttrFieldWriter (per-field)
    participant IDX as IndexExecutor
    participant DONE as OnPutDoneContext

    FH->>FV: preparePut (assign LID)
    FH->>FV: handlePut
    FV->>FV: putMetaData (sync, master)
    FV-->>SUM: putSummary task
    FV-->>ATT: putAttributes tasks (fan-out per field)
    FV-->>IDX: putIndexedFields task
    SUM-->>DONE: ack
    ATT-->>DONE: ack (N fields)
    IDX-->>DONE: ack
    DONE->>FH: all-done → ack FeedToken → TLS commit
```

### §3.3 PendingLidTracker 与 commit barrier

`storeonlyfeedview.h:143-144` 持有两个 tracker：

- `_pendingLidsForDocStore`：跟踪"已经发给 summary 线程、但还没真正落到 docstore"的 lid。搜索/visit 拉 doc 时如果 lid 在 pending 集合里，必须等它 drain。
- `_pendingLidsForCommit`：跟踪"整条 put 的 fan-out 还没全部 ack"的 lid。`forceCommit` 必须等它清零。

这套机制是 **write→search 可见性**的关键（§8 会再展开）。

---

## §3.END 中场小结

到这里为止，架构骨架清楚了：

- **纵向**：`StoreOnly → FastAccess → Searchable` 逐层叠加 DocStore → Attribute → Index；
- **横向**：`Ready / NotReady / Removed` 三个 sub-db 分别用其中某一层作为 FeedView；
- **线程**：master fan-out 到 summary/attribute/index 三组 worker，通过 OnDone 汇总；
- **一致性**：PendingLidTracker 负责写入顺序与可见性屏障。

下一章开始，我们拿具体的 `字段类型 × 存储模式` 组合去过这条链路，看看哪些分支真正被激活。

---

## §4 字段类型 × 存储模式 组合矩阵

### §4.0 基础认知

Vespa schema 把每个 field 的"去向"分成三个可叠加的 target：

| Target | 含义 | 存储介质 |
|---|---|---|
| **index** | 构建倒排索引、支持全文检索/term match | MemoryIndex → DiskIndex（fusion 合并） |
| **attribute** | 构建列式内存结构、支持 filter/rank/grouping | AttributeVector（mmap 的 `.dat/.idx`） |
| **summary** | 出现在查询返回体里 | LogDocumentStore |

一个 field 可以是 `index`，或 `attribute`，或 `summary`，或任意组合，或都不是（只写 TLS 做异地冷备的场景）。**但是**这里有一条隐含规则：

> **只要 field 出现在 summary 里，整行 doc 必须序列化进 DocumentStore**——哪怕它同时是 attribute/index。
> 这是因为 DocumentStore 按"整行"存，不是按字段存。

这条规则是 §6 bug 的根源。

字段类型则有 7 类：`primitive / array / weighted-set / map / struct / tensor / reference`。下面按类型展开。

### §4.1 Primitive（int, long, float, double, bool, string）

最常见的 8 种"目标模式组合"：

| # | index | attribute | summary | 代表用途 | 激活路径 |
|---|---|---|---|---|---|
| 1 | — | — | ✓ | 日志原文 | DocStore only |
| 2 | — | ✓ | — | 仅 filter | Attribute only |
| 3 | ✓ | — | — | 纯全文检索 | Index + DocStore（summary 派生） |
| 4 | — | ✓ | ✓ | 带返回的 filter 字段 | Attribute + DocStore |
| 5 | ✓ | — | ✓ | 显示原文的全文检索 | Index + DocStore |
| 6 | ✓ | ✓ | — | 不返回但需检索+排序 | Index + Attribute + DocStore（隐式 summary） |
| 7 | ✓ | ✓ | ✓ | 最常用：全文检索+rank+返回 | 全路径 |
| 8 | — | — | — | — | 几乎无意义（只落 TLS） |

**注意表里的"隐式 summary"**：即便 schema 没写 `summary`，整行 doc 依旧会进 DocumentStore，因为 update/reindex 都可能要求从 DocumentStore 重建整个 doc。只是"查询返回"时不会出这个字段。

#### §4.1.a 组合 1：`field x type string { summary }`

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary<br/>LogDocumentStore]
    M --> DMS[(DocumentMetaStore)]
    DS --> LDS[(LogDocumentStore)]

    style DS fill:#C8FFC8
```

- 写入链路：只进 DocMetaStore 和 DocStore。
- **读取**：必须从 LogDocumentStore deserialize 整行 doc 再取字段。

#### §4.1.b 组合 2：`field price type int { attribute: fast-search }`

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary<br/>LogDocumentStore]
    P --> AW[putAttributes<br/>IAttributeWriter]
    M --> DMS[(DocumentMetaStore)]
    DS --> LDS[(LogDocumentStore)]
    AW --> AV[(IntegerAttribute)]

    style AW fill:#B5E7FF
    style DS fill:#F5F5F5,stroke-dasharray: 3 3
```

- **注意 DocumentStore 仍被写**（虚线框提示"内容只有 header，字段值是冗余的"）。如果 schema 启用了 `match: fast-access` 而不把字段列为 summary，docstore 里这字段的值可以从 attribute 派生，但**整行 doc** 仍然要存（见 §4.0 规则）。
- AttributeWriter 把单字段路由给对应的 `_attributeFieldWriter` 执行器（按 field-name hash 到某一线程），保证**同一 field 的写入严格串行**。

#### §4.1.c 组合 3：`field body type string { indexing: index }`

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary<br/>LogDocumentStore]
    P --> IW[putIndexedFields<br/>IIndexWriter]
    IW --> MI[(MemoryIndex<br/>per field)]
    M --> DMS[(DocumentMetaStore)]
    DS --> LDS[(LogDocumentStore)]

    style IW fill:#C8FFC8
```

- IndexWriter 把任务 `execute()` 到 **indexExecutor** 线程。MemoryIndex 内部按 field 划分，每个 field 有自己的 WordStore+PostingList。
- 因为 summary 没声明，DocumentStore 仍然写整行（supporting reindex、update、get）。

#### §4.1.d 组合 4：`field user_id type long { attribute | summary }`

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary]
    P --> AW[putAttributes]
    M --> DMS[(DocumentMetaStore)]
    DS --> LDS[(LogDocumentStore)]
    AW --> AV[(LongAttribute)]

    style AW fill:#B5E7FF
    style DS fill:#C8FFC8
```

- 和组合 2 结构一样，但 docstore 这次是"正常"被使用（读取路径可能直接从 attribute 取；也可能从 docstore 取。取决于 summary class 配置）。

#### §4.1.e 组合 5：`field body type string { index | summary }`

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary]
    P --> IW[putIndexedFields]
    M --> DMS[(DocumentMetaStore)]
    DS --> LDS[(LogDocumentStore)]
    IW --> MI[(MemoryIndex)]

    style DS fill:#C8FFC8
    style IW fill:#C8FFC8
```

- 最常见的全文检索场景。summary 可能是 `dynamic summary`（extract snippet），需要从 DocStore + 索引 posting 位置共同渲染。

#### §4.1.f 组合 6：`field title type string { index | attribute }`

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary]
    P --> AW[putAttributes]
    P --> IW[putIndexedFields]
    M --> DMS[(DocumentMetaStore)]
    DS --> LDS[(LogDocumentStore)]
    AW --> AV[(StringAttribute<br/>enum-store)]
    IW --> MI[(MemoryIndex)]

    style AW fill:#B5E7FF
    style IW fill:#C8FFC8
    style DS fill:#F5F5F5,stroke-dasharray: 3 3
```

- 最"重"的写入组合之一：**三条 worker 线程同时工作** + 一条 master。
- 注意 String attribute 用 enum-store 做去重：写入时做哈希查表，多条 doc 共用同一个 enum 值，节省内存但增加写入 CPU。

#### §4.1.g 组合 7：`index | attribute | summary`

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary]
    P --> AW[putAttributes]
    P --> IW[putIndexedFields]
    M --> DMS[(DocumentMetaStore)]
    DS --> LDS[(LogDocumentStore)]
    AW --> AV[(Attribute)]
    IW --> MI[(MemoryIndex)]

    style AW fill:#B5E7FF
    style IW fill:#C8FFC8
    style DS fill:#C8FFC8
```

- 一个 put 扇出到 4 条线程。写入吞吐由**最慢的一条**决定——通常是 indexExecutor（词典互斥 + posting merge）。

#### §4.1.h 组合 8：`indexing: ""`（都不是）

实际中罕见。意义：字段只存在 TLS 里，供 replay 时带到可能后加的 schema 扩展。**FeedView 层不会执行任何 put 分支**，但 DocStore 仍然会写整行 doc（因为 DocStore 是按 doc 粒度，不按 field）。

### §4.2 Array / WeightedSet

`array<int>`、`weightedset<string>` 等**多值字段**在写入路径上和 primitive 一样走 `putAttributes`，但 AttributeVector 内部存储方式不同：

```
MultiValueMapping：
  docId → valueRange offset → 实际 value 数组
```

写入步骤：

1. AttributeWriter 拿到整个 Document；
2. 对该字段构造 `vector<Value>`；
3. 调用 `MultiValueAttribute::setValues(lid, values)`；
4. MultiValueMapping 分配一段新 range（旧 range 交给 GenerationHolder）。

```mermaid
flowchart LR
    P[PutOperation] --> AW[putAttributes]
    AW -->|per-field| MV[MultiValueAttribute]
    MV --> MM[MultiValueMapping<br/>RCU segment]
    MV --> ES[(EnumStore<br/>only string/ref)]

    style AW fill:#B5E7FF
```

**关键代价**：多值字段的 update 操作（`addElements`、`removeElements`）要读出旧 range、计算新 range、写入新 range，O(n) 复制。超大的 weightedset 是写入延迟的常见源头。

### §4.3 Map

Vespa schema 中 `map<K,V>` 在内部被**编译成两列 attribute**：`mapfield.key` 和 `mapfield.value`，都用 array 实现，且共用同一个 `docId → range` 结构。

```mermaid
flowchart LR
    D[Document<br/>MapFieldValue] --> S[struct-field-path<br/>schema expansion]
    S --> K[mapfield.key<br/>ArrayAttribute]
    S --> V[mapfield.value<br/>ArrayAttribute]
    K --> MM1[(MultiValueMapping)]
    V --> MM2[(MultiValueMapping)]
```

**更新特性**：map 支持对单个 key 做 partial update（`{"field{mykey}": {"assign": 42}}`）。AttributeWriter 会定位到该 key 在 value 数组中的 index，做**in-place** 修改——这条路径是写入最快的之一（§6 快路径）。

### §4.4 Struct / Complex

Struct field 的 attribute 化依赖 **struct-field-path**：只有被声明为 `struct-field x.y { indexing: attribute }` 的内层标量才真正走 AttributeVector。其他内层字段仅存在于 DocumentStore 的整行 doc 里。

```mermaid
flowchart LR
    D[Document<br/>StructValue] --> FP[struct-field-path<br/>extractor]
    FP -->|声明为 attribute 的子字段| AV[(AttributeVector)]
    FP -->|其他| DS[LogDocumentStore<br/>整行 doc]
    D --> DS

    style AV fill:#B5E7FF
    style DS fill:#C8FFC8
```

**一个重要细节**：对 struct 做 partial update 时，如果某个非 attribute 子字段被修改，就必须**从 DocStore 读出整行 doc，修改后重写**——这条路径在 `makeUpdatedDocument` 里（`storeonlyfeedview.cpp:467-499`），是 update 慢路径。此时 `UpdateScope._nonAttributeFields` 会被置 true。

### §4.5 Tensor

Tensor 字段的特殊性：**只能是 attribute，不能 index**。两种存储方式：

- **Dense tensor**：按固定维度打包成 `vector<double/float/int8>`；
- **Sparse / mixed tensor**：稀疏坐标 + 值。

```mermaid
flowchart LR
    P[PutOperation] --> M[putMetaData]
    P --> DS[putSummary]
    P --> AW[putAttributes]
    AW --> TA[TensorAttribute]
    TA --> TS[(TensorStore<br/>dense=blob / sparse=btree)]
    P -.可选.-> NN[HNSW Index<br/>via Attribute]
    AW -.-> NN

    style AW fill:#B5E7FF
    style DS fill:#C8FFC8
```

如果 tensor 声明了 `index { hnsw }`，写入还会触发**HNSW 图的插入**——这条路径由 AttributeWriter 在完成 attribute 写入后调用 `TensorAttribute::prepare_set_tensor` + `complete_set_tensor`，中间是可以部分并行的（HNSW 构建是 CPU 热点之一）。

### §4.6 Reference（parent-child imported field）

`reference<parent_type>` 字段：

- 写入父类型时走正常路径，无特殊；
- 子类型里 `import field parent.foo as foo` 的 **imported attribute** **不产生写入路径**——它在 search 时按需从父 attribute 读；
- Reference 字段本身是 attribute（存 GID），写入时除了 attribute 还要通知 `ReferenceStore` 维护一个 GID→LID 的索引（供 imported field 解引用）。

```mermaid
flowchart LR
    P[PutOperation child] --> AW[putAttributes]
    AW --> RA[ReferenceAttribute]
    RA --> GIDMAP[(GID→parent LID cache)]

    P2[PutOperation parent] --> EVENT[ReferenceResolver<br/>通知子 type]
    EVENT --> INV[invalidate child<br/>GID→LID cache]
```

父侧的 put 会通过 `DocumentDBReferentRegistry` 通知子 DocumentDB 失效缓存——这在跨 DocumentDB 的 put 中引入一条**耦合**，工程上需要注意。

### §4.7 组合矩阵总览

```mermaid
flowchart TB
    Put[PutOperation 一次写入]
    Put --> DMS[putMetaData<br/>DocumentMetaStore]
    Put --> COND{根据 schema}

    COND -->|任何情况| DS[putSummary<br/>LogDocumentStore]
    COND -->|任一字段为 attribute| AW[putAttributes<br/>IAttributeWriter]
    COND -->|任一字段为 index| IW[putIndexedFields<br/>IIndexWriter]

    AW --> P1[Primitive Attr]
    AW --> P2[MultiValue Attr<br/>array/weightedset]
    AW --> P3[Map 拆成两列]
    AW --> P4[Struct 展平]
    AW --> P5[Tensor + HNSW]
    AW --> P6[Reference + GID map]

    IW --> I1[String/Primitive<br/>MemoryIndex]
    IW --> I2[URI/预定义分词]

    style DMS fill:#FFE4B5
    style DS fill:#C8FFC8
    style AW fill:#B5E7FF
    style IW fill:#C8FFC8
```

**核心观察**：

1. **DocumentMetaStore 与 DocumentStore 是"一定写"**——只要有 put。
2. **Attribute 路径按字段数量横向 fan-out**——同一个 doc 的不同 attribute field 在不同线程上并行写。
3. **Index 路径永远落在单个 indexExecutor**——这是写入瓶颈的常见位置。
4. **Map / Struct / Tensor 引入额外的 in-memory 数据结构维护**，读写代价都比 primitive 高一个量级。

---

## §5 写入时序：Put / Update / Remove / Move

### §5.1 Put：从 FeedHandler 到所有组件的完整时序

一次 put 的生命周期横跨 RPC 线程、master 线程、3 组 worker 线程、TLS 线程。下面这张图按时间从上到下展开：

```mermaid
sequenceDiagram
    autonumber
    participant RPC as RPC Thread<br/>(DocumentAPI)
    participant FH as FeedHandler<br/>(master)
    participant TLS as TLS Writer
    participant FV as FeedView<br/>(master)
    participant DMS as DocumentMetaStore
    participant SUM as summaryExecutor
    participant ATT as attributeFieldWriter<br/>(per-field N 条)
    participant IDX as indexExecutor
    participant CB as OnDone Callback

    RPC->>FH: PutOperation (bucket, doc)
    FH->>FH: considerWriteOperationForRejection<br/>(DiskMemUsageFilter / AttributeUsageFilter)
    Note right of FH: 若资源紧张→直接 reject<br/>distributor 退避
    FH->>TLS: append packet (serialNum=N)
    TLS-->>FH: ack (TLS fsync 可配)
    FH->>FV: preparePut (分配 lid)
    FV->>DMS: putIfAbsent(gid) → lid
    FH->>FV: handlePut (异步)
    FV->>DMS: putMetaData(lid, gid, bucket, ts)
    par fan-out 到 3 类 worker
        FV-)SUM: putSummary task
        SUM->>SUM: LogDocumentStore.put(lid, doc)
        SUM-)CB: onDone
    and
        FV-)ATT: putAttributes fan-out (每个 attr field 一条)
        ATT->>ATT: AttributeVector.update(lid, value)
        ATT-)CB: onDone
    and
        FV-)IDX: putIndexedFields task
        IDX->>IDX: MemoryIndex.insertDocument(lid, doc)
        IDX-)CB: onDone
    end
    CB->>CB: 汇聚所有 onDone
    CB->>FH: ack FeedToken (客户端响应可返回)
    Note right of CB: 此时"写入完成"，但尚未<br/>commit，search 可见性依赖<br/>visibilityDelay 与 forceCommit
```

**关键点**：

- **TLS 写在最前面**——保证任何后续操作都可在 crash 后 replay；
- **DocumentStore / Attribute / Index 的 fan-out 是完全并行的**，互相不等待；
- **OnDone 是计数回调**，所有 worker 都 ack 后才向 client 返回成功；
- **ack 不等于 search 可见**：可见性要等 `forceCommit`（或 `visibilityDelay` 触发的 auto-commit）把 RCU generation 推进。

### §5.2 Remove：墓碑化 + 惰性 LID 回收

```mermaid
sequenceDiagram
    autonumber
    participant FH as FeedHandler
    participant FV as FeedView
    participant DMS as DocumentMetaStore
    participant SUM as summaryExecutor
    participant ATT as attributeFieldWriter
    participant IDX as indexExecutor

    FH->>FV: handleRemove(gid)
    FV->>DMS: remove(lid)
    Note right of DMS: lid 进入 free-list<br/>不立即复用
    par 三条路径
        FV-)SUM: removeSummary(lid)<br/>追加 tombstone 到 LogDocumentStore
    and
        FV-)ATT: removeAttributes(lid)<br/>AttributeVector.clearDoc(lid)
    and
        FV-)IDX: removeIndexedFields(lid)<br/>MemoryIndex.removeDocument(lid)
    end
```

- DocumentStore 的 remove 不立刻回收磁盘空间，只追加一条"removed"记录，待 bloat 阈值到再 `compactBloat` 合并；
- AttributeVector 的 `clearDoc` 把该 lid 的值置为默认（或 null），不回收内存；
- MemoryIndex 的 remove 把 lid 加入该 field 的 `_removedDocs` 位图，查询时做过滤，真正从 posting list 去掉等 flush→disk + fusion。

### §5.3 Update：三条路径的分叉

文件：`storeonlyfeedview.cpp:303-464`。这是最复杂的一条路径。

```mermaid
flowchart TB
    U[UpdateOperation] --> S[用 Schema+AttributeManager<br/>构建 UpdateScope]
    S --> CHK{updateScope.<br/>_indexedFields 或<br/>_nonAttributeFields ?}

    CHK -->|都是 false<br/>纯 attribute 快路径| FAST[updateAttributes<br/>直接 in-place 修改]
    FAST --> DONE1[OnDone → ack]

    CHK -->|true| SLOW[慢路径]
    SLOW --> READ[shared().execute<br/>makeUpdatedDocument]
    READ --> GET[SummaryAdapter.get lid<br/>从 DocStore 读整行 doc]
    GET --> APPLY[apply DocumentUpdate<br/>到 prevDoc]
    APPLY --> SER[重新 serialize]
    SER --> FUT[填充 FutureDoc + FutureStream]
    FUT --> FO1[updateIndexedFields<br/>if indexed fields touched]
    FUT --> FO2[updateAttributes<br/>struct field path]
    FUT --> FO3[putSummary<br/>写回整行 doc]
    FO1 --> DONE2[OnDone → ack]
    FO2 --> DONE2
    FO3 --> DONE2

    style FAST fill:#C8FFC8
    style SLOW fill:#FFD4D4
```

**快路径（左分支）**：只有一条 fan-out，只动 attribute。写入延迟可以低到微秒级。  
**慢路径（右分支）**：有一次 DocStore **读**（随机 IO）+ 反序列化 + 重新应用 update + 再序列化 + DocStore **写**，延迟是快路径的几十倍到上百倍。

因此**识别 update 是否进入快路径**是工程优化的关键。代码锚点：

- `UpdateScope::onUpdateField`（`storeonlyfeedview.h:125-133`）——**每个被 update 的 field 都会调用一次**。只要有一个 field 不是 `AttributeVector::isUpdateableInMemoryOnly()`，就置 `_nonAttributeFields=true`；
- `isUpdateableInMemoryOnly` 对于 tensor、enum-based string、HNSW 都可能返回 false；
- **一个典型坑**：给 string 字段加上 `match: substring` 或改变 dictionary 类型，会让原本快路径的 update 突然变慢。

### §5.4 Move：跨 Sub-DB 搬迁的两端写

`CombiningFeedView::handleMove` 一个 MoveOperation 变成"源 subdb remove + 目标 subdb put"：

```mermaid
sequenceDiagram
    participant CFV as CombiningFeedView
    participant SRC as Source SubDB<br/>(e.g. NotReady)
    participant DST as Target SubDB<br/>(e.g. Ready)
    participant BKT as BucketDB

    CFV->>DST: put(lid', gid, bucket, doc)
    Note over DST: 目标 subdb 的 FeedView<br/>执行完整 putAttributes<br/>+ putIndexedFields
    CFV->>SRC: remove(lid)
    Note over SRC: 源 subdb 的 FeedView<br/>清除 attr + index + docstore
    CFV->>BKT: 更新 bucket 的 subdb 归属
```

两侧共享同一个 `OnDone`，避免中间状态被外部观察。

---

## §6 Bug 解读：attribute 字段与 docstore 的纠缠

你回忆的那个 bug："某字段如果是 attribute，就不更新 docstore，结果出问题了。"——这是一个**非常典型的错误推广**。本章把正确语义、错误模式、正确修复方向讲透。

### §6.1 正确语义

**规则 1（硬性）**：只要一次 put 提交了完整 Document，`LogDocumentStore` 就必须收到这份 doc。原因：

- Document 是**整行存储**，不按字段存；
- 未来的 update、reindex、migrate、visit 都可能需要完整 doc 来重建其它组件；
- Summary 渲染时，对于没有 attribute 表达的字段（比如大 string、非 attribute 的 struct 子字段）只能从 docstore 拿。

**规则 2（优化）**：summary 渲染时，如果字段同时是 attribute，runtime 可以选择**从 attribute 读**而不从 docstore 读（节省一次反序列化）。这条优化由 `DocumentRetriever` 决定，**与写入路径无关**。

代码锚点：

- `DocumentRetriever`（`proton/server/documentretriever.h:17-64`）里维护 `FieldSetAttributeDB`，判断一个 field set 是否"全部 attribute-only"；
- 若是，`retrieve()` 走 attribute 快读，跳过 `_summaryAdapter->get(lid)`；
- 若否，`_summaryAdapter->get(lid)` 必须从 docstore 读。

**结论**：docstore 是真源（source of truth for document body），attribute 是"热副本"。写入时不能跳过真源，读取时可以选择热副本。

### §6.2 错误模式（可能就是你当时踩的坑）

假设读代码时看到 `DocumentRetriever` 里有"字段全是 attribute 就跳过 docstore"的逻辑，然后把这条规则错误地搬到**写入路径**：

```cpp
// 错误修改：
void StoreOnlyFeedView::internalPut(...) {
    putMetaData(...);
    if (!allFieldsAreAttributes(doc)) {      // ← 新增的错误判断
        putSummary(serialNum, lid, doc, onWriteDone);
    }
    putAttributes(serialNum, lid, *doc, onWriteDone);
    putIndexedFields(serialNum, lid, doc, onWriteDone);
}
```

**表现**：

| 操作 | 现象 |
|---|---|
| Put（all-attr doc） | 静默成功，但 DocStore 里没这条 doc |
| Get / Visit | 查不到 doc body，只拿到 attribute 值 |
| Update（带非 attr 字段） | `makeUpdatedDocument` 里 `summaryAdapter->get(lid)` 返回 `nullptr`，要么 crash 要么写入"空 doc + update"的残缺 |
| Remove | 正常（remove 只需 meta） |
| Reindex / schema 改动 | 没有 doc body 可重建，Ready/NotReady 相互迁移时丢数据 |
| Fusion 后 get | 彻底丢 |

### §6.3 为什么这条规则看起来"好像对"

诱惑力在于：**如果整个 schema 里所有字段都是 attribute**，理论上 doc body 确实可以从所有 attribute 拼回来。Vespa 支持这种"attribute-only"的 summary class。但有两个陷阱：

1. **即便现在所有字段都 attribute**，update 来一次新字段（版本兼容）就破坏前提；
2. **Struct / Tensor / Map** 的 attribute 化只覆盖声明过的子字段，未声明的内层字段**只在 docstore 里**——"all-attribute"的判断极容易漏掉这些隐式依赖。

### §6.4 正确的"跳过 docstore" 的优化空间

真正合法的跳过场景只有一个：**attribute-only partial update**。也就是 §5.3 的 update 快路径。此时：

- 没有读 DocStore（`makeUpdatedDocument` 不调用）；
- 没有写 DocStore（`putSummary` 不调用）；
- 只更新 attribute 的值；
- doc body 在 DocStore 里依然是**旧版本**，但 summary 渲染时 `DocumentRetriever` 会用 attribute 的新值覆盖——这是一致的。

代码锚点：`storeonlyfeedview.cpp:303-464` 的 `internalUpdate`。你可以搜 `updateScope.hasIndexOrNonAttributeFields()`（或等价判断），它的 **false 分支**就是快路径。

**扩展这条路径的方法**（§11 会再展开）：

1. 让更多字段声明为 attribute（降低 `_nonAttributeFields=true` 的概率）；
2. 让 `AttributeVector::isUpdateableInMemoryOnly()` 对更多 attribute 类型返回 true（例如 in-place tensor modify）；
3. Map 的 `{key}` partial update 已经是 in-place 快路径，可以显式把热点字段建模成 map；
4. Struct field-path 级 attribute 可以让部分 struct update 走快路径。

### §6.5 一段有意思的观察：replay 时没这个 bug

因为 TLS 里存的是完整 `DocumentUpdate` / `Put` 的序列化形式。Replay 时 FeedHandler 会**重放完整 op**，进入和正常写一样的 FeedView 路径。如果你的错误修改逻辑是在写入路径上跳过 docstore，replay 后也会**复现**这个问题——**不是 crash recovery 就能救回来**。这也是当年这类 bug 难发现的原因：测试集里单条 put+get 过得去，一碰 update + restart 就炸。

---

## §7 Flush / Fusion / Compact 与写入的耦合

### §7.1 Flush 的整体调度

`FlushEngine`（`searchcore/proton/flushengine/flushengine.{h,cpp}`）负责协调所有可 flush 组件。每个 sub-db 暴露一组 `IFlushTarget`：

| FlushTarget | 所属 | 触发条件 | 产物 |
|---|---|---|---|
| `DocumentMetaStoreFlushTarget` | 每个 sub-db | dirty LID 数超阈值 / 间隔时间 | `.dat/.idx` meta 文件 |
| `SummaryFlushTarget` | 每个 sub-db | LogDocumentStore bloat 或定时 | 新 chunk 文件 + offset 索引 |
| `FlushableAttribute` | Ready/NotReady 的每个 attribute | memory 阈值 / 间隔 | per-attr `.dat/.idx` |
| `IndexFlushTarget` | Ready 的 IndexManager | MemoryIndex 超阈值 | 新 DiskIndex 分片 |
| `IndexFusionTarget` | Ready | 多个 DiskIndex 分片 | 合并成单个 DiskIndex |
| `ShrinkLidSpaceFlushTarget` | 每个 sub-db | LID 空间 fragmentation | 压紧 LID |

**调度策略**文件：`flushengine/prepare_restart_flush_strategy.cpp` 等。优先级按"内存回收效率"排序：一次 flush 能释放多少 dirty 页。

```mermaid
flowchart LR
    FE[FlushEngine<br/>定时唤醒] --> COLLECT[收集所有 IFlushTarget]
    COLLECT --> RANK[按策略排序<br/>memory/age/priority]
    RANK --> PICK[挑选一个或几个]
    PICK --> EXEC[在独立线程执行 flush]
    EXEC --> TASK1[DocumentMetaStore]
    EXEC --> TASK2[SummaryStore]
    EXEC --> TASK3[Attribute]
    EXEC --> TASK4[Index Memory→Disk]
    EXEC --> TASK5[Index Fusion]
    EXEC --> TASK6[ShrinkLidSpace]
    TASK1 --> PRUNE[更新 prunedSerialNum]
    TASK2 --> PRUNE
    TASK3 --> PRUNE
    TASK4 --> PRUNE
    PRUNE --> TLS[TLS prune]
```

Flush 完成后，更新该组件的 `flushedSerialNum`。TLS 只会 prune 到**所有组件**都 flush 过的最小 serialNum——也就是"最慢的那个人决定 TLS 的大小"。

### §7.2 Memory Index → Disk Index 的双缓冲

IndexMaintainer 的写入是典型的 **LSM-like** 结构：

```mermaid
flowchart LR
    W[写入] --> M1[MemoryIndex<br/>active]
    FLUSH[触发 flush] -.switch.-> SWAP{交换}
    SWAP --> M2[MemoryIndex<br/>new active]
    SWAP --> FROZEN[MemoryIndex<br/>frozen]
    FROZEN --> DISK[DiskIndex fragment]
    DISK --> FUSION[多个 DiskIndex]
    FUSION --> MERGED[单一合并 DiskIndex]
```

- Flush 开始时**不阻塞写入**：新写入进 `new active`，旧的 frozen 继续在 serialize 到 disk；
- 搜索时 merge 所有当前 MemoryIndex + 所有 DiskIndex fragment + frozen；
- Fusion 把多个 DiskIndex 合成一个，是**后台 IO 密集 + CPU 密集**任务，会显著挤占写入带宽。

### §7.3 Attribute Flush 的 generation guard

Attribute flush 不是复制整个内存，而是：

1. **take guard**：记录当前 generation；
2. 开始序列化当前数据结构；
3. 此时继续写入，产生新 generation 的数据，但旧 generation 被 guard 保留；
4. 序列化完成后 release guard，GenerationHolder 回收旧数据；
5. 更新 flushedSerialNum，写 metadata 文件。

文件：`vespalib/util/generationhandler.{h,cpp}`、`vespalib/util/rcuvector.h`。

### §7.4 DocumentStore 的 compaction

`LogDocumentStore` 是"追加-only chunk file + offset index"。remove / update 造成 bloat：

- `compactBloat`：遍历 chunk，把非废弃的 doc 复制到新 chunk，旧 chunk 删除；
- `compactSpread`：按 bucket 重新组织 chunk 布局（提升 get-by-bucket 的 locality）。

两者都是后台任务，读写会和它们**竞争磁盘 IO**——当写入吞吐大到让 bloat 快速增长时，compaction 可能跟不上。

### §7.5 ShrinkLidSpace：LID 压缩

当一个 sub-db 经历大量 remove 后，LID 空间出现空洞（`docIdLimit` 虚高）。`ShrinkLidSpaceFlushTarget` 触发 `handleCompactLidSpace`（`storeonlyfeedview.cpp:758-779`）：

- 通知 DocumentMetaStore shrink metadata 数组；
- 通知每个 AttributeVector shrink；
- 通知 IndexMaintainer shrink bitmap；
- 通知 DocumentStore 更新 offset 索引上限。

**与写入的耦合**：ShrinkLidSpace 只在 `docIdLimit` 之后的 LID **确保无未完成写**时才能执行——依赖 PendingLidTracker。

### §7.6 Fusion 期间的 serialNum 边界

Fusion 把 DiskIndex_A（覆盖 serialNum ≤ S1）和 DiskIndex_B（S1+1..S2）合成新 DiskIndex_M（≤ S2）。关键约束：

- Fusion 期间**写入继续进 MemoryIndex**，不影响；
- 生成的 DiskIndex_M 文件原子替换旧的 A、B；
- **searcher 在切换点需要一次 snapshot 切换**，短暂内存峰值（A+B+M 同时驻留）。

---

## §8 读写互相影响：RCU、Generation、可见性延迟

这一节回答"一次写什么时候能被搜索看到，在这之间读和写互相怎么影响"。

### §8.1 无锁读的底层：GenerationHandler + RcuVector

代码锚点：

- `vespalib/util/generationhandler.h:16-141`
- `vespalib/util/rcuvector.h:34-100`

读侧流程：

```cpp
// 伪代码
{
    auto guard = genHandler.takeGuard();   // 记录当前 generation G
    const auto* snap = rcuVec.load();       // 读到 G 时刻的数组基址
    for (auto id : ids) use(snap[id]);      // 不会被 writer 抢走
}  // guard 析构 → generation G 的 refcount--
```

写侧流程：

```cpp
rcuVec.expandAndInsert(newValue);  // 拷贝旧数组到新地址，原子发布新指针
genHandler.incGeneration();        // bump 当前 generation
genHandler.updateFirstUsedGeneration();  // 没 reader 使用的旧数据由 GenerationHolder 延迟释放
```

**关键性质**：reader 永远不阻塞 writer，writer 也不阻塞 reader。代价是旧数据要等所有 reader 释放 guard 后才能真正回收（generation hold）。

### §8.2 AttributeVector 的写入可见性

AttributeVector 的 `update(lid, value)` **不会立即让 reader 看到新值**。只有在 `commit()` 之后：

- `commit()` 会做：把累积在 write buffer 里的修改 commit 到正式结构、`incGeneration`、对 multi-value/enum 结构触发 GC；
- 文件：`searchlib/attribute/attributevector.h:447-449`。

这意味着**批量写入的可见性是成组推进的**，而不是每条写入后立刻变可见。

### §8.3 FeedView 的 forceCommit 与 CommitTimeTracker

`IFeedView::forceCommit(CommitParam)` 让所有组件推进到给定 serialNum。触发者有三类：

1. **visibilityDelay 计时器**：`CommitTimeTracker`（`proton/common/commit_time_tracker.h:14-20`）管理。每 `visibilityDelay` 毫秒触发一次 forceCommit；
2. **显式 `flush`/`sync` feed 指令**；
3. **FlushEngine 开 flush 前**，必须先 forceCommit 到 flush 的 serialNum。

`visibilityDelay` 在 `DocumentDBMaintenanceConfig::_visibilityDelay`（`document_db_maintenance_config.h:108`）配置。典型值：

- 0ms：每条 put 后立即可见（延迟低，CPU 高）；
- 100ms：批量可见（延迟可接受，CPU 友好）；
- 1s：离线灌数据，最省 CPU。

### §8.4 PendingLidTracker 的阻塞点

`PendingLidTracker` 有两套（§3.3）：

- `_pendingLidsForDocStore`：用于 `get(lid)`——**如果 lid 正在 pending**，get 必须等它 drain；
- `_pendingLidsForCommit`：用于 forceCommit——阻塞 commit 直到所有 fan-out ack。

**读会不会阻塞写？** 典型情况下不会。但有一类场景会：`makeUpdatedDocument` 内部会 `_summaryAdapter->get(lid)`，如果同一个 lid 正在 pending docstore 写，get 会等——这是写入路径内部的"自阻塞"。

### §8.5 MemoryIndex 的可见性

MemoryIndex 对每个 field 有独立的 posting list。写入路径：

```
insertDocument(lid, doc) → 按 term 插入 posting list
```

Posting list 使用 BTree + 小段数组的混合结构，insert 本身是 O(log n)。读侧通过 `takeGenerationHandler()` 拿到当前 snapshot。

**与 DiskIndex 的协作**：一次查询要 merge 所有 MemoryIndex + DiskIndex posting list——写入 peak 时 MemoryIndex 很大，查询 CPU 和内存也升高。这是写入→读取的**被动耦合**。

### §8.6 可见性时间线

```mermaid
sequenceDiagram
    participant W as Writer
    participant FV as FeedView
    participant AV as AttributeVector
    participant MI as MemoryIndex
    participant DS as DocumentStore
    participant CT as CommitTimeTracker
    participant R as Reader (search)

    W->>FV: put(doc)
    FV->>AV: update (buffered)
    FV->>MI: insertDocument (立即可 search 但 gen 未 bump)
    FV->>DS: put (async)
    Note over R: put 之后立刻 search<br/>看不到新值 (generation 未 bump)

    Note over CT: visibilityDelay ms 后
    CT->>FV: forceCommit
    FV->>AV: commit (bump gen)
    FV->>MI: commit (bump gen)
    FV->>DS: commit
    Note over R: 之后 search 可见
```

### §8.7 后台 flush / fusion 对读的影响

- **Flush 期间**：正常读不受影响（RCU）；
- **Fusion 期间**：旧 DiskIndex 尚未被替换，searcher 继续用旧 shard；切换瞬间需要重载新 shard，page cache 冷启动会造成 **短时间查询延迟抖动**；
- **Attribute flush**：同 flush，不阻塞读，但磁盘写 IO 消耗会和查询的 random read 竞争；
- **DocStore compactBloat**：大量顺序 IO，可能把 random get-by-lid 的延迟推高。

这些都是"写路径的后台任务→读路径"的**隐性耦合**。

---

## §9 TLS 回放、冷启动、空节点恢复

### §9.1 启动时序总览

代码锚点：`searchcore/proton/server/documentdb.cpp`、`feedhandler.cpp`、`transactionlogmanager.h`。

```mermaid
sequenceDiagram
    autonumber
    participant Proc as proton 进程
    participant DB as DocumentDB
    participant SUB as DocumentSubDBCollection
    participant DMS as DocumentMetaStore
    participant ATT as AttributeManager
    participant IDX as IndexManager
    participant DS as SummaryManager
    participant FH as FeedHandler
    participant TLS as TransactionLogManager

    Proc->>DB: 构造，state=CONSTRUCT
    DB->>DB: internalInit() → state=LOAD
    DB->>SUB: createInitializer() (并行任务树)
    par 并行加载持久化产物
        SUB->>DMS: load .dat/.idx → 重建 gid→lid B+tree
        SUB->>ATT: load 每个 attribute 的 .dat/.idx
        SUB->>IDX: 加载所有 DiskIndex
        SUB->>DS: 打开 LogDocumentStore + offset 索引
    end
    SUB-->>DB: InitDoneTask
    DB->>DB: initFinish()<br/>setupBucketHandler / initViews
    DB->>FH: startTransactionLogReplay(min flushedSerialNum)
    FH->>TLS: prepareReplay → startReplay
    loop 每个 TLS Packet
        TLS->>FH: receive(packet)
        FH->>FH: ReplayPacketDispatcher.deserialize<br/>→ FeedOperation
        FH->>FH: performPut/Update/Remove<br/>(同正常写一样)
    end
    TLS-->>FH: replayDone
    FH->>SUB: onReplayDone()<br/>构建 free-list、compact lid space
    DB->>DB: state=ONLINE，开始接受 RPC 写
```

文件锚点：

- `documentdb.cpp:289-293` `internalInit`
- `documentdb.cpp:321-333` `initManagers`
- `documentdb.cpp:336-347` `initFinish`
- `documentdb.cpp:723-743` `startTransactionLogReplay`
- `feedhandler.cpp:470-491` `replayTransactionLog`
- `feedstates.h:48-67` `ReplayTransactionLogState`

### §9.2 SerialNum 是 Proton 一切的总线

每个组件持久化时记录自己的 `flushedSerialNum`：

| 组件 | flushedSerialNum 含义 |
|---|---|
| DocumentMetaStore | meta 文件覆盖到这个 serial 为止 |
| AttributeVector × N | 每个 attribute 自己的 serial |
| DocumentStore | LogDocumentStore 已 fsync 的最大 serial |
| IndexMaintainer | 最近 flush 的 DiskIndex shard 覆盖的 serial |

启动时：

```
replayStartSerial = min(所有组件的 flushedSerialNum) + 1
replayEndSerial   = TLS 中最大 serial
```

Replay 区间内的每条 op 会**对所有 flushedSerialNum 比它小的组件**重新应用——已经 flush 过的组件会跳过这条 op（每个组件内部对比 serial 自动 dedup）。

例如：DocStore flushed 到 1000，attribute X flushed 到 800。Replay [801..1000] 时：

- DocStore 看到 op.serial≤flushed → 跳过；
- Attribute X 看到 op.serial>flushed → 应用。

这种**per-component 幂等**是保证 partial flush 仍然能正确恢复的基石。

### §9.3 Replay 走的是同一条 FeedView 路径

**重要**：replay **不是**走 shortcut，而是和正常 RPC 写**走同一个 IFeedView**。区别只在：

- FeedToken 不需要 ack 回 RPC client（用 `ReplayFeedToken`，`proton/common/replay_feed_token_factory.h`）；
- TLS 不再写一遍（已经在 TLS 里了）；
- 应用 `ReplayThrottlingPolicy`（`replay_throttling_policy.h:14-25`）做 dynamic throttle，避免 replay 把 disk/memory 打满。

这就是为什么 §6 的 bug "replay 救不回来"——bug 在 FeedView 里，replay 也会复现。

### §9.4 Empty / 全冷启动节点

完全空的节点有两类：

**A. 本地 TLS 还在，磁盘 flush 全没了**（罕见，比如手动删 var 目录但保留 TLS）：

- 启动时 sub-db init 加载到全空状态；
- replayStartSerial = 1，replayEndSerial = TLS 末尾；
- Replay 把所有 op 重新跑——和"全新构建"等价；
- 完成后 BucketDB 自然重建（每条 put/remove 都更新 BucketDB）。

**B. 节点完全干净，加入集群**（典型新增节点）：

- 本地 TLS 也是空的，replay 无事可做；
- DocumentDB 进入 ONLINE，但**没有任何 bucket**；
- 此时 distributor 通过 **bucket merge / migrate-in** 把 bucket 从其他副本拉过来；
- 走的是另一条路径：persistence engine `createBucket / put` 从 RPC 进入，落到 FeedHandler 的正常写路径——和客户端写没有本质区别。

### §9.5 Bucket 迁入：和正常写完全一样的路径

距离最近的代码锚点：`bucketdb/i_bucket_create_notifier.h:16-21`、`bucketmovejob.h:77`。

```mermaid
flowchart LR
    PEER[其他副本节点] -->|MessageBus| SP[StorageProtocol<br/>persistence layer]
    SP --> PE[PersistenceEngine]
    PE --> CB[createBucket]
    PE --> PUT[put doc]
    PE --> RM[remove doc]
    CB --> CFV[CombiningFeedView::handleCreateBucket]
    PUT --> FV[CombiningFeedView::handlePut]
    RM --> FV2[CombiningFeedView::handleRemove]
    FV --> ALL[正常的<br/>FeedHandler→FeedView→组件]
```

- 接收一个迁入的 doc 与接收一个 client put **代码路径完全一致**；
- 唯一差别是 `LoadType` 标记和 throttle 策略（避免迁入打挂正常写）；
- TLS 同样会写——这条迁入对本节点而言是一条新的 write，要复制到自己的 TLS。

### §9.6 BucketDB 的重建

BucketDB（`bucketdb/bucketdb.h:14-77`）在内存里维护 `BucketId → 文档计数 / 校验和 / 状态`。**它不持久化**，每次启动从零构建：

- DocumentMetaStore 加载时遍历每个 `RawDocumentMetaData` 的 BucketId，喂给 BucketDB；
- Replay 期间继续累加；
- `onReplayDone()`（`storeonlydocsubdb.cpp:175-189`）做最终的 free-list 构建和 lid space compact。

这种"派生状态零持久化"的设计简化了 crash recovery——只要 DocumentMetaStore 是对的，BucketDB 一定能正确重建。

### §9.7 Resource backpressure 在 replay 的特殊处理

正常写时，`DiskMemUsageFilter`（`disk_mem_usage_filter.h:24-86`）和 `AttributeUsageFilter`（`attribute_usage_filter.h:22-50`）会拒掉操作，distributor 退避。

但 **replay 不能拒**——拒掉就意味着永久数据丢失。因此 replay 用 `ReplayThrottlingPolicy` 来**减速而非拒绝**：

- 检测到压力：动态 sleep / 减小 batch；
- 保证 replay 总能完成；
- 启动时间因此可能拉长——但比丢数据强。

代码锚点：`feedhandler.cpp:493`（throttle policy 注入）。

### §9.8 启动状态机

```mermaid
stateDiagram-v2
    [*] --> CONSTRUCT
    CONSTRUCT --> LOAD: internalInit
    LOAD --> REPLAY_TRANSACTION_LOG: initFinish + sub-db loaded
    REPLAY_TRANSACTION_LOG --> APPLY_LIVE_CONFIG: replay done
    APPLY_LIVE_CONFIG --> RECONFIGURE: 应用最新 config
    RECONFIGURE --> ONLINE: 接受 RPC 写
    ONLINE --> [*]
```

Replay 期间外部 RPC 写会被 reject 或 buffer（取决于 distributor 协议层），不会真正进入 FeedHandler。

---

## §10 Distributor / Bucket 管理接口

### §10.1 分层关系

Vespa 的写入路径在集群层是**分布层 + 内容层**两层架构：

```mermaid
flowchart TB
    C[Client]
    C -->|DocumentAPI| MB[messagebus]
    MB --> CON[Container Cluster<br/>Document Processor]
    CON --> DIST[Distributor Cluster]
    DIST -->|按 bucket 路由 + 副本协调| SN1[Storage Node 1]
    DIST --> SN2[Storage Node 2]
    DIST --> SN3[Storage Node 3]
    SN1 --> PROTON1[Proton<br/>PersistenceEngine]
    SN2 --> PROTON2[Proton]
    SN3 --> PROTON3[Proton]
    PROTON1 --> FH[FeedHandler → FeedView]
```

- **Distributor**：知道每个 bucket 应该在哪些 storage node 上有几份副本；它把一个 doc-id hash 到 bucket，再把写入并行发到所有副本节点。
- **Storage Node + Proton**：每个 storage node 跑一个 Proton，里面是一组 DocumentDB（每个 schema 一个），DocumentDB 内是我们前面讨论的 sub-db × FeedView。

### §10.2 Distributor 给 Proton 的 5 类操作

代码锚点：`searchcore/proton/server/buckethandler.h`、`persistenceengine/`。

| 操作 | 语义 | 在 Proton 内表现 |
|---|---|---|
| `put / update / remove` | 普通写 | 走 §5 的 FeedView 链路 |
| `createBucket / deleteBucket` | 给 / 收回 bucket 所有权 | 调用 `IFeedView::handleDeleteBucket`、`BucketHandler` |
| `splitBucket` | 把一个 bucket 拆成两个（哈希前缀延长） | 多条 MoveOperation |
| `joinBuckets` | 把两个 bucket 合并 | 多条 MoveOperation |
| `merge` | 副本间数据对齐 | 远端拉缺失的 doc → 本地 put |

### §10.3 Bucket 状态决定 sub-db 路由

`IBucketStateCalculator`（`proton/server/ibucketstatecalculator.h`）告诉 `CombiningFeedView` 一个 bucket 当前该属于哪个 sub-db：

- `nodeRetired() && !nodeMaintenance()` → bucket 进 NotReady（即将迁出，不需要保持搜索就绪）；
- 正常状态 + bucket 在节点 Ready 集合 → Ready；
- 已删除的 bucket → Removed（墓碑）。

```mermaid
flowchart LR
    DIST[Distributor 通知<br/>bucket state] --> CALC[IBucketStateCalculator]
    CALC --> CFV[CombiningFeedView]
    CFV -->|bucket=Ready| RFV[SearchableFeedView]
    CFV -->|bucket=NotReady| FFV[FastAccessFeedView]
    CFV -->|bucket=Removed| SFV[StoreOnlyFeedView]
```

### §10.4 BucketMoveJob

`bucketmovejob.h` 定义了一个后台 maintainer：扫描 BucketDB，找到"目前属于错的 sub-db"的 bucket，逐 doc 发出 `MoveOperation`。例子：

- NotReady 的 bucket 该变成 Ready（attribute 已加载完）→ 把 doc 从 NotReady 迁到 Ready；
- 节点 retired，Ready 的 bucket 改进 NotReady → 反向迁。

这是**proton 内部的"小搬迁"**，与 distributor 主导的"跨节点搬迁"不同层级。

### §10.5 跨节点 merge 的代价

当某副本数据少了（节点恢复 / 新加节点 / 副本不一致），distributor 触发 merge：

- 落到 Proton 端表现为大量 `put` 和 `remove`；
- 走和正常写一样的 FeedView 路径；
- 流量通过 `LoadType` 标记，FeedHandler 用专门的 throttle 防止压垮正常流量；
- 读侧（search）期间能看到 merge 进来的 doc 立刻可见（受 visibilityDelay 控制）。

### §10.6 Bucket 局部性与写入性能

`compactSpread` 可以按 bucket 重排 DocStore chunk（§7.4）。原因：

- visit / merge / get-by-bucket 是按 bucket 批量读 doc；
- 按 bucket 排好后，一次 IO 能拿到一段连续的 doc，random read → sequential read；
- 写入路径不直接受益，但写入完成后的 background flush + compact 让"未来读"更快。

---

## §11 写入优化分析

按你提到的方向（async / batch / partial-update / 读写互相影响 / 节点恢复），分三类整理：A 工程可落地、B 架构级、C 集群级。

### §11.A 工程可落地（10 条）

#### A1. 扩大 update 快路径

**现状**：`UpdateScope` 只要有一个非 attribute / 非 in-memory-updateable 字段就走慢路径（DocStore 读+写 + 索引重建）。  
**优化**：

- 让更多 attribute 类型实现 `isUpdateableInMemoryOnly()=true`：tensor 的 `modify` 操作、HNSW 的局部更新、enum-store 的纯 add 等；
- Schema 编译期生成"一定走快路径"的 update operation 校验函数，让 client 知道哪种 update 安全；
- 监控指标：暴露 `update_fast_path_ratio`，工程上以"提升此比率"为目标。

**预期收益**：典型 partial update 的 p99 延迟 ↓ 30–80%。

#### A2. 把热点字段建模成 map 而非 struct

**原因**：map 的单 key partial update 是 in-place 快路径（§4.3）；struct 的非 attribute 子字段 update 永远走慢路径。  
**做法**：业务侧对高频更新的属性集合用 `map<string, X>` 建模而非 `struct{...}`。

#### A3. AttributeFieldWriter 的 per-field hash 调优

**现状**：`ISequencedTaskExecutor` 按 field-name hash 分配线程，热点 field 可能撞同一线程 → 排队。  
**优化**：

- 给热点 field 单独 pin 一条线程（或单独执行器）；
- 使用 weighted hash（按字段历史 QPS 分配）；
- 暴露 `attribute_executor_queue_depth` per-thread metric。

#### A4. Summary executor 的并行化

**现状**：`_summaryExecutor` 通常单线程，是 doc 写入的瓶颈。  
**优化**：

- LogDocumentStore 内部 chunk 写支持并行（不同 lid 的 doc 写不同 chunk file）；
- 或在 SummaryAdapter 之前做 batch：把多个 lid 的 doc 攒成一组再 write，单次 syscall 写多 doc。

#### A5. Index thread 的 batch flush

**现状**：MemoryIndex insertDocument 是逐 doc 调用，每条都要 lock dictionary + grow posting。  
**优化**：

- FeedView 在 indexExecutor 入口处用 micro-batch（10–100 条 doc）合并 → 一次性 sort+merge posting；
- 字典 lookup 复用：同一 batch 内的 term 共享一次查询。

**注意**：会增加 commit barrier 复杂度，但大批量 ingest 收益巨大（参考 ElasticSearch 的 `refresh_interval` 思路）。

#### A6. visibilityDelay 分级

**现状**：visibilityDelay 是 DocumentDB 全局配置。  
**优化**：

- 按 LoadType 分级：客户端写 = 100ms，merge/迁入 = 1s，replay = 不 commit；
- CommitTimeTracker 支持 multi-channel；
- 配合 §11.A4 的 batch，节省 forceCommit 调用次数。

#### A7. 写入侧背压精细化

**现状**：DiskMemUsageFilter / AttributeUsageFilter 是粗粒度（按节点资源）。  
**优化**：

- 按 sub-db 分别背压（NotReady 满了不影响 Ready 写）；
- 按字段背压（某个 attribute 接近上限只拒该字段的 update）；
- 提供 `expected_resource_after_op` 计算（不是事后才发现 OOM）。

#### A8. Skip docstore for known-derivable summary

**现状**：DocStore 永远写完整 doc。  
**安全的扩展（与 §6 区分）**：

- Schema 编译期判断**所有 summary 字段都能从 attribute / index 唯一派生**；
- 这种 schema 写入时 docstore 可以**只存最小 header**（gid+timestamp），doc body 完全省去；
- update 时 docstore 不需要被读，也不需要被写——彻底跳过 `makeUpdatedDocument`。
- **注意**：必须在 schema 加载时 freeze 这个判定，运行时新加非 attribute 字段会失效（需要做 schema 升级时回写 docstore）。

#### A9. TLS group commit + async fsync

**现状**：TLS 默认每条 op append+fsync。  
**优化**：

- Group commit：N ms 内的 op 攒一起 fsync（已有，但参数可调）；
- async ack：先 ack 给 client（"已 enqueue 到 TLS"）再后台 fsync，承担 N ms 的 durability 风险换吞吐；
- 对 replay throttle policy 友好：fsync 慢时减小 replay 速率。

#### A10. PendingLidTracker 的 lock-free 化

**现状**：tracker 内部用 mutex 保护 pending set。  
**优化**：

- 用 RCU + per-lid atomic counter；
- 或按 lid hash 分桶降低锁粒度；
- 写入 fan-out 极高时（攒 100 条 doc），tracker 是潜在热点。

### §11.B 架构级（4 条）

#### B1. 编译期特化的 FeedView

当前三层继承 + 虚函数 dispatch 是**运行时多态**，每次 put 都过虚表。Schema 在加载时已知，可以做：

- **代码生成**：根据 schema 把 `internalPut` 内的 `putAttributes/putIndexedFields` 调用直接展开成对每个具体字段的写入序列，避免运行时遍历 attribute map；
- **JIT/template 实例化**：编译时按"字段集"特化 FeedView，写入路径变成纯顺序代码 + inline；
- **预期**：单 doc CPU ↓ 20–40%。

#### B2. Per-field 流水线

把 fan-out 改成 **streaming pipeline**：

```
RPC → [ParseDoc] → [Field demux] → [AttrPipe×N | IndexPipe | SummaryPipe] → Commit
```

每个 pipe 是一个 SPSC 队列，背压自动透传。优势：

- 取消 OnDone 计数器开销；
- 自然 batch（队列里多条一起处理）；
- 监控更直接。

#### B3. TLS 并行 replay

**现状**：replay 单线程顺序应用 op。  
**优化**：

- 按 bucket / lid 区间并行 replay（不同 bucket 之间无依赖）；
- 但要保证同一 lid 的 op 顺序——按 lid hash 分 partition；
- 显著缩短冷启动时间（大库可能从 30 分钟降到 5 分钟）。

**风险**：需要重做 serialNum 推进逻辑，每个 partition 独立追踪。

#### B4. DocumentStore 的二级化

**现状**：DocStore 是单层 LogDocumentStore。  
**优化**：

- 引入"hot tier"：最近 N 分钟的 doc 内存常驻（不只是 page cache）；
- "cold tier"：旧 doc 走 LogDocumentStore；
- update 慢路径首次 read 命中 hot tier → 快路径化。

### §11.C 集群级（3 条）

#### C1. Bucket-aware client routing

让 Document Processor 在 hash 时考虑**目标节点的写入压力**：高负载节点的副本由其他副本先写，最后补齐。等价于"读写分离"的写时版本。

#### C2. Replay/merge 流量与正常流量物理隔离

把 `LoadType=Merge / Replay` 的流量走独立 executor + 独立 disk IO quota：

- 用 cgroup blkio 限制 DocStore compactBloat 的 IOPS；
- 用 sched_setaffinity 把 replay 线程钉到非 latency-critical 的核；
- 用独立的 attributeFieldWriter 池给 merge 流量。

#### C3. 跨节点 update 共享

**场景**：同一 doc 的副本都收到同一 partial update。当前每个节点都各自 read-modify-write。  
**优化**：副本之一计算 "update→new doc" 后把**结果**广播给其他副本（类似 Raft 的 leader-apply），其他副本直接 put → 省去 N-1 次的 DocStore 读。

### §11.D 优化收益矩阵（粗估）

| 优化 | 写入吞吐 | 写入 p99 | 冷启动时间 | 内存 | 实施难度 |
|---|---|---|---|---|---|
| A1 update fast path | + | -- | | | 低 |
| A2 map vs struct | + | -- | | | 业务侧 |
| A3 attr executor pinning | + | - | | | 低 |
| A4 summary parallel | ++ | - | | | 中 |
| A5 index batch | ++ | + (单条慢) | | | 中 |
| A6 visibilityDelay 分级 | + | - | | | 低 |
| A7 精细背压 | | + | | | 中 |
| A8 skip docstore (safe) | ++ | -- | | -- | 中 |
| A9 TLS group commit | ++ | + | | | 低 |
| A10 lock-free tracker | + | - | | | 中 |
| B1 编译期 FeedView | ++ | - | | - | 高 |
| B2 streaming pipeline | ++ | - | | | 高 |
| B3 并行 replay | | | --- | + | 高 |
| B4 docstore hot tier | + | -- | | + | 高 |
| C1 bucket-aware routing | + | - | | | 中（架构变更）|
| C2 流量隔离 | | -- | | | 中 |
| C3 update 副本共享 | + | -- | | | 高（协议变更）|

> 表中 `+`/`++` 表示提升，`-`/`--` 表示下降（值更小 = 更好），空表示中性。

---

## 附：术语速查

| 术语 | 含义 |
|---|---|
| **LID** | Local Document Id，sub-db 内的内部 32-bit id |
| **GID** | Global Document Id，跨集群的 12 byte id |
| **SerialNum** | TLS 单调递增序号，所有组件依此对齐 |
| **Sub-DB** | Ready / NotReady / Removed 三个文档域 |
| **FeedView** | 写入路径的 polymorphic facade |
| **AttributeVector** | 列式内存结构，per-field 一份 |
| **MemoryIndex / DiskIndex** | 倒排索引的内存形态 / 磁盘形态 |
| **DocumentMetaStore (DMS)** | LID↔GID + bucket + ts |
| **LogDocumentStore (DocStore)** | 整行 doc 的持久化存储 |
| **TLS** | Transaction Log Server，proton 的 WAL |
| **Generation** | RCU 的版本号，读写隔离的基础 |
| **visibilityDelay** | 写入到搜索可见的延迟上限 |
| **Fusion** | 多个 DiskIndex 合成一个的后台过程 |
| **PendingLidTracker** | 跟踪未完成的 fan-out 操作 |
| **OnDoneContext** | 多 worker 完成回调的计数器 |

---

## 附：本文档主要代码锚点速查

| 主题 | 文件 | 行号 |
|---|---|---|
| IFeedView 接口 | `proton/server/ifeedview.h` | 26-69 |
| StoreOnlyFeedView | `proton/server/storeonlyfeedview.{h,cpp}` | h:44-248, cpp:230-499 |
| FastAccessFeedView | `proton/server/fast_access_feed_view.{h,cpp}` | 17-90 |
| SearchableFeedView | `proton/server/searchable_feed_view.{h,cpp}` | 17-196 |
| CombiningFeedView | `proton/server/combiningfeedview.{h,cpp}` | — |
| IDocumentSubDB | `proton/server/idocumentsubdb.h` | 55-126 |
| StoreOnlyDocSubDB | `proton/server/storeonlydocsubdb.{h,cpp}` | h:82-240 |
| FastAccessDocSubDB | `proton/server/fast_access_doc_subdb.{h,cpp}` | h:27-100+ |
| SearchableDocSubDB | `proton/server/searchabledocsubdb.{h,cpp}` | h:37-100+ |
| DocumentMetaStore | `proton/documentmetastore/documentmetastore.h` | 35-146 |
| LogDocumentStore | `searchlib/docstore/logdocumentstore.h` | 18-57 |
| AttributeManager | `proton/attribute/attributemanager.h` | 31-100+ |
| IAttributeWriter | `proton/attribute/i_attribute_writer.h` | 24-68 |
| AttributeWriter | `proton/attribute/attribute_writer.{h,cpp}` | 18-100+ |
| IndexManager | `proton/index/indexmanager.h` | 32-100+ |
| IIndexWriter | `proton/index/i_index_writer.h` | 14-37 |
| ExecutorThreadingService | `proton/server/executorthreadingservice.h` | 19-91 |
| FeedHandler | `proton/server/feedhandler.{h,cpp}` | h:49-200, cpp:470-491 |
| DocumentRetriever | `proton/server/documentretriever.h` | 17-64 |
| BucketHandler | `proton/server/buckethandler.h` | — |
| TransactionLogManager | `proton/server/transactionlogmanager.h` | 14-72 |
| ReplayTransactionLogState | `proton/server/feedstates.h` | 48-67 |
| ReplayPacketDispatcher | `proton/server/replaypacketdispatcher.h` | 16-33 |
| DocumentDB 启动 | `proton/server/documentdb.cpp` | 289-347, 723-743 |
| DocumentSubDBCollection | `proton/server/documentsubdbcollection.h` | 58-150 |
| BucketDB | `proton/bucketdb/bucketdb.h` | 14-77 |
| GenerationHandler | `vespalib/util/generationhandler.h` | 16-141 |
| RcuVector | `vespalib/util/rcuvector.h` | 34-100 |
| AttributeVector commit | `searchlib/attribute/attributevector.h` | 447-449 |
| CommitTimeTracker | `proton/common/commit_time_tracker.h` | 14-20 |
| visibilityDelay | `proton/server/document_db_maintenance_config.h` | 108 |
| DiskMemUsageFilter | `proton/server/disk_mem_usage_filter.h` | 24-86 |
| AttributeUsageFilter | `proton/attribute/attribute_usage_filter.h` | 22-50 |
| ReplayThrottlingPolicy | `proton/server/replay_throttling_policy.h` | 14-25 |
| BucketCreateNotifier | `proton/bucketdb/i_bucket_create_notifier.h` | 16-21 |
| BucketMoveJob | `proton/server/bucketmovejob.h` | 77 |

---

*文档版本：v1。如发现行号漂移，以源码为准。*







