// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "louds_trie.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <queue>
#include <stdexcept>

namespace vespalib::succinct {

LoudsTrie::LoudsTrie() noexcept
    : _louds(),
      _has_zpath(),
      _labels_store(),
      _zpath_data_store(),
      _zpath_offsets_store(),
      _zpath_lengths_store(),
      _child_bitmaps_store(),
      _labels(nullptr),
      _zpath_data(nullptr),
      _zpath_offsets(nullptr),
      _zpath_lengths(nullptr),
      _child_bitmaps(nullptr),
      _zpath_data_size(0),
      _num_zpaths(0),
      _num_bitmap_nodes(0),
      _is_bitmap_node(),
      _first_child_id(),
      _child_count(),
      _num_nodes(0),
      _mmap_backed(false)
{
}

LoudsTrie::~LoudsTrie() = default;

LoudsTrie::LoudsTrie(LoudsTrie &&o) noexcept
    : _louds(std::move(o._louds)),
      _has_zpath(std::move(o._has_zpath)),
      _labels_store(std::move(o._labels_store)),
      _zpath_data_store(std::move(o._zpath_data_store)),
      _zpath_offsets_store(std::move(o._zpath_offsets_store)),
      _zpath_lengths_store(std::move(o._zpath_lengths_store)),
      _child_bitmaps_store(std::move(o._child_bitmaps_store)),
      _labels(nullptr),
      _zpath_data(nullptr),
      _zpath_offsets(nullptr),
      _zpath_lengths(nullptr),
      _child_bitmaps(nullptr),
      _zpath_data_size(o._zpath_data_size),
      _num_zpaths(o._num_zpaths),
      _num_bitmap_nodes(o._num_bitmap_nodes),
      _is_bitmap_node(std::move(o._is_bitmap_node)),
      _first_child_id(std::move(o._first_child_id)),
      _child_count(std::move(o._child_count)),
      _num_nodes(o._num_nodes),
      _mmap_backed(o._mmap_backed)
{
    if (_mmap_backed) {
        _labels = o._labels;
        _zpath_data = o._zpath_data;
        _zpath_offsets = o._zpath_offsets;
        _zpath_lengths = o._zpath_lengths;
        _child_bitmaps = o._child_bitmaps;
    } else {
        sync_pointers();
    }
    o._labels = nullptr;
    o._zpath_data = nullptr;
    o._zpath_offsets = nullptr;
    o._zpath_lengths = nullptr;
    o._child_bitmaps = nullptr;
    o._num_nodes = 0;
}

LoudsTrie &
LoudsTrie::operator=(LoudsTrie &&o) noexcept
{
    if (this != &o) {
        _louds = std::move(o._louds);
        _has_zpath = std::move(o._has_zpath);
        _labels_store = std::move(o._labels_store);
        _zpath_data_store = std::move(o._zpath_data_store);
        _zpath_offsets_store = std::move(o._zpath_offsets_store);
        _zpath_lengths_store = std::move(o._zpath_lengths_store);
        _child_bitmaps_store = std::move(o._child_bitmaps_store);
        _zpath_data_size = o._zpath_data_size;
        _num_zpaths = o._num_zpaths;
        _num_bitmap_nodes = o._num_bitmap_nodes;
        _is_bitmap_node = std::move(o._is_bitmap_node);
        _first_child_id = std::move(o._first_child_id);
        _child_count = std::move(o._child_count);
        _num_nodes = o._num_nodes;
        _mmap_backed = o._mmap_backed;
        if (_mmap_backed) {
            _labels = o._labels;
            _zpath_data = o._zpath_data;
            _zpath_offsets = o._zpath_offsets;
            _zpath_lengths = o._zpath_lengths;
            _child_bitmaps = o._child_bitmaps;
        } else {
            sync_pointers();
        }
        o._labels = nullptr;
        o._zpath_data = nullptr;
        o._zpath_offsets = nullptr;
        o._zpath_lengths = nullptr;
        o._child_bitmaps = nullptr;
        o._num_nodes = 0;
    }
    return *this;
}

void
LoudsTrie::sync_pointers() noexcept
{
    _labels = _labels_store.data();
    _zpath_data = _zpath_data_store.data();
    _zpath_offsets = _zpath_offsets_store.data();
    _zpath_lengths = _zpath_lengths_store.data();
    _child_bitmaps = _child_bitmaps_store.data();
    _zpath_data_size = _zpath_data_store.size();
    _num_zpaths = _zpath_offsets_store.size();
    _num_bitmap_nodes = _child_bitmaps_store.size() / 4;
}

void
LoudsTrie::rebuild_nav_arrays()
{
    _first_child_id.assign(_num_nodes, INVALID);
    _child_count.assign(_num_nodes, 0);
    uint32_t next_child_id = 1;
    for (uint32_t i = 0; i < _num_nodes; ++i) {
        uint32_t pos = _louds.select0(i) + 1;
        uint32_t degree = _louds.one_seq_len(pos);
        _child_count[i] = degree;
        if (degree > 0) {
            _first_child_id[i] = next_child_id;
            next_child_id += degree;
        }
    }
}

uint32_t
LoudsTrie::find_child(uint32_t node, uint8_t label) const noexcept
{
    uint32_t fc = _first_child_id[node];
    if (fc == INVALID) return INVALID;

    uint32_t nc = _child_count[node];

    if (_is_bitmap_node.size() > 0 && node < _is_bitmap_node.size() && _is_bitmap_node.test(node)) {
        // Bitmap lookup: O(1) via popcount
        uint32_t bm_idx = _is_bitmap_node.rank1(node);
        uint32_t word_idx = bm_idx * 4 + (label / 64);
        uint64_t bm_word = _child_bitmaps[word_idx];
        uint32_t bit = label % 64;

        if (!((bm_word >> bit) & 1)) return INVALID;

        // Count set bits before this position to get child index
        uint32_t child_offset = 0;
        for (uint32_t w = 0; w < (label / 64); ++w) {
            child_offset += Optimized::popCount(_child_bitmaps[bm_idx * 4 + w]);
        }
        uint64_t mask = (uint64_t{1} << bit) - 1;
        child_offset += Optimized::popCount(bm_word & mask);

        return fc + child_offset;
    }

    // Linear scan for small fanout (faster than binary search due to
    // sequential access and branch prediction).
    // For larger fanout, use binary search.
    if (nc <= 16) {
        for (uint32_t i = fc; i < fc + nc; ++i) {
            if (_labels[i] == label) return i;
            if (_labels[i] > label) return INVALID; // sorted, early exit
        }
        return INVALID;
    }

    // Binary search on labels for medium fanout
    uint32_t lo = fc;
    uint32_t hi = fc + nc;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (_labels[mid] < label) {
            lo = mid + 1;
        } else if (_labels[mid] > label) {
            hi = mid;
        } else {
            return mid;
        }
    }
    return INVALID;
}

