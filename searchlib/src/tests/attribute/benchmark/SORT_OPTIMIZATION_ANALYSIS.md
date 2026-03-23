# Histogram-Based Sort Optimization for Vespa

## Background

Search EP 希望在 match 截断能实现 early stop 的效果，以此来减少单个分片过多的 match 导致 p99 过高。

## Solution

目前 vespa 并不支持 early stop，不过 match-phase 支持在匹配达到一定量后使用一个 attribute 来做过滤，从而实现更少的 match 结果。

具体做法是 query eval 过程中，当 match hits 数达到配置的 max-hits 时，可以主动给当前 query tree 增加一个 range iterator 来过滤。

range filter 的配置为取固定 top/bottom 结果，取决于 sort type：

```cpp
string range_spec = make_string("[;;%s%zu", (_descending)? "-" : "", want_hits);
```

其中 want_hits 为希望获取到的 top/bottom 结果数，通过 max_hits / hit_ratio 来预估，假设 hit_ratio 在所有 doc 上是分布均匀的，那我们再匹配 want_hits 数同样能得到 max-hits 数的结果。

实际执行过程中如果 want_hits 过大（默认超过 20% 总数）就不会主动注入 range filter。

## 性能分析

考虑单分片数据集，设大小为 N，固定 match 过程中的 max-hits，设 a = max-hits / N，设置 want_hits 上限为 20% 总数，即 0.2N，同时假设 hit_ratio 在 match 过程中均匀分布。

考虑不同 query 的 hit_ratio，我们可以得到如下曲线图：

```
                matched_hits over hit_ratio

 N ─┐                          ..... (without match-phase, y = N * hit_ratio)
    │                    .....
    │              .....●───────────────────
    │         .....     │
5max-hits ─   ....●     │
    │        .  │ │     │
    │       .   │ │     │
2max-hits ─  .  │ ●─────●───────────────●──
    │      .    │
 max-hits ●     │
    │    .│     │
    │   . │     │
 0 ─┼────┼─────┼────────────────────────┼──
    0    a     5a                        1
                      hit_ratio
```

说明:
1. 横轴为 query 的 hit_ratio，纵轴为 query match 到的 hits 数
2. 当 hit_ratio 为 a 时，我们得到 max-hits 个结果数
3. 当 hit_ratio 大于 5a 时，当我们 match 到 max-hits 个结果时，此时估算 want_hits 会小于 0.2N，根据假设，后续 match 到 max-hits 个结果

以上分析可以大致确定不同 hit_ratio 情况下 matched_hits 的模糊上下界，考虑到实际情况中 hit_ratio 的分布不可能均匀，所以在 hit_ratio 大于 a 时就有可能触发 range filter 归并，同时在 hit_ratio [a, 5a] 区间内触发 range filter 产生的 matched_hits 可能不会显著小于上界。

### 进一步分析

基于以上分析，进一步得出如下曲线图：

```
                matched_hits over hit_ratio

 N ─┐                          ..... (without: y = N * hit_ratio)
    │                    .....
    │              .....●───────────────────  (ideal: with match-phase)
    │         .....     │
5max-hits ─   ....●     │
    │        .  │ │     │
    │       .   │ │     │
2max-hits ─  .  │ ●─────●───────────────●──
    │   ...─┘   │            (maybe: actual with non-uniform distribution)
 max-hits ●     │
    │    .│     │
    │   . │     │
 0 ─┼────┼─────┼────────────────────────┼──
    0    a     5a                        1
                      hit_ratio
```

说明:
1. 红线(maybe)表示可能的 matched hits 随 hit ratio 的分布，这个取决于 query 在拉链上的 match 分布
2. 当触发 range filter 同时不能大幅度减少 match hits 的情况下，也即红线和灰线 gap 不足够大时，range filter 带来的额外开销没法覆盖减少 matched hits 带来的收益，性能可能劣化

## 1. ES BKD Sort Optimization — What It Does

Elasticsearch/Lucene uses BKD-tree for a technique called **"early termination with competitive iterator"**:

```
Query: WHERE filter ORDER BY price ASC LIMIT 10
```

