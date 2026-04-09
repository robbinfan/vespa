# HNSW Multi-Embedding Batch Search Optimization

Optimized HNSW index search for scenarios where multiple query embeddings target
the same tensor field. Two specific use cases:

- **MIND** (Multi-Interest Network with Dynamic Routing): User has ~10 interest
  embeddings. Each represents a different purchasing interest (e.g., shoes, electronics,
  books). Query retrieves candidates per interest, then OR-merges and re-ranks.
- **OnePiece**: User has 6 **complementary** embeddings, each capturing a different
  aspect of search intent (brand affinity, style, price sensitivity, etc.).
  Not progressive refinements — each embedding retrieves largely disjoint documents.

## Problem

Vespa's current query evaluation model treats each `nearestNeighbor` operator
independently. When a MIND model produces 10 interest embeddings, the query plan
becomes:

```
OR(
  nearestNeighbor(embedding, interest_1, k=20, ef=200),
  nearestNeighbor(embedding, interest_2, k=20, ef=200),
  ...
  nearestNeighbor(embedding, interest_10, k=20, ef=200)
)
```

Each runs a full HNSW traversal **serially**:
- 10 × independent entry point descent
- 10 × independent visited set allocation
- 10 × independent distance computations for overlapping graph regions
- Same nodes visited multiple times across queries (redundant work)

For OnePiece with 6 complementary embeddings, the measured cost is **8× single-query
CPU** (not 6× — the extra overhead comes from cache thrashing between independent
traversals).

### Filter Amplification

With `ann + filter` queries, the problem compounds:
- Each independent search evaluates the filter bitvector per candidate node
- The same document's filter bit is checked 6-10 times across queries
- Filter evaluation overhead scales linearly with number of embeddings
- In restrictive filter scenarios, each search does more exploration to find
  enough passing documents, further multiplying the cost

## Solution: Three Strategies

All strategies share the same core mechanism — `HnswIndex::search_layer_batch` —
which uses a **shared visited set** and **batch distance computation** (load document
vector once, compute N distances).

### Strategy 1: Batch Search (MIND — Adaptive)

**For**: MIND multi-interest retrieval where interests may be concentrated or spread.

```
Adaptive Batch:
  1. Cluster the N query embeddings by similarity (dot product > threshold)
  2. Groups of nearby interests → batch search (shared visited set)
  3. Isolated interests → independent single search
  4. Union results across all groups
```

**Key insight**: Nearby interests explore overlapping graph regions. Sharing the
visited set avoids redundant distance computations. Distant interests explore
disjoint regions where batching adds overhead without saving work.

**Benchmark results** (100K docs, 64-dim, 50 clusters, 10 interests, averaged over 5 users):

| Interest Distribution    | Strategy       | DC Reduction | Projected Speedup | Recall |
|--------------------------|----------------|-------------|-------------------|--------|
| Concentrated (2 clusters)| Adaptive Batch | **3.6×**    | ~3.6×             | 0.996  |
| Mixed (3+3+4)            | Adaptive Batch | **1.5×**    | ~1.5×             | 0.997  |
| Spread (10 clusters)     | Adaptive Batch | **1.0×**    | ~1.0×             | 0.997  |

Speedup ≈ DC (distance calc) reduction ratio. Vector load is ~0 in C++ (inline
pointer dereference).

Adaptive batch **never degrades** below independent baseline — it automatically
falls back to independent search when interests are spread.

### Strategy 2: Adaptive Batch (OnePiece — Complementary Embeddings)

**For**: OnePiece multi-embedding retrieval where each embedding captures a
**different aspect** of the user's intent (complementary, not progressive).

**Key discovery**: OnePiece embeddings are complementary, not progressive refinements.
Each embedding captures a distinct aspect (brand affinity, style, price sensitivity,
etc.). Ground-truth overlap between embeddings is near 0%, meaning each embedding
retrieves almost entirely different documents.

```
Adaptive Batch (same as MIND):
  1. Cluster the 6 query embeddings by similarity (dot product > 0.3)
  2. Similar embeddings → batch search (shared visited set)
  3. Dissimilar embeddings → independent single search
  4. Union results across all groups
```

