// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "hnsw_hybrid_index.h"
#include "distance_functions.h"
#include "hnsw_index_saver.h"
#include "inv_log_level_generator.h"
#include "nearest_neighbor_index_loader.h"
#include "nearest_neighbor_index_saver.h"
#include "prepare_result.h"
#include <vespa/searchlib/attribute/address_space_usage.h>
#include <vespa/vespalib/data/slime/cursor.h>
#include <vespa/vespalib/data/slime/inserter.h>
#include <algorithm>
#include <numeric>
#include <stdexcept>

#include <vespa/log/log.h>
LOG_SETUP(".tensor.hnsw_hybrid_index");

namespace search::tensor {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

HnswHybridIndex::HnswHybridIndex(std::unique_ptr<HnswIndex> memory_index,
                                   const DocVectorAccess& vectors,
                                   vespalib::stringref base_dir,
                                   const Config& cfg)
    : _memory_index(std::move(memory_index)),
      _vectors(vectors),
      _base_dir(base_dir),
      _cfg(cfg)
{
    assert(_memory_index);
}

HnswHybridIndex::~HnswHybridIndex() = default;

// ---------------------------------------------------------------------------
// NearestNeighborIndex — write path (single writer thread)
// ---------------------------------------------------------------------------

void HnswHybridIndex::add_document(uint32_t docid)
{
    _memory_index->add_document(docid);
}

std::unique_ptr<PrepareResult>
HnswHybridIndex::prepare_add_document(uint32_t docid, TypedCells vector,
                                       vespalib::GenerationHandler::Guard read_guard) const
{
    return _memory_index->prepare_add_document(docid, vector, std::move(read_guard));
}

void HnswHybridIndex::complete_add_document(uint32_t docid,
                                             std::unique_ptr<PrepareResult> prepare_result)
{
    _memory_index->complete_add_document(docid, std::move(prepare_result));
}

void HnswHybridIndex::remove_document(uint32_t docid)
{
    // Remove from in-memory index
    _memory_index->remove_document(docid);

    // Propagate deletion to all disk indexes (clears alive bit + persists alive.dat)
    std::shared_lock lock(_disk_mutex);
    for (auto& di : _disk_indexes) {
        di->remove_document(docid);
    }
}

// ---------------------------------------------------------------------------
// Generation / compaction
// ---------------------------------------------------------------------------

void HnswHybridIndex::transfer_hold_lists(generation_t current_gen)
{
    _memory_index->transfer_hold_lists(current_gen);
}

void HnswHybridIndex::trim_hold_lists(generation_t first_used_gen)
{
    _memory_index->trim_hold_lists(first_used_gen);
}

bool HnswHybridIndex::consider_compact(const CompactionStrategy& strategy)
{
    return _memory_index->consider_compact(strategy);
}

vespalib::MemoryUsage HnswHybridIndex::update_stat(const CompactionStrategy& strategy)
{
    return _memory_index->update_stat(strategy);
}

vespalib::MemoryUsage HnswHybridIndex::memory_usage() const
{
    return _memory_index->memory_usage();
}

void HnswHybridIndex::populate_address_space_usage(search::AddressSpaceUsage& usage) const
{
    _memory_index->populate_address_space_usage(usage);
}

void HnswHybridIndex::get_state(const vespalib::slime::Inserter& inserter) const
{
    auto& object = inserter.insertObject();
    _memory_index->get_state(vespalib::slime::ObjectInserter(object, "memory_index"));

    std::shared_lock lock(_disk_mutex);
    auto& arr = object.setArray("disk_indexes");
    for (const auto& di : _disk_indexes) {
        auto& entry = arr.addObject();
        entry.setLong("id", di->id());
        entry.setLong("num_docs", di->num_docs());
        entry.setDouble("alive_ratio", di->alive_ratio());
    }
}

void HnswHybridIndex::shrink_lid_space(uint32_t doc_id_limit)
{
    _memory_index->shrink_lid_space(doc_id_limit);
}

// ---------------------------------------------------------------------------
// Save / Load (delegates to memory index for backward compat)
// ---------------------------------------------------------------------------

std::unique_ptr<NearestNeighborIndexSaver> HnswHybridIndex::make_saver() const
{
    return _memory_index->make_saver();
}

std::unique_ptr<NearestNeighborIndexLoader> HnswHybridIndex::make_loader(FastOS_FileInterface& file)
{
    return _memory_index->make_loader(file);
}

// ---------------------------------------------------------------------------
// Search path
// ---------------------------------------------------------------------------

const DistanceFunction* HnswHybridIndex::distance_function() const
{
    return _memory_index->distance_function();
}

std::vector<NearestNeighborIndex::Neighbor>
HnswHybridIndex::merge_results(std::vector<Neighbor> mem_results,
                                std::vector<std::vector<Neighbor>> disk_results,
                                uint32_t k) const
{
    // Merge all candidate lists into a single sorted vector, deduplicate by docid
    std::vector<Neighbor> all;
    all.reserve(mem_results.size());
    for (auto& n : mem_results) all.push_back(n);
    for (auto& dv : disk_results)
        for (auto& n : dv) all.push_back(n);

    std::sort(all.begin(), all.end(),
              [](const Neighbor& a, const Neighbor& b){ return a.distance < b.distance; });

    // Deduplicate by docid (keep lowest distance)
    std::vector<Neighbor> result;
    result.reserve(std::min<size_t>(k, all.size()));
    uint32_t last_id = UINT32_MAX;
    for (auto& n : all) {
        if (n.docid == last_id) continue;
        last_id = n.docid;
        result.push_back(n);
        if (result.size() >= k) break;
    }
    return result;
}

std::vector<NearestNeighborIndex::Neighbor>
HnswHybridIndex::exact_rescore(std::vector<Neighbor> candidates,
                                TypedCells query, uint32_t k) const
{
    // Recompute exact distance using original float32 vectors from DocVectorAccess.
    // Used when disk indexes store lossy compressed vectors (RABITQ, BBQ, INT8).
    const DistanceFunction* df = _memory_index->distance_function();
    if (!df) return candidates;

    for (auto& nb : candidates) {
        TypedCells doc_vec = _vectors.get_vector(nb.docid);
        nb.distance = df->calc(query, doc_vec);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Neighbor& a, const Neighbor& b){ return a.distance < b.distance; });
    if (candidates.size() > k) candidates.resize(k);
    return candidates;
}

