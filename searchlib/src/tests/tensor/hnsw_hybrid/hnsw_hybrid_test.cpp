// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/searchlib/common/bitvector.h>
#include <vespa/searchlib/tensor/distance_functions.h>
#include <vespa/searchlib/tensor/doc_vector_access.h>
#include <vespa/searchlib/tensor/hnsw_index.h>
#include <vespa/searchlib/tensor/hnsw_hybrid_index.h>
#include <vespa/searchlib/tensor/hnsw_disk_index.h>
#include <vespa/searchlib/tensor/scalar_quantizer.h>
#include <vespa/searchlib/tensor/vector_compressor.h>
#include <vespa/searchlib/tensor/random_level_generator.h>
#include <vespa/searchlib/tensor/inv_log_level_generator.h>
#include <vespa/vespalib/gtest/gtest.h>
#include <vespa/vespalib/io/fileutil.h>
#include <vespa/vespalib/util/generationhandler.h>
#include <cmath>
#include <filesystem>
#include <vector>

#include <vespa/log/log.h>
LOG_SETUP("hnsw_hybrid_test");

using namespace search::tensor;
using namespace vespalib::eval;
using search::BitVector;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

class MyDocVectorAccess : public DocVectorAccess {
    std::vector<std::vector<float>> _vecs;
public:
    void set(uint32_t docid, std::vector<float> v) {
        if (docid >= _vecs.size()) _vecs.resize(docid + 1);
        _vecs[docid] = std::move(v);
    }
    vespalib::eval::TypedCells get_vector(uint32_t docid) const override {
        vespalib::ConstArrayRef<float> ref(_vecs[docid]);
        return TypedCells(ref);
    }
};

struct FixedLevelGenerator : public RandomLevelGenerator {
    uint32_t level;
    explicit FixedLevelGenerator(uint32_t l = 0) : level(l) {}
    uint32_t max_level() override { return level; }
};

static std::string test_dir() {
    return "/tmp/hnsw_hybrid_test";
}

class HnswHybridTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::remove_all(test_dir());
        std::filesystem::create_directories(test_dir());
    }
    void TearDown() override {
        std::filesystem::remove_all(test_dir());
    }

    std::unique_ptr<HnswIndex> make_hnsw(MyDocVectorAccess& vecs) {
        HnswIndex::Config cfg(32, 16, 200, 10, true);
        auto dist = std::make_unique<SquaredEuclideanDistance>(CellType::FLOAT);
        auto gen  = std::make_unique<FixedLevelGenerator>(0);
        return std::make_unique<HnswIndex>(vecs, std::move(dist), std::move(gen), cfg);
    }
};

// ---------------------------------------------------------------------------
// Test 1: ScalarQuantizer INT8 round-trip
// ---------------------------------------------------------------------------
TEST_F(HnswHybridTest, scalar_quantizer_int8_roundtrip)
{
    ScalarQuantizer<CellType::INT8> q;
    const size_t dims = 4;

    std::vector<float> v1 = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> v2 = {-1.0f, 0.0f, 5.0f, 10.0f};
    std::vector<const float*> ptrs = {v1.data(), v2.data()};

    q.train(ptrs, dims);
    EXPECT_GT(q.scale(), 0.0f);

    // compress v1
    std::vector<int8_t> compressed(dims);
    q.compress(v1.data(), compressed.data(), dims);

    // decompress
    std::vector<float> recovered(dims);
    q.decompress(compressed.data(), recovered.data(), dims);

    for (size_t i = 0; i < dims; ++i) {
        EXPECT_NEAR(v1[i], recovered[i], 0.15f)
            << "dim " << i << ": orig=" << v1[i] << " recovered=" << recovered[i];
    }
}

// ---------------------------------------------------------------------------
// Test 2: ScalarQuantizer BFLOAT16 round-trip
// ---------------------------------------------------------------------------
TEST_F(HnswHybridTest, scalar_quantizer_bfloat16_roundtrip)
{
    ScalarQuantizer<CellType::BFLOAT16> q;
    const size_t dims = 4;
    std::vector<float> v = {1.5f, -2.25f, 3.125f, 0.0f};

    std::vector<uint8_t> compressed(q.compressed_bytes(dims));
    q.compress(v.data(), compressed.data(), dims);

    std::vector<float> recovered(dims);
    q.decompress(compressed.data(), recovered.data(), dims);

    for (size_t i = 0; i < dims; ++i) {
        // BFloat16 truncates mantissa: error bounded by 2^-7 * |value|
        EXPECT_NEAR(v[i], recovered[i], std::abs(v[i]) * 0.01f + 1e-4f)
            << "dim " << i;
    }
}

