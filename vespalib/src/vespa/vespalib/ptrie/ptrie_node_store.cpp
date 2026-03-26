// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "ptrie_node_store.h"
#include <vespa/vespalib/datastore/datastore.hpp>
#include <cstring>

namespace vespalib::ptrie {

using datastore::EntryRef;

constexpr size_t MIN_BUFFER_ARRAYS = 1024;

PTrieNodeStore::PTrieNodeStore()
    : _store(),
      _type(RefType::align(1),
            MIN_BUFFER_ARRAYS,
            RefType::offsetSize() / RefType::align(1)),
      _typeId(0)
{
    _store.addType(&_type);
    _store.init_primary_buffers();
}

PTrieNodeStore::~PTrieNodeStore()
{
    _store.dropBuffers();
}

uint32_t
PTrieNodeStore::alloc(size_t bytes)
{
    // Align to 4 bytes (RefType alignment).
    size_t aligned = RefType::align(bytes);
    auto result = _store.rawAllocator<char>(_typeId).alloc(aligned);

    // Zero-fill for consistent reads.
    if (aligned > bytes) {
        std::memset(result.data + bytes, 0, aligned - bytes);
    }

    return result.ref.ref();
}

char*
PTrieNodeStore::get(uint32_t ref)
{
    RefType internalRef(EntryRef(ref));
    return const_cast<char*>(_store.getEntry<char>(internalRef));
}

const char*
PTrieNodeStore::get(uint32_t ref) const
{
    RefType internalRef(EntryRef(ref));
    return _store.getEntry<char>(internalRef);
}

void
PTrieNodeStore::holdNode(uint32_t ref, size_t nodeBytes)
{
    if (ref == INVALID_REF) return;
    size_t aligned = RefType::align(nodeBytes);
    _store.holdElem(EntryRef(ref), aligned);
}

void
PTrieNodeStore::transferHoldLists(generation_t generation)
{
    _store.transferHoldLists(generation);
}

void
PTrieNodeStore::trimHoldLists(generation_t usedGen)
{
    _store.trimElemHoldList(usedGen);
}

MemoryUsage
PTrieNodeStore::getMemoryUsage() const
{
    return _store.getMemoryUsage();
}

} // namespace vespalib::ptrie
