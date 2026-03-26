// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include <vespa/vespalib/stllike/string.h>
#include <vespa/vespalib/util/memoryusage.h>
#include <vespa/vespalib/util/generationhandler.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace vespalib::ptrie {

/**
 * Arena-based append-only allocator for trie nodes.
 *
 * Inspired by topling/CSPP's arena design:
 * - Append-only allocation (no in-place modification)
 * - 32-bit offsets as node references (saves memory vs 64-bit pointers)
 * - Thread-safe allocation via atomic bump pointer
 * - Old nodes become dead space, reclaimed by periodic compaction
 *
 * The append-only property is key for lock-free reads: readers see a consistent
 * snapshot because data at any offset is immutable once written.
 */
class NodeArena {
public:
    static constexpr uint32_t INVALID_REF = 0;
    static constexpr size_t   INITIAL_CAPACITY = 4096;

    NodeArena();
    ~NodeArena();

    // Allocate `bytes` in the arena. Returns offset to allocated block.
    // Thread-safe: uses CAS bump pointer for concurrent allocation.
    // IMPORTANT: ensureCapacity must be called under external synchronization
    // if multiple threads may cause growth. For the current design, the writer
    // thread pre-grows the arena; readers never allocate.
    uint32_t alloc(size_t bytes);

    // Pre-grow arena to at least `capacity` bytes. NOT thread-safe.
    // Call from writer thread before concurrent inserts.
    void reserve(size_t capacity);

    // Swap arena contents with another arena. NOT thread-safe.
    void swapWith(NodeArena& other);

    // Access allocated memory at given offset.
    char*       get(uint32_t offset)       { return _data.data() + offset; }
    const char* get(uint32_t offset) const { return _data.data() + offset; }

    template <typename T>
    T* getAs(uint32_t offset) { return reinterpret_cast<T*>(get(offset)); }

    template <typename T>
    const T* getAs(uint32_t offset) const { return reinterpret_cast<const T*>(get(offset)); }

    size_t   size() const { return _size.load(std::memory_order_relaxed); }
    size_t   capacity() const { return _data.size(); }

    MemoryUsage getMemoryUsage(size_t dead_bytes) const;

private:
    void ensureCapacity(size_t needed);

    std::vector<char>     _data;
    std::atomic<size_t>   _size;   // Current allocation point (CAS bump pointer)
};

/**
 * Compact node header (V2 encoding, 8 bytes).
 *
 * Node layout in arena:
 *   [NodeHeader: 8B]
 *   [prefix bytes: prefix_len bytes, 4-byte aligned]
 *   [child entries: num_children * ChildEntry (5B packed)]
 *
 * V1 was 16B with 7B of padding/reserved. V2 removes all waste.
 */
struct NodeHeader {
    static constexpr uint8_t FLAG_HAS_VALUE   = 0x01;
    static constexpr uint8_t FLAG_IS_BRANCH   = 0x02;

    uint8_t  flags;
    uint8_t  num_children;    // Number of child entries (0 for leaf-only)
    uint16_t prefix_len;      // Length of prefix bytes following header
    uint32_t value;           // Stored value (valid when FLAG_HAS_VALUE set)

    bool hasValue()  const { return (flags & FLAG_HAS_VALUE) != 0; }
    bool isBranch()  const { return (flags & FLAG_IS_BRANCH) != 0; }
};

static_assert(sizeof(NodeHeader) == 8, "NodeHeader must be 8 bytes");

/**
 * Packed child entry (V2, 5 bytes). Sorted by key byte.
 * Uses memcpy for unaligned uint32_t offset access.
 */
struct ChildEntry {
    uint8_t  key;             // Branch byte
    uint8_t  offset_bytes[4]; // Little-endian uint32_t offset

    uint32_t getOffset() const {
        uint32_t v;
        std::memcpy(&v, offset_bytes, 4);
        return v;
    }
    void setOffset(uint32_t off) {
        std::memcpy(offset_bytes, &off, 4);
    }

