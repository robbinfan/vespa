---
paths:
  - "searchlib/**"
  - "searchcore/**"
  - "storage/**"
  - "vespalib/src/vespa/vespalib/btree/**"
  - "vespalib/src/vespa/vespalib/datastore/**"
  - "vespalib/src/vespa/vespalib/util/generationhandler*"
  - "vespalib/src/vespa/vespalib/util/rcuvector*"
---

# Evaluation Harness Specification

This harness covers BOTH performance benchmarking AND correctness verification.
Benchmarks without correctness are dangerous — a 2x faster merge is worthless if
it silently drops documents. Correctness without benchmarks is incomplete — a
100% correct merge that takes 10x longer causes operational incidents.

When performing optimization or modification on any component in searchlib, searchcore,
storage, or their vespalib dependencies, evaluations must systematically cover the
dimensions below. Do NOT write ad-hoc tests that only cover the "happy path" of
the current change.

## Pre-Optimization Checklist

Before writing any optimization code:
1. Read this spec completely
2. Write a benchmark plan listing which dimensions apply and how each will be measured
3. Present the plan for review BEFORE implementing
4. Establish baseline measurements with the EXISTING code first

## Mandatory Dimensions (all optimizations must cover these)

### D1: Single-Operation Throughput
- Measure ops/sec for each relevant operation type independently
- For index components: insert, lookup, update, delete
- For storage: put, get, remove, visit
- For btree/datastore: allocate, lookup, iterate, compact
- Report: mean, stddev, and throughput over time (detect degradation)

### D2: Memory Footprint
- Peak RSS during benchmark
- Steady-state memory after warmup
- Memory growth curve over time (detect leaks or unbounded growth)
- Per-element overhead (total memory / number of elements)
- For datastore: buffer utilization, dead space ratio

### D3: Concurrent Mixed Read/Write
- Test configurations: 1W+4R, 2W+8R, 4W+16R (W=writers, R=readers)
- Report throughput AND latency percentiles: p50, p95, p99, p999
- Measure reader throughput degradation under write load
- Measure writer throughput degradation under read load
- For RCU-based structures: generation hold list growth under contention

### D4: Full Lifecycle (not just memory-resident)
- Memory index: insert → flush to disk → read back → fusion with existing disk index
- Attributes: load → update → flush → reload
- Storage: put → distribute → persist → recover
- Measure end-to-end, not just the in-memory phase
- Include restart/recovery warmup time

### D5: Data Distribution Sensitivity
- Uniform random keys/values
- Zipfian distribution (skewed, hot keys) with configurable skew (s=0.5, 1.0, 1.5)
- Sequential (monotonically increasing keys)
- Adversarial (worst-case for the data structure, e.g., reverse-sorted for btree)
- Varying key/value sizes (small, medium, large)
- Report how throughput and memory change across distributions

### D6: Scale Testing
- Test at minimum 3 scales: 1x, 10x, 100x typical workload
- For memory index: 10K, 100K, 1M, 10M documents
- For btree: 1M, 10M, 100M entries
- Report whether performance degrades linearly, logarithmically, or worse
- Detect cliff effects (sudden performance drop at certain scale)

## Conditional Dimensions (apply when relevant)

### D7: CPU Cache Efficiency (for low-level data structure changes)
- Use `perf stat` to measure L1/L2/L3 cache miss rates
- IPC (instructions per cycle)
- Branch misprediction rate
- Compare before/after optimization

### D8: I/O Patterns (for disk-touching components)
- Sequential vs random I/O ratio
- Read/write amplification factor
- fsync latency impact
- Direct I/O vs mmap comparison where applicable

### D9: System-Level Effects (for changes affecting concurrency primitives)
- `std::atomic` contention (measure with ThreadSanitizer or perf)
- Memory ordering overhead (relaxed vs acquire/release vs seq_cst)
- NUMA effects for multi-socket systems
- Context switch rate under load

## Output Format

All benchmark results MUST be reported in structured format:

```
## Benchmark Results: [component] - [change description]

### Environment
- CPU: [model, cores, frequency]
- Memory: [total, available]
- OS: [distribution, kernel version]

### D1: Single-Operation Throughput
| Operation | Baseline (ops/sec) | Optimized (ops/sec) | Delta |
|-----------|-------------------|---------------------|-------|
| insert    | ...               | ...                 | +X%   |
| lookup    | ...               | ...                 | +X%   |

### D2: Memory Footprint
| Metric              | Baseline | Optimized | Delta |
|---------------------|----------|-----------|-------|
| Peak RSS            | ...      | ...       | -X%   |
| Steady-state        | ...      | ...       | -X%   |
| Per-element overhead| ...      | ...       | -X%   |

[... repeat for each applicable dimension ...]
```

