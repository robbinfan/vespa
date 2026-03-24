// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "nearest_neighbor_index.h"
#include "prepare_result.h"
#include "hnsw_index.h"
#include "hnsw_disk_index.h"
#include "vector_compressor.h"
#include <vespa/eval/eval/typed_cells.h>
#include <vespa/vespalib/stllike/string.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace search::tensor {

/**
 * Hybrid HNSW index: one in-memory HnswIndex (the "write buffer") plus
 * zero or more read-only HnswDiskIndex instances representing past flushes
 * and fusions (analogous to Vespa's inverted-index architecture where a
 * MemoryIndex is backed by multiple DiskIndex + Fusion instances).
 *
 * Naming convention (mirroring searchcorespi):
 *   In-memory buffer  → "memory index"
 *   Flush output dir  → hnsw.flush.<id>/
 *   Fusion output dir → hnsw.fusion.<id>/   (same class, different id space)
 *
 * Concurrency model:
 *   - All writes (add / remove) go to the memory index via the single
 *     attribute writer thread.
 *   - Disk index list is protected by a shared_mutex (many readers,
 *     one writer during flush/fusion swap).
 *   - Deletions are propagated to all disk indexes by clearing the alive
 *     bit (atomic, lock-free per-BitVector).
 *
 * Flush / Fusion policy:
 *   Callers (HnswFlushTarget / HnswFusion) invoke flush_memory_index()
 *   and run_fusion() respectively.  These are heavy operations and must
 *   be called off the attribute writer thread.
 */
class HnswHybridIndex : public NearestNeighborIndex {
public:
    using TypedCells = vespalib::eval::TypedCells;

    struct Config {
        uint32_t flush_threshold_docs{100'000};
        uint32_t flush_threshold_mb{2048};
        uint32_t max_flush_indexes{8};
        double   tombstone_ratio_limit{0.2};
        VectorCompressor::CompressionType fusion_compression{
            VectorCompressor::CompressionType::NONE};
        uint32_t rerank_candidates{100};
    };

    HnswHybridIndex(std::unique_ptr<HnswIndex> memory_index,
                    vespalib::stringref base_dir,
                    const Config& cfg);

    ~HnswHybridIndex() override;

    // --- NearestNeighborIndex interface ---

    void add_document(uint32_t docid) override;

    std::unique_ptr<search::tensor::PrepareResult>
    prepare_add_document(uint32_t docid, TypedCells vector,
                         vespalib::GenerationHandler::Guard read_guard) const override;

    void complete_add_document(uint32_t docid,
                               std::unique_ptr<PrepareResult> prepare_result) override;

    void remove_document(uint32_t docid) override;

    void transfer_hold_lists(generation_t current_gen) override;
    void trim_hold_lists(generation_t first_used_gen) override;
    bool consider_compact(const CompactionStrategy& strategy) override;
    vespalib::MemoryUsage update_stat(const CompactionStrategy& strategy) override;
    vespalib::MemoryUsage memory_usage() const override;
    void populate_address_space_usage(search::AddressSpaceUsage& usage) const override;
    void get_state(const vespalib::slime::Inserter& inserter) const override;
    void shrink_lid_space(uint32_t doc_id_limit) override;

    std::unique_ptr<NearestNeighborIndexSaver> make_saver() const override;
    std::unique_ptr<NearestNeighborIndexLoader> make_loader(FastOS_FileInterface& file) override;

    std::vector<Neighbor> find_top_k(uint32_t k, TypedCells vector,
                                     uint32_t explore_k,
                                     double distance_threshold) const override;

    std::vector<Neighbor> find_top_k_with_filter(uint32_t k, TypedCells vector,
                                                  const BitVector& filter,
                                                  uint32_t explore_k,
                                                  double distance_threshold) const override;

    const DistanceFunction* distance_function() const override;

    // --- Hybrid-specific operations ---

    /** Number of docs in the in-memory index. */
    uint32_t memory_index_doc_count() const;

    /** True if flush threshold has been exceeded. */
    bool needs_flush() const;

    /** True if fusion should be triggered (too many flush indexes or tombstones). */
    bool needs_fusion() const;

    /**
     * Flush the current memory index to a new disk index.
     * Returns the new flush id on success, or 0 on failure.
     * Must be called off the attribute writer thread.
     *
     * @param global_docids  mapping from HnswIndex internal docid → global docid
     * @param alive          BitVector marking live documents (indexed by HnswIndex docid)
     */
    uint32_t flush_memory_index(const std::vector<uint32_t>& global_docids,
                                const search::BitVector& alive);

    /**
     * Merge all existing disk indexes into one fusion index and optionally
     * apply compression.  Replaces the disk index list atomically.
     * Must be called off the attribute writer thread.
     */
    bool run_fusion();

    /** Access the underlying memory index (for flush building). */
    HnswIndex& memory_index() { return *_memory_index; }
    const HnswIndex& memory_index() const { return *_memory_index; }

    const Config& config() const { return _cfg; }

    /** Current list of disk indexes (caller must hold read lock or use snapshot). */
    std::vector<std::shared_ptr<HnswDiskIndex>> disk_index_snapshot() const;

private:
    std::vector<Neighbor> merge_results(
        std::vector<Neighbor> mem_results,
        std::vector<std::vector<Neighbor>> disk_results,
        uint32_t k) const;

    std::unique_ptr<HnswIndex> _memory_index;
    vespalib::string _base_dir;
    Config _cfg;

    mutable std::shared_mutex _disk_mutex;
    std::vector<std::shared_ptr<HnswDiskIndex>> _disk_indexes;

    std::atomic<uint32_t> _next_flush_id{1};
    std::atomic<uint32_t> _next_fusion_id{1};
};

}