uint32_t
LoudsTrie::match_zpath(uint32_t node, const uint8_t *key, uint32_t key_len,
                       uint32_t key_pos) const noexcept
{
    if (!has_zpath(node)) return 0;

    auto zp = zpath(node);
    if (key_pos + zp.len > key_len) return INVALID;

    if (std::memcmp(key + key_pos, zp.data, zp.len) != 0) return INVALID;

    return zp.len;
}

size_t
LoudsTrie::memory_usage() const noexcept
{
    return _louds.memory_usage()
         + _has_zpath.memory_usage()
         + _num_nodes * sizeof(uint8_t)
         + _zpath_data_size * sizeof(uint8_t)
         + _num_zpaths * sizeof(uint32_t)
         + _num_zpaths * sizeof(uint16_t)
         + _is_bitmap_node.memory_usage()
         + _num_bitmap_nodes * 4 * sizeof(uint64_t)
         + _first_child_id.size() * sizeof(uint32_t)
         + _child_count.size() * sizeof(uint16_t)
         + sizeof(LoudsTrie);
}

size_t
LoudsTrie::serialized_size() const noexcept
{
    // Header: num_nodes(4) + louds_size(4) + zpath_data_size(4) + num_zpaths(4)
    //         + num_bitmap_nodes(4) = 20 bytes
    return 20
         + _louds.serialized_size()
         + _has_zpath.serialized_size()
         + _num_nodes
         + _zpath_data_size
         + _num_zpaths * sizeof(uint32_t)
         + _num_zpaths * sizeof(uint16_t)
         + _is_bitmap_node.serialized_size()
         + _num_bitmap_nodes * 4 * sizeof(uint64_t);
}

