// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "nearest_neighbor_index.h"
#include "vector_compressor.h"
#include <vespa/searchlib/common/bitvector.h>
#include <vespa/vespalib/stllike/string.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace search::tensor {

/**
 * A read-only, file-backed HNSW index representing one flush or fusion snapshot.
 *
 * Directory layout (analogous to `index.flush.<id>` for inverted indexes):
 *
 *   hnsw.flush.<id>/
 *     vectors.dat      -- raw or compressed vector data (one vector per entry)
 *     graph.dat        -- HNSW graph: entry point + per-node level/link arrays
 *     docidmap.dat     -- mapping from local slot → global docid (uint32_t[])
 *     alive.dat        -- BitVector; cleared when a document is deleted
 *     compressor.dat   -- VectorCompressor parameters (absent when type=NONE)
 *     meta.dat         -- HnswDiskIndexMeta (fixed-size header)
 *
 * Thread safety: one writer (flush/fusion builder) vs many concurrent readers.
 * Once opened with open(), the index is immutable except for alive.dat updates
 * (remove_document).  alive is protected by an internal mutex.
 */
class HnswDiskIndex {
public:
    /** Fixed-size metadata header written to meta.dat */
    struct Meta {
        uint32_t version{1};
        uint32_t num_docs{0};          // number of slots (including deleted)
        uint32_t dims{0};              // vector dimensionality
        uint32_t entry_docid{0};       // global docid of HNSW entry node
        int32_t  entry_level{-1};      // HNSW entry level (-1 = empty)
        uint32_t compression_type{0};  // VectorCompressor::CompressionType cast to uint32_t
        uint8_t  _pad[8]{};
    };
    static_assert(sizeof(Meta) == 32, "Meta size changed");

    /** One node in the graph as stored in graph.dat */
    struct GraphNode {
        uint32_t num_levels;     // number of levels this node participates in
        uint32_t links_offset;   // byte offset into links section for level-0 links
    };

    using UP = std::unique_ptr<HnswDiskIndex>;

    explicit HnswDiskIndex(vespalib::stringref base_dir, uint32_t id);
    ~HnswDiskIndex();

    /** Open an existing disk index from disk. Returns false on failure. */
    bool open();

    /** Flush id (matches directory suffix). */
    uint32_t id() const { return _id; }

    /** True if the disk index has been successfully opened. */
    bool is_open() const { return _is_open; }

    /** Number of alive documents (not yet a precise count — based on BitVector). */
    uint32_t alive_count() const;

    /** Total number of document slots (alive + deleted). */
    uint32_t num_docs() const { return _meta.num_docs; }

    uint32_t dims() const { return _meta.dims; }

    /** Ratio of alive documents. Used to trigger fusion when too many are deleted. */
    double alive_ratio() const;

    /** Mark a global docid as deleted in this disk index. Thread-safe. */
    void remove_document(uint32_t global_docid);

    /** Map local slot → global docid. */
    uint32_t local_to_global(uint32_t local_id) const { return _docid_map[local_id]; }

    /** Get raw (possibly compressed) vector bytes for local slot. */
    const void* raw_vector(uint32_t local_id) const;

    /** Get decompressed float32 vector for local slot (allocates on each call). */
    std::vector<float> get_float_vector(uint32_t local_id) const;

    /** Search: return at most k neighbors, visiting explore_k candidates. */
    std::vector<NearestNeighborIndex::Neighbor>
    find_top_k(uint32_t k, vespalib::eval::TypedCells query,
               uint32_t explore_k, double distance_threshold,
               const search::BitVector* filter = nullptr) const;

    const vespalib::string& base_dir() const { return _base_dir; }
    vespalib::string dir() const;

    // --- Static builder helpers ---

    /**
     * Write a new disk index from scratch.
     * Vectors are given as float32 (before compression).
     *
     * @param base_dir         parent directory
     * @param id               flush/fusion index id
     * @param global_docids    global docid for each slot (length = num_docs)
     * @param alive            BitVector[0..num_docs) marking live docs
     * @param vectors          float32 vectors, one per slot, length = num_docs*dims
     * @param dims             vector dimensionality
     * @param entry_local_id   local slot that is the HNSW entry node
     * @param entry_level      HNSW entry level
     * @param graph_nodes      per-node (num_levels, links section offset)
     * @param links_data       raw concatenated link arrays for all nodes/levels
     * @param compressor       quantizer to apply (nullptr → store float32)
     */
    static bool write(const vespalib::string& base_dir,
                      uint32_t id,
                      const std::vector<uint32_t>& global_docids,
                      const search::BitVector& alive,
                      const std::vector<float>& vectors,
                      uint32_t dims,
                      uint32_t entry_local_id,
                      int32_t  entry_level,
                      const std::vector<GraphNode>& graph_nodes,
                      const std::vector<uint32_t>&  links_data,
                      const VectorCompressor* compressor);

private:
    struct LinkRange {
        uint32_t offset;
        uint32_t count;
    };

    // Greedy HNSW search layer helper
    void search_layer(const float* query_vec,
                      uint32_t   entry_local,
                      int        level,
                      uint32_t   ef,
                      std::vector<std::pair<double,uint32_t>>& result,
                      const search::BitVector* filter) const;

    double calc_distance(const float* query, uint32_t local_id) const;

    LinkRange get_links(uint32_t local_id, uint32_t level) const;

    vespalib::string _base_dir;
    uint32_t         _id;
    bool             _is_open{false};

    Meta             _meta;

    // Loaded data blobs (owned)
    std::vector<uint8_t>   _vectors_data;    // raw (possibly compressed) vectors
    std::vector<GraphNode> _graph_nodes;     // per-node header
    std::vector<uint32_t>  _links_data;      // all link arrays concatenated
    std::vector<uint32_t>  _docid_map;       // local_id -> global_docid

    std::unique_ptr<BitVector>        _alive;
    mutable std::mutex                _alive_mutex;

    std::unique_ptr<VectorCompressor> _compressor;  // nullptr if NONE

    // Scratch buffer for decompressed query (reused per search call via thread_local)
};

}
