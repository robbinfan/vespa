# Handoff: Adaptive HNSW explore_k for Filtered ANN Queries

## Background

VSAG (VLDB'25) identifies a recall degradation pattern in HNSW with pre-filters:
when a global filter removes most candidates from the beam, the *effective*
`ef_search` shrinks and recall drops silently. Vespa's `hnsw.exploreAdditionalHits`
is static, requiring users to manually over-provision for worst-case selectivity.

This work introduces **two complementary tracks** to automate ef tuning:

| Track | Scope | Latency | Status |
|-------|-------|---------|--------|
| **A: Engine-internal** | Per-query, selectivity-based | Zero (planning phase) | **Implemented** |
| **B: External control-loop agent** | Per-rank-profile, metrics-driven | Seconds (feedback loop) | **Designed, not implemented** |

---

## Track A: What Was Done

### Changes (commit `5cfd4ac5`)

| File | Change |
|------|--------|
| `nearest_neighbor_blueprint.h` | Added `_global_filter_hit_ratio` member |
| `nearest_neighbor_blueprint.cpp` | Added `compute_adaptive_explore()` helper; `set_global_filter()` stores selectivity; `perform_top_k()` uses adaptive extras on filter branch |
| `tensorattribute_test.cpp` | Mock captures `explore_k`; 4 new tests: no-filter, weak, strong (capped), full-filter |

### Core Formula

```
factor         = min(1.0 / selectivity, 4.0)          // mult cap
target_ef      = (k + base_extra) * factor
adaptive_extra = clamp(target_ef - k, base_extra, 2000) // floor + abs cap
```

### Invariants

- **No filter / selectivity >= 1.0**: `adaptive_extra == base_extra` (bit-exact backward compat)
- **base_extra is a floor**: user/Track-B value is never reduced
- **abs cap 2000**: prevents degenerate blowup at extreme selectivity
- **Brute-force path untouched**: selectivity below `_brute_force_limit` already switches to brute force upstream; adaptive logic never fires

### Rollback

- `git revert <commit>`, or
- Set `kEnableAdaptiveExplore = false` in `nearest_neighbor_blueprint.cpp` and rebuild

---

## Track B: What Was Designed (Not Implemented)

An external agent reads Vespa metrics and adjusts the per-rank-profile **baseline**
`hnsw.exploreAdditionalHits` via a custom `EfAutotuneSearcher` in the container.

### Available Signals

| Metric | Endpoint |
|--------|----------|
| `content.proton.documentdb.matching.rank_profile.query_latency` (p95/p99) | `/prometheus/v1/values` |
| `content.proton.documentdb.matching.rank_profile.docs_matched.rate` | same |
| `degraded_queries.rate`, `empty_results.rate` | same |

### Architecture

```
Control-Loop Agent  --pull--> /prometheus/v1/values
                    --push--> EfAutotuneSearcher (in-memory map, per rank-profile)
                              injects hnsw.exploreAdditionalHits into queries
```

### Implementation Needed

1. `EfAutotuneSearcher.java` -- container Searcher that injects `exploreAdditionalHits` annotation
2. `EfAutotuneHandler.java` -- REST endpoint `POST /autotune/ef` to receive agent updates
3. External agent (Python/Go) -- reads metrics, runs control law, pushes new baselines

### Why Both Tracks

- Track A handles **per-query** selectivity variation (filter changes every request)
- Track B handles **workload-level** drift (right baseline depends on SLA, hardware, doc count)
- They compose: agent tunes `base_extra`, engine scales it per-query

---

## Testing Checklist

### Done
- [x] Standalone helper logic validated (11 assertions, `-Wall -Wextra -Werror`)
- [x] 4 new blueprint tests covering all branches

### Needs CI
- [ ] Full Vespa C++ build (`searchlib` module)
- [ ] Existing NN blueprint tests still pass (no signature/behavior change for no-filter path)
- [ ] HNSW index tests in `searchlib/src/tests/tensor/hnsw_index/`

### Recommended Follow-up
- [ ] Recall benchmark: selectivity in {0.9, 0.5, 0.25, 0.1}, compare adaptive on/off
- [ ] Latency benchmark: confirm overhead is within `mult_cap` factor
- [ ] Track B prototype on a canary cluster

---

## Key Design Decisions

1. **Planning-phase only**: all logic runs in `set_global_filter()` -> `perform_top_k()`, never in the HNSW hot loop
2. **No new config knobs**: the constants (`mult_cap=4`, `abs_cap=2000`) are compile-time; tuning via config can be added later if needed
3. **No public API change**: `hnsw.exploreAdditionalHits` semantics unchanged for callers
4. **Division-by-zero guard**: `est_hits == 0` now returns `1.0` instead of dividing

---

## Files Reference

- Plan: `/root/.claude/plans/cryptic-tickling-island.md`
- Source: `searchlib/src/vespa/searchlib/queryeval/nearest_neighbor_blueprint.{h,cpp}`
- Tests: `searchlib/src/tests/attribute/tensorattribute/tensorattribute_test.cpp`
