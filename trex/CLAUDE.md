# trex — Development Guide

trex is a trigram-indexed code search tool written in Go. Single file: `main.go`.

## Build & test

```bash
go build -o trex .
./trex build --dir /path/to/repo
./trex search --pattern "YourPattern" --files-only
```

## Architecture

- `main.go` contains everything: index build, incremental update, search, bloom filter
- Index format is v5 (varint delta encoded posting lists + bloom section)
- No external dependencies (stdlib only)

## Key design decisions

- Single-file by design — easy to vendor into any repo's `.claude/tools/`
- No daemon — mmap + OS page cache gives 7–30ms warm search, good enough for 2–50k files
- `--rg` flag delegates within-file verification to ripgrep when available
- regexp→trigram plan uses AND/OR tree mirroring the regexp AST (not flat literal extraction)

## Testing

Compare against grep ground truth:
```bash
./trex search --pattern "PATTERN" --files-only 2>/dev/null | sort > /tmp/trex.txt
grep -rlE "PATTERN" --include="*.cpp" --include="*.h" --include="*.java" . | sed 's|^\./||' | sort > /tmp/grep.txt
diff /tmp/trex.txt /tmp/grep.txt
```

Zero false negatives is the correctness bar. False positives (candidates that don't match) are acceptable but should be low.

## Roadmap

See README.md "Comparison with industry tools" section and the items below:

### High priority
1. **Boundary trigrams** — extract cross-boundary trigrams at Concat nodes (zoekt-style). Biggest filtering improvement for patterns with character classes.
2. **CharClass expansion** — expand small character classes (`[Cc]`) to enable boundary trigram extraction.

### Medium priority
3. **Bitset candidates** — replace `[]uint32` sorted slices with `[]uint64` bitset (2KB for 16k files, fits L1 cache, SIMD AND/OR).
4. **Daemon mode** — Unix socket server for <5ms queries in high-frequency usage.

### Low priority
5. **Content-in-index** — embed content for files <1KB to skip disk reads.
6. **Sparse variable-length grams** — boundary-biased tokens (GitHub Blackbird style).
7. **Case-insensitive search** — normalize to lowercase at index and query time.
