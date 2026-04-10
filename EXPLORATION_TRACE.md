# Multi-Embedding HNSW 优化完整探索记录

## 背景

Vespa 中多 embedding 召回场景（MIND 10 兴趣向量，OnePiece 6 互补向量），每次查询需要 N 次独立 HNSW 搜索，线性放大延迟。目标：降低 N 次搜索的总开销。

---

## 第一阶段：Batch HNSW — 充满信心的起点

### 方案
共享 visited set + 统一候选队列，每个节点一次 load、N 次距离计算。

### AI 的初始分析（过于乐观）
- 成本模型：vector load = 500ns，distance calc = 50ns
- 预测加速：2.2x - 4.7x
- 逻辑："共享 vector load 是巨大的节省"

### 实现（C++）
完整实现了 `search_layer_batch_helper`，包含：
- 共享 visited set
- 统一 `BatchNearestPriQ`（按 min_distance 排序）
- 每个 query 独立的 top-k 结果追踪
- filter 只检查一次（不是 N 次）
- Blueprint 层自动检测同 field 的多个 NN query，合并为 batch

### Go Benchmark 结果（100K docs, 64D, 10 interests）

| 兴趣分布 | DC 减少 | 预测加速 | Recall |
|---|---|---|---|
| 集中（2 clusters） | 3.6× | ~3.6× | 0.996 |
| 混合（3+3+4） | 1.5× | ~1.5× | 0.997 |
| 分散（10 clusters） | 1.0× | ~1.0× | 0.997 |

**看起来不错？但是...**

### 现实打脸：C++ benchmark 显示 batch 更慢

用户反馈：**本地 C++ bench 显示 batch 反而变慢了。**

根因分析 — `get_vector()` 的真实成本：
```cpp
// hnsw_index.h — 这就是一个 inline 指针解引用，~0ns
TypedCells get_vector(uint32_t docid) const {
    return _vectors.get_vector(docid, 0);  // 指针运算，无 I/O
}
```

**Vector load ≈ 0ns。整个 batch 方案的核心前提是错的。**

修正后的成本模型：距离计算本身包含内存访问（3-50ns），没有单独的 "load" 开销。Batch 的额外 N 次距离计算/节点成为净开销，不是节省。

**教训**：AI 能构建精致的、内部自洽的分析，但建立在未验证的假设上。成本模型有表格、百分比、多场景 — 看起来很严谨，实际上最关键的参数就是错的。

---

## 第二阶段：绝望的探索 — 尝试所有能想到的方案

### 尝试 1：三角不等式剪枝

**想法**：如果 query A 和 B 相似，用 A 到节点的距离推导 B 的下界，跳过不必要的计算。

```
ipDist(q_b, doc) >= (√ipDist(anchor, doc) - √ipDist(anchor, q_b))²
```

**结果**：64 维下零有效剪枝。

**为什么失败**：高维空间中归一化向量近乎正交，三角不等式的下界极其松弛（bound=0.02，实际距离=0.8）。维度诅咒使所有成对距离集中在相似值附近。

### 尝试 2：投机解码（Speculative Decoding 类比）

**想法**：借鉴 LLM 投机解码 — draft（小 ef 快速搜索）→ verify（用 draft 结果做种子）。

**结果**：在 500K/128D 多层 HNSW 上验证，seed 永远被拒绝。

**为什么失败**：HNSW 多层下降是 per-query 的。每个 query 从顶层 greedy descent 收敛到不同的图区域。Query A 的结果作为 Query B 的种子时，全量下降总是找到更好的入口点。图结构使得跨 query 共享入口点本质上不可能。

Benchmark 数据（500K docs, 128D, 4 层 HNSW）：
```
SearchSingle:       DescentDC=212, BeamDC=27145, Recall=0.876
SearchSpeculative:  DescentDC=212, BeamDC=27145, Recall=0.876  (完全相同！)
SearchWarmChain:    DescentDC=212, BeamDC=27145, Recall=0.876  (完全相同！)
```

Seed 100% 被拒绝。加速为 0%。

### 尝试 3：Warm-Start 链式搜索

**想法**：按相似度排序 N 个 query，每个 query 用前一个最相似 query 的结果做种子。

**结果**：同上 — seeds always rejected。