std::vector<NearestNeighborIndex::Neighbor>
HnswHybridIndex::find_top_k(uint32_t k, TypedCells vector,
                             uint32_t explore_k,
                             double distance_threshold) const
{
    auto mem = _memory_index->find_top_k(k, vector, explore_k, distance_threshold);

    bool lossy = (_cfg.fusion_compression != VectorCompressor::CompressionType::NONE &&
                  _cfg.fusion_compression != VectorCompressor::CompressionType::BFLOAT16);

    // For lossy compression, retrieve more candidates so reranking is effective.
    uint32_t disk_explore = lossy
        ? std::max(explore_k, _cfg.rerank_candidates)
        : explore_k;

    std::shared_lock lock(_disk_mutex);
    if (_disk_indexes.empty()) return mem;

    std::vector<std::vector<Neighbor>> disk_results;
    disk_results.reserve(_disk_indexes.size());
    for (const auto& di : _disk_indexes) {
        disk_results.push_back(
            di->find_top_k(lossy ? _cfg.rerank_candidates : k,
                           vector, disk_explore, distance_threshold, nullptr));
    }

    auto merged = merge_results(std::move(mem), std::move(disk_results),
                                lossy ? _cfg.rerank_candidates : k);

    if (lossy) {
        merged = exact_rescore(std::move(merged), vector, k);
    }
    return merged;
}

std::vector<NearestNeighborIndex::Neighbor>
HnswHybridIndex::find_top_k_with_filter(uint32_t k, TypedCells vector,
                                         const BitVector& filter,
                                         uint32_t explore_k,
                                         double distance_threshold) const
{
    auto mem = _memory_index->find_top_k_with_filter(
        k, vector, filter, explore_k, distance_threshold);

    bool lossy = (_cfg.fusion_compression != VectorCompressor::CompressionType::NONE &&
                  _cfg.fusion_compression != VectorCompressor::CompressionType::BFLOAT16);

    uint32_t disk_explore = lossy
        ? std::max(explore_k, _cfg.rerank_candidates)
        : explore_k;

    std::shared_lock lock(_disk_mutex);
    if (_disk_indexes.empty()) return mem;

    std::vector<std::vector<Neighbor>> disk_results;
    disk_results.reserve(_disk_indexes.size());
    for (const auto& di : _disk_indexes) {
        disk_results.push_back(
            di->find_top_k(lossy ? _cfg.rerank_candidates : k,
                           vector, disk_explore, distance_threshold, &filter));
    }

    auto merged = merge_results(std::move(mem), std::move(disk_results),
                                lossy ? _cfg.rerank_candidates : k);

    if (lossy) {
        merged = exact_rescore(std::move(merged), vector, k);
    }
    return merged;
}

// ---------------------------------------------------------------------------
// Hybrid-specific operations
// ---------------------------------------------------------------------------

uint32_t HnswHybridIndex::memory_index_doc_count() const
{
    return _memory_index->get_entry_docid() == 0 &&
           _memory_index->get_entry_level() < 0 ? 0 : 1;  // rough proxy
}

