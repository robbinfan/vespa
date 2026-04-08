#!/usr/bin/env python3
"""
HNSW Multi-Embedding Batch Search Simulator
=============================================
Simulates the core optimization: independent vs batch vs speculative HNSW search.

Measures:
- Distance calculations count (primary cost metric)
- Recall@k vs brute-force ground truth
- Wall-clock time
- Memory loads (doc vector accesses)
"""

import numpy as np
import time
from collections import defaultdict
from dataclasses import dataclass, field
from typing import List, Tuple, Set, Optional
import heapq

# ============================================================================
# HNSW Graph Implementation (simplified but faithful to Vespa's logic)
# ============================================================================

@dataclass
class HnswConfig:
    M: int = 16          # max links per node at level 0
    M_max: int = 16      # max links at construction
    ef_construction: int = 200
    ml: float = 1.0 / np.log(16)  # level multiplier

@dataclass
class SearchStats:
    distance_calcs: int = 0
    vector_loads: int = 0    # distinct doc vectors loaded from memory
    nodes_visited: int = 0

    def reset(self):
        self.distance_calcs = 0
        self.vector_loads = 0
        self.nodes_visited = 0

    def __repr__(self):
        return f"dist_calcs={self.distance_calcs}, vec_loads={self.vector_loads}, nodes_visited={self.nodes_visited}"