**为什么失败**：同投机解码。即使两个 query 向量很相似（dot > 0.8），它们在 HNSW 图中的最优入口点仍然不同。多层下降的成本很低（~200 DC），但它找到的入口点几乎总是比任何种子更好。

### 尝试 4：Per-Query 独立探索队列 + 共享 Visited Set

**结果**：灾难性失败。Union recall 跌至 0.02-0.46。

**为什么失败**：共享 visited set 阻断了其他 query 的探索路径。Query A 探索区域 X 标记了 v1, v2, v3 为已访问。Query B 需要穿过 v1, v2, v3 到达目标区域 Y，但它们已被标记 — Query B 被困住了。

**结论**：共享 visited set **必须**配合统一探索队列。独立队列 + 共享 visited set 是互斥的设计选择。

### 尝试 5：Per-Query Early Termination

**结果**：分散兴趣的 recall 被摧毁。

**为什么失败**：HNSW 中 "连续 N 步无改善" ≠ "不可能改善"。图连通性意味着好结果可能在长平台后出现。

### 阶段小结

所有算法层面的多 query 协同优化都失败了。根本原因：

> **HNSW 图结构是 per-query 的。不同 query 在图中的最优路径不同，无法有效共享。**

在 vector load ≈ 0 的前提下，batch 的唯一优势（减少唯一节点访问数）被额外的 N 次距离计算/节点抵消。

---

## 第三阶段：转向 — 降低单次距离计算的成本

### 思路转变

既然不能减少距离计算的次数，能不能让每次计算更便宜？

> **"投机召回"：用低精度计算做图遍历，高精度做最终排序**

### Int8 量化方案

```
量化：float32_val → round(val × 127) → int8_val
距离：dot_int8 / (127×127) ≈ dot_float32
硬件加速：AVX512 VNNI 一条指令处理 4× int8 运算
```

关键洞察：HNSW 图遍历只需要**相对排序**正确，不需要精确距离值。Int8 量化保留了 94-96% 的 rank correlation。

### Benchmark 结果（500K docs, 128D, 多层 HNSW）

**Phase 0 — 暴力搜索 rank correlation 验证：**
- Int8 vs Float32 brute-force top-200 overlap: 94-96%
- 量化误差不影响最终排序质量

**Phase 1 — HNSW 搜索对比：**

| 方法 | F32 DCs | I8 DCs | Recall |
|---|---|---|---|
| Float32 baseline | ~27,000 | 0 | 0.876 |
| Int8 + Rerank (ef) | ~200 | ~28,000 | 0.870 |
| Int8 + Rerank (2×ef) | ~400 | ~55,000 | 0.880 |

**有效加速计算**（VNNI int8 距离 ≈ float32 的 1/4 成本）：
```
Float32: 27,000 × 1.0 = 27,000 等效单位
Int8+Rerank: 28,000 × 0.25 + 200 × 1.0 = 7,200 等效单位
加速比: 27,000 / 7,200 ≈ 3.75×
```

**~3-4x 有效加速，recall 几乎无损。**

### 降维的追加讨论

考虑过在 int8 基础上叠加 PCA 降维（128D → 64D），结论是 **128D 不值得降维**：
- 128D 已经不算高维，每个维度都携带有效信号
- PCA 砍一半会损失 5-10% recall，换来的额外 2x 加速不划算
- Int8 128D 已经给了 ~4x 加速，边际收益递减
- 降维更适合 768D/1024D 的大模型 embedding

---

## 第四阶段：Vespa 实现路径

### 发现：Vespa 原生支持所有需要的组件

深入分析 Vespa 源码后发现：

1. **`tensor<int8>` + HNSW 索引**：原生支持
2. **Int8 硬件加速距离计算**：`SquaredEuclideanDistanceHW<Int8Float>` 通过 `reinterpret_cast<int8_t*>` 直接调用 AVX512/AVX2 加速
3. **两阶段排序**：first-phase + second-phase + rerank-count
4. **实时流式写入**：Document Processor 可在写入时量化

### 关键实现细节

**必须用 `distance-metric: euclidean`，不能用 `innerproduct`**：

```cpp
// euclidean_distance.h — Int8 有 HW 加速
static const int8_t *cast(const Int8Float * p) {
    return reinterpret_cast<const int8_t *>(p);  // → AVX512 加速
}

// distance_function_factory.cpp — InnerProduct 的 Int8 没有 HW 加速
case DistanceMetric::InnerProduct:
    default: return InnerProductDistance(CellType::FLOAT);  // 回退到 float 转换！
```