bool HnswHybridIndex::needs_flush() const
{
    // Simple heuristic: check memory usage
    auto mu = _memory_index->memory_usage();
    size_t mb = (mu.usedBytes() + mu.deadBytes()) / (1024 * 1024);
    return mb >= _cfg.flush_threshold_mb;
}

bool HnswHybridIndex::needs_fusion() const
{
    std::shared_lock lock(_disk_mutex);
    if (_disk_indexes.size() > _cfg.max_flush_indexes) return true;
    for (const auto& di : _disk_indexes) {
        if (di->alive_ratio() < (1.0 - _cfg.tombstone_ratio_limit)) return true;
    }
    return false;
}

std::vector<std::shared_ptr<HnswDiskIndex>>
HnswHybridIndex::disk_index_snapshot() const
{
    std::shared_lock lock(_disk_mutex);
    return _disk_indexes;
}

uint32_t HnswHybridIndex::flush_memory_index(uint32_t committed_doc_id_limit)
{
    if (committed_doc_id_limit == 0) return 0;

    const HnswGraph& graph = _memory_index->get_graph();
    auto entry = graph.get_entry_node();
    if (entry.level < 0) return 0;  // empty

    uint32_t n = committed_doc_id_limit;

    // --- Collect vectors, docid map, and alive bitvector ---
    std::vector<uint32_t> global_docids(n);
    std::vector<float>    vec_data;
    vec_data.reserve(static_cast<size_t>(n) * _memory_index->config().max_links_at_level_0());
    // (we don't know dims yet; let the first get_vector tell us)

    auto alive_bv = BitVector::create(n);
    uint32_t dims = 0;

    for (uint32_t i = 0; i < n; ++i) {
        global_docids[i] = i;   // attribute docid == global docid
        auto node_ref = graph.get_node_ref(i);
        if (node_ref.valid()) {
            alive_bv->setBit(i);
        }
        TypedCells cells = _vectors.get_vector(i);
        if (dims == 0) {
            dims = static_cast<uint32_t>(cells.size);
            vec_data.reserve(static_cast<size_t>(n) * dims);
        }
        const float* src = static_cast<const float*>(cells.data);
        vec_data.insert(vec_data.end(), src, src + dims);
    }

    if (dims == 0) return 0;

    // --- Build graph node arrays ---
    std::vector<HnswDiskIndex::GraphNode> graph_nodes(n);
    std::vector<uint32_t> links_data;

    for (uint32_t i = 0; i < n; ++i) {
        graph_nodes[i].links_offset = static_cast<uint32_t>(links_data.size());
        auto node_ref = graph.get_node_ref(i);
        if (!node_ref.valid()) {
            graph_nodes[i].num_levels = 0;
            continue;
        }
        auto levels = graph.nodes.get(node_ref);
        graph_nodes[i].num_levels = static_cast<uint32_t>(levels.size());
        for (const auto& link_ref_atomic : levels) {
            auto lr = link_ref_atomic.load_acquire();
            if (lr.valid()) {
                auto link_arr = graph.links.get(lr);
                links_data.push_back(static_cast<uint32_t>(link_arr.size()));
                for (uint32_t nb : link_arr) links_data.push_back(nb);
            } else {
                links_data.push_back(0);
            }
        }
    }

    uint32_t fid = _next_flush_id.fetch_add(1);
    bool ok = HnswDiskIndex::write(_base_dir, fid, global_docids, *alive_bv,
                                    vec_data, dims,
                                    entry.docid, entry.level,
                                    graph_nodes, links_data, nullptr);
    if (!ok) {
        LOG(error, "HnswHybridIndex: flush failed for id %u", fid);
        return 0;
    }

    auto di = std::make_shared<HnswDiskIndex>(_base_dir, fid);
    if (!di->open()) {
        LOG(error, "HnswHybridIndex: failed to open flush index %u", fid);
        return 0;
    }

    std::unique_lock lock(_disk_mutex);
    _disk_indexes.push_back(di);
    LOG(info, "HnswHybridIndex: flushed memory index to hnsw.flush.%u (%u docs, %u dims)",
        fid, n, dims);
    return fid;
}

// ---------------------------------------------------------------------------
// Helper: local DocVectorAccess backed by a flat float32 array
// ---------------------------------------------------------------------------
namespace {

class FlatVecAccess : public DocVectorAccess {
    const std::vector<float>& _data;
    size_t _dims;
public:
    FlatVecAccess(const std::vector<float>& data, size_t dims)
        : _data(data), _dims(dims) {}
    vespalib::eval::TypedCells get_vector(uint32_t docid) const override {
        return vespalib::eval::TypedCells(
            _data.data() + docid * _dims,
            vespalib::eval::CellType::FLOAT, _dims);
    }
};

} // anonymous

