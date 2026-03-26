// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include <vespa/vespalib/datastore/datastore.h>
#include <vespa/vespalib/util/memoryusage.h>
#include <vespa/vespalib/util/generationhandler.h>
#include <cstdint>

namespace vespalib::ptrie {

/**
 * DataStore-backed node storage for Patricia Trie (Direction 2).
 *
 * Replaces the append-only NodeArena with Vespa's DataStore using the
 * WordStore pattern (rawAllocator<char> for variable-length allocation).
 *
 * Key advantages over NodeArena:
 * - Incremental memory reclamation via holdElem/trimElemHoldList
 *   (no full compaction needed)
 * - Generation-based deferred freeing integrates with GenerationHandler
 * - Free list reuse for same-size allocations
 * - Proper dead/hold byte tracking
 *
 * Uses AlignedEntryRefT<22,2> like WordStore:
 * - 22 offset bits → 4M entries per buffer (at 4-byte alignment)
 * - 2 alignment bits → 4-byte aligned entries
 * - 10 buffer bits → up to 1024 buffers
 * - Total addressable: 1024 * 4M * 4B = 16 GB
 *
 * EntryRef is 32 bits, same size as the arena offset it replaces.
 */
class PTrieNodeStore {
public:
    using DataStoreType = datastore::DataStoreT<datastore::AlignedEntryRefT<22, 2>>;
    using RefType = DataStoreType::RefType;
    using generation_t = GenerationHandler::generation_t;

    static constexpr uint32_t INVALID_REF = 0;

    PTrieNodeStore();
    ~PTrieNodeStore();

    /**
     * Allocate variable-length node data. Returns an EntryRef encoded as uint32_t.
     * The allocated region is 4-byte aligned.
     *
     * NOT thread-safe for concurrent allocation. Writer thread only.
     */
    uint32_t alloc(size_t bytes);

    /**
     * Get pointer to allocated data at the given ref.
     * Safe for concurrent reads (data is immutable once written).
     */
    char* get(uint32_t ref);
    const char* get(uint32_t ref) const;

    template <typename T>
    T* getAs(uint32_t ref) { return reinterpret_cast<T*>(get(ref)); }

    template <typename T>
    const T* getAs(uint32_t ref) const { return reinterpret_cast<const T*>(get(ref)); }

    /**
     * Mark a node as held — it will be freed once no readers reference it.
     * Called when COW replaces a node.
     */
    void holdNode(uint32_t ref, size_t nodeBytes);

    /**
     * Transfer held nodes to the generation hold list.
     * Must be called after freeze() with the current generation.
     */
    void transferHoldLists(generation_t generation);

    /**
     * Trim the hold list, freeing nodes from generations no longer in use.
     * Called after GenerationHandler::updateFirstUsedGeneration().
     */
    void trimHoldLists(generation_t usedGen);

    MemoryUsage getMemoryUsage() const;

private:
    DataStoreType                    _store;
    datastore::BufferType<char>      _type;
    const uint32_t                   _typeId;
};

} // namespace vespalib::ptrie