class SimpleHNSW:
    """Simplified HNSW index focusing on the search path."""

    def __init__(self, dim: int, config: HnswConfig = None):
        self.dim = dim
        self.cfg = config or HnswConfig()
        self.vectors = {}        # docid -> np.array
        self.graph = {}          # docid -> {level: [neighbor_docids]}
        self.max_level = {}      # docid -> max_level
        self.entry_point = None
        self.entry_level = -1
        self.stats = SearchStats()

    def _random_level(self):
        return int(-np.log(np.random.uniform()) * self.cfg.ml)

    def _distance(self, a: np.ndarray, b: np.ndarray) -> float:
        self.stats.distance_calcs += 1
        return float(np.sum((a - b) ** 2))  # squared euclidean

    def _distance_to_doc(self, query: np.ndarray, docid: int) -> float:
        self.stats.vector_loads += 1
        return self._distance(query, self.vectors[docid])

    def _distance_to_doc_batch(self, queries: List[np.ndarray], docid: int) -> List[float]:
        """Load doc vector ONCE, compute distances for all queries."""
        self.stats.vector_loads += 1  # ONE load instead of N
        doc_vec = self.vectors[docid]
        dists = []
        for q in queries:
            self.stats.distance_calcs += 1
            dists.append(float(np.sum((q - doc_vec) ** 2)))
        return dists

    def add(self, docid: int, vector: np.ndarray):
        self.vectors[docid] = vector
        level = self._random_level()
        self.max_level[docid] = level
        self.graph[docid] = {l: [] for l in range(level + 1)}

        if self.entry_point is None:
            self.entry_point = docid
            self.entry_level = level
            return

        # Find entry point by greedy descent
        ep = self.entry_point
        ep_dist = self._distance(vector, self.vectors[ep])

        for l in range(self.entry_level, level, -1):
            changed = True
            while changed:
                changed = False
                for neighbor in self.graph.get(ep, {}).get(l, []):
                    d = self._distance(vector, self.vectors[neighbor])
                    if d < ep_dist:
                        ep = neighbor
                        ep_dist = d
                        changed = True

        # Insert at each level
        for l in range(min(level, self.entry_level), -1, -1):
            # Search layer
            candidates = [(ep_dist, ep)]
            visited = {ep}
            best = [(ep_dist, ep)]

            while candidates:
                d, c = heapq.heappop(candidates)
                if best and d > best[-1][0] * 2:
                    break
                for neighbor in self.graph.get(c, {}).get(l, []):
                    if neighbor not in visited:
                        visited.add(neighbor)
                        nd = self._distance(vector, self.vectors[neighbor])
                        heapq.heappush(candidates, (nd, neighbor))
                        best.append((nd, neighbor))
                        best.sort()
                        if len(best) > self.cfg.ef_construction:
                            best = best[:self.cfg.ef_construction]

            # Select M nearest neighbors
            neighbors = [n for _, n in sorted(best)[:self.cfg.M]]
            self.graph[docid][l] = neighbors
            for n in neighbors:
                if l not in self.graph[n]:
                    self.graph[n][l] = []
                if len(self.graph[n][l]) < self.cfg.M:
                    self.graph[n][l].append(docid)

        if level > self.entry_level:
            self.entry_point = docid
            self.entry_level = level

    # ========================================================================
    # SEARCH METHODS
    # ========================================================================

    def _search_layer(self, query: np.ndarray, ep: int, ep_dist: float,
                      ef: int, level: int = 0) -> List[Tuple[float, int]]:
        """Standard single-query HNSW search at one level."""
        candidates = [(ep_dist, ep)]
        visited = {ep}
        best = [(ep_dist, ep)]

        while candidates:
            d, c = heapq.heappop(candidates)
            if best and len(best) >= ef and d > best[-1][0]:
                break
            self.stats.nodes_visited += 1
            for neighbor in self.graph.get(c, {}).get(level, []):
                if neighbor not in visited:
                    visited.add(neighbor)
                    nd = self._distance_to_doc(query, neighbor)
                    if len(best) < ef or nd < best[-1][0]:
                        heapq.heappush(candidates, (nd, neighbor))
                        best.append((nd, neighbor))
                        best.sort()
                        if len(best) > ef:
                            best = best[:ef]
        return best

    def search_single(self, query: np.ndarray, k: int, ef: int) -> List[Tuple[float, int]]:
        """Standard single-query search (baseline)."""
        if self.entry_point is None:
            return []

        ep = self.entry_point
        ep_dist = self._distance_to_doc(query, ep)

        # Descend through upper levels
        for l in range(self.entry_level, 0, -1):
            changed = True
            while changed:
                changed = False
                for neighbor in self.graph.get(ep, {}).get(l, []):
                    nd = self._distance_to_doc(query, neighbor)
                    if nd < ep_dist:
                        ep = neighbor
                        ep_dist = nd
                        changed = True

        # Search at level 0
        results = self._search_layer(query, ep, ep_dist, ef, level=0)
        return sorted(results)[:k]

    def search_batch(self, queries: List[np.ndarray], k: int, ef: int) -> List[List[Tuple[float, int]]]:
        """
        Batch search: shared visited set, unified candidate queue,
        batch distance computation.
        """
        if self.entry_point is None:
            return [[] for _ in queries]

        n = len(queries)

        # Shared entry point descent (use first query)
        ep = self.entry_point
        ep_dist = self._distance_to_doc(queries[0], ep)
        for l in range(self.entry_level, 0, -1):
            changed = True
            while changed:
                changed = False
                for neighbor in self.graph.get(ep, {}).get(l, []):
                    nd = self._distance_to_doc(queries[0], neighbor)
                    if nd < ep_dist:
                        ep = neighbor
                        ep_dist = nd
                        changed = True

        # Initialize per-query best lists
        all_best = []  # per-query: list of (dist, docid)
        for qi in range(n):
            d = self._distance_to_doc(queries[qi], ep) if qi > 0 else ep_dist
            all_best.append([(d, ep)])

        # Shared visited set + unified candidate queue
        visited = {ep}
        # Candidate queue: (min_dist_across_queries, docid)
        ep_dists = [all_best[qi][0][0] for qi in range(n)]
        min_ep_dist = min(ep_dists)
        candidates = [(min_ep_dist, ep)]
        limit_dists = [float('inf')] * n

        while candidates:
            min_d, c = heapq.heappop(candidates)
            # Stop if no query can benefit
            if min_d > max(limit_dists):
                break

            self.stats.nodes_visited += 1
            for neighbor in self.graph.get(c, {}).get(0, []):
                if neighbor not in visited:
                    visited.add(neighbor)
                    # BATCH distance: load vector ONCE
                    dists = self._distance_to_doc_batch(queries, neighbor)

                    min_dist = float('inf')
                    any_useful = False
                    for qi in range(n):
                        d = dists[qi]
                        min_dist = min(min_dist, d)
                        if d < limit_dists[qi] or len(all_best[qi]) < ef:
                            any_useful = True
                            all_best[qi].append((d, neighbor))
                            all_best[qi].sort()
                            if len(all_best[qi]) > ef:
                                all_best[qi] = all_best[qi][:ef]
                            if len(all_best[qi]) >= ef:
                                limit_dists[qi] = all_best[qi][-1][0]

                    if any_useful:
                        heapq.heappush(candidates, (min_dist, neighbor))

        return [sorted(best)[:k] for best in all_best]

    def search_speculative(self, queries: List[np.ndarray], k: int, ef: int,
                           draft_ef_ratio: float = 0.1) -> List[List[Tuple[float, int]]]:
        """
        Speculative ANN: Draft/Verify two-phase search.
        - Draft: small ef across all queries to find seed regions
        - Verify: full ef search from seed points
        """
        if self.entry_point is None:
            return [[] for _ in queries]

        n = len(queries)
        draft_ef = max(3, int(ef * draft_ef_ratio))

        # --- DRAFT PHASE: coarse batch search with small ef ---
        draft_results = self.search_batch(queries, k=draft_ef, ef=draft_ef)

        # Collect all unique seed points
        seed_points = set()
        for results in draft_results:
            for _, docid in results:
                seed_points.add(docid)

        # --- VERIFY PHASE: refined search from seed points ---
        # Shared entry point descent (reuse draft's work by starting from seeds)
        all_best = []
        for qi in range(n):
            best = []
            for docid in seed_points:
                d = self._distance_to_doc(queries[qi], docid)
                best.append((d, docid))
            best.sort()
            if len(best) > ef:
                best = best[:ef]
            all_best.append(best)

        # Shared visited set starting from seeds
        visited = set(seed_points)
        limit_dists = [float('inf')] * n
        for qi in range(n):
            if len(all_best[qi]) >= ef:
                limit_dists[qi] = all_best[qi][-1][0]

        # Build candidate queue from seeds
        candidates = []
        for docid in seed_points:
            min_d = min(all_best[qi][0][0] for qi in range(n) if all_best[qi])
            heapq.heappush(candidates, (min_d, docid))

        # Verify search
        while candidates:
            min_d, c = heapq.heappop(candidates)
            if min_d > max(limit_dists):
                break
            self.stats.nodes_visited += 1
            for neighbor in self.graph.get(c, {}).get(0, []):
                if neighbor not in visited:
                    visited.add(neighbor)
                    dists = self._distance_to_doc_batch(queries, neighbor)
                    min_dist = float('inf')
                    any_useful = False
                    for qi in range(n):
                        d = dists[qi]
                        min_dist = min(min_dist, d)
                        if d < limit_dists[qi] or len(all_best[qi]) < ef:
                            any_useful = True
                            all_best[qi].append((d, neighbor))
                            all_best[qi].sort()
                            if len(all_best[qi]) > ef:
                                all_best[qi] = all_best[qi][:ef]
                            if len(all_best[qi]) >= ef:
                                limit_dists[qi] = all_best[qi][-1][0]
                    if any_useful:
                        heapq.heappush(candidates, (min_dist, neighbor))

        return [sorted(best)[:k] for best in all_best]