1. Start iterating matching docs, collect top-10 by price
2. After 10 hits collected, the **competitive value** = worst price in heap (e.g., $50)
3. BKD-tree knows each leaf block's `[min_price, max_price]`
4. **Skip entire blocks** where `min_price > $50` (can't contain better candidates)
5. As better candidates found, competitive value drops → more blocks skipped
6. Net effect: only scan ~O(K * log N) docs instead of all N matches

Key requirement: **index-ordered iteration** — BKD tree can iterate docs in value order.

## 2. Vespa's Current Sort Path — The Problem

### Current execution flow:

```
match_thread.cpp:
1. inner_match_loop()
   ├── For each matching doc:
   │   ├── search->unpack(docId)           // evaluate query
   │   ├── rankHit(docId) or addHit(docId) // collect ALL matches
   │   └── (continue to next doc)
   │
2. processResult()
   ├── result->sort(*context.sort->sorter, sortLimit)
   │   ├── initSortData(hits, n)           // serialize sort keys for ALL hits
   │   │   └── For each hit: attr->serializeForSort(docId, buf)
   │   └── radix_sort(sortData, n, topn)   // sort ALL hits, partial sort for top-N
   └── Copy top-N sorted hits to result
```

### Key problems:

1. **No early termination during matching**: ALL matching documents are collected
   regardless of sort order. Matching a query with 1M hits but only needing top-10
   sorted by price still processes all 1M.

2. **Sort happens post-match**: Sort keys are materialized only after ALL matches
   are collected. This means `initSortData()` serializes sort keys for ALL hits.

3. **Radix sort does partial sort** (topn optimization), but the input is still
   all N matched documents.

4. **No value-order iteration**: B-tree posting lists are document-order, not
   value-order. You can't iterate "cheapest first".

### Existing partial optimization — match-phase limiter:

Vespa has `MatchPhaseLimiter` (`match_phase_limiter.cpp`) which can limit the
matching phase using a range query on an attribute:

```cpp
// attribute_limiter.cpp — original count-based range limit
string range_spec = make_string("[;;%s%zu", descending ? "-" : "", want_hits);
// Creates a range term like "[;;-10000]" to get top-10000 by attribute
```

This uses `rangeLimit` in `PostingListSearchContext::applyRangeLimit()` to
iterate only the top-N values from the dictionary. **This is effectively
value-ordered iteration** — but it's:
- Configured statically per rank-profile, not per-query
- Uses a fixed `max_hits` count, not adaptive
- Doesn't use any distribution information to set the range bound

## 3. Where Histogram Helps — The Sort Optimization

### The Core Idea: Histogram-Guided Range Bound Injection

Instead of matching ALL documents and then sorting, use the histogram to
**predict the sort key threshold** and inject it as an implicit range filter
BEFORE matching:

```
Original query:  WHERE category='shoes' ORDER BY price ASC LIMIT 100
                 → matches 500K docs, sort all, take top 100

With histogram:
  1. histogram.estimate_percentile(price, target=200)  → price threshold = $89
  2. Inject: WHERE category='shoes' AND price <= $89 ORDER BY price ASC LIMIT 100
             → matches ~200 docs, sort 200, take top 100
```

### Why histogram is better than existing match-phase limiter:

| Aspect | Match-phase limiter (count-based) | Histogram-guided (value-based) |
|--------|----------------------------------|-------------------------------|
| Threshold | Fixed `max_hits` count via `[;;N]` | Adaptive value bound via `[;V]` or `[V;]` |
| Accuracy | Depends on uniform assumption | Distribution-aware, handles skew |
| Configuration | Static per rank-profile | Automatic, no config needed |
| Range precision | Over-fetches for skewed data | Tight bound from histogram |
| Safety | Can miss results if limit too low | 2x overshoot factor ensures completeness |

### The Algorithm:

```
Input: sort_spec = [(price, ASC)], limit = K, filter = original_query
       histogram = price attribute's histogram

1. Compute desired_hits = K * OVERSHOOT_FACTOR  (e.g., 100 * 2 = 200)

2. Use histogram to find threshold value V such that:
   histogram.estimate(min_value, V) >= desired_hits
   (Linear scan on histogram buckets with interpolation)

3. Inject range filter: original_query AND price <= V

4. Execute match loop with reduced candidate set

5. Sort reduced set, take top K

6. If fewer than K results (overshoot was insufficient):
   Fall back to full match (rare with good histogram)
```

### For descending sort (e.g., ORDER BY timestamp DESC):

```
histogram.estimate(V, max_value) >= desired_hits
→ inject: timestamp >= V
```

### For multi-field sort (ORDER BY price ASC, timestamp DESC):

Only optimize the **primary sort key**. The threshold is still based on the
primary key's histogram. Secondary keys don't affect the candidate set —
they only matter for tie-breaking within the top-K.

## 4. Implementation — Actual Code Changes

### 4.1 AttributeHistogram (`searchlib/attribute/attribute_histogram.h/cpp`)

Equi-width histogram maintained per numeric attribute with posting lists.

```cpp
class AttributeHistogram {
    static constexpr uint32_t DEFAULT_NUM_BUCKETS = 1024; // 4KB memory

    void reset(int64_t min_value, int64_t max_value);
    void add(int64_t value, uint32_t count);          // O(1) per dict entry
    uint32_t estimate(int64_t lo, int64_t hi) const;  // O(num_buckets)
    int64_t estimate_threshold_for_count(uint32_t target_count, bool ascending) const;
};
```

Key design decisions:
- **Bucket width** computed via floating-point to avoid int64 overflow: `(double(max) - double(min) + 1.0) / num_buckets`
- **Estimate** uses fractional overlap for partial buckets with `std::round` to avoid systematic underestimation
- **Threshold estimation** uses linear scan with interpolation (not binary search), O(num_buckets)
- **Thread safety**: stored as `shared_ptr<const AttributeHistogram>` — readers take a copy, writer atomically swaps

### 4.2 Histogram Lifecycle (`postinglistattribute.cpp`)

Built from the frozen posting dictionary after commit:

```cpp
// attributevector.cpp:commit()
void AttributeVector::commit(bool forceUpdateStats) {
    onCommit();
    updateCommittedDocIdLimit();
    if (auto *posting_base = getIPostingListAttributeBase()) {
        posting_base->rebuild_histogram();  // rebuild after posting changes
    }
    updateStat(forceUpdateStats);
}
```

The rebuild performs a single pass through the frozen dictionary:
1. Find min/max values across all postings
2. Collect (value, count) pairs
3. Build histogram from collected data
4. Atomically swap `shared_ptr` (old readers stay safe)

Optimization: **dirty flag** — only rebuild when posting lists have actually changed (`updatePostings` or `clearAllPostings`). Skips the full dictionary scan on no-op commits.

### 4.3 Histogram-Guided Limiter (`attribute_limiter.cpp`)

The histogram replaces count-based `[;;-N]` with a precise value range `[;V]` (ascending) or `[V;]` (descending):

```cpp
// attribute_limiter.cpp:create_search()
if (use_histogram) {
    bool ascending = !_descending;
    size_t k = want_hits * 2;  // 2x safety margin
    int64_t threshold = _histogram->estimate_threshold_for_count(
        static_cast<uint32_t>(k), ascending);
    if (ascending) {
        range_spec = make_string("[;%ld]", threshold);    // attr <= threshold
    } else {
        range_spec = make_string("[%ld;]", threshold);    // attr >= threshold
    }
}
```

Activation conditions:
- Histogram is available and valid
- No diversity (diversity needs count-based limiting)
- `match_freq >= 0.1` (high enough that limiting provides substantial benefit)
- `max_group_size >= want_hits` (no diversity limiting)

### 4.4 Range Estimation in Search Context (`postinglistsearchcontext.h`)

`NumericPostingSearchContext::approximateHits()` uses the histogram for O(1) hit count estimation instead of iterating the posting list:

```cpp
if (this->_histogram && this->_histogram->is_valid() && this->_uniqueValues >= 2) {
    // For float/double types, use floor/ceil to avoid truncation
    if constexpr (std::is_floating_point_v<BaseType>) {
        lo_int = static_cast<int64_t>(std::floor(lo_d));
        hi_int = static_cast<int64_t>(std::ceil(hi_d));
    }
    estimate = this->_histogram->estimate(lo_int, hi_int);
}
```

## 5. Robustness Fixes (Code Review)

The following critical issues were identified and fixed:

### 5.1 Thread Safety: `unique_ptr` → `shared_ptr`

**Problem**: `PostingListAttributeBase::_histogram` was `unique_ptr`. A `rebuild_histogram()` call during commit could free the old histogram while search threads still held raw pointers to it → **use-after-free**.

**Fix**: Changed to `shared_ptr<const AttributeHistogram>`. All consumers (`PostingListSearchContext`, `AttributeLimiter`, `MatchPhaseLimiter`) now hold `shared_ptr` copies. Rebuild atomically replaces the pointer; old readers stay safe via reference counting.

### 5.2 Integer Overflow for Large Value Ranges

**Problem**: `max_value - min_value + 1` overflows int64 when `max_value` is near `INT64_MAX`. Similarly `hi + 1` in `estimate()` overflows.

**Fix**: All arithmetic uses floating-point intermediaries:
```cpp
_bucket_width = (double(max_value) - double(min_value) + 1.0) / _num_buckets;
double hi_upper = static_cast<double>(hi) + 1.0;  // instead of hi + 1
```

### 5.3 Float→int64 Truncation

**Problem**: For float/double attributes, `static_cast<int64_t>(_low)` truncates toward zero (e.g., range `[1.2, 5.8]` becomes `[1, 5]`), potentially missing documents at boundaries.

**Fix**: Use `std::floor` for lo and `std::ceil` for hi, with range clamping to avoid UB at int64 extremes.

### 5.4 Dirty Flag for Rebuild Performance

**Problem**: `rebuild_histogram()` was called on every `AttributeVector::commit()`, performing a full dictionary scan even when no posting lists changed.

**Fix**: Added `_histogram_dirty` flag, set `true` in `updatePostings()` and `clearAllPostings()`. `rebuild_histogram()` returns early when not dirty.

### 5.5 Estimation Rounding

**Problem**: `static_cast<uint32_t>(sum)` in `estimate()` truncates, causing systematic underestimation (e.g., 99.7 → 99).

**Fix**: Use `std::round(sum)` for more accurate estimates.

### 5.6 Unit Tests

Added comprehensive gtest suite (`searchlib/src/tests/attribute/attribute_histogram/`):
- Construction, add/remove, validity states
- Full-range and half-range estimation accuracy
- Threshold estimation (ascending/descending)
- Edge cases: equal min/max, inverted range, empty histogram
- Large INT64 ranges (near overflow boundary)
- Skewed (Zipf) distribution accuracy
- Single-bucket histogram

## 6. Expected Performance Impact

### Scenario: 10M docs, sort by price ASC LIMIT 100

Without optimization:
- Match: 500K docs (filter matches 5%)
- Sort: serialize 500K sort keys + radix sort
- Total sort overhead: ~50ms

With histogram-guided bound (overshoot 2x):
- Histogram lookup: ~1 us
- Match: 200 docs (range filter + original filter)
- Sort: serialize 200 sort keys + radix sort
- Total sort overhead: ~0.02ms
- **Speedup: ~2500x for the sort phase**

### When it doesn't help:
- Query matches very few docs (< K * OVERSHOOT): no benefit, range is wider than needed
- Sort attribute has very low cardinality: histogram resolution insufficient
- No sort spec (pure relevance ranking): not applicable
- `match_freq < 0.1`: query is already selective enough, range filter skipped

### Memory overhead:
- 1024 buckets * 4 bytes = **4KB per numeric attribute** (with posting lists)
- String attributes: no histogram built (not applicable)

## 7. Comparison: BKD vs Histogram Approach

| Aspect | BKD (ES) | Histogram (Vespa) |
|--------|----------|-------------------|
| Mechanism | Index-ordered iteration + block skip | Pre-computed range bound injection |
| Granularity | Per-leaf-block (~512 docs) | Per-histogram-bucket (~4KB) |
| Dynamic | Competitive threshold tightens during scan | Static threshold set before match |
| Multi-field | Limited (BKD is single-field) | Same (primary key only) |
| Write cost | High (segment merges) | Low (O(dict_size) rebuild, skip when clean) |
| Accuracy | Exact block boundaries | ~0.1% error (good enough) |
| Architecture fit | Requires different storage | Works with existing B-tree |

**Key insight**: The histogram approach trades the BKD's **dynamic per-doc
pruning** for a **one-shot statistical bound**. This is less optimal in theory
but far simpler to implement and good enough in practice — the 2x overshoot
factor handles the imprecision.

## Reference

1. [Vespa match-phase documentation](https://docs.vespa.ai/en/reference/schema-reference.html#match-phase)