## Correctness Dimensions (mandatory for storage/feed/distribution changes)

### C1: Operation Ordering and Consistency
- Per-bucket operation sequencing: verify Put(ts=N) followed by Remove(ts=N+1) always results in removal
- Concurrent Put + Remove on same document: verify highest timestamp wins
- Concurrent Put + Update on same document: verify update applies to correct version
- Test-and-set with racing operations: verify condition is evaluated atomically
- Cross-document-type ordering: verify operations on different doc types in same bucket are independent

### C2: Merge Correctness
- Basic merge: N replicas with divergent content → all converge to same state
- Source-only copy handling: verify source-only copies deleted only after successful merge
- Source-only mutation detection: verify merge fails if source-only copy changes during merge
- Partial chain failure: node N in chain fails mid-merge → verify chain unwinds correctly
- Cluster state change during merge: verify outdated merges are aborted, not partially applied
- Merge with concurrent feed: documents written during merge must not be lost
- Merge throttler saturation: verify BUSY responses under queue overflow, verify recovery after drain
- Unordered merge chaining: verify no deadlock when two nodes have full throttle windows
- Rapid cluster state oscillation: state flips N times while merges in-flight → verify no stuck merges
- **Known code gap**: MergeThrottler broken cycle detection + dual-reply unwinding
  (`mergethrottler.cpp:1025-1040`) lacks integration test
- **Known code gap**: Unordered merge queue deadlock prevention logic
  (`mergethrottler.cpp:718-736`) not tested

### C3: Split/Join Correctness
- Split with concurrent feed: operations arriving during split() are remapped, not lost
- Join with concurrent feed: same guarantee for join
- Split then immediate join: verify round-trip preserves all documents
- Bucket info consistency: after split/join, all replicas report consistent bucket info
- Operation remapping ordering: verify remapped operations maintain per-document ordering

### C4: Persistence and Recovery
- Crash during flush: verify recovery replays TLS correctly, no data loss
- Crash after TLS write but before memory index update: verify TLS replay rebuilds state
- TLS serial number validation: verify `impossible` serials are detected (TLS < flushed = fatal)
- Long offline node recovery: node offline for N hours, rejoins → verify data consistency
- Recovery under load: node recovering while cluster is under write load
- Partial flush recovery: flush wrote some files but not all → verify consistent state after restart
- **Known code gap**: Long-offline node GC fires BEFORE merge completes
  (`garbagecollectionoperation.cpp:70-78`) — GC can prune stale data on recovering node
  before merge copies newer data from other replicas. If recovering node was only replica,
  data is permanently lost with no warning. No safeguard exists.
- **Known code gap**: State preemption during recovery is untested
  (`stripe_bucket_db_updater.cpp:292` — explicit TODO in code)

### C5: Feed Path Correctness

The feed path uses a NON-TRANSACTIONAL design: MetaStore is authoritative (sync on
master thread), while doc store / attributes / index are async fire-and-forget with
NO rollback. Durability relies on TLS replay, not operation-level atomicity.

#### C5.1: FeedView Subsystem Consistency
- After any sequence of Put/Update/Remove, verify ALL four subsystems agree:
  MetaStore (GID→LID), DocumentStore (content), Attributes (field values), Index (searchable)
- Test through each FeedView variant independently:
  - StoreOnlyFeedView: MetaStore + DocumentStore only
  - FastAccessFeedView: + Attributes (via IAttributeWriter on AttributeFieldWriter threads)
  - SearchableFeedView: + Index (via IIndexWriter on Index thread)
- Partial failure scenarios (no rollback exists):
  - DocStore write fails → doc in MetaStore+Attributes+Index but content unretrievable
  - Attribute write fails for one field → partial attribute state
  - Index write fails → doc exists but not searchable
  - Verify TLS replay recovers all these partial states to consistent

#### C5.2: Threading and Ordering
- Master thread: MetaStore updates (blocking, source of truth)
- Summary thread: DocumentStore writes (async, parallel for different LIDs)
- AttributeFieldWriter: per-attribute sequenced executor (parallel across attributes)
- Index thread: single thread for all index operations (sequential)
- Shared thread pool: document reconstruction during updates
- Verify: `_pendingLidsForDocStore.waitComplete(lid)` prevents stale reads during update
  (update reads doc from store before prior put completes → gets wrong base document)
- Verify: rapid Put(lid=X) then Update(lid=X) → update applies to correct version
- Verify: forceCommit coordination across all threads (attributes → summary → index)

