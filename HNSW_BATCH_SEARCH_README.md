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

### Strategy 2: Progressive Retrieval (OnePiece)

**For**: OnePiece-style progressive embeddings where each step refines the same
user representation (coarse → fine).

```
Progressive Retrieval:
  Phase 1 (Draft):  Batch HNSW search using step 1-2 embeddings (coarse)
                    → retrieve wider candidate set (3× k)
  Phase 2 (Verify): Brute-force re-rank candidates using step 3-6 embeddings (fine)
                    → trivial cost (~200 candidates × distance calc)
```

**Key insight**: Progressive embeddings have 60%+ ground-truth overlap between steps.
The first 2 steps already find most of the relevant candidates. Later steps only
need to re-score a small candidate set, not traverse the full HNSW graph.

**Benchmark results** (500K docs, 64-dim, 6 progressive steps, 5 users):

| Method                    | VecLoads | Proj. Speedup | Recall |
|---------------------------|----------|---------------|--------|
| Independent (6× single)  | 40073    | 1.0×          | 0.943  |
| Batch (all 6 steps)      | 9036     | **3.6×**      | 0.983  |
| Progressive (draft=2)    | 8506     | **4.5×**      | 0.965  |
| Progressive (draft=3)    | 8996     | **4.1×**      | 0.983  |

Progressive retrieval is the clear winner for OnePiece:
- **4.5× projected speedup** with draft=2 (8× CPU → ~1.8× CPU)
- **Recall improves** because batch search lets adjacent steps help each other
- Re-rank phase adds negligible cost (~200 brute-force distance calcs)

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
| `use_progressive`         | `false`               | **`true`**                |
| `progressive_draft_steps` | N/A                   | **`2`** (or `3`)          |
| Strategy used             | Adaptive Batch        | Progressive Retrieval     |

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

### OnePiece (6 progressive steps)

```
Current:     6 × independent HNSW = 8× single-query CPU (measured)
Progressive: 2 batch HNSW + 4 brute-force re-rank = ~1.8× CPU
Improvement: ~4.5× speedup (8× → 1.8×)
```

### OnePiece with Filter

```
Current:     6 × (HNSW + filter eval) = 8× + filter overhead
Progressive: 2 × (HNSW + filter eval) + 4 × (brute-force, trivial filter)
Improvement: ~5× (filter checked on fewer nodes + fewer HNSW traversals)
```

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
