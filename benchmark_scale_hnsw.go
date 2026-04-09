package main

import (
	"container/heap"
	"fmt"
	"math"
	"math/rand"
	"sort"
	"time"
)

// ============================================================================
// Priority Queue
// ============================================================================

type PQItem struct {
	dist  float64
	docid int
}
type MinPQ []PQItem

func (pq MinPQ) Len() int            { return len(pq) }
func (pq MinPQ) Less(i, j int) bool  { return pq[i].dist < pq[j].dist }
func (pq MinPQ) Swap(i, j int)       { pq[i], pq[j] = pq[j], pq[i] }
func (pq *MinPQ) Push(x interface{}) { *pq = append(*pq, x.(PQItem)) }
func (pq *MinPQ) Pop() interface{} {
	old := *pq
	n := len(old)
	item := old[n-1]
	*pq = old[:n-1]
	return item
}

// ============================================================================
// Stats
// ============================================================================

type SearchStats struct {
	DistCalcs    int64
	NodesVisited int64
	DescentDC    int64 // distance calcs during greedy descent only
}

func (s *SearchStats) Reset() {
	s.DistCalcs = 0
	s.NodesVisited = 0
	s.DescentDC = 0
}

type Neighbor struct {
	Docid    int
	Distance float64
}

// ============================================================================
// Vector Utilities
// ============================================================================

func ipDist(a, b []float32) float64 {
	var dot float64
	for i := range a {
		dot += float64(a[i]) * float64(b[i])
	}
	return 1.0 - dot
}

func normalize(v []float32) {
	var norm float64
	for _, x := range v {
		norm += float64(x) * float64(x)
	}
	norm = math.Sqrt(norm)
	if norm > 0 {
		for i := range v {
			v[i] = float32(float64(v[i]) / norm)
		}
	}
}

// ============================================================================
// Multi-Level HNSW Index
// ============================================================================

type HnswNode struct {
	Links [][]int // links[level] = list of neighbor docids
	Level int     // max level for this node
}

type HnswIndexML struct {
	Dim      int
	M        int
	MMax0    int // max links at level 0 (typically 2*M)
	EfC      int // ef during construction
	ML       float64
	Vecs     [][]float32
	Nodes    []HnswNode
	N        int
	EntryID  int
	MaxLevel int
	Stats    SearchStats
	counting bool
}

func NewHnswIndexML(dim, m int) *HnswIndexML {
	return &HnswIndexML{
		Dim:   dim,
		M:     m,
		MMax0: m * 2,
		EfC:   m * 2,
		ML:    1.0 / math.Log(float64(m)),
	}
}

func (h *HnswIndexML) randomLevel(rng *rand.Rand) int {
	return int(-math.Log(rng.Float64()) * h.ML)
}

func (h *HnswIndexML) distToDoc(query []float32, docid int) float64 {
	if h.counting {
		h.Stats.DistCalcs++
	}
	return ipDist(query, h.Vecs[docid])
}

func (h *HnswIndexML) distToDocDescent(query []float32, docid int) float64 {
	if h.counting {
		h.Stats.DistCalcs++
		h.Stats.DescentDC++
	}
	return ipDist(query, h.Vecs[docid])
}

