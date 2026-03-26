// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#pragma once

#include "louds_trie.h"
#include <cstdint>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace vespalib::succinct {

/**
 * Nested Louds Trie (NLT): a multi-level succinct dictionary.
 *
 * Provides O(key_length) point lookups mapping a key to its ordinal position
 * (0-based) among all stored keys. Returns NOT_FOUND for non-existent keys,
 * typically terminating early at the first non-matching character.
 *
 * The trie uses path compression (zpath) to collapse chains of single-child
 * nodes. The zpath strings are themselves organized into a nested LoudsTrie
 * recursively, achieving very high compression ratios especially on keys
 * with shared prefixes (URLs, file paths, product tokens).
 *
 * Immutable once built. Thread-safe for concurrent reads.
 * Supports zero-copy mmap via setup_mmap().
 **/
class NestedLoudsTrie {
public:
    static constexpr size_t NOT_FOUND = std::numeric_limits<size_t>::max();

    NestedLoudsTrie() noexcept;
    ~NestedLoudsTrie();
    NestedLoudsTrie(NestedLoudsTrie &&o) noexcept;
    NestedLoudsTrie &operator=(NestedLoudsTrie &&o) noexcept;

    size_t num_keys() const noexcept { return _num_keys; }
    bool empty() const noexcept { return _num_keys == 0; }

    /**
     * Look up a key. Returns its 0-based ordinal, or NOT_FOUND.
     * For non-existent keys, typically terminates early.
     **/
    size_t lookup(const uint8_t *key, uint32_t key_len) const noexcept;
    size_t lookup(const std::string &key) const noexcept {
        return lookup(reinterpret_cast<const uint8_t *>(key.data()), key.size());
    }
    size_t lookup(const char *key, uint32_t key_len) const noexcept {
        return lookup(reinterpret_cast<const uint8_t *>(key), key_len);
    }

    /**
     * Reconstruct the key at ordinal position nth (0-based).
     * Used by sequential read during fusion.
     **/
    void restore(size_t nth, std::string &word) const;

    /**
     * Iterator for sorted traversal. Produces keys in lexicographic order.
     **/
    class Iterator {
    public:
        Iterator(const NestedLoudsTrie &trie, bool at_end = false);
        ~Iterator();
        Iterator(Iterator &&) noexcept = default;
        Iterator &operator=(Iterator &&) noexcept = default;

        bool valid() const noexcept { return _trie && _ordinal < _trie->_num_keys; }
        size_t ordinal() const noexcept { return _ordinal; }
        const std::string &key() const noexcept { return _current_key; }

        void next();
        void seek_to_first();

    private:
        struct StackEntry {
            uint32_t node;
            uint32_t next_child;  // next child index to explore
            uint32_t num_children;
            uint32_t key_len;     // key length when this node was entered
        };

        void advance_to_next_term();

        const NestedLoudsTrie *_trie;
        std::vector<StackEntry> _stack;
        std::string _current_key;
        size_t _ordinal;
    };

    Iterator begin() const { return Iterator(*this, false); }
    Iterator end() const { return Iterator(*this, true); }

    size_t memory_usage() const noexcept;

    // Serialization
    size_t serialized_size() const noexcept;
    void serialize(void *buf) const noexcept;
    void load(const void *buf, size_t len);

    /**
     * Zero-copy mmap setup. Returns bytes consumed.
     * The caller must keep the underlying mmap alive while this object is in use.
     **/
    size_t setup_mmap(const void *buf, size_t len);

    class Builder;

private:
    friend class Iterator;
    void sync_pointers() noexcept;

    LoudsTrie _trie;
    RankSelect _is_term;                // marks terminal (accepting) nodes

    // Owned storage (empty in mmap mode)
    std::vector<uint32_t> _node_to_ord_store;
    std::vector<uint32_t> _ord_to_node_store;

    // Active data pointers
    const uint32_t* _node_to_ord;   // bfs_node_id -> lex ordinal (indexed by is_term.rank1)
    const uint32_t* _ord_to_node;   // lex ordinal -> bfs_node_id

    uint32_t _num_keys;
    bool _mmap_backed;
};

/**
 * Builds a NestedLoudsTrie from sorted, unique keys.
 **/
class NestedLoudsTrie::Builder {
public:
    explicit Builder(uint32_t nest_level = 4);
    ~Builder();

    void add(const uint8_t *key, uint32_t key_len);
    void add(const std::string &key) {
        add(reinterpret_cast<const uint8_t *>(key.data()), key.size());
    }
    void add(const char *key, uint32_t key_len) {
        add(reinterpret_cast<const uint8_t *>(key), key_len);
    }

    std::unique_ptr<NestedLoudsTrie> finish();

private:
    LoudsTrie::Builder _trie_builder;
    std::vector<std::string> _keys;
    uint32_t _nest_level;
};

} // namespace vespalib::succinct
