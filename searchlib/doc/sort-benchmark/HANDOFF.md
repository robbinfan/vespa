# Handoff — lazy top-K sort for FastS_SortSpec

分支：`claude/document-sort-implementation-hzb6B`
相关 commits：
- `8e5916f4` — `doc: add sort implementation walkthrough`（前置文档）
- `8ae939bd` — `searchlib: add lazy top-K sort path to FastS_SortSpec`（本次改动）

## TL;DR

在 `FastS_SortSpec::sortResults` 顶部加了一条 **lazy top-K 路径**：当 `topn << n`
且第一个 sort 字段是便宜的定宽 attribute 时，只对全部 hit 编码第一字段，用
`std::nth_element` 选出候选集（上限 `4 * topn`），再对候选集完整编码 + 排序。
条件不满足或候选集爆炸时**完全透明地 fallback** 到现有的 radix/std::sort/qsort。

独立 harness 实测：在多字段 + 尾部字段昂贵（模拟 UCA collator）的场景下比
全排序快 **17-25 倍**；纯 cheap int 场景退化到与 partial_sort 相当。

## 状态

| 项目 | 状态 |
| --- | --- |
| 核心实现 (`sortresults.{h,cpp}`) | ✅ 已提交 |
| 单元测试 (`multilevelsort.cpp`) | ✅ 新增 `testLazyTopK` 覆盖 5 个场景 |
| 独立 L1 harness | ✅ 差分 126/126 全绿 |
| 本机 syntax check（stub 头 + clang） | ✅ 过 |
| **真实 searchlib 构建 / 单元测试运行** | ❌ 本机缺 `/opt/vespa-deps`，**需要在开发机跑一次** |
| Proton 集成测试 | ❌ 同上 |
| 端到端多分片验证 | ❌ 同上 |

## 改动文件

```
searchlib/src/vespa/searchlib/common/sortresults.h          +22 行
searchlib/src/vespa/searchlib/common/sortresults.cpp       +118 行
searchlib/src/tests/sortspec/multilevelsort.cpp            +170 行
searchlib/doc/sort-benchmark/                              + 新增目录（harness）
```

核心函数：

- `FastS_SortSpec::lazyFirstFieldIsCheap() const` — gate，判断第一字段是否适合 lazy
- `FastS_SortSpec::tryLazyTopK(RankedHit *a, uint32_t n, uint32_t topn)` — 主路径，成功返回 true
- `FastS_SortSpec::sortResults` — 只改了最前面加一次 `if (tryLazyTopK(...)) return;`，原有所有路径原封不动保留

## 在开发机上需要跑的验证

**这些步骤必须在有 `/opt/vespa-deps` 的构建机上执行**，我在 container 里没办法做。

```bash
# 1. 完整编译 searchlib
cd <vespa-root>
./bootstrap.sh
cd build && make -j<N> searchlib

# 2. 跑 sort 相关 unit tests
ctest -R 'sortresults|sortspec|multilevelsort' --output-on-failure

# 最关键的是新加的那个 test：
#   "require that lazy top-K path agrees with full sort on the top K winners"
# 它对齐了 reference sorter（method=2 全排）和 topk sorter（lazy 路径）的
# 前 topn 条 sortdata，byte-for-byte 比较。

# 3. 如果可能，跑一次 proton match 层的测试
ctest -R 'matching' --output-on-failure
```

如果 unit test 挂了，**最可能的几个地方**：

1. `_sortDataArray.resize(winners)` —— `vespalib::Array::resize(n)` 理论上等价于
   `std::vector::resize`，但如果 vespalib::Array 的 resize 语义有差异（比如 shrink
   会释放内存导致 `_binarySortData` 里残留的索引失效），可能需要改成不 resize，
   而是在 `getSortRef`/`copySortData` 那边用显式长度。这个改动是可选的。
2. `initSortData` 内部的 `freeSortData()` 把 `_binarySortData` 也清了 —— 我的
   lazy 路径正是依赖这个，因为 lazy 第一阶段用的是 *local* `f0bytes` scratch，
   不是 `_binarySortData`，所以 freeSortData 清空后重建是干净的。如果将来有人
   把 initSortData 改成"增量追加"，这里的假设就破了。
3. UCA converter 路径：我用 `lazyFirstFieldIsCheap()` 把有 converter 的字段全拒
   了，所以理论上不会走到。但如果 `serializeForAscendingSort(..., converter=nullptr)`
   在真正的 UCA attribute 上行为异常（比如跨线程状态），我的 gate 可能放进去
   了它不该放的字段。test 里应该能暴露出来。

## 性能验证（可选，但推荐）

harness 跑得到的是 std::sort 为基线的合成数据结果。真实 Vespa 用的是 radix
sort，constants 差距会比 harness 显示的小。建议在真实 proton 上选一个多字段
排序的 query 做对比：

```
# 对照：默认不会触发 lazy（比如 topn 很大或单字段）
# 实验：topn=20 + 2+ attribute 字段 + 第一字段有区分度
```