// Build multi-level HNSW graph
func (h *HnswIndexML) Build(vectors [][]float32, rng *rand.Rand) {
	h.N = len(vectors)
	h.Vecs = vectors
	h.Nodes = make([]HnswNode, h.N)

	// Insert first node
	level0 := h.randomLevel(rng)
	h.Nodes[0] = HnswNode{
		Links: make([][]int, level0+1),
		Level: level0,
	}
	h.EntryID = 0
	h.MaxLevel = level0

	for i := 1; i < h.N; i++ {
		if i%100000 == 0 {
			fmt.Printf("  Inserted %d/%d nodes...\n", i, h.N)
		}

		nodeLevel := h.randomLevel(rng)
		h.Nodes[i] = HnswNode{
			Links: make([][]int, nodeLevel+1),
			Level: nodeLevel,
		}

		ep := h.EntryID
		epDist := ipDist(vectors[i], h.Vecs[ep])

		// Greedy descent through upper levels
		for lev := h.MaxLevel; lev > nodeLevel; lev-- {
			changed := true
			for changed {
				changed = false
				if lev <= h.Nodes[ep].Level {
					for _, nb := range h.Nodes[ep].Links[lev] {
						d := ipDist(vectors[i], h.Vecs[nb])
						if d < epDist {
							ep = nb
							epDist = d
							changed = true
						}
					}
				}
			}
		}

		// Insert at each level from nodeLevel down to 0
		for lev := min(nodeLevel, h.MaxLevel); lev >= 0; lev-- {
			// Beam search at this level
			maxLinks := h.M
			if lev == 0 {
				maxLinks = h.MMax0
			}

			pq := &MinPQ{{epDist, ep}}
			heap.Init(pq)
			visited := map[int]bool{ep: true}
			best := []PQItem{{epDist, ep}}

			for pq.Len() > 0 {
				item := heap.Pop(pq).(PQItem)
				if len(best) >= h.EfC && item.dist > best[h.EfC-1].dist {
					break
				}
				if lev <= h.Nodes[item.docid].Level {
					for _, nb := range h.Nodes[item.docid].Links[lev] {
						if !visited[nb] {
							visited[nb] = true
							d := ipDist(vectors[i], h.Vecs[nb])
							if len(best) < h.EfC || d < best[len(best)-1].dist {
								heap.Push(pq, PQItem{d, nb})
								best = append(best, PQItem{d, nb})
								sort.Slice(best, func(a, b int) bool { return best[a].dist < best[b].dist })
								if len(best) > h.EfC {
									best = best[:h.EfC]
								}
							}
						}
					}
				}
			}

			// Connect to nearest neighbors
			limit := maxLinks
			if limit > len(best) {
				limit = len(best)
			}
			neighbors := make([]int, limit)
			for k := 0; k < limit; k++ {
				neighbors[k] = best[k].docid
			}
			h.Nodes[i].Links[lev] = neighbors

			// Add reverse links (capped)
			for _, nb := range neighbors {
				if lev <= h.Nodes[nb].Level {
					if len(h.Nodes[nb].Links[lev]) < maxLinks {
						h.Nodes[nb].Links[lev] = append(h.Nodes[nb].Links[lev], i)
					} else {
						// Replace worst neighbor
						worstIdx := -1
						worstDist := 0.0
						for li, ln := range h.Nodes[nb].Links[lev] {
							d := ipDist(h.Vecs[nb], h.Vecs[ln])
							if d > worstDist {
								worstDist = d
								worstIdx = li
							}
						}
						d := ipDist(h.Vecs[nb], h.Vecs[i])
						if d < worstDist && worstIdx >= 0 {
							h.Nodes[nb].Links[lev][worstIdx] = i
						}
					}
				}
			}

			// Update entry point for next level
			if len(best) > 0 {
				ep = best[0].docid
				epDist = best[0].dist
			}
		}

		if nodeLevel > h.MaxLevel {
			h.EntryID = i
			h.MaxLevel = nodeLevel
		}
	}
}

// ============================================================================
// Search Methods
// ============================================================================

// greedyDescend: descend from entry through upper levels, return level-0 entry point
func (h *HnswIndexML) greedyDescend(query []float32) (int, float64) {
	ep := h.EntryID
	epDist := h.distToDocDescent(query, ep)

	for lev := h.MaxLevel; lev > 0; lev-- {
		changed := true
		for changed {
			changed = false
			if lev <= h.Nodes[ep].Level {
				for _, nb := range h.Nodes[ep].Links[lev] {
					d := h.distToDocDescent(query, nb)
					if d < epDist {
						ep = nb
						epDist = d
						changed = true
					}
				}
			}
		}
	}
	return ep, epDist
}

// beamSearchL0: standard beam search at level 0
func (h *HnswIndexML) beamSearchL0(query []float32, ep int, epDist float64, k, ef int) []PQItem {
	pq := &MinPQ{{epDist, ep}}
	heap.Init(pq)
	visited := map[int]bool{ep: true}
	best := []PQItem{{epDist, ep}}

	for pq.Len() > 0 {
		item := heap.Pop(pq).(PQItem)
		if len(best) >= ef && item.dist > best[ef-1].dist {
			break
		}
		h.Stats.NodesVisited++
		for _, nb := range h.Nodes[item.docid].Links[0] {
			if !visited[nb] {
				visited[nb] = true
				d := h.distToDoc(query, nb)
				if len(best) < ef || d < best[len(best)-1].dist {
					heap.Push(pq, PQItem{d, nb})
					best = append(best, PQItem{d, nb})
					sort.Slice(best, func(i, j int) bool { return best[i].dist < best[j].dist })
					if len(best) > ef {
						best = best[:ef]
					}
				}
			}
		}
	}
	return best
}

