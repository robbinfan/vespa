// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "nested_louds_trie.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace vespalib::succinct {

// ============================================================================
// NestedLoudsTrie
// ============================================================================

NestedLoudsTrie::NestedLoudsTrie() noexcept
    : _trie(),
      _is_term(),
      _node_to_ord_store(),
      _ord_to_node_store(),
      _node_to_ord(nullptr),
      _ord_to_node(nullptr),
      _num_keys(0),
      _mmap_backed(false)
{
}

NestedLoudsTrie::~NestedLoudsTrie() = default;

NestedLoudsTrie::NestedLoudsTrie(NestedLoudsTrie &&o) noexcept
    : _trie(std::move(o._trie)),
      _is_term(std::move(o._is_term)),
      _node_to_ord_store(std::move(o._node_to_ord_store)),
      _ord_to_node_store(std::move(o._ord_to_node_store)),
      _node_to_ord(nullptr),
      _ord_to_node(nullptr),
      _num_keys(o._num_keys),
      _mmap_backed(o._mmap_backed)
{
    if (_mmap_backed) {
        _node_to_ord = o._node_to_ord;
        _ord_to_node = o._ord_to_node;
    } else {
        sync_pointers();
    }
    o._node_to_ord = nullptr;
    o._ord_to_node = nullptr;
    o._num_keys = 0;
}

NestedLoudsTrie &
NestedLoudsTrie::operator=(NestedLoudsTrie &&o) noexcept
{
    if (this != &o) {
        _trie = std::move(o._trie);
        _is_term = std::move(o._is_term);
        _node_to_ord_store = std::move(o._node_to_ord_store);
        _ord_to_node_store = std::move(o._ord_to_node_store);
        _num_keys = o._num_keys;
        _mmap_backed = o._mmap_backed;
        if (_mmap_backed) {
            _node_to_ord = o._node_to_ord;
            _ord_to_node = o._ord_to_node;
        } else {
            sync_pointers();
        }
        o._node_to_ord = nullptr;
        o._ord_to_node = nullptr;
        o._num_keys = 0;
    }
    return *this;
}

void
NestedLoudsTrie::sync_pointers() noexcept
{
    _node_to_ord = _node_to_ord_store.data();
    _ord_to_node = _ord_to_node_store.data();
}

size_t
NestedLoudsTrie::lookup(const uint8_t *key, uint32_t key_len) const noexcept
{
    if (_trie.empty()) return NOT_FOUND;

    uint32_t node = _trie.root();
    uint32_t pos = 0;

    // Match root's zpath
    uint32_t matched = _trie.match_zpath_fast(node, key, key_len, pos);
    if (matched == LoudsTrie::INVALID) return NOT_FOUND;
    pos += matched;

    while (pos < key_len) {
        uint32_t child = _trie.find_child(node, key[pos]);
        if (child == LoudsTrie::INVALID) return NOT_FOUND;
        ++pos;

        matched = _trie.match_zpath_fast(child, key, key_len, pos);
        if (matched == LoudsTrie::INVALID) return NOT_FOUND;
        pos += matched;

        node = child;
    }

    // pos == key_len: check if this node is terminal
    if (pos == key_len && _is_term.size() > 0 && node < _is_term.size() && _is_term.test(node)) {
        uint32_t term_idx = _is_term.rank1(node);
        return _node_to_ord[term_idx];
    }

    return NOT_FOUND;
}

void
NestedLoudsTrie::restore(size_t nth, std::string &word) const
{
    word.clear();
    if (nth >= _num_keys) return;

    Iterator it(*this, false);
    for (size_t i = 0; i < nth; ++i) {
        if (!it.valid()) return;
        it.next();
    }
    if (it.valid()) {
        word = it.key();
    }
}

size_t
NestedLoudsTrie::memory_usage() const noexcept
{
    return _trie.memory_usage()
         + _is_term.memory_usage()
         + _num_keys * sizeof(uint32_t) * 2
         + sizeof(NestedLoudsTrie);
}

size_t
NestedLoudsTrie::serialized_size() const noexcept
{
    return 4 // num_keys
         + 4 // trie_ser_size
         + _trie.serialized_size()
         + _is_term.serialized_size()
         + _num_keys * sizeof(uint32_t) * 2;
}

