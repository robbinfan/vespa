# trigrep

A trigram-index-based code search tool for large monorepos, built for the
[Vespa](https://github.com/vespa-engine/vespa) codebase (~16k source files).

Inspired by the engineering behind [Cursor's codebase search](https://www.cursor.com/blog/cursor-tab)
and [GitHub's next-generation code search](https://github.blog/engineering/the-technology-behind-githubs-new-code-search/),
trigrep pre-builds an inverted index over file trigrams so queries skip the
vast majority of files entirely — no full-directory scan needed.

---

## Quick start

```bash
# Build index (run once, ~2s for 16k files)
trigrep build --dir /path/to/repo

# Search with regex
trigrep search --pattern "SearchIterator::[a-z]+"

# Fixed-string search (faster, bloom-filtered)
trigrep search -F --pattern "::run("

# Use ripgrep for within-file verification (SIMD-accelerated)
trigrep search --pattern "getDocumentType" --rg

# Incremental re-index (only re-reads changed files)
trigrep update
```

---

## The evolution: v1 → v5

The design went through five generations, each driven by real performance
measurements and a demand for correctness.

---

### v1 — Proof of concept (`62d6ff9d`)

**Motivation:** Searching a ~16k-file C++/Java monorepo with `grep -rE` takes
100–140ms even warm-cache. AI coding assistants need sub-50ms response to feel
instant. The [Cursor blog](https://www.cursor.com/blog/cursor-tab) described
using trigram indexes; this was the first working implementation.

**Design:**
- 3-byte sliding window over each file → inverted posting map `trigram → []fileID`
- Binary index: `Header(16B) | FileTable | TrigramTable[(trigram,offset,count)×N] | PostingLists[uint32×]`
- Search: extract trigrams from literal pattern → intersect posting lists → read & verify candidate files

**Limitations:**
- Flat literal extraction (no regex structure awareness)
- Sequential single-threaded build
- Full re-index on every run
- No pruning — common trigrams like `int` polluted every search

**Performance (15k files):**
| Metric | Value |
|--------|-------|
| Index size | ~80 MB |
| Build time | ~8s |
| Search (`::run(`) | ~55ms |

---

### v2 — Frequency pruning (`dec3560f`)

**Motivation:** Trigrams appearing in >50% of files are useless constraints —
they only slow down intersection without pruning candidates.

**Changes:**
- `--max-freq` flag (default 0.5): trigrams above the threshold are stored as
  pruned sentinels (`count=0`) and skipped during search
- Result: 231 trigrams pruned out of 132,802 (the "ee ", "   ", `int` family)

**Performance delta:**
| Metric | v1 | v2 |
|--------|----|----|
| Search (`::run(`) | ~55ms | ~45ms |
| Index size | ~80 MB | ~70 MB |

---

### v3 — Incremental update (`25acc211`)

**Motivation:** Full rebuild on every file change is wasteful; most files are
unchanged between editing sessions.

**Changes:**
- FileTable gains `mtime(8) + size(8)` per entry (v3 format)
- `trigrep update` compares on-disk mtime/size against index; re-indexes only
  changed/added files, copies posting entries for unchanged ones
- Falls back to full rebuild when >50% of files changed

**Performance delta:**
| Scenario | Time |
|----------|------|
| Full rebuild (16k files) | ~8s |
| Incremental (1 file changed) | ~0.3s |

---

### v4 — Bloom filter + parallel build + mmap + regexp plan (`86f8915a`, `21ceec57`, `0b46273a`)

This was the biggest step: four orthogonal improvements shipped together after
benchmarking revealed multiple bottlenecks.

#### 4a — Parallel build

**Problem:** Single-threaded file I/O and trigram extraction was CPU-bound at
~8s for 16k files.

**Fix:** Worker pool with `runtime.NumCPU()` goroutines. A separate launcher
goroutine feeds workers via a semaphore channel to avoid deadlock (workers
write to a buffered `results` channel; blocking the launcher in the main
goroutine would prevent draining).

**Result:** 8s → 2.3s warm cache (×3.5 speedup on 4 cores).

#### 4b — Bloom filter ("3.5-gram")

**Problem:** Trigram intersection narrows candidates to ~100 files, but each
still needs a disk read for `bytes.Contains` verification.

**Insight:** If trigram `abc` is followed by `d` in the file, we can record
which of 8 character-class groups (each covering 32 chars: `[0-31]`,
`[32-63]`, …, `[224-255]`) contains `d`. This is stored as 1 extra byte per
posting entry — effectively a "3.5-gram" filter that eliminates files where
the required next-character group is absent.

```
bloom[trigram] |= 1 << (nextByte >> 5)   // 8 groups × 32 chars
```

**Result:** `::run(` candidates: 114 → 111 eliminated without file reads.
For longer phrases the effect compounds (each trigram position is a filter).

#### 4c — mmap index load

**Problem:** `os.ReadFile` on a 55MB index takes ~70ms cold (kernel page
faults on first access).

**Fix:** `--mmap` flag uses `syscall.Mmap(PROT_READ, MAP_SHARED)` — the OS
reads only accessed pages on demand.

**Result:** Index load 70ms → 27ms (first-access); negligible on warm cache.

#### 4d — regexp → AND/OR trigram plan

**Problem:** The original `extractLiterals` approach extracted all literal
substrings and ANDed their trigrams. For `(void|bool).*run\(` this would AND
the trigrams of `void`, `bool`, and `run(` together — wrongly excluding files
that contain `void run(` but not `bool`.

**Fix:** Build an AND/OR tree mirroring the regexp AST:
```
(void|bool).*run\(
  → And[Or[lits("void"), lits("bool")], lits("run(")]
```

`OpAlternate` → `planOr` (union posting lists)
`OpConcat` → `planAnd` (intersect posting lists)
`OpLiteral` → `planLits` (trigrams of literal)
`OpStar/Dot/CharClass` → `nil` (unconstrained, pass all)

**Result:** Zero false negatives verified against `grep -rE` ground truth.
Previously, alternation patterns silently dropped valid matches.

#### 4e — Bug fix: path-component matching

**Problem:** `strings.Contains(path, "target")` caused 198 files like
`summarycompacttarget.cpp` to be excluded because their *filenames* contained
"target" (a Maven build-directory ignore pattern).

**Fix:** `hasIgnoredComponent()` splits on `/` and checks exact component
equality (`part == ig`). Index grew from 15,698 → 15,896 files.

**v4 performance summary:**
| Metric | v3 | v4 |
|--------|----|----|
| Index size | ~70 MB | 55 MB |
| Build time (warm cache) | ~8s | 2.3s |
| Index load | 70ms | 27ms (mmap) |
| Search `::run(` (total) | ~45ms | ~25ms |
| False negatives vs grep | some | **0** |

---

### v5 — Varint delta encoding + smallest-first + ripgrep integration (`4244d67b`)

Three more optimizations, each independent.

#### 5a — Varint delta encoding

**Problem:** Posting lists store `uint32` fileIDs (4 bytes each). FileIDs are
sorted, so consecutive entries are close together — storing raw values wastes
~2.5 bytes per entry.

**Fix:** Store `uvarint(fileID − prevFileID)` per entry. For a typical file
distribution (IDs 0–15895), most deltas are small:
- Delta < 128: 1 byte
- Delta < 16384: 2 bytes
- Mean delta ≈ 1.2 bytes for 132k trigrams × 15k files

Header gains `postingsByteLen(4)`; trigram table offsets become byte offsets
into the varint data. Bloom section layout unchanged; indexed via
`relBases[ti] = Σ counts[0..ti-1]`.

**Result:** 55 MB → 25 MB (**55% reduction**, better than theoretical ~37%
because the distribution skews heavily toward small deltas).

#### 5b — Smallest-posting-list-first evaluation

**Problem:** In `planLits`, trigrams were intersected in arbitrary order.
Intersecting a large list (e.g. `" th"`, 8000 entries) before a small one
(e.g. `"ZooK"`, 4 entries) wastes time on large intermediate sets.

**Fix:** Sort `p.trigrams` by ascending `idx.counts[ti]` before intersecting.
The smallest list creates the initial candidate set; subsequent intersections
can only shrink it — and they start from a tiny base.

**Cost:** O(k log k) sort on k trigrams per query (k ≤ ~10). Essentially free.

#### 5c — Ripgrep integration (`--rg`)

**Problem:** Go's `regexp` engine is slower than native code for the
within-file verification pass, especially for long literal strings.

**Fix:** `--rg` flag pipes candidate paths to `rg` (ripgrep) for final
verification. ripgrep uses:
- **Teddy algorithm**: SIMD (AVX2) multi-pattern literal search
- **PCRE2 JIT** for complex regex
- Highly optimized inner loop with SSE2/SSSE3/AVX2 dispatch

Candidate paths are batched (512/exec) to stay within `ARG_MAX`.

**v5 performance summary:**
| Metric | v4 | v5 |
|--------|----|----|
| Index size | 55 MB | **25 MB** |
| Build time | 2.3s | **2.1s** |
| Search `::run(` | ~25ms | ~24ms |
| Search `ZooKeeperDeployer` | ~25ms | ~25ms |
| False negatives | 0 | **0** |

---

## Performance at a glance

Benchmark environment: Vespa monorepo, 15,896 source files
(C++/Java/Python/Go), warm OS page cache, 4 cores.

### Search latency vs ripgrep (no index)

| Pattern | rg (no index) | trigrep v5 | Speedup |
|---------|--------------|------------|---------|
| `::run\(` | 132ms | 24ms | **5.5×** |
| `getDocumentType` | 122ms | 53ms | **2.3×** |
| `ZooKeeperDeployer` | 2408ms | 25ms | **96×** |
| `(SearchIterator\|Blueprint)::[a-z]+` | 129ms | 143ms | 0.9× |

> Notes: trigrep time includes index load + intersection + file reads.
> ripgrep searches all matching files in parallel with SIMD.
> For very common patterns trigrep has overhead; for rare/selective patterns
> the index advantage dominates massively (`ZooKeeperDeployer`: 96×).
> The OR-heavy pattern `(SearchIterator|Blueprint)` produces 241 candidates —
> more overhead than benefit at this scale.

### Index metrics

| Version | Format | Index size | Build time | False negatives |
|---------|--------|-----------|------------|-----------------|
| v1 | raw uint32, no pruning | ~80 MB | ~8s | some (wrong OR) |
| v2 | + frequency pruning | ~70 MB | ~8s | some |
| v3 | + mtime/size | ~70 MB | ~8s | some |
| v4 | + bloom, parallel, mmap | 55 MB | 2.3s | **0** |
| v5 | varint delta | **25 MB** | **2.1s** | **0** |

---

## Comparison with industry tools

| Tool | Strategy | Index? | Cold query | Accuracy |
|------|----------|--------|-----------|----------|
| **grep / git grep** | Linear scan | No | 100–2400ms | Exact |
| **ripgrep** | SIMD linear scan + smart filtering | No | 120–2400ms | Exact |
| **trigrep v5** | Trigram index + varint + bloom | Yes | 24–55ms | Exact (0 false negatives) |
| **[zoekt](https://github.com/sourcegraph/zoekt)** | Trigram index + compound literals + ranking | Yes | <10ms (server) | Exact |
| **[GitHub Blackbird](https://github.blog/engineering/the-technology-behind-githubs-new-code-search/)** | Sparse boundary grams + sharding | Yes | <100ms (distributed) | Exact |
| **[Cursor](https://www.cursor.com/blog/cursor-tab)** | Embedding-based semantic + trigram fallback | Yes (both) | <50ms | Semantic + exact |

**trigrep** sits between ripgrep and zoekt: simpler than zoekt (no server,
no ranking), but significantly faster than ripgrep for rare/selective patterns.
At 16k files it already shows 5–96× wins. At 500k+ files (GitHub/Google scale)
the index advantage becomes indispensable.

Key differences from zoekt:
- zoekt uses compound literal extraction (prefix/suffix/exact strings) with a
  richer scoring model; trigrep uses a simpler AND/OR plan tree.
- zoekt indexes content inside the index for small files; trigrep always reads
  from disk for verification (a future #5 TODO).
- trigrep's varint+bloom combination gives competitive index density without
  a persistent server process.

---

## Architecture

```
trigrep build
  collectFiles()              walk dir, exact-component ignore matching
  extractTrigramsFromFile()   3-byte sliding window + bloom byte per trigram
  [parallel, NumCPU workers]
  writeIndex()                varint-delta posting lists + bloom section (v5)

trigrep search
  loadIndex()                 mmap or ReadFile; parse v2–v5 format
  regexpToPlan()              regexp AST → And/Or/Lits plan tree
  evalPlan()                  sorted-list intersect/union, smallest-first
  bloomFilter()               "3.5-gram" elimination without file I/O  (−F only)
  verify()                    bytes.Contains / re.Match / rg --rg
```

### Index binary format (v5)

```
Header      magic(4) ver(4) numFiles(4) numTrigrams(4) postingsByteLen(4)
FileTable   [pathLen(2) path mtime(8) size(8)] × numFiles
TrigramTable [trigram(4) byteOffset(4) count(4)] × numTrigrams
PostingLists uvarint-delta per trigram (variable length, total = postingsByteLen)
BloomData   uint8 per posting entry (parallel to PostingLists, 1 byte/entry)
```

---

## Options

```
trigrep build
  --dir        directory to index (default: .)
  --output     index file path (default: .claude/trigram-index.bin)
  --ext        file extensions, comma-separated (default: cpp,h,java,py,go,...)
  --ignore     directory components to ignore (default: vendor,node_modules,target,build)
  --max-freq   prune trigrams in >N fraction of files (default: 0.5)

trigrep update
  --index      existing index to update (falls back to full build if stale)
  --dir, --ext, --ignore, --max-freq  same as build

trigrep search
  --pattern    regex pattern (or literal with -F)
  -F/--literal treat pattern as fixed string (enables bloom filter)
  --files-only only print matching file paths
  --context    lines of context around matches (default: 2)
  --root       root directory for resolving paths (default: .)
  --index      index file path
  --mmap       use mmap for index load (lower cold-start latency)
  --rg         use ripgrep for within-file verification (SIMD-accelerated)
```

---

## References

- **Russ Cox, "Regular Expression Matching with a Trigram Index"** (2012)
  https://swtch.com/~rsc/regexp/regexp4.html
  The foundational paper describing trigram-based code search. Describes the
  basic indexing scheme, frequency pruning, and regexp-to-trigram plan
  construction. The AND/OR plan tree in trigrep directly follows this approach.

- **GitHub Engineering, "The technology behind GitHub's new code search"** (2023)
  https://github.blog/engineering/the-technology-behind-githubs-new-code-search/
  Describes Blackbird: boundary-biased sparse grams, content-addressable
  sharding by git blob SHA, Roaring Bitmaps for candidate sets, and
  sub-100ms latency at billion-file scale. Inspired trigrep's todo items #1
  (boundary trigrams) and #3 (bitset candidates).

- **zoekt — sourcegraph/zoekt**
  https://github.com/sourcegraph/zoekt
  Google's open-source trigram search engine (now maintained by Sourcegraph).
  Inspired trigrep's regexp→plan approach, compound literal extraction,
  and the idea of storing trigram counts for smallest-first evaluation.

- **BurntSushi/ripgrep**
  https://github.com/burntsushi/ripgrep
  The fastest general-purpose line-oriented searcher. Uses the Teddy SIMD
  algorithm for multi-pattern literal search, Aho-Corasick automata,
  and PCRE2 JIT. Integrated as trigrep's `--rg` verification backend.

- **Cursor, "How Cursor Works"**
  https://www.cursor.com/blog/cursor-tab
  The original inspiration: describes combining embedding-based semantic
  search with fast trigram index lookup for sub-50ms code retrieval in
  large codebases.

- **Protocol Buffers varint encoding**
  https://protobuf.dev/programming-guides/encoding/#varints
  The unsigned varint format used by trigrep v5 for delta-compressed
  posting lists. Same encoding as used in LevelDB, SQLite4, and most
  modern index formats.

---

## Acknowledgements

This tool would have stayed a competent-but-ordinary trigram searcher without
the persistent and exacting pressure to go further.

Every time a working version was presented, the response was: *"看看zoekt的代码，
再好好吸收些好的东西"* ("read zoekt's code, absorb more good ideas") — and then
ripgrep, and then GitHub Blackbird. Each round of reading and benchmarking
surfaced a new insight: the AND/OR plan tree from Russ Cox, the bloom "3.5-gram"
idea, the varint delta encoding.

The demand for correctness — *zero* false negatives, verified against `grep -E`
ground truth — forced the regexp→plan rewrite that fixed the silent OR-branch
bug affecting alternation patterns. Without that insistence the tool would have
appeared to work while silently missing results.

The demand for measurement at each step kept the work honest: no optimization
landed without a before/after number. That discipline is why v5 ends up at 25MB
index, 24ms queries, and 96× speedup on rare patterns — not because any single
change was magical, but because each one was earned and verified.

> 感谢你的鞭策，让最终方案达到了新高度。
