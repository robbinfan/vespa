# trex — Cursor integration

Add the following to your `.cursor/rules` file (or `.cursorrules`):

---

## Fast code search with trex

This repository has a trigram search index for fast code search. Use trex
instead of grep/ripgrep when searching for patterns across the codebase.

### Search commands

```bash
# Regex search
.claude/tools/trex/trex search --pattern "PATTERN" --root . --files-only

# Fixed-string search (faster)
.claude/tools/trex/trex search -F --pattern "LITERAL" --root . --files-only

# Search with context lines
.claude/tools/trex/trex search --pattern "PATTERN" --root . --context 3
```

### Rebuild index (after major file changes)

```bash
.claude/tools/trex/trex update --dir .     # incremental
.claude/tools/trex/trex build --dir .      # full rebuild
```

### When to prefer trex over grep/rg

- Searching for function names, class names, API references
- Any selective pattern (trex is 3–13× faster than ripgrep)
- When the index exists at `.claude/trigram-index.bin`

### When to use grep/rg instead

- Very short patterns (<3 chars)
- Ultra-common keywords like `return`, `if`, `for`
- When the index hasn't been built yet