**Benchmark results** (500K docs, 64-dim, 50 clusters, 6 complementary embeddings, 5 users):

| Embedding Distribution    | Strategy       | DC Reduction | Projected Speedup | Recall |
|---------------------------|----------------|-------------|-------------------|--------|
| Concentrated (2 aspects)  | Adaptive Batch | **1.3×**    | ~1.3×             | 0.990  |
| Mixed (3 aspects)         | Adaptive Batch | **1.2×**    | ~1.2×             | 0.995  |
| Spread (6 aspects)        | Adaptive Batch | **1.0×**    | ~1.0×             | 0.950  |

Speedup ≈ DC reduction ratio. Vector load is ~0 in C++.

**Why Progressive Retrieval FAILS for complementary embeddings**:

Progressive retrieval (draft=2, verify=4) assumes early embeddings contain most
relevant candidates. With complementary embeddings, each embedding retrieves
different documents — draft with 2 embeddings only finds candidates for 2 out of
6 aspects. Result: **recall drops to 0.33-0.51** (catastrophic).

| Method             | Concentrated | Mixed  | Spread |
|--------------------|-------------|--------|--------|
| Independent        | 0.985       | 0.988  | 0.950  |
| Adaptive Batch     | **0.990**   | **0.995** | **0.950** |
| Progressive (d=2)  | 0.347       | 0.337  | 0.325  |

**Why Full Batch can be SLOWER for spread embeddings**:

Full batch (unified queue) exploring 6 dissimilar directions visits too many nodes.
In spread configuration: 285K distance calcs vs 32K for independent → **0.5× speed
(slower than baseline)**. Adaptive Batch avoids this by falling back to independent.

**Why optimization headroom is limited for complementary embeddings**:

With ~0% ground truth overlap, the 6 embeddings explore nearly disjoint graph regions.
Shared visited set provides minimal savings because few nodes are shared across
queries. This is fundamentally different from MIND concentrated interests where
many interests share the same graph region.

### Strategy 3: Speculative Search (Experimental)

Draft/verify pattern inspired by speculative decoding in LLMs:
- Draft: batch search with small ef (10-20% of full)
- Verify: full batch search seeded from draft results

Currently **not recommended** at scales under 500K docs — the verify phase overhead
outweighs draft savings. May benefit at larger scale where entry point descent
is a significant cost.

## Architecture

### Overview

```
Query Layer (Blueprint)                    Index Layer (HnswIndex)
─────────────────────                      ────────────────────────
                                           nearest_neighbor_index.h
                                           ├── find_top_k_batch()         [virtual]
                                           ├── find_top_k_batch_with_filter()
                                           └── find_top_k_progressive()
                                                      │
intermediate_blueprints.cpp                            │ override
├── OrBlueprint::optimize_self()           hnsw_index.h/cpp
│   └── try_optimize_batch_nn()            ├── search_layer_batch_helper  [core: shared visited + batch dist]
│       (detect N same-field NN queries    ├── search_layer_batch         [VisitedTracker selection]
│        → merge into 1 batch blueprint)   ├── top_k_candidates_batch     [shared entry point descent]
│                                          ├── find_top_k_batch           [public API → top_k_by_docid_batch]
nearest_neighbor_batch_blueprint.h/cpp     ├── find_top_k_progressive     [draft HNSW + verify re-rank]
├── perform_top_k_batch()                  └── find_top_k_batch_speculative [draft/verify, experimental]
│   (calls batch/progressive/speculative)
├── set_global_filter()                    hnsw_index_utils.h
│   (filter-aware, brute-force fallback)   ├── BatchHnswCandidate {docid, node_ref, min_distance}
└── createLeafSearch()                     └── BatchNearestPriQ (ordered by min_distance)
    (NnsIndexIterator over merged hits)
```

### Query Detection & Rewriting

`OrBlueprint::optimize_self()` in `intermediate_blueprints.cpp` detects when
multiple children are `NearestNeighborBlueprint` targeting the **same** tensor
attribute (matched by `ITensorAttribute*` pointer). When 2+ NN queries target the
same field, it extracts their query tensors, removes the individual NN blueprints,
and inserts a single `NearestNeighborBatchBlueprint` holding all query tensors.

