# Case Study: AI-Assisted Engineering — From Overconfidence to Discovery

## The Journey of Optimizing Multi-Embedding HNSW Search

### Context
We needed to speed up multi-embedding ANN retrieval in Vespa (HNSW index). Users have N interest embeddings (MIND: 10, OnePiece: 6). Each query requires N independent HNSW searches — a linear cost multiplier.

### Phase 1: The "Obvious" Solution (Dunning-Kruger Peak)

**Idea**: Batch HNSW search — share visited set across N queries, compute N distances per node visit.

**AI's initial analysis was extremely optimistic:**
- Projected 2.2x-4.7x speedup
- Built elaborate cost model: vector load = 500ns, distance calc = 50ns
- Conclusion: "Sharing vector loads across N queries is a massive win"

**The cost model was wrong.** In C++ implementation:
```cpp
// get_vector(docid) is just an inline pointer dereference — ~0ns
TypedCells get_vector(uint32_t docid) const {
    return _vectors.get_vector(docid, 0);  // pointer arithmetic, no I/O
}
```

Vector load cost ≈ 0. The entire premise of batch search was built on a false assumption.

**Lesson**: AI (and humans) can build elaborate, internally-consistent analyses on top of wrong assumptions. The cost model *looked* rigorous — it had tables, percentages, multiple scenarios. But it never verified the foundational assumption.

### Phase 2: The Valley of Despair

After C++ benchmarks confirmed batch search was **net negative** (more distance calcs per node, no vector load savings), we tried every angle:

| Approach | Why It Failed |
|---|---|
| **Triangle inequality pruning** | Bounds too loose in 64D+ (curse of dimensionality) |
| **Speculative decoding** | HNSW multi-level descent is per-query, cannot be shared |
| **Warm-start chaining** | Seeds from Query A always rejected by Query B's descent |
| **Shared candidate pool** | Queries have different targets, shared state adds overhead |

Each approach followed the same pattern:
1. AI proposes idea with compelling reasoning
2. We build benchmark to validate
3. Results show 0-2% improvement (noise level)
4. We understand WHY it failed (structural reason)

**Key insight at 500K scale**: HNSW upper-level descent accounts for ~200 DCs but converges to different graph regions per query. Seeds from one query are always worse than fresh descent for another query. The graph structure makes sharing inherently impossible.

### Phase 3: The Pivot — Speculative Recall with Int8

After exhausting algorithmic optimizations, we asked a different question:

> "Can we make each individual distance calculation cheaper?"

**Int8 quantization for graph traversal:**
- Quantize float32 vectors → int8 (scale by 127 for normalized vectors)
- Traverse HNSW graph with int8 distances (~4x cheaper with VNNI/AVX512)
- Rerank final candidates with float32 precision
- Graph traversal only needs relative ordering — int8 preserves this (94-96% overlap)

**Results (Go benchmark, 500K docs, 128D):**
- Float32 DCs per search: ~27,000
- Int8 DCs per search: ~28,000 (same work, 4x cheaper)
- Float32 rerank DCs: ~200 (top candidates only)
- **Effective speedup: ~3x with maintained recall**

### Phase 4: Discovery — Vespa Already Supports This

The best part: Vespa already has all the building blocks:
- `tensor<int8>` type with HNSW index support
- HW-accelerated int8 euclidean distance (AVX512/AVX2)
- Two-phase ranking (int8 HNSW retrieval → float32 rerank)
- Real-time streaming writes (document processor for quantization)

No custom C++ needed. Schema + document processor + rank profile.

---

## Lessons for AI-Assisted Engineering

### 1. AI Amplifies Dunning-Kruger Effect
AI can generate sophisticated analyses that feel authoritative but rest on unverified assumptions. The batch HNSW cost model had tables, formulas, multiple scenarios — and was completely wrong about the most important parameter.

**Mitigation**: Always validate foundational assumptions before building on them. "What would need to be true for this to work?"

### 2. Negative Results Are Valuable
We tried 5+ approaches that failed. Each failure narrowed the search space:
- Batch search failure → problem is per-distance cost, not per-visit overhead
- Triangle inequality failure → can't prune in high dimensions
- Speculative/warm-start failure → can't share HNSW structure across queries
- These eliminations led directly to "make individual DCs cheaper" → int8

**The path to the solution went through the failures.**

### 3. AI + Human = Better Than Either
- **AI strength**: Rapidly prototype and benchmark ideas (Go benchmark in minutes)
- **AI weakness**: Over-optimistic initial analysis, doesn't question assumptions
- **Human strength**: "But in C++, get_vector is just a pointer dereference..."
- **Human weakness**: Would take much longer to implement/benchmark each idea

The human caught the wrong assumption. The AI pivoted quickly to explore alternatives. The combination was powerful.

### 4. Benchmarks > Reasoning
Every approach that "should work" based on reasoning failed when benchmarked:
- Batch search: "sharing visits must save work" → net negative
- Warm-start: "similar queries have nearby results" → seeds always rejected
- Only int8 quantization survived contact with actual measurement

### 5. The Solution Was Orthogonal to the Original Problem
We started trying to optimize N-query coordination (batch, sharing, pruning).
The solution was optimizing single-query cost (int8 quantization).
Sometimes the best optimization is perpendicular to where you're looking.

---

## Timeline
1. **Start**: "Batch HNSW is 2-4x faster!" (wrong)
2. **Reality check**: C++ benchmarks show batch is slower
3. **Exploration**: Triangle inequality, speculative decoding, warm-start → all fail
4. **Understanding**: HNSW structure fundamentally per-query, can't share
5. **Pivot**: "Make individual operations cheaper" → int8 quantization
6. **Validation**: 3x effective speedup confirmed
7. **Discovery**: Vespa already supports all required components

Total wall clock: ~2 hours of AI-assisted exploration that would have taken weeks of human-only work. The failures were cheap (minutes each), the learning was real, and the solution was better than the original approach.
