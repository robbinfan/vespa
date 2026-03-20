---
description: "Use when: online issue needs root cause analysis. Don't use when: root cause already known. Output: analysis.md with evidence chain and candidate causes."
user-invocable: true
---

# /diagnose — Root Cause Analysis

Systematic evidence collection and root cause analysis for Vespa online issues.
Follows the Boris Tane pattern: output structured analysis, wait for human annotation before proceeding.

## Workflow

### Step 1: Collect Evidence
Run these in parallel using subagents:

**Metrics Agent** (Explore subagent):
- Check metrics-proxy `/metrics` endpoint for anomalies
- Look for: latency spikes, error rate changes, QPS drops, memory growth
- Capture the time window of the anomaly

**Log Agent** (Explore subagent):
- Search relevant logs for errors, warnings, stack traces
- Correlate timestamps with metric anomalies
- Focus on the specific time window identified

**Code Agent** (Explore subagent):
- Search recent commits in the suspected module
- Check if the issue correlates with a recent deployment
- Read relevant source code in the suspected area

### Step 2: Output analysis.md

Write findings to `analysis.md` in the working directory with this structure:

```markdown
# Issue Analysis: [brief description]
Date: [timestamp]

## Observed Symptoms
- [metric/log evidence with exact values]

## Affected Components
- Module: [name]
- Layer: [container/content/config]
- Data path: [query/feed/config]

## Candidate Root Causes (ranked by evidence strength)

### Cause 1: [description]
Evidence: [what supports this]
Confidence: [high/medium/low]
Affected code: [file:line references]

### Cause 2: [description]
...

## What I Don't Know Yet
- [gaps in analysis that need human input]

## Suggested Next Steps
- [specific investigation actions]
```

### Step 3: Wait for Annotation

After writing analysis.md, tell the user:
"Analysis written to analysis.md. Please review and annotate:
- Confirm or reject candidate causes
- Add domain context I'm missing
- Point me to the right module if I'm looking in the wrong place"

Do NOT proceed to code changes until the user confirms a root cause.

## Constraints

- Never guess at root cause without evidence
- Prefer metrics over logs, logs over code reading
- If the issue spans Java/C++ boundary, trace through `jrt` RPC layer
- Truncate log output to 30 lines max per source
- Write all findings to analysis.md, not to chat context
