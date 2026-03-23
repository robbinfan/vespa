// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace search::attribute {

/**
 * Equi-width histogram for fast range query hit estimation on numeric attributes.
 *
 * Maintains bucket counts for a fixed number of equi-width buckets spanning
 * the value range [min_value, max_value]. Provides O(num_buckets) range
 * estimation that is far more accurate than the uniform distribution
 * assumption for skewed data.
 *
 * Lifecycle:
 *   - Built from the enum dictionary after commit (rebuild_from_dictionary)
 *   - Queried during search context creation (estimate_hits_in_range)
 *   - Rebuilt on restart (same as posting lists)
 *
 * Memory overhead: num_buckets * 4 bytes per attribute (default 1024 = 4KB).
 */
class AttributeHistogram {
public:
    static constexpr uint32_t DEFAULT_NUM_BUCKETS = 1024;

    AttributeHistogram();
    explicit AttributeHistogram(uint32_t num_buckets);

    /**
     * Reset histogram and set new value bounds.
     * Must be called before adding values.
     */
    void reset(int64_t min_value, int64_t max_value);

    /**
     * Add a document with the given value to the histogram.
     * O(1) per call.
     */
    void add(int64_t value);

    /**
     * Add count documents with the given value. O(1).
     * Used during histogram rebuild from dictionary.
     */
    void add(int64_t value, uint32_t count);

    /**
     * Remove a document with the given value from the histogram.
     * O(1) per call. Used for incremental updates.
     */
    void remove(int64_t value);

    /**
     * Estimate the number of hits in the range [lo, hi].
     * O(num_buckets) but typically much faster due to early termination.
     */
    uint32_t estimate(int64_t lo, int64_t hi) const;

    /**
     * Find the value threshold V such that approximately target_count
     * documents fall within the "best" portion of the sort order.
     *
     * For ascending=true (ORDER BY attr ASC LIMIT K):
     *   Returns V such that estimate(min_value, V) >= target_count.
     *   Documents with attr <= V include the top-K candidates.
     *
     * For ascending=false (ORDER BY attr DESC LIMIT K):
     *   Returns V such that estimate(V, max_value) >= target_count.
     *   Documents with attr >= V include the top-K candidates.
     *
     * Used for sort optimization: inject range bound to reduce candidate set.
     * O(num_buckets) worst case, O(log(num_buckets)) with binary search.
     */
    int64_t estimate_threshold_for_count(uint32_t target_count, bool ascending) const;

    bool     is_valid() const { return _valid; }
    uint32_t total_count() const { return _total_count; }
    uint32_t num_buckets() const { return _num_buckets; }
    int64_t  min_value() const { return _min_value; }
    int64_t  max_value() const { return _max_value; }

private:
    uint32_t bucket_index(int64_t value) const;

    std::vector<uint32_t> _buckets;
    uint32_t _num_buckets;
    int64_t  _min_value;
    int64_t  _max_value;
    double   _bucket_width;
    uint32_t _total_count;
    bool     _valid;
};

}