看 proton 的 match-phase 时间分布（或者加个 log 统计 `tryLazyTopK` 的 hit
rate），确认：
- lazy 被触发的 query 比例
- 触发时 candidate 集大小分布（避免太多命中 fallback）
- 触发时比 fallback 路径快多少

如果 hit rate 很低 → 可能需要放宽 gate（降 `LAZY_MIN_N` 或提 `LAZY_MAX_CAND_FACTOR`）。
如果 hit rate 很高但加速不明显 → 说明 Vespa 的 radix sort 已经够快，lazy 的
省 encode 不值那几次扫描。

## 可调 heuristic（在 `sortresults.cpp` 顶部的匿名 namespace）

```cpp
constexpr uint32_t LAZY_MIN_N            = 256;  // 最小 n 门槛
constexpr uint32_t LAZY_MAX_CAND_FACTOR  = 4;    // 候选集上限 = factor * topn
```

门槛选择依据（见 `findings.md`）：

- `LAZY_MIN_N=256`：低于这个 N，nth_element 的常数开销 + 两次缓冲区分配 ≈
  legacy 全排的总成本，没得赚。
- `LAZY_MAX_CAND_FACTOR=4`：基于 harness 的 fallback 开销测量（约 +10-20%）。
  更小 → 更多 fallback、更少误触发；更大 → 更激进、但 tie-heavy 字段会退化。

两个值都是**拍的**。上线前建议用真实 workload 扫一遍。

## 不会触发 lazy 的情况（by design）

这些都是 by design 的 fallback，**不是 bug**：

1. `n < 256` —— 太小，不值得
2. `topn * 4 > n` —— topn 占比太高，partial_sort 也赢不了多少
3. 只有 1 个 sort 字段 —— lazy 的省 encode 前提是有尾部字段可省
4. 第一字段是 RANK / DOCID —— 这两个本身编码就是 6/8 字节常数时间，省不了什么
5. 第一字段是 attribute 但有 converter（UCA、lowercase）—— 可能变长
6. 第一字段是 string attribute（`getFixedWidth() == 0`）—— 变长
7. 第一字段是 multi-value —— 排序管线已经不支持
8. 候选集在 tie 扫描阶段超过 `4 * topn` —— 第一字段基数太低

第 5、6 两种场景其实是**最容易有 expensive tail field** 的（字符串排序+
数字 tiebreak），但 lazy 第一字段的 encode 本身就贵。做它们需要"只编前缀
字节"这类更复杂的优化，属于下一轮。

## 下一步（按性价比排序）

### Phase 2.5：放宽 gate

- **支持 RANK 作为第一字段**。只要尾部字段昂贵，rank 是便宜的前缀。只需要
  在 `lazyFirstFieldIsCheap` 里允许 `ASC_RANK/DESC_RANK`，并在 step 1 的
  encode 循环里用 `serializeForSort<convertForSort<HitRank, ...>>` 替代
  attribute 调用。估计半天能做完。

- **支持 DOCID 同理**。

### Phase 3：方向 2 — Block-Max attribute range pruning

对 integer/float attribute 暴露 per-block min/max，在 lazy 的 step 1 里整段跳
过不可能入围的 block。需要改 `IAttributeVector` 接口或加 side channel。
影响面大，review 周期长，建议**等 lazy 在生产跑稳之后再做**。

### Phase 4：方向 4 — 把 sort 推进 match iterator

真·WAND for sort：让 match iterator 按 sort key 顺序产出 doc，攒够 K 条且
后续 sort key 不可能改变 top-K 时停止。收益最大，改动也最大。建议**独立立项**。

Vespa 现有 `match-phase` 配置已经做了这一类的近似版本，可以先去看它的代码
再决定是扩展它还是另起炉灶。相关文件：

- `searchcore/src/vespa/searchcore/proton/matching/match_phase_limiter.*`
- `searchcore/src/vespa/searchcore/proton/matching/attribute_limiter.*`

## 参考文档

- `searchlib/doc/sort-implementation.md` — Vespa 排序完整流程（query → content node → protobuf → container 归并）
- `searchlib/doc/sort-benchmark/README.md` — harness 构建/运行说明
- `searchlib/doc/sort-benchmark/findings.md` — 基准测试原始数据 + 结论分析
- `searchlib/doc/sort-benchmark/baseline.txt` — benchmark raw 输出

## 我（Claude）没做的事

- 没有在真实 Vespa 构建链上编译过一次 —— **这是 reviewer 要验证的第一件事**
- 没有跑过 `multilevelsort_test` / `sortresults_test` 的 unit test
- 没有调过 heuristic 常数 —— 目前是基于 harness 数据的初始猜测
- 没有加 metrics / logging 来观察生产上 lazy 的 hit rate
- 没有动 container 端的任何代码 —— 合并逻辑保持原样
- 没有做方向 2 / 方向 4

这些都在"下一步"里了。
