# Nested Louds Trie (NLT) — Vespa Disk Index Dictionary Exploration

## 1. Background: PageDict4 and Its Performance Bottleneck

Vespa's disk index dictionary uses **PageDict4**, a 7-level hierarchical skip list
stored across 3 separate files:

```
.ssdat (L7 in-memory binary search -> L6 sparse-sparse entries)
   |
.spdat (L5 -> L4 -> L3 sparse page skip levels)
   |
.pdat  (L2 -> L1 -> L0 leaf pages, 4KB each, LCP-compressed terms)
```

A single lookup traverses all 3 files, touching ~7 cache lines across 3 separate
mmap regions. This design was optimized for sequential disk I/O in the HDD era,
but has inherent limitations on modern hardware:

**Performance bottlenecks:**
1. **No bloom filter** — every negative lookup (non-existent term) pays the full
   7-level traversal cost. In real workloads, negative lookups can dominate.
2. **3-file cache pressure** — each lookup jumps across 3 independent virtual
   address spaces (.ssdat -> .spdat -> .pdat), causing TLB and cache misses.
3. **O(log N) string comparisons** — binary search at each level compares full
   strings, which is expensive for long keys (URLs, paths).
4. **No prefix sharing** — each page stores terms with LCP compression, but
   there's no structural sharing of common prefixes across pages.

## 2. What is NLT?

A **Nested Louds Trie** is a succinct trie data structure that encodes the tree
topology in a bit vector using LOUDS (Level-Order Unary Degree Sequence):

```
For each node visited in BFS order:
  write d ones (d = number of children), then one zero
```

This gives **~2 bits per node** for the tree structure. Combined with:
- **Path compression (zpath)**: chains of single-child nodes collapsed into
  multi-byte labels, critical for URLs/paths
- **Adaptive child lookup**: bitmap (O(1) via popcount) for high-fanout nodes,
  linear scan for low-fanout nodes
- **O(1) rank via two-level directory**: superblock (2048-bit) + block (64-bit)
  cumulative popcount cache

The result: a dictionary that is **2-3x faster** and uses **50-75% less memory**
than PageDict4.

## 3. Implementation Architecture

All code is Apache 2.0 licensed, designed from scratch for Vespa. The algorithm
is inspired by academic LOUDS trie literature, but the implementation is original.

### Layer 1: `vespalib/succinct/rank_select.{h,cpp}`
Bit vector with O(1) `rank1()` and O(log N) `select0()/select1()`.
- Two-level directory: `_super_blocks` (uint32, absolute popcount per 2048 bits)
  + `_block_ranks` (uint16, relative popcount per 64-bit word)
- BMI2 `pdep` + `tzcnt` for `word_select()` on supporting hardware
- Full serialization/deserialization with backward compatibility

### Layer 2: `vespalib/succinct/louds_trie.{h,cpp}`
LOUDS-encoded trie with path compression and adaptive child lookup.
- **Precomputed navigation arrays**: `_first_child_id[]` and `_child_count[]`
  eliminate all `select0` calls from the lookup hot path
- **Bitmap threshold = 36**: nodes with >= 36 children use 256-bit bitmap +
  popcount for O(1) child lookup
- **Linear scan for small fanout**: nodes with <= 16 children use sequential
  scan with early exit (faster than binary search due to branch prediction)
- **`match_zpath_fast()`**: combined `has_zpath` test + `rank1` + `memcmp`
  in a single inline method, avoiding redundant rank computations

### Layer 3: `vespalib/succinct/nested_louds_trie.{h,cpp}`
Dictionary layer: maps string keys to ordinals (0-based lexicographic rank).
- Terminal node tracking via `_is_term` bit vector with rank
- Bidirectional mapping: `_node_to_ord[]` (BFS rank -> lex ordinal) and
  `_ord_to_node[]` (lex ordinal -> BFS node id)
- DFS iterator for sequential scan (fusion)

