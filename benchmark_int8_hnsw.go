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
// Vector types and distance functions
// ============================================================================

type Neighbor struct {
	Docid    int
	Distance float64
}

func ipDistF32(a, b []float32) float64 {
	var dot float64
	for i := range a {
		dot += float64(a[i]) * float64(b[i])
	}
	return 1.0 - dot
}

// Int8 quantized distance — simulates VNNI-accelerated int8 dot product
func ipDistI8(a, b []int8) float64 {
	var dot int64
	for i := range a {
		dot += int64(a[i]) * int64(b[i])
	}
	// Normalize back: dot_f32 ≈ dot_i8 / (127*127)
	return 1.0 - float64(dot)/float64(127*127)
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

// Quantize normalized float32 vector to int8 (symmetric, scale=1/127)
// For normalized vectors, values are in [-1, 1], so just scale by 127
func quantizeI8(v []float32) []int8 {
	q := make([]int8, len(v))
	for i, x := range v {
		val := math.Round(float64(x) * 127.0)
		if val > 127 {
			val = 127
		}
		if val < -127 {
			val = -127
		}
		q[i] = int8(val)
	}
	return q
}

// ============================================================================
// Multi-Level HNSW Index with Int8 support
// ============================================================================

type HnswNode struct {
	Links [][]int
	Level int
}

type HnswIndex struct {
	Dim       int
	M         int
	MMax0     int
	EfC       int
	ML        float64
	VecsF32   [][]float32
	VecsI8    [][]int8
	Nodes     []HnswNode
	N         int
	EntryID   int
	MaxLevel  int

	// Stats
	DistCalcsF32 int64
	DistCalcsI8  int64
	counting     bool
}

func NewHnswIndex(dim, m int) *HnswIndex {
	return &HnswIndex{
		Dim:   dim,
		M:     m,
		MMax0: m * 2,
		EfC:   m * 2,
		ML:    1.0 / math.Log(float64(m)),
	}
}

func (h *HnswIndex) ResetStats() {
	h.DistCalcsF32 = 0
	h.DistCalcsI8 = 0
}

func (h *HnswIndex) distF32(a, b []float32) float64 {
	if h.counting {
		h.DistCalcsF32++
	}
	return ipDistF32(a, b)
}

func (h *HnswIndex) distI8(a, b []int8) float64 {
	if h.counting {
		h.DistCalcsI8++
	}
	return ipDistI8(a, b)
}

func (h *HnswIndex) randomLevel(rng *rand.Rand) int {
	return int(-math.Log(rng.Float64()) * h.ML)
}

func (h *HnswIndex) Build(vectors [][]float32, rng *rand.Rand) {
	h.N = len(vectors)
	h.VecsF32 = vectors
	h.VecsI8 = make([][]int8, h.N)
	for i := range vectors {
		h.VecsI8[i] = quantizeI8(vectors[i])
	}
	h.Nodes = make([]HnswNode, h.N)

	level0 := h.randomLevel(rng)
	h.Nodes[0] = HnswNode{Links: make([][]int, level0+1), Level: level0}
	h.EntryID = 0
	h.MaxLevel = level0

	for i := 1; i < h.N; i++ {
		if i%100000 == 0 {
			fmt.Printf("  Inserted %d/%d nodes...\n", i, h.N)
		}

		nodeLevel := h.randomLevel(rng)
		h.Nodes[i] = HnswNode{Links: make([][]int, nodeLevel+1), Level: nodeLevel}

		ep := h.EntryID
		epDist := ipDistF32(vectors[i], h.VecsF32[ep])

		for lev := h.MaxLevel; lev > nodeLevel; lev-- {
			changed := true
			for changed {
				changed = false
				if lev <= h.Nodes[ep].Level {
					for _, nb := range h.Nodes[ep].Links[lev] {
						d := ipDistF32(vectors[i], h.VecsF32[nb])
						if d < epDist {
							ep = nb
							epDist = d
							changed = true
						}
					}
				}
			}
		}

		for lev := min(nodeLevel, h.MaxLevel); lev >= 0; lev-- {
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
							d := ipDistF32(vectors[i], h.VecsF32[nb])
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

			limit := maxLinks
			if limit > len(best) {
				limit = len(best)
			}
			neighbors := make([]int, limit)
			for k := 0; k < limit; k++ {
				neighbors[k] = best[k].docid
			}
			h.Nodes[i].Links[lev] = neighbors

			for _, nb := range neighbors {
				if lev <= h.Nodes[nb].Level {
					if len(h.Nodes[nb].Links[lev]) < maxLinks {
						h.Nodes[nb].Links[lev] = append(h.Nodes[nb].Links[lev], i)
					} else {
						worstIdx := -1
						worstDist := 0.0
						for li, ln := range h.Nodes[nb].Links[lev] {
							d := ipDistF32(h.VecsF32[nb], h.VecsF32[ln])
							if d > worstDist {
								worstDist = d
								worstIdx = li
							}
						}
						d := ipDistF32(h.VecsF32[nb], h.VecsF32[i])
						if d < worstDist && worstIdx >= 0 {
							h.Nodes[nb].Links[lev][worstIdx] = i
						}
					}
				}
			}

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

// SearchF32: standard full-precision search
func (h *HnswIndex) SearchF32(query []float32, k, ef int) []Neighbor {
	queryI8 := quantizeI8(query) // not used, just for consistency
	_ = queryI8

	ep := h.EntryID
	epDist := h.distF32(query, h.VecsF32[ep])
	for lev := h.MaxLevel; lev > 0; lev-- {
		changed := true
		for changed {
			changed = false
			if lev <= h.Nodes[ep].Level {
				for _, nb := range h.Nodes[ep].Links[lev] {
					d := h.distF32(query, h.VecsF32[nb])
					if d < epDist {
						ep = nb
						epDist = d
						changed = true
					}
				}
			}
		}
	}

	pq := &MinPQ{{epDist, ep}}
	heap.Init(pq)
	visited := map[int]bool{ep: true}
	best := []PQItem{{epDist, ep}}

	for pq.Len() > 0 {
		item := heap.Pop(pq).(PQItem)
		if len(best) >= ef && item.dist > best[ef-1].dist {
			break
		}
		for _, nb := range h.Nodes[item.docid].Links[0] {
			if !visited[nb] {
				visited[nb] = true
				d := h.distF32(query, h.VecsF32[nb])
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

	if len(best) > k {
		best = best[:k]
	}
	result := make([]Neighbor, len(best))
	for i, b := range best {
		result[i] = Neighbor{b.docid, b.dist}
	}
	return result
}

// SearchI8Rerank: graph traversal with int8, rerank top candidates with float32
func (h *HnswIndex) SearchI8Rerank(query []float32, k, ef int) []Neighbor {
	queryI8 := quantizeI8(query)

	// Upper level descent with int8
	ep := h.EntryID
	epDist := h.distI8(queryI8, h.VecsI8[ep])
	for lev := h.MaxLevel; lev > 0; lev-- {
		changed := true
		for changed {
			changed = false
			if lev <= h.Nodes[ep].Level {
				for _, nb := range h.Nodes[ep].Links[lev] {
					d := h.distI8(queryI8, h.VecsI8[nb])
					if d < epDist {
						ep = nb
						epDist = d
						changed = true
					}
				}
			}
		}
	}

	// Level 0 beam search with int8
	pq := &MinPQ{{epDist, ep}}
	heap.Init(pq)
	visited := map[int]bool{ep: true}
	best := []PQItem{{epDist, ep}}

	for pq.Len() > 0 {
		item := heap.Pop(pq).(PQItem)
		if len(best) >= ef && item.dist > best[ef-1].dist {
			break
		}
		for _, nb := range h.Nodes[item.docid].Links[0] {
			if !visited[nb] {
				visited[nb] = true
				d := h.distI8(queryI8, h.VecsI8[nb])
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

	// RERANK: top candidates with full float32 precision
	for i := range best {
		best[i].dist = h.distF32(query, h.VecsF32[best[i].docid])
	}
	sort.Slice(best, func(i, j int) bool { return best[i].dist < best[j].dist })

	if len(best) > k {
		best = best[:k]
	}
	result := make([]Neighbor, len(best))
	for i, b := range best {
		result[i] = Neighbor{b.docid, b.dist}
	}
	return result
}

// SearchI8RerankOverfetch: same but overfetch (ef*2) with int8, rerank more candidates
func (h *HnswIndex) SearchI8RerankOverfetch(query []float32, k, ef int) []Neighbor {
	queryI8 := quantizeI8(query)
	efDraft := ef * 2 // overfetch to compensate for int8 ranking errors

	ep := h.EntryID
	epDist := h.distI8(queryI8, h.VecsI8[ep])
	for lev := h.MaxLevel; lev > 0; lev-- {
		changed := true
		for changed {
			changed = false
			if lev <= h.Nodes[ep].Level {
				for _, nb := range h.Nodes[ep].Links[lev] {
					d := h.distI8(queryI8, h.VecsI8[nb])
					if d < epDist {
						ep = nb
						epDist = d
						changed = true
					}
				}
			}
		}
	}

	pq := &MinPQ{{epDist, ep}}
	heap.Init(pq)
	visited := map[int]bool{ep: true}
	best := []PQItem{{epDist, ep}}

	for pq.Len() > 0 {
		item := heap.Pop(pq).(PQItem)
		if len(best) >= efDraft && item.dist > best[efDraft-1].dist {
			break
		}
		for _, nb := range h.Nodes[item.docid].Links[0] {
			if !visited[nb] {
				visited[nb] = true
				d := h.distI8(queryI8, h.VecsI8[nb])
				if len(best) < efDraft || d < best[len(best)-1].dist {
					heap.Push(pq, PQItem{d, nb})
					best = append(best, PQItem{d, nb})
					sort.Slice(best, func(i, j int) bool { return best[i].dist < best[j].dist })
					if len(best) > efDraft {
						best = best[:efDraft]
					}
				}
			}
		}
	}

	// Rerank ALL candidates with float32
	for i := range best {
		best[i].dist = h.distF32(query, h.VecsF32[best[i].docid])
	}
	sort.Slice(best, func(i, j int) bool { return best[i].dist < best[j].dist })

	if len(best) > k {
		best = best[:k]
	}
	result := make([]Neighbor, len(best))
	for i, b := range best {
		result[i] = Neighbor{b.docid, b.dist}
	}
	return result
}

// ============================================================================
// Evaluation
// ============================================================================

func bruteForceTopK(vecs [][]float32, query []float32, k int) []Neighbor {
	results := make([]Neighbor, len(vecs))
	for i, vec := range vecs {
		results[i] = Neighbor{i, ipDistF32(query, vec)}
	}
	sort.Slice(results, func(i, j int) bool { return results[i].Distance < results[j].Distance })
	if len(results) > k {
		results = results[:k]
	}
	return results
}

func calcRecall(results []Neighbor, gt []Neighbor) float64 {
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

// Measure rank correlation between int8 and float32 distances
func measureRankCorrelation(vecsF32 [][]float32, vecsI8 [][]int8, queries [][]float32, topK int) float64 {
	totalOverlap := 0.0
	for _, q := range queries {
		qI8 := quantizeI8(q)

		// Float32 top-K
		f32Dists := make([]PQItem, len(vecsF32))
		for i := range vecsF32 {
			f32Dists[i] = PQItem{ipDistF32(q, vecsF32[i]), i}
		}
		sort.Slice(f32Dists, func(i, j int) bool { return f32Dists[i].dist < f32Dists[j].dist })

		// Int8 top-K
		i8Dists := make([]PQItem, len(vecsI8))
		for i := range vecsI8 {
			i8Dists[i] = PQItem{ipDistI8(qI8, vecsI8[i]), i}
		}
		sort.Slice(i8Dists, func(i, j int) bool { return i8Dists[i].dist < i8Dists[j].dist })

		// Overlap
		f32Set := map[int]bool{}
		for i := 0; i < topK; i++ {
			f32Set[f32Dists[i].docid] = true
		}
		hits := 0
		for i := 0; i < topK; i++ {
			if f32Set[i8Dists[i].docid] {
				hits++
			}
		}
		totalOverlap += float64(hits) / float64(topK)
	}
	return totalOverlap / float64(len(queries))
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// ============================================================================
// Main
// ============================================================================

func main() {
	const (
		NDOCS        = 500000
		DIM          = 128
		NUM_CLUSTERS = 50
		M            = 16
		EF           = 200
		K            = 20
		NQUERIES     = 10
		NUM_USERS    = 3
	)

	rng := rand.New(rand.NewSource(42))

	fmt.Println("================================================================================")
	fmt.Println("Int8 Speculative Recall Benchmark (500K, 128D)")
	fmt.Println("================================================================================")
	fmt.Printf("Config: %d docs, %d dim, %d clusters, M=%d, ef=%d, K=%d\n",
		NDOCS, DIM, NUM_CLUSTERS, M, EF, K)
	fmt.Println()

	// Generate data
	fmt.Printf("Generating %d clustered vectors (%d clusters)... ", NDOCS, NUM_CLUSTERS)
	centers := make([][]float32, NUM_CLUSTERS)
	for c := 0; c < NUM_CLUSTERS; c++ {
		center := make([]float32, DIM)
		for j := range center {
			center[j] = float32(rng.NormFloat64())
		}
		normalize(center)
		centers[c] = center
	}
	vectors := make([][]float32, NDOCS)
	for i := 0; i < NDOCS; i++ {
		clusterID := rng.Intn(NUM_CLUSTERS)
		vec := make([]float32, DIM)
		for j := range vec {
			vec[j] = centers[clusterID][j] + float32(rng.NormFloat64())*0.15
		}
		normalize(vec)
		vectors[i] = vec
	}
	fmt.Println("done")

	// Quantize
	fmt.Print("Quantizing to int8... ")
	vecsI8 := make([][]int8, NDOCS)
	for i := range vectors {
		vecsI8[i] = quantizeI8(vectors[i])
	}
	fmt.Println("done")

	// === Phase 0: Rank correlation (brute force) ===
	fmt.Println()
	fmt.Println("=== Phase 0: Int8 vs Float32 Rank Correlation (brute force) ===")
	sampleQueries := make([][]float32, 5)
	for i := range sampleQueries {
		cid := rng.Intn(NUM_CLUSTERS)
		q := make([]float32, DIM)
		for j := range q {
			q[j] = centers[cid][j] + float32(rng.NormFloat64())*0.1
		}
		normalize(q)
		sampleQueries[i] = q
	}

	for _, topK := range []int{20, 100, 200} {
		corr := measureRankCorrelation(vectors, vecsI8, sampleQueries, topK)
		fmt.Printf("  Int8 vs F32 overlap @%d: %.1f%%\n", topK, corr*100)
	}

	// === Phase 1: Build HNSW ===
	fmt.Println()
	fmt.Printf("Building multi-level HNSW (M=%d)...\n", M)
	hnsw := NewHnswIndex(DIM, M)
	t := time.Now()
	hnsw.Build(vectors, rng)
	buildTime := time.Since(t)
	fmt.Printf("  Build time: %.1fs, max level: %d\n", buildTime.Seconds(), hnsw.MaxLevel)
	fmt.Println()

	// === Phase 2: Search comparison ===
	fmt.Println("=== Phase 2: HNSW Search — F32 vs Int8+Rerank ===")
	fmt.Println()

	for user := 0; user < NUM_USERS; user++ {
		// Generate queries
		queries := make([][]float32, NQUERIES)
		nClusters := 2 + rng.Intn(4) // 2-5 clusters
		perm := rng.Perm(NUM_CLUSTERS)
		for i := 0; i < NQUERIES; i++ {
			cid := perm[i%nClusters]
			q := make([]float32, DIM)
			for j := range q {
				q[j] = centers[cid][j] + float32(rng.NormFloat64())*0.1
			}
			normalize(q)
			queries[i] = q
		}

		// Ground truth
		gts := make([][]Neighbor, NQUERIES)
		for qi, q := range queries {
			gts[qi] = bruteForceTopK(vectors, q, K)
		}

		fmt.Printf("User %d (%d interest clusters):\n", user+1, nClusters)
		fmt.Printf("  %-30s %8s %10s %10s %10s\n",
			"Method", "Recall", "F32 DCs", "I8 DCs", "Total DC")
		fmt.Println("  " + repeatStr("-", 72))

		type MethodResult struct {
			Name     string
			Recall   float64
			F32DCs   int64
			I8DCs    int64
		}

		methods := []struct {
			Name string
			Run  func(q []float32) []Neighbor
		}{
			{"F32 (baseline)", func(q []float32) []Neighbor {
				return hnsw.SearchF32(q, K, EF)
			}},
			{"I8 + F32 rerank", func(q []float32) []Neighbor {
				return hnsw.SearchI8Rerank(q, K, EF)
			}},
			{"I8 + F32 rerank (2x overfetch)", func(q []float32) []Neighbor {
				return hnsw.SearchI8RerankOverfetch(q, K, EF)
			}},
		}

		for _, m := range methods {
			hnsw.counting = true
			hnsw.ResetStats()

			totalRecall := 0.0
			for qi, q := range queries {
				res := m.Run(q)
				totalRecall += calcRecall(res, gts[qi])
			}
			avgRecall := totalRecall / float64(NQUERIES)

			hnsw.counting = false

			fmt.Printf("  %-30s %8.4f %10d %10d %10d\n",
				m.Name, avgRecall, hnsw.DistCalcsF32, hnsw.DistCalcsI8,
				hnsw.DistCalcsF32+hnsw.DistCalcsI8)
		}
		fmt.Println()
	}

	fmt.Println("================================================================================")
	fmt.Println("ANALYSIS")
	fmt.Println("================================================================================")
	fmt.Println()
	fmt.Println("  F32 DC = float32 distance calcs (expensive, ~T ns)")
	fmt.Println("  I8 DC  = int8 distance calcs (cheap, ~T/4 ns with VNNI)")
	fmt.Println()
	fmt.Println("  Effective speedup = baseline_F32_DCs / (method_F32_DCs + method_I8_DCs/4)")
	fmt.Println("  (assuming int8 VNNI is 4x faster than float32)")
}

func repeatStr(s string, n int) string {
	result := ""
	for i := 0; i < n; i++ {
		result += s
	}
	return result
}
