---
paths:
  - "searchlib/**"
  - "searchcore/**"
  - "storage/**"
  - "vespalib/src/vespa/vespalib/btree/**"
  - "vespalib/src/vespa/vespalib/datastore/**"
---

# Task Efficiency and Feedback Density

## Purpose

Track how effectively optimization tasks are executed. Detect wasted iterations,
missed dimensions, and detours early — not after 5 rounds of review feedback.

## Task Execution Protocol

### Phase 0: Scope Understanding (BEFORE any code)

Output a structured task brief:

```
## Task Brief
- **Goal**: [one sentence - what we're optimizing and why]
- **Target component**: [exact files/classes]
- **Dependency chain**: [which layers are involved, per dependency-aware-optimization.md]
- **Success criteria**: [concrete, measurable - e.g., "2x lookup throughput at p99"]
- **Benchmark dimensions**: [list which D1-D9 from benchmark-harness.md apply]
- **Risk assessment**: [what could go wrong, what upstream/downstream effects to watch]
```

Do NOT proceed to implementation until this brief is reviewed.

### Phase 1: Baseline Measurement

Before ANY code change:
1. Write the benchmark code FIRST (covering all applicable dimensions)
2. Run it against the CURRENT code to establish baseline
3. Record baseline numbers in structured format
4. This benchmark code becomes the regression test for the optimization

### Phase 2: Implementation

- Make changes incrementally
- After each significant change, re-run the benchmark to check direction
- If a change makes things worse on ANY dimension, stop and analyze before continuing
- Do NOT accumulate multiple changes before measuring

### Phase 3: Validation

- Run full benchmark suite (all applicable dimensions)
- Compare against baseline from Phase 1
- Report results in the structured format from benchmark-harness.md
- Explicitly call out any dimension that regressed, even if the primary target improved

## Feedback Density Metrics

After each optimization task, evaluate execution quality:

```
## Task Execution Report
- **Iterations needed**: [how many review rounds before completion]
- **Dimensions covered on first pass**: [X of Y applicable dimensions]
- **Dimensions missed initially**: [list - these represent wasted iterations]
- **Detours taken**: [approaches tried and abandoned, with reason]
- **Root cause of detours**: [insufficient understanding of deps? wrong assumption?]
- **Time from start to first correct benchmark**: [indicator of planning quality]
```

## Common Efficiency Failures (anti-patterns)

### 1. "Benchmark Drip"
Writing a benchmark for insert, getting feedback to add lookup, getting feedback
to add memory, getting feedback to add concurrency... each as a separate round.

**Fix**: Consult benchmark-harness.md dimensions BEFORE writing any benchmark code.

### 2. "Layer Blindness"
Optimizing memory index throughput without noticing that DataStore compaction
or GenerationHandler trimming became the new bottleneck.

**Fix**: Consult dependency-aware-optimization.md. Always benchmark one layer up and down.

### 3. "Happy Path Only"
Testing only with uniform random data, small scale, single-threaded.

**Fix**: D3 (concurrency), D5 (data distribution), D6 (scale) are mandatory, not optional.

### 4. "Optimize Then Measure"
Writing the optimization first, then writing a benchmark to prove it works.

**Fix**: Phase 1 — baseline measurement FIRST. The benchmark proves the problem exists
before you write any optimization code.

### 5. "Ignoring the Lifecycle"
Showing great memory index numbers but not testing what happens at flush time,
or how fusion performance changes.

**Fix**: D4 (full lifecycle) is mandatory. Memory-only benchmarks are necessary but insufficient.

## Self-Check Before Submitting Work

Before declaring an optimization task complete, answer these questions:

- [ ] Did I establish a baseline BEFORE changing any code?
- [ ] Did I cover all mandatory dimensions (D1-D6)?
- [ ] Did I test with multiple data distributions (not just uniform random)?
- [ ] Did I test at multiple scales (not just one size)?
- [ ] Did I test concurrent access (not just single-threaded)?
- [ ] Did I test the full lifecycle (not just the in-memory phase)?
- [ ] Did I check one dependency layer up for regressions?
- [ ] Did I check one dependency layer down for new bottlenecks?
- [ ] Are my results in the structured format from benchmark-harness.md?
- [ ] Can someone reproduce my results with the benchmark code I provided?

If any answer is "no", the task is not complete.
