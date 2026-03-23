// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "attribute_histogram.h"
#include <algorithm>
#include <cmath>

namespace search::attribute {

AttributeHistogram::AttributeHistogram()
    : AttributeHistogram(DEFAULT_NUM_BUCKETS)
{
}

AttributeHistogram::AttributeHistogram(uint32_t num_buckets)
    : _buckets(std::max(num_buckets, 1u), 0),
      _num_buckets(std::max(num_buckets, 1u)),
      _min_value(0),
      _max_value(0),
      _bucket_width(1.0),
      _total_count(0),
      _valid(false)
{
}

void
AttributeHistogram::reset(int64_t min_value, int64_t max_value)
{
    _min_value = min_value;
    _max_value = max_value;
    _total_count = 0;
    std::fill(_buckets.begin(), _buckets.end(), 0);
    if (max_value > min_value) {
        // Use floating point to avoid int64_t overflow when max_value - min_value + 1 exceeds INT64_MAX
        _bucket_width = (static_cast<double>(max_value) - static_cast<double>(min_value) + 1.0) / _num_buckets;
        _valid = true;
    } else if (max_value == min_value) {
        _bucket_width = 1.0;
        _valid = true;
    } else {
        _valid = false;
    }
}

uint32_t
AttributeHistogram::bucket_index(int64_t value) const
{
    if (value <= _min_value) return 0;
    if (value >= _max_value) return _num_buckets - 1;
    // Use double subtraction to avoid int64_t overflow/precision loss for large ranges
    uint32_t idx = static_cast<uint32_t>((static_cast<double>(value) - static_cast<double>(_min_value)) / _bucket_width);
    if (idx >= _num_buckets) idx = _num_buckets - 1;
    return idx;
}

void
AttributeHistogram::add(int64_t value)
{
    if (!_valid) return;
    _buckets[bucket_index(value)]++;
    _total_count++;
}

void
AttributeHistogram::add(int64_t value, uint32_t count)
{
    if (!_valid || count == 0) return;
    _buckets[bucket_index(value)] += count;
    _total_count += count;
}

void
AttributeHistogram::remove(int64_t value)
{
    if (!_valid) return;
    uint32_t idx = bucket_index(value);
    if (_buckets[idx] > 0) {
        _buckets[idx]--;
        _total_count--;
    }
}

uint32_t
AttributeHistogram::estimate(int64_t lo, int64_t hi) const
{
    if (!_valid || lo > _max_value || hi < _min_value) return 0;
    lo = std::max(lo, _min_value);
    hi = std::min(hi, _max_value);

    double sum = 0;
    uint32_t lo_bucket = bucket_index(lo);
    uint32_t hi_bucket = bucket_index(hi);

    // Use double(hi) + 1.0 instead of hi + 1 to avoid int64_t overflow when hi == INT64_MAX
    double hi_upper = static_cast<double>(hi) + 1.0;

    if (lo_bucket == hi_bucket) {
        // Query range falls within a single bucket — use fractional overlap
        double bkt_lo = _min_value + lo_bucket * _bucket_width;
        double bkt_hi = bkt_lo + _bucket_width;
        double overlap = (std::min(bkt_hi, hi_upper) -
                         std::max(bkt_lo, static_cast<double>(lo))) / _bucket_width;
        return static_cast<uint32_t>(std::round(overlap * _buckets[lo_bucket]));
    }

    // Partial first bucket
    {
        double bkt_lo = _min_value + lo_bucket * _bucket_width;
        double bkt_hi = bkt_lo + _bucket_width;
        double frac = (bkt_hi - std::max(bkt_lo, static_cast<double>(lo))) / _bucket_width;
        sum += frac * _buckets[lo_bucket];
    }

    // Full middle buckets
    for (uint32_t i = lo_bucket + 1; i < hi_bucket; ++i) {
        sum += _buckets[i];
    }

    // Partial last bucket
    {
        double bkt_lo = _min_value + hi_bucket * _bucket_width;
        double bkt_hi = bkt_lo + _bucket_width;
        double frac = (std::min(bkt_hi, hi_upper) - bkt_lo) / _bucket_width;
        sum += frac * _buckets[hi_bucket];
    }

    return static_cast<uint32_t>(std::round(sum));
}

int64_t
AttributeHistogram::estimate_threshold_for_count(uint32_t target_count, bool ascending) const
{
    if (!_valid || target_count == 0) {
        return ascending ? _max_value : _min_value;
    }
    if (target_count >= _total_count) {
        return ascending ? _max_value : _min_value;
    }

    // Scan buckets from the "good" end, accumulating until target reached
    uint32_t accumulated = 0;
    if (ascending) {
        // ORDER BY attr ASC: scan from lowest bucket upward
        for (uint32_t i = 0; i < _num_buckets; ++i) {
            accumulated += _buckets[i];
            if (accumulated >= target_count) {
                // The threshold is at the upper edge of this bucket
                // Interpolate within the bucket for better precision
                uint32_t excess = accumulated - target_count;
                double frac = 1.0 - static_cast<double>(excess) / std::max(1u, _buckets[i]);
                double bucket_lo = _min_value + i * _bucket_width;
                return static_cast<int64_t>(bucket_lo + frac * _bucket_width);
            }
        }
        return _max_value;
    } else {
        // ORDER BY attr DESC: scan from highest bucket downward
        for (uint32_t i = _num_buckets; i > 0; --i) {
            accumulated += _buckets[i - 1];
            if (accumulated >= target_count) {
                uint32_t excess = accumulated - target_count;
                double frac = 1.0 - static_cast<double>(excess) / std::max(1u, _buckets[i - 1]);
                double bucket_hi = _min_value + i * _bucket_width;
                return static_cast<int64_t>(bucket_hi - frac * _bucket_width);
            }
        }
        return _min_value;
    }
}

}