// SearchSingle: full independent search (descent + beam)
func (h *HnswIndexML) SearchSingle(query []float32, k, ef int) []Neighbor {
	ep, epDist := h.greedyDescend(query)
	best := h.beamSearchL0(query, ep, epDist, k, ef)
	if len(best) > k {
		best = best[:k]
	}
	result := make([]Neighbor, len(best))
	for i, b := range best {
		result[i] = Neighbor{b.docid, b.dist}
	}
	return result
}

// SearchSpeculative: anchor draft + seed verify + accept/reject per-query
func (h *HnswIndexML) SearchSpeculative(queries [][]float32, k, ef int) [][]Neighbor {
	n := len(queries)
	results := make([][]Neighbor, n)

	// DRAFT: full search with first query
	ep0, epDist0 := h.greedyDescend(queries[0])
	draftBest := h.beamSearchL0(queries[0], ep0, epDist0, ef, ef)

	best0 := draftBest
	if len(best0) > k {
		best0 = best0[:k]
	}
	results[0] = make([]Neighbor, len(best0))
	for i, b := range best0 {
		results[0][i] = Neighbor{b.docid, b.dist}
	}

	// Also get anchor's descent endpoint for similarity comparison
	anchorEP := ep0
	_ = anchorEP

	// VERIFY + ACCEPT/REJECT: each remaining query
	seedK := 20
	if seedK > len(draftBest) {
		seedK = len(draftBest)
	}
	for qi := 1; qi < n; qi++ {
		q := queries[qi]

		// Verify: find best seed from draft top-20
		bestSeedDist := math.Inf(1)
		for si := 0; si < seedK; si++ {
			d := h.distToDoc(q, draftBest[si].docid)
			if d < bestSeedDist {
				bestSeedDist = d
			}
		}

		// ACCEPT/REJECT decision:
		// Compare seed quality vs full descent quality
		// Do full descent to get the "correct" entry point
		fullEP, fullEPDist := h.greedyDescend(q)

		// Use whichever entry point is better
		ep := fullEP
		epDist := fullEPDist
		if bestSeedDist < fullEPDist {
			// ACCEPT: seed from draft is better than full descent
			// Find the seed docid again
			for si := 0; si < seedK; si++ {
				d := ipDist(q, h.Vecs[draftBest[si].docid])
				if math.Abs(d-bestSeedDist) < 1e-12 {
					ep = draftBest[si].docid
					epDist = bestSeedDist
					break
				}
			}
		}
		// If REJECT: we already paid for full descent, use it

		// Beam search from best entry point
		best := h.beamSearchL0(q, ep, epDist, k, ef)
		if len(best) > k {
			best = best[:k]
		}
		results[qi] = make([]Neighbor, len(best))
		for i, b := range best {
			results[qi][i] = Neighbor{b.docid, b.dist}
		}
	}
	return results
}

// SearchWarmChain: each query seeded by most similar previous query's result
func (h *HnswIndexML) SearchWarmChain(queries [][]float32, k, ef int) [][]Neighbor {
	n := len(queries)
	results := make([][]Neighbor, n)
	resultNodes := make([][]PQItem, n)

	// First query: full search
	ep0, epDist0 := h.greedyDescend(queries[0])
	resultNodes[0] = h.beamSearchL0(queries[0], ep0, epDist0, ef, ef)
	best0 := resultNodes[0]
	if len(best0) > k {
		best0 = best0[:k]
	}
	results[0] = make([]Neighbor, len(best0))
	for i, b := range best0 {
		results[0][i] = Neighbor{b.docid, b.dist}
	}

	for qi := 1; qi < n; qi++ {
		q := queries[qi]

		// Find most similar previous query
		bestPrev := 0
		bestSim := -math.MaxFloat64
		for pq := 0; pq < qi; pq++ {
			dot := 0.0
			for d := range q {
				dot += float64(q[d]) * float64(queries[pq][d])
			}
			if dot > bestSim {
				bestSim = dot
				bestPrev = pq
			}
		}

		// Seed from previous query's best result
		seedK := 10
		if seedK > len(resultNodes[bestPrev]) {
			seedK = len(resultNodes[bestPrev])
		}
		bestSeedDist := math.Inf(1)
		bestSeed := -1
		for si := 0; si < seedK; si++ {
			d := h.distToDoc(q, resultNodes[bestPrev][si].docid)
			if d < bestSeedDist {
				bestSeedDist = d
				bestSeed = resultNodes[bestPrev][si].docid
			}
		}

		// Full descent as fallback
		fullEP, fullEPDist := h.greedyDescend(q)

		// Use whichever is better
		ep := fullEP
		epDist := fullEPDist
		if bestSeedDist < fullEPDist {
			ep = bestSeed
			epDist = bestSeedDist
		}

		// Beam search
		resultNodes[qi] = h.beamSearchL0(q, ep, epDist, ef, ef)
		best := resultNodes[qi]
		if len(best) > k {
			best = best[:k]
		}
		results[qi] = make([]Neighbor, len(best))
		for i, b := range best {
			results[qi][i] = Neighbor{b.docid, b.dist}
		}
	}
	return results
}

