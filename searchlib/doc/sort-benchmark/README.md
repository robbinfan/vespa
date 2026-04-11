# sort-benchmark — standalone L1 harness for sort optimization

This is **not** part of the Vespa build. It is a self-contained C++ program used
to prototype, benchmark and differentially test the lazy / top-K optimizations
planned for `FastS_SortSpec` in `searchlib/src/vespa/searchlib/common/sortresults.cpp`.

## Why a standalone harness

Vespa's full C++ build needs `/opt/vespa-deps` (custom protobuf / gtest / openssl-fips
/ etc.). In environments where that is not available, the real
`sortresults.cpp` cannot be compiled. The algorithmic hot path of sort, however,
is pure C++: `convertForSort<T, asc>` + `serializeForSort<C>` + a `memcmp`
comparator. The harness re-implements just those primitives so we can iterate
on the algorithm locally with plain `g++ -O2 -std=c++17`.

Code changes validated here must still be mirrored into `sortresults.cpp` and
validated with the real `searchlib` unit tests (`sortresults_test`,
`multilevelsort_test`) before landing.

## Build & run

```bash
cd searchlib/doc/sort-benchmark
g++ -O2 -std=c++17 -Wall -Wextra -o sort_bench sort_bench.cpp
./sort_bench                      # default grid
./sort_bench --diff               # differential correctness test only
./sort_bench --n 1000000 --k 20   # single point benchmark
```

## What it measures

For each point `(N, K, field_count, first_field_cardinality)`:

1. **Baseline (legacy-equivalent)** — full encode of all N hits, `std::sort`
   with `memcmp` comparator. This matches what `sortresults.cpp` does when
   `_method == 1`. Radix sort has different constants but the same asymptotic
   shape as a partial sort cut-off; we keep `std::sort` as the reference since
   it lets us A/B compare with nothing else changing.
2. **partial_sort** — full encode, `std::partial_sort` for the top K. Phase 1.
3. **lazy top-K** — encode field 0 for all N hits into a scratch buffer, pick
   the candidate set `C = {h : field0(h) <= pivot_K}` via `std::nth_element`,
   then encode remaining fields for `C` only, full-sort `C`. Phase 1 + Phase 2.

The harness also runs a **differential test**: for every (N, K) point it
verifies that `top-K(lazy) == top-K(baseline)` byte-for-byte on the final
sortdata of the K winners.

## Layout

- `sort_bench.cpp` — single-file harness: encoding primitives, fake attribute
  storage, three sort paths, benchmark harness, differential test.
- `build.sh` — convenience wrapper.
- `baseline.txt` — captured baseline timings on the dev machine (for reference).

## Caveats

- No UCA / lowercase converters. The harness only exercises fixed-width numeric
  fields. That is intentional: the lazy-encoding optimization deliberately
  falls back to the legacy path for variable-width first fields. Correctness
  for strings/UCA is covered by the in-tree `multilevelsort_test` once changes
  are mirrored back.
- Single-threaded. Sort in proton runs inside a match thread; we are optimizing
  per-thread latency, not throughput.
- The harness uses `std::sort` as the "legacy" baseline rather than the
  in-tree radix sort. Radix has better constants for very large N and simple
  keys, but the comparison we care about is `Nlog N` vs `N + K log K`, which
  `std::sort` already captures. See `baseline.txt` for empirical numbers.
