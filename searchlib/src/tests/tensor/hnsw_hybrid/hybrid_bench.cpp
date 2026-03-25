// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
//
// Hybrid HNSW memory & query benchmark
//
// Measures memory overhead and query QPS across 4 index states:
//   1. Pure in-memory HnswIndex (baseline)
//   2. Hybrid: memory index + 1 flush disk index (no fusion, no compression)
//   3. Hybrid: memory cleared, single fusion disk index (no compression)
//   4. Hybrid: fusion disk index with INT8 compression + reranking
//
// Usage:
//   ./searchlib_hnsw_hybrid_bench_app [num_docs] [dims] [num_queries]
//   Defaults: 20000 docs, 128 dims, 500 queries

#include <vespa/searchlib/tensor/bbq_compressor.h>
#include <vespa/searchlib/tensor/distance_functions.h>
#include <vespa/searchlib/tensor/doc_vector_access.h>
#include <vespa/searchlib/tensor/hnsw_disk_index.h>
#include <vespa/searchlib/tensor/hnsw_hybrid_index.h>
#include <vespa/searchlib/tensor/hnsw_index.h>
#include <vespa/searchlib/tensor/inv_log_level_generator.h>
#include <vespa/searchlib/tensor/scalar_quantizer.h>
#include <vespa/searchlib/tensor/vector_compressor.h>
#include <vespa/searchlib/common/bitvector.h>
#include <vespa/eval/eval/typed_cells.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

using namespace search::tensor;
using namespace vespalib::eval;
using search::BitVector;

// -----------------------------------------------------------------------
struct Timer {
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point _t0;
    Timer() : _t0(Clock::now()) {}
    double ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - _t0).count();
    }
    void reset() { _t0 = Clock::now(); }
};

// -----------------------------------------------------------------------
class FlatDocVectors : public DocVectorAccess {
    const std::vector<float>& _data;
    size_t _dims;
public:
    FlatDocVectors(const std::vector<float>& data, size_t dims)
        : _data(data), _dims(dims) {}
    TypedCells get_vector(uint32_t docid) const override {
        return TypedCells(_data.data() + docid * _dims, CellType::FLOAT, _dims);
    }
};

// -----------------------------------------------------------------------
// Print a section header
static void section(const std::string& title) {
    std::cout << "\n" << std::string(70, '=') << "\n"
              << "  " << title << "\n"
              << std::string(70, '=') << "\n";
}

static void row(const std::string& label, const std::string& value) {
    std::cout << std::left << std::setw(40) << label
              << std::right << value << "\n";
}

// -----------------------------------------------------------------------
// Brute-force top-k for ground truth
std::vector<uint32_t> exact_topk(const float* q, const std::vector<float>& data,
                                  size_t dims, uint32_t k)
{
    size_t n = data.size() / dims;
    std::vector<std::pair<double,uint32_t>> dists(n);
    for (size_t i = 0; i < n; ++i) {
        double d = 0; const float* v = data.data() + i * dims;
        for (size_t j = 0; j < dims; ++j) { double x = q[j]-v[j]; d += x*x; }
        dists[i] = {d, (uint32_t)i};
    }
    std::partial_sort(dists.begin(), dists.begin()+k, dists.end());
    std::vector<uint32_t> res(k);
    for (uint32_t i = 0; i < k; ++i) res[i] = dists[i].second;
    return res;
}

// -----------------------------------------------------------------------
// Measure QPS of a search function over num_queries queries
using SearchFn = std::function<std::vector<NearestNeighborIndex::Neighbor>(TypedCells)>;

struct QpsResult {
    double qps;
    double recall;
};

QpsResult measure_qps(SearchFn search,
                      const std::vector<float>& queries, size_t dims,
                      const std::vector<std::vector<uint32_t>>& gt,
                      uint32_t k, uint32_t nq)
{
    double recall_sum = 0;
    Timer t;
    for (uint32_t qi = 0; qi < nq; ++qi) {
        TypedCells qc(queries.data() + qi * dims, CellType::FLOAT, dims);
        auto res = search(qc);
        const auto& g = gt[qi];
        uint32_t hits = 0;
        for (auto& nb : res) {
            for (uint32_t gid : g) if (nb.docid == gid) { ++hits; break; }
        }
        recall_sum += (double)hits / k;
    }
    double elapsed_s = t.ms() / 1000.0;
    return {(double)nq / elapsed_s, recall_sum / nq};
}

// -----------------------------------------------------------------------
// Memory usage helper
static std::string fmt_mb(size_t bytes) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << (bytes / 1048576.0) << " MB";
    return os.str();
}