void
LoudsTrie::serialize(void *buf) const noexcept
{
    auto *p = static_cast<uint8_t *>(buf);

    std::memcpy(p, &_num_nodes, 4); p += 4;
    uint32_t louds_ser_size = _louds.serialized_size();
    std::memcpy(p, &louds_ser_size, 4); p += 4;
    std::memcpy(p, &_zpath_data_size, 4); p += 4;
    std::memcpy(p, &_num_zpaths, 4); p += 4;
    std::memcpy(p, &_num_bitmap_nodes, 4); p += 4;

    _louds.serialize(p); p += louds_ser_size;
    _has_zpath.serialize(p); p += _has_zpath.serialized_size();
    std::memcpy(p, _labels, _num_nodes); p += _num_nodes;
    std::memcpy(p, _zpath_data, _zpath_data_size); p += _zpath_data_size;
    std::memcpy(p, _zpath_offsets, _num_zpaths * sizeof(uint32_t)); p += _num_zpaths * sizeof(uint32_t);
    std::memcpy(p, _zpath_lengths, _num_zpaths * sizeof(uint16_t)); p += _num_zpaths * sizeof(uint16_t);
    _is_bitmap_node.serialize(p); p += _is_bitmap_node.serialized_size();
    std::memcpy(p, _child_bitmaps, _num_bitmap_nodes * 4 * sizeof(uint64_t));
}

void
LoudsTrie::load(const void *buf, size_t len)
{
    auto *p = static_cast<const uint8_t *>(buf);
    if (len < 20) throw std::runtime_error("LoudsTrie::load: buffer too small");

    uint32_t louds_ser_size, zpath_data_size, num_zpaths, num_bitmap_nodes;
    std::memcpy(&_num_nodes, p, 4); p += 4;
    std::memcpy(&louds_ser_size, p, 4); p += 4;
    std::memcpy(&zpath_data_size, p, 4); p += 4;
    std::memcpy(&num_zpaths, p, 4); p += 4;
    std::memcpy(&num_bitmap_nodes, p, 4); p += 4;

    auto remaining = [&]() -> size_t {
        return len - (p - static_cast<const uint8_t*>(buf));
    };

    if (louds_ser_size > remaining()) {
        throw std::runtime_error("LoudsTrie::load: louds_ser_size exceeds buffer");
    }
    _louds.load(p, louds_ser_size); p += louds_ser_size;

    _has_zpath.load(p, remaining());
    p += _has_zpath.serialized_size();

    size_t dataNeeded = (size_t)_num_nodes + zpath_data_size
                       + num_zpaths * (sizeof(uint32_t) + sizeof(uint16_t))
                       + num_bitmap_nodes * 4 * sizeof(uint64_t);
    if (dataNeeded > remaining()) {
        throw std::runtime_error("LoudsTrie::load: buffer too small for data fields");
    }

    _labels_store.resize(_num_nodes);
    std::memcpy(_labels_store.data(), p, _num_nodes); p += _num_nodes;

    _zpath_data_store.resize(zpath_data_size);
    std::memcpy(_zpath_data_store.data(), p, zpath_data_size); p += zpath_data_size;

    _zpath_offsets_store.resize(num_zpaths);
    std::memcpy(_zpath_offsets_store.data(), p, num_zpaths * sizeof(uint32_t)); p += num_zpaths * sizeof(uint32_t);

    _zpath_lengths_store.resize(num_zpaths);
    std::memcpy(_zpath_lengths_store.data(), p, num_zpaths * sizeof(uint16_t)); p += num_zpaths * sizeof(uint16_t);

    _is_bitmap_node.load(p, remaining());
    p += _is_bitmap_node.serialized_size();

    if (num_bitmap_nodes * 4 * sizeof(uint64_t) > remaining()) {
        throw std::runtime_error("LoudsTrie::load: buffer too small for bitmaps");
    }
    _child_bitmaps_store.resize(num_bitmap_nodes * 4);
    std::memcpy(_child_bitmaps_store.data(), p, num_bitmap_nodes * 4 * sizeof(uint64_t));

    sync_pointers();
    _mmap_backed = false;

    // Rebuild precomputed navigation arrays from LOUDS
    rebuild_nav_arrays();
}

