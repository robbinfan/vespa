# Vespa Sort 实现解析

本文档梳理 Vespa 中**自定义字段排序**（sorting）的完整实现路径，从 query 到达 content node、到 per-hit `sortdata` 的生成与序列化、再到 container 端如何把多个 content node 返回的有序结果合并成一个全局有序结果。

重点回答两个常见疑问：

1. `sortdata` 这个字节串到底是什么？里面装的是什么？为什么 container 不需要"懂"字段类型就能比较？
2. container 是怎么把多份分片的结果合成一份的？

---

## 0. 总体思路一句话

> Content node 把每条命中的所有排序字段，**按 sort spec 的顺序**编码成一个**可以直接 `memcmp` 的字节串**，称为 `sortdata`。Container 把这些字节串当成不透明 blob，**不解码**直接做字节比较，从而完成多分片归并。

整个设计的精髓在于"把语义比较转换成字节比较"，container 因此不需要任何字段类型/locale/升降序的知识。

---

## 1. Query 中的 sort spec 是怎么传到 content node 的

### 1.1 Wire 协议

Search 协议（`searchlib/src/protobuf/search_protocol.proto`）：

```protobuf
message SearchRequest {
    ...
    repeated SortField sorting = 5;
    ...
}

message SortField {
    bool ascending = 1;
    string field = 2;
}
```

每个排序字段在协议里只是一对 `(ascending, field_name)`，其中 `field_name` 既可以是普通 attribute，也可以是函数形式的字符串，例如 `uca(title,en_US,PRIMARY)` 或 `lowercase(name)`。

### 1.2 协议层 → 内部字符串 spec

Container 把 protobuf 的 sort 字段集合转成一行紧凑字符串，例如 `+price -timestamp +uca(title,en_US)`：

`searchlib/src/vespa/searchlib/engine/proto_converter.cpp:18`

```cpp
template <typename T>
vespalib::string make_sort_spec(const T &sorting) {
    vespalib::string spec;
    for (const auto &field_spec: sorting) {
        if (!spec.empty()) spec.push_back(' ');
        spec.push_back(field_spec.ascending() ? '+' : '-');
        spec.append(field_spec.field());
    }
    return spec;
}
```

### 1.3 Content node 解析

`searchlib/src/vespa/searchlib/common/sortspec.cpp` 把这串文本解析成 `std::vector<SortInfo>`，每个 `SortInfo` 包含：

- 字段名
- `_ascending`（true/false）
- 可选的 `BlobConverter`（例如 UCA collation、`lowercase` 等）

UCA / lowercase 这类不能直接 `memcmp` 的字段，需要 converter 先把原值 transform 成 collation key。后续步骤都不会再去看原值。

---

## 2. Content node 上 per-hit `sortdata` 的生成

核心类是 `FastS_SortSpec`，定义在 `searchlib/src/vespa/searchlib/common/sortresults.h`，实现在 `sortresults.cpp`。

### 2.1 数据结构

`sortresults.h:89`

```cpp
struct SortData : public search::RankedHit {
    uint32_t _idx;   // 该 hit 的 sortdata 在大缓冲区里的起始偏移
    uint32_t _len;   // 该 hit 的 sortdata 字节长度
    uint32_t _pos;   // radix sort 用的临时位置
};
```

`FastS_SortSpec` 内部维护两块东西：

```cpp
vespalib::Array<uint8_t>  _binarySortData;   // 所有命中的 sortdata 拼成一个大缓冲区
vespalib::Array<SortData> _sortDataArray;    // 每条命中一个 (idx, len)
```

也就是说：**所有命中的 sortdata 都连续存在一块大字节缓冲里**，`SortData` 数组只是在这个大缓冲上开窗口（offset + length）。这样既省内存又对 cache 友好。

### 2.2 编码每条命中（`initSortData`）