```
Before:  OR(NN(field, q1), NN(field, q2), ..., NN(field, qN), other_children...)
After:   OR(NNBatch(field, [q1..qN]), other_children...)
```

### Core Algorithm: `search_layer_batch_helper`

The batch search algorithm in `hnsw_index.cpp`:

1. **Shared entry point descent**: Navigate upper HNSW levels once using the first
   query vector. All queries start from the same entry point at level 0.

2. **Unified candidate queue**: A single `BatchNearestPriQ` ordered by `min_distance`
   (minimum distance across all queries). This ensures nodes promising for ANY query
   are explored first.

3. **Per-query result tracking**: Each query maintains its own `FurthestPriQ` of
   best-k results and a `limit_dist` threshold. A candidate is useful if its distance
   to any query is below that query's limit.

4. **Global pruning**: Stop when the candidate's `min_distance` exceeds ALL queries'
   limits (i.e., no query can benefit from further exploration).

5. **Filter-once**: When a filter bitvector is present, each node is checked against
   the filter exactly once (not N times).

### Batch Distance Computation

The core optimization in `search_layer_batch_helper`:

```cpp
// For each neighbor of the current candidate:
auto neighbor_vec = get_vector(neighbor_docid);  // ~0ns pointer dereference
for (size_t q = 0; q < num_queries; ++q) {
    double dist = calc_distance(query_vecs[q], neighbor_vec);  // N distance calcs
    // Update per-query best results...
}
```

Independent search visits the same node N times. Batch search visits it once
and computes N distances.

**Note on vector load cost**: In C++, `get_vector(docid)` is an inline pointer
dereference (~0ns). There is no separate "vector load" cost — the memory access
happens inside `_distance_func->calc()` as part of the distance computation.
The batch advantage comes from reducing the total number of unique nodes visited
(shared visited set), not from eliminating a separate load step.

**Distance calc cost** (includes vector memory access):

| Cache scenario        | distCalc/node | Note                              |
|-----------------------|---------------|-----------------------------------|
| Cold (L3 miss)        | ~50ns         | First access: ~48ns mem + ~2ns IP |
| Mixed (realistic)     | ~10ns         | HNSW traversal cache hit mix      |
| Warm (L1/L2 hit)      | ~3ns          | Hot graph region, mostly compute  |

Batch speedup ≈ DistCalc reduction ratio (the reliable metric).

### Filter Handling

Batch search evaluates filters once per unique node (not N times):

| Scenario              | Independent (10 queries) | Batch          | Saving |
|-----------------------|--------------------------|----------------|--------|
| Filter check per node | 10×                      | 1×             | 10×    |
| Node visits           | 10×                      | 1×             | 10×    |
| Distance calcs        | 10×                      | 10× (unchanged)| 0×     |

For restrictive filters (e.g., category + region), the filter bitvector check
savings alone can be significant.

### Result Merging in Blueprint

`NearestNeighborBatchBlueprint::perform_top_k_batch()` calls the appropriate
index method, then merges per-query results:

```cpp
// Union of all per-query results, keeping min distance per docid
std::unordered_map<uint32_t, double> doc_to_min_dist;
for (const auto& hits : per_query_hits) {
    for (const auto& hit : hits) {
        auto it = doc_to_min_dist.find(hit.docid);
        if (it == doc_to_min_dist.end())
            doc_to_min_dist.emplace(hit.docid, hit.distance);
        else
            it->second = std::min(it->second, hit.distance);
    }
}
// Sort by docid for deterministic iterator output
```

Each document's score = **min distance across all query vectors** (best match among
interests/aspects). The merged hits are delivered via `NnsIndexIterator`.

### Global Filter Handling in Blueprint

`set_global_filter()` determines the search strategy:
1. If filter passes too few docs (`max_hit_ratio < brute_force_limit`) → fall back
   to brute-force (disable approximate search)
2. Otherwise → pass filter bitvector to batch search, which checks it once per node

## Configuration

The `NearestNeighborBatchBlueprint` supports three modes via constructor parameters:

