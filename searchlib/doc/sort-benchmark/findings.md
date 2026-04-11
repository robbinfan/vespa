# Sort optimization — harness findings

Captured by `sort_bench` on a 4-core Ubuntu box, g++ 13.3, `-O2`. All numbers
are best-of-5 microseconds for a single sort call, measured with reused
buffers (no per-call allocation noise).

See `baseline.txt` for raw output.

## Regime 1: cheap numeric fields (int32, ~1 ns per encode)

| N | K | F | baseline (us) | partial_sort | lazy | partial vs base | lazy vs base |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 10k | 10 | 1 | 1499 | 88 | 173 | **0.06** | 0.12 |
| 10k | 10 | 3 | 1560 | 190 | 170 | 0.12 | **0.11** |
| 100k | 10 | 3 | 20805 | 1904 | 2891 | **0.09** | 0.14 |
| 1M | 10 | 3 | 280696 | 19725 | 35364 | **0.07** | 0.13 |
| 1M | 1000 | 3 | 279050 | 21432 | 35330 | **0.08** | 0.13 |

**Finding:** `std::partial_sort` alone is a 10-30x improvement over full sort.
Lazy encoding is only a 7-10x improvement and **loses to partial_sort by ~2x**.
Reason: int32 encode is so cheap that lazy's scratch buffer + nth_element +
tie scan overhead eats the savings.

**Implication:** When fields are all cheap numeric, we should use `partial_sort`
and *not* bother with lazy encoding.

## Regime 2: expensive tail fields (field 0 cheap int32, tail fields ~= UCA)

Tail fields here burn 64 dependent loads per encode, simulating a UCA
collator / lowercased-string hash.

| N | K | F | baseline (us) | partial_sort | lazy | partial vs base | lazy vs base |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 10k | 10 | 2 | 3689 | 2148 | **160** | 0.58 | **0.04** |
| 10k | 10 | 3 | 6711 | 5383 | **175** | 0.80 | **0.03** |
| 100k | 10 | 3 | 73170 | 55124 | **2758** | 0.75 | **0.04** |
| 1M | 10 | 3 | 846150 | 574322 | **34090** | 0.68 | **0.04** |
| 1M | 100 | 3 | 852898 | 575108 | **34219** | 0.67 | **0.04** |

**Finding:** Lazy wins by a **17-25x** margin over both baseline and
partial_sort. Partial_sort alone only cuts ~30-40% because it still encodes
the expensive tail fields for all N hits. Lazy only pays the expensive cost
for the small candidate set.

**Implication:** When there is a cheap fixed-width first field followed by
expensive tail fields, lazy is the only path that avoids paying for the tail
fields at N-scale.

## Regime 3: low first-field cardinality (ties at pivot)

Column `card(f0) = 16` → field 0 only has 16 distinct values, so the pivot has
many ties and the candidate set blows up past the `4 * topn` threshold.

| N | K | card(f0) | baseline | partial | lazy | notes |
| --- | --- | --- | --- | --- | --- | --- |
| 100k | 100 | 16 | 19453 | 1521 | 23451 | **lazy-fallback, 21% slower** |
| 1M | 100 | 16 | 264890 | 14354 | 293920 | lazy-fallback, 11% slower |

**Finding:** The fallback path exists and produces correct results, but has a
real cost: we spent time encoding field 0 and running nth_element only to
throw it away. The `4 * topn` cap keeps this bounded to ~10-20% worst-case.

**Implication:** When the first field is suspected to have low cardinality
(or when we've fallen back in a recent call), prefer the partial_sort path
directly. This needs either schema hints, caller hints, or a simple adaptive
strategy (stick with whatever won last time).

## Correctness

`sort_bench --diff` runs 126 points across
`N ∈ {100, 1024, 10000, 50000}`, `K ∈ {1, 10, 100, 1000}`,
`F ∈ {1, 2, 3}`, `cardinality ∈ {uniform, 16, 1024}` and verifies that the
lazy path produces byte-identical top-K sortdata as the full-sort baseline.
All 126 cases pass.

## Port plan

Port both improvements to `FastS_SortSpec::sortResults` in sortresults.cpp:

```cpp
void FastS_SortSpec::sortResults(RankedHit a[], uint32_t n, uint32_t topn) {
    const bool smallTopK = (topn < n) && (n >= 256) && (topn * 4 <= n);

    if (smallTopK && has_multiple_attribute_fields() && first_field_is_fixed_width()) {
        if (try_lazy_topk(a, n, topn)) return;  // may fall back
    }

    // Phase 1 fallback: partial_sort over full encoding.
    initSortData(a, n);
    if (smallTopK) {
        run_partial_sort(topn);
    } else {
        run_legacy_sort();   // existing method 0/1/2 dispatch
    }
    // write-back loop (unchanged)
}
```

Key heuristic values (will tune against real proton workloads later):
- `n >= 256` — below this the overhead of any optimization swamps the work
- `topn * 4 <= n` — otherwise partial_sort has no room to beat full sort
- `candidate_set <= 4 * topn` — otherwise lazy fallback

The existing `_method` knob (0/1/2) is preserved as-is: if a caller
explicitly picks one of the legacy methods (e.g. tests), the optimization
decision still respects it.