`sortresults.cpp:220` 附近的 `initSortData(const RankedHit *a, uint32_t n)`：对每条 hit 按 sort spec 中的字段顺序逐个写入 `_binarySortData`：

```cpp
for (uint32_t i = 0; i < n; ++i) {
    uint32_t len = 0;
    for (auto &v : _vectors) {
        int written = 0;
        switch (v._type) {
        case ASC_DOCID:
            serializeForSort<convertForSort<uint32_t,true>>(hits[i].getDocId(),  mySortData);
            serializeForSort<convertForSort<uint16_t,true>>(_partitionId,        mySortData + sizeof(uint32_t));
            written = sizeof(uint32_t) + sizeof(uint16_t);
            break;
        case DESC_DOCID:  /* 同上但 ascending=false */ break;
        case ASC_RANK:
            serializeForSort<convertForSort<HitRank,true>>(hits[i].getRank(), mySortData);
            written = sizeof(HitRank);
            break;
        case DESC_RANK:   /* 同上 */ break;
        case ASC_VECTOR:
            written = v._vector->serializeForAscendingSort(
                          hits[i].getDocId(), mySortData, available, v._converter);
            break;
        case DESC_VECTOR:
            written = v._vector->serializeForDescendingSort(
                          hits[i].getDocId(), mySortData, available, v._converter);
            break;
        }
        available -= written;
        mySortData += written;
        len += written;
    }
    sd._idx = idx;
    sd._len = len;
    idx += len;
}
```

要点：

- 字段是**按 sort spec 顺序串接**进入缓冲区，没有任何分隔符、没有 length prefix。
- 升 / 降是在编码阶段就处理好的（不同的 `serializeForAscendingSort` / `serializeForDescendingSort`）。
- 变长字段（字符串、UCA collation key）由 attribute 自己负责写入字节，必要时 `realloc` 扩容。
- 每条 hit 编码完成后只记下 `(idx, len)`，原值就再也不需要了。

### 2.3 字段实际编码（让 `memcmp` 等价于语义比较）

定义在 `staging_vespalib/src/vespa/vespalib/util/sort.h` 的 `convertForSort<T, asc>` 模板。这是整套设计能成立的关键：

**有符号整数（升序）**

```cpp
static inline uint32_t convert(int32_t v) {
    return v ^ (uint32_t(INT32_MAX) + 1);    // 翻 sign bit
}
```

XOR `0x80000000` 让最负的数变成 `0x00000000`、最正的数变成 `0xFFFFFFFF`，无符号字节顺序就和有符号大小顺序一致。

**有符号整数（降序）**

```cpp
static inline uint32_t convert(int32_t v) {
    return v ^ INT32_MAX;                    // 翻除符号位外所有位 == 反转顺序
}
```

**double（升序）** —— `sort.h:53`

```cpp
static inline uint64_t convert(double v) {
    union { double f; uint64_t u; } x; x.f = v;
    return (int64_t(x.u) >= 0)
            ? (x.u ^ (uint64_t(INT64_MAX)+1))   // 正：翻最高位
            : (x.u ^ UINT64_MAX);               // 负：全部翻
}
```

IEEE 754 的位表示有个特点：正数位序天然单调，但负数是"越负越大"。两段公式正好把它修成全局单调，再 `memcmp` 即可。

**无符号整数升序**：原样拷贝；**降序**：按位取反。

**字符串**：升序原样写入并以 `\0` 结尾；降序逐字节取反。

最后所有数值都通过

```cpp
template <typename C>
uint32_t serializeForSort(typename C::InputType v, void *dst) {
    typename C::UIntType nbo(vespalib::nbo::n2h(C::convert(v)));
    memcpy(dst, &nbo, sizeof(nbo));
    return sizeof(nbo);
}
```

写成**网络字节序（big-endian）**——这一步不是为了跨机器兼容（毕竟原本就是字节流），而是为了让 `memcmp` 从高位 byte 开始比较时顺序正确。

### 2.4 排序

