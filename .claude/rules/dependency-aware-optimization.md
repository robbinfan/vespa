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

```
Layer 5: storage (bucket distribution, persistence SPI, merge/split)
Layer 4: searchcore/proton (DocumentDB, flush engine, fusion, feed handler)
Layer 3: searchlib (memory index, disk index, attributes, posting lists)
Layer 2: vespalib/btree (B+tree with freeze/thaw, concurrent readers)
Layer 1: vespalib/datastore (buffer management, EntryRef addressing)
Layer 0: vespalib/util (GenerationHandler RCU, RcuVector, atomic primitives)
```

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

**Persistence** (`storage/persistence/`):
- FileStorHandler queues operations per bucket
- Throttled by shared_operation_throttler
- Bucket split/merge/GC are background maintenance ops

**Benchmark requirements:**
- Put/Get/Remove throughput through full SPI stack
- Throttler behavior under burst load
- Bucket split/merge impact on concurrent operations

## Optimization Workflow

1. **Identify the layer** you're optimizing
2. **Read one layer down** to understand what you depend on
3. **Read one layer up** to understand who depends on you
4. **Benchmark at your layer** with standard dimensions (see benchmark-harness.md)
5. **Benchmark one layer up** to verify no upstream regression
6. **Check layer 0 (RCU)** if your change affects allocation/deallocation patterns

## Common Cross-Layer Pitfalls

| Change | Unexpected Impact |
|--------|-------------------|
| Faster btree insert | More frequent freeze → more copy-on-write → more DataStore alloc pressure |
| Smaller DataStore buffers | Less memory but more buffer switches → cache misses in readers |
| Aggressive compaction | Lower memory but compaction pauses block all readers holding old guards |
| Larger memory index before flush | Less flush frequency but longer flush duration → bigger feed hiccup |
| Optimized posting list iteration | May bottleneck on disk I/O if data not in page cache |
| Lock-free optimization in storage | May increase CPU cost from CAS retries under contention |
