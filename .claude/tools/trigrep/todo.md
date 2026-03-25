# trigrep todo

## Done
- v2: basic trigram index
- v3: mtime+size for incremental update
- v4: 8-bit bloom filter ("3.5-gram" effect)
- v5: varint-delta posting lists (~55% size reduction, 55MB→25MB)
- parallel build (worker pool, runtime.NumCPU())
- mmap load (--mmap flag)
- regexp→trigram AND/OR plan (correct alternation handling)
- fix hasIgnoredComponent (exact path-component match, not substring)
- smallest-posting-list-first evaluation (free optimization)
- --rg flag: pipe candidates to ripgrep for SIMD-accelerated within-file verification

## #1 — Boundary trigrams (high value, medium complexity)

At each `Concat` node, extract cross-boundary trigrams from the suffix of the
left child and the prefix of the right child.

Example: `[Cc]lass` = Concat[CharClass([C,c]), Literal("lass")]
- Left suffix candidates: {"C", "c"}
- Right prefix: "la"
- Boundary trigrams: {"Cla", "cla"}  ← much more selective than "las"/"ass" alone

Reference: zoekt `extractStringLiterals` + `substrCache`; GitHub Blackbird
"boundary-aware n-grams".

Implementation sketch:
- Give each trigramPlan an optional `prefix []string` / `suffix []string`.
- At Concat, combine: for each (s, p) in suffix×prefix, emit trigrams of s+p.
- These extra trigrams are ANDed into the plan.

## #2 — CharClass small-set expansion

In `regexpToPlan`, handle `syntax.OpCharClass` for small sets (≤ 10 distinct chars):
return `planOr[planLits{"A"}, planLits{"a"}]` etc.

By itself this produces no trigrams (single chars have none), but combined with
boundary trigrams (#1) it enables cross-boundary filtering for patterns like
`[Cc]lass`, `[a-z]_name`, etc.

## #3 — Roaring Bitmap / bitset for candidate sets

Currently candidates are `[]uint32` sorted slices with O(n) intersect/union.
With 15k files a 2KB bitset (1 bit per file) fits in L1 cache; AND/OR become
SIMD bitwise ops instead of merge scans.

Library options:
- `github.com/roaringbitmap/roaring` — compressed, good for sparse sets
- Plain `[]uint64` bitset — simpler, faster for dense sets (15k files → 235 uint64s)

## #4 — Sparse / variable-length grams (GitHub Blackbird style)

Observation: trigrams starting/ending at word boundaries or rare chars are
much more selective than interior common trigrams.

Blackbird uses "boundary-biased" variable-length tokens:
- Tokens start/end at whitespace, punctuation, or case transitions.
- This gives naturally rare tokens without frequency pruning.

Could replace or supplement the fixed trigram approach for ~10× better
selectivity on typical code search patterns.

## #5 — Content-in-index (for tiny files)

For files < 1KB, store the entire content in the index. Avoids disk reads
for small headers/config files that often dominate candidate lists.

## #6 — Shard index by content hash (Git blob SHA)

Like GitHub Blackbird: deduplicate files with identical content across branches/commits.
Relevant if trigrep is used in a monorepo with many similar files.

## #7 — Case-insensitive literal search

Currently `-F` is case-sensitive; `-i` not implemented.
For case-insensitive, normalize to lowercase at index time and at query time.
Alternatively, expand the query's trigrams to all case variants (2^3 = 8 per
trigram for ASCII alpha) and OR them — may still be worthwhile.
