# Query Evaluation 设计分析：Strict vs Non-Strict

本文档分析 Vespa `searchlib/queryeval` 中查询执行（query evaluation）的核心设计：
Blueprint 与 SearchIterator 的两层抽象，strict / non-strict 两种遍历语义为何存在，
以及它们在 AND / OR / AND_NOT / RANK / NEAR / WEAK_AND 等算子树和匹配主循环中
是如何互相配合的。

所有路径均相对于仓库根 `searchlib/src/vespa/searchlib/queryeval/`。

---

## 1. 两层抽象：Blueprint 和 SearchIterator

Vespa 的查询树有两套对象：

- **Blueprint**（`blueprint.h`）：查询的"计划期"表示。它持有每个节点的元数据
  （`FieldSpec`、`HitEstimate`、`cost_tier`、`tree_size` 等），可以被优化、重排、
  替换。它不做实际的 doc 遍历。
- **SearchIterator**（`searchiterator.h`）：查询的"执行期"表示。它是真正负责
  按 docid 推进、产出命中的对象。

Blueprint → SearchIterator 的工厂方法是：

```cpp
// blueprint.h
virtual SearchIteratorUP createSearch(fef::MatchData &md, bool strict) const = 0;
virtual bool inheritStrict(size_t i) const = 0;
```

两个关键点：

1. `createSearch` 带有一个 **`bool strict`** 参数，由父节点在构造时传入。
2. `inheritStrict(i)` 由每个 Intermediate Blueprint 重写，用来回答：
   "当我自己被要求 strict 时，我的第 i 个子节点是否也必须 strict？"

这两个方法合起来定义了 strictness 如何沿着查询树向下传播。

---

## 2. SearchIterator 的 Seek 语义

`SearchIterator` 的核心 API（`searchiterator.h`）：

```cpp
virtual void doSeek(uint32_t docid) = 0;
virtual void doUnpack(uint32_t docid) = 0;

bool     seek(uint32_t docid);        // return docid == _docid after doSeek
uint32_t seekFirst(uint32_t docid);   // return _docid after doSeek
uint32_t seekNext(uint32_t docid);    // unconditional doSeek, return _docid
vespalib::Trinary is_strict() const;  // True / False / Undefined
```

Strict / Non-strict 的契约差别都体现在 `doSeek(N)` 执行完之后 `_docid` 的取值上：

- **Non-strict iterator**：`doSeek(N)` 只回答"N 是不是命中"。
  - 如果 N 命中，`_docid == N`。
  - 如果 N 不命中，`_docid` 的状态是 *未定义* 的——调用方不能假设它指向了下一个命中，
    必须自己挑下一个候选 docid 再来 `seek`。
- **Strict iterator**：`doSeek(N)` 不仅回答 N 是否命中，还保证 `_docid` 指向
  `>= N` 的第一个命中；如果没有了就是 `endDocId`。

换言之，**strict 负责"跳过空洞"，non-strict 只负责"回答这个点"**。这是整个设计
的基石。

---

## 3. 一个具体例子：AndSearchStrict vs AndSearchNoStrict

`andsearchnostrict.h` 里的 AND 非 strict 版本极其简单：

```cpp
void doSeek(uint32_t docid) override {
    const Children & children(getChildren());
    for (uint32_t i = 0; i < children.size(); ++i) {
        if (!children[i]->seek(docid)) {
            return;            // 任一子不命中 → 直接返回
        }
    }
    setDocId(docid);
}
vespalib::Trinary is_strict() const override { return Trinary::False; }
```

它不会"往后走"，只回答调用者问的那个 docid。`andsearchstrict.h` 则要做更多事：

```cpp
void doSeek(uint32_t docid) override {
    const MultiSearch::Children & children(getChildren());
    for (uint32_t i = 0; i < children.size(); ++i) {
        children[i]->doSeek(docid);
        if (children[i]->getDocId() != docid) {
            advance<true>(i);  // 用第一个 strict 子推着整体往前走
            return;
        }
    }
    setDocId(docid);
}
vespalib::Trinary is_strict() const override { return Trinary::True; }
```