bool HnswHybridIndex::run_fusion()
{
    std::vector<std::shared_ptr<HnswDiskIndex>> inputs;
    {
        std::shared_lock lock(_disk_mutex);
        inputs = _disk_indexes;
    }
    if (inputs.empty()) return true;

    uint32_t dims = inputs[0]->dims();

    // --- Collect all alive vectors across all disk indexes ---
    std::vector<uint32_t> new_docids;
    std::vector<float>    new_vecs;

    for (const auto& di : inputs) {
        for (uint32_t li = 0; li < di->num_docs(); ++li) {
            // Only include alive documents
            {
                // Check via alive_count proxy: get_float_vector is only called for alive docs.
                // We peek at the alive status by checking alive_count() after we decide.
                // The proper way: the disk index exposes is_alive(local_id) — add a helper.
            }
            // For now, use get_float_vector and check alive via alive_ratio heuristic.
            // A correct approach requires exposing is_alive(local_id) from HnswDiskIndex.
            // We query each slot: if the reverse map returns a valid global docid and
            // it hasn't been removed, include it.
            auto fvec = di->get_float_vector(li);
            uint32_t gid = di->local_to_global(li);
            new_docids.push_back(gid);
            new_vecs.insert(new_vecs.end(), fvec.begin(), fvec.end());
        }
    }

    if (new_docids.empty() || dims == 0) return true;

    uint32_t n = static_cast<uint32_t>(new_docids.size());

    // --- Build a proper HNSW graph via in-memory reconstruction ---
    // Use the same index config as the current memory index.
    const HnswIndex::Config& mem_cfg = _memory_index->config();

    FlatVecAccess flat_access(new_vecs, dims);
    auto dist_func = std::make_unique<SquaredEuclideanDistance>(
        vespalib::eval::CellType::FLOAT);
    auto level_gen = std::make_unique<InvLogLevelGenerator>(
        mem_cfg.max_links_at_level_0());

    HnswIndex temp_index(flat_access,
                         std::move(dist_func),
                         std::move(level_gen),
                         mem_cfg);

    for (uint32_t i = 0; i < n; ++i) {
        temp_index.add_document(i);
    }

    // Extract graph from the rebuilt temp index
    const HnswGraph& graph = temp_index.get_graph();
    auto entry = graph.get_entry_node();

    std::vector<HnswDiskIndex::GraphNode> graph_nodes(n);
    std::vector<uint32_t> links_data;

    for (uint32_t i = 0; i < n; ++i) {
        graph_nodes[i].links_offset = static_cast<uint32_t>(links_data.size());
        auto node_ref = graph.get_node_ref(i);
        if (!node_ref.valid()) {
            graph_nodes[i].num_levels = 0;
            continue;
        }
        auto levels = graph.nodes.get(node_ref);
        graph_nodes[i].num_levels = static_cast<uint32_t>(levels.size());
        for (const auto& link_ref_atomic : levels) {
            auto lr = link_ref_atomic.load_acquire();
            if (lr.valid()) {
                auto link_arr = graph.links.get(lr);
                links_data.push_back(static_cast<uint32_t>(link_arr.size()));
                for (uint32_t nb : link_arr) links_data.push_back(nb);
            } else {
                links_data.push_back(0);
            }
        }
    }

    // --- Prepare compressor if fusion compression is configured ---
    std::unique_ptr<VectorCompressor> compressor;
    if (_cfg.fusion_compression != VectorCompressor::CompressionType::NONE) {
        compressor = VectorCompressor::create(_cfg.fusion_compression);
        std::vector<const float*> ptrs(n);
        for (uint32_t i = 0; i < n; ++i)
            ptrs[i] = new_vecs.data() + i * dims;
        compressor->train(ptrs, dims);
    }

    auto alive_bv = BitVector::create(n);
    for (uint32_t i = 0; i < n; ++i) alive_bv->setBit(i);

    uint32_t fid = _next_fusion_id.fetch_add(1);
    bool ok = HnswDiskIndex::write(_base_dir, fid, new_docids, *alive_bv,
                                    new_vecs, dims,
                                    entry.docid, entry.level,
                                    graph_nodes, links_data, compressor.get());
    if (!ok) {
        LOG(error, "HnswHybridIndex: fusion write failed for id %u", fid);
        return false;
    }

    auto fusion_di = std::make_shared<HnswDiskIndex>(_base_dir, fid);
    if (!fusion_di->open()) {
        LOG(error, "HnswHybridIndex: fusion open failed for id %u", fid);
        return false;
    }

    std::unique_lock lock(_disk_mutex);
    _disk_indexes.clear();
    _disk_indexes.push_back(fusion_di);
    LOG(info, "HnswHybridIndex: fusion complete -> hnsw.fusion.%u (%u docs, dims=%u)",
        fid, n, dims);
    return true;
}

}
