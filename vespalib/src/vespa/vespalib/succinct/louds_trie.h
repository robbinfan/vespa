// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#pragma once

#include "rank_select.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vespalib::succinct {

/**
 * LOUDS (Level-Order Unary Degree Sequence) encoded trie with path compression.
 *
 * Tree structure is encoded in a bit vector: for each node visited in BFS order,
 * we write d ones followed by a zero, where d is the number of children.
 * Navigation uses rank/select on this bit vector.
 *
 * Path compression (zpath): chains of single-child nodes are collapsed into
 * a single node with a multi-byte label. This is critical for space efficiency
 * with long keys (URLs, paths, etc).
 *
 * Child lookup uses adaptive strategy:
 *   - fanout < 36: sorted byte array, binary search
 *   - fanout >= 36: 256-bit bitmap with popcount for O(1) lookup
 *
 * Supports zero-copy mmap mode via setup_mmap().
 **/
class LoudsTrie {
public:
    static constexpr uint32_t INVALID = UINT32_MAX;
    static constexpr uint32_t BITMAP_THRESHOLD = 36;

    LoudsTrie() noexcept;
    ~LoudsTrie();
    LoudsTrie(LoudsTrie &&o) noexcept;
    LoudsTrie &operator=(LoudsTrie &&o) noexcept;

    uint32_t num_nodes() const noexcept { return _num_nodes; }
    bool empty() const noexcept { return _num_nodes == 0; }

    // Navigation — O(1) via precomputed arrays (no select0 needed)
    uint32_t root() const noexcept { return _num_nodes > 0 ? 0 : INVALID; }

    uint32_t first_child(uint32_t node) const noexcept {
        return _first_child_id[node];
    }

    uint32_t num_children(uint32_t node) const noexcept {
        return _child_count[node];
    }

    /**
     * Find child with given edge label. Returns child node id or INVALID.
     * Uses bitmap for high-fanout nodes, binary search otherwise.
     **/
    uint32_t find_child(uint32_t node, uint8_t label) const noexcept;

    uint8_t label(uint32_t node) const noexcept {
        return _labels[node];
    }

    // Path compression
    bool has_zpath(uint32_t node) const noexcept {
        return _has_zpath.test(node);
    }

    struct ZpathRef {
        const uint8_t *data;
        uint32_t len;
    };

    ZpathRef zpath(uint32_t node) const noexcept {
        uint32_t idx = _has_zpath.rank1(node);
        return {_zpath_data + _zpath_offsets[idx], _zpath_lengths[idx]};
    }

    /**
     * Combined has_zpath + zpath check. Avoids redundant rank1 call.
     * Returns length matched, 0 if no zpath, INVALID if mismatch.
     **/
    uint32_t match_zpath_fast(uint32_t node, const uint8_t *key,
                              uint32_t key_len, uint32_t key_pos) const noexcept {
        if (!_has_zpath.test(node)) return 0;
        uint32_t idx = _has_zpath.rank1(node);
        uint32_t zlen = _zpath_lengths[idx];
        if (key_pos + zlen > key_len) return INVALID;
        const uint8_t *zdata = _zpath_data + _zpath_offsets[idx];
        if (std::memcmp(key + key_pos, zdata, zlen) != 0) return INVALID;
        return zlen;
    }

    /**
     * Match zpath bytes against key starting at key_pos.
     * Returns number of matched bytes, or INVALID if mismatch.
     **/
    uint32_t match_zpath(uint32_t node, const uint8_t *key, uint32_t key_len,
                         uint32_t key_pos) const noexcept;

    size_t memory_usage() const noexcept;

    // Serialization
    size_t serialized_size() const noexcept;
    void serialize(void *buf) const noexcept;
    void load(const void *buf, size_t len);

    /**
     * Zero-copy mmap setup. Returns bytes consumed.
     * Navigation arrays (_first_child_id, _child_count) are still computed
     * and owned, but all serialized data points directly into mmap memory.
     **/
    size_t setup_mmap(const void *buf, size_t len);

    /**
     * Builder: constructs LoudsTrie from a sorted list of keys.
     * Keys must be unique and in lexicographic order.
     **/
    class Builder;

private:
    void rebuild_nav_arrays();
    void sync_pointers() noexcept;

    RankSelect _louds;          // tree structure
    RankSelect _has_zpath;      // marks nodes with path-compressed labels

    // Owned storage (empty in mmap mode)
    std::vector<uint8_t> _labels_store;
    std::vector<uint8_t> _zpath_data_store;
    std::vector<uint32_t> _zpath_offsets_store;
    std::vector<uint16_t> _zpath_lengths_store;
    std::vector<uint64_t> _child_bitmaps_store;

    // Active data pointers (used by all read methods)
    const uint8_t* _labels;
    const uint8_t* _zpath_data;
    const uint32_t* _zpath_offsets;
    const uint16_t* _zpath_lengths;
    const uint64_t* _child_bitmaps;
    uint32_t _zpath_data_size;
    uint32_t _num_zpaths;
    uint32_t _num_bitmap_nodes;

    RankSelect _is_bitmap_node;         // marks nodes using bitmap child lookup

    // Precomputed navigation arrays — always owned (not serialized)
    std::vector<uint32_t> _first_child_id;
    std::vector<uint16_t> _child_count;
    uint32_t _num_nodes;
    bool _mmap_backed;
};

/**
 * Builds a LoudsTrie from sorted keys via BFS.
 **/
class LoudsTrie::Builder {
public:
    Builder();
    ~Builder();

    void add(const uint8_t *key, uint32_t key_len);
    void add(const std::string &key) { add(reinterpret_cast<const uint8_t *>(key.data()), key.size()); }

    std::unique_ptr<LoudsTrie> build();

private:
    struct TrieNode {
        std::vector<std::pair<uint8_t, uint32_t>> children; // label -> child index
        std::vector<uint8_t> zpath;
    };

    void insert(const uint8_t *key, uint32_t key_len, uint32_t pos, uint32_t node_idx);
    void split_node(uint32_t node_idx, uint32_t zpath_pos);

    std::vector<TrieNode> _nodes;
    uint32_t _num_keys;
    std::string _last_key;
};

} // namespace vespalib::succinct