static std::string fmt_qps(double qps) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(0) << qps << " q/s";
    return os.str();
}

static std::string fmt_recall(double r) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << r;
    return os.str();
}

// -----------------------------------------------------------------------
int main(int argc, char** argv)
{
    uint32_t num_docs   = (argc > 1) ? std::stoul(argv[1]) : 20'000;
    uint32_t dims       = (argc > 2) ? std::stoul(argv[2]) : 128;
    uint32_t num_queries= (argc > 3) ? std::stoul(argv[3]) : 500;
    uint32_t k          = 10;
    uint32_t explore_k  = 100;
    std::string work_dir = "/tmp/hnsw_hybrid_bench";

    std::filesystem::remove_all(work_dir);
    std::filesystem::create_directories(work_dir);

    // -----------------------------------------------------------------------
    section("Setup");
    std::cout << "docs=" << num_docs << "  dims=" << dims
              << "  queries=" << num_queries << "  k=" << k << "\n\n";

    std::mt19937_64 rng(0xABCD1234);
    std::normal_distribution<float> nd(0, 1);

    std::vector<float> data(static_cast<size_t>(num_docs) * dims);
    for (float& v : data) v = nd(rng);

    std::vector<float> queries(static_cast<size_t>(num_queries) * dims);
    for (float& v : queries) v = nd(rng);

    // Ground truth
    std::cout << "Computing brute-force ground truth...\n";
    std::vector<std::vector<uint32_t>> gt(num_queries);
    { Timer t;
      for (uint32_t qi = 0; qi < num_queries; ++qi)
          gt[qi] = exact_topk(queries.data() + qi * dims, data, dims, k);
      std::cout << "  done in " << std::fixed << std::setprecision(1)
                << t.ms() << " ms\n"; }

    // -----------------------------------------------------------------------
    // State 1: Pure in-memory HnswIndex
    // -----------------------------------------------------------------------
    section("State 1: Pure in-memory HnswIndex");

    FlatDocVectors flat_vecs(data, dims);
    HnswIndex::Config hnsw_cfg(32, 16, 200, 10, true);

    Timer build_t;
    HnswIndex mem_idx(flat_vecs,
                      std::make_unique<SquaredEuclideanDistance>(CellType::FLOAT),
                      std::make_unique<InvLogLevelGenerator>(32),
                      hnsw_cfg);
    for (uint32_t i = 0; i < num_docs; ++i) mem_idx.add_document(i);
    double build_ms = build_t.ms();

    auto mem_usage = mem_idx.memory_usage();
    size_t graph_bytes = mem_usage.usedBytes();
    size_t data_bytes  = data.size() * sizeof(float);  // DenseTensorStore (always)

    row("Build time", std::to_string((int)build_ms) + " ms");
    row("HnswGraph memory (nodes+links)", fmt_mb(graph_bytes));
    row("DenseTensorStore (float32 vecs)", fmt_mb(data_bytes));
    row("Total (graph + store)", fmt_mb(graph_bytes + data_bytes));

    auto s1_search = [&](TypedCells qc) {
        return mem_idx.find_top_k(k, qc, explore_k, 1e18);
    };
    auto [qps1, recall1] = measure_qps(s1_search, queries, dims, gt, k, num_queries);
    row("QPS", fmt_qps(qps1));
    row("Recall@10", fmt_recall(recall1));

    // -----------------------------------------------------------------------
    // State 2: Hybrid — memory index + flush disk index (no compression)
    // -----------------------------------------------------------------------
    section("State 2: Hybrid (memory + 1 flush, NONE compression)");

    // Build a new hybrid index (so we can flush it)
    auto mem_for_hybrid = std::make_unique<HnswIndex>(
        flat_vecs,
        std::make_unique<SquaredEuclideanDistance>(CellType::FLOAT),
        std::make_unique<InvLogLevelGenerator>(32),
        hnsw_cfg);
    for (uint32_t i = 0; i < num_docs; ++i) mem_for_hybrid->add_document(i);

    HnswHybridIndex::Config hybrid_cfg;
    hybrid_cfg.fusion_compression = VectorCompressor::CompressionType::NONE;
    hybrid_cfg.rerank_candidates  = 100;

    HnswHybridIndex hybrid(std::move(mem_for_hybrid), flat_vecs, work_dir, hybrid_cfg);

    Timer flush_t;
    uint32_t fid = hybrid.flush_memory_index(num_docs);
    double flush_ms = flush_t.ms();

    auto snapshot = hybrid.disk_index_snapshot();
    size_t disk_bytes_none = 0;
    if (!snapshot.empty()) disk_bytes_none = snapshot[0]->num_docs() * dims * sizeof(float);

    auto mem_after_flush = hybrid.memory_index().memory_usage();

    row("Flush time", std::to_string((int)flush_ms) + " ms  (id=" + std::to_string(fid) + ")");
    row("Memory index graph (after flush, not cleared)", fmt_mb(mem_after_flush.usedBytes()));
    row("Disk index loaded vectors (NONE = float32)", fmt_mb(disk_bytes_none));
    row("DenseTensorStore (always present)", fmt_mb(data_bytes));
    row("Total approx (graph + disk + store)", fmt_mb(mem_after_flush.usedBytes() + disk_bytes_none + data_bytes));
    row("Note", "In production, memory index is reset after flush; graph RAM freed");

    auto s2_search = [&](TypedCells qc) {
        return hybrid.find_top_k(k, qc, explore_k, 1e18);
    };
    auto [qps2, recall2] = measure_qps(s2_search, queries, dims, gt, k, num_queries);
    row("QPS", fmt_qps(qps2));
    row("Recall@10", fmt_recall(recall2));

    // -----------------------------------------------------------------------
    // State 3: Post-fusion (no compression)
    // -----------------------------------------------------------------------
    section("State 3: Post-fusion (NONE compression)");

    Timer fusion_t;
    hybrid.run_fusion();
    double fusion_ms = fusion_t.ms();

    auto snap3 = hybrid.disk_index_snapshot();
    size_t disk3_bytes = snap3.empty() ? 0 : snap3[0]->num_docs() * dims * sizeof(float);

    row("Fusion time", std::to_string((int)fusion_ms) + " ms");
    row("Fusion disk index (float32)", fmt_mb(disk3_bytes));
    row("DenseTensorStore (always present)", fmt_mb(data_bytes));
    row("Total (fusion disk + store) [no in-mem graph]", fmt_mb(disk3_bytes + data_bytes));
    row("Memory saving vs pure in-mem", fmt_mb(graph_bytes));  // graph freed post-fusion

    auto s3_search = [&](TypedCells qc) {
        return hybrid.find_top_k(k, qc, explore_k, 1e18);
    };
    auto [qps3, recall3] = measure_qps(s3_search, queries, dims, gt, k, num_queries);
    row("QPS (memory index empty, all in disk)", fmt_qps(qps3));
    row("Recall@10", fmt_recall(recall3));

    // -----------------------------------------------------------------------
    // State 4: Post-fusion with INT8 compression + reranking
    // -----------------------------------------------------------------------
    section("State 4: Post-fusion with INT8 compression + reranking");

    // Rebuild hybrid from scratch, set fusion compression to INT8
    auto mem_for_int8 = std::make_unique<HnswIndex>(
        flat_vecs,
        std::make_unique<SquaredEuclideanDistance>(CellType::FLOAT),
        std::make_unique<InvLogLevelGenerator>(32),
        hnsw_cfg);
    for (uint32_t i = 0; i < num_docs; ++i) mem_for_int8->add_document(i);

    HnswHybridIndex::Config cfg_int8;
    cfg_int8.fusion_compression = VectorCompressor::CompressionType::INT8;
    cfg_int8.rerank_candidates  = 50;

    HnswHybridIndex hybrid_int8(std::move(mem_for_int8), flat_vecs, work_dir + "/int8", cfg_int8);
    std::filesystem::create_directories(work_dir + "/int8");

    hybrid_int8.flush_memory_index(num_docs);

    Timer fusion_int8_t;
    hybrid_int8.run_fusion();  // compresses during fusion
    double fusion_int8_ms = fusion_int8_t.ms();

    auto snap4 = hybrid_int8.disk_index_snapshot();
    size_t disk4_bytes = snap4.empty() ? 0 : snap4[0]->num_docs() * dims * sizeof(int8_t);

    row("Fusion time (with INT8 train+compress)", std::to_string((int)fusion_int8_ms) + " ms");
    row("Fusion disk index (INT8, 4x compressed)", fmt_mb(disk4_bytes));
    row("DenseTensorStore (float32, for reranking)", fmt_mb(data_bytes));
    row("Total (disk INT8 + store)", fmt_mb(disk4_bytes + data_bytes));
    row("Memory saved vs NONE fusion", fmt_mb(disk3_bytes - disk4_bytes));

    auto s4_search = [&](TypedCells qc) {
        return hybrid_int8.find_top_k(k, qc, explore_k, 1e18);
    };
    auto [qps4, recall4] = measure_qps(s4_search, queries, dims, gt, k, num_queries);
    row("rerank_candidates", std::to_string(cfg_int8.rerank_candidates));
    row("QPS (INT8 disk + rerank)", fmt_qps(qps4));
    row("Recall@10", fmt_recall(recall4));

    // -----------------------------------------------------------------------
    // State 5: Post-fusion with BBQ-4bit + reranking
    // -----------------------------------------------------------------------
    section("State 5: Post-fusion with BBQ-4bit compression + reranking");

    auto mem_for_bbq = std::make_unique<HnswIndex>(
        flat_vecs,
        std::make_unique<SquaredEuclideanDistance>(CellType::FLOAT),
        std::make_unique<InvLogLevelGenerator>(32),
        hnsw_cfg);
    for (uint32_t i = 0; i < num_docs; ++i) mem_for_bbq->add_document(i);

    HnswHybridIndex::Config cfg_bbq;
    cfg_bbq.fusion_compression = VectorCompressor::CompressionType::BBQ;
    cfg_bbq.rerank_candidates  = 50;

    HnswHybridIndex hybrid_bbq(std::move(mem_for_bbq), flat_vecs, work_dir + "/bbq", cfg_bbq);
    std::filesystem::create_directories(work_dir + "/bbq");

    hybrid_bbq.flush_memory_index(num_docs);

    Timer fusion_bbq_t;
    hybrid_bbq.run_fusion();
    double fusion_bbq_ms = fusion_bbq_t.ms();

    // BBQ-4bit: 4 bits/dim → dims/2 bytes
    size_t disk5_bytes = snap4.empty() ? 0 : (size_t)num_docs * dims / 2;

    row("Fusion time (BBQ-4bit train+compress)", std::to_string((int)fusion_bbq_ms) + " ms");
    row("Fusion disk index (BBQ-4bit, 8x compressed)", fmt_mb(disk5_bytes));
    row("DenseTensorStore (float32, for reranking)", fmt_mb(data_bytes));
    row("Total (disk BBQ + store)", fmt_mb(disk5_bytes + data_bytes));

    auto s5_search = [&](TypedCells qc) {
        return hybrid_bbq.find_top_k(k, qc, explore_k, 1e18);
    };
    auto [qps5, recall5] = measure_qps(s5_search, queries, dims, gt, k, num_queries);
    row("rerank_candidates", std::to_string(cfg_bbq.rerank_candidates));
    row("QPS (BBQ-4bit disk + rerank)", fmt_qps(qps5));
    row("Recall@10", fmt_recall(recall5));

    // -----------------------------------------------------------------------
    // Summary table
    // -----------------------------------------------------------------------
    section("Summary");

    const int W1 = 38, W2 = 14, W3 = 14, W4 = 14;
    auto hdr = [&]() {
        std::cout << std::left << std::setw(W1) << "State"
                  << std::right << std::setw(W2) << "TotalRAM"
                  << std::setw(W3) << "QPS"
                  << std::setw(W4) << "Recall@10" << "\n"
                  << std::string(W1+W2+W3+W4, '-') << "\n";
    };
    auto rowfmt = [&](const std::string& s, size_t ram, double qps, double recall) {
        std::cout << std::left  << std::setw(W1) << s
                  << std::right << std::setw(W2) << fmt_mb(ram)
                  << std::setw(W3) << fmt_qps(qps)
                  << std::setw(W4) << fmt_recall(recall) << "\n";
    };

    hdr();
    rowfmt("1. Pure memory HnswIndex",
           graph_bytes + data_bytes, qps1, recall1);
    rowfmt("2. Memory + flush disk (NONE)",
           mem_after_flush.usedBytes() + disk_bytes_none + data_bytes, qps2, recall2);
    rowfmt("3. Post-fusion (NONE)",
           disk3_bytes + data_bytes, qps3, recall3);
    rowfmt("4. Post-fusion (INT8 + rerank@50)",
           disk4_bytes + data_bytes, qps4, recall4);
    rowfmt("5. Post-fusion (BBQ-4bit + rerank@50)",
           disk5_bytes + data_bytes, qps5, recall5);

    std::cout << "\nNotes:\n"
              << "  DenseTensorStore (float32) is ALWAYS present regardless of index state.\n"
              << "  It is used for summary features and reranking.\n"
              << "  State 2 shows overlap (memory graph + disk): in production, memory\n"
              << "  index is reset/freed after flush; disk index replaces it.\n"
              << "  Reranking fetches exact float32 from store after compressed disk search.\n";

    std::filesystem::remove_all(work_dir);
    return 0;
}