#### C5.3: DocumentMetaStore LID Management
- LID allocation: verify no duplicate LIDs assigned under concurrent feed
- LID recycling: `_lidReuseDelayer.delayReuse(lid)` → verify removed LID not reused
  while readers still hold generation guard referencing it
- LID compaction (LidSpaceCompactionJob): verify GID↔LID bijection maintained after
  compaction moves documents from high→low LIDs
- Verify: compaction under concurrent feed doesn't lose or duplicate documents

#### C5.4: Update Path (the most complex operation)
- Update reconstructs document: reads from DocStore → applies update → writes back
- Verify: `waitComplete(lid)` prevents reading stale DocStore content
- Verify: concurrent updates to same document produce correct final state
- Verify: update with indexed + non-attribute fields triggers correct reconstruction
  path (shared thread pool) and index path gets the reconstructed document via FutureDoc
- Verify: test-and-set condition evaluated on master thread BEFORE async dispatch,
  but concurrent Remove can execute between check and SPI dispatch

#### C5.5: IndexMaintainer Lifecycle
- Memory index → flush trigger → disk index → fusion
- Verify no documents lost across transitions
- Concurrent feed + flush: new operations must go to NEW memory index after switch,
  not the one being flushed
- Concurrent feed + fusion: operations during fusion go to current memory index,
  fusion only merges completed disk indexes
- Index switch atomicity: the swap from old IndexCollection to new must be atomic
  from readers' perspective (no partial view with some disk indexes missing)
- Verify: queries during flush/fusion see all documents (union of memory + disk indexes)

#### C5.6: Attribute Consistency
- Attribute values must match document store after any operation sequence
- Per-attribute parallelism: different attributes updated on different threads
- Verify: all attributes for one document reflect the same operation (not a mix of
  old and new values from concurrent updates)
- Struct field attributes: updated via reconstructed document, not direct update
- Verify: enum store consistency after rapid updates to same string/enum attribute

### C6: Distributed Correctness

#### C6.1: Cluster State Transitions
- State change propagation: verify all stripes updated atomically (park → update → unpark)
- Rapid state transitions: V1 pending → V2 arrives before V1 completes → verify no data corruption
- Bucket ownership transfer: verify all non-owned buckets cleared, all owned buckets present
- **Known code gap**: Timestamp generation non-atomic across stripes during state transition
  (`top_level_bucket_db_updater.cpp:263` — FIXME in code). Stripes generate timestamps
  independently, can cause bucket metadata inconsistencies.
- **Known code gap**: Config downsize race — config removing nodes arrives before matching
  cluster state (`top_level_bucket_db_updater_test.cpp:2121` — test DISABLED)

#### C6.2: Stripe Coordination
- Park/unpark mechanism: verify no ABA problem in back-to-back park→unpark→park
  (mitigated in `distributor_stripe_pool.cpp:60-67` but adds latency)
- Stripe hang detection: verify system detects when a stripe is stuck in tick()
  and never reaches park point (currently unmonitored)
- Event notification: verify no lost notifications due to race between
  `notify_event_has_triggered()` and `wait_until_event_notified_or_timed_out()`
  (`distributor_stripe_thread.cpp:77` — TODO in code, no mutex protection)

#### C6.3: Replica Convergence
- Replica divergence detection: after N operations with failures, verify all replicas
  converge after merge completes
- Bucket distribution after state change: verify all documents accessible after
  adding/removing nodes
- Global bucket consistency: global buckets must maintain consistency across
  all distributors
- Throttler back-pressure recovery: after back-pressure period ends, verify
  merges resume normally
- Stale reads with deferred activation: verify client sees consistent view during
  transition between mutable and read-only bucket databases
- OutdatedNodes inheritance: if node was marked outdated in State-V1 but never replied,
  verify State-V2 doesn't merge V1's stale RequestBucketInfoReply into V2's database

## Correctness Test Output Format

```
## Correctness Results: [component] - [change description]

### C1: Operation Ordering
| Scenario | Result | Evidence |
|----------|--------|----------|
| Concurrent Put+Remove same doc | PASS/FAIL | [description] |
| Test-and-set with racing ops | PASS/FAIL | [description] |

### C2: Merge Correctness
| Scenario | Result | Evidence |
|----------|--------|----------|
| Partial chain failure unwind | PASS/FAIL | [description] |
| Source-only mutation detection | PASS/FAIL | [description] |

[... repeat for each applicable dimension ...]
```

## Query Path Dimensions (mandatory for matching/ranking/summary changes)

