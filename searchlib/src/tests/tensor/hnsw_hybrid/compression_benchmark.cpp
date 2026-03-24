// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
//
// Benchmark: compression ratio, reconstruct error, recall@10 and throughput
// for all VectorCompressor implementations.
//
// Usage:
//   ./searchlib_hnsw_compression_benchmark_app [num_docs] [dims] [num_queries]
//
// Defaults: 50 000 docs, 128 dims, 1 000 queries.
//
// Output (example):
//   Compressor   Ratio   ReconErr   Recall@10  TrainMs  CompressMs  SearchMs
//   NONE          1.00   0.000000   1.000       0.0       123.4      456.7
//   INT8          4.00   0.003210   0.962       3.2        24.1      201.3
//   BFLOAT16      2.00   0.000031   1.000       0.0        18.7      311.2
//   RABITQ       32.00   0.341200   0.781       45.1        8.3      102.1
//   BBQ(4bit)     8.00   0.021300   0.931      12.3        15.6      153.4

#include <vespa/searchlib/tensor/bbq_compressor.h>
#include <vespa/searchlib/tensor/hnsw_disk_index.h>
#include <vespa/searchlib/tensor/hnsw_index.h>
#include <vespa/searchlib/tensor/rabitq_compressor.h>
#include <vespa/searchlib/tensor/scalar_quantizer.h>
#include <vespa/searchlib/tensor/vector_compressor.h>
#include <vespa/searchlib/tensor/distance_functions.h>
#include <vespa/searchlib/tensor/doc_vector_access.h>
#include <vespa/searchlib/tensor/inv_log_level_generator.h>
#include <vespa/searchlib/common/bitvector.h>
#include <vespa/vespalib/io/fileutil.h>
#include <vespa/eval/eval/typed_cells.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace search::tensor;
using namespace vespalib::eval;
using search::BitVector;

// ---------------------------------------------------------------------------
// Timing helper
// ---------------------------------------------------------------------------
struct Timer {
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point _start;
    Timer() : _start(Clock::now()) {}
    double ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - _start).count();
    }
};

// ---------------------------------------------------------------------------
// DocVectorAccess backed by flat float array
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Build HnswIndex and extract graph structure
// ---------------------------------------------------------------------------
struct GraphInfo {
    std::vector<HnswDiskIndex::GraphNode> nodes;
    std::vector<uint32_t>                 links;
    uint32_t entry_local{0};
    int32_t  entry_level{-1};
};

GraphInfo extract_graph(const HnswGraph& graph, uint32_t n)
{
    GraphInfo gi;
    gi.nodes.resize(n);
    auto entry = graph.get_entry_node();
    gi.entry_local = entry.docid;
    gi.entry_level = entry.level;

    for (uint32_t i = 0; i < n; ++i) {
        gi.nodes[i].links_offset = static_cast<uint32_t>(gi.links.size());
        auto nr = graph.get_node_ref(i);
        if (!nr.valid()) { gi.nodes[i].num_levels = 0; continue; }
        auto levels = graph.nodes.get(nr);
        gi.nodes[i].num_levels = static_cast<uint32_t>(levels.size());
        for (const auto& lr : levels) {
            auto lref = lr.load_acquire();
            if (lref.valid()) {
                auto arr = graph.links.get(lref);
                gi.links.push_back(static_cast<uint32_t>(arr.size()));
                for (uint32_t nb : arr) gi.links.push_back(nb);
            } else {
                gi.links.push_back(0);
            }
        }
    }
    return gi;
}

// ---------------------------------------------------------------------------
// Compute exact top-K for a single query using brute force
// ---------------------------------------------------------------------------
std::vector<uint32_t> exact_topk(const float* query,
                                  const std::vector<float>& data,
                                  size_t dims, uint32_t k)
{
    size_t n = data.size() / dims;
    std::vector<std::pair<double, uint32_t>> dist(n);
    for (size_t i = 0; i < n; ++i) {
        double d = 0.0;
        const float* v = data.data() + i * dims;
        for (size_t j = 0; j < dims; ++j) {
            double diff = query[j] - v[j];
            d += diff * diff;
        }
        dist[i] = {d, static_cast<uint32_t>(i)};
    }
    std::partial_sort(dist.begin(), dist.begin() + k, dist.end());
    std::vector<uint32_t> res(k);
    for (uint32_t i = 0; i < k; ++i) res[i] = dist[i].second;
    return res;
}

// ---------------------------------------------------------------------------
// Per-compressor benchmark
// ---------------------------------------------------------------------------
struct Result {
    std::string name;
    double      ratio;          // float32_bytes / compressed_bytes
    double      reconstruct_err;// mean per-dim absolute error
    double      recall10;       // recall@10 averaged over queries
    double      train_ms;
    double      compress_ms;
    double      search_ms;      // total wall time for all queries
    double      search_ms_per_q;
};