    bool operator<(const ChildEntry& rhs) const { return key < rhs.key; }
} __attribute__((packed));

static_assert(sizeof(ChildEntry) == 5, "ChildEntry must be 5 bytes");

// Helper to construct a ChildEntry.
inline ChildEntry makeChild(uint8_t key, uint32_t offset) {
    ChildEntry e;
    e.key = key;
    e.setOffset(offset);
    return e;
}

/**
 * Concurrent Patricia Trie (CPTrie) — a Vespa-customized implementation
 * inspired by topling/CSPP.
 *
 * Design principles:
 * 1. Arena-based append-only allocation (like CSPP's memory pool)
 *    - Nodes are 32-bit offsets, not pointers
 *    - Append-only → readers never see partially written data
 *
 * 2. Copy-on-write for mutations (like CSPP's COW semantics)
 *    - Insert/remove creates new nodes along the modified path
 *    - Old nodes become dead space in the arena
 *    - No in-place modification → lock-free reads
 *
 * 3. Atomic root pointer for frozen snapshots
 *    - Writer updates root with release semantics
 *    - Readers load root with acquire semantics
 *    - Combined with GenerationHandler for safe memory reclamation
 *
 * 4. Byte-wise radix trie with path compression
 *    - Each edge labeled with a byte sequence (prefix)
 *    - Branch on first divergent byte
 *    - Natural lexicographic ordering for iteration
 *    - Operations on different branches don't interfere (CSPP's key insight)
 *
 * 5. Sorted sparse children arrays
 *    - Most nodes in text indices have few children
 *    - Binary search for lookup, linear for small arrays
 *
 * Value type is uint32_t (suitable for PostingListPtr / EntryRef).
 */
class PatriciaTrie {
public:
    using generation_t = GenerationHandler::generation_t;

    /**
     * Iterator for in-order (lexicographic) traversal of the trie.
     * Maintains a stack of (node_offset, child_index) pairs for DFS.
     */
    class Iterator {
    public:
        Iterator();

        bool   valid() const { return _valid; }
        vespalib::stringref getKey() const { return vespalib::stringref(_key.data(), _key.size()); }
        uint32_t getData() const { return _value; }
        void   operator++();

        bool operator==(const Iterator& rhs) const;
        bool operator!=(const Iterator& rhs) const { return !(*this == rhs); }

    private:
        friend class PatriciaTrie;
        Iterator(const NodeArena* arena, uint32_t root);

        struct StackEntry {
            uint32_t node_offset;
            uint8_t  child_index;   // Next child to visit
            uint8_t  num_children;
        };

        void descend(uint32_t node_offset);
        void advance();

        const NodeArena*          _arena;
        std::vector<StackEntry>   _stack;
        std::vector<char>         _key;         // Accumulated key bytes
        std::vector<uint16_t>     _key_lengths; // Key length at each stack level
        uint32_t                  _value;
        bool                      _valid;
    };

    /**
     * Frozen view for lock-free reader access.
     * Captures the root at a point in time; all traversals use that snapshot.
     */
    class FrozenView {
    public:
        FrozenView();

        bool     find(vespalib::stringref key, uint32_t& value) const;
        Iterator begin() const;
        size_t   size() const { return _num_entries; }
        bool     empty() const { return _num_entries == 0; }

    private:
        friend class PatriciaTrie;
        FrozenView(const NodeArena* arena, uint32_t root, size_t num_entries);

        const NodeArena*  _arena;
        uint32_t          _root;
        size_t            _num_entries;
    };

    PatriciaTrie();
    ~PatriciaTrie();

    // --- Writer operations ---

    /**
     * Insert key-value pair (single-writer, no contention).
     * Returns true if new key was inserted, false if key already existed.
     */
    bool insert(vespalib::stringref key, uint32_t value);

