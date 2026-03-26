// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "patricia_trie.h"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace vespalib::ptrie {

// ============================================================================
// NodeArena
// ============================================================================

NodeArena::NodeArena()
    : _data(INITIAL_CAPACITY, 0),
      _size(sizeof(NodeHeader))  // Reserve offset 0 as INVALID_REF sentinel
{
}

NodeArena::~NodeArena() = default;

uint32_t
NodeArena::alloc(size_t bytes)
{
    size_t aligned = align4(bytes);
    size_t old_size = _size.load(std::memory_order_relaxed);
    size_t new_size = old_size + aligned;

    // CAS loop for thread-safe bump allocation.
    while (!_size.compare_exchange_weak(old_size, new_size,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
        new_size = old_size + aligned;
    }

    // For concurrent use, arena must be pre-sized via reserve().
    // For single-writer use, we can grow here (not thread-safe).
    if (new_size > _data.size()) {
        ensureCapacity(new_size);
    }

    return static_cast<uint32_t>(old_size);
}

void
NodeArena::reserve(size_t capacity)
{
    ensureCapacity(capacity);
}

void
NodeArena::ensureCapacity(size_t needed)
{
    if (needed <= _data.size()) return;
    size_t new_cap = _data.size();
    while (new_cap < needed) {
        new_cap *= 2;
    }
    _data.resize(new_cap, 0);
}

void
NodeArena::swapWith(NodeArena& other)
{
    _data.swap(other._data);
    size_t my_size = _size.load(std::memory_order_relaxed);
    size_t other_size = other._size.load(std::memory_order_relaxed);
    _size.store(other_size, std::memory_order_relaxed);
    other._size.store(my_size, std::memory_order_relaxed);
}

MemoryUsage
NodeArena::getMemoryUsage(size_t dead_bytes) const
{
    size_t used = _size.load(std::memory_order_relaxed);
    return MemoryUsage(_data.size(), used, dead_bytes, 0);
}

// ============================================================================
// PatriciaTrie
// ============================================================================

PatriciaTrie::PatriciaTrie()
    : _arena(),
      _root(NodeArena::INVALID_REF),
      _frozen_root(NodeArena::INVALID_REF),
      _num_entries(0),
      _dead_bytes(0),
      _freeze_arena_mark(0),
      _hold_list()
{
}

PatriciaTrie::~PatriciaTrie() = default;

// --- Node layout helpers (V2 compact encoding) ---
// Layout: [8B NodeHeader] [prefix, 4-aligned] [children * 5B]

size_t
PatriciaTrie::prefixAlignedSize(uint16_t prefix_len) const
{
    return align4(prefix_len);
}

const NodeHeader*
PatriciaTrie::getHeader(uint32_t offset) const
{
    return _arena.getAs<NodeHeader>(offset);
}

const char*
PatriciaTrie::getPrefix(uint32_t offset) const
{
    return _arena.get(offset + sizeof(NodeHeader));
}

const ChildEntry*
PatriciaTrie::getChildren(uint32_t offset) const
{
    const NodeHeader* hdr = getHeader(offset);
    size_t prefix_bytes = prefixAlignedSize(hdr->prefix_len);
    return reinterpret_cast<const ChildEntry*>(_arena.get(offset + sizeof(NodeHeader) + prefix_bytes));
}

size_t
PatriciaTrie::nodeSize(uint32_t node_offset) const
{
    const NodeHeader* hdr = getHeader(node_offset);
    size_t sz = sizeof(NodeHeader);
    sz += prefixAlignedSize(hdr->prefix_len);
    sz += hdr->num_children * sizeof(ChildEntry);
    return sz;
}

void
PatriciaTrie::markDead(uint32_t node_offset)
{
    if (node_offset != NodeArena::INVALID_REF) {
        _dead_bytes.fetch_add(nodeSize(node_offset), std::memory_order_relaxed);
    }
}

int
PatriciaTrie::findChild(const ChildEntry* children, uint8_t num_children, uint8_t key_byte) const
{
    // Linear search for small arrays, binary for larger.
    if (num_children <= 8) {
        for (int i = 0; i < num_children; ++i) {
            if (children[i].key == key_byte) return i;
            if (children[i].key > key_byte) return -1;
        }
        return -1;
    }
    // Binary search on sorted children.
    int lo = 0, hi = num_children - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (children[mid].key == key_byte) return mid;
        if (children[mid].key < key_byte) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// --- Node creation helpers (allocate in arena) ---

uint32_t
PatriciaTrie::createLeaf(const char* prefix, size_t prefix_len, uint32_t value)
{
    size_t total = sizeof(NodeHeader) + align4(prefix_len);
    uint32_t offset = _arena.alloc(total);

    NodeHeader* hdr = _arena.getAs<NodeHeader>(offset);
    hdr->flags = NodeHeader::FLAG_HAS_VALUE;  // Leaf always has value
    hdr->num_children = 0;
    hdr->prefix_len = static_cast<uint16_t>(prefix_len);
    hdr->value = value;

    if (prefix_len > 0) {
        char* dst = _arena.get(offset + sizeof(NodeHeader));
        std::memcpy(dst, prefix, prefix_len);
    }
    return offset;
}

uint32_t
PatriciaTrie::createBranch(const char* prefix, size_t prefix_len,
                                  bool has_value, uint32_t value,
                                  const ChildEntry* children, uint8_t num_children)
{
    size_t prefix_aligned = align4(prefix_len);
    size_t total = sizeof(NodeHeader) + prefix_aligned + num_children * sizeof(ChildEntry);
    uint32_t offset = _arena.alloc(total);

    NodeHeader* hdr = _arena.getAs<NodeHeader>(offset);
    hdr->flags = NodeHeader::FLAG_IS_BRANCH;
    if (has_value) hdr->flags |= NodeHeader::FLAG_HAS_VALUE;
    hdr->num_children = num_children;
    hdr->prefix_len = static_cast<uint16_t>(prefix_len);
    hdr->value = value;

    if (prefix_len > 0) {
        char* dst = _arena.get(offset + sizeof(NodeHeader));
        std::memcpy(dst, prefix, prefix_len);
    }

    if (num_children > 0) {
        char* dst = _arena.get(offset + sizeof(NodeHeader) + prefix_aligned);
        std::memcpy(dst, children, num_children * sizeof(ChildEntry));
    }
    return offset;
}

// --- Node snapshot ---
// CRITICAL: Arena pointers are invalidated by alloc() (vector resize).
// All COW functions must snapshot node data BEFORE any arena allocation.

static constexpr size_t SNAP_PREFIX_SBO = 64;
static constexpr size_t SNAP_CHILDREN_SBO = 32;

struct NodeSnapshot {
    NodeHeader header;

    char prefix_sbo[SNAP_PREFIX_SBO];
    std::vector<char> prefix_heap;
    const char* prefix_data() const {
        return (header.prefix_len <= SNAP_PREFIX_SBO) ? prefix_sbo : prefix_heap.data();
    }

    ChildEntry children_sbo[SNAP_CHILDREN_SBO];
    std::vector<ChildEntry> children_heap;
    const ChildEntry* children_data() const {
        return (header.num_children <= SNAP_CHILDREN_SBO) ? children_sbo : children_heap.data();
    }
};

static NodeSnapshot
snapshotNode(const NodeArena& arena, uint32_t offset)
{
    NodeSnapshot snap;
    snap.header = *arena.getAs<NodeHeader>(offset);

    uint16_t plen = snap.header.prefix_len;
    if (plen > 0) {
        const char* pfx = arena.get(offset + sizeof(NodeHeader));
        if (plen <= SNAP_PREFIX_SBO) {
            std::memcpy(snap.prefix_sbo, pfx, plen);
        } else {
            snap.prefix_heap.assign(pfx, pfx + plen);
        }
    }

    uint8_t nc = snap.header.num_children;
    if (nc > 0) {
        size_t pfx_aligned = align4(plen);
        const ChildEntry* ch = reinterpret_cast<const ChildEntry*>(
            arena.get(offset + sizeof(NodeHeader) + pfx_aligned));
        if (nc <= SNAP_CHILDREN_SBO) {
            std::memcpy(snap.children_sbo, ch, nc * sizeof(ChildEntry));
        } else {
            snap.children_heap.assign(ch, ch + nc);
        }
    }
    return snap;
}

// --- COW node rebuilders ---

static constexpr size_t CHILD_SBO_SIZE = 256;

void
PatriciaTrie::patchChildInPlace(uint32_t node_offset, uint8_t key_byte, uint32_t child_offset)
{
    const NodeHeader* hdr = getHeader(node_offset);
    size_t prefix_bytes = prefixAlignedSize(hdr->prefix_len);
    ChildEntry* children = reinterpret_cast<ChildEntry*>(
        _arena.get(node_offset + sizeof(NodeHeader) + prefix_bytes));
    int idx = findChild(children, hdr->num_children, key_byte);
    assert(idx >= 0);
    children[idx].setOffset(child_offset);
}

uint32_t
PatriciaTrie::withChild(uint32_t node_offset, uint8_t key_byte, uint32_t child_offset)
{
    const NodeHeader* old_hdr = getHeader(node_offset);
    uint8_t nc = old_hdr->num_children;

    const ChildEntry* old_children = getChildren(node_offset);
    int replace_idx = findChild(old_children, nc, key_byte);

    if (replace_idx >= 0) {
        // Replace existing child: clone entire node, patch one offset.
        size_t total = nodeSize(node_offset);
        uint16_t plen = old_hdr->prefix_len;

        char node_buf[2048];
        std::vector<char> node_heap;
        char* node_copy;
        if (total <= sizeof(node_buf)) {
            node_copy = node_buf;
        } else {
            node_heap.resize(total);
            node_copy = node_heap.data();
        }
        std::memcpy(node_copy, _arena.get(node_offset), total);

        uint32_t new_offset = _arena.alloc(total);
        std::memcpy(_arena.get(new_offset), node_copy, total);

        size_t prefix_bytes = prefixAlignedSize(plen);
        ChildEntry* new_children = reinterpret_cast<ChildEntry*>(
            _arena.get(new_offset + sizeof(NodeHeader) + prefix_bytes));
        new_children[replace_idx].setOffset(child_offset);
        return new_offset;
    }

    // Slow path: adding a new child (count increases).
    auto snap = snapshotNode(_arena, node_offset);
    const ChildEntry* src = snap.children_data();

    ChildEntry buf[CHILD_SBO_SIZE];
    uint8_t out = 0;
    bool inserted = false;

    for (uint8_t i = 0; i < nc; ++i) {
        if (!inserted && src[i].key > key_byte) {
            buf[out++] = makeChild(key_byte, child_offset);
            inserted = true;
        }
        buf[out++] = src[i];
    }
    if (!inserted) {
        buf[out++] = makeChild(key_byte, child_offset);
    }

    return createBranch(snap.prefix_data(), snap.header.prefix_len,
                        snap.header.hasValue(), snap.header.value,
                        buf, out);
}

uint32_t
PatriciaTrie::withoutChild(uint32_t node_offset, uint8_t key_byte)
{
    auto snap = snapshotNode(_arena, node_offset);

    const ChildEntry* src = snap.children_data();
    uint8_t nc = snap.header.num_children;
    ChildEntry buf[CHILD_SBO_SIZE];
    uint8_t new_count = 0;
    for (uint8_t i = 0; i < nc; ++i) {
        if (src[i].key != key_byte) {
            buf[new_count++] = src[i];
        }
    }
    if (new_count == 0 && !snap.header.hasValue()) {
        return NodeArena::INVALID_REF;
    }

    // If only one child remains and no value, merge with child (path compression).
    if (new_count == 1 && !snap.header.hasValue()) {
        uint32_t child_off = buf[0].getOffset();
        auto child_snap = snapshotNode(_arena, child_off);

        size_t merged_len = snap.header.prefix_len + 1 + child_snap.header.prefix_len;
        char merged_sbo[128];
        std::vector<char> merged_heap;
        char* merged;
        if (merged_len <= sizeof(merged_sbo)) {
            merged = merged_sbo;
        } else {
            merged_heap.resize(merged_len);
            merged = merged_heap.data();
        }

        if (snap.header.prefix_len > 0) {
            std::memcpy(merged, snap.prefix_data(), snap.header.prefix_len);
        }
        merged[snap.header.prefix_len] = static_cast<char>(buf[0].key);
        if (child_snap.header.prefix_len > 0) {
            std::memcpy(merged + snap.header.prefix_len + 1,
                        child_snap.prefix_data(), child_snap.header.prefix_len);
        }

        markDead(child_off);

        if (child_snap.header.isBranch()) {
            return createBranch(merged, merged_len,
                                child_snap.header.hasValue(), child_snap.header.value,
                                child_snap.children_data(), child_snap.header.num_children);
        } else {
            return createLeaf(merged, merged_len, child_snap.header.value);
        }
    }

    if (new_count == 0) {
        return createLeaf(snap.prefix_data(), snap.header.prefix_len, snap.header.value);
    }

    return createBranch(snap.prefix_data(), snap.header.prefix_len,
                        snap.header.hasValue(), snap.header.value,
                        buf, new_count);
}

uint32_t
PatriciaTrie::withValue(uint32_t node_offset, uint32_t value)
{
    auto snap = snapshotNode(_arena, node_offset);

    if (snap.header.isBranch()) {
        return createBranch(snap.prefix_data(), snap.header.prefix_len, true, value,
                            snap.children_data(), snap.header.num_children);
    } else {
        return createLeaf(snap.prefix_data(), snap.header.prefix_len, value);
    }
}

uint32_t
PatriciaTrie::withoutValue(uint32_t node_offset)
{
    auto snap = snapshotNode(_arena, node_offset);

    if (snap.header.num_children == 0) {
        return NodeArena::INVALID_REF;
    }

    // If only one child remains, merge (path compression).
    if (snap.header.num_children == 1) {
        uint32_t child_off = snap.children_data()[0].getOffset();
        auto child_snap = snapshotNode(_arena, child_off);

        size_t merged_len = snap.header.prefix_len + 1 + child_snap.header.prefix_len;
        char merged_sbo[128];
        std::vector<char> merged_heap;
        char* merged;
        if (merged_len <= sizeof(merged_sbo)) {
            merged = merged_sbo;
        } else {
            merged_heap.resize(merged_len);
            merged = merged_heap.data();
        }

        if (snap.header.prefix_len > 0) {
            std::memcpy(merged, snap.prefix_data(), snap.header.prefix_len);
        }
        merged[snap.header.prefix_len] = static_cast<char>(snap.children_data()[0].key);
        if (child_snap.header.prefix_len > 0) {
            std::memcpy(merged + snap.header.prefix_len + 1,
                        child_snap.prefix_data(), child_snap.header.prefix_len);
        }

        markDead(child_off);

        if (child_snap.header.isBranch()) {
            return createBranch(merged, merged_len,
                                child_snap.header.hasValue(), child_snap.header.value,
                                child_snap.children_data(), child_snap.header.num_children);
        } else {
            return createLeaf(merged, merged_len, child_snap.header.value);
        }
    }

    return createBranch(snap.prefix_data(), snap.header.prefix_len, false, 0,
                        snap.children_data(), snap.header.num_children);
}

// --- Core insert (iterative COW) ---

static constexpr int MAX_INSERT_DEPTH = 128;

struct InsertPathEntry {
    uint32_t node_offset;
    uint8_t  key_byte;
};

uint32_t
PatriciaTrie::insertImpl(uint32_t node_offset, const char* key, size_t key_len,
                                uint32_t value, bool& inserted, bool skip_mark_dead)
{
    if (node_offset == NodeArena::INVALID_REF) {
        inserted = true;
        return createLeaf(key, key_len, value);
    }

    InsertPathEntry path[MAX_INSERT_DEPTH];
    int path_depth = 0;

    uint32_t cur_offset = node_offset;
    const char* k = key;
    size_t k_len = key_len;

    for (;;) {
        auto snap = snapshotNode(_arena, cur_offset);
        uint16_t prefix_len = snap.header.prefix_len;

        size_t common = 0;
        size_t max_common = std::min(static_cast<size_t>(prefix_len), k_len);
        while (common < max_common && k[common] == snap.prefix_data()[common]) {
            ++common;
        }

        if (common < prefix_len) {
            // Partial prefix match → split this node.
            uint32_t shortened;
            if (snap.header.isBranch()) {
                shortened = createBranch(snap.prefix_data() + common + 1,
                                         prefix_len - common - 1,
                                         snap.header.hasValue(), snap.header.value,
                                         snap.children_data(), snap.header.num_children);
            } else {
                shortened = createLeaf(snap.prefix_data() + common + 1,
                                       prefix_len - common - 1,
                                       snap.header.value);
            }

            uint32_t new_subtree;
            if (common == k_len) {
                ChildEntry child = makeChild(
                    static_cast<uint8_t>(snap.prefix_data()[common]), shortened);
                new_subtree = createBranch(k, common, true, value, &child, 1);
            } else {
                ChildEntry children[2];
                uint32_t new_leaf = createLeaf(k + common + 1,
                                                k_len - common - 1, value);
                uint8_t old_byte = static_cast<uint8_t>(snap.prefix_data()[common]);
                uint8_t new_byte = static_cast<uint8_t>(k[common]);
                int first = (old_byte < new_byte) ? 0 : 1;
                children[first] = makeChild(old_byte, shortened);
                children[1 - first] = makeChild(new_byte, new_leaf);
                new_subtree = createBranch(k, common, false, 0, children, 2);
            }

            inserted = true;
            if (!skip_mark_dead && !isMutable(cur_offset)) markDead(cur_offset);

            uint32_t result = new_subtree;
            for (int i = path_depth - 1; i >= 0; --i) {
                uint32_t old_off = path[i].node_offset;
                if (!skip_mark_dead && isMutable(old_off)) {
                    patchChildInPlace(old_off, path[i].key_byte, result);
                    return node_offset;
                }
                if (!skip_mark_dead) markDead(old_off);
                result = withChild(old_off, path[i].key_byte, result);
            }
            return result;
        }

        // Full prefix matched.
        if (k_len == prefix_len) {
            inserted = !snap.header.hasValue();
            if (!skip_mark_dead && isMutable(cur_offset)) {
                NodeHeader* mut_hdr = _arena.getAs<NodeHeader>(cur_offset);
                mut_hdr->flags |= NodeHeader::FLAG_HAS_VALUE;
                mut_hdr->value = value;
                return node_offset;
            }
            if (!skip_mark_dead) markDead(cur_offset);
            uint32_t result = withValue(cur_offset, value);

            for (int i = path_depth - 1; i >= 0; --i) {
                uint32_t old_off = path[i].node_offset;
                if (!skip_mark_dead && isMutable(old_off)) {
                    patchChildInPlace(old_off, path[i].key_byte, result);
                    return node_offset;
                }
                if (!skip_mark_dead) markDead(old_off);
                result = withChild(old_off, path[i].key_byte, result);
            }
            return result;
        }

        // key_len > prefix_len: descend into child.
        uint8_t next_byte = static_cast<uint8_t>(k[prefix_len]);
        int child_idx = findChild(snap.children_data(), snap.header.num_children, next_byte);

        if (child_idx < 0) {
            uint32_t new_leaf = createLeaf(k + prefix_len + 1,
                                            k_len - prefix_len - 1, value);
            inserted = true;
            if (!skip_mark_dead && isMutable(cur_offset)) {
                uint32_t result = withChild(cur_offset, next_byte, new_leaf);
                for (int i = path_depth - 1; i >= 0; --i) {
                    uint32_t old_off = path[i].node_offset;
                    if (isMutable(old_off)) {
                        patchChildInPlace(old_off, path[i].key_byte, result);
                        return node_offset;
                    }
                    markDead(old_off);
                    result = withChild(old_off, path[i].key_byte, result);
                }
                return result;
            }
            if (!skip_mark_dead) markDead(cur_offset);
            uint32_t result = withChild(cur_offset, next_byte, new_leaf);

            for (int i = path_depth - 1; i >= 0; --i) {
                uint32_t old_off = path[i].node_offset;
                if (!skip_mark_dead && isMutable(old_off)) {
                    patchChildInPlace(old_off, path[i].key_byte, result);
                    return node_offset;
                }
                if (!skip_mark_dead) markDead(old_off);
                result = withChild(old_off, path[i].key_byte, result);
            }
            return result;
        }

        // Record this node on the path and descend.
        assert(path_depth < MAX_INSERT_DEPTH);
        path[path_depth++] = {cur_offset, next_byte};

        cur_offset = snap.children_data()[child_idx].getOffset();
        k += prefix_len + 1;
        k_len -= prefix_len + 1;

        if (cur_offset == NodeArena::INVALID_REF) {
            inserted = true;
            uint32_t result = createLeaf(k, k_len, value);

            for (int i = path_depth - 1; i >= 0; --i) {
                uint32_t old_off = path[i].node_offset;
                if (!skip_mark_dead && isMutable(old_off)) {
                    patchChildInPlace(old_off, path[i].key_byte, result);
                    return node_offset;
                }
                if (!skip_mark_dead) markDead(old_off);
                result = withChild(old_off, path[i].key_byte, result);
            }
            return result;
        }
    }
}

bool
PatriciaTrie::insert(vespalib::stringref key, uint32_t value)
{
    bool inserted = false;
    uint32_t old_root = _root.load(std::memory_order_relaxed);
    uint32_t new_root = insertImpl(old_root, key.data(), key.size(), value, inserted);
    _root.store(new_root, std::memory_order_relaxed);
    if (inserted) {
        _num_entries.fetch_add(1, std::memory_order_relaxed);
    }
    return inserted;
}

bool
PatriciaTrie::casInsert(vespalib::stringref key, uint32_t value)
{
    for (;;) {
        uint32_t old_root = _root.load(std::memory_order_acquire);
        size_t arena_before = _arena.size();

        bool inserted = false;
        uint32_t new_root = insertImpl(old_root, key.data(), key.size(), value, inserted,
                                       /*skip_mark_dead=*/true);

        if (new_root == old_root) {
            return false;
        }

        if (_root.compare_exchange_strong(old_root, new_root,
                                          std::memory_order_release,
                                          std::memory_order_acquire)) {
            if (inserted) {
                _num_entries.fetch_add(1, std::memory_order_relaxed);
            }
            return inserted;
        }

        size_t wasted = _arena.size() - arena_before;
        if (wasted > 0) {
            _dead_bytes.fetch_add(wasted, std::memory_order_relaxed);
        }
    }
}

// --- Core remove (COW recursive) ---

uint32_t
PatriciaTrie::removeImpl(uint32_t node_offset, const char* key, size_t key_len, bool& removed)
{
    if (node_offset == NodeArena::INVALID_REF) {
        removed = false;
        return NodeArena::INVALID_REF;
    }

    auto snap = snapshotNode(_arena, node_offset);
    uint16_t prefix_len = snap.header.prefix_len;

    if (key_len < prefix_len) {
        removed = false;
        return node_offset;
    }
    if (std::memcmp(key, snap.prefix_data(), prefix_len) != 0) {
        removed = false;
        return node_offset;
    }

    if (key_len == prefix_len) {
        if (!snap.header.hasValue()) {
            removed = false;
            return node_offset;
        }
        removed = true;
        markDead(node_offset);
        return withoutValue(node_offset);
    }

    uint8_t next_byte = static_cast<uint8_t>(key[prefix_len]);
    int child_idx = findChild(snap.children_data(), snap.header.num_children, next_byte);

    if (child_idx < 0) {
        removed = false;
        return node_offset;
    }

    uint32_t old_child = snap.children_data()[child_idx].getOffset();
    uint32_t new_child = removeImpl(old_child, key + prefix_len + 1,
                                    key_len - prefix_len - 1, removed);
    if (!removed) {
        return node_offset;
    }

    markDead(node_offset);
    if (new_child == NodeArena::INVALID_REF) {
        return withoutChild(node_offset, next_byte);
    } else {
        return withChild(node_offset, next_byte, new_child);
    }
}

bool
PatriciaTrie::remove(vespalib::stringref key)
{
    bool removed = false;
    uint32_t old_root = _root.load(std::memory_order_relaxed);
    uint32_t new_root = removeImpl(old_root, key.data(), key.size(), removed);
    _root.store(new_root, std::memory_order_relaxed);
    if (removed) {
        _num_entries.fetch_sub(1, std::memory_order_relaxed);
    }
    return removed;
}

// --- Find ---

bool
PatriciaTrie::findImpl(uint32_t node_offset, const char* key, size_t key_len, uint32_t& value) const
{
    uint32_t offset = node_offset;
    const char* k = key;
    size_t k_len = key_len;

    while (offset != NodeArena::INVALID_REF) {
        const NodeHeader* hdr = getHeader(offset);
        uint16_t prefix_len = hdr->prefix_len;

        if (k_len < prefix_len) return false;
        if (prefix_len > 0) {
            const char* prefix = getPrefix(offset);
            if (std::memcmp(k, prefix, prefix_len) != 0) return false;
        }

        if (k_len == prefix_len) {
            if (hdr->hasValue()) {
                value = hdr->value;
                return true;
            }
            return false;
        }

        uint8_t next_byte = static_cast<uint8_t>(k[prefix_len]);
        const ChildEntry* children = getChildren(offset);
        int child_idx = findChild(children, hdr->num_children, next_byte);
        if (child_idx < 0) return false;

        offset = children[child_idx].getOffset();
        k += prefix_len + 1;
        k_len -= prefix_len + 1;
    }
    return false;
}

bool
PatriciaTrie::find(vespalib::stringref key, uint32_t& value) const
{
    return findImpl(_root.load(std::memory_order_relaxed), key.data(), key.size(), value);
}

// --- Freeze / generation management ---

void
PatriciaTrie::freeze()
{
    uint32_t root = _root.load(std::memory_order_relaxed);
    _frozen_root.store(root, std::memory_order_release);
    _freeze_arena_mark = _arena.size();
}

PatriciaTrie::FrozenView
PatriciaTrie::getFrozenView() const
{
    uint32_t root = _frozen_root.load(std::memory_order_acquire);
    return FrozenView(&_arena, root, _num_entries.load(std::memory_order_relaxed));
}

void
PatriciaTrie::transferHoldLists(generation_t generation)
{
    size_t dead = _dead_bytes.exchange(0, std::memory_order_relaxed);
    if (dead > 0) {
        _hold_list.push_back({generation, dead});
    }
}

void
PatriciaTrie::trimHoldLists(generation_t used_generation)
{
    while (!_hold_list.empty() && _hold_list.front().generation < used_generation) {
        _hold_list.erase(_hold_list.begin());
    }
}

MemoryUsage
PatriciaTrie::getMemoryUsage() const
{
    size_t total_dead = _dead_bytes.load(std::memory_order_relaxed);
    for (const auto& entry : _hold_list) {
        total_dead += entry.dead_bytes;
    }
    return _arena.getMemoryUsage(total_dead);
}

// --- Compaction ---

uint32_t
PatriciaTrie::copySubtree(uint32_t node_offset, NodeArena& target) const
{
    if (node_offset == NodeArena::INVALID_REF) {
        return NodeArena::INVALID_REF;
    }

    const NodeHeader* hdr = getHeader(node_offset);
    uint16_t prefix_len = hdr->prefix_len;
    uint8_t num_children = hdr->num_children;

    // Recursively copy children to get new offsets.
    ChildEntry new_children[256];
    if (num_children > 0) {
        const ChildEntry* old_children = getChildren(node_offset);
        for (uint8_t i = 0; i < num_children; ++i) {
            new_children[i].key = old_children[i].key;
            new_children[i].setOffset(copySubtree(old_children[i].getOffset(), target));
        }
    }

    // Allocate node in target arena.
    size_t prefix_aligned = align4(prefix_len);
    size_t total = sizeof(NodeHeader) + prefix_aligned + num_children * sizeof(ChildEntry);
    uint32_t new_offset = target.alloc(total);

    // Copy header.
    NodeHeader* new_hdr = target.getAs<NodeHeader>(new_offset);
    *new_hdr = *hdr;

    // Copy prefix.
    if (prefix_len > 0) {
        const char* old_prefix = getPrefix(node_offset);
        std::memcpy(target.get(new_offset + sizeof(NodeHeader)), old_prefix, prefix_len);
    }

    // Copy children with updated offsets.
    if (num_children > 0) {
        char* dst = target.get(new_offset + sizeof(NodeHeader) + prefix_aligned);
        std::memcpy(dst, new_children, num_children * sizeof(ChildEntry));
    }

    return new_offset;
}

size_t
PatriciaTrie::compact()
{
    uint32_t old_root = _root.load(std::memory_order_relaxed);
    if (old_root == NodeArena::INVALID_REF) {
        return 0;
    }

    size_t old_arena_used = _arena.size();
    size_t old_dead = _dead_bytes.load(std::memory_order_relaxed);
    for (const auto& entry : _hold_list) {
        old_dead += entry.dead_bytes;
    }

    if (old_dead < old_arena_used / 4) {
        return 0;
    }

    NodeArena new_arena;
    size_t estimated_live = old_arena_used - old_dead;
    new_arena.reserve(estimated_live + estimated_live / 4);

    uint32_t new_root = copySubtree(old_root, new_arena);

    _arena.swapWith(new_arena);
    _root.store(new_root, std::memory_order_relaxed);
    _dead_bytes.store(0, std::memory_order_relaxed);
    _hold_list.clear();
    _freeze_arena_mark = 0;

    _frozen_root.store(NodeArena::INVALID_REF, std::memory_order_relaxed);

    size_t new_arena_used = _arena.size();
    return old_arena_used - new_arena_used;
}

// --- Iteration ---

PatriciaTrie::Iterator
PatriciaTrie::begin() const
{
    return Iterator(&_arena, _root.load(std::memory_order_relaxed));
}

// --- FrozenView ---

PatriciaTrie::FrozenView::FrozenView()
    : _arena(nullptr), _root(NodeArena::INVALID_REF), _num_entries(0)
{
}

PatriciaTrie::FrozenView::FrozenView(const NodeArena* arena, uint32_t root, size_t num_entries)
    : _arena(arena), _root(root), _num_entries(num_entries)
{
}

bool
PatriciaTrie::FrozenView::find(vespalib::stringref key, uint32_t& value) const
{
    if (_arena == nullptr || _root == NodeArena::INVALID_REF) return false;

    uint32_t offset = _root;
    const char* k = key.data();
    size_t k_len = key.size();

    while (offset != NodeArena::INVALID_REF) {
        const NodeHeader* hdr = _arena->getAs<NodeHeader>(offset);
        const char* prefix = _arena->get(offset + sizeof(NodeHeader));
        uint16_t prefix_len = hdr->prefix_len;

        if (k_len < prefix_len) return false;
        if (prefix_len > 0 && std::memcmp(k, prefix, prefix_len) != 0) return false;

        if (k_len == prefix_len) {
            if (hdr->hasValue()) {
                value = hdr->value;
                return true;
            }
            return false;
        }

        uint8_t next_byte = static_cast<uint8_t>(k[prefix_len]);
        size_t prefix_aligned = align4(prefix_len);
        const ChildEntry* children = reinterpret_cast<const ChildEntry*>(
            _arena->get(offset + sizeof(NodeHeader) + prefix_aligned));

        int found = -1;
        uint8_t nc = hdr->num_children;
        if (nc <= 8) {
            for (int i = 0; i < nc; ++i) {
                if (children[i].key == next_byte) { found = i; break; }
                if (children[i].key > next_byte) break;
            }
        } else {
            int lo = 0, hi = nc - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (children[mid].key == next_byte) { found = mid; break; }
                if (children[mid].key < next_byte) lo = mid + 1;
                else hi = mid - 1;
            }
        }

        if (found < 0) return false;

        offset = children[found].getOffset();
        k += prefix_len + 1;
        k_len -= prefix_len + 1;
    }
    return false;
}

PatriciaTrie::Iterator
PatriciaTrie::FrozenView::begin() const
{
    return Iterator(_arena, _root);
}

// --- Iterator ---

PatriciaTrie::Iterator::Iterator()
    : _arena(nullptr), _stack(), _key(), _key_lengths(), _value(0), _valid(false)
{
}

PatriciaTrie::Iterator::Iterator(const NodeArena* arena, uint32_t root)
    : _arena(arena), _stack(), _key(), _key_lengths(), _value(0), _valid(false)
{
    if (arena != nullptr && root != NodeArena::INVALID_REF) {
        descend(root);
    }
}

void
PatriciaTrie::Iterator::descend(uint32_t node_offset)
{
    while (node_offset != NodeArena::INVALID_REF) {
        const NodeHeader* hdr = _arena->getAs<NodeHeader>(node_offset);
        const char* prefix = _arena->get(node_offset + sizeof(NodeHeader));

        _key_lengths.push_back(static_cast<uint16_t>(_key.size()));

        for (uint16_t i = 0; i < hdr->prefix_len; ++i) {
            _key.push_back(prefix[i]);
        }

        StackEntry entry;
        entry.node_offset = node_offset;
        entry.child_index = 0;
        entry.num_children = hdr->num_children;
        _stack.push_back(entry);

        if (hdr->hasValue()) {
            _value = hdr->value;
            _valid = true;
            return;
        }

        if (hdr->num_children == 0) {
            _valid = false;
            return;
        }

        size_t prefix_aligned = align4(hdr->prefix_len);
        const ChildEntry* children = reinterpret_cast<const ChildEntry*>(
            _arena->get(node_offset + sizeof(NodeHeader) + prefix_aligned));

        _key.push_back(static_cast<char>(children[0].key));
        _stack.back().child_index = 1;
        node_offset = children[0].getOffset();
    }
    _valid = false;
}

void
PatriciaTrie::Iterator::advance()
{
    if (!_stack.empty()) {
        StackEntry& top = _stack.back();
        const NodeHeader* hdr = _arena->getAs<NodeHeader>(top.node_offset);

        if (top.child_index < top.num_children) {
            size_t prefix_aligned = align4(hdr->prefix_len);
            const ChildEntry* children = reinterpret_cast<const ChildEntry*>(
                _arena->get(top.node_offset + sizeof(NodeHeader) + prefix_aligned));

            uint8_t ci = top.child_index;
            top.child_index++;

            _key.push_back(static_cast<char>(children[ci].key));
            descend(children[ci].getOffset());
            return;
        }
    }

    while (!_stack.empty()) {
        _key.resize(_key_lengths.back());
        _key_lengths.pop_back();
        _stack.pop_back();

        if (_stack.empty()) break;

        StackEntry& parent = _stack.back();
        const NodeHeader* phdr = _arena->getAs<NodeHeader>(parent.node_offset);

        _key.resize(_key_lengths.back() + phdr->prefix_len);

        if (parent.child_index < parent.num_children) {
            size_t prefix_aligned = align4(phdr->prefix_len);
            const ChildEntry* children = reinterpret_cast<const ChildEntry*>(
                _arena->get(parent.node_offset + sizeof(NodeHeader) + prefix_aligned));

            uint8_t ci = parent.child_index;
            parent.child_index++;

            _key.push_back(static_cast<char>(children[ci].key));
            descend(children[ci].getOffset());
            return;
        }
    }

    _valid = false;
}

void
PatriciaTrie::Iterator::operator++()
{
    if (!_valid) return;
    advance();
}

bool
PatriciaTrie::Iterator::operator==(const Iterator& rhs) const
{
    if (!_valid && !rhs._valid) return true;
    if (_valid != rhs._valid) return false;
    return _key == rhs._key;
}

// --- Foreach ---

void
PatriciaTrie::foreach_key(std::function<void(vespalib::stringref)> func) const
{
    uint32_t root = _root.load(std::memory_order_relaxed);
    if (root == NodeArena::INVALID_REF) return;
    std::vector<char> key_buf;
    foreachImpl(root, key_buf, func);
}

void
PatriciaTrie::foreachImpl(uint32_t node_offset, std::vector<char>& key_buf,
                                 std::function<void(vespalib::stringref)>& func) const
{
    const NodeHeader* hdr = getHeader(node_offset);
    const char* prefix = getPrefix(node_offset);

    size_t old_len = key_buf.size();
    for (uint16_t i = 0; i < hdr->prefix_len; ++i) {
        key_buf.push_back(prefix[i]);
    }

    if (hdr->hasValue()) {
        func(vespalib::stringref(key_buf.data(), key_buf.size()));
    }

    if (hdr->isBranch()) {
        const ChildEntry* children = getChildren(node_offset);
        for (uint8_t i = 0; i < hdr->num_children; ++i) {
            key_buf.push_back(static_cast<char>(children[i].key));
            foreachImpl(children[i].getOffset(), key_buf, func);
            key_buf.pop_back();
        }
    }

    key_buf.resize(old_len);
}

// ============================================================================
// ShardedPatriciaTrie
// ============================================================================

ShardedPatriciaTrie::ShardedPatriciaTrie()
    : _shards(),
      _total_entries(0)
{
}

ShardedPatriciaTrie::~ShardedPatriciaTrie() = default;

bool
ShardedPatriciaTrie::insert(vespalib::stringref key, uint32_t value)
{
    if (key.empty()) return false;
    uint8_t shard = static_cast<uint8_t>(key[0]);
    bool result = _shards[shard].insert(vespalib::stringref(key.data() + 1, key.size() - 1), value);
    if (result) {
        _total_entries.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

bool
ShardedPatriciaTrie::casInsert(vespalib::stringref key, uint32_t value)
{
    if (key.empty()) return false;
    uint8_t shard = static_cast<uint8_t>(key[0]);
    bool result = _shards[shard].casInsert(vespalib::stringref(key.data() + 1, key.size() - 1), value);
    if (result) {
        _total_entries.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

bool
ShardedPatriciaTrie::remove(vespalib::stringref key)
{
    if (key.empty()) return false;
    uint8_t shard = static_cast<uint8_t>(key[0]);
    bool result = _shards[shard].remove(vespalib::stringref(key.data() + 1, key.size() - 1));
    if (result) {
        _total_entries.fetch_sub(1, std::memory_order_relaxed);
    }
    return result;
}

bool
ShardedPatriciaTrie::find(vespalib::stringref key, uint32_t& value) const
{
    if (key.empty()) return false;
    uint8_t shard = static_cast<uint8_t>(key[0]);
    return _shards[shard].find(vespalib::stringref(key.data() + 1, key.size() - 1), value);
}

void
ShardedPatriciaTrie::freeze()
{
    for (int i = 0; i < NUM_SHARDS; ++i) {
        _shards[i].freeze();
    }
}

void
ShardedPatriciaTrie::transferHoldLists(generation_t generation)
{
    for (int i = 0; i < NUM_SHARDS; ++i) {
        _shards[i].transferHoldLists(generation);
    }
}

void
ShardedPatriciaTrie::trimHoldLists(generation_t used_generation)
{
    for (int i = 0; i < NUM_SHARDS; ++i) {
        _shards[i].trimHoldLists(used_generation);
    }
}

MemoryUsage
ShardedPatriciaTrie::getMemoryUsage() const
{
    MemoryUsage total;
    for (int i = 0; i < NUM_SHARDS; ++i) {
        total.merge(_shards[i].getMemoryUsage());
    }
    return total;
}

void
ShardedPatriciaTrie::reserveArena(size_t total_bytes)
{
    size_t per_shard = total_bytes / NUM_SHARDS + 4096;
    for (int i = 0; i < NUM_SHARDS; ++i) {
        _shards[i].reserveArena(per_shard);
    }
}

} // namespace vespalib::ptrie
