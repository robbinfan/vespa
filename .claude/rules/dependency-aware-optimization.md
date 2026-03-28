---
paths:
  - "searchlib/src/vespa/searchlib/memoryindex/**"
  - "searchlib/src/vespa/searchlib/diskindex/**"
  - "searchlib/src/vespa/searchlib/attribute/**"
  - "searchlib/src/vespa/searchlib/index/**"
  - "searchcore/src/vespa/searchcore/proton/**"
  - "storage/src/vespa/storage/**"
  - "vespalib/src/vespa/vespalib/btree/**"
  - "vespalib/src/vespa/vespalib/datastore/**"
  - "vespalib/src/vespa/vespalib/util/generationhandler*"
  - "vespalib/src/vespa/vespalib/util/rcuvector*"
---

# Dependency-Aware Optimization Rules

## Core Principle

Vespa's performance-critical stack is a deep dependency chain. Optimizing one layer
without understanding its impact on layers above AND below leads to local optima that
may be global pessima.

## The Dependency Chain

There are TWO dependency chains in Vespa — the **data structure chain** (vertical)
and the **feed/query pipeline** (horizontal). Both must be considered.

### Vertical: Data Structure Layers
```
Layer 5: storage (bucket distribution, persistence SPI, merge/split)
Layer 4: searchcore/proton (DocumentDB, flush engine, fusion, feed handler)
Layer 3: searchlib (memory index, disk index, attributes, posting lists)
Layer 2: vespalib/btree (B+tree with freeze/thaw, concurrent readers)
Layer 1: vespalib/datastore (buffer management, EntryRef addressing)
Layer 0: vespalib/util (GenerationHandler RCU, RcuVector, atomic primitives)
```

### Horizontal: Feed Pipeline (write path)
```
Client → Storage (FileStorHandler, per-bucket stripe locking)
  → PersistenceEngine (SPI, write filter, handler dispatch)
    → FeedHandler (serializes to TLS, then dispatches to FeedView)
      → FeedView (3 variants, non-transactional):
        ├── Master thread: MetaStore (GID→LID, authoritative, sync)
        ├── Summary thread: DocumentStore (async, fire-and-forget)
        ├── AttributeFieldWriter: Attributes (per-field parallel, async)
        └── Index thread: MemoryIndex (single-threaded, async)
      → IndexMaintainer:
        ├── Memory index (active writes)
        ├── Flush → disk index (triggered by memory/resource pressure)
        └── Fusion (merges N disk indexes → 1)
```

### Horizontal: Distributor Pipeline (cluster management)
```
ClusterState change → TopLevelDistributor
  → DistributorStripePool (park all → update state → unpark)
    → Per-stripe: IdealStateManager recalculates bucket placement
      → Maintenance operations: MergeOp, SplitOp, JoinOp, GCOp
        → MergeThrottler (queue + active window + chain forwarding)
          → Persistence layer (actual data merge)
```

**CRITICAL**: Changes to any component in one pipeline often affect the other.
E.g., optimizing MemoryIndex insert speed increases flush frequency, which
affects IndexMaintainer fusion scheduling, which affects query latency.

## Rules by Layer

### Layer 0: GenerationHandler / RCU (vespalib/util)

**What you must understand before touching this layer:**
- GenerationHandler uses LSB-as-validity-bit trick in refcount atomics
- Guard acquisition is lock-free (fetch_add on refcount)
- `updateFirstUsedGeneration()` walks hold list — O(n) in number of generations
- Memory ordering: seq_cst only for validity flip, relaxed for guard increment

**Benchmark requirements:**
- Guard acquisition/release throughput under N concurrent readers
- Generation advancement rate vs reader stall (hold list growth)
- Memory reclamation latency (time from dealloc to actual free)
- Impact on L1 cache: guard hot in reader's cache line, writer bounces it

**Upstream impact check:**
- BTree node allocation/deallocation rate depends on generation trimming speed
- DataStore buffer compaction blocked by long-lived guards
- Slow generation trimming → unbounded memory growth in ALL upper layers

### Layer 1: DataStore (vespalib/datastore)

**What you must understand:**
- Buffer states: FREE → ACTIVE → HOLD → FREE
- EntryRef = 32-bit (buffer_id + offset), limits addressable space
- Compaction: copies live entries to new buffer, old buffer enters HOLD
- Hold list trimmed by GenerationHandler (Layer 0)

**Benchmark requirements:**
- Allocation throughput for different entry sizes
- Compaction overhead (copy cost + generation hold time)
- Buffer utilization ratio (live bytes / allocated bytes)
- Dead space accumulation pattern over insert/delete cycles