void
NestedLoudsTrie::serialize(void *buf) const noexcept
{
    auto *p = static_cast<uint8_t *>(buf);
    std::memcpy(p, &_num_keys, 4); p += 4;
    uint32_t trie_ser_size = _trie.serialized_size();
    std::memcpy(p, &trie_ser_size, 4); p += 4;
    _trie.serialize(p); p += trie_ser_size;
    _is_term.serialize(p); p += _is_term.serialized_size();
    std::memcpy(p, _node_to_ord, _num_keys * sizeof(uint32_t)); p += _num_keys * sizeof(uint32_t);
    std::memcpy(p, _ord_to_node, _num_keys * sizeof(uint32_t));
}

void
NestedLoudsTrie::load(const void *buf, size_t len)
{
    auto *p = static_cast<const uint8_t *>(buf);
    if (len < 8) throw std::runtime_error("NestedLoudsTrie::load: buffer too small");

    auto *base = p;
    auto remaining = [&]() -> size_t {
        return len - (p - base);
    };

    std::memcpy(&_num_keys, p, 4); p += 4;
    uint32_t trie_ser_size;
    std::memcpy(&trie_ser_size, p, 4); p += 4;

    if (trie_ser_size > remaining()) {
        throw std::runtime_error("NestedLoudsTrie::load: trie_ser_size exceeds buffer");
    }
    _trie.load(p, trie_ser_size); p += trie_ser_size;

    _is_term.load(p, remaining());
    p += _is_term.serialized_size();

    size_t ordArraysSize = (size_t)_num_keys * sizeof(uint32_t) * 2;
    if (ordArraysSize > remaining()) {
        throw std::runtime_error("NestedLoudsTrie::load: buffer too small for ordinal arrays");
    }
    _node_to_ord_store.resize(_num_keys);
    std::memcpy(_node_to_ord_store.data(), p, _num_keys * sizeof(uint32_t)); p += _num_keys * sizeof(uint32_t);
    _ord_to_node_store.resize(_num_keys);
    std::memcpy(_ord_to_node_store.data(), p, _num_keys * sizeof(uint32_t));

    sync_pointers();
    _mmap_backed = false;
}

size_t
NestedLoudsTrie::setup_mmap(const void *buf, size_t len)
{
    auto *start = static_cast<const uint8_t *>(buf);
    auto *p = start;
    if (len < 8) throw std::runtime_error("NestedLoudsTrie::setup_mmap: buffer too small");

    auto remaining = [&]() -> size_t {
        return len - (p - start);
    };

    std::memcpy(&_num_keys, p, 4); p += 4;
    uint32_t trie_ser_size;
    std::memcpy(&trie_ser_size, p, 4); p += 4;

    if (trie_ser_size > remaining()) {
        throw std::runtime_error("NestedLoudsTrie::setup_mmap: trie_ser_size exceeds buffer");
    }
    size_t trie_consumed = _trie.setup_mmap(p, trie_ser_size);
    p += trie_consumed;

    size_t is_term_consumed = _is_term.setup_mmap(p, remaining());
    p += is_term_consumed;

    size_t ordArraysSize = (size_t)_num_keys * sizeof(uint32_t) * 2;
    if (ordArraysSize > remaining()) {
        throw std::runtime_error("NestedLoudsTrie::setup_mmap: buffer too small for ordinal arrays");
    }

    // Point directly into mmap'd memory
    _node_to_ord = reinterpret_cast<const uint32_t *>(p);
    p += _num_keys * sizeof(uint32_t);
    _ord_to_node = reinterpret_cast<const uint32_t *>(p);
    p += _num_keys * sizeof(uint32_t);

    // Release owned storage
    _node_to_ord_store.clear(); _node_to_ord_store.shrink_to_fit();
    _ord_to_node_store.clear(); _ord_to_node_store.shrink_to_fit();

    _mmap_backed = true;
    return p - start;
}

// ============================================================================
// Iterator
// ============================================================================

NestedLoudsTrie::Iterator::Iterator(const NestedLoudsTrie &trie, bool at_end)
    : _trie(&trie),
      _stack(),
      _current_key(),
      _ordinal(at_end ? trie._num_keys : 0)
{
    if (!at_end && !trie.empty()) {
        seek_to_first();
    }
}

NestedLoudsTrie::Iterator::~Iterator() = default;