```cpp
NearestNeighborBatchBlueprint(field, attr_tensor, query_tensors,
    target_num_hits_per_query,
    approximate,
    explore_additional_hits,
    distance_threshold,
    brute_force_limit,
    use_speculative,      // Enable speculative draft/verify
    use_progressive,      // Enable progressive retrieval (OnePiece)
    progressive_draft_steps  // Number of vectors for HNSW phase (default: 2)
);
```

| Parameter                 | MIND Setting          | OnePiece Setting          |
|---------------------------|-----------------------|---------------------------|
| `use_speculative`         | `false`               | `false`                   |
| `use_progressive`         | `false`               | `false` (complementary)   |
| `progressive_draft_steps` | N/A                   | N/A                       |
| Strategy used             | Adaptive Batch        | Adaptive Batch            |

**Note**: Progressive retrieval (`use_progressive=true`) is only appropriate when
embeddings are truly progressive refinements (coarse → fine). For OnePiece's
complementary embeddings, use the default Adaptive Batch path (`use_progressive=false`).

## Cost Model

**Corrected**: In C++, `get_vector(docid)` is an inline pointer dereference
returning a `TypedCells` view — it costs ~0ns. There is **no separate vector
load cost**. The memory access (cache hit/miss) happens inside
`_distance_func->calc()` as part of the distance computation.

| Operation                        | Latency   | Notes                               |
|----------------------------------|-----------|--------------------------------------|
| get_vector() (pointer deref)     | ~0ns      | Inline, returns TypedCells view      |
| Distance calc (cold, L3 miss)    | ~50ns     | ~48ns memory fetch + ~2ns compute    |
| Distance calc (mixed, realistic) | ~10ns     | HNSW traversal with cache locality   |
| Distance calc (warm, L1/L2 hit)  | ~3ns      | Hot graph region, mostly compute     |
| Filter bitvector check           | ~1ns      | Usually in L1 cache                  |

The batch search advantage comes from **reducing the number of unique nodes
visited** (shared visited set), not from eliminating a separate vector load step.
Projected speedup ≈ **DistCalc reduction ratio**, which is the reliable metric.

## Projected Production Impact

Projected speedup ≈ **DistCalc reduction ratio** (since vector load ≈ 0 in C++).
The actual distance calc latency varies with cache behavior (3-50ns), but the
speedup ratio is determined by how many fewer distance calculations are needed.

### MIND (10 interests)

Performance depends on interest distribution — real users typically have 1-3 dominant
interest categories, making concentrated/mixed the common case:

```
                          DC Reduction   Projected Speedup
Concentrated (2 clusters):   3.6×        ~3.6× (shared graph region)
Mixed (3+3+4):               1.5×        ~1.5×
Spread (10 clusters):        1.0×        ~1.0× (no penalty)
```

Most real MIND users fall in concentrated/mixed.

### OnePiece (6 complementary embeddings)

OnePiece embeddings are **complementary** (each captures a different aspect),
with near-zero ground truth overlap between embeddings. This limits optimization
headroom compared to MIND:

```
                          DC Reduction   Projected Speedup
Concentrated (2 aspects):   1.3×         ~1.3×
Mixed (3 aspects):          1.2×         ~1.2×
Spread (6 aspects):         1.0×         ~1.0× (no penalty)
```

**Important**: Progressive retrieval is NOT applicable — recall drops to 0.33 because
draft embeddings cannot represent other aspects' candidates.

The modest DC reduction (1.0-1.3×) reflects the fundamental constraint:
complementary embeddings explore disjoint graph regions, so shared visited set
provides minimal benefit. Real-world improvement depends on how much overlap exists
between the actual OnePiece embeddings — if some aspects are correlated in practice,
the DC reduction (and thus speedup) could be higher.

### Filter Savings (both MIND and OnePiece)

Even when DistCalc savings are modest, batch search still saves on filter
evaluation:

```
                     Independent (6 queries)    Adaptive Batch    Saving
Filter checks/node:        6×                      1×              6×
```

For restrictive filters (category + region), this alone may justify batch search.

## Failed Optimization Attempts (MIND)

The following approaches were explored to push MIND batch search beyond Adaptive
Batch v1. All failed due to fundamental limitations. Documenting them here to
avoid repeating the same dead ends.

### Attempt 1: Triangle Inequality Pruning (Selective Distance Computation)

