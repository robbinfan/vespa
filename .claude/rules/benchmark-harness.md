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

# Benchmark Harness Specification

When performing performance optimization on any component in searchlib, searchcore,
storage, or their vespalib dependencies, ALL benchmarks must systematically cover the
dimensions below. Do NOT write ad-hoc benchmarks that only test the "happy path" of
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

## Anti-Patterns to Avoid

- Writing a benchmark that only tests insert but not lookup (or vice versa)
- Benchmarking only at one scale
- Testing only uniform random distribution
- Measuring only throughput without latency percentiles
- Testing memory index performance without flush/fusion lifecycle
- Ignoring memory footprint when optimizing for speed
- Not establishing a baseline before making changes
- Benchmarking in isolation without concurrent access patterns