    /**
     * CAS-based insert for multi-writer concurrent access.
     *
     * Multiple threads may call casInsert() concurrently. Each insert:
     * 1. Reads the current root (acquire)
     * 2. Builds a new subtree with the key inserted (COW path copy)
     * 3. CAS-swaps the root pointer
     * 4. On CAS failure, discards work and retries with the new root
     *
     * Contention is rare when keys have different prefixes (branch isolation).
     * The arena must be pre-sized (reserve()) to avoid reallocation during
     * concurrent access, since vector resize is not thread-safe.
     *
     * Returns true if new key was inserted, false if key already existed.
     */
    bool casInsert(vespalib::stringref key, uint32_t value);

    /**
     * Remove a key. Returns true if key was found and removed.
     * Single-writer only (not CAS-safe for concurrent remove).
     */
    bool remove(vespalib::stringref key);

    /**
     * Find a key. Lock-free, safe to call concurrently with writers.
     * Uses the mutable root (may see uncommitted state from CAS writers).
     */
    bool find(vespalib::stringref key, uint32_t& value) const;

    // --- Frozen view for readers ---

    /**
     * Make current state visible to readers.
     * Sets frozen root = current root with release semantics.
     */
    void freeze();

    /**
     * Get a frozen (read-only) snapshot for concurrent readers.
     */
    FrozenView getFrozenView() const;

    // --- Lifecycle / memory management ---

    void   transferHoldLists(generation_t generation);
    void   trimHoldLists(generation_t used_generation);
    size_t size() const { return _num_entries.load(std::memory_order_relaxed); }
    MemoryUsage getMemoryUsage() const;

    /**
     * Pre-size the arena to avoid reallocation during concurrent inserts.
     * Call before starting concurrent casInsert() operations.
     * Estimate: ~300-600 bytes per entry depending on key length.
     */
    void reserveArena(size_t bytes) { _arena.reserve(bytes); }

    /**
     * Compact the arena by rebuilding the trie into a fresh arena.
     * All live nodes are copied; dead space is reclaimed.
     *
     * Single-writer only — must not be called concurrently with inserts.
     * Readers (via frozen view) are safe: they read from the old arena
     * until the next freeze() + generation cycle releases it.
     *
     * Returns the number of bytes reclaimed.
     */
    size_t compact();

    // --- Iteration (writer side) ---
    Iterator begin() const;

    // --- Foreach (ordered traversal without iterator allocation) ---
    void foreach_key(std::function<void(vespalib::stringref)> func) const;

private:
    // Internal insert into subtree rooted at `node_offset`.
    // Returns the offset of the (possibly new) subtree root.
    // If skip_mark_dead is true, dead bytes are not tracked (used by casInsert
    // which needs to defer dead accounting until CAS succeeds).
    uint32_t insertImpl(uint32_t node_offset, const char* key, size_t key_len,
                        uint32_t value, bool& inserted, bool skip_mark_dead = false);

    // Internal remove from subtree rooted at `node_offset`.
    // Returns the offset of the (possibly new) subtree root, or INVALID_REF if subtree is now empty.
    uint32_t removeImpl(uint32_t node_offset, const char* key, size_t key_len, bool& removed);

    // Find in subtree.
    bool findImpl(uint32_t node_offset, const char* key, size_t key_len, uint32_t& value) const;

    // Create a leaf node with given key suffix and value.
    uint32_t createLeaf(const char* prefix, size_t prefix_len, uint32_t value);

    // Create a branch node with given prefix, optional value, and children.
    uint32_t createBranch(const char* prefix, size_t prefix_len,
                          bool has_value, uint32_t value,
                          const ChildEntry* children, uint8_t num_children);

    // Rebuild a node with an added or replaced child.
    uint32_t withChild(uint32_t node_offset, uint8_t key_byte, uint32_t child_offset);

    // Rebuild a node with a child removed.
    uint32_t withoutChild(uint32_t node_offset, uint8_t key_byte);

    // Rebuild a node with value set or cleared.
    uint32_t withValue(uint32_t node_offset, uint32_t value);
    uint32_t withoutValue(uint32_t node_offset);

    // Helpers for reading node data from arena.
    const NodeHeader*  getHeader(uint32_t offset) const;
    const char*        getPrefix(uint32_t offset) const;
    const ChildEntry*  getChildren(uint32_t offset) const;
    size_t             prefixAlignedSize(uint16_t prefix_len) const;

