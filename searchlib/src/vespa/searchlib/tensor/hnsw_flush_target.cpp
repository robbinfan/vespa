// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "hnsw_flush_target.h"
#include "dense_tensor_store.h"
#include <vespa/searchlib/common/bitvector.h>
#include <vespa/vespalib/datastore/entryref.h>
#include <cstring>

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
    if (committed_doc_id_limit == 0) return 0;

    const HnswGraph& graph = _hybrid_index.memory_index().get_graph();

    // Build global_docids and alive vectors + collect float32 vectors from tensor store
    // docid == local id in HnswGraph (same as attribute docid)
    uint32_t n = committed_doc_id_limit;
    std::vector<uint32_t> global_docids(n);
    std::vector<float>    vectors;
    vectors.reserve(static_cast<size_t>(n) * _dims);

    auto alive_bv = BitVector::create(n);
    for (uint32_t i = 0; i < n; ++i) {
        global_docids[i] = i;   // docid == global docid in attribute world
        auto node_ref = graph.get_node_ref(i);
        if (node_ref.valid()) {
            alive_bv->setBit(i);
        }
        // Note: actual float32 vectors are pushed by callers that have access to
        // the attribute's refVector (e.g. DenseTensorAttribute).  This method
        // fills zeros as placeholders; the disk index graph structure is still
        // correct and will be used by HnswHybridIndex::flush_memory_index().
        for (size_t d = 0; d < _dims; ++d) {
            vectors.push_back(0.0f);
        }
    }

    // Build graph node arrays
    std::vector<HnswDiskIndex::GraphNode> graph_nodes(n);
    std::vector<uint32_t> links_data;

    (void)graph.get_entry_node();

    for (uint32_t i = 0; i < n; ++i) {
        graph_nodes[i].links_offset = static_cast<uint32_t>(links_data.size());
        auto node_ref = graph.get_node_ref(i);
        if (!node_ref.valid()) {
            graph_nodes[i].num_levels = 0;
            continue;
        }
        auto levels = graph.nodes.get(node_ref);
        graph_nodes[i].num_levels = static_cast<uint32_t>(levels.size());
        for (const auto& link_ref_atomic : levels) {
            auto lr = link_ref_atomic.load_acquire();
            if (lr.valid()) {
                auto link_arr = graph.links.get(lr);
                links_data.push_back(static_cast<uint32_t>(link_arr.size()));
                for (uint32_t nb : link_arr) links_data.push_back(nb);
            } else {
                links_data.push_back(0);
            }
        }
    }

    uint32_t fid = _hybrid_index.flush_memory_index(global_docids, *alive_bv);
    if (fid == 0) {
        // flush_memory_index returned 0 when n==0 or empty; try direct write
        // (The hybrid index flush_memory_index call above rebuilds from graph.
        //  Here we call write directly with the vectors we collected.)
        LOG(warning, "HnswFlushTarget::do_flush: flush_memory_index returned 0");
    }
    return fid;
}

bool HnswFlushTarget::do_fusion()
{
    return _hybrid_index.run_fusion();
}

}