size_t
LoudsTrie::setup_mmap(const void *buf, size_t len)
{
    auto *start = static_cast<const uint8_t *>(buf);
    auto *p = start;
    if (len < 20) throw std::runtime_error("LoudsTrie::setup_mmap: buffer too small");

    uint32_t louds_ser_size, zpath_data_size, num_zpaths, num_bitmap_nodes;
    std::memcpy(&_num_nodes, p, 4); p += 4;
    std::memcpy(&louds_ser_size, p, 4); p += 4;
    std::memcpy(&zpath_data_size, p, 4); p += 4;
    std::memcpy(&num_zpaths, p, 4); p += 4;
    std::memcpy(&num_bitmap_nodes, p, 4); p += 4;

    auto remaining = [&]() -> size_t {
        return len - (p - start);
    };

    if (louds_ser_size > remaining()) {
        throw std::runtime_error("LoudsTrie::setup_mmap: louds_ser_size exceeds buffer");
    }
    size_t louds_consumed = _louds.setup_mmap(p, louds_ser_size);
    p += louds_consumed;

    size_t has_zpath_consumed = _has_zpath.setup_mmap(p, remaining());
    p += has_zpath_consumed;

    // Point directly into mmap'd memory
    _labels = p;
    p += _num_nodes;

    _zpath_data = p;
    _zpath_data_size = zpath_data_size;
    p += zpath_data_size;

    _zpath_offsets = reinterpret_cast<const uint32_t *>(p);
    _num_zpaths = num_zpaths;
    p += num_zpaths * sizeof(uint32_t);

    _zpath_lengths = reinterpret_cast<const uint16_t *>(p);
    p += num_zpaths * sizeof(uint16_t);

    size_t is_bm_consumed = _is_bitmap_node.setup_mmap(p, remaining());
    p += is_bm_consumed;

    _child_bitmaps = reinterpret_cast<const uint64_t *>(p);
    _num_bitmap_nodes = num_bitmap_nodes;
    p += num_bitmap_nodes * 4 * sizeof(uint64_t);

    // Release owned storage
    _labels_store.clear(); _labels_store.shrink_to_fit();
    _zpath_data_store.clear(); _zpath_data_store.shrink_to_fit();
    _zpath_offsets_store.clear(); _zpath_offsets_store.shrink_to_fit();
    _zpath_lengths_store.clear(); _zpath_lengths_store.shrink_to_fit();
    _child_bitmaps_store.clear(); _child_bitmaps_store.shrink_to_fit();

    _mmap_backed = true;

    // Navigation arrays are always computed (not serialized)
    rebuild_nav_arrays();

    return p - start;
}

// ============================================================================
// Builder
// ============================================================================

LoudsTrie::Builder::Builder()
    : _nodes(),
      _num_keys(0),
      _last_key()
{
    _nodes.emplace_back(); // root node
}

LoudsTrie::Builder::~Builder() = default;

