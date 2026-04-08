// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "nearest_neighbor_index.h"

namespace search::tensor {

std::vector<std::vector<NearestNeighborIndex::Neighbor>>
NearestNeighborIndex::find_top_k_batch(
        uint32_t k,
        vespalib::ConstArrayRef<vespalib::eval::TypedCells> vectors,
        uint32_t explore_k,
        double distance_threshold) const
{
    std::vector<std::vector<Neighbor>> results;
    results.reserve(vectors.size());
    for (const auto& vec : vectors) {
        results.emplace_back(find_top_k(k, vec, explore_k, distance_threshold));
    }
    return results;
}

std::vector<std::vector<NearestNeighborIndex::Neighbor>>
NearestNeighborIndex::find_top_k_batch_with_filter(
        uint32_t k,
        vespalib::ConstArrayRef<vespalib::eval::TypedCells> vectors,
        const BitVector &filter,
        uint32_t explore_k,
        double distance_threshold) const
{
    std::vector<std::vector<Neighbor>> results;
    results.reserve(vectors.size());
    for (const auto& vec : vectors) {
        results.emplace_back(find_top_k_with_filter(k, vec, filter, explore_k, distance_threshold));
    }
    return results;
}

}