`advance<>` 的核心思路是：第一个 strict 子告诉你下一个候选 docid，然后重新去
其它子上 `seek`，失败就再让第一个子往前走。一个 strict AND 内部等价于由最稀疏的
子做"驱动者"（driver），其余子是"验证者"（checker）。

---

## 4. Strictness 为什么要分？

核心原因只有一个：**性能**。具体拆成两条理由：

### 4.1 只有一个节点需要真正"推进"扫描

考虑 `A AND B AND C`，其中 `est(A)=100, est(B)=10^6, est(C)=10^7`。
如果所有三个子都 strict，它们每个都会独立维护"我下一个命中在哪"。但实际上，
一旦我们用 A 做驱动者（每次从 A 拿一个 docid），B 和 C 就只需要回答
"是不是命中这个 docid"——这正是 non-strict 的契约。

让 B、C 只做 non-strict，可以：

- 省掉 B、C 内部维护"下一个命中"的状态管理成本；
- 让底层的 term iterator 在 B、C 上走 attribute / btree 的 "contains" 路径，
  而不是 postings 的 "nextGEQ" 路径；
- 绝大多数 non-matching 的情况在第一个子（cheap check）就被剪掉，B、C 根本
  不会被调用。

### 4.2 根节点必须 strict

匹配主循环（`searchcore/proton/matching/match_thread.cpp` 的 `inner_match_loop`）
的形态是：

```cpp
uint32_t docId = search->seekFirst(docid_range.begin);
while ((docId < docid_range.end) && !context.atSoftDoom()) {
    if (do_rank) {
        search->unpack(docId);
        context.rankHit<use_rank_drop_limit>(docId);
    } else {
        context.addHit(docId);
    }
    ...
    docId = Strategy::seek_next(*search, docId + 1);
}
```

主循环只会做 `seekFirst / seekNext`，它依赖"iterator 自己往前走"的语义——
也就是 strict。因此：

```cpp
// searchcore/proton/matching/query.cpp
SearchIterator::UP Query::createSearch(MatchData &md) const {
    return _blueprint->createSearch(md, true);   // 根节点永远 strict
}
```

一棵查询树的 strictness 就是这样：**从根的 `true` 出发，通过每个节点
`inheritStrict(i)` 的规则往下传递，直到叶子**。

---

## 5. 每种算子的 strictness 传播规则

摘自 `intermediate_blueprints.cpp`：

| Blueprint        | `sort(children)`                         | `inheritStrict(i)`          |
|------------------|------------------------------------------|-----------------------------|
| `AndBlueprint`   | `TieredLessEstimate`（估计小的靠前）     | `i == 0`                    |
| `OrBlueprint`    | `TieredGreaterEstimate`（估计大的靠前）  | `true`（全部）              |
| `AndNotBlueprint`| 正项第 0 固定，其余按大小排              | `i == 0`                    |
| `RankBlueprint`  | 顺序保留                                 | `i == 0`                    |
| `NearBlueprint`  | `TieredLessEstimate`                     | `i == 0`                    |
| `ONearBlueprint` | 顺序保留                                 | `i == 0`                    |
| `WeakAndBlueprint` | 顺序保留（对应 `_weights`）            | `true`（全部）              |
| `SourceBlender`  | 顺序保留                                 | `true`（全部）              |

每一条规则都有一个直观解释：

- **AND**：排序把最稀疏的子放到 0 号位，作为驱动者，它继承 strict；其它子只
  做 contains 检查，non-strict。
- **OR**：所有子都要真正产出各自的命中序列才能合并；因此所有子都是 strict。
  排序把估计大的放前面是为了让 heap-merge 更稳定、减少小子频繁上浮。
- **AND_NOT**：形如 `A AND NOT(B,C,...)`，等价于 A 驱动、B/C 作为过滤。
  A 要 strict，B/C 只需要 non-strict 做否定检查。
- **RANK**：`RANK(A, B)` 的命中集合完全由 A 决定，B 只提供 rank feature。
  A strict，B 只需要在 A 命中的位置被 unpack。
