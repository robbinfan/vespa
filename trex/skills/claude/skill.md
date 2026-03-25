---
name: fast-regex-search
description: >
  Fast regex search over large codebases using trigram index. Dramatically
  faster than grep/ripgrep by first filtering candidate files via a prebuilt
  trigram index, then running the actual regex only on those files.
  Use when the user wants to search a large codebase quickly or asks to
  implement/use fast regex search.
---

# Fast regex search with trex

## Setup (run once per repo)

```bash
# Build from source (if binary not present)
cd .claude/tools/trex && go build -o trex . && cd -

# Build index
.claude/tools/trex/trex build --dir .
```

## Search

```bash
# Regex search — returns matching file paths
.claude/tools/trex/trex search --pattern "PATTERN" --root . --files-only

# Fixed-string search (faster, bloom-filtered)
.claude/tools/trex/trex search -F --pattern "LITERAL" --root . --files-only

# With context lines
.claude/tools/trex/trex search --pattern "PATTERN" --root . --context 3

# With ripgrep verification (if rg is installed)
.claude/tools/trex/trex search --pattern "PATTERN" --root . --rg --files-only
```

## When to use

- Searching for function/class definitions across the codebase
- Finding all files that reference a specific API
- Any regex or literal search where speed matters
- Especially effective for rare/selective patterns (3–13× faster than ripgrep)

## When NOT to use

- Very short patterns (<3 chars like `if`) — no trigram benefit, falls back to full scan
- Ultra-common patterns (`return`) — ripgrep's SIMD scan is faster
- The index needs to be built first; if it's missing, fall back to grep/rg

## Rebuild index after major changes

```bash
.claude/tools/trex/trex update --dir .     # incremental
.claude/tools/trex/trex build --dir .      # full rebuild
```
