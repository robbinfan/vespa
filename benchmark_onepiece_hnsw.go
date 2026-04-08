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
// Stats & Types
// ============================================================================

type SearchStats struct {
	DistCalcs    int64
	VecLoads     int64
	NodesVisited int64
}

func (s *SearchStats) Reset() {
	s.DistCalcs = 0
	s.VecLoads = 0
	s.NodesVisited = 0
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
// OnePiece Progressive Embedding Generation
// ============================================================================

// generateOnePieceEmbeddings generates 6 progressive embeddings for a user.
// Each step is a refinement of the previous -- same semantic direction,
// progressively more precise. sigma controls inter-step noise.
func generateOnePieceEmbeddings(rng *rand.Rand, baseCenter []float32, dim, nSteps int, stepSigma float32) [][]float32 {
	embeddings := make([][]float32, nSteps)
	// Start from base interest direction
	current := make([]float32, dim)
	copy(current, baseCenter)
	// Add initial user-level noise
	for j := range current {
		current[j] += float32(rng.NormFloat64()) * 0.15
	}
	normalize(current)

	for s := 0; s < nSteps; s++ {
		emb := make([]float32, dim)
		copy(emb, current)
		// Each step adds small refinement noise (decreasing with step)
		noise := stepSigma / float32(1+s) // later steps = smaller perturbation
		for j := range emb {
			emb[j] += float32(rng.NormFloat64()) * noise
		}
		normalize(emb)
		embeddings[s] = emb
		// Progressive refinement: next step starts from current output
		copy(current, emb)
	}
	return embeddings
}

// ============================================================================
// HNSW Index
// ============================================================================

type HnswIndex struct {
	Dim      int
	M        int
	Vecs     [][]float32
	Links    [][]int
	N        int
	Stats    SearchStats
	Entry    int
	counting bool
}

func NewHnswIndex(dim, m int) *HnswIndex {
	return &HnswIndex{Dim: dim, M: m, Entry: -1}
}

func (h *HnswIndex) distToDoc(query []float32, docid int) float64 {
	if h.counting {
		h.Stats.VecLoads++
		h.Stats.DistCalcs++
	}
	return ipDist(query, h.Vecs[docid])
}

func (h *HnswIndex) distToDocBatch(queries [][]float32, docid int) []float64 {
	if h.counting {
		h.Stats.VecLoads++
	}
	doc := h.Vecs[docid]
	dists := make([]float64, len(queries))
	for qi, q := range queries {
		if h.counting {
			h.Stats.DistCalcs++
		}
		dists[qi] = ipDist(q, doc)
	}
	return dists
}

func (h *HnswIndex) BuildNSW(vectors [][]float32) {
	h.N = len(vectors)
	h.Vecs = vectors
	h.Links = make([][]int, h.N)
	h.Entry = 0
	h.Links[0] = nil

	efC := h.M * 2
	if efC < 64 {
		efC = 64
	}

	for i := 1; i < h.N; i++ {
		if i%100000 == 0 {
			fmt.Printf("  Inserted %d/%d nodes...\n", i, h.N)
		}

		ep := h.Entry
		epDist := ipDist(vectors[i], h.Vecs[ep])
		changed := true
		for changed {
			changed = false
			for _, nb := range h.Links[ep] {
				d := ipDist(vectors[i], h.Vecs[nb])
				if d < epDist {
					ep = nb
					epDist = d
					changed = true
				}
			}
		}

		pq := &MinPQ{{epDist, ep}}
		heap.Init(pq)
		visited := map[int]bool{ep: true}
		best := []PQItem{{epDist, ep}}

		for pq.Len() > 0 {
			item := heap.Pop(pq).(PQItem)
			if len(best) >= efC && item.dist > best[efC-1].dist {
				break
			}
			for _, nb := range h.Links[item.docid] {
				if !visited[nb] {
					visited[nb] = true
					d := ipDist(vectors[i], h.Vecs[nb])
					if len(best) < efC || d < best[len(best)-1].dist {
						heap.Push(pq, PQItem{d, nb})
						best = append(best, PQItem{d, nb})
						sort.Slice(best, func(a, b int) bool { return best[a].dist < best[b].dist })
						if len(best) > efC {
							best = best[:efC]
						}
					}
				}
			}
		}

		limit := h.M
		if limit > len(best) {
			limit = len(best)
		}
		neighbors := make([]int, limit)
		for k := 0; k < limit; k++ {
			neighbors[k] = best[k].docid
		}
		h.Links[i] = neighbors

		for _, nb := range neighbors {
			if len(h.Links[nb]) < h.M*2 {
				h.Links[nb] = append(h.Links[nb], i)
			} else {
				worstIdx := -1
				worstDist := 0.0
				for li, ln := range h.Links[nb] {
					d := ipDist(h.Vecs[nb], h.Vecs[ln])
					if d > worstDist {
						worstDist = d
						worstIdx = li
					}
				}
				d := ipDist(h.Vecs[nb], h.Vecs[i])
				if d < worstDist && worstIdx >= 0 {
					h.Links[nb][worstIdx] = i
				}
			}
		}
	}
}

// ============================================================================
// Search Methods
// ============================================================================

func (h *HnswIndex) SearchSingle(query []float32, k, ef int) []Neighbor {
	h.counting = true
	defer func() { h.counting = false }()

	ep := h.Entry
	epDist := h.distToDoc(query, ep)
	changed := true
	for changed {
		changed = false
		for _, nb := range h.Links[ep] {
			d := h.distToDoc(query, nb)
			if d < epDist {
				ep = nb
				epDist = d
				changed = true
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
		h.Stats.NodesVisited++
		for _, nb := range h.Links[item.docid] {
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

	if len(best) > k {
		best = best[:k]
	}
	result := make([]Neighbor, len(best))
	for i, b := range best {
		result[i] = Neighbor{b.docid, b.dist}
	}
	return result
}

func (h *HnswIndex) SearchBatch(queries [][]float32, k, ef int) [][]Neighbor {
	h.counting = true
	defer func() { h.counting = false }()

	n := len(queries)
	ep := h.Entry
	epDist := h.distToDoc(queries[0], ep)
	changed := true
	for changed {
		changed = false
		for _, nb := range h.Links[ep] {
			d := h.distToDoc(queries[0], nb)
			if d < epDist {
				ep = nb
				epDist = d
				changed = true
			}
		}
	}

	allBest := make([][]PQItem, n)
	for qi := 0; qi < n; qi++ {
		var d float64
		if qi == 0 {
			d = epDist
		} else {
			d = h.distToDoc(queries[qi], ep)
		}
		allBest[qi] = []PQItem{{d, ep}}
	}

	visited := make(map[int]bool, ef*2)
	visited[ep] = true
	limitDists := make([]float64, n)
	for i := range limitDists {
		limitDists[i] = math.Inf(1)
	}

	minEpDist := math.Inf(1)
	for qi := 0; qi < n; qi++ {
		if allBest[qi][0].dist < minEpDist {
			minEpDist = allBest[qi][0].dist
		}
	}
	cands := &MinPQ{{minEpDist, ep}}
	heap.Init(cands)

	for cands.Len() > 0 {
		item := heap.Pop(cands).(PQItem)
		allLimitsMet := true
		for qi := 0; qi < n; qi++ {
			if item.dist <= limitDists[qi] {
				allLimitsMet = false
				break
			}
		}
		if allLimitsMet && limitDists[0] < math.Inf(1) {
			break
		}
		h.Stats.NodesVisited++

		for _, nb := range h.Links[item.docid] {
			if !visited[nb] {
				visited[nb] = true
				dists := h.distToDocBatch(queries, nb)
				minD := math.Inf(1)
				anyUseful := false
				for qi := 0; qi < n; qi++ {
					d := dists[qi]
					if d < minD {
						minD = d
					}
					if len(allBest[qi]) < ef || d < limitDists[qi] {
						anyUseful = true
						allBest[qi] = append(allBest[qi], PQItem{d, nb})
						sort.Slice(allBest[qi], func(i, j int) bool {
							return allBest[qi][i].dist < allBest[qi][j].dist
						})
						if len(allBest[qi]) > ef {
							allBest[qi] = allBest[qi][:ef]
						}
						if len(allBest[qi]) >= ef {
							limitDists[qi] = allBest[qi][ef-1].dist
						}
					}
				}
				if anyUseful {
					heap.Push(cands, PQItem{minD, nb})
				}
			}
		}
	}

	results := make([][]Neighbor, n)
	for qi := 0; qi < n; qi++ {
		best := allBest[qi]
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

// SearchProgressive: OnePiece progressive retrieval
// Phase 1: Batch HNSW search using early-step (coarse) embeddings
// Phase 2: Brute-force re-rank candidates using later-step (fine) embeddings
func (h *HnswIndex) SearchProgressive(allStepEmbeddings [][]float32, k, ef, draftSteps int) [][]Neighbor {
	nSteps := len(allStepEmbeddings)
	if draftSteps >= nSteps {
		draftSteps = nSteps
	}

	// Phase 1: HNSW search using draft (early) step embeddings
	draftQueries := allStepEmbeddings[:draftSteps]
	draftK := k * 3 // retrieve wider candidate set for re-ranking
	if draftK < ef/2 {
		draftK = ef / 2
	}

	var draftResults [][]Neighbor
	if draftSteps == 1 {
		draftResults = [][]Neighbor{h.SearchSingle(draftQueries[0], draftK, ef)}
	} else {
		draftResults = h.SearchBatch(draftQueries, draftK, ef)
	}

	// Collect candidate union from draft results
	candidateSet := map[int]bool{}
	for _, res := range draftResults {
		for _, nb := range res {
			candidateSet[nb.Docid] = true
		}
	}
	candidates := make([]int, 0, len(candidateSet))
	for docid := range candidateSet {
		candidates = append(candidates, docid)
	}

	// Phase 2: Re-rank candidates using ALL step embeddings (brute force)
	h.counting = true
	defer func() { h.counting = false }()

	results := make([][]Neighbor, nSteps)
	for qi := 0; qi < nSteps; qi++ {
		query := allStepEmbeddings[qi]
		ranked := make([]Neighbor, len(candidates))
		for ci, docid := range candidates {
			ranked[ci] = Neighbor{docid, h.distToDoc(query, docid)}
		}
		sort.Slice(ranked, func(i, j int) bool { return ranked[i].Distance < ranked[j].Distance })
		if len(ranked) > k {
			ranked = ranked[:k]
		}
		results[qi] = ranked
	}
	return results
}

// ============================================================================
// Evaluation Utilities
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

func computeRecall(result []Neighbor, gt []Neighbor) float64 {
	gtSet := map[int]bool{}
	for _, nb := range gt {
		gtSet[nb.Docid] = true
	}
	overlap := 0
	for _, nb := range result {
		if gtSet[nb.Docid] {
			overlap++
		}
	}
	if len(gtSet) == 0 {
		return 1.0
	}
	return float64(overlap) / float64(len(gtSet))
}

func computeUnionRecall(results [][]Neighbor, gts [][]Neighbor) float64 {
	gtUnion := map[int]bool{}
	for _, gt := range gts {
		for _, nb := range gt {
			gtUnion[nb.Docid] = true
		}
	}
	resultUnion := map[int]bool{}
	for _, res := range results {
		for _, nb := range res {
			resultUnion[nb.Docid] = true
		}
	}
	overlap := 0
	for docid := range gtUnion {
		if resultUnion[docid] {
			overlap++
		}
	}
	if len(gtUnion) == 0 {
		return 1.0
	}
	return float64(overlap) / float64(len(gtUnion))
}

// ============================================================================
// Main
// ============================================================================

func main() {
	const (
		NDOCS        = 500000  // ~50万 per shard (simulate 1 shard of 21)
		DIM          = 64
		NUM_CLUSTERS = 50
		NSTEPS       = 6      // OnePiece 6 progressive steps
		K            = 20
		EF           = 200
		M            = 32
		NRUNS        = 3
		NUM_USERS    = 5
		STEP_SIGMA   = float32(0.08) // inter-step noise (small = high overlap)
	)

	const cacheMissNs = 40.0
	const distCalcNs = 2.0

	rng := rand.New(rand.NewSource(42))

	fmt.Println("================================================================================")
	fmt.Println("OnePiece Progressive Retrieval Benchmark")
	fmt.Println("================================================================================")
	fmt.Printf("Config: %d docs, %d dim, %d clusters, M=%d, ef=%d, K=%d\n",
		NDOCS, DIM, NUM_CLUSTERS, M, EF, K)
	fmt.Printf("        %d progressive steps, step_sigma=%.2f, %d users\n",
		NSTEPS, STEP_SIGMA, NUM_USERS)
	fmt.Println()

	// Generate clustered item embeddings
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

	// Check embedding similarity between progressive steps (sample user)
	sampleEmbs := generateOnePieceEmbeddings(rng, centers[0], DIM, NSTEPS, STEP_SIGMA)
	fmt.Println("\n  Progressive embedding similarity (sample user):")
	fmt.Printf("  Step pair     dot product    IP distance\n")
	for i := 0; i < NSTEPS; i++ {
		for j := i + 1; j < NSTEPS; j++ {
			var dot float64
			for d := range sampleEmbs[i] {
				dot += float64(sampleEmbs[i][d]) * float64(sampleEmbs[j][d])
			}
			fmt.Printf("  step%d-step%d:  %.4f         %.4f\n", i+1, j+1, dot, 1.0-dot)
		}
	}

	// Build NSW graph
	fmt.Println("\nBuilding NSW graph:")
	t0 := time.Now()
	hnsw := NewHnswIndex(DIM, M)
	hnsw.BuildNSW(vectors)
	buildTime := time.Since(t0)
	fmt.Printf("  Build time: %.1fs\n", buildTime.Seconds())
	avgDeg := 0.0
	for i := 0; i < hnsw.N; i++ {
		avgDeg += float64(len(hnsw.Links[i]))
	}
	fmt.Printf("  Avg degree: %.1f\n", avgDeg/float64(hnsw.N))

	// Generate user query sets
	fmt.Printf("\nGenerating %d user query sets (%d steps each)...\n", NUM_USERS, NSTEPS)
	type UserData struct {
		Embeddings   [][]float32   // 6 progressive embeddings
		GroundTruths [][]Neighbor  // per-step ground truth
	}
	var users []UserData
	for u := 0; u < NUM_USERS; u++ {
		cid := rng.Intn(NUM_CLUSTERS)
		embs := generateOnePieceEmbeddings(rng, centers[cid], DIM, NSTEPS, STEP_SIGMA)
		gts := make([][]Neighbor, NSTEPS)
		for s, q := range embs {
			gts[s] = bruteForceTopK(vectors, q, K)
		}
		users = append(users, UserData{Embeddings: embs, GroundTruths: gts})
	}

	// Check ground truth overlap between steps (should be high for OnePiece)
	avgStepOverlap := 0.0
	overlapCount := 0
	for _, u := range users {
		for i := 0; i < NSTEPS; i++ {
			for j := i + 1; j < NSTEPS; j++ {
				set1 := map[int]bool{}
				for _, nb := range u.GroundTruths[i] {
					set1[nb.Docid] = true
				}
				overlap := 0
				for _, nb := range u.GroundTruths[j] {
					if set1[nb.Docid] {
						overlap++
					}
				}
				avgStepOverlap += float64(overlap) / float64(K)
				overlapCount++
			}
		}
	}
	avgStepOverlap /= float64(overlapCount)
	fmt.Printf("  Avg ground truth overlap between steps: %.1f%% (high = progressive structure works)\n",
		avgStepOverlap*100)

	// === BENCHMARK ===
	fmt.Println()
	fmt.Println("################################################################################")
	fmt.Println("BENCHMARK RESULTS (averaged over", NUM_USERS, "users)")
	fmt.Println("################################################################################")

	type MethodResult struct {
		Name      string
		AvgRecall float64
		UnionRec  float64
		DistCalcs float64
		VecLoads  float64
		TimeMs    float64
	}
	var allMethodResults []MethodResult

	// Method 1: Independent (6 x SearchSingle) — current OnePiece behavior
	{
		var totalRecall, totalUnion, totalDC, totalVL, totalTime float64
		for _, u := range users {
			for run := 0; run < NRUNS; run++ {
				hnsw.Stats.Reset()
				t := time.Now()
				results := make([][]Neighbor, NSTEPS)
				for s, q := range u.Embeddings {
					results[s] = hnsw.SearchSingle(q, K, EF)
				}
				elapsed := time.Since(t)
				if run == 0 {
					recalls := 0.0
					for s := range results {
						recalls += computeRecall(results[s], u.GroundTruths[s])
					}
					totalRecall += recalls / float64(NSTEPS)
					totalUnion += computeUnionRecall(results, u.GroundTruths)
					totalDC += float64(hnsw.Stats.DistCalcs)
					totalVL += float64(hnsw.Stats.VecLoads)
				}
				totalTime += elapsed.Seconds() * 1000
			}
		}
		allMethodResults = append(allMethodResults, MethodResult{
			Name:      "Independent (6x single)",
			AvgRecall: totalRecall / float64(NUM_USERS),
			UnionRec:  totalUnion / float64(NUM_USERS),
			DistCalcs: totalDC / float64(NUM_USERS),
			VecLoads:  totalVL / float64(NUM_USERS),
			TimeMs:    totalTime / float64(NUM_USERS*NRUNS),
		})
	}

	// Method 2: Batch (all 6 steps batched)
	{
		var totalRecall, totalUnion, totalDC, totalVL, totalTime float64
		for _, u := range users {
			for run := 0; run < NRUNS; run++ {
				hnsw.Stats.Reset()
				t := time.Now()
				results := hnsw.SearchBatch(u.Embeddings, K, EF)
				elapsed := time.Since(t)
				if run == 0 {
					recalls := 0.0
					for s := range results {
						recalls += computeRecall(results[s], u.GroundTruths[s])
					}
					totalRecall += recalls / float64(NSTEPS)
					totalUnion += computeUnionRecall(results, u.GroundTruths)
					totalDC += float64(hnsw.Stats.DistCalcs)
					totalVL += float64(hnsw.Stats.VecLoads)
				}
				totalTime += elapsed.Seconds() * 1000
			}
		}
		allMethodResults = append(allMethodResults, MethodResult{
			Name:      "Batch (all 6 steps)",
			AvgRecall: totalRecall / float64(NUM_USERS),
			UnionRec:  totalUnion / float64(NUM_USERS),
			DistCalcs: totalDC / float64(NUM_USERS),
			VecLoads:  totalVL / float64(NUM_USERS),
			TimeMs:    totalTime / float64(NUM_USERS*NRUNS),
		})
	}

	// Method 3: Progressive (draft steps 1-2, verify all 6)
	for _, draftSteps := range []int{1, 2, 3} {
		name := fmt.Sprintf("Progressive (draft=%d steps)", draftSteps)
		var totalRecall, totalUnion, totalDC, totalVL, totalTime float64
		for _, u := range users {
			for run := 0; run < NRUNS; run++ {
				hnsw.Stats.Reset()
				t := time.Now()
				results := hnsw.SearchProgressive(u.Embeddings, K, EF, draftSteps)
				elapsed := time.Since(t)
				if run == 0 {
					recalls := 0.0
					for s := range results {
						recalls += computeRecall(results[s], u.GroundTruths[s])
					}
					totalRecall += recalls / float64(NSTEPS)
					totalUnion += computeUnionRecall(results, u.GroundTruths)
					totalDC += float64(hnsw.Stats.DistCalcs)
					totalVL += float64(hnsw.Stats.VecLoads)
				}
				totalTime += elapsed.Seconds() * 1000
			}
		}
		allMethodResults = append(allMethodResults, MethodResult{
			Name:      name,
			AvgRecall: totalRecall / float64(NUM_USERS),
			UnionRec:  totalUnion / float64(NUM_USERS),
			DistCalcs: totalDC / float64(NUM_USERS),
			VecLoads:  totalVL / float64(NUM_USERS),
			TimeMs:    totalTime / float64(NUM_USERS*NRUNS),
		})
	}

	// Print results
	baseline := allMethodResults[0]
	baselineCost := baseline.VecLoads*cacheMissNs + baseline.DistCalcs*distCalcNs

	fmt.Println()
	fmt.Printf("  %-30s %8s %10s %10s %10s %10s %10s\n",
		"Method", "Recall", "UnionRec", "DistCalcs", "VecLoads", "Sim.Speed", "Proj.Speed")
	fmt.Println("  " + repeatStr("-", 98))

	for _, r := range allMethodResults {
		simSpeedup := baseline.TimeMs / r.TimeMs
		projCost := r.VecLoads*cacheMissNs + r.DistCalcs*distCalcNs
		projSpeedup := baselineCost / projCost
		fmt.Printf("  %-30s %8.4f %10.4f %10.0f %10.0f %9.1fx %9.1fx\n",
			r.Name, r.AvgRecall, r.UnionRec, r.DistCalcs, r.VecLoads, simSpeedup, projSpeedup)
	}

	// Highlight best progressive variant
	fmt.Println()
	fmt.Println("================================================================================")
	fmt.Println("ANALYSIS (OnePiece Progressive Retrieval)")
	fmt.Println("================================================================================")
	fmt.Println()
	fmt.Printf("  Current approach: 6 independent HNSW searches = 8x CPU cost (observed)\n")
	fmt.Printf("  Baseline (simulated): %.0f dist calcs, %.0f vec loads\n", baseline.DistCalcs, baseline.VecLoads)
	fmt.Println()

	// Print per-method analysis
	for i, r := range allMethodResults {
		if i == 0 {
			continue
		}
		projCost := r.VecLoads*cacheMissNs + r.DistCalcs*distCalcNs
		projSpeedup := baselineCost / projCost
		vlReduction := baseline.VecLoads / r.VecLoads
		fmt.Printf("  %s:\n", r.Name)
		fmt.Printf("    VecLoad reduction: %.1fx | Proj. speedup: %.1fx | Recall: %.4f\n",
			vlReduction, projSpeedup, r.AvgRecall)
	}

	fmt.Println()
	fmt.Println("  RECOMMENDATION for OnePiece:")
	fmt.Println("    Progressive (draft=2): Use step1-2 for HNSW search, step3-6 for re-rank")
	fmt.Println("    - Only 2 HNSW traversals (batch) instead of 6")
	fmt.Println("    - Re-rank phase is trivial (brute-force over ~100-200 candidates)")
	fmt.Println("    - Expected: 8x CPU -> ~2x CPU (4x improvement)")
	fmt.Println()
	fmt.Println("  Cost model: vecLoad=40ns (L3 miss), distCalc=2ns (arithmetic)")
	fmt.Println("  'Proj.Speed' = projected real-world speedup using this cost model")
}

func repeatStr(s string, n int) string {
	result := ""
	for i := 0; i < n; i++ {
		result += s
	}
	return result
}
