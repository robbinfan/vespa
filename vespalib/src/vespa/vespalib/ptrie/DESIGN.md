# PatriciaTrie (ptrie) — Design Document

## Overview

A Vespa-customized concurrent Patricia trie designed to replace the B-tree
(`vespalib::btree::BTree`) as the dictionary structure in Vespa's memory index.
Inspired by [topling/CSPP](https://github.com/topling/cspp-memtable) design
principles, adapted to Vespa's single-writer-multi-reader (SWMR) architecture.

## Motivation

### Current B-tree bottlenecks

1. **Write hotspot**: B-tree root and upper internal nodes are touched by
   every insert. Single-writer serialization via `ISequencedTaskExecutor`
   limits write throughput.

2. **COW overhead per node**: Each modification may trigger O(log₁₆N) node
   copies (frozen node → new copy), amplified by the 16-slot branching factor.

3. **Hold list pressure**: High read concurrency delays generation-based
   reclamation, causing memory bloat.

### Why Patricia trie

- **No write hotspot**: Different keys with different prefixes modify
  different subtrees — zero contention between branches. This is CSPP's
  key insight for achieving linear multi-core scaling.

- **Path compression**: Shared prefixes are stored once. Typical text index
  vocabularies have high prefix sharing (e.g., "search", "searching",
  "searched" share "search").

- **Natural byte order**: Trie structure inherently provides lexicographic
  iteration — no comparator overhead.

- **O(key_length) operations**: Insert/find/remove are proportional to key
  length, not tree size. For typical terms (5-20 bytes), this is very fast.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    PatriciaTrie                       │
│                                                       │
│  _root (atomic<uint32_t>) ──────────────┐            │
│  _frozen_root (atomic<uint32_t>) ──┐    │            │
│                                    │    │            │
│  ┌──────────────────────────────── ▼ ── ▼ ──────┐   │
│  │              NodeArena (append-only)          │   │
│  │                                                │   │
│  │  [sentinel] [node1] [node2] [node3] ...       │   │
│  │   offset=0   offset=16  offset=48  ...        │   │
│  │                                                │   │
│  │  Nodes are IMMUTABLE once written.             │   │
│  │  COW creates new nodes; old become dead space. │   │
│  └────────────────────────────────────────────────┘   │
│                                                       │
│  Writer:  relaxed store to _root                      │
│  Readers: acquire load from _frozen_root              │
│  freeze(): release store _root → _frozen_root         │
└─────────────────────────────────────────────────────┘
```

### Node Layout

Nodes are variable-length records in the arena:

```
┌──────────────────┐
│   NodeHeader     │  16 bytes
│   (flags, prefix_len, num_children, value)
├──────────────────┤
│   prefix bytes   │  prefix_len bytes (4-byte aligned)
├──────────────────┤
│   ChildEntry[0]  │  8 bytes each
│   ChildEntry[1]  │  (sorted by key byte)
│   ...            │
│   ChildEntry[N]  │
└──────────────────┘
```

**NodeHeader** (16 bytes):
- `flags`: HAS_VALUE (0x01), IS_BRANCH (0x02)
- `prefix_len`: compressed prefix length
- `num_children`: 0 for leaf, 1-255 for branch
- `value`: stored uint32_t (PostingListPtr)

**ChildEntry** (8 bytes):
- `key`: single branch byte
- `node_offset`: 32-bit offset of child in arena

### Key Design Decisions

#### 1. Arena-based append-only allocation

Like CSPP's memory pool:
- All nodes allocated sequentially in a `std::vector<char>`
- 32-bit offsets instead of 64-bit pointers (saves 50% on references)
- Atomic bump pointer for allocation (future: CAS-based concurrent alloc)
- No per-node malloc/free overhead

Why append-only matters: readers traversing the trie will only ever access
data at offsets less than what was allocated when they started. Since we
never modify written data, readers see a consistent snapshot without locks.

#### 2. Copy-on-write (COW) mutations

Adapted from CSPP's COW semantics:
- Insert: create new nodes along the modified path, atomically swap root
- Remove: same COW path + path compression when branches collapse
- Old nodes become dead space, tracked via `_dead_bytes`
- Dead space reclaimed through generation-based hold lists (compatible with
  Vespa's existing `GenerationHandler`)

#### 3. Atomic root for SWMR

Compatible with Vespa's existing SWMR pattern:
- `_root`: writer's current root (relaxed access)
- `_frozen_root`: snapshot visible to readers (release/acquire)
- `freeze()`: atomically publishes current root to readers
- `FrozenView`: captures frozen root for read-only traversal

#### 4. Sorted sparse children

For text index vocabularies:
- Most trie nodes have 1-10 children (not 256)
- Children stored as sorted `ChildEntry[]` array
- Linear scan for ≤8 children, binary search for more
- Future optimization: ART-style adaptive node sizes (Node4/16/48/256)

### Concurrency Model

**V1: Single-writer, multi-reader (`insert`)**
- Matches existing Vespa memory index model
- Writer: relaxed memory order for arena writes and root update
- Readers: acquire load of frozen root, then traverse immutable data

**V2: Multi-writer via root-level CAS (`casInsert`)**
- Arena allocation: CAS bump pointer (lock-free, wait-free for each thread)
- Tree modification: optimistic COW + CAS at root pointer
- Algorithm:
  1. Load current root (acquire)
  2. Build new COW path with key inserted (`insertImpl` with `skip_mark_dead`)
  3. CAS root from old→new (release/acquire)
  4. On success: account dead bytes, increment entry count
  5. On failure: all newly created arena nodes are wasted (dead), retry from step 1
- Arena must be pre-sized via `reserveArena()` before concurrent use (vector
  resize is not thread-safe)
- Different-prefix writes rarely contend at root (each modifies a different
  subtree, but both need to swap root). Retry is fast because the second
  attempt reads the winner's new root and builds on it.
- Same-key writes: exactly one wins the CAS, the other sees the key exists

**Concurrency guarantees:**
- `find()` is lock-free and can run concurrently with any number of `casInsert()` calls
- `casInsert()` is lock-free (CAS retry, no blocking)
- `freeze()` + `getFrozenView()` provide snapshot isolation for readers
- `remove()` is single-writer only (not CAS-safe)

**Future: Sub-tree level CAS**
- CAS at child pointers instead of root (less contention)
- Requires node layout changes (atomic child offsets or 256-way branches)
- Would eliminate root bottleneck for truly independent branches

## Memory Management

### Dead space tracking

```
insert("hello", 1)  →  arena: [sentinel][leaf:"hello"=1]
insert("hello", 2)  →  arena: [sentinel][leaf:"hello"=1][leaf:"hello"=2]
                                          ^^^DEAD^^^       ^^^LIVE^^^
```

Each COW operation creates new nodes and marks old ones as dead. Dead bytes
are tracked per-generation via hold lists:

1. COW marks old node's bytes in `_dead_bytes`
2. `transferHoldLists(gen)` moves `_dead_bytes` to hold list with generation
3. `trimHoldLists(used_gen)` removes entries where `gen < used_gen`

### Arena compaction

When dead space exceeds 25% of arena usage, `compact()` rebuilds the trie:
1. Deep-copy all live nodes (reachable from current root) into a fresh arena
2. Recursively copy children first to get new offsets, then allocate parent
3. Swap old/new arenas via `NodeArena::swapWith()` (handles non-movable atomics)
4. Reset dead bytes, hold list, frozen root

**Single-writer only** — must not run concurrently with inserts. Readers via
frozen view are safe: they read the old arena until the next freeze() + generation
cycle.

**Results (N=1M):**
- Before: 540 MB arena, 553 B/entry, 96% dead
- After: 23 MB arena, **24.0 B/entry**, 0% dead
- Reclaimed: 517 MB in 208 ms
- Post-compact lookup: 308 ns (37% faster due to better cache locality)

## Comparison with B-tree

| Aspect | BTree | PatriciaTrie |
|--------|-------|-------------|
| Lookup | O(log₁₆N) comparisons | O(key_len) byte ops |
| Insert | O(log₁₆N) + node splits | O(key_len) + COW path |
| Memory per key | ~64B (node slots + overhead) | ~24B + key_len (header + prefix + child) |
| Write hotspot | Root always touched | Only shared-prefix path |
| Iteration order | Requires comparator | Natural byte order |
| Missing key detect | Full tree traversal | Early termination at first mismatch |

## Benchmark Data

### V1 — Baseline (commit e159589c)

Environment: Linux 6.18.5, g++ -O2 -std=c++17, single socket.

#### Single-thread performance (ns/op)

| Operation | N=10K | N=100K | N=1M | std::map 1M | unordered_map 1M |
|-----------|-------|--------|------|-------------|-----------------|
| seq insert | 1194 | 1248 | 1568 | 277 | — |
| random insert | 1276 | 1831 | 3069 | 1308 | 367 |
| **lookup hit** | 175 | 365 | **706** | 1692 | 397 |
| **lookup miss** | 10 | 7 | **6** | 77 | 465 |
| frozen lookup | 155 | 337 | 700 | — | — |
| iteration | 32 | 85 | 97 | 192 | — |

Key ratios at N=1M:
- Lookup hit: **PTrie 2.4x faster than std::map**, 1.8x slower than hash
- Lookup miss: **PTrie 12x faster than std::map, 77x faster than hash**
- Insert: PTrie 2.3x slower than std::map (COW overhead)

#### Memory (N=1M)

| Metric | Value |
|--------|-------|
| arena total | 540 MB |
| bytes/entry | 553 B |
| dead bytes | 517 MB (96%) |

Note: 96% dead is expected without generation trimming — each COW insert
creates a full new path and marks old nodes dead. In production with
`GenerationHandler`, dead bytes are reclaimed periodically.

#### CAS multi-writer insert (ns/op, N=1M, root-level CAS)

| Threads | ns/op | arena (KB) | dead (KB) | Speedup vs 1T |
|---------|-------|------------|-----------|---------------|
| 1 (CAS) | 844 | 540,365 | 516,927 | 1.0x |
| 1 (insert) | 2,441 | 540,365 | 516,927 | — |
| 2 | 2,134 | 1,085,154 | 2,124,748 | 0.40x |
| 4 | 24,992 | 2,043,489 | 7,855,601 | 0.03x |

**Root-level CAS bottleneck**: 4 threads = 30x slower than 1 thread.
Every insert rebuilds full path and CAS at root. CAS failures waste
entire paths (dead bytes explode from 517MB to 7.8GB).

Fix: sharded CAS (V2) or sub-tree CAS (V3).

### V2 — Sharded CAS (commit TBD)

256 sub-tries keyed by first byte. Each shard has independent root and arena.
CAS contention eliminated for keys with different first bytes.

#### Sharded CAS insert (ns/op, N=1M)

| Threads | root CAS | sharded CAS | **Speedup** | root dead | sharded dead |
|---------|----------|-------------|-------------|-----------|-------------|
| 1 | 1,006 | 839 | 1.2x | 818 MB | 330 MB |
| 2 | 1,803 | **531** | **3.4x** | 2,414 MB | 377 MB |
| 4 | 6,326 | **315** | **20x** | 11,802 MB | 388 MB |

Key findings:
- **4-thread sharded: 315 ns/op** — 20x faster than root CAS (6,326 ns)
- **4 threads faster than 1 thread** (315 vs 839) — true multi-core scaling
- Dead bytes: 11.8GB → 388MB (**30x reduction**) — near-zero CAS retry waste
- Sharded lookup: 498 ns/op, comparable to single-trie (616 ns)

#### Sharded lookup (ns/op, N=1M)

| Operation | single trie | sharded trie |
|-----------|-------------|-------------|
| lookup hit | 616 | 498 |

Sharded lookup is actually faster due to smaller per-shard trees (better cache).

### V3 — SBO + Iterative Insert (commit TBD)

Key optimizations:
1. **Stack-based child arrays**: Replaced `std::vector<ChildEntry>` in `withChild`,
   `withoutChild`, `withoutValue` with stack-allocated `ChildEntry buf[256]`.
   Eliminates heap allocation on every COW node rebuild.
2. **Iterative insert**: Converted recursive `insertImpl` to iterative two-phase
   (descent + bottom-up rebuild). Eliminates ~400-byte `NodeSnapshot` stack
   frames per recursion level.
3. **Copy-patch withChild**: For the common case of replacing an existing child
   (same child count), clone the entire node via single memcpy and patch one
   4-byte child offset. Avoids separate header/prefix/children snapshot+copy.
4. **SBO for merged prefix buffers**: Stack-allocated `char[128]` for path
   compression merge buffers, falling back to heap for very long prefixes.

#### Single-thread performance (ns/op)

| Operation | V2 (N=1M) | V3 (N=1M) | **Speedup** | std::map 1M |
|-----------|-----------|-----------|-------------|-------------|
| seq insert | 3,542 | **928** | **3.8x** | 270 |
| random insert | 4,829 | **1,969** | **2.5x** | 710 |
| lookup hit | 526 | **543** | 1.0x | 856 |
| lookup miss | 6 | **6** | 1.0x | 56 |
| frozen lookup | 538 | **498** | 1.08x | — |
| iteration | 55 | **55** | 1.0x | 85 |

Key ratios at N=1M:
- Seq insert: PTrie **3.4x slower** than std::map (was 13.7x in V2)
- Random insert: PTrie **2.8x slower** than std::map (was 6.6x in V2)
- Lookup hit: PTrie **1.6x faster** than std::map
- Lookup miss: PTrie **9x faster** than std::map

#### Sharded CAS insert (ns/op, N=1M)

| Threads | root CAS V3 | sharded CAS V3 | Speedup |
|---------|-------------|-----------------|---------|
| 1 | 518 | 408 | 1.3x |
| 2 | 1,658 | **507** | **3.3x** |
| 4 | 5,151 | **323** | **16x** |

CAS performance improved ~50% from V2 due to faster per-insert COW operations.
4-thread sharded CAS (323 ns/op) is faster than 1-thread (408 ns/op) — true scaling.

### V4 — Arena Compaction + BTree Comparison

Arena compaction eliminates 96% dead space from COW mutations.
BTree-str is a self-contained B-tree (ORDER=16) with string-ref comparator
modeling Vespa's `BTree<WordKey, PostingListPtr, KeyComp>`.

#### PTrie vs BTree-str (N=1M, single-thread)

| Operation | PTrie | PTrie (post-compact) | BTree-str | PTrie advantage |
|-----------|-------|---------------------|-----------|-----------------|
| seq insert | 904 ns | — | 134 ns | 6.7x slower |
| random insert | 1,860 ns | — | 172 ns | 10.8x slower |
| lookup hit | 489 ns | **308 ns** | 622 ns | **2.0x faster** |
| lookup miss | 6 ns | 6 ns | 120 ns | **20x faster** |
| memory/entry | 553 B (96% dead) | **24.0 B** | 79.6 B | **3.3x smaller** |

#### Key takeaways

1. **Lookup is PTrie's strength**: 2x faster hit, 20x faster miss vs BTree-str.
   Trie's O(key_len) byte comparison beats B-tree's O(log₁₆N) string comparisons.

2. **Insert is PTrie's weakness**: 6-10x slower due to COW path copy overhead.
   Every insert allocates new nodes for the entire root-to-leaf path.
   B-tree modifies nodes in-place (or with minimal frozen-node copying).

3. **Memory after compaction is excellent**: 24 B/entry vs 80 B/entry for BTree.
   The trie's path compression is very effective for text index vocabularies.

4. **Post-compact lookup is 37% faster** than pre-compact (308 vs 489 ns)
   due to eliminated dead nodes improving cache locality.

5. **Compaction cost**: 208 ms for 1M entries (reclaiming 517 MB). This is
   acceptable as a periodic background operation.

#### Compaction across sizes

| N | Arena before | Dead % | Arena after | B/entry after | Reclaimed | Time |
|---|-------------|--------|-------------|---------------|-----------|------|
| 10K | 3.5 MB | 93% | 234 KB | 24.0 B | 3.3 MB | 0.2 ms |
| 100K | 44.7 MB | 95% | 2.3 MB | 24.0 B | 42.3 MB | 7.6 ms |
| 1M | 540 MB | 96% | 23.4 MB | 24.0 B | 517 MB | 208 ms |

### V5 — Mutable Node Optimization (In-Place Update)

Key insight: after `freeze()`, all nodes allocated by subsequent inserts are only
reachable from `_root` (mutable), not from `_frozen_root`. These "mutable" nodes
can be modified in-place without COW.

Mechanism:
- `_freeze_arena_mark` records arena size at freeze() time
- `isMutable(offset)` = offset >= `_freeze_arena_mark`
- Ascent phase: mutable ancestors get `patchChildInPlace()` (zero allocation)
- Short-circuit: once a mutable ancestor is patched, ascent stops immediately
- Only for single-writer mode; CAS always uses full COW (data race safety)

#### Single-thread performance (ns/op, N=1M)

| Operation | V4 (full COW) | V5 (mutable) | BTree-str | V5 vs BTree |
|-----------|---------------|-------------|-----------|-------------|
| seq insert | 904 | **409** | 148 | 2.8x slower |
| random insert | 1,860 | **751** | 189 | 4.0x slower |
| lookup hit | 489 | **426** | 697 | **1.6x faster** |
| lookup miss | 6 | **6** | 130 | **22x faster** |
| memory/entry | 553 B (96% dead) | **76 B (0% dead)** | 80 B | comparable |

Insert gap vs BTree: 6-10x → **2.8-4x**. Zero dead bytes in single-writer mode.

#### Batch Commit Pattern (Vespa Memory Index)

Vespa's memory index uses batch commit: freeze → [N inserts] → freeze → ...
The mutable optimization is ideal for this pattern: only the first insert per
subtree path after freeze does COW; subsequent inserts hit mutable nodes.

**Deferred freeze** reduces COW frequency by freezing every N batches instead
of every batch. For example, `deferred-50` freezes every 50 batches — 50x fewer
COW boundaries, dramatically reducing dead space and path-copy overhead.

##### N=100K

| Mode | b=100 | b=1000 | b=5000 |
|------|-------|--------|--------|
| BTree-str | 491 | 462 | 458 |
| PTrie freeze | 860 | 588 | 454 |
| PTrie deferred-10 | 556 | 443 | 388 |
| **PTrie deferred-50** | **456** | **428** | **455** |
| PTrie no-freeze | — | — | 419 (b=100K) |

##### N=1M (key benchmark — production scale)

| Mode | b=100 | b=1000 | b=5000 | Memory |
|------|-------|--------|--------|--------|
| **BTree-str** | **786** | **793** | **792** | **76 B/e** |
| PTrie freeze | 1144 | 1002 | 719 | 163-321 B/e |
| PTrie deferred-10 | 1022 | 727 | 581 | 95-226 B/e |
| **PTrie deferred-50** | **733** | **547** | **553** | **79-163 B/e** |
| PTrie no-freeze | — | — | 611 (b=1M) | 74 B/e |

**deferred-50 beats BTree at every batch size at N=1M:**
- b=100: 733 vs 786 ns (**7% faster**)
- b=1000: 547 vs 793 ns (**31% faster**)
- b=5000: 553 vs 792 ns (**30% faster**)

Memory: deferred-50 at b=5000 = 79 B/e vs BTree 76 B/e (within 4%).

**Copy-on-freeze (cof)** was evaluated but rejected: O(N) full-trie copy at each
freeze is catastrophic for small batches (396,374 ns/op at N=1M b=100). The COW +
mutable + deferred approach is strictly superior.

##### Concurrent Readers (SWMR, N=1M)

Writer inserts batches with `freeze()` after each. Reader threads do frozen lookups
concurrently.

| Config | Writer ns/op | Reader Mops/s | Writer slowdown |
|--------|-------------|---------------|-----------------|
| b=1000 writer-only | 1056 | — | — |
| b=1000 +1R | 1361 | 2.3 | 1.3x |
| b=1000 +4R | 2217 | 7.7 | 2.1x |
| b=5000 writer-only | 750 | — | — |
| b=5000 +1R | 1256 | 2.4 | 1.7x |
| b=5000 +4R | 1821 | 8.2 | 2.4x |

Reader throughput scales linearly (~2 Mops/s per reader thread). Writer slowdown
is 2-2.4x with 4 concurrent readers due to cache contention on shared arena.

#### Memory Analysis: Peak vs Final

Dead bytes from COW accumulate between compactions. Key metric is **peak memory**
(worst-case during operation), not just final:

##### N=1M peak memory (B/entry)

| Mode | b=100 | b=1000 | b=5000 |
|------|-------|--------|--------|
| **BTree-str** | **76** | **76** | **76** |
| deferred-50 | 163 (2.1x) | 95 (1.25x) | 79 (1.04x) |
| **def50+compact** | **48 (0.63x)** | **60 (0.79x)** | **79 (1.04x)** |

**def50+compact** (deferred-50 + auto-compact when dead > 50%):
- Peak memory **smaller than BTree** at b≤1000 (48-60 vs 76 B/e)
- Final memory after compaction: 24-26 B/e (**3x smaller** than BTree)
- Compaction overhead: 11 compactions at b=100, 2 at b=1000 (amortized)
- Insert speed: 891 ns at b=100, 675 ns at b=1000 (14% faster than BTree)

##### Speed vs memory trade-off (N=1M, b=1000)

| Mode | ns/op | vs BTree | Peak B/e | vs BTree |
|------|-------|----------|----------|----------|
| BTree-str | 786 | — | 76 | — |
| deferred-50 | 554 | **30% faster** | 95 | 1.25x more |
| def50+compact | 675 | **14% faster** | 60 | **21% less** |

#### Recommended Configuration for Vespa Memory Index

Based on benchmarks, the optimal PTrie configuration for replacing BTree in Vespa's
memory index dictionary:

1. **Deferred freeze (interval=50) + auto-compact**: Best balance of speed and
   memory. 14% faster insert than BTree with 21% less peak memory at b=1000.
   For latency-sensitive workloads, use plain deferred-50 (30% faster, 25% more memory).
2. **Pre-reserve arena**: `reserveArena(num_terms * 200)` to avoid resize during
   concurrent reads.
3. **Single-writer `insert()`**: Use the mutable optimization for in-place patching.
   Use `casInsert()` only if multi-writer is required (loses mutable optimization).
4. **Batch size ≥ 1000**: The sweet spot where PTrie dominates BTree on both speed
   and memory. Vespa's typical batch sizes (1000-5000) are ideal.

### V6 — RCU Overhead: BTree+DataStore vs PTrie

Vespa's BTree uses DataStore + GenerationHandler RCU for memory management:
- **Reader**: `takeGuard()` = acquire load + `fetch_add(2)` + `fetch_sub(2)` (3 atomic ops)
- **Writer**: `incGeneration()` = `seq_cst` fence + conditional allocation + hold list scan

PTrie's arena-based design eliminates all of this:
- **Reader**: Just acquire load of `_frozen_root` (1 read-only atomic, no refcount)
- **Writer**: Just release store to `_frozen_root` (no fence, no hold list)

#### Per-query RCU cost (ns/op)

| Operation | 1 thread | 4 threads | 8 threads |
|-----------|----------|-----------|-----------|
| BTree RCU: takeGuard+release | 15 | **40** | **41** |
| PTrie: acquire load | 2 | **0.5** | **0.9** |
| **Speedup** | 7.5x | **80x** | **45x** |

At 4+ threads, BTree's atomic refcount causes **cache line bouncing** —
`fetch_add(2)` invalidates the GenerationHold cache line on all cores. PTrie's
acquire load is read-only, served from L1 cache on every core.

#### Per-commit writer cost (ns/op)

| Operation | Cost |
|-----------|------|
| BTree: incGeneration (seq_cst fence) | 9 |
| BTree: hold list transfer+trim | 5 |
| **BTree total** | **14** |
| PTrie: release store | **0.8** |
| **Speedup** | **17x** |

#### Impact on query latency

Scenario: N=1M, query does 10 frozen lookups, 4 reader threads:

| Aspect | BTree + DataStore RCU | PTrie |
|--------|----------------------|-------|
| Lookup cost | 622 ns × 10 = 6220 | 308 ns × 10 = 3080 |
| RCU overhead | 40 ns × 10 = **400** | 0.5 ns × 10 = **5** |
| **Total per query** | **6620 ns** | **3085 ns** |
| RCU as % of total | 6% | 0.2% |

PTrie's total query latency is **2.1x lower**, with RCU overhead reduced from 6% to
0.2% of total time. Under heavy read contention (8+ threads), BTree's RCU overhead
can exceed 10% of query time.

#### Why PTrie doesn't need GenerationHandler for reads

BTree + DataStore requires per-query generation guards because:
1. DataStore may free buffer memory at any time after generation advances
2. Guard prevents trim from freeing buffers that readers are traversing
3. Without guard: use-after-free if writer trims during read

PTrie's arena is append-only:
1. Data at any offset is **never overwritten** — reads are always safe
2. Arena vector never shrinks — only grows (or is swapped at compaction)
3. `_frozen_root` atomic provides snapshot isolation without refcounting
4. Only compaction needs generation tracking (swap old arena → hold → free)

**Net result**: PTrie eliminates O(readers × queries) atomic operations, replacing
them with O(readers × queries) simple loads. At Vespa's query rates (10K+ QPS),
this saves millions of atomic operations per second.

## Integration Plan

### Phase 1: Standalone library (current)
- `vespalib/ptrie/patricia_trie.h` — core implementation
- Unit tests + micro-benchmarks
- Interface: `insert/find/remove/freeze/getFrozenView/begin`

### Phase 2: Dictionary adapter
- Create `PatriciaTrieDictionary` wrapping `PatriciaTrie`
- Implement `DictionaryTree`-compatible interface
- Keep `WordStore` — trie uses word strings as keys directly
- Map: `word_string → PostingListPtr`

### Phase 3: FieldIndex integration
- Replace `DictionaryTree = BTree<WordKey, PostingListPtr>` in `FieldIndex`
- Adapter translates `KeyComp` lookups to direct string lookups
- `OrderedFieldIndexInserter` adapted for trie's sorted-insert pattern

### Phase 4: Multi-writer (done — V2)
- CAS-based arena allocation (atomic bump pointer)
- Root-level CAS with optimistic retry
- `casInsert()` for concurrent writers, `find()` lock-free for readers

### Phase 5: Sub-tree CAS (future)
- CAS at child pointer level instead of root
- Requires 256-way or ART-style nodes with atomic child offsets
- Would eliminate root bottleneck for truly independent branches

## File Structure

```
vespalib/src/vespa/vespalib/ptrie/
    CMakeLists.txt
    DESIGN.md                  ← this file
    patricia_trie.h            ← public header
    patricia_trie.cpp          ← implementation

vespalib/src/tests/ptrie/
    CMakeLists.txt
    patricia_trie_test.cpp     ← unit tests
```
