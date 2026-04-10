# Int8 Speculative Recall — Vespa Implementation Design

## Problem
Multi-embedding retrieval (MIND 10 interests, OnePiece 6 embeddings) requires N independent HNSW searches per query. Each search costs ~27K float32 distance calculations. We need to reduce this cost without sacrificing recall.

## Solution: Int8 Graph Traversal + Float32 Rerank
- **Graph traversal**: Use int8 quantized vectors for HNSW navigation (~4x cheaper with VNNI/AVX512)
- **Rerank**: Score top candidates with full float32 precision
- **Result**: ~3x effective speedup, <1% recall loss (validated in Go benchmark)

## Why This Works
1. HNSW graph traversal only needs *relative ordering* — int8 preserves rank correlation (94-96% overlap @200)
2. Final scoring uses float32 — no quality loss in output
3. AVX512 VNNI processes 4x int8 ops per instruction vs float32

## Vespa Already Supports This
- `tensor<int8>` with HNSW index (distance_function_factory.cpp)
- HW-accelerated int8 dot product (avx512.cpp, avx2.cpp)
- Two-phase ranking with rerank-count

---

## Schema Design

```
schema user_embedding {
  document user_embedding {
    # Original float32 embeddings for precise reranking
    field interest_0 type tensor<float>(x[128]) {
      indexing: attribute | summary
    }
    # ... interest_1 through interest_9

    # Int8 quantized embeddings for fast HNSW traversal
    field interest_0_i8 type tensor<int8>(x[128]) {
      indexing: attribute | index
      attribute {
        # IMPORTANT: Use euclidean, NOT innerproduct!
        # Int8 HW acceleration (AVX512) only exists for euclidean distance.
        # InnerProduct with int8 falls back to float conversion (no speedup).
        # For normalized vectors: euclidean ranking == innerproduct ranking
        # (because ||a-b||^2 = 2 - 2*<a,b> for unit vectors)
        distance-metric: euclidean
      }
      index {
        hnsw {
          max-links-per-node: 16
          neighbors-to-explore-at-insert: 200
        }
      }
    }
    # ... interest_1_i8 through interest_9_i8
  }

  # Speculative recall: int8 HNSW → float32 rerank
  rank-profile speculative_recall inherits default {
    inputs {
      query(q) tensor<float>(x[128])
      query(q_i8) tensor<int8>(x[128])
    }
    first-phase {
      expression: closeness(field, interest_0_i8)
    }
    second-phase {
      rerank-count: 200
      expression: sum(query(q) * attribute(interest_0))
    }
  }
}
```

## Document Processor — Quantize at Write Time

```java
public class Int8QuantizationProcessor extends DocumentProcessor {

    @Override
    public Progress process(Processing processing) {
        for (DocumentOperation op : processing.getDocumentOperations()) {
            if (op instanceof DocumentPut) {
                Document doc = ((DocumentPut) op).getDocument();
                for (int i = 0; i < 10; i++) {
                    quantizeField(doc, "interest_" + i, "interest_" + i + "_i8");
                }
            }
        }
        return Progress.DONE;
    }

    private void quantizeField(Document doc, String srcField, String dstField) {
        TensorFieldValue src = (TensorFieldValue) doc.getFieldValue(srcField);
        if (src == null) return;

        Tensor srcTensor = src.getTensor().orElse(null);
        if (srcTensor == null) return;

        // Symmetric quantization: int8_val = round(float_val * 127)
        // Assumes input vectors are L2-normalized (values in [-1, 1])
        TensorType dstType = new TensorType.Builder(TensorType.Value.INT8)
            .indexed("x", 128).build();

        IndexedTensor.Builder builder = IndexedTensor.Builder.of(dstType);
        Iterator<Tensor.Cell> cells = srcTensor.cellIterator();
        while (cells.hasNext()) {
            Tensor.Cell cell = cells.next();
            double val = cell.getDoubleValue();
            // Clamp to [-1, 1] then scale to [-127, 127]
            val = Math.max(-1.0, Math.min(1.0, val));
            int quantized = (int) Math.round(val * 127.0);
            builder.cell(quantized, cell.getKey().numericLabel(0));
        }

        doc.setFieldValue(dstField, new TensorFieldValue(builder.build()));
    }
}
```

## Query Flow

```
# Client sends both float32 and int8 query vectors
POST /search/
{
  "yql": "select * from user_embedding where {targetHits:100}nearestNeighbor(interest_0_i8, q_i8)",
  "ranking": "speculative_recall",
  "input.query(q)": {"values": [0.1, -0.3, ...]},        // float32
  "input.query(q_i8)": {"values": [13, -38, ...]}          // pre-quantized int8
}
```

**What happens internally:**
1. `nearestNeighbor(interest_0_i8, q_i8)` → HNSW search using int8 distances (AVX512 accelerated)
2. Returns targetHits (100) candidates from int8 graph
3. `first-phase`: scores all 100 candidates with int8 closeness
4. `second-phase`: reranks top 200 (or all 100) with float32 `sum(query(q) * attribute(interest_0))`
5. Final result: precision of float32, speed of int8