// ============================================================================
// Evaluation
// ============================================================================

func bruteForceTopK(vecs [][]float32, query []float32, k int) []Neighbor {
	results := make([]Neighbor, len(vecs))
	for i, vec := range vecs {
		results[i] = Neighbor{i, ipDist(query, vec)}
	}
	sort.Slice(results, func(i, j int) bool { return results[i].Distance < results[j].Distance })
	if len(results) > k {
		results = results[:k]
	}
	return results
}

func recall(results []Neighbor, gt []Neighbor) float64 {
	gtSet := map[int]bool{}
	for _, nb := range gt {
		gtSet[nb.Docid] = true
	}
	hits := 0
	for _, nb := range results {
		if gtSet[nb.Docid] {
			hits++
		}
	}
	return float64(hits) / float64(len(gt))
}

func generateClusteredVectors(rng *rand.Rand, numDocs, dim, numClusters int) ([][]float32, [][]float32) {
	centers := make([][]float32, numClusters)
	for c := 0; c < numClusters; c++ {
		center := make([]float32, dim)
		for j := range center {
			center[j] = float32(rng.NormFloat64())
		}
		normalize(center)
		centers[c] = center
	}

	vectors := make([][]float32, numDocs)
	for i := 0; i < numDocs; i++ {
		clusterID := rng.Intn(numClusters)
		vec := make([]float32, dim)
		for j := range vec {
			vec[j] = centers[clusterID][j] + float32(rng.NormFloat64())*0.15
		}
		normalize(vec)
		vectors[i] = vec
	}
	return vectors, centers
}

func generateConcentratedQueries(rng *rand.Rand, centers [][]float32, dim, nQueries, nClusters int) [][]float32 {
	queries := make([][]float32, nQueries)
	perm := rng.Perm(len(centers))
	for i := 0; i < nQueries; i++ {
		cid := perm[i%nClusters]
		q := make([]float32, dim)
		for j := range q {
			q[j] = centers[cid][j] + float32(rng.NormFloat64())*0.1
		}
		normalize(q)
		queries[i] = q
	}
	return queries
}

func generateSpreadQueries(rng *rand.Rand, centers [][]float32, dim, nQueries int) [][]float32 {
	queries := make([][]float32, nQueries)
	perm := rng.Perm(len(centers))
	for i := 0; i < nQueries; i++ {
		cid := perm[i%len(perm)]
		q := make([]float32, dim)
		for j := range q {
			q[j] = centers[cid][j] + float32(rng.NormFloat64())*0.1
		}
		normalize(q)
		queries[i] = q
	}
	return queries
}