// ---------------------------------------------------------------------------
// Test 3: VectorCompressor factory
// ---------------------------------------------------------------------------
TEST_F(HnswHybridTest, vector_compressor_factory)
{
    using T = VectorCompressor::CompressionType;
    auto c_none = VectorCompressor::create(T::NONE);
    EXPECT_EQ(c_none, nullptr);  // NONE returns nullptr

    auto c_int8 = VectorCompressor::create(T::INT8);
    ASSERT_NE(c_int8, nullptr);
    EXPECT_EQ(c_int8->cell_type(), CellType::INT8);

    auto c_bf16 = VectorCompressor::create(T::BFLOAT16);
    ASSERT_NE(c_bf16, nullptr);
    EXPECT_EQ(c_bf16->cell_type(), CellType::BFLOAT16);
}

// ---------------------------------------------------------------------------
// Test 4: HnswDiskIndex write + open + search
// ---------------------------------------------------------------------------
TEST_F(HnswHybridTest, disk_index_write_and_search)
{
    const uint32_t dims = 2;
    const uint32_t n = 4;

    std::vector<uint32_t> docids = {10, 20, 30, 40};
    auto alive = BitVector::create(n);
    for (uint32_t i = 0; i < n; ++i) alive->setBit(i);

    // Simple 2D vectors
    std::vector<float> vectors = {
        1.0f, 0.0f,   // doc 10
        0.0f, 1.0f,   // doc 20
        2.0f, 0.0f,   // doc 30
        0.0f, 2.0f,   // doc 40
    };

    // Build a trivial star graph (doc 0 links to all others at level 0)
    std::vector<HnswDiskIndex::GraphNode> graph_nodes(n);
    std::vector<uint32_t> links_data;
    for (uint32_t i = 0; i < n; ++i) {
        graph_nodes[i].links_offset = static_cast<uint32_t>(links_data.size());
        graph_nodes[i].num_levels = 1;
        // Each node links to its neighbors
        std::vector<uint32_t> neighbors;
        for (uint32_t j = 0; j < n; ++j) {
            if (j != i) neighbors.push_back(j);
        }
        links_data.push_back(static_cast<uint32_t>(neighbors.size()));
        for (uint32_t nb : neighbors) links_data.push_back(nb);
    }

    bool ok = HnswDiskIndex::write(test_dir(), 1,
                                    docids, *alive,
                                    vectors, dims,
                                    0, 0,  // entry_local=0, entry_level=0
                                    graph_nodes, links_data,
                                    nullptr);  // no compression
    ASSERT_TRUE(ok);

    HnswDiskIndex di(test_dir(), 1);
    ASSERT_TRUE(di.open());
    EXPECT_EQ(di.num_docs(), n);
    EXPECT_EQ(di.dims(), dims);
    EXPECT_EQ(di.alive_count(), n);
    EXPECT_NEAR(di.alive_ratio(), 1.0, 1e-6);

    // Search for nearest to (1.1, 0.1) -> should be doc 10 (1,0)
    std::vector<float> query = {1.1f, 0.1f};
    TypedCells query_cells(query.data(), CellType::FLOAT, dims);

    auto results = di.find_top_k(2, query_cells, 10, 1e9);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].docid, 10u);

    // Delete doc 10
    di.remove_document(10);
    EXPECT_EQ(di.alive_count(), n - 1);
    EXPECT_NEAR(di.alive_ratio(), (n-1.0)/n, 1e-6);

    // Search again: doc 10 should not appear
    results = di.find_top_k(3, query_cells, 10, 1e9);
    for (auto& nb : results) {
        EXPECT_NE(nb.docid, 10u);
    }
}