**Idea**: If query A and query B are similar (small inter-query distance), use A's
distance to a node to derive a lower bound for B's distance, skipping the distance
computation when the bound exceeds B's current threshold.

For IP distance on normalized vectors:
```
ipDist(q_b, doc) >= (sqrt(ipDist(anchor, doc)) - sqrt(ipDist(anchor, q_b)))²
```

**Result**: Zero effective pruning in 64 dimensions.

**Why it failed**: In high-dimensional space, normalized vectors are nearly orthogonal.
The triangle inequality bound `(√d1 - √d2)²` produces values far below actual
distances (e.g., bound = 0.02 when actual distance = 0.8). The bound is
mathematically correct but practically useless — it almost never exceeds the
pruning threshold.

**Lesson**: Triangle inequality pruning works well in low dimensions (2D-8D) where
distances have more variance. In 64+ dimensions, the "curse of dimensionality"
makes all pairwise distances concentrate around similar values, rendering
lower bounds too loose.

### Attempt 2: Per-Query Early Termination

**Idea**: Track per-query "stale count" — how many consecutive candidates fail to
improve a query's top-k. When staleCount exceeds a threshold, mark that query as
"done" and stop computing distances for it.

**Result**: Recall destroyed for spread interests (10 different clusters).

**Why it failed**: Spread interests need to traverse long graph paths to reach
distant clusters. Early in the search, many candidates are far from a spread
interest's target — producing high stale counts. But the search eventually reaches
the target region through graph connectivity. Premature termination cuts off this
path before the query finds its results.

**Lesson**: In HNSW, "no improvement for N steps" does NOT mean "no improvement
possible." The graph structure means good results can appear after long plateaus,
especially when the target region is far from the entry point. Early termination
is fundamentally incompatible with interests that explore different graph regions.

### Attempt 3: Per-Query Exploration Queues + Shared Visited Set

**Idea**: Give each query its own candidate priority queue (instead of one unified
queue), but share the visited set to avoid redundant node visits. When any query
visits a node, compute distances for all queries. Each query independently decides
which neighbors to explore based on its own best candidates.

**Result**: **Catastrophic failure**. Union recall dropped to 0.02-0.46. Vector loads
increased 5-6× above independent baseline.

**Why it failed**: This is a fundamental incompatibility. The shared visited set
blocks exploration paths for other queries:

```
Query A explores region X, marking nodes v1, v2, v3, ... as visited.
Query B needs to traverse THROUGH v1, v2, v3 to reach its target region Y.
But v1, v2, v3 are already marked visited → Query B cannot enter region Y.
Query B is stuck exploring whatever nodes are reachable WITHOUT going through
the nodes Query A already visited.
```

The unified queue in v1 avoids this because ALL queries share the same exploration
frontier — when the frontier moves through region X, it naturally continues toward
region Y. Per-query queues fragment the exploration, and the shared visited set
turns this fragmentation into an impenetrable barrier.

**Lesson**: Shared visited set **requires** unified exploration (single candidate
queue driving all queries together). Any approach that combines shared visited set
with independent per-query exploration will fundamentally break recall. These two
design choices are mutually exclusive.

### Conclusion

Adaptive Batch v1 (unified queue + shared visited set + adaptive clustering) is
the optimal algorithmic approach for MIND batch search. The 3.6× / 1.5× / 1.0×
results for concentrated / mixed / spread interests represent the algorithmic
ceiling for this approach.

Further MIND performance gains require **system-level** optimizations:
- Thread-level parallelism (parallel interest groups)
- SIMD batch distance computation (load vector once, compute N distances with AVX)
- Memory prefetching during graph traversal
- Hardware-specific tuning (cache line size, NUMA-aware allocation)

## Files Changed

### C++ Core (searchlib)