### Q1: Blueprint and Iterator Correctness
- Blueprint::optimize() tree rewriting: verify result set is identical before and after optimization
- Strict vs non-strict propagation: verify inheritStrict() for each IntermediateBlueprint
  type returns correct strictness for each child position:
  - AND: child 0 strict, others non-strict (sorted by estimate, cheapest first)
  - OR: all children inherit parent's strictness
  - ANDNOT: child 0 strict, negation children non-strict
  - RANK: child 0 strict, ranking children non-strict
  - NEAR/ONEAR: child 0 strict only
  - WeakAnd: all children strict
  - SourceBlender: all children strict
- Filter optimization: createFilterSearch() must be UPPER_BOUND (superset of real matches)
  or LOWER_BOUND (subset). Verify filter results are consistent with full evaluation.
- OptimizedAndNotForBlackListing: uses doSeek() directly (bypasses bounds check).
  Verify initRange() is always called before seekFast() — otherwise out-of-bounds access.
- SourceBlenderSearch: verify all documents routable via SourceSelector after
  IndexMaintainer flush/fusion transitions (no "orphaned" documents in wrong source)

### Q2: Two-Phase Ranking Correctness
- Phase 1 (match threads): all matching documents get first-phase score
- Phase 2 (re-rank): only top-K get second-phase score, verify K is correct
- Rendezvous barriers: verify GetSecondPhaseWork correctly selects global top-K
  from per-thread heaps (not just top-K of thread 0)
- Rank drop limit: documents below threshold are excluded from results — verify
  threshold doesn't accidentally exclude documents that would rank high in phase 2
- Score consistency: verify NaN/Inf handling (should map to -HUGE_VAL, not corrupt heap)
- Match limiter: verify estimate_match_frequency() doesn't cause premature termination
  that changes result set (limiter is approximation — verify it doesn't miss top hits)
- Soft doom (timeout): verify partial results have correct coverage reporting
  (don't claim 100% coverage if scan was truncated)

### Q3: Summary and Feature Consistency
- Summary features are computed via DocsumMatcher which RE-EXECUTES the query
  for selected documents. Verify summary feature values match phase-2 values.
- Document content from DocumentStore must be consistent with what was indexed:
  after feed+flush cycle, summary content must match last write
  (THIS IS THE StoreOnlyFeedView BUG CLASS: update modifies attribute/index but
  docstore gets stale content → summary returns wrong field values)
- Attribute values in summary must reflect latest committed state, not in-flight updates

### Q4: Multi-Thread Query Correctness
- DocidRangeScheduler: verify document ranges are non-overlapping and cover full space
  (AdaptiveScheduler uses work-stealing — verify no double-counting)
- DualMergeDirector: binary merge tree — verify no hits lost during merge
- Per-thread HitCollector: three-tier (heap + docid vector + bitvector) — verify
  tier transitions don't lose documents
- Concurrent query + feed: query during flush/fusion must see union of all indexes
  (memory + flushing + disk), never a partial view

### Q5: Query Performance
- Match thread scalability: throughput vs thread count (1, 2, 4, 8, 16 threads)
- Blueprint optimize() overhead: time spent in tree rewriting vs actual matching
- Strict vs non-strict: measure impact of strict propagation changes on throughput
- Filter optimization: measure cost of createFilterSearch() vs full evaluation
- Phase 2 overhead: re-ranking cost vs phase 1 cost at varying top-K sizes
- Summary fetch latency: per-hit cost including DocumentStore read + feature re-computation
- SourceBlender overhead: cost of selector lookup per document

## Performance Anti-Patterns to Avoid

- Writing a benchmark that only tests insert but not lookup (or vice versa)
- Benchmarking only at one scale
- Testing only uniform random distribution
- Measuring only throughput without latency percentiles
- Testing memory index performance without flush/fusion lifecycle
- Ignoring memory footprint when optimizing for speed
- Not establishing a baseline before making changes
- Benchmarking in isolation without concurrent access patterns

## Correctness Anti-Patterns to Avoid

- Testing only the success path without failure injection
- Testing merge without concurrent feed operations
- Testing split/join without verifying document counts before and after
- Assuming per-bucket ordering implies global ordering
- Testing persistence without crash/recovery scenarios
- Testing feed path through only one FeedView variant (must test all three)
- Ignoring the TLS async write window when claiming durability
- Testing IndexMaintainer transitions without concurrent operations
- Modifying FeedView without verifying DocumentStore content matches after update
  (the StoreOnlyFeedView bug: index/attributes updated but docstore gets stale content)
- Changing Blueprint optimize() without verifying result set equivalence pre/post optimization
- Testing query evaluation with single-source index only (must test SourceBlender across
  memory + multiple disk indexes, especially during/after flush and fusion)
- Modifying ranking without verifying summary features match phase-2 features
  (DocsumMatcher re-executes the query — different code path than match thread)
