---
name: benchmark-harness
description: Generate a comprehensive benchmark plan and code for a Vespa component optimization, covering all mandatory dimensions from the harness spec.
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash, Write, Edit, Agent
---

# Benchmark Harness Generator

You are generating a comprehensive benchmark for a Vespa component optimization.

## Input

The user will specify:
- **Component**: which module/class is being optimized (e.g., "memory index field_index", "btree insert", "datastore compaction")
- **Change description**: what optimization is being made or evaluated

## Workflow

### Step 1: Identify the component's layer

Read `.claude/rules/dependency-aware-optimization.md` and identify which layer (0-5) this component belongs to. Determine:
- One layer down (what it depends on)
- One layer up (what depends on it)

### Step 2: Determine applicable dimensions

Read `.claude/rules/benchmark-harness.md`. ALL of D1-D6 are mandatory. Determine which of D7-D9 also apply based on the component.

### Step 3: Output the benchmark plan

Before writing any code, output a structured plan:

```
## Benchmark Plan: [component]

### Layer Analysis
- Target: Layer N - [name]
- Depends on: Layer N-1 - [name] (will benchmark for new bottlenecks)
- Depended by: Layer N+1 - [name] (will benchmark for regressions)

### Dimensions Covered
- [x] D1: Single-Op Throughput — [specific ops to test]
- [x] D2: Memory Footprint — [specific metrics]
- [x] D3: Concurrent Mixed R/W — [specific configurations]
- [x] D4: Full Lifecycle — [specific lifecycle stages]
- [x] D5: Data Distribution — [specific distributions]
- [x] D6: Scale Testing — [specific scales]
- [x/n/a] D7: CPU Cache — [if applicable]
- [x/n/a] D8: I/O Patterns — [if applicable]
- [x/n/a] D9: System-Level — [if applicable]

### Files to Create/Modify
- [list benchmark source files]
- [list CMakeLists.txt changes]
```

### Step 4: Ask for confirmation

Wait for user approval of the plan before writing code.

### Step 5: Generate benchmark code

Write the benchmark as a GTest executable following Vespa conventions:
- File location: `<module>/src/tests/<component>/benchmark/`
- CMakeLists.txt with `vespa_add_executable(... TEST ...)`
- Use existing Vespa patterns (look at `searchlib/src/tests/postinglistbm/` and `vespalib/src/tests/btree/btree-stress/` for reference)
- Include data generators for all D5 distributions
- Include multi-threaded driver for D3 concurrency
- Output results in the structured format from benchmark-harness.md

### Step 6: Run baseline

Build and run the benchmark against current code. Record baseline numbers.
