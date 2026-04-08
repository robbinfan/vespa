// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#pragma once

#include "blueprint.h"
#include <vespa/searchlib/tensor/distance_function.h>
#include <vespa/searchlib/tensor/nearest_neighbor_index.h>

namespace search::tensor { class ITensorAttribute; }
namespace vespalib::eval { struct Value; }

namespace search::queryeval {

/**
 * Blueprint for batch nearest neighbor search across multiple query tensors.
 *
 * Optimized for MIND-style multi-interest retrieval where multiple embeddings
 * target the same tensor field. Uses shared graph traversal and batch distance
 * computation via HnswIndex::find_top_k_batch.
 *
 * The result is the union of all per-query results (deduplicated, sorted by docid).
 * Each document's score = min distance across all query vectors (best interest match).
 */
class NearestNeighborBatchBlueprint : public ComplexLeafBlueprint {
private:
    const tensor::ITensorAttribute& _attr_tensor;
    std::vector<std::unique_ptr<vespalib::eval::Value>> _query_tensors;
    uint32_t _target_num_hits_per_query;
    bool _approximate;
    uint32_t _explore_additional_hits;
    double _distance_threshold;
    double _brute_force_limit;
    search::tensor::DistanceFunction::UP _fallback_dist_fun;
    const search::tensor::DistanceFunction *_dist_fun;
    bool _use_speculative;

    // Union of all per-query results, deduplicated and sorted by docid.
    // Each entry stores (docid, min_distance_across_queries).
    std::vector<search::tensor::NearestNeighborIndex::Neighbor> _merged_hits;
    std::shared_ptr<const GlobalFilter> _global_filter;

    void perform_top_k_batch();
public:
    NearestNeighborBatchBlueprint(const queryeval::FieldSpec& field,
                                  const tensor::ITensorAttribute& attr_tensor,
                                  std::vector<std::unique_ptr<vespalib::eval::Value>> query_tensors,
                                  uint32_t target_num_hits_per_query,
                                  bool approximate,
                                  uint32_t explore_additional_hits,
                                  double distance_threshold,
                                  double brute_force_limit,
                                  bool use_speculative = false);
    NearestNeighborBatchBlueprint(const NearestNeighborBatchBlueprint&) = delete;
    NearestNeighborBatchBlueprint& operator=(const NearestNeighborBatchBlueprint&) = delete;
    ~NearestNeighborBatchBlueprint();

    const tensor::ITensorAttribute& get_attribute_tensor() const { return _attr_tensor; }
    uint32_t get_target_num_hits_per_query() const { return _target_num_hits_per_query; }
    size_t get_num_query_tensors() const { return _query_tensors.size(); }
    void set_global_filter(const GlobalFilter &global_filter) override;

    std::unique_ptr<SearchIterator> createLeafSearch(const search::fef::TermFieldMatchDataArray& tfmda,
                                                     bool strict) const override;
    void visitMembers(vespalib::ObjectVisitor& visitor) const override;
    bool always_needs_unpack() const override;
};

}