**Upstream impact check:**
- BTree node store performance directly depends on DataStore allocation speed
- WordStore, FeatureStore, PostingListStore in memory index all use DataStore
- Compaction pauses propagate to all concurrent readers

### Layer 2: BTree (vespalib/btree)

**What you must understand:**
- Copy-on-write with freeze/thaw semantics
- Two release fences during freeze: after nodes frozen, after roots updated
- BTreeNodeAllocator manages freeze/hold queues
- Frozen root published via atomic store with release semantics
- Readers load frozen root with acquire semantics — zero contention

**Benchmark requirements:**
- Insert/lookup/iterate throughput at different tree sizes (1K to 100M)
- Freeze cost (proportional to number of modified nodes)
- Memory amplification from copy-on-write (worst case: full path copy)
- Scan performance: iterator vs apply() functor (existing bench: btree_scan_speed_test)
- Concurrent stress: writer + N readers (existing bench: btree_stress_test)

**Upstream impact check:**
- Memory index dictionary is a BTree (word_ref → posting_ref)
- Each posting list is a BTree (doc_id → feature_ref)
- DocumentMetaStore's GID-to-LID mapping uses BTree
- Attribute indexes (EnumStore) use BTree

### Layer 3: Searchlib Components

**Memory Index** (`searchlib/memoryindex/`):
- FieldIndex contains: WordStore + dictionary BTree + PostingListStore + FeatureStore
- All backed by DataStore, all coordinated by single GenerationHandler
- Document inverter is the hot write path

**Benchmark requirements beyond standard dimensions:**
- Inversion throughput: documents/sec at varying field count and length
- Dictionary growth: word count vs memory vs lookup time
- PostingList update cost: append vs btree split
- Interaction with flush: memory index size at flush trigger vs flush duration

**Disk Index** (`searchlib/diskindex/`):
- Fusion merges N disk indexes into 1
- PostingListFile uses sequential I/O for reading

**Benchmark requirements:**
- Fusion throughput: entries/sec, I/O bandwidth utilized
- Fusion memory overhead (temporary buffers, merge state)
- Read performance: posting list iteration speed from disk (mmap vs direct I/O)

### Layer 4: Searchcore/Proton

#### 4a: FeedView Pipeline (write path)

**Architecture**: Non-transactional, eventually-consistent via TLS replay.

Three FeedView variants form an inheritance chain:
```
StoreOnlyFeedView (MetaStore + DocumentStore)
  └─ FastAccessFeedView (+Attributes via AttributeFieldWriter)
       └─ SearchableFeedView (+Index via IndexWriter on Index thread)
```

**Threading model** — a single Put touches 4+ threads:
| Thread | Writes to | Sync? | Error handling |
|--------|-----------|-------|----------------|
| Master | MetaStore (GID→LID) | Blocking | Fatal — throws, aborts operation |
| Summary | DocumentStore | Async | Silent — no rollback, doc unretrievable |
| AttrFieldWriter | Attributes (per-field) | Async | Silent — partial attribute state |
| Index | MemoryIndex | Async | Silent — doc not searchable |

**Critical synchronization**:
- `_pendingLidsForDocStore.waitComplete(lid)` — prevents update from reading stale
  document content during reconstruction (update reads doc from store, applies delta)
- `forceCommit()` coordinates across all threads: attributes → summary → index freeze
- LID reuse delayed via `_lidReuseDelayer` to protect readers holding old generation guards

**What you must understand:**
- MetaStore is the ONLY authoritative subsystem. Others can fail silently.
- Updates reconstruct full document on Shared thread pool, then distribute to index/attrs
- Test-and-set checked on Master thread (sync), but SPI dispatch is async — race window exists
- No rollback: if docstore write fails, document "exists" in MetaStore+Attrs+Index but has no content

**Benchmark AND correctness requirements:**
- Feed throughput: puts/sec through each FeedView variant at varying doc sizes
- Update throughput: updates/sec including document reconstruction cost
- forceCommit latency: time to commit across all threads (limits visibility delay)
- Partial failure recovery: inject failures in each subsystem, verify TLS replay restores consistency
- Concurrent feed: rapid put+update+remove to same LID, verify final state matches timestamp ordering

#### 4b: IndexMaintainer (index lifecycle)

**Architecture**: Manages memory→disk→fusion lifecycle.

```
IndexMaintainer
  ├─ Current memory index (receiving live writes)
  ├─ Flushing memory index (being written to disk, no new writes)
  ├─ N disk indexes (completed flushes, read-only)
  └─ Fusion (merging N disk indexes → 1, background)
```

**What you must understand:**
- Flush creates a NEW memory index, atomically swaps IndexCollection
- Old memory index becomes read-only, flushed to disk in background
- During flush: queries see union of (new memory index + flushing memory index + disk indexes)
- Fusion only touches completed disk indexes, not the active memory index
- IndexCollection swap is atomic via shared_ptr replacement

