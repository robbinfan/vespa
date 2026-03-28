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

## Task Mode

Every task must declare its mode upfront. The mode determines which dimensions are mandatory.

**Exploration mode** — "is this direction worth pursuing?"
- Required: D1 + D2 + D6 (enough to judge if the approach is viable)
- Recommended: D3a, D5 (catch obvious issues early)
- Output clearly marked `[EXPLORATION]` — results are directional, not production-ready

**Production mode** — "this will be merged and deployed"
- Required: ALL of D1-D9 (as applicable) + C1-C6 + Q1-Q5
- D3b (TSan + Concurrency Safety Proof) mandatory for any concurrent code
- Phase 0.5 mandatory for replacement tasks

**Mode transition**: When an exploration PR is promoted to production, a full
re-evaluation is required. Do NOT assume exploration benchmarks are sufficient.

> **Origin**: PR #1/2/3 were all exploration-quality but had production-level bugs
> that only a full evaluation would have caught.

## Task Execution Protocol

### Phase 0: Scope Understanding (BEFORE any code)

Output a structured task brief:

```
## Task Brief
- **Mode**: [Exploration | Production]
- **Goal**: [one sentence - what we're optimizing and why]
- **Target component**: [exact files/classes]
- **Dependency chain**: [which layers are involved, per dependency-aware-optimization.md]
- **Success criteria**: [concrete, measurable - e.g., "2x lookup throughput at p99"]
- **Benchmark dimensions**: [list which D1-D9 from benchmark-harness.md apply]
- **Risk assessment**: [what could go wrong, what upstream/downstream effects to watch]
```

Do NOT proceed to implementation until this brief is reviewed.

### Phase 0.5: Replacement Target Analysis (when task is "replace X with Y")

When the task involves replacing an existing component, you MUST analyze the existing
implementation BEFORE designing the replacement. Read the actual code, don't work from
assumptions or simplified mental models.

Output:

```
## Replacement Target Analysis: [component being replaced]
- **Architecture**: [core data structures, file layout, access patterns]
- **I/O model**: [number of files, mmap strategy, cache behavior, access sequence]
- **Interface contracts**: [how upstream/downstream code calls it, invariants relied upon]
- **Known bottlenecks**: [why it's being replaced — with evidence from code/profiling]
- **Design tradeoffs**: [what the original design optimized FOR and AGAINST]
```

This analysis constrains the benchmark:
- **Baseline must use the REAL existing component**, not a simulation. If the real
  component cannot be benchmarked directly, explicitly document the simulation's
  limitations and which behaviors are NOT modeled.
- **All access patterns of the original must be covered**: if the original supports
  5 operations, the benchmark must compare all 5, not just the one you expect to win.
- **Interface equivalence must be verified**: the replacement must satisfy the same
  contracts (e.g., same DictionaryFileRandRead interface, same ordering guarantees).

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

### Phase 4: Self-Audit (mandatory, every task, no exceptions)

After Phase 3 completes — even if no one asks for a review — answer these questions:

```
## Self-Audit
1. **Skipped dimensions**: Which mandatory dimensions did I skip? Why?
   [list, or "none — all D1-D6 covered with evidence at [file:line]"]

2. **Unverified assumptions**: Where did I assume something without testing?
   [e.g., "assumed immutable → no concurrent test needed" — was this actually verified?]

3. **Production crash scenario**: If this code goes to production and crashes,
   what is the most likely cause?
   [e.g., "data race on shared_ptr during concurrent read+rebuild"]

4. **Harness blind spots**: Is there a failure mode that no current harness rule covers?
   [If yes → propose a new rule with Origin tag, update benchmark-harness.md NOW]

5. **Rule gaming**: Did I technically satisfy a rule while missing its intent?
   [e.g., "D3: ran 2 threads but no actual contention" — this satisfies the letter but not the spirit]
```

Question 3 is the most important. If the answer describes a scenario that
harness rules don't cover, that IS the next harness evolution.

Question 5 prevents the harness from becoming a checkbox exercise.
If the answer is "yes", tighten the rule immediately.

> **Origin**: PR #1/2/3 retrospective — all bugs were in categories the harness
> "covered" but not precisely enough to force CC to actually catch them.

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

### 5. "Strawman Baseline"
Replacing component X with Y, but benchmarking Y against a simplified simulation of X
instead of the real implementation. The simulation omits critical behaviors (e.g., 3-file
mmap, LCP compression, cache line distribution) so the comparison is misleading.

**Fix**: Phase 0.5 — analyze the real component. Benchmark against it directly, or
explicitly document every simplification and its impact on results.

### 6. "Ignoring the Lifecycle"
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
- [ ] If replacing a component: did I read its actual code first (Phase 0.5)?
- [ ] If replacing: is my baseline the real component, not a simplified simulation?

If any answer is "no", the task is not complete.