### Layer 4: `searchlib/diskindex/nlt_dictionary.{h,cpp}`
Integration with Vespa's disk index infrastructure.
- `NltDictionaryFileSeqWrite`: builds NLT from sorted term stream
- `NltDictionaryFileSeqRead`: sequential iterator for fusion
- `NltDictionaryFileRandRead`: mmap-backed random lookup by term string

### Zero-Copy mmap Support
All three data structure layers support `setup_mmap()`: internal arrays are
backed by `const T*` pointers that can point into either owned `std::vector`
storage (build/load mode) or directly into mmap'd memory (zero-copy mode).
- `NltDictionaryFileRandRead` uses `FastOS_File::enableMemoryMap()` with
  configurable flags (MAP_POPULATE, MAP_HUGETLB) and fadvise options,
  matching PageDict4's mmap pattern exactly
- Navigation arrays (`_first_child_id`, `_child_count`) are always computed
  at load time — not serialized, so they remain owned vectors
- Custom move constructors handle pointer fixup between owned and mmap modes

### Tests and Benchmarks
- `vespalib/src/tests/succinct/nlt_test.cpp`: correctness tests
- `realistic_benchmark.cpp`: NLT vs 3-file 7-level PD4 simulation with
  flush and fusion benchmarks, scales up to 5M terms

## 4. Optimization History and Results

### Round 1: Baseline Implementation
Initial LOUDS trie with naive rank/select. NLT was **1.8x slower** than PD4
on positive lookups due to expensive rank1() and select0() calls.

### Round 2: O(1) rank1 + BMI2 word_select
Replaced linear-scan rank with two-level superblock+block directory. Added
`pdep`+`tzcnt` for select within a word.
- **Result: 1.5x speedup** on all lookups
- NLT now competitive with PD4 (1.3x slower positive, 1.1x slower negative)

### Round 3: select0 Elimination
Combined the two `select0` calls in `find_child` into one, and used `match_zpath_fast`
to avoid redundant `rank1` in zpath matching.
- **Result: additional 1.5x speedup**
- NLT now **faster than PD4** on most workloads

### Round 4: Precomputed Navigation Arrays
Eliminated `select0` entirely from the lookup path by precomputing `_first_child_id`
and `_child_count` arrays during BFS build.
- **Result: additional 2x speedup**
- NLT now **2-3x faster** than PD4

### Round 5: Builder and Lookup Refinements
- Linear scan for small fanout (<=16 children) instead of binary search
- Eliminated `std::sort` in Builder (input is pre-sorted)
- Computed navigation arrays during BFS instead of N x select0 calls
- **Result: build time reduced from 2.5x to 1.3x vs PD4 at 5M terms**

### Final Benchmark Results (3-file realistic PD4 simulation)

**Lookup Latency (ns/op):**

| Scale | PD4 Pos | NLT Pos | **Speedup** | PD4 Neg | NLT Neg | **Speedup** |
|-------|---------|---------|-------------|---------|---------|-------------|
| 100K  | 379     | 169     | **2.2x**    | 396     | 147     | **2.7x**    |
| 500K  | 622     | 281     | **2.2x**    | 562     | 211     | **2.7x**    |
| 1M    | 798     | 357     | **2.2x**    | 681     | 235     | **2.9x**    |
| 2M    | 933     | 462     | **2.0x**    | 769     | 266     | **2.9x**    |
| 5M    | 1137    | 630     | **1.8x**    | 949     | 329     | **2.9x**    |
| 1M URL| 1042    | 420     | **2.5x**    | 193     | 18      | **10.7x**   |

**Memory Usage:**

| Scale | PD4 | NLT | **Ratio** |
|-------|-----|-----|-----------|
| 500K  | 65 MB | 17 MB | **0.26x** |
| 1M    | 130 MB | 34 MB | **0.26x** |
| 5M    | 652 MB | 162 MB | **0.25x** |
| 1M URL| 130 MB | 43 MB | **0.33x** |

**Flush (Build) Performance:**

| Scale | PD4 | NLT | Ratio |
|-------|-----|-----|-------|
| 1M    | 240 ms | 452 ms | 1.9x |
| 5M    | 1751 ms | 2363 ms | 1.3x |

