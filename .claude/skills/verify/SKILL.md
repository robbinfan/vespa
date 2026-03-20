---
description: "Use when: fix applied and built, need to validate behavior/performance. Don't use when: fix hasn't been applied or build failed. Output: verify-report.md with test results and regression analysis."
user-invocable: true
---

# /verify — Post-Fix Verification & Regression Check

Validate that a fix works correctly and hasn't introduced regressions.
Final phase of the Shrivu pattern: diagnose → fix → verify.

## Prerequisites

- `fix-report.md` exists with PASS build and test status
- If fix-report shows failures, stop and tell user to resolve via `/fix` first

## Workflow

### Step 1: Read Fix Context
- Read `fix-report.md` to understand what changed
- Read `analysis.md` to understand the original problem
- Identify the affected module(s) and the original symptoms

### Step 2: Targeted Regression Testing
Run broader tests in the affected module to catch regressions:

**Java:**
```bash
mvn test -pl <module>
```
If the fix touches APIs used by other modules, also test dependents:
```bash
mvn test -pl <dependent-module>
```

**C++:**
```bash
ctest --output-on-failure
```

### Step 3: Verify Original Symptom
- Re-check the original symptom described in `analysis.md`
- If a specific test reproduced the bug, confirm it now passes
- If the symptom was a runtime behavior, describe how to manually verify

### Step 4: Check for Side Effects
- Review the diff for unintended behavioral changes
- For Java: check if ABI plugin would flag breaking changes
- For config changes: verify `.def` files are consistent
- For cross-boundary changes (Java ↔ C++): verify both sides

### Step 5: Output verify-report.md

Write results to `verify-report.md`:

```markdown
# Verification Report: [issue description]
Date: [timestamp]

## Fix Summary
- Root cause: [from analysis.md]
- Fix: [from fix-report.md]

## Regression Tests
- Module: [name] — [X/Y passed]
- Dependent modules tested: [list]
- New failures: [NONE or list]

## Original Symptom Check
- Symptom: [description]
- Status: [RESOLVED / NOT RESOLVED]
- Evidence: [test name or manual check description]

## Side Effect Analysis
- ABI compatibility: [OK / BREAKING]
- Config consistency: [OK / ISSUES]
- Cross-boundary impact: [NONE / details]

## Verdict
[READY TO COMMIT / NEEDS WORK]
- [any remaining items]
```

## Constraints

- Only run tests — do NOT modify code in this phase
- If new failures appear, report them but do not attempt fixes
- Test output: show only failures, not full passing test list
- If verdict is NEEDS WORK, list exactly what needs to change
- Write results to verify-report.md, not to chat context
- Respect architecture boundaries: don't test unrelated modules