// ---------------------------------------------------------------------------
// Test 5: HnswDiskIndex with INT8 compression
// ---------------------------------------------------------------------------
TEST_F(HnswHybridTest, disk_index_with_int8_compression)
{
    const uint32_t dims = 4;
    const uint32_t n = 5;

    std::vector<uint32_t> docids;
    for (uint32_t i = 0; i < n; ++i) docids.push_back(i * 10);

    auto alive = BitVector::create(n);
    for (uint32_t i = 0; i < n; ++i) alive->setBit(i);

    std::vector<float> vectors;
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t d = 0; d < dims; ++d) {
            vectors.push_back(static_cast<float>(i * dims + d));
        }
    }

    // Build chain graph
    std::vector<HnswDiskIndex::GraphNode> graph_nodes(n);
    std::vector<uint32_t> links_data;
    for (uint32_t i = 0; i < n; ++i) {
        graph_nodes[i].links_offset = static_cast<uint32_t>(links_data.size());
        graph_nodes[i].num_levels = 1;
        std::vector<uint32_t> nb;
        if (i > 0) nb.push_back(i - 1);
        if (i + 1 < n) nb.push_back(i + 1);
        links_data.push_back(static_cast<uint32_t>(nb.size()));
        for (uint32_t j : nb) links_data.push_back(j);
    }

    // Train and write with INT8 compression
    ScalarQuantizer<CellType::INT8> q;
    std::vector<const float*> ptrs;
    for (uint32_t i = 0; i < n; ++i) ptrs.push_back(vectors.data() + i * dims);
    q.train(ptrs, dims);

    bool ok = HnswDiskIndex::write(test_dir(), 2,
                                    docids, *alive,
                                    vectors, dims,
                                    0, 0,
                                    graph_nodes, links_data,
                                    &q);
    ASSERT_TRUE(ok);

    HnswDiskIndex di(test_dir(), 2);
    ASSERT_TRUE(di.open());
    EXPECT_EQ(di.num_docs(), n);

    // The compressor should be loaded and decompression should work
    auto vec0 = di.get_float_vector(0);
    ASSERT_EQ(vec0.size(), dims);
    // Values should approximately match original (within INT8 quantization error)
    for (uint32_t d = 0; d < dims; ++d) {
        EXPECT_NEAR(vec0[d], vectors[d], 1.5f)
            << "dim " << d << ": orig=" << vectors[d] << " got=" << vec0[d];
    }
}

// ---------------------------------------------------------------------------
// Test 6: HnswHybridIndex basic add + search
// ---------------------------------------------------------------------------
TEST_F(HnswHybridTest, hybrid_index_basic_search)
{
    MyDocVectorAccess vecs;
    vecs.set(1, {2.0f, 2.0f});
    vecs.set(2, {3.0f, 2.0f});
    vecs.set(3, {2.0f, 3.0f});
    vecs.set(4, {1.0f, 2.0f});

    auto hnsw = make_hnsw(vecs);
    HnswHybridIndex::Config cfg;
    HnswHybridIndex hybrid(std::move(hnsw), test_dir(), cfg);

    // Insert documents into memory index
    for (uint32_t id : {1u, 2u, 3u, 4u}) {
        hybrid.add_document(id);
    }

    // Search
    std::vector<float> q = {2.1f, 2.1f};
    TypedCells qcells(q.data(), CellType::FLOAT, 2);

    auto results = hybrid.find_top_k(2, qcells, 20, 1e9);
    ASSERT_GE(results.size(), 1u);
    // Nearest to (2.1, 2.1) should be docid 1 (2,2) or 3 (2,3)
    EXPECT_TRUE(results[0].docid == 1u || results[0].docid == 3u || results[0].docid == 2u);

    // Remove doc 1
    hybrid.remove_document(1);

    // After removal, doc 1 should not appear
    results = hybrid.find_top_k(3, qcells, 20, 1e9);
    for (auto& nb : results) {
        EXPECT_NE(nb.docid, 1u);
    }
}

// ---------------------------------------------------------------------------
// Test 7: compression_type_from_string / to_string round-trip
// ---------------------------------------------------------------------------
TEST_F(HnswHybridTest, compression_type_string_roundtrip)
{
    using T = VectorCompressor::CompressionType;
    for (auto t : {T::NONE, T::INT8, T::BFLOAT16, T::RABITQ, T::BBQ}) {
        auto s = to_string(t);
        EXPECT_EQ(compression_type_from_string(s), t) << "failed for " << s;
    }
}

GTEST_MAIN_RUN_ALL_TESTS()
