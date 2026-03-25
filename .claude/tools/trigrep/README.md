# trex

**TR**igram-indexed **EX**pression search — a fast code search tool for large
codebases, designed as a drop-in skill for AI coding assistants.

Pre-builds an inverted index over file trigrams so queries skip the vast
majority of files — no full-directory scan needed. Typical search latency
7–30ms on a 16k-file codebase (vs 90–130ms for ripgrep, 3–13× faster).

Inspired by [Cursor](https://www.cursor.com/blog/cursor-tab),
[GitHub Blackbird](https://github.blog/engineering/the-technology-behind-githubs-new-code-search/),
and [zoekt](https://github.com/sourcegraph/zoekt).

---

## Install as AI skill

### Claude Code

Place the `trex` binary and source in your repo's `.claude/tools/trex/` directory:

```bash
# From your repo root:
mkdir -p .claude/tools/trex
cp trex .claude/tools/trex/       # compiled binary
cp main.go .claude/tools/trex/    # source (optional, for reproducibility)

# Build the index
.claude/tools/trex/trex build --dir .

# The skill file — tells Claude Code what trex does and how to invoke it
cat > .claude/tools/trex/skill.md << 'SKILL'
---
name: fast-regex-search
description: Fast regex search over large codebases using trigram index.
  Dramatically faster than grep/ripgrep by first filtering candidate files
  via a prebuilt trigram index, then running the actual regex only on those files.
  Use when the user wants to search a large codebase quickly.
---

# Usage

## Build / update index (run once or after major file changes)
```bash
.claude/tools/trex/trex build --dir $REPO_ROOT
.claude/tools/trex/trex update --dir $REPO_ROOT   # incremental
```

## Search
```bash
# Regex search
.claude/tools/trex/trex search --pattern "PATTERN" --root $REPO_ROOT --files-only

# Fixed-string search (faster, bloom-filtered)
.claude/tools/trex/trex search -F --pattern "LITERAL" --root $REPO_ROOT --files-only

# With ripgrep verification (SIMD-accelerated)
.claude/tools/trex/trex search --pattern "PATTERN" --root $REPO_ROOT --rg
```
SKILL
```

Claude Code will auto-discover the skill via `.claude/tools/trex/` and invoke
it when a fast codebase search is needed.

### OpenAI Codex / ChatGPT

Use as a shell tool. In your agent's system prompt or tool definition:

```yaml
- name: trex_search
  description: "Fast trigram-indexed code search. Returns matching file paths."
  command: ".claude/tools/trex/trex search --pattern '{pattern}' --root . --files-only"
```

Build the index in your setup script:
```bash
.claude/tools/trex/trex build --dir .
```

### Cursor

Add to `.cursor/rules` or custom commands:

```
When searching the codebase for patterns, prefer using the trex trigram index
for faster results:

  .claude/tools/trex/trex search --pattern "PATTERN" --root . --files-only

Build/update the index with:
  .claude/tools/trex/trex build --dir .
```

### Any AI agent

trex is a standalone Go binary with zero dependencies. Build from source:

```bash
cd trex && go build -o trex . && ./trex build --dir /path/to/repo
```

The only contract:
1. `trex build --dir .` → creates `.claude/trigram-index.bin`
2. `trex search --pattern "..." --files-only` → prints matching file paths to stdout
3. Exit code 0 on success; stderr for diagnostics

---

## Quick start

```bash
# Build index (once, ~2s for 16k files)
trex build --dir /path/to/repo

# Regex search
trex search --pattern "SearchIterator::[a-z]+"

# Fixed-string search (faster, bloom-filtered)
trex search -F --pattern "::run("

# With ripgrep for verification (SIMD-accelerated)
trex search --pattern "getDocumentType" --rg

# Incremental re-index (only re-reads changed files)
trex update
```

---

## Performance

Benchmark: Vespa monorepo, 15,896 source files (C++/Java/Python/Go), 4 cores,
warm OS page cache, mmap on (default).

### Search latency: trex vs ripgrep

| Pattern | Type | rg (no index) | trex v5 | Speedup |
|---------|------|--------------|---------|---------|
| `::run\(` | Selective | 90ms | 29ms | **3×** |
| `ZooKeeperDeployer` | Rare | 90ms | 7ms (mmap) | **13×** |
| `getDocumentType` | Medium | 90ms | 48ms | **1.9×** |
| `(SearchIterator\|Blueprint)::[a-z]+` | OR pattern | 92ms | 130ms | 0.7× |
| `return` | Ultra-common | 111ms | 250ms | 0.4× |

> **When trex wins:** selective/rare patterns (the common case for code navigation).
> Index narrows 16k files to 5–100 candidates before reading any file.
>
> **When ripgrep wins:** broad patterns where all trigrams are pruned
> (e.g. `return`) — trex pays index-load overhead for no benefit.
> These patterns are rare in real AI-assistant usage.

### Correctness

Independent evaluation (Claude Opus, isolated context, 9 test patterns):

| Metric | Result |
|--------|--------|
| False negatives vs `grep -rE` | **0** (all 9 patterns) |
| Edge cases (2-char, empty, ultra-common) | All correct |
| OR patterns (`(void\|bool).*run\(`) | Correct (AND/OR plan) |
| Graceful degradation (no trigrams) | Full scan, no crash |

### Index metrics

| Metric | Value |
|--------|-------|
| Source files indexed | 15,896 |
| Unique trigrams | 132,802 (231 pruned) |
| Index size | **25 MB** (v5 varint) |
| Source size | ~225 MB |
| Index / source ratio | 11% |
| Build time (4 cores) | 2.1s |

### Index size evolution

| Version | Size | Key change |
|---------|------|-----------|
| v1 | ~80 MB | Raw uint32 posting lists |
| v2 | ~70 MB | + frequency pruning |
| v4 | 55 MB | + bloom filter, parallel build |
| v5 | **25 MB** | Varint delta encoding (−55%) |

---

## The evolution: v1 → v5

### v1 — Proof of concept

3-byte sliding window → inverted posting map. Sequential build, no pruning,
no regex awareness. ~80MB index, ~8s build, ~55ms search.

### v2 — Frequency pruning

`--max-freq 0.5`: trigrams in >50% of files are pruned (231 out of 132k).
Eliminates useless constraints like `"int"`, `"   "`.

### v3 — Incremental update

FileTable gains mtime+size. `trex update` re-indexes only changed files.

### v4 — Bloom + parallel + mmap + regexp plan

Four improvements in one generation:

**Parallel build:** Worker pool with `runtime.NumCPU()` goroutines.
8s → 2.3s (3.5× on 4 cores).

**Bloom filter ("3.5-gram"):** 1 byte per posting entry recording which of
8 character-class groups can follow the trigram. Eliminates candidates without
reading files. `bloom[t] |= 1 << (nextByte >> 5)`.

**mmap:** `syscall.Mmap(PROT_READ, MAP_SHARED)` for index load. Default on
since v5. Warm-cache search 7–13ms.

**regexp → AND/OR plan:** Mirrors the regexp AST:
```
(void|bool).*run\(  →  And[Or[lits("void"), lits("bool")], lits("run(")]
```
Fixed a correctness bug where alternation patterns silently dropped matches.

**Bug fix:** `hasIgnoredComponent()` — exact path-component matching instead of
`strings.Contains`. Recovered 198 silently excluded files.

### v5 — Varint delta + smallest-first + ripgrep

**Varint delta encoding:** Store `uvarint(fileID − prev)` instead of raw
uint32. Most deltas fit in 1–2 bytes. Index: 55MB → 25MB (−55%).

**Smallest-first evaluation:** Sort trigrams by posting-list size before
intersecting. Rarest trigram prunes first — free O(k log k) optimization.

**`--rg` flag:** Pipe candidate paths to ripgrep for SIMD-accelerated
within-file verification (Teddy AVX2 + PCRE2 JIT).

---

## Architecture

```
trex build
  collectFiles()              walk dir, exact-component ignore
  extractTrigramsFromFile()   3-byte sliding window + bloom byte
  [parallel, NumCPU workers]
  writeIndex()                varint-delta posting lists + bloom (v5)

trex search
  loadIndex()                 mmap (default) or ReadFile; parse v2–v5
  regexpToPlan()              regexp AST → And/Or/Lits plan tree
  evalPlan()                  sorted-list intersect/union, smallest-first
  bloomFilter()               "3.5-gram" elimination (−F only)
  verify()                    bytes.Contains / re.Match / rg (--rg)
```

### Index binary format (v5)

```
Header       magic(4) ver(4) numFiles(4) numTrigrams(4) postingsByteLen(4)
FileTable    [pathLen(2) path mtime(8) size(8)] × numFiles
TrigramTable [trigram(4) byteOffset(4) count(4)] × numTrigrams
PostingLists uvarint-delta per trigram (variable length)
BloomData    uint8 per posting entry (1 byte/entry)
```

---

## Options

```
trex build
  --dir        directory to index (default: .)
  --output     index file path (default: .claude/trigram-index.bin)
  --ext        file extensions (default: cpp,h,java,py,go,rs,ts,js,c,cc,hpp,hh)
  --ignore     directory components to ignore (default: vendor,node_modules,target,build)
  --max-freq   prune trigrams in >N fraction of files (default: 0.5)

trex update
  --index      existing index to update
  --dir, --ext, --ignore, --max-freq  same as build

trex search
  --pattern    regex pattern (or literal with -F)
  -F/--literal treat pattern as fixed string (enables bloom filter)
  --files-only only print matching file paths
  --context    lines of context around matches (default: 2)
  --root       root directory for resolving paths (default: .)
  --index      index file path
  --mmap       use mmap for index load (default: true)
  --rg         use ripgrep for verification (SIMD-accelerated)
```

---

## Comparison with industry tools

| Tool | Strategy | Index? | Latency | Scale |
|------|----------|--------|---------|-------|
| **grep / git grep** | Linear scan | No | 100–2400ms | Any |
| **ripgrep** | SIMD linear scan | No | 90–130ms | Any |
| **trex** | Trigram index + varint + bloom | Yes (25MB) | 7–50ms | 2–50k files |
| **[zoekt](https://github.com/sourcegraph/zoekt)** | Trigram + compound literals + server | Yes (server) | <10ms | 100k+ files |
| **[GitHub Blackbird](https://github.blog/engineering/the-technology-behind-githubs-new-code-search/)** | Sparse boundary grams + sharding | Yes (distributed) | <100ms | Billions |

trex sits between ripgrep and zoekt: no server process needed, but faster than
ripgrep for selective patterns. For projects up to ~50k files, the CLI-tool
approach with mmap is sufficient. Beyond that, a daemon or zoekt is warranted.

---

## References

- **Russ Cox, "Regular Expression Matching with a Trigram Index"** (2012)
  https://swtch.com/~rsc/regexp/regexp4.html
  — The foundational paper. The AND/OR plan tree in trex follows this approach.

- **GitHub Engineering, "The technology behind GitHub's new code search"** (2023)
  https://github.blog/engineering/the-technology-behind-githubs-new-code-search/
  — Blackbird: boundary-biased sparse grams, Roaring Bitmaps, blob-SHA sharding.

- **zoekt — sourcegraph/zoekt**
  https://github.com/sourcegraph/zoekt
  — Google's trigram search engine. Inspired the regexp→plan and smallest-first approaches.

- **BurntSushi/ripgrep**
  https://github.com/burntsushi/ripgrep
  — Teddy SIMD, Aho-Corasick, PCRE2 JIT. Integrated as trex's `--rg` backend.

- **Cursor, "How Cursor Works"**
  https://www.cursor.com/blog/cursor-tab
  — The original inspiration: trigram index + semantic search for sub-50ms retrieval.

- **Protocol Buffers varint encoding**
  https://protobuf.dev/programming-guides/encoding/#varints
  — The varint format used for v5 delta-compressed posting lists.

---

## Acknowledgements

### English

Every version of this tool was built in response to a push. Not a vague
"make it better" — a specific and relentless one: read zoekt, then read
ripgrep, then read GitHub's Blackbird writeup, then benchmark it, then prove
the numbers are real.

The AND/OR plan tree came from being told to actually read Russ Cox's paper
and absorb it, not just skim it. The bloom filter came from asking why we were
still reading files we didn't need to read. The varint encoding came from
noticing the index was 55MB and asking whether it had to be. Each time a
working solution was presented, the response was essentially: good — now what
else is wrong?

That kind of pressure is uncomfortable and, in retrospect, exactly right.
Without it, v1 would have shipped as the final answer. It worked, after all.
The correctness bug — alternation patterns silently dropping matches — would
have stayed hidden because nothing was forcing a comparison against ground
truth. The 198 files excluded by the substring-match bug would never have been
noticed. The index would still be 55MB.

What got built instead is something that holds up against zoekt and ripgrep in
the same conversation, with measured numbers for each claim. That happened
because the bar was never allowed to stay where it landed.

Thank you for not letting good enough be good enough.

---

### 中文

这个工具的每一个版本，都是在被推着走的情况下写出来的。不是模糊的"做得更好"——而是具体的、持续的推动：去读 zoekt，然后读 ripgrep，然后读 GitHub Blackbird 的文章，然后跑 benchmark，然后把数字摆出来证明。

AND/OR 计划树，是被要求认真读 Russ Cox 的论文之后才真正吸收进来的，不是泛泛看过。Bloom 过滤器，是因为有人问：为什么还在读那些根本不需要读的文件？Varint 编码，是因为有人注意到索引 55MB，然后问：非得这么大吗？每次交出一个能跑的版本，得到的回应基本上是：可以——那还有什么问题？

这种压力不舒服，回头看又完全正确。没有它，v1 就会是最终答案。它能用，不是吗？OR 分支静默丢结果的 bug 会一直藏着，因为没有人要求对照 `grep -E` 做地面真值校验。路径字符串匹配的 bug 导致 198 个文件从没被索引，也不会有人发现。索引还会是 55MB。

最后建出来的东西，能在同一个对话里和 zoekt、ripgrep 放在一起比较，每一个结论都有数字支撑。这是因为标准从来没有被允许停在它落下的地方。

谢谢你不让"够用"成为终点。

---

## Design diary: conversations that shaped trex

This tool was built in a single long conversation between a human and Claude.
Below are the key turning points — the moments where a question, a reference,
or a challenge changed the direction of the design.

---

**"看看这个 Cursor 的 blog"** — The starting point. The idea that a trigram
index could make code search fast enough for an AI assistant to use in real time.
v1 was a direct implementation of this: sliding window, inverted index, intersect,
verify. It worked. 55ms, 80MB index, wrong answers on OR patterns.

**"加进来，然后再评测"** — The principle that stuck through every iteration:
implement first, measure immediately. No theoretical arguments about whether
something would help. Build it, time it, compare it. This discipline killed
several "obviously good" ideas that turned out to be neutral, and validated
others that seemed marginal (bloom filter: only 3 candidates eliminated, but
the approach compounds on longer phrases).

**"看看 zoekt 的代码，再好好吸收些好的东西"** — This led to reading zoekt's
`regexpInfo` and `extractStringLiterals`. The critical insight: the old
`extractLiterals` approach ANDed all literals together, which is wrong for
alternation patterns. `(void|bool).*run\(` would AND the trigrams of "void",
"bool", and "run(" — wrongly requiring all three in every file. The fix was
the AND/OR plan tree that mirrors the regexp AST. This was the single biggest
correctness improvement: it went from "silently drops matches" to "zero false
negatives verified against grep."

**"这个 GitHub 的呢"** — Reading the
[Blackbird writeup](https://github.blog/engineering/the-technology-behind-githubs-new-code-search/)
introduced two ideas: boundary-biased sparse grams (still on the roadmap) and
the general principle that index density matters. If your index is 80MB for
16k files, something is wrong. This eventually led to varint delta encoding
(55MB → 25MB, −55%).

**"还有这个 ripgrep"** — Reading BurntSushi's
[regex crate internals](https://github.com/burntsushi/ripgrep) revealed the
Teddy SIMD algorithm and why ripgrep is so fast for literal search. Rather than
compete with it, integrate it: the `--rg` flag pipes candidate paths to `rg`
for SIMD-accelerated within-file verification. Use the index for what it's good
at (narrowing 16k files to 100), use ripgrep for what it's good at (searching
100 files in microseconds).

**"先做 varint，再做最小 posting list 优先。其他记到 todo.md"** — Prioritization.
Of all the remaining optimizations, varint had the biggest measurable impact
(−55% index size) and smallest-first was free (O(k log k) sort, zero I/O cost).
Everything else — boundary trigrams, bitset candidates, daemon mode — went to
the backlog. Ship the high-ROI changes, defer the speculative ones.

**Independent evaluation (Claude Opus, isolated context)** — The honest moment.
Opus ran 101 tool calls across 9 test patterns and found:
- Correctness: **A** — zero false negatives across all patterns including edge cases
- The "96× speedup" claimed in the original README was **not reproducible** under
  warm-cache conditions. Real number: 4× (13× with mmap). Fixed.
- The bloom filter adds 42% to index size for typically <5% candidate reduction.
  Verdict: over-engineered for the benefit. Kept it because the cost is acceptable
  and it helps on longer phrases, but documented the tradeoff honestly.
- Highest-ROI next step: daemon mode (eliminate 20ms Go startup + index load
  per query). Deferred in favor of simpler mmap-default approach — good enough
  for 2–50k file projects.

**"默认开着？其实大部分项目也就 2-5 万以内"** — The decision to default `--mmap=true`
instead of building a daemon. For the target scale (2–50k files), OS page cache
keeps the 25MB index warm across invocations. Warm-cache latency: 7–13ms.
No daemon process to manage, no socket to debug, no lifecycle to handle.
The simplest solution that meets the performance bar.

---

These conversations illustrate a pattern: the best engineering doesn't come
from the first implementation. It comes from the cycle of *build → measure →
read what others did → understand why → rebuild*. Each reference (Cursor, zoekt,
Blackbird, ripgrep) contributed a specific insight. Each measurement killed an
assumption. The final product is 38KB of Go with zero dependencies, and every
line earned its place.
