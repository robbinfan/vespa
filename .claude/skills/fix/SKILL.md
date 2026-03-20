---
description: "Use when: root cause confirmed, need code change + build + test. Don't use when: still analyzing or root cause not confirmed. Output: fix-report.md with changes, build status, test results."
user-invocable: true
---

# /fix — Code Fix with Build & Test Verification

Apply code fix for a confirmed root cause, then verify through compilation and testing.
Follows the Shrivu pattern: minimal scope, hard verification, structured output.

## Prerequisites

- `analysis.md` exists with a confirmed root cause (user-annotated)
- If no confirmed root cause, stop and tell user to run `/diagnose` first

## Workflow

### Step 1: Read Confirmed Root Cause
- Read `analysis.md`, find the user-confirmed cause
- Identify the exact module(s) and file(s) to change
- If the fix spans more than 3 modules, warn the user and ask for scope confirmation

### Step 2: Implement Fix
- Make the minimal change that addresses the root cause
- Do NOT refactor surrounding code
- Do NOT add features beyond the fix
- For Java: follow `-Xlint:all -Werror` — no new warnings
- For C++: follow vespalib conventions, add test if touching public API

### Step 3: Build Affected Modules
Detect language and build:

**Java modules:**
```bash
mvn install -pl <module> -am -DskipTests
```

**C++ modules:**
```bash
make -j$(nproc) <target>
```

If build fails:
- Show first 20 lines of error
- Attempt to fix compilation errors (max 3 attempts)
- If still failing after 3 attempts, stop and report

### Step 4: Run Tests
**Java:**
```bash
mvn test -pl <module>
```

**C++:**
```bash
ctest --output-on-failure
```

### Step 5: Output fix-report.md

Write results to `fix-report.md`:

```markdown
# Fix Report: [issue description]
Date: [timestamp]
Root cause: [from analysis.md]

## Changes Made
- [file:line] — [what changed and why]

## Build Status
- Module: [name] — [PASS/FAIL]
- Duration: [seconds]

## Test Status
- Module: [name] — [X/Y tests passed]
- Failed tests: [list if any]

## Remaining Risks
- [anything the fix doesn't cover]

## Next Step
- Ready for `/verify` (performance validation)
- OR: [what needs to be fixed first]
```

## Constraints

- One confirmed root cause → one fix. Don't batch unrelated changes
- Build output: capture but only show errors/warnings summary (≤20 lines)
- Test output: show only failures, not full passing test list
- If ABI check fails, this is a breaking change — warn user explicitly
- Write results to fix-report.md, not to chat context
- NEVER modify `parent/pom.xml` or `screwdriver.yaml`