    // Helper: find child by key byte in sorted children array.
    int findChild(const ChildEntry* children, uint8_t num_children, uint8_t key_byte) const;

    // Foreach traversal helper.
    void foreachImpl(uint32_t node_offset, std::vector<char>& key_buf,
                     std::function<void(vespalib::stringref)>& func) const;

    // Record dead bytes for memory tracking.
    size_t nodeSize(uint32_t node_offset) const;
    void   markDead(uint32_t node_offset);

    // Check if a node was allocated AFTER the last freeze AND in-place
    // mutation is allowed. Post-freeze nodes are only reachable from _root,
    // so single-writer can modify them in-place without COW.
    // NOT safe for CAS mode (multiple writers may race on same node).
    bool isMutable(uint32_t node_offset) const {
        return node_offset >= _freeze_arena_mark;
    }

    // In-place update of a child pointer in a mutable (post-freeze) node.
    // Only valid when isMutable(node_offset) is true and child already exists.
    void patchChildInPlace(uint32_t node_offset, uint8_t key_byte, uint32_t child_offset);

    // Deep-copy a subtree from the current arena into a target arena.
    // Returns the root offset of the copied subtree in the target arena.
    uint32_t copySubtree(uint32_t node_offset, NodeArena& target) const;

    NodeArena                _arena;
    std::atomic<uint32_t>    _root;          // Current mutable root
    std::atomic<uint32_t>    _frozen_root;   // Frozen root visible to readers
    std::atomic<size_t>      _num_entries;
    std::atomic<size_t>      _dead_bytes;     // Dead bytes from COW replacements
    size_t                   _freeze_arena_mark; // Arena size at last freeze()

    // Hold list for deferred dead-bytes accounting
    struct HoldEntry {
        generation_t generation;
        size_t       dead_bytes;
    };
    std::vector<HoldEntry>   _hold_list;
};

// Align to 4-byte boundary.
inline size_t align4(size_t n) { return (n + 3) & ~size_t(3); }

/**
 * Sharded Patricia Trie — 256 sub-tries keyed by first byte.
 *
 * Eliminates CAS contention for concurrent multi-writer inserts.
 * Keys with different first bytes are routed to independent shards,
 * each with its own root pointer and arena. CAS failures only occur
 * when two writers insert keys with the same first byte to the same shard.
 *
 * Benchmark: 4 threads, N=1M — sharded CAS is 20x faster than root CAS.
 *
 * Trade-offs:
 * - 256 independent arenas (slightly more memory overhead from per-shard minimums)
 * - Iteration requires merging 256 sorted streams (trivial: concatenate in order)
 * - Lookup adds one byte dispatch (negligible)
 */
class ShardedPatriciaTrie {
public:
    using generation_t = GenerationHandler::generation_t;

    ShardedPatriciaTrie();
    ~ShardedPatriciaTrie();

    // --- Writer operations ---

    /** Single-writer insert. */
    bool insert(vespalib::stringref key, uint32_t value);

    /** CAS-based multi-writer insert. Pre-call reserveArena(). */
    bool casInsert(vespalib::stringref key, uint32_t value);

    /** Single-writer remove. */
    bool remove(vespalib::stringref key);

    /** Lock-free find, safe with concurrent writers. */
    bool find(vespalib::stringref key, uint32_t& value) const;

    // --- Frozen view ---
    void freeze();

    // --- Lifecycle ---
    void   transferHoldLists(generation_t generation);
    void   trimHoldLists(generation_t used_generation);
    size_t size() const { return _total_entries.load(std::memory_order_relaxed); }
    MemoryUsage getMemoryUsage() const;
    void   reserveArena(size_t total_bytes);

private:
    static constexpr int NUM_SHARDS = 256;

    PatriciaTrie             _shards[NUM_SHARDS];
    std::atomic<size_t>      _total_entries;
};

} // namespace vespalib::ptrie
