# HNSW Multi-Embedding Batch Search Optimization

Optimized HNSW index search for scenarios where multiple query embeddings target
the same tensor field — specifically **MIND** (Multi-Interest Network with Dynamic
Routing) and **OnePiece** progressive embedding retrieval.

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
- 10 × independent vector loads (L3 cache misses at ~40ns each)
- Same document vectors loaded multiple times across queries

For OnePiece with 6 progressive embeddings, the measured cost is **8× single-query
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
visited set avoids redundant vector loads. Distant interests explore disjoint regions
where batching adds overhead without saving vector loads.

**Benchmark results** (100K docs, 64-dim, 50 clusters, 10 interests, averaged over 5 users):

| Interest Distribution    | Strategy         | VecLoads | Proj. Speedup | Recall |
|--------------------------|------------------|----------|---------------|--------|
| Concentrated (2 clusters)| Adaptive Batch   | 5253     | **3.6×**      | 0.996  |
| Mixed (3+3+4)            | Adaptive Batch   | 14337    | **1.5×**      | 0.997  |
| Spread (10 clusters)     | Adaptive Batch   | 22623    | **1.0×**      | 0.997  |

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

| Embedding Distribution    | Strategy       | VecLoads | Proj. Speedup | Recall |
|---------------------------|----------------|----------|---------------|--------|
| Concentrated (2 aspects)  | Adaptive Batch | 21797    | **1.3×**      | 0.990  |
| Mixed (3 aspects)         | Adaptive Batch | 24086    | **1.2×**      | 0.995  |
| Spread (6 aspects)        | Adaptive Batch | 32420    | **1.0×**      | 0.950  |

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

### C++ Implementation

```
nearest_neighbor_index.h       // Virtual base: find_top_k_batch, find_top_k_progressive
├── hnsw_index.h/cpp           // Core implementation:
│   ├── search_layer_batch_helper  // Shared visited set + batch distance
│   ├── search_layer_batch         // Visited tracker selection (BitVector/HashSet)
│   ├── top_k_candidates_batch     // Shared entry point descent
│   ├── find_top_k_batch           // Public batch search API
│   ├── find_top_k_progressive     // Progressive: draft HNSW + verify re-rank
│   └── find_top_k_batch_speculative // Speculative draft/verify
│
├── hnsw_index_utils.h         // BatchHnswCandidate, BatchNearestPriQ
│
└── nearest_neighbor_batch_blueprint.h/cpp  // Query evaluation layer
    ├── perform_top_k_batch()      // Calls batch/progressive based on config
    ├── set_global_filter()        // Filter-aware, brute-force fallback
    └── createLeafSearch()         // Returns NnsIndexIterator over merged hits
```

### Batch Distance Computation

The core optimization in `search_layer_batch_helper`:

```cpp
// For each neighbor of the current candidate:
auto neighbor_vec = get_vector(neighbor_docid);  // ONE vector load
for (size_t q = 0; q < num_queries; ++q) {
    double dist = calc_distance(query_vecs[q], neighbor_vec);  // N distance calcs
    // Update per-query best results...
}
```

Independent search loads the same vector N times (N cache misses at ~40ns each).
Batch search loads it once (1 cache miss) and computes N distances (~2ns each).

**Saving per shared node**: `(N-1) × 40ns - (N-1) × 2ns ≈ (N-1) × 38ns`

For N=10 interests: **342ns saved per shared visited node**.

### Filter Handling

Batch search evaluates filters once per unique node (not N times):

| Scenario              | Independent (10 queries) | Batch          | Saving |
|-----------------------|--------------------------|----------------|--------|
| Filter check per node | 10×                      | 1×             | 10×    |
| Vector load per node  | 10×                      | 1×             | 10×    |
| Distance calcs        | 10×                      | 10× (unchanged)| 0×     |

For restrictive filters (e.g., category + region), the filter bitvector check
savings alone can be significant.

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

Real-world HNSW search is **memory-bandwidth bound**, not compute-bound:

| Operation                  | Latency | Notes                          |
|----------------------------|---------|--------------------------------|
| Vector load (L3 miss)     | ~40ns   | 256 bytes random access        |
| Distance calc (64-dim IP) | ~2ns    | Sequential arithmetic (SIMD)   |
| Filter bitvector check    | ~1ns    | Usually in L1 cache            |

The **20:1 ratio** between vector load and distance calc means reducing vector
loads is the primary optimization lever. Batch search targets exactly this.

## Projected Production Impact

### MIND (10 interests)

Performance depends on interest distribution — real users typically have 1-3 dominant
interest categories, making concentrated/mixed the common case:

```
                         Independent    Adaptive Batch    Speedup
Concentrated (2 clusters):  10× CPU       ~2.8× CPU       3.6×
Mixed (3+3+4):              10× CPU       ~6.7× CPU       1.5×
Spread (10 clusters):       10× CPU       10× CPU         1.0× (no penalty)
```

Most real MIND users fall in concentrated/mixed → **expected 1.5×-3.6× speedup**.

### OnePiece (6 complementary embeddings)

OnePiece embeddings are **complementary** (each captures a different aspect),
with near-zero ground truth overlap between embeddings. This limits optimization
headroom compared to MIND:

```
                         Independent    Adaptive Batch    Speedup
Concentrated (2 aspects):  8× CPU        ~6.2× CPU        1.3×
Mixed (3 aspects):         8× CPU        ~6.7× CPU        1.2×
Spread (6 aspects):        8× CPU        8× CPU            1.0× (no penalty)
```

**Important**: Progressive retrieval is NOT applicable — recall drops to 0.33 because
draft embeddings cannot represent other aspects' candidates.

The modest speedup (1.0-1.3×) reflects the fundamental constraint: complementary
embeddings explore disjoint graph regions, so shared visited set provides minimal
benefit. Real-world improvement depends on how much overlap exists between the
actual OnePiece embeddings — if some aspects are correlated in practice, speedup
could be higher.

### Filter Savings (both MIND and OnePiece)

Even when vector load savings are modest, batch search still saves on filter
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
queue), but share the visited set to avoid redundant vector loads. When any query
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

| File | Change |
|------|--------|
| `searchlib/.../tensor/nearest_neighbor_index.h` | Added `find_top_k_batch`, `find_top_k_batch_with_filter`, `find_top_k_progressive` virtual methods |
| `searchlib/.../tensor/nearest_neighbor_index.cpp` | Default implementations (fallback to per-vector) |
| `searchlib/.../tensor/hnsw_index.h` | Batch/progressive search method declarations |
| `searchlib/.../tensor/hnsw_index.cpp` | Core batch search, speculative, progressive implementations |
| `searchlib/.../tensor/hnsw_index_utils.h` | `BatchHnswCandidate`, `BatchNearestPriQ` types |
| `searchlib/.../queryeval/nearest_neighbor_batch_blueprint.h/cpp` | Query evaluation layer for batch/progressive |
| `searchlib/.../queryeval/CMakeLists.txt` | Added batch blueprint source |
| `searchlib/.../queryeval/intermediate_blueprints.cpp` | OR optimization to detect same-field NN queries |
| `searchlib/src/tests/tensor/hnsw_index/hnsw_index_test.cpp` | Batch and progressive search tests |
| `benchmark_batch_hnsw.go` | MIND benchmark (100K docs, 3 interest distributions) |
| `benchmark_onepiece_hnsw.go` | OnePiece benchmark (500K docs, progressive embeddings) |

## Benchmark Reproduction

```bash
# MIND multi-interest benchmark
go run benchmark_batch_hnsw.go

# OnePiece progressive retrieval benchmark
go run benchmark_onepiece_hnsw.go
```