| File | Change |
|------|--------|
| `.../tensor/nearest_neighbor_index.h` | Added `find_top_k_batch`, `find_top_k_batch_with_filter`, `find_top_k_progressive` virtual methods |
| `.../tensor/nearest_neighbor_index.cpp` | Default fallback implementations (loop calling single-vector `find_top_k`) |
| `.../tensor/hnsw_index.h` | Batch/progressive/speculative method declarations + private helpers |
| `.../tensor/hnsw_index.cpp` | Core implementations: `search_layer_batch_helper` (shared visited + batch dist), `top_k_candidates_batch`, `find_top_k_batch`, `find_top_k_progressive`, `find_top_k_batch_speculative` |
| `.../tensor/hnsw_index_utils.h` | `BatchHnswCandidate` (min_distance across queries), `BatchNearestPriQ` |
| `.../queryeval/nearest_neighbor_batch_blueprint.h` | `NearestNeighborBatchBlueprint` class: holds N query tensors, merged results |
| `.../queryeval/nearest_neighbor_batch_blueprint.cpp` | `perform_top_k_batch()`, `set_global_filter()`, `createLeafSearch()`, cell type conversion |
| `.../queryeval/CMakeLists.txt` | Added `nearest_neighbor_batch_blueprint.cpp` |
| `.../queryeval/intermediate_blueprints.cpp` | `try_optimize_batch_nn()` in `OrBlueprint::optimize_self()` — detects same-field NN queries |

### Tests

| File | Change |
|------|--------|
| `searchlib/src/tests/tensor/hnsw_index/hnsw_index_test.cpp` | 7 test cases: batch vs independent consistency, batch with filter, single-vector batch, speculative recall, identical vectors, progressive retrieval, progressive with filter |

### Benchmarks (Go)

| File | Description |
|------|-------------|
| `benchmark_batch_hnsw.go` | MIND benchmark: 100K docs, 64-dim, 50 clusters, 10 interests, 3 distributions (concentrated/mixed/spread), 4 methods (independent/batch/adaptive/speculative) |
| `benchmark_onepiece_hnsw.go` | OnePiece benchmark: 500K docs, 64-dim, 6 complementary embeddings, 3 distributions, 4 methods (independent/batch/adaptive/progressive) |

## Benchmark Reproduction

```bash
# MIND multi-interest benchmark (~2min on modern hardware)
go run benchmark_batch_hnsw.go

# OnePiece complementary embedding benchmark (~15min, 500K graph build)
go run benchmark_onepiece_hnsw.go
```

Both benchmarks output:
- Raw counters: DistCalcs, VecLoads (informational), DC/VL reduction ratios
- Projected speedup under 3 distance calc cost scenarios (cold 50ns / mixed 10ns / warm 3ns)
- Per-query and union recall vs brute-force ground truth

## Key Design Decisions

1. **Unified queue + shared visited set**: The ONLY working combination. Per-query
   queues + shared visited set is fundamentally broken (see Failed Attempts §3).
   The unified queue ensures all queries share the exploration frontier.

2. **Adaptive clustering**: Auto-groups similar queries (dot product > 0.3) for
   batching, falls back to independent for dissimilar queries. This guarantees
   no regression vs baseline.

3. **DistCalc reduction as primary metric**: In C++, vector load is ~0 (inline
   pointer dereference). The speedup comes from reducing the number of distance
   computations via shared visited set. Speedup ≈ DC reduction ratio.

4. **Complementary ≠ progressive**: OnePiece embeddings are complementary (each
   captures a different aspect with ~0% ground truth overlap). Progressive retrieval
   (draft/verify) is catastrophically wrong for this case (recall → 0.33).

5. **Backwards compatible**: Default implementations in base class fall back to
   per-vector loops. Existing single-query paths are untouched.

## Next Steps for Production Validation

1. **Measure real embedding similarity**: Run dot-product analysis on actual MIND/OnePiece
   embeddings to determine which distribution case (concentrated/mixed/spread) applies.
   This determines the expected DC reduction.

2. **Profile actual cache behavior**: Use `perf stat` to measure L2/L3 miss rates
   during HNSW search. This determines where in the cold→warm range the real system
   operates, and thus the actual speedup.

3. **Test with real filters**: Filter amplification savings (N× → 1× filter checks
   per node) are independent of cache assumptions and may be the largest win in
   restrictive filter scenarios.

4. **System-level optimizations** (if algorithmic ceiling is reached):
   - Thread-level parallelism for interest groups
   - SIMD batch distance computation
   - Memory prefetching during graph traversal