`sortresults.cpp:482` 附近，根据 `_method` 三选一：

| method | 算法 |
| --- | --- |
| 0 | `search::qsort`（自定义快排） |
| 1 | `std::sort` + `StdSortDataCompare` |
| 2 | `search::radix_sort`（默认） |

无论哪种，比较器都是 `memcmp` + 长度 tiebreak：

```cpp
int cmp(const SortData &a, const SortData &b) const {
    uint32_t len = std::min(a._len, b._len);
    int r = memcmp(_sortSpec + a._idx, _sortSpec + b._idx, len);
    return r ? r : int(a._len) - int(b._len);
}
```

排完序后回写 hits 数组的顺序，`_binarySortData` + `_sortDataArray` 同时也成为响应里要带回去的 `sortdata`。

---

## 3. `sortdata` 在 SearchReply / 协议里的样子

### 3.1 SearchReply 内部表示

`searchlib/src/vespa/searchlib/engine/searchreply.h:69`

```cpp
std::vector<uint32_t> sortIndex;   // 长度 = hits + 1，类似 CSR offset
std::vector<char>     sortData;    // 所有 hit 的 sortdata 拼成的大字节缓冲
```

布局正好是 CSR：第 `i` 条 hit 的 sortdata 是 `sortData[sortIndex[i] .. sortIndex[i+1])`。这和 content node 内部的 `_binarySortData` + `_sortDataArray` 是同构的。

### 3.2 写入 protobuf

`searchlib/src/vespa/searchlib/engine/proto_converter.cpp:86` 附近：

```cpp
bool has_sort_data = (reply.sortIndex.size() > 0);
assert(!has_sort_data || (reply.sortIndex.size() == reply.hits.size() + 1));

for (size_t i = 0; i < reply.hits.size(); ++i) {
    auto *hit = proto.add_hits();
    hit->set_global_id(reply.hits[i].gid.get(), GlobalId::LENGTH);
    hit->set_relevance(reply.hits[i].metric);
    if (has_sort_data) {
        size_t off = reply.sortIndex[i];
        size_t sz  = reply.sortIndex[i+1] - reply.sortIndex[i];
        hit->set_sort_data(&reply.sortData[off], sz);
    }
}
```

对应的 proto 字段：

```protobuf
message Hit {
    bytes  global_id    = 1;
    double relevance    = 2;
    bytes  sort_data    = 3;          // 就是它
    repeated Feature match_features = 4;
}
```

注意 `Hit.sort_data` 是 `bytes`：**协议层不知道里面有多少字段、什么类型、升降序如何**。那些信息全部"烘焙"进字节里了。

---

## 4. Container 端怎么把多份分片结果合起来

### 4.1 `LeanHit`：分发器层的轻量 hit

`container-search/src/main/java/com/yahoo/search/dispatch/LeanHit.java`

```java
public class LeanHit implements Comparable<LeanHit> {
    private final byte[] gid;
    private final double relevance;
    private final byte[] sortData;     // 直接保存 protobuf 里那块 bytes
    ...

    @Override
    public int compareTo(LeanHit o) {
        int res = (sortData != null)
                ? compareData(sortData, o.sortData)
                : Double.compare(o.relevance, relevance);
        return (res != 0) ? res : compareData(gid, o.gid);
    }

    private static int compareData(byte[] left, byte[] right) {
        int i = Arrays.mismatch(left, right);
        if (i < 0) return 0;
        int max = Math.min(left.length, right.length);
        if (i >= max) return left.length - right.length;
        int vl = left[i] & 0xFF;
        int vr = right[i] & 0xFF;
        return vl - vr;     // 无符号字节比较 == Java 版 memcmp
    }
}
```

`Arrays.mismatch` 找出第一处不同的字节下标，然后把字节当 `unsigned` 比 —— 这就是 Java 里 `memcmp` 的等价物。`LeanHit` 完全不需要知道 sortdata 里到底有几个字段、是什么类型、是升序还是降序。