// ============================================================================
// Main
// ============================================================================

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func main() {
	const (
		NDOCS        = 500000
		DIM          = 128
		NUM_CLUSTERS = 50
		M            = 16
		EF           = 200
		K            = 20
		NINTERESTS   = 10
		NUM_USERS    = 3
	)

	rng := rand.New(rand.NewSource(42))

	fmt.Println("================================================================================")
	fmt.Println("Multi-Level HNSW Benchmark — Large Scale (500K, 128D)")
	fmt.Println("================================================================================")
	fmt.Printf("Config: %d docs, %d dim, %d clusters, M=%d, ef=%d, K=%d, %d interests\n",
		NDOCS, DIM, NUM_CLUSTERS, M, EF, K, NINTERESTS)
	fmt.Println()

	// Generate data
	fmt.Printf("Generating %d clustered vectors (%d clusters)... ", NDOCS, NUM_CLUSTERS)
	vectors, centers := generateClusteredVectors(rng, NDOCS, DIM, NUM_CLUSTERS)
	fmt.Println("done")

	// Build multi-level HNSW
	fmt.Printf("Building multi-level HNSW (M=%d)...\n", M)
	hnsw := NewHnswIndexML(DIM, M)
	t := time.Now()
	hnsw.Build(vectors, rng)
	buildTime := time.Since(t)
	fmt.Printf("  Build time: %.1fs, max level: %d\n", buildTime.Seconds(), hnsw.MaxLevel)

	// Count avg degree at level 0
	totalDeg := 0
	for i := 0; i < hnsw.N; i++ {
		if len(hnsw.Nodes[i].Links) > 0 {
			totalDeg += len(hnsw.Nodes[i].Links[0])
		}
	}
	fmt.Printf("  Avg L0 degree: %.1f\n", float64(totalDeg)/float64(hnsw.N))
	fmt.Println()

	// Query distributions
	type QuerySet struct {
		Name    string
		Queries [][]float32
	}

	for user := 0; user < NUM_USERS; user++ {
		querysets := []QuerySet{
			{"concentrated (2 clusters)", generateConcentratedQueries(rng, centers, DIM, NINTERESTS, 2)},
			{"spread (10 clusters)", generateSpreadQueries(rng, centers, DIM, NINTERESTS)},
		}

		for _, qs := range querysets {
			fmt.Println("################################################################################")
			fmt.Printf("User %d — %s\n", user+1, qs.Name)
			fmt.Println("################################################################################")

			queries := qs.Queries

			// Ground truth
			gts := make([][]Neighbor, NINTERESTS)
			for i, q := range queries {
				gts[i] = bruteForceTopK(vectors, q, K)
			}

			type Result struct {
				Name      string
				Recall    float64
				DistCalcs int64
				DescentDC int64
				BeamDC    int64
			}

			methods := []struct {
				Name string
				Run  func() [][]Neighbor
			}{
				{"Independent", func() [][]Neighbor {
					res := make([][]Neighbor, NINTERESTS)
					for qi, q := range queries {
						res[qi] = hnsw.SearchSingle(q, K, EF)
					}
					return res
				}},
				{"Speculative (anchor+seed)", func() [][]Neighbor {
					return hnsw.SearchSpeculative(queries, K, EF)
				}},
				{"Warm Chain", func() [][]Neighbor {
					return hnsw.SearchWarmChain(queries, K, EF)
				}},
			}

			var results []Result
			for _, m := range methods {
				hnsw.counting = true
				hnsw.Stats.Reset()
				res := m.Run()
				hnsw.counting = false

				avgRecall := 0.0
				for qi := range res {
					avgRecall += recall(res[qi], gts[qi])
				}
				avgRecall /= float64(NINTERESTS)

				results = append(results, Result{
					Name:      m.Name,
					Recall:    avgRecall,
					DistCalcs: hnsw.Stats.DistCalcs,
					DescentDC: hnsw.Stats.DescentDC,
					BeamDC:    hnsw.Stats.DistCalcs - hnsw.Stats.DescentDC,
				})
			}

			baseline := results[len(results)-3] // independent
			fmt.Printf("\n  %-28s %8s %10s %10s %10s %8s\n",
				"Method", "Recall", "TotalDC", "DescentDC", "BeamDC", "Speedup")
			fmt.Println("  " + repeatStr("-", 80))
			for _, r := range results {
				speedup := float64(baseline.DistCalcs) / float64(r.DistCalcs)
				fmt.Printf("  %-28s %8.4f %10d %10d %10d %7.2fx\n",
					r.Name, r.Recall, r.DistCalcs, r.DescentDC, r.BeamDC, speedup)
			}
			fmt.Println()
			results = results[:0]
		}
	}

	fmt.Println("================================================================================")
	fmt.Println("ANALYSIS")
	fmt.Println("================================================================================")
	fmt.Println()
	fmt.Println("  DescentDC = distance calcs during greedy descent (upper levels + L0 greedy)")
	fmt.Println("  BeamDC = distance calcs during beam search at level 0")
	fmt.Println("  Speculative/WarmChain save on DescentDC by reusing prev query's entry point")
	fmt.Println("  At 500K/128D scale, descent is a larger fraction of total cost")
}

func repeatStr(s string, n int) string {
	result := ""
	for i := 0; i < n; i++ {
		result += s
	}
	return result
}