void
LoudsTrie::Builder::split_node(uint32_t node_idx, uint32_t zpath_pos)
{
    assert(zpath_pos < _nodes[node_idx].zpath.size());

    // Save data before emplace_back invalidates references
    auto old_children = std::move(_nodes[node_idx].children);
    auto old_zpath = _nodes[node_idx].zpath; // copy
    uint8_t split_byte = old_zpath[zpath_pos];

    uint32_t new_child_idx = _nodes.size();
    _nodes.emplace_back();
    // After emplace_back, use indices only
    _nodes[new_child_idx].children = std::move(old_children);
    _nodes[new_child_idx].zpath.assign(old_zpath.begin() + zpath_pos + 1, old_zpath.end());

    _nodes[node_idx].children.clear();
    _nodes[node_idx].children.push_back({split_byte, new_child_idx});
    _nodes[node_idx].zpath.resize(zpath_pos);
}

void
LoudsTrie::Builder::insert(const uint8_t *key, uint32_t key_len, uint32_t pos, uint32_t node_idx)
{
    // IMPORTANT: never hold references to _nodes[] across emplace_back calls.

    // Match zpath
    uint32_t zp_match = 0;
    uint32_t zp_size = _nodes[node_idx].zpath.size();
    while (zp_match < zp_size && pos < key_len) {
        if (key[pos] != _nodes[node_idx].zpath[zp_match]) {
            // Mismatch in zpath: split
            split_node(node_idx, zp_match);
            if (pos < key_len) {
                uint32_t leaf_idx = _nodes.size();
                _nodes.emplace_back();
                _nodes[leaf_idx].zpath.assign(key + pos + 1, key + key_len);
                _nodes[node_idx].children.push_back({key[pos], leaf_idx});
                auto &ch = _nodes[node_idx].children;
                if (ch.size() == 2 && ch[0].first > ch[1].first) {
                    std::swap(ch[0], ch[1]);
                }
            }
            return;
        }
        ++zp_match;
        ++pos;
    }

    if (zp_match < _nodes[node_idx].zpath.size()) {
        split_node(node_idx, zp_match);
        return;
    }

    if (pos >= key_len) {
        return;
    }

    // Find or create child
    uint8_t next_byte = key[pos];
    for (const auto &[lbl, child_idx] : _nodes[node_idx].children) {
        if (lbl == next_byte) {
            insert(key, key_len, pos + 1, child_idx);
            return;
        }
    }

    // New child — keys are added in sorted order, goes at the end
    uint32_t new_idx = _nodes.size();
    _nodes.emplace_back();
    _nodes[new_idx].zpath.assign(key + pos + 1, key + key_len);
    _nodes[node_idx].children.push_back({next_byte, new_idx});
}

void
LoudsTrie::Builder::add(const uint8_t *key, uint32_t key_len)
{
    std::string key_str(reinterpret_cast<const char *>(key), key_len);
    assert(_num_keys == 0 || key_str > _last_key);
    _last_key = key_str;
    ++_num_keys;

    insert(key, key_len, 0, 0);
}

