# Histogram-Based Sort Optimization for Vespa

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
// attribute_limiter.cpp:79
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

| Aspect | Match-phase limiter | Histogram-guided |
|--------|-------------------|------------------|
| Threshold | Fixed `max_hits` count | Adaptive per-query value bound |
| Accuracy | Depends on uniform assumption | Distribution-aware |
| Configuration | Static per rank-profile | Automatic, no config needed |
| Multi-field | Single attribute | Can handle primary + secondary sort |
| Safety | Can miss results if limit too low | Overshoot factor ensures completeness |

### The Algorithm:

```
Input: sort_spec = [(price, ASC)], limit = K, filter = original_query
       histogram = price attribute's histogram

1. Compute desired_hits = K * OVERSHOOT_FACTOR  (e.g., 100 * 3 = 300)

2. Use histogram to find threshold value V such that:
   histogram.estimate(min_value, V) >= desired_hits
   (Binary search on histogram buckets)

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

## 4. Integration Points in Vespa Code

### Option A: Enhance MatchPhaseLimiter (Recommended)

**Modify:** `match_phase_limiter.cpp` and `attribute_limiter.cpp`

Currently, `AttributeLimiter::create_search()` uses:
```cpp
string range_spec = make_string("[;;%s%zu", descending ? "-" : "", want_hits);
```

This tells the posting list to iterate the top `want_hits` values from the
dictionary. **The histogram can make this smarter**:

```cpp
// New: use histogram to compute a tighter range bound
if (auto* hist = attr->getIPostingListAttributeBase()->get_histogram()) {
    int64_t threshold = hist->estimate_threshold_for_count(desired_hits, ascending);
    // Use a value-based range instead of count-based range
    range_spec = make_string("[;%ld;%s", threshold, ascending ? "" : "-");
}
```

**Advantage**: Reuses existing infrastructure. The `LimitedSearch` AND-ing
with the main query already works. We just make the range tighter.

### Option B: New SortBoundOptimizer (Clean but more work)

Create a new component that sits between blueprint creation and matching:

```
match_tools_factory.cpp → create SortBoundOptimizer
  → Reads sort spec + histogram
  → Computes value bound for primary sort key
  → Injects range filter into blueprint tree
  → match_thread runs with reduced search space
```

### Option C: Dynamic Threshold in Match Loop (Most powerful)

Analogous to ES's competitive iterator, but using the histogram:

During `inner_match_loop()`, maintain a heap sorted by the sort key:

```cpp
// In Context::rankHit() or addHit():
int64_t sort_value = sort_attr->getInt(docId);
if (heap_full && sort_value > competitive_threshold) {
    skip;  // This doc can't be in top-K
}
```

This requires fetching the sort attribute value during matching, which adds
per-document overhead but enables dynamic pruning. The histogram helps set
the initial competitive threshold before the heap fills up.

## 5. Histogram API Addition Needed

Add to `AttributeHistogram`:

```cpp
/**
 * Find the value V such that estimate(min_value, V) >= target_count.
 * Used for sort optimization: given top-K sort, find the value bound.
 *
 * @param target_count  desired number of hits
 * @param ascending     true for ASC sort (find upper bound),
 *                      false for DESC sort (find lower bound)
 * @return the threshold value, or max/min if cannot satisfy
 */
int64_t estimate_threshold_for_count(uint32_t target_count, bool ascending) const;
```

Implementation: binary search over histogram buckets, accumulating counts
until target is reached. O(log(num_buckets)).

## 6. Expected Performance Impact

### Scenario: 10M docs, sort by price ASC LIMIT 100

Without optimization:
- Match: 500K docs (filter matches 5%)
- Sort: serialize 500K sort keys + radix sort
- Total sort overhead: ~50ms

With histogram-guided bound (overshoot 3x):
- Histogram lookup: ~1 μs
- Match: 300 docs (range filter + original filter)
- Sort: serialize 300 sort keys + radix sort
- Total sort overhead: ~0.03ms
- **Speedup: ~1600x for the sort phase**

### When it doesn't help:
- Query matches very few docs (< K * OVERSHOOT): no benefit, range is wider
- Sort attribute has very low cardinality: histogram resolution insufficient
- No sort spec (pure relevance ranking): not applicable

## 7. Comparison: BKD vs Histogram Approach

| Aspect | BKD (ES) | Histogram (Vespa) |
|--------|----------|-------------------|
| Mechanism | Index-ordered iteration + block skip | Pre-computed range bound injection |
| Granularity | Per-leaf-block (~512 docs) | Per-histogram-bucket (~4KB) |
| Dynamic | Competitive threshold tightens during scan | Static threshold set before match |
| Multi-field | Limited (BKD is single-field) | Same (primary key only) |
| Write cost | High (segment merges) | Low (O(dict_size) rebuild) |
| Accuracy | Exact block boundaries | ~0.1% error (good enough) |
| Architecture fit | Requires different storage | Works with existing B-tree |

**Key insight**: The histogram approach trades the BKD's **dynamic per-doc
pruning** for a **one-shot statistical bound**. This is less optimal in theory
but far simpler to implement and good enough in practice — the overshoot
factor handles the imprecision.