**Benchmark AND correctness requirements:**
- Flush trigger latency: time from trigger to new memory index accepting writes
- Memory index switch: verify zero document loss during atomic swap
- Concurrent feed + flush: documents arriving during flush go to NEW memory index
- Concurrent feed + fusion: documents not affected (fusion only on disk indexes)
- Query coverage during transitions: verify queries see ALL documents at all times
- Fusion throughput: entries/sec, I/O bandwidth, memory overhead

#### 4c: Flush Engine

**Flush Engine** (`searchcore/proton/flushengine/`):
- FlushStrategy selects flush targets based on memory pressure
- ThreadedFlushTarget executes flush asynchronously
- Flush blocks new memory index allocation until complete

**Benchmark requirements:**
- Flush latency vs memory index size
- Feed throughput degradation during flush
- Fusion trigger frequency and duration under sustained feed
- Recovery time: TLS replay speed after restart

### Layer 5: Storage

#### 5a: Persistence Pipeline

**Persistence** (`storage/persistence/`):
- FileStorHandler maintains N stripes (bucket_hash % num_stripes)
- Per-bucket exclusive locking for writes, shared locking for reads
- Operations dispatched to PersistenceEngine (Proton) via SPI async interface
- Throttled by SharedOperationThrottler (soft limit, not hard)

**Benchmark requirements:**
- Put/Get/Remove throughput through full SPI stack
- Stripe contention: throughput vs number of stripes vs bucket distribution
- Throttler behavior under burst load (rejection rate, recovery curve)
- Per-bucket sequencing overhead under high-cardinality bucket counts

**Correctness requirements:**
- Per-bucket operation ordering preserved through stripe → SPI → handler chain
- Split/join remaps in-flight operations correctly (no lost operations)
- FileStorHandler lock ordering: no deadlocks during split (sorted bucket order)

#### 5b: Distributor and Merge

**Distributor** (`storage/distributor/`):
- TopLevelDistributor with N stripes (each owns disjoint bucket subset)
- IdealStateManager triggers maintenance: merge, split, join, GC
- MergeThrottler prevents merge storms (active window + queue + chain forwarding)

**Benchmark requirements:**
- Merge throughput: merges/sec at varying replica divergence
- Throttler behavior: queue depth, active window utilization, BUSY rejection rate
- Stripe coordination overhead: park/unpark latency during state changes
- State change propagation time: cluster state → all stripes updated

**Correctness requirements:**
- Merge chain correctness: partial chain failure → full unwind, no partial merge
- Source-only copy: mutation detection prevents data loss
- Cluster state version: outdated merges aborted, not partially applied
- Unordered chaining: no deadlock when two nodes have full throttle windows
- Back-pressure: recovery after RESOURCE_EXHAUSTED → merges resume normally
- Long-offline node: rejoining triggers correct merge to restore redundancy

## Optimization Workflow

1. **Identify the layer** you're optimizing (vertical AND horizontal position)
2. **Read one layer down** to understand what you depend on
3. **Read one layer up** to understand who depends on you
4. **For feed path changes**: also check horizontal neighbors (e.g., FeedView → IndexMaintainer)
5. **Benchmark at your layer** with standard dimensions (see benchmark-harness.md)
6. **Run correctness checks** for the applicable C-dimensions (see benchmark-harness.md C1-C6)
7. **Benchmark one layer up** to verify no upstream regression
8. **Check layer 0 (RCU)** if your change affects allocation/deallocation patterns

## Common Cross-Layer Pitfalls

| Change | Unexpected Impact |
|--------|-------------------|
| Faster btree insert | More frequent freeze → more copy-on-write → more DataStore alloc pressure |
| Smaller DataStore buffers | Less memory but more buffer switches → cache misses in readers |
| Aggressive compaction | Lower memory but compaction pauses block all readers holding old guards |
| Larger memory index before flush | Less flush frequency but longer flush duration → bigger feed hiccup |
| Optimized posting list iteration | May bottleneck on disk I/O if data not in page cache |
| Lock-free optimization in storage | May increase CPU cost from CAS retries under contention |
| Faster feed throughput | More memory index churn → more flush/fusion → more I/O |
| More AttributeFieldWriter threads | Better attribute parallelism but more forceCommit coordination cost |
| Optimized MetaStore operations | Can unmask downstream bottleneck in async docstore/attr/index writes |
| Faster merge execution | More aggressive redistribution → higher network/disk load on recovering nodes |
| Reduced merge throttle window | Lower resource usage but slower convergence after node failure |
| Optimized split/join | May trigger more frequent bucket rebalancing → more merge operations |