### 4.2 `InterleavedSearchInvoker`：归并多分片

`container-search/src/main/java/com/yahoo/search/dispatch/InterleavedSearchInvoker.java:215` 附近的 `mergeResult`：

```java
List<LeanHit> partial = partialResult.getLeanHits();   // 新到的某分片的有序结果
List<LeanHit> merged  = new ArrayList<>(needed);

while (indexCurrent < current.size() && indexPartial < partial.size() && merged.size() < needed) {
    LeanHit incomingHit = partial.get(indexPartial);
    LeanHit currentHit  = current.get(indexCurrent);
    int cmpRes = currentHit.compareTo(incomingHit);
    if (cmpRes < 0) { merged.add(currentHit);  indexCurrent++; }
    else if (cmpRes > 0) { merged.add(incomingHit); indexPartial++; }
    else { merged.add(currentHit); indexCurrent++; indexPartial++; }
}
// 把剩余的尾巴接上
```

这是一段标准的"两个有序列表合并"。每收到一个分片就和当前 running 结果做一次 merge，截断到 `needed`（offset + hits）。因为每个分片送来的 `LeanHit` 列表本身在 content node 上已经按同一种 `memcmp` 序排好了，这种线性 merge 给出的就是全局有序结果。

### 4.3 `FastHit` / `SortDataHitSorter`：暴露给用户层

到 `Result` 这一层的命中是 `FastHit`，它也保存原始 sortdata：

`container-search/src/main/java/com/yahoo/prelude/fastsearch/FastHit.java`

```java
private transient byte[] sortData = null;
private transient Sorting sortDataSorting = null;

public void setSortData(byte[] data, Sorting sorting) {
    this.sortData = data;
    this.sortDataSorting = sorting;
}

static int compareSortData(FastHit left, FastHit right, Sorting sorting) {
    if (!left.hasSortData(sorting) || !right.hasSortData(sorting)) return 0;
    int i = Arrays.mismatch(left.sortData, right.sortData);
    if (i < 0) return 0;
    int max = Math.min(left.sortData.length, right.sortData.length);
    if (i >= max) return left.sortData.length - right.sortData.length;
    return (left.sortData[i] & 0xFF) - (right.sortData[i] & 0xFF);
}
```

`SortDataHitSorter` 把这套比较器接到一般 `HitGroup` 上，`Hit` 没有 sortdata 时还能 fallback 到普通 comparator：

```java
public static Comparator<Hit> getComparator(Sorting sorting, Comparator<Hit> fallback) {
    if (fallback == null)
        return (l, r) -> compareTwo(l, r, sorting);
    return (l, r) -> compareWithFallback(l, r, sorting, fallback);
}
```

---

## 5. 把流程整体串一遍

```
                              query 里的 sorting
                                    │
                                    ▼
            container: ProtoConverter.make_sort_spec   "+price -ts +uca(title,en_US)"
                                    │
                                    ▼
                            搜索协议 SearchRequest.sorting
                                    │
                                    ▼
        content node: SortSpec parse → vector<SortInfo>(field, asc, converter)
                                    │
                                    ▼
        FastS_SortSpec::initSortData
            for each hit:
                for each sort field:
                    convertForSort<T, asc>(value)  // 让 memcmp 正确
                    serializeForSort -> 写入 _binarySortData
                记录 (idx, len)
                                    │
                                    ▼
        memcmp 排序（radix / std::sort / qsort 任选）
                                    │
                                    ▼
        SearchReply { sortIndex[], sortData[] }   ← CSR 布局
                                    │
                                    ▼
        proto_converter.search_reply_to_proto
            Hit.sort_data = bytes              ← 协议层不解释
                                    │
                                    ▼
        container: ProtobufSerialization → LeanHit(sortData)
                                    │
                                    ▼
        InterleavedSearchInvoker.mergeResult
            两个有序列表线性 merge
            比较器 = LeanHit.compareTo = unsigned-byte memcmp
                                    │
                                    ▼
        FastHit.setSortData / SortDataHitSorter
            最终 Result 中的 hit 仍然按 sortdata 排序
```

