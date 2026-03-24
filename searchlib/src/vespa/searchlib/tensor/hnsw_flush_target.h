// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "hnsw_hybrid_index.h"
#include <vespa/vespalib/stllike/string.h>
#include <memory>

namespace search::tensor {

class DenseTensorStore;

/**
 * Manages the flush and fusion lifecycle for an HnswHybridIndex.
 *
 * This class is called from DenseTensorAttribute::onCommit() to check
 * whether a flush or fusion is needed and trigger it.
 *
 * Naming convention (mirroring searchcorespi::index):
 *   flush  → memory index → hnsw.flush.<id>/
 *   fusion → N flush indexes → hnsw.fusion.<id>/  (HnswFusion)
 *
 * The actual heavy I/O is done on the flush thread (via the task returned
 * by maybe_flush() / maybe_fusion()).  The caller is responsible for
 * running these tasks off the attribute writer thread.
 */
class HnswFlushTarget {
public:
    HnswFlushTarget(HnswHybridIndex& hybrid_index,
                    const DenseTensorStore& tensor_store,
                    uint32_t dims);

    ~HnswFlushTarget();

    /** True if the memory index should be flushed to disk now. */
    bool needs_flush() const;

    /** True if fusion should be triggered now. */
    bool needs_fusion() const;

    /**
     * Flush the current memory index to a new hnsw.flush.<id>/ directory.
     *
     * @param committed_doc_id_limit  current committed doc id limit
     * @return flush id on success, 0 on failure
     *
     * Must be called off the attribute writer thread.
     * The caller must hold the attribute read guard during the call.
     */
    uint32_t do_flush(uint32_t committed_doc_id_limit);

    /**
     * Run fusion: merge all existing disk indexes into one.
     * Must be called off the attribute writer thread.
     */
    bool do_fusion();

    HnswHybridIndex& hybrid_index() { return _hybrid_index; }

private:
    HnswHybridIndex&         _hybrid_index;
    const DenseTensorStore&  _tensor_store;
    uint32_t                 _dims;
};

}
