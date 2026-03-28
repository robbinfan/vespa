---
name: optimization-review
description: Post-task retrospective for optimization work. Analyzes what was done, what was missed, where detours happened, and generates lessons learned.
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash, Agent
---

# Optimization Retrospective

Run this AFTER an optimization task is complete. It analyzes the work done and
identifies inefficiencies to improve future iterations.

## Input

The user will specify:
- **Component**: what was optimized
- **Branch or commits**: the git range to analyze (default: current branch vs main)

## Workflow

### Step 1: Gather Evidence

1. Read the git log for all commits on the branch
2. Read all changed files (both implementation and benchmark code)
3. Read any benchmark results that were produced
4. Check the conversation history / commit messages for iteration patterns

### Step 2: Dimension Coverage Audit

Read `.claude/rules/benchmark-harness.md` and check which dimensions were covered:

```
## Dimension Coverage
| Dimension | Covered? | Evidence |
|-----------|----------|----------|
| D1: Single-Op Throughput | Yes/No/Partial | [file:line or "missing"] |
| D2: Memory Footprint | Yes/No/Partial | [file:line or "missing"] |
| D3: Concurrent R/W | Yes/No/Partial | [file:line or "missing"] |
| D4: Full Lifecycle | Yes/No/Partial | [file:line or "missing"] |
| D5: Data Distribution | Yes/No/Partial | [file:line or "missing"] |
| D6: Scale Testing | Yes/No/Partial | [file:line or "missing"] |
| D7: CPU Cache | N/A or covered | [file:line] |
| D8: I/O Patterns | N/A or covered | [file:line] |
| D9: System-Level | N/A or covered | [file:line] |

Coverage: X/6 mandatory, Y/Z conditional
```

### Step 3: Dependency Chain Audit

Read `.claude/rules/dependency-aware-optimization.md` and check:

```
## Dependency Chain Analysis
- Target layer: [N]
- Layer below tested for bottlenecks: Yes/No — [evidence]
- Layer above tested for regressions: Yes/No — [evidence]
- Cross-layer pitfalls checked: [list which were relevant and whether they were tested]
```

### Step 4: Iteration Efficiency Analysis

Analyze the commit history and code evolution:

```
## Iteration Analysis
- Total commits: [N]
- Commits that were "fix benchmark" or "add missing test": [N] — these indicate missed dimensions
- Commits that reverted or changed approach: [N] — these indicate detours
- Was baseline established before optimization? Yes/No

### Detour Log
| Commit | What happened | Root cause | Could it have been avoided? |
|--------|--------------|------------|----------------------------|
| abc123 | Reverted approach X | [reason] | [Yes: if read dep chain / No: genuinely needed exploration] |

### Dimension Discovery Timeline
| Dimension | When first covered | Triggered by |
|-----------|--------------------|-------------|
| D1 | Commit 1 (initial) | Self |
| D2 | Commit 5 | User feedback — "what about memory?" |
| D3 | Commit 8 | User feedback — "concurrent?" |
```

### Step 5: Generate Retrospective Report

```
## Retrospective: [component] optimization

### Summary
- Goal: [what was the optimization target]
- Result: [was it achieved? by how much?]
- Efficiency score: [X/10] based on iteration count and coverage completeness

### What went well
- [list]

### What was missed initially
- [list dimensions, deps, or scenarios that required extra iterations]

### Root causes of inefficiency
- [e.g., "Did not consult benchmark-harness.md before writing benchmarks"]
- [e.g., "Did not check Layer 2 (btree) impact when optimizing Layer 3 (memory index)"]

### Recommendations for next time
- [specific, actionable improvements]

### Harness gaps
- [any dimensions or scenarios not covered by current harness spec that SHOULD be added]
```

### Step 6: Update harness rules if needed

If the retrospective reveals gaps in the harness spec itself (new dimensions,
new cross-layer pitfalls), propose additions to:
- `.claude/rules/benchmark-harness.md`
- `.claude/rules/dependency-aware-optimization.md`

This makes the harness a living document that improves with each optimization cycle.