std::unique_ptr<LoudsTrie>
LoudsTrie::Builder::build()
{
    auto trie = std::make_unique<LoudsTrie>();
    if (_nodes.empty()) return trie;

    uint32_t num_nodes = _nodes.size();
    trie->_num_nodes = num_nodes;

    // BFS to produce LOUDS encoding, reorder nodes, and compute navigation arrays
    std::vector<uint64_t> louds_bits;
    std::vector<uint8_t> labels(num_nodes, 0);
    std::vector<bool> has_zpath_bits(num_nodes, false);
    std::vector<uint8_t> zpath_data;
    std::vector<uint32_t> zpath_offsets;
    std::vector<uint16_t> zpath_lengths;
    std::vector<bool> is_bitmap_bits(num_nodes, false);
    std::vector<uint64_t> child_bitmaps;
    std::vector<uint32_t> first_child_ids(num_nodes, INVALID);
    std::vector<uint16_t> child_counts(num_nodes, 0);

    uint32_t louds_bit_count = 0;
    auto push_bit = [&](bool bit) {
        uint32_t word_idx = louds_bit_count / 64;
        uint32_t bit_idx = louds_bit_count % 64;
        if (word_idx >= louds_bits.size()) {
            louds_bits.push_back(0);
        }
        if (bit) {
            louds_bits[word_idx] |= (uint64_t{1} << bit_idx);
        }
        ++louds_bit_count;
    };

    struct BfsEntry {
        uint32_t old_idx;
        uint8_t label;
    };
    std::queue<BfsEntry> bfs_queue;
    bfs_queue.push({0, 0});

    // LOUDS super-root: "10"
    push_bit(true);
    push_bit(false);

    uint32_t bfs_idx = 0;
    uint32_t next_child_id = 1;
    while (!bfs_queue.empty()) {
        auto [old_idx, incoming_label] = bfs_queue.front();
        bfs_queue.pop();

        auto &node = _nodes[old_idx];
        uint32_t degree = node.children.size();

        labels[bfs_idx] = incoming_label;

        // Compute navigation arrays directly during BFS
        child_counts[bfs_idx] = degree;
        if (degree > 0) {
            first_child_ids[bfs_idx] = next_child_id;
            next_child_id += degree;
        }

        // Write LOUDS: d ones + one zero
        for (uint32_t i = 0; i < degree; ++i) push_bit(true);
        push_bit(false);

        // Zpath
        if (!node.zpath.empty()) {
            has_zpath_bits[bfs_idx] = true;
            zpath_offsets.push_back(zpath_data.size());
            zpath_lengths.push_back(node.zpath.size());
            zpath_data.insert(zpath_data.end(), node.zpath.begin(), node.zpath.end());
        }

        // Bitmap for high-fanout nodes
        if (degree >= BITMAP_THRESHOLD) {
            is_bitmap_bits[bfs_idx] = true;
            uint64_t bm[4] = {0, 0, 0, 0};
            for (auto &[lbl, child_idx] : node.children) {
                bm[lbl / 64] |= (uint64_t{1} << (lbl % 64));
            }
            child_bitmaps.insert(child_bitmaps.end(), bm, bm + 4);
        }

        for (auto &[lbl, child_idx] : node.children) {
            bfs_queue.push({child_idx, lbl});
        }

        ++bfs_idx;
    }

    // Build LOUDS RankSelect
    trie->_louds.build(louds_bits.data(), louds_bit_count);

    // Build has_zpath RankSelect
    {
        uint32_t num_words = (num_nodes + 63) / 64;
        std::vector<uint64_t> zpath_words(num_words, 0);
        for (uint32_t i = 0; i < num_nodes; ++i) {
            if (has_zpath_bits[i]) {
                zpath_words[i / 64] |= (uint64_t{1} << (i % 64));
            }
        }
        trie->_has_zpath.build(zpath_words.data(), num_nodes);
    }

    // Build is_bitmap_node RankSelect
    {
        uint32_t num_words = (num_nodes + 63) / 64;
        std::vector<uint64_t> bm_words(num_words, 0);
        for (uint32_t i = 0; i < num_nodes; ++i) {
            if (is_bitmap_bits[i]) {
                bm_words[i / 64] |= (uint64_t{1} << (i % 64));
            }
        }
        trie->_is_bitmap_node.build(bm_words.data(), num_nodes);
    }

    trie->_labels_store = std::move(labels);
    trie->_zpath_data_store = std::move(zpath_data);
    trie->_zpath_offsets_store = std::move(zpath_offsets);
    trie->_zpath_lengths_store = std::move(zpath_lengths);
    trie->_child_bitmaps_store = std::move(child_bitmaps);

    trie->_first_child_id = std::move(first_child_ids);
    trie->_child_count = std::move(child_counts);

    trie->sync_pointers();
    trie->_mmap_backed = false;

    return trie;
}

} // namespace vespalib::succinct