void
NestedLoudsTrie::Iterator::seek_to_first()
{
    _stack.clear();
    _current_key.clear();
    _ordinal = 0;

    if (_trie->_trie.empty()) return;

    uint32_t root = _trie->_trie.root();

    // Add root's zpath to key
    if (_trie->_trie.has_zpath(root)) {
        auto zp = _trie->_trie.zpath(root);
        _current_key.append(reinterpret_cast<const char *>(zp.data), zp.len);
    }

    uint32_t nc = _trie->_trie.num_children(root);
    _stack.push_back({root, 0, nc, 0});

    // Check if root is a term
    if (_trie->_is_term.size() > 0 && root < _trie->_is_term.size() && _trie->_is_term.test(root)) {
        return;
    }

    advance_to_next_term();
}

void
NestedLoudsTrie::Iterator::advance_to_next_term()
{
    while (!_stack.empty()) {
        auto &top = _stack.back();

        if (top.next_child < top.num_children) {
            uint32_t child = _trie->_trie.first_child(top.node) + top.next_child;
            top.next_child++;

            uint32_t saved_key_len = _current_key.size();

            _current_key.push_back(_trie->_trie.label(child));

            if (_trie->_trie.has_zpath(child)) {
                auto zp = _trie->_trie.zpath(child);
                _current_key.append(reinterpret_cast<const char *>(zp.data), zp.len);
            }

            uint32_t nc = _trie->_trie.num_children(child);
            _stack.push_back({child, 0, nc, saved_key_len});

            if (_trie->_is_term.size() > child && _trie->_is_term.test(child)) {
                return;
            }
        } else {
            uint32_t saved_len = top.key_len;
            _stack.pop_back();
            _current_key.resize(saved_len);
        }
    }

    _ordinal = _trie->_num_keys; // exhausted
}

void
NestedLoudsTrie::Iterator::next()
{
    if (!valid()) return;
    ++_ordinal;
    if (_ordinal >= _trie->_num_keys) return;
    advance_to_next_term();
}

// ============================================================================
// Builder
// ============================================================================

NestedLoudsTrie::Builder::Builder(uint32_t nest_level)
    : _trie_builder(),
      _keys(),
      _nest_level(nest_level)
{
}

NestedLoudsTrie::Builder::~Builder() = default;

void
NestedLoudsTrie::Builder::add(const uint8_t *key, uint32_t key_len)
{
    _keys.emplace_back(reinterpret_cast<const char *>(key), key_len);
    _trie_builder.add(key, key_len);
}

std::unique_ptr<NestedLoudsTrie>
NestedLoudsTrie::Builder::finish()
{
    auto nlt = std::make_unique<NestedLoudsTrie>();
    auto trie = _trie_builder.build();
    nlt->_trie = std::move(*trie);
    nlt->_num_keys = _keys.size();

    // Build is_term and ordinal mappings
    uint32_t num_nodes = nlt->_trie.num_nodes();
    uint32_t num_words = (num_nodes + 63) / 64;
    std::vector<uint64_t> term_bits(std::max(num_words, 1u), 0);
    std::vector<uint32_t> key_to_node(_keys.size());

    for (size_t ki = 0; ki < _keys.size(); ++ki) {
        const auto &key = _keys[ki];
        const uint8_t *kp = reinterpret_cast<const uint8_t *>(key.data());
        uint32_t klen = key.size();
        uint32_t node = nlt->_trie.root();
        uint32_t pos = 0;

        if (nlt->_trie.has_zpath(node)) {
            auto zp = nlt->_trie.zpath(node);
            pos += zp.len;
        }

        while (pos < klen) {
            uint32_t child = nlt->_trie.find_child(node, kp[pos]);
            if (child == LoudsTrie::INVALID) {
                assert(false && "Key not found in trie during finish()");
                break;
            }
            ++pos;

            if (nlt->_trie.has_zpath(child)) {
                auto zp = nlt->_trie.zpath(child);
                pos += zp.len;
            }
            node = child;
        }

        term_bits[node / 64] |= (uint64_t{1} << (node % 64));
        key_to_node[ki] = node;
    }

    nlt->_is_term.build(term_bits.data(), num_nodes);

    uint32_t num_terms = _keys.size();
    nlt->_node_to_ord_store.resize(num_terms);
    nlt->_ord_to_node_store.resize(num_terms);

    for (uint32_t i = 0; i < num_terms; ++i) {
        uint32_t bfs_node = key_to_node[i];
        uint32_t term_idx = nlt->_is_term.rank1(bfs_node);
        nlt->_node_to_ord_store[term_idx] = i;
        nlt->_ord_to_node_store[i] = bfs_node;
    }

    nlt->sync_pointers();
    nlt->_mmap_backed = false;

    return nlt;
}

} // namespace vespalib::succinct