# ============================================================================
# BENCHMARK
# ============================================================================

def brute_force_topk(vectors: dict, query: np.ndarray, k: int) -> List[Tuple[float, int]]:
    """Ground truth: exact brute-force search."""
    dists = []
    for docid, vec in vectors.items():
        d = float(np.sum((query - vec) ** 2))
        dists.append((d, docid))
    dists.sort()
    return dists[:k]


def compute_recall(result: List[Tuple[float, int]], ground_truth: List[Tuple[float, int]]) -> float:
    """Recall@k: fraction of ground truth results found."""
    gt_ids = {docid for _, docid in ground_truth}
    found_ids = {docid for _, docid in result}
    if not gt_ids:
        return 1.0
    return len(gt_ids & found_ids) / len(gt_ids)


def run_benchmark():
    np.random.seed(42)

    # Parameters
    N_DOCS = 10000         # number of documents in index
    DIM = 64               # vector dimension
    N_QUERIES = 10         # number of interest embeddings (MIND scenario)
    K = 20                 # top-k per query
    EF = 100               # exploration factor
    N_RUNS = 3             # repeat for stable timing

    print("=" * 80)
    print("HNSW Multi-Embedding Batch Search Benchmark")
    print("=" * 80)
    print(f"Documents: {N_DOCS}, Dimension: {DIM}, Queries: {N_QUERIES}, K: {K}, ef: {EF}")
    print()

    # Build index
    print("Building HNSW index...", end=" ", flush=True)
    t0 = time.time()
    hnsw = SimpleHNSW(DIM, HnswConfig(M=16, ef_construction=200))
    doc_vectors = {}
    for i in range(1, N_DOCS + 1):
        vec = np.random.randn(DIM).astype(np.float32)
        doc_vectors[i] = vec
        hnsw.add(i, vec)
    build_time = time.time() - t0
    print(f"done ({build_time:.1f}s)")

    # Generate query vectors (simulate MIND: cluster of interests)
    # Some queries are close together, some far apart (realistic)
    base_query = np.random.randn(DIM).astype(np.float32)
    queries = []
    for i in range(N_QUERIES):
        # Mix of close and distant interests
        if i < N_QUERIES // 2:
            # Close interest (small perturbation)
            q = base_query + np.random.randn(DIM).astype(np.float32) * 0.5
        else:
            # Distant interest (large perturbation)
            q = np.random.randn(DIM).astype(np.float32) * 2.0
        queries.append(q)

    # Ground truth (brute force)
    print("Computing ground truth (brute force)...", end=" ", flush=True)
    ground_truths = [brute_force_topk(doc_vectors, q, K) for q in queries]
    print("done")

    # ========================================================================
    # Method 1: Independent searches (current Vespa behavior)
    # ========================================================================
    print()
    print("-" * 70)
    print("Method 1: INDEPENDENT SEARCHES (current baseline)")
    print("-" * 70)

    total_stats = SearchStats()
    all_results_independent = []
    times = []

    for run in range(N_RUNS):
        hnsw.stats.reset()
        t0 = time.time()
        results = []
        for q in queries:
            r = hnsw.search_single(q, K, EF)
            results.append(r)
        elapsed = time.time() - t0
        times.append(elapsed)
        if run == 0:
            all_results_independent = results
            total_stats = SearchStats(
                distance_calcs=hnsw.stats.distance_calcs,
                vector_loads=hnsw.stats.vector_loads,
                nodes_visited=hnsw.stats.nodes_visited
            )

    recalls = [compute_recall(all_results_independent[i], ground_truths[i]) for i in range(N_QUERIES)]
    avg_recall = np.mean(recalls)
    avg_time = np.mean(times)

    print(f"  Avg recall@{K}:       {avg_recall:.4f}")
    print(f"  Distance calcs:      {total_stats.distance_calcs:,}")
    print(f"  Vector loads:        {total_stats.vector_loads:,}")
    print(f"  Nodes visited:       {total_stats.nodes_visited:,}")
    print(f"  Avg time:            {avg_time*1000:.1f}ms")

    baseline_dist_calcs = total_stats.distance_calcs
    baseline_vec_loads = total_stats.vector_loads
    baseline_time = avg_time
    baseline_recall = avg_recall

    # ========================================================================
    # Method 2: Batch search (shared visited set + batch distance)
    # ========================================================================
    print()
    print("-" * 70)
    print("Method 2: BATCH SEARCH (shared visited set + batch distance)")
    print("-" * 70)

    times = []
    all_results_batch = []

    for run in range(N_RUNS):
        hnsw.stats.reset()
        t0 = time.time()
        results = hnsw.search_batch(queries, K, EF)
        elapsed = time.time() - t0
        times.append(elapsed)
        if run == 0:
            all_results_batch = results
            batch_stats = SearchStats(
                distance_calcs=hnsw.stats.distance_calcs,
                vector_loads=hnsw.stats.vector_loads,
                nodes_visited=hnsw.stats.nodes_visited
            )

    recalls = [compute_recall(all_results_batch[i], ground_truths[i]) for i in range(N_QUERIES)]
    avg_recall_batch = np.mean(recalls)
    avg_time_batch = np.mean(times)

    print(f"  Avg recall@{K}:       {avg_recall_batch:.4f}")
    print(f"  Distance calcs:      {batch_stats.distance_calcs:,} ({batch_stats.distance_calcs/baseline_dist_calcs:.2f}x)")
    print(f"  Vector loads:        {batch_stats.vector_loads:,} ({batch_stats.vector_loads/baseline_vec_loads:.2f}x of baseline)")
    print(f"  Nodes visited:       {batch_stats.nodes_visited:,}")
    print(f"  Avg time:            {avg_time_batch*1000:.1f}ms ({baseline_time/avg_time_batch:.1f}x speedup)")
    print(f"  Recall delta:        {avg_recall_batch - baseline_recall:+.4f}")

    # ========================================================================
    # Method 3: Speculative search (Draft/Verify)
    # ========================================================================
    for draft_ratio in [0.1, 0.2, 0.3]:
        print()
        print("-" * 70)
        print(f"Method 3: SPECULATIVE SEARCH (draft_ef_ratio={draft_ratio})")
        print("-" * 70)

        times = []
        all_results_spec = []

        for run in range(N_RUNS):
            hnsw.stats.reset()
            t0 = time.time()
            results = hnsw.search_speculative(queries, K, EF, draft_ef_ratio=draft_ratio)
            elapsed = time.time() - t0
            times.append(elapsed)
            if run == 0:
                all_results_spec = results
                spec_stats = SearchStats(
                    distance_calcs=hnsw.stats.distance_calcs,
                    vector_loads=hnsw.stats.vector_loads,
                    nodes_visited=hnsw.stats.nodes_visited
                )

        recalls = [compute_recall(all_results_spec[i], ground_truths[i]) for i in range(N_QUERIES)]
        avg_recall_spec = np.mean(recalls)
        avg_time_spec = np.mean(times)

        print(f"  Avg recall@{K}:       {avg_recall_spec:.4f}")
        print(f"  Distance calcs:      {spec_stats.distance_calcs:,} ({spec_stats.distance_calcs/baseline_dist_calcs:.2f}x)")
        print(f"  Vector loads:        {spec_stats.vector_loads:,} ({spec_stats.vector_loads/baseline_vec_loads:.2f}x of baseline)")
        print(f"  Nodes visited:       {spec_stats.nodes_visited:,}")
        print(f"  Avg time:            {avg_time_spec*1000:.1f}ms ({baseline_time/avg_time_spec:.1f}x speedup)")
        print(f"  Recall delta:        {avg_recall_spec - baseline_recall:+.4f}")

    # ========================================================================
    # Summary
    # ========================================================================
    print()
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"{'Method':<35} {'Recall@'+str(K):<12} {'Dist Calcs':<15} {'Vec Loads':<15} {'Speedup':<10}")
    print("-" * 80)
    print(f"{'Independent (baseline)':<35} {baseline_recall:<12.4f} {baseline_dist_calcs:<15,} {baseline_vec_loads:<15,} {'1.0x':<10}")
    print(f"{'Batch (shared visited)':<35} {avg_recall_batch:<12.4f} {batch_stats.distance_calcs:<15,} {batch_stats.vector_loads:<15,} {f'{baseline_time/avg_time_batch:.1f}x':<10}")

    # Re-run speculative with 0.2 for summary
    hnsw.stats.reset()
    results_spec = hnsw.search_speculative(queries, K, EF, draft_ef_ratio=0.2)
    spec_stats_02 = SearchStats(
        distance_calcs=hnsw.stats.distance_calcs,
        vector_loads=hnsw.stats.vector_loads,
        nodes_visited=hnsw.stats.nodes_visited
    )
    recalls_spec = np.mean([compute_recall(results_spec[i], ground_truths[i]) for i in range(N_QUERIES)])

    print()
    print("KEY INSIGHT: Vector loads (memory bandwidth) is the dominant cost in")
    print("real HNSW. Batch search loads each doc vector ONCE instead of N times.")
    print(f"With {N_QUERIES} queries: {baseline_vec_loads/batch_stats.vector_loads:.1f}x fewer vector loads in batch mode.")


if __name__ == "__main__":
    run_benchmark()
