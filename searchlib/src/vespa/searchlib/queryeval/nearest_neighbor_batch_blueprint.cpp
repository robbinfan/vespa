// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "emptysearch.h"
#include "nearest_neighbor_batch_blueprint.h"
#include "nns_index_iterator.h"
#include <vespa/eval/eval/fast_value.h>
#include <vespa/searchlib/fef/termfieldmatchdataarray.h>
#include <vespa/searchlib/tensor/dense_tensor_attribute.h>
#include <vespa/searchlib/tensor/distance_function_factory.h>
#include <vespa/vespalib/util/arrayref.h>
#include <algorithm>
#include <unordered_map>
#include <vespa/log/log.h>

LOG_SETUP(".searchlib.queryeval.nearest_neighbor_batch_blueprint");

using vespalib::eval::CellType;
using vespalib::eval::FastValueBuilderFactory;
using vespalib::eval::TypedCells;
using vespalib::eval::Value;
using vespalib::eval::ValueType;

namespace search::queryeval {

namespace {

template<typename LCT, typename RCT>
std::unique_ptr<Value>
convert_cells(const ValueType &new_type, std::unique_ptr<Value> old_value)
{
    auto old_cells = old_value->cells().typify<LCT>();
    auto builder = FastValueBuilderFactory::get().create_value_builder<RCT>(new_type);
    auto new_cells = builder->add_subspace();
    assert(old_cells.size() == new_cells.size());
    auto p = new_cells.begin();
    for (LCT value : old_cells) {
        RCT conv(value);
        *p++ = conv;
    }
    return builder->build(std::move(builder));
}

struct ConvertCellsSelector
{
    template <typename LCT, typename RCT>
    static auto invoke(const ValueType &new_type, std::unique_ptr<Value> old_value) {
        return convert_cells<LCT, RCT>(new_type, std::move(old_value));
    }
    auto operator() (CellType from, CellType to, std::unique_ptr<Value> old_value) const {
        using MyTypify = vespalib::eval::TypifyCellType;
        ValueType new_type = old_value->type().cell_cast(to);
        return vespalib::typify_invoke<2,MyTypify,ConvertCellsSelector>(from, to, new_type, std::move(old_value));
    }
};

} // namespace <unnamed>

NearestNeighborBatchBlueprint::NearestNeighborBatchBlueprint(
        const queryeval::FieldSpec& field,
        const tensor::ITensorAttribute& attr_tensor,
        std::vector<std::unique_ptr<Value>> query_tensors,
        uint32_t target_num_hits_per_query,
        bool approximate,
        uint32_t explore_additional_hits,
        double distance_threshold,
        double brute_force_limit,
        bool use_speculative,
        bool use_progressive,
        uint32_t progressive_draft_steps)
    : ComplexLeafBlueprint(field),
      _attr_tensor(attr_tensor),
      _query_tensors(std::move(query_tensors)),
      _target_num_hits_per_query(target_num_hits_per_query),
      _approximate(approximate),
      _explore_additional_hits(explore_additional_hits),
      _distance_threshold(std::numeric_limits<double>::max()),
      _brute_force_limit(brute_force_limit),
      _fallback_dist_fun(),
      _dist_fun(nullptr),
      _use_speculative(use_speculative),
      _use_progressive(use_progressive),
      _progressive_draft_steps(progressive_draft_steps),
      _merged_hits(),
      _global_filter(GlobalFilter::create())
{
    CellType attr_ct = _attr_tensor.getTensorType().cell_type();
    _fallback_dist_fun = search::tensor::make_distance_function(_attr_tensor.distance_metric(), attr_ct);
    _dist_fun = _fallback_dist_fun.get();
    assert(_dist_fun);
    auto nns_index = _attr_tensor.nearest_neighbor_index();
    if (nns_index) {
        _dist_fun = nns_index->distance_function();
        assert(_dist_fun);
    }

    // Convert all query tensors to the required cell type.
    CellType required_ct = _dist_fun->expected_cell_type();
    for (auto& qt : _query_tensors) {
        auto query_ct = qt->cells().type;
        if (query_ct != required_ct) {
            ConvertCellsSelector converter;
            qt = converter(query_ct, required_ct, std::move(qt));
        }
    }

    if (distance_threshold < std::numeric_limits<double>::max()) {
        _distance_threshold = _dist_fun->convert_threshold(distance_threshold);
    }

    // Estimate: total hits is union across all queries, capped at num_docs.
    uint32_t est_hits = _attr_tensor.get_num_docs();
    setEstimate(HitEstimate(est_hits, false));
    set_want_global_filter(nns_index && _approximate);
}

NearestNeighborBatchBlueprint::~NearestNeighborBatchBlueprint() = default;

void
NearestNeighborBatchBlueprint::set_global_filter(const GlobalFilter &global_filter)
{
    _global_filter = global_filter.shared_from_this();
    auto nns_index = _attr_tensor.nearest_neighbor_index();
    LOG(debug, "batch set_global_filter with: %s / %s / %s / %zu queries",
        (_approximate ? "approximate" : "exact"),
        (nns_index ? "nns_index" : "no_index"),
        (_global_filter->has_filter() ? "has_filter" : "no_filter"),
        _query_tensors.size());

    if (_approximate && nns_index) {
        uint32_t est_hits = _attr_tensor.get_num_docs();
        if (_global_filter->has_filter()) {
            uint32_t max_hits = _global_filter->filter()->countTrueBits();
            double max_hit_ratio = static_cast<double>(max_hits) / est_hits;
            if (max_hit_ratio < _brute_force_limit) {
                _approximate = false;
                LOG(debug, "batch: too many hits filtered, using brute force");
            } else {
                est_hits = std::min(est_hits, max_hits);
            }
        }
        if (_approximate) {
            // For batch, estimate total unique hits across all queries.
            uint32_t total_target = std::min(est_hits, _target_num_hits_per_query * static_cast<uint32_t>(_query_tensors.size()));
            setEstimate(HitEstimate(total_target, false));
            perform_top_k_batch();
            LOG(debug, "perform_top_k_batch found %zu merged hits", _merged_hits.size());
        }
    }
}

void
NearestNeighborBatchBlueprint::perform_top_k_batch()
{
    auto nns_index = _attr_tensor.nearest_neighbor_index();
    if (!_approximate || !nns_index || _query_tensors.empty()) {
        return;
    }

    // Gather TypedCells for all query tensors.
    std::vector<TypedCells> query_cells;
    query_cells.reserve(_query_tensors.size());
    for (const auto& qt : _query_tensors) {
        query_cells.push_back(qt->cells());
    }

    uint32_t k = _target_num_hits_per_query;
    uint32_t explore_k = k + _explore_additional_hits;

    std::vector<std::vector<search::tensor::NearestNeighborIndex::Neighbor>> per_query_hits;
    if (_use_progressive && _query_tensors.size() > _progressive_draft_steps) {
        // Progressive retrieval: draft steps do HNSW, rest re-rank candidates.
        const BitVector *filter_ptr = _global_filter->has_filter() ? _global_filter->filter() : nullptr;
        per_query_hits = nns_index->find_top_k_progressive(k, query_cells, filter_ptr,
                                                           explore_k, _distance_threshold,
                                                           _progressive_draft_steps);
    } else if (_global_filter->has_filter()) {
        auto filter = _global_filter->filter();
        per_query_hits = nns_index->find_top_k_batch_with_filter(k, query_cells, *filter, explore_k, _distance_threshold);
    } else {
        per_query_hits = nns_index->find_top_k_batch(k, query_cells, explore_k, _distance_threshold);
    }

    // Merge: union of all per-query results, keeping min distance per docid.
    std::unordered_map<uint32_t, double> doc_to_min_dist;
    for (const auto& hits : per_query_hits) {
        for (const auto& hit : hits) {
            auto it = doc_to_min_dist.find(hit.docid);
            if (it == doc_to_min_dist.end()) {
                doc_to_min_dist.emplace(hit.docid, hit.distance);
            } else {
                it->second = std::min(it->second, hit.distance);
            }
        }
    }

    _merged_hits.clear();
    _merged_hits.reserve(doc_to_min_dist.size());
    for (const auto& [docid, dist] : doc_to_min_dist) {
        _merged_hits.emplace_back(docid, dist);
    }
    std::sort(_merged_hits.begin(), _merged_hits.end(),
              [](const auto& a, const auto& b) { return a.docid < b.docid; });
}

std::unique_ptr<SearchIterator>
NearestNeighborBatchBlueprint::createLeafSearch(const search::fef::TermFieldMatchDataArray& tfmda, bool) const
{
    assert(tfmda.size() == 1);
    fef::TermFieldMatchData &tfmd = *tfmda[0];
    if (!_merged_hits.empty()) {
        return NnsIndexIterator::create(tfmd, _merged_hits, _dist_fun);
    }
    // Fallback: empty search (brute-force path would need NearestNeighborIterator per query).
    return std::make_unique<EmptySearch>();
}

void
NearestNeighborBatchBlueprint::visitMembers(vespalib::ObjectVisitor& visitor) const
{
    ComplexLeafBlueprint::visitMembers(visitor);
    visitor.visitString("attribute_tensor", _attr_tensor.getTensorType().to_spec());
    visitor.visitInt("num_query_tensors", _query_tensors.size());
    visitor.visitInt("target_num_hits_per_query", _target_num_hits_per_query);
    visitor.visitBool("approximate", _approximate);
    visitor.visitInt("explore_additional_hits", _explore_additional_hits);
    visitor.visitBool("use_speculative", _use_speculative);
    visitor.visitBool("use_progressive", _use_progressive);
    if (_use_progressive) {
        visitor.visitInt("progressive_draft_steps", _progressive_draft_steps);
    }
}

bool
NearestNeighborBatchBlueprint::always_needs_unpack() const
{
    return true;
}

}