---

## 6. `sortdata` 一个具体例子

排序规格：`+age -score`（age 是 int32，score 是 double）。

某条 hit 的字段值：`age=30, score=12.5`。

content node 写出的 sortdata 字节布局：

```
┌───────────────────────────┬───────────────────────────────────────┐
│ age 编码 (4 bytes, asc)   │ score 编码 (8 bytes, desc)            │
└───────────────────────────┴───────────────────────────────────────┘
```

- `age=30`：先 `convertForSort<int32_t,true>(30) = 30 ^ 0x80000000 = 0x8000001E`，再网络序写入 → `80 00 00 1E`
- `score=12.5`（降序）：先转 IEEE 754，再走 `convertForSort<double,false>`（正数 ⇒ XOR `INT64_MAX` 反转顺序），再 big-endian 写入 → 8 个字节

把两块拼起来就是 12 字节的 sortdata。比较两条 hit 时：

1. `memcmp` 先碰前 4 个字节 ⇒ `age` 升序的天然结果（因为符号位翻过了）。
2. 前 4 字节相等才会比到后 8 字节 ⇒ `score` 降序的结果。

这正好就是"先按 age 升序、再按 score 降序"的字典序定义。Container 的 Java 代码做完全一样的事，只是用 `Arrays.mismatch` + `(b & 0xFF)` 实现 unsigned memcmp。

---

## 7. 这个设计的取舍

**优点**

- **零类型依赖**：container 不需要 schema，不需要知道 collator，不需要知道升降序。完全靠字节。
- **网络紧凑**：不重复传字段值，只传一段已经"压扁"的 key。
- **合并极快**：byte-level compare，没有装箱、没有反射、没有 locale lookup。
- **可拓展 converter**：UCA 这种复杂排序也只是在 content node 上多一次 transform，下游同样无感。

**代价**

- **不可读**：`sortdata` 是不透明字节流，单看一个 hit 的 sortdata 看不出原始字段值。
- **size 估算**：变长字段需要预估 / `realloc`（`FastS_SortSpec::realloc`）。
- **耦合**：编码格式和"如何用 memcmp 比"紧紧绑定，未来如果想改某个类型的 byte 表示就要同时升级 content + container。
- **变长字段无 length prefix**：靠每个字段自带终结符（比如字符串的 `\0`）或固定宽度来分界；改变长字段编码时要小心。

---

## 8. 关键文件速查表

| 模块 | 文件 |
| --- | --- |
| 协议 | `searchlib/src/protobuf/search_protocol.proto` |
| Container 拼 spec 字符串 | `searchlib/src/vespa/searchlib/engine/proto_converter.cpp` |
| Reply 协议结构 | `searchlib/src/vespa/searchlib/engine/searchreply.h` |
| Sort spec 解析 | `searchlib/src/vespa/searchlib/common/sortspec.{h,cpp}` |
| Per-hit 编码 + 排序 | `searchlib/src/vespa/searchlib/common/sortresults.{h,cpp}` |
| memcmp-兼容编码 | `staging_vespalib/src/vespa/vespalib/util/sort.h` |
| Java LeanHit / 比较 | `container-search/src/main/java/com/yahoo/search/dispatch/LeanHit.java` |
| Java 多分片归并 | `container-search/src/main/java/com/yahoo/search/dispatch/InterleavedSearchInvoker.java` |
| Java FastHit sortdata | `container-search/src/main/java/com/yahoo/prelude/fastsearch/FastHit.java` |
| Java HitGroup 排序入口 | `container-search/src/main/java/com/yahoo/prelude/fastsearch/SortDataHitSorter.java` |
