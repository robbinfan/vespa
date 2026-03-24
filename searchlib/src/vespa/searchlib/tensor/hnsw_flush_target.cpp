// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "hnsw_flush_target.h"

#include <vespa/log/log.h>
LOG_SETUP(".tensor.hnsw_flush_target");

namespace search::tensor {

HnswFlushTarget::HnswFlushTarget(HnswHybridIndex& hybrid_index,
                                   const DenseTensorStore& tensor_store,
                                   uint32_t dims)
    : _hybrid_index(hybrid_index),
      _tensor_store(tensor_store),
      _dims(dims)
{}

HnswFlushTarget::~HnswFlushTarget() = default;

bool HnswFlushTarget::needs_flush() const
{
    return _hybrid_index.needs_flush();
}

bool HnswFlushTarget::needs_fusion() const
{
    return _hybrid_index.needs_fusion();
}

uint32_t HnswFlushTarget::do_flush(uint32_t committed_doc_id_limit)
{
    // HnswHybridIndex::flush_memory_index() reads float32 vectors directly
    // from the DocVectorAccess supplied at construction (backed by DenseTensorStore).
    // All graph extraction and vector I/O is handled inside flush_memory_index().
    return _hybrid_index.flush_memory_index(committed_doc_id_limit);
}

bool HnswFlushTarget::do_fusion()
{
    return _hybrid_index.run_fusion();
}

}