对归一化向量，euclidean 和 inner product 排序等价（`||a-b||² = 2 - 2<a,b>`）。

（注：用户的 fork 版本已实现了 int8 inner product HW 加速 + shadow write，不受此限制）

### 实现架构

```
写入路径:
  float32 embedding → Document Processor → round(val × 127) → int8
  同时存储 float32（rerank 用）和 int8（HNSW 遍历用）

查询路径:
  nearestNeighbor(int8_field, q_i8) → HNSW 遍历（int8 HW 加速）
  → first-phase: int8 closeness
  → second-phase: float32 精确排序 (rerank-count: 200)
```

---

## 完整时间线

```
Step 1  [AI 提案]     Batch HNSW — 共享 visited set，预测 2-4x 加速
Step 2  [AI 实现]     完整 C++ 实现 + Go benchmark，结果看起来不错
Step 3  [人类发现]    "C++ 里 get_vector 就是指针解引用，成本约 0"
Step 4  [认知崩塌]    成本模型的核心假设是错的，batch 实际更慢
Step 5  [AI 尝试]     三角不等式剪枝 → 64D+ 下界太松，零剪枝
Step 6  [AI 尝试]     投机解码类比 → 种子永远被拒绝
Step 7  [AI 尝试]     Warm-start 链式搜索 → 同上
Step 8  [AI 验证]     500K/128D 多层 HNSW benchmark 确认以上全部失败
Step 9  [人类提问]    "投机召回，低精度计算 + rerank，有戏么？"
Step 10 [AI 实现]     Int8 量化 benchmark → 3-4x 有效加速！
Step 11 [AI 分析]     Vespa 源码分析 → 原生支持 int8 HNSW + 两阶段排序
Step 12 [人类补充]    "我们 fork 已有 int8 IP 加速 + shadow write"
Step 13 [讨论]        降维 128D → 64D？结论：不值得，int8 已够用
```

---

## 关键教训

### 1. AI 放大了达克效应
AI 生成的分析看起来权威（表格、公式、多场景），但核心假设未经验证。成本模型精致且自洽 — 但 vector load = 500ns 这个数字是错的。

### 2. 负面结果有价值
5+ 种方案失败，每次失败都缩小了搜索空间：
- Batch 失败 → 问题是单次距离计算成本，不是访问开销
- 三角不等式失败 → 高维不能剪枝
- 投机/Warm-start 失败 → HNSW 结构不能跨 query 共享
- 这些排除直接导向 "让单次计算更便宜" → int8

### 3. AI + 人类 > 任何一方
- **AI 优势**：快速原型和 benchmark（Go benchmark 分钟级完成）
- **AI 劣势**：初始分析过于乐观，不质疑假设
- **人类优势**："C++ 里 get_vector 就是指针解引用" — 一句话推翻整个方案
- **人类优势**："投机召回，低精度 + rerank" — 指出正确方向

### 4. Benchmark > 推理
每个 "应该有效" 的方案在 benchmark 面前都失败了。只有 int8 量化存活。

### 5. 解决方案与原始问题正交
我们花了大量时间优化 N-query 协同（batch, sharing, pruning）。最终解决方案是优化单次查询成本（int8 量化）。有时最好的优化垂直于你的视角。

### 6. 快速失败的价值
每次失败只花几分钟（AI 写 benchmark + 运行），但学到的知识是真实的。传统方式可能每个方案要花一周，最终学到同样的结论。AI 让探索的成本极低，使得 "大量快速失败" 成为可行的策略。

---

## 文件清单

| 文件 | 说明 |
|---|---|
| `benchmark_batch_hnsw.go` | MIND batch benchmark（100K, 64D, 修正成本模型） |
| `benchmark_onepiece_hnsw.go` | OnePiece 互补 embedding benchmark（500K, 64D） |
| `benchmark_scale_hnsw.go` | 500K/128D 多层 HNSW — 验证投机/warm-start 失败 |
| `benchmark_int8_hnsw.go` | Int8 量化 benchmark — 确认 3x 加速 |
| `int8_speculative_recall_design.md` | Vespa 实现设计文档 |
| `HNSW_BATCH_SEARCH_README.md` | 原 PR#6 的详细技术文档（含失败尝试记录） |