- **NEAR / ONEAR**：做位置匹配，第一子做驱动者 strict，其它子在 0 号位命中
  处检查位置是否足够接近，non-strict 足矣。
- **WEAK_AND**：采用 WAND 算法（`WeakAndSearch`），内部以阈值淘汰，需要每个
  term 都能独立给出下一个命中，因此全部 strict。
- **SourceBlender**：按 source 分流后每一路是独立子搜索，每一路都要能独立
  推进，所以全部 strict。

IntermediateBlueprint 的基础实现里把传播拼起来：

```cpp
// blueprint.cpp：IntermediateBlueprint::createSearch
for (size_t i = 0; i < _children.size(); ++i) {
    bool strictChild = (strict && inheritStrict(i));
    subSearches.push_back(_children[i]->createSearch(md, strictChild));
}
return createIntermediateSearch(std::move(subSearches), strict, md);
```

注意是 `strict && inheritStrict(i)`：**父节点一旦被要求 non-strict，整个子树都
只能是 non-strict**。这保证了"strict 子树最多只有一个从根延伸的路径"这个不变量。

---

## 6. Cost 模型与 Flow

Vespa 这版本还没有独立的 `AndFlow` / `OrFlow` 类，流控制是通过三件事隐式表达的：

1. **HitEstimate**（`blueprint.h`）：每个 Blueprint 都向上报告自己的命中估计，
   AND 用 `min`、OR 用 `sat_sum`、AND_NOT 用第一子的估计组合。
2. **Cost tier**：`COST_TIER_NORMAL=1`，`COST_TIER_EXPENSIVE=2`。昂贵的算子
   （例如复杂 phrase、global filter 查询）被归到更高 tier，排序时优先比较 tier。
3. **Tiered 排序器**：

   ```cpp
   struct TieredLessEstimate {
       bool operator()(Blueprint *a, Blueprint *b) const {
           const auto &l = a->getState(), &r = b->getState();
           if (l.cost_tier() != r.cost_tier())
               return l.cost_tier() < r.cost_tier();
           return l.estimate() < r.estimate();
       }
   };
   ```

   AND 用 `TieredLessEstimate`（把便宜且稀疏的放前面），OR 用
   `TieredGreaterEstimate`（把便宜但密的放前面）。

这个 cost 模型与 strictness 之间的耦合是：**排序决定了谁在 0 号位，
0 号位决定了谁继承 strict**。因此"估计准不准"直接影响"谁成为 strict 驱动者"，
进而影响整体的 seek 次数。叶子节点（attribute / disk index / btree）里
`HitEstimate` 的质量对 AND 查询的性能非常关键。

`AndBlueprint::computeNextHitRate` 还会把积累的 hit rate 沿子链路复合衰减，
用来给后续子节点更真实的估计，从而让子节点再决定自己内部是否要做 posting-cutoff
等优化。这是一个轻量的"递推 flow"，在没有显式 flow 对象的情况下起到类似作用。

---

## 7. 在 BitVector 合流时的 strict 使用

`searchiterator.cpp` 中的 `and_hits_into` 很好地展示了 strict 带来的算法级差别：

```cpp
void SearchIterator::and_hits_into(BitVector &result, uint32_t begin_id) {
    if (is_strict() == vespalib::Trinary::True) {
        and_hits_into_strict(result, begin_id);   // O(n+m) merge-join
    } else {
        and_hits_into_non_strict(result, begin_id); // O(popcount) point-check
    }
}
```

- **strict 路径**：把 iterator 的命中序列和 BitVector 的 set bits 当两条有序流
  做 merge-join，指针一起走，中间可以 `clearInterval` 批量清零，访存友好。
- **non-strict 路径**：遍历 BitVector 的每一个 true bit，对 iterator `seek` 一次；
  如果未命中就清零。代码更简单，但每一个候选 docid 都要做一次完整的 seek。

这一段代码是理解"为什么 strict 有价值"的最干净的样本：同一个语义
（iterator 和 bitvector 做 AND），strict 版本直接砍掉一半 seek 成本。

---

## 8. Termwise 评估和 strict