Result run_benchmark(const std::string& name,
                     VectorCompressor* compressor,   // nullptr → NONE
                     const std::vector<float>& data,
                     const GraphInfo& gi,
                     const std::vector<uint32_t>& global_docids,
                     const BitVector& alive,
                     size_t dims,
                     const std::vector<float>& queries,
                     const std::vector<std::vector<uint32_t>>& ground_truth,
                     const std::string& work_dir,
                     uint32_t id,
                     uint32_t k)
{
    uint32_t n = static_cast<uint32_t>(data.size() / dims);
    uint32_t nq = static_cast<uint32_t>(queries.size() / dims);

    // --- Train ---
    double train_ms = 0.0;
    if (compressor) {
        std::vector<const float*> ptrs(n);
        for (uint32_t i = 0; i < n; ++i) ptrs[i] = data.data() + i * dims;
        Timer t;
        compressor->train(ptrs, dims);
        train_ms = t.ms();
    }

    // --- Compress ---
    Timer ct;
    bool ok = HnswDiskIndex::write(work_dir, id,
                                    global_docids, alive,
                                    data, dims,
                                    gi.entry_local, gi.entry_level,
                                    gi.nodes, gi.links,
                                    compressor);
    double compress_ms = ct.ms();
    assert(ok);

    // --- Compute ratio ---
    size_t float32_bytes = static_cast<size_t>(n) * dims * sizeof(float);
    size_t comp_bytes = compressor
        ? static_cast<size_t>(n) * compressor->compressed_bytes(dims)
        : float32_bytes;
    double ratio = static_cast<double>(float32_bytes) / static_cast<double>(comp_bytes);

    // --- Reconstruct error (on first 500 docs) ---
    double rec_err = 0.0;
    if (compressor) {
        uint32_t sample = std::min<uint32_t>(500, n);
        size_t cbytes = compressor->compressed_bytes(dims);
        std::vector<uint8_t> buf(cbytes);
        std::vector<float>   recovered(dims);
        for (uint32_t i = 0; i < sample; ++i) {
            compressor->compress(data.data() + i * dims, buf.data(), dims);
            compressor->decompress(buf.data(), recovered.data(), dims);
            for (size_t j = 0; j < dims; ++j) {
                rec_err += std::fabs(recovered[j] - data[i * dims + j]);
            }
        }
        rec_err /= static_cast<double>(sample) * dims;
    }

    // --- Load index and search ---
    HnswDiskIndex di(work_dir, id);
    assert(di.open());

    double search_ms = 0.0;
    double recall_sum = 0.0;
    {
        Timer st;
        for (uint32_t qi = 0; qi < nq; ++qi) {
            TypedCells qcells(queries.data() + qi * dims, CellType::FLOAT, dims);
            auto results = di.find_top_k(k, qcells, k * 4, 1e18);

            // Recall@k
            const auto& gt = ground_truth[qi];
            uint32_t hits = 0;
            for (auto& nb : results) {
                for (uint32_t gid : gt) {
                    if (nb.docid == gid) { ++hits; break; }
                }
            }
            recall_sum += static_cast<double>(hits) / static_cast<double>(k);
        }
        search_ms = st.ms();
    }

    return Result{
        name,
        ratio,
        rec_err,
        recall_sum / nq,
        train_ms,
        compress_ms,
        search_ms,
        search_ms / nq
    };
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    uint32_t num_docs   = (argc > 1) ? std::stoul(argv[1]) : 50'000;
    uint32_t dims       = (argc > 2) ? std::stoul(argv[2]) : 128;
    uint32_t num_queries= (argc > 3) ? std::stoul(argv[3]) : 1'000;
    uint32_t k          = 10;
    std::string work_dir = "/tmp/hnsw_benchmark";

    std::cerr << "Generating " << num_docs << " docs × " << dims << " dims, "
              << num_queries << " queries...\n";

    // --- Generate data ---
    std::mt19937_64 rng(42);
    std::normal_distribution<float> nd(0.0f, 1.0f);

    std::vector<float> data(static_cast<size_t>(num_docs) * dims);
    for (float& v : data) v = nd(rng);

    std::vector<float> queries(static_cast<size_t>(num_queries) * dims);
    for (float& v : queries) v = nd(rng);

    // --- Build HnswIndex for the graph structure ---
    std::cerr << "Building in-memory HNSW graph...\n";
    FlatDocVectors vecs(data, dims);
    HnswIndex::Config cfg(32, 16, 200, 10, true);
    auto dist_func = std::make_unique<SquaredEuclideanDistance>(CellType::FLOAT);
    auto level_gen = std::make_unique<InvLogLevelGenerator>(32);
    HnswIndex hnsw(vecs, std::move(dist_func), std::move(level_gen), cfg);
    {
        Timer t;
        for (uint32_t i = 0; i < num_docs; ++i) hnsw.add_document(i);
        std::cerr << "  HNSW build: " << std::fixed << std::setprecision(1)
                  << t.ms() << " ms\n";
    }

    GraphInfo gi = extract_graph(hnsw.get_graph(), num_docs);

    // docid map (identity) and alive bitvector
    std::vector<uint32_t> docids(num_docs);
    std::iota(docids.begin(), docids.end(), 0);
    auto alive = BitVector::create(num_docs);
    for (uint32_t i = 0; i < num_docs; ++i) alive->setBit(i);

    // --- Ground truth (brute-force top-k) ---
    std::cerr << "Computing ground truth (brute-force)...\n";
    std::vector<std::vector<uint32_t>> gt(num_queries);
    {
        Timer t;
        for (uint32_t qi = 0; qi < num_queries; ++qi) {
            gt[qi] = exact_topk(queries.data() + qi * dims, data, dims, k);
        }
        std::cerr << "  Ground truth: " << t.ms() << " ms\n";
    }

    // --- Work dir ---
    std::filesystem::remove_all(work_dir);
    std::filesystem::create_directories(work_dir);

    // --- Run benchmarks ---
    std::vector<Result> results;

    auto run = [&](const std::string& label, VectorCompressor* c, uint32_t id) {
        std::cerr << "Benchmarking " << label << "...\n";
        results.push_back(run_benchmark(label, c, data, gi, docids, *alive,
                                         dims, queries, gt, work_dir, id, k));
    };

    run("NONE",        nullptr,                                  1);
    {
        ScalarQuantizer<CellType::INT8> q;
        run("INT8",    &q,                                       2);
    }
    {
        ScalarQuantizer<CellType::BFLOAT16> q;
        run("BFLOAT16",&q,                                       3);
    }
    {
        // RaBitQ is O(d²) at train+compress time; skip for very large dims
        if (dims <= 256) {
            RaBitQCompressor q;
            run("RABITQ", &q,                                    4);
        } else {
            std::cerr << "  Skipping RABITQ (dims=" << dims << " > 256)\n";
        }
    }
    {
        BBQCompressor q4(4);
        run("BBQ-4bit", &q4,                                     5);
    }
    {
        BBQCompressor q2(2);
        run("BBQ-2bit", &q2,                                     6);
    }
    {
        BBQCompressor q1(1);
        run("BBQ-1bit", &q1,                                     7);
    }

    // --- Print table ---
    std::filesystem::remove_all(work_dir);

    const int W = 12;
    std::cout << "\n";
    std::cout << "Benchmark: " << num_docs << " docs × " << dims << " dims, "
              << num_queries << " queries, recall@" << k << "\n\n";
    std::cout << std::left
              << std::setw(12) << "Compressor"
              << std::right
              << std::setw(W)  << "Ratio"
              << std::setw(W)  << "ReconErr"
              << std::setw(W)  << "Recall@10"
              << std::setw(W)  << "Train(ms)"
              << std::setw(W)  << "Comp(ms)"
              << std::setw(W)  << "Search(ms)"
              << std::setw(W)  << "ms/query"
              << "\n";
    std::cout << std::string(12 + W * 7, '-') << "\n";

    for (const auto& r : results) {
        std::cout << std::left << std::setw(12) << r.name
                  << std::right << std::fixed
                  << std::setw(W) << std::setprecision(2) << r.ratio
                  << std::setw(W) << std::setprecision(6) << r.reconstruct_err
                  << std::setw(W) << std::setprecision(3)  << r.recall10
                  << std::setw(W) << std::setprecision(1)  << r.train_ms
                  << std::setw(W) << std::setprecision(1)  << r.compress_ms
                  << std::setw(W) << std::setprecision(1)  << r.search_ms
                  << std::setw(W) << std::setprecision(3)  << r.search_ms_per_q
                  << "\n";
    }

    std::cout << "\nNotes:\n"
              << "  Ratio      = float32_bytes / compressed_bytes\n"
              << "  ReconErr   = mean |reconstructed - original| per dimension\n"
              << "  Recall@10  = fraction of true top-10 found by compressed search\n"
              << "  Comp(ms)   = time to compress all docs + write to disk\n"
              << "  Search(ms) = total search time for all queries\n";

    return 0;
}