## Multi-Embedding Query (MIND with 10 interests)

For multi-embedding retrieval, run N separate nearestNeighbor searches and merge:

```
yql: select * from user_embedding where
  {targetHits:100}nearestNeighbor(interest_0_i8, q_i8) OR
  {targetHits:100}nearestNeighbor(interest_1_i8, q_i8) OR
  ...
```

Each nearestNeighbor independently traverses its int8 HNSW graph.
Second-phase reranks the merged candidate set with float32.

## Cost Analysis

### Per-search costs (128D, ef=200)
| Operation | Float32 | Int8+Rerank |
|---|---|---|
| HNSW DCs | ~27K × 50ns = 1.35ms | ~27K × 12ns = 0.33ms |
| Rerank DCs | 0 | 200 × 50ns = 0.01ms |
| **Total** | **1.35ms** | **0.34ms** |
| **Speedup** | 1.0x | **~4x** |

### Multi-embedding (N=10 interests)
| Scenario | Float32 | Int8+Rerank |
|---|---|---|
| 10 searches | 13.5ms | 3.4ms |
| Speedup | 1.0x | ~4x |

*Note: 50ns assumes warm cache. Cold cache (L3 miss) may be higher, but int8 benefits from 4x smaller memory footprint → better cache utilization, amplifying the advantage.*

## Storage Overhead
- Float32: 128 × 4 = 512 bytes per vector
- Int8: 128 × 1 = 128 bytes per vector
- Overhead per interest: 128 bytes (25% increase)
- For 10 interests: 1,280 bytes additional = 25% more storage
- HNSW graph structure: shared or separate (separate is simpler, ~2x graph memory)

## Trade-offs
| Aspect | Benefit | Cost |
|---|---|---|
| Latency | ~4x per search | None |
| Recall | <1% loss | Requires validation |
| Storage | Int8 vectors 4x smaller | Duplicate vectors (float32 + int8) |
| Write path | Simple quantization | Document processor overhead (~negligible) |
| Complexity | Uses existing Vespa features | Additional fields in schema |

## Alternative: Single Int8 Field (No Rerank)
If 94-96% recall is acceptable, skip float32 entirely:
- Only store `tensor<int8>(x[128])` with HNSW index
- No float32 attribute, no second-phase rerank
- 4x less storage, ~4x faster
- Trade-off: ~4-6% recall loss

## Critical Implementation Detail: Distance Metric

**Must use `distance-metric: euclidean` for int8 HNSW, NOT `innerproduct`.**

Looking at Vespa source code (`distance_function_factory.cpp` + `euclidean_distance.h`):

```cpp
// euclidean_distance.h line 49 — Int8 HW acceleration via reinterpret_cast
static const int8_t *cast(const Int8Float * p) {
    return reinterpret_cast<const int8_t *>(p);
}
// → calls _computer.squaredEuclideanDistance(int8_t*, int8_t*, sz)
// → AVX512/AVX2 accelerated int8 computation
```

```cpp
// distance_function_factory.cpp — InnerProduct does NOT support Int8
case DistanceMetric::InnerProduct:
    switch (cell_type) {
    case CellType::FLOAT:  return InnerProductDistanceHW<float>();   // ✓
    case CellType::DOUBLE: return InnerProductDistanceHW<double>();  // ✓
    default:               return InnerProductDistance(CellType::FLOAT);  // ✗ fallback!
    }
```

For normalized vectors (L2 norm = 1), euclidean and inner product give identical rankings:
- `||a-b||^2 = ||a||^2 + ||b||^2 - 2<a,b> = 2 - 2<a,b>`
- Minimizing euclidean distance = maximizing inner product

The int8 `squaredEuclideanDistance` implementation (`private_helpers.hpp`):
```cpp
// "3 times faster with int32_t than with int64_t and 16x faster than float"
template<typename TemporaryT=int32_t>
double squaredEuclideanDistanceT(const int8_t * a, const int8_t * b, size_t sz) {
    TemporaryT sum = 0;
    for (size_t i(0); i < sz; i++) {
        int16_t d = int16_t(a[i]) - int16_t(b[i]);
        sum += d * d;
    }
    return sum;
}
```

## Quantization Detail

Vespa's `Int8Float` does simple truncation (`int8_t _bits = (int8_t)float_value`).
For normalized vectors in [-1, 1], this maps everything to {-1, 0, 1} — **terrible quality**.

The document processor MUST do proper quantization: `int8_val = round(float_val * 127.0)`.
The stored int8 values should be in [-127, 127], not [-1, 1].
Distance computation operates on the quantized values directly.

## Implementation Steps
1. **Schema change**: Add int8 tensor fields with `distance-metric: euclidean` + HNSW index
2. **Document processor**: Quantize float32 → int8 with `round(val * 127)` at write time
3. **Rank profile**: first-phase int8 euclidean, second-phase float32 inner product
4. **Query template**: Send both q (float32) and q_i8 (pre-quantized int8)
5. **Validation**: A/B test recall and latency against float32 baseline