**Fusion (K-way Merge) Performance:**

| Scenario | PD4 | NLT | Ratio |
|----------|-----|-----|-------|
| 5 x 500K | 444 ms | 1326 ms | 3.0x |
| 20 x 100K | 422 ms | 1095 ms | 2.6x |

### Why NLT Wins on Lookups

1. **Single contiguous memory region** vs PD4's 3 separate mmap files —
   fewer TLB misses, better cache locality
2. **~4 cache lines per lookup** vs PD4's ~7 cache lines across 3 regions
3. **Prefix sharing** — common prefixes stored once in the trie, not
   repeated across pages
4. **O(1) child lookup** for high-fanout nodes via bitmap + popcount
5. **Path compression** — URL-like terms skip entire prefix in one memcmp

### Known Weaknesses and Future Work

1. **Sequential scan 8-14x slower** — DFS iterator requires stack-based
   traversal. Fix: pre-compute DFS order array for linear scan.
2. **Flush 1.3-1.9x slower** — trie construction from sorted stream.
   Fix: if MemTable uses a trie (e.g., CSPP Patricia), flush can directly
   BFS-traverse the in-memory trie without rebuilding.
3. **Fusion 2.6-3.0x slower** — dominated by NLT build cost.
   Same fix as flush: trie-to-trie conversion.
4. **Disk size 1.4x larger for random terms** — fixed offset entries.
   Fix: delta-coded variable-length offsets.
5. ~~No mmap I/O~~ — **Implemented**: zero-copy `setup_mmap()` across all layers,
   `NltDictionaryFileRandRead` uses `FastOS_File::enableMemoryMap()` matching PD4's pattern.

## 5. Acknowledgments

This exploration was initiated, directed, and driven by **@robbinfan**, who
provided critical architectural insights throughout the process:

> **"你得和PageDict4对比，尤其是数据很多情况下的内存占用，磁盘占用，查询性能，和fusion速度。"**
> *(You must compare with PageDict4, especially under large data: memory, disk,
> query performance, and fusion speed.)*

This directive established the benchmark-driven methodology that guided all
optimization work. Without concrete PageDict4 comparison data, we would not
have known that NLT was initially 1.8x slower and needed 4 rounds of optimization.

> **"PageDict4这套是可能到7层，所以你看看构造下大规模数据下，多个dict文件，跳来跳去的性能"**
> *(PageDict4 can have up to 7 levels, so test with large-scale data, multiple
> dict files, jumping between them)*

This insight revealed that the original sorted-array simulation was too
favorable to PageDict4. The realistic 3-file simulation showed NLT's cache
advantage is even larger than initially measured — PD4 lookups are 30-50%
slower with realistic multi-file jumping.

> **"这个是immutable的，flush和fusion也很关键"**
> *(This is immutable — flush and fusion are also critical)*

This refocused the benchmark to include build and merge performance, leading
to the Builder optimization that reduced flush overhead from 2.5x to 1.3x.

> **"你看pd4这些都是走mmap的，nlt当然也得啊"**
> *(PD4 uses mmap for everything — NLT must too, obviously.)*

This drove the implementation of zero-copy mmap support across all data
structure layers, with `setup_mmap()` pointing directly into mmap'd memory
without any memcpy, matching PageDict4's `FastOS_File::enableMemoryMap()` pattern.

> **"如果memory index的dict也用类似的trie，是不是就快了？我在另一个session尝试用CSPP在搞"**
> *(If the memory index dict also uses a trie, wouldn't flush be faster?
> I'm working on CSPP in another session.)*

This identified the key architectural insight for the next phase: if the
MemTable uses a CSPP Patricia trie, flush can directly BFS-traverse it to
emit LOUDS encoding, eliminating the entire trie reconstruction phase and
bringing flush performance to parity with PageDict4.

---

*Project: Vespa NLT Disk Index Dictionary Exploration*
*Branch: `claude/vespa-nlt-exploration-SPvJF`*
*Date: 2026-03-26*