`intermediate_blueprints.cpp` 里的 AND / OR / AND_NOT `createIntermediateSearch`
还会检查 `termwise_limit`，把若干个便宜的叶子先合并成一个"termwise"子搜索：

```cpp
bool termwise_strict = (strict && inheritStrict(helper.first_termwise));
auto termwise_search = AndSearch::create(helper.get_termwise_children(), termwise_strict);
helper.insert_termwise(std::move(termwise_search), termwise_strict);
```

要点：termwise 合并体会**占用原来第一个（或其它）termwise 子的位置**，因此它是
否 strict 取决于被它代替的那个槽位的 strictness。这里仍然是 `inheritStrict`
规则在起作用——并不是某种特殊豁免。这也验证了一个设计约束：`strict` 是
**按槽位** 而不是按子 Blueprint 对象决定的。

---

## 9. 与 Matching / Ranking 流程的交互

把上面所有东西串起来，`searchcore/proton/matching/` 里的匹配流程是这样的：

1. `Query::createSearch(md)` 以 `strict=true` 为根调用 `Blueprint::createSearch`，
   构造整棵 iterator 树。根一定 strict。
2. `MatchThread::inner_match_loop` 拿到根 iterator，`seekFirst` / `seekNext` 驱动。
   这个循环只相信 strict 契约。
3. 命中点上：
   - `unpack(docId)` 只在需要 rank feature 的 iterator 上被调用
     （由 `UnpackInfo` 决定），避免在非 rank 路径上做无谓工作。
   - `context.rankHit` 调 rank framework 做第一阶段打分。
4. `RANK(a, b)` 的 b 虽然 non-strict，但会在 a 命中处被 `unpack`，这样 rank
   表达式才能访问 b 的 term feature。这正是 "RANK 的第 1 个槽位以后都 non-strict"
   的意义：他们不贡献集合，只贡献特征。
5. `AND_NOT` 的 b...c 子在 non-strict 模式下只会被第 0 号正项命中时的 docid
   探测，达到"否定过滤器"的效果。

这些流程之所以能写得这么简洁，完全建立在"只有一条 strict 链从根通到某个叶子"
这个不变量之上。如果没有 strict / non-strict 区分，整棵树的每个算子都得实现
"推进下一个命中"的完整逻辑，AND 的第二子要维护自己的状态、RANK 的 b 子要
真的扫整个索引，这些开销在大 corpus 下是不可接受的。

---

## 10. 设计要点小结

1. **两段式抽象**：Blueprint 负责规划（估计、排序、strictness 决策），
   SearchIterator 负责执行。
2. **Strict 契约**：`doSeek(N)` 之后 `_docid >= N`，指向下一个命中或 endDocId。
3. **Non-strict 契约**：`doSeek(N)` 只保证回答 "N 是否命中"。
4. **不变量**：根一定 strict；`strict && inheritStrict(i)` 决定子 strictness；
   一个 intermediate 算子内部最多只有一个"驱动槽位"继承 strict。
5. **规则映射**：
   - 驱动 + 过滤 类算子（AND, AND_NOT, NEAR, ONEAR, RANK）→ 只有 0 号 strict；
   - 合流类算子（OR, WeakAnd, SourceBlender）→ 全部 strict。
6. **Cost 与 strict 的耦合**：`sort` 决定 0 号槽位的身份，HitEstimate 和
   cost_tier 决定排序结果。估计质量直接影响 strict 驱动者是否稀疏。
7. **算法效益**：strict 让 BitVector 合流能做 merge-join（O(n+m)），让 AND
   能用"稀疏子驱动 + 密集子检查"的策略，让主循环只用 seekFirst / seekNext 就能
   跑完整个查询。
8. **与 Matching/Ranking 的接口**：根 strict 保证 `inner_match_loop` 的简洁；
   non-strict 保证 rank-only 的子树（例如 RANK 的 b）不会消耗扫描预算。

这一套设计的本质是：**通过类型上的两种 seek 契约，把"谁驱动扫描、谁做点查"
这个调度问题在编译期就决定下来**，从而把高频路径上的 iterator 实现做到极致简洁。
