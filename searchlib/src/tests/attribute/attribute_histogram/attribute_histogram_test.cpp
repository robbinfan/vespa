// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/searchlib/attribute/attribute_histogram.h>
#include <vespa/vespalib/gtest/gtest.h>
#include <cstdint>
#include <limits>
#include <cmath>

using search::attribute::AttributeHistogram;

class AttributeHistogramTest : public ::testing::Test {
protected:
    AttributeHistogram _hist;

    AttributeHistogramTest() : _hist() {}

    void build_uniform(int64_t min_val, int64_t max_val, uint32_t docs_per_value) {
        _hist.reset(min_val, max_val);
        for (int64_t v = min_val; v <= max_val; ++v) {
            _hist.add(v, docs_per_value);
        }
    }
};

TEST_F(AttributeHistogramTest, default_construction_is_invalid) {
    EXPECT_FALSE(_hist.is_valid());
    EXPECT_EQ(0u, _hist.total_count());
    EXPECT_EQ(AttributeHistogram::DEFAULT_NUM_BUCKETS, _hist.num_buckets());
}

TEST_F(AttributeHistogramTest, reset_makes_valid) {
    _hist.reset(0, 100);
    EXPECT_TRUE(_hist.is_valid());
    EXPECT_EQ(0, _hist.min_value());
    EXPECT_EQ(100, _hist.max_value());
    EXPECT_EQ(0u, _hist.total_count());
}

TEST_F(AttributeHistogramTest, reset_with_equal_min_max_is_valid) {
    _hist.reset(42, 42);
    EXPECT_TRUE(_hist.is_valid());
}

TEST_F(AttributeHistogramTest, reset_with_inverted_range_is_invalid) {
    _hist.reset(100, 0);
    EXPECT_FALSE(_hist.is_valid());
}

TEST_F(AttributeHistogramTest, add_and_total_count) {
    _hist.reset(0, 99);
    _hist.add(50);
    _hist.add(50);
    _hist.add(10, 5);
    EXPECT_EQ(7u, _hist.total_count());
}

TEST_F(AttributeHistogramTest, add_to_invalid_histogram_is_noop) {
    _hist.add(42);
    EXPECT_EQ(0u, _hist.total_count());
}

TEST_F(AttributeHistogramTest, remove_decrements_count) {
    _hist.reset(0, 99);
    _hist.add(50, 10);
    _hist.remove(50);
    EXPECT_EQ(9u, _hist.total_count());
}

TEST_F(AttributeHistogramTest, remove_does_not_underflow) {
    _hist.reset(0, 99);
    _hist.remove(50); // bucket is already 0
    EXPECT_EQ(0u, _hist.total_count());
}

TEST_F(AttributeHistogramTest, estimate_empty_histogram) {
    _hist.reset(0, 99);
    EXPECT_EQ(0u, _hist.estimate(0, 99));
}

TEST_F(AttributeHistogramTest, estimate_invalid_histogram) {
    EXPECT_EQ(0u, _hist.estimate(0, 99));
}

TEST_F(AttributeHistogramTest, estimate_range_outside_histogram) {
    _hist.reset(10, 20);
    _hist.add(15, 100);
    EXPECT_EQ(0u, _hist.estimate(0, 5));   // below
    EXPECT_EQ(0u, _hist.estimate(25, 30)); // above
}

TEST_F(AttributeHistogramTest, estimate_full_range) {
    build_uniform(0, 1023, 10);
    uint32_t est = _hist.estimate(0, 1023);
    // Should be close to total (1024 values * 10 docs = 10240)
    EXPECT_NEAR(10240, est, 50); // allow small rounding
}

TEST_F(AttributeHistogramTest, estimate_half_range) {
    build_uniform(0, 1023, 10);
    uint32_t est = _hist.estimate(0, 511);
    EXPECT_NEAR(5120, est, 100);
}

TEST_F(AttributeHistogramTest, estimate_single_value_range) {
    _hist.reset(0, 1023);
    _hist.add(500, 100);
    uint32_t est = _hist.estimate(499, 501);
    // All 100 docs are in bucket containing 500, estimate should be reasonable
    EXPECT_GT(est, 0u);
}

TEST_F(AttributeHistogramTest, estimate_threshold_ascending_basic) {
    build_uniform(0, 1023, 10);
    // Want 5000 docs ascending: should be roughly value 500
    int64_t threshold = _hist.estimate_threshold_for_count(5000, true);
    EXPECT_GT(threshold, 400);
    EXPECT_LT(threshold, 600);
}

TEST_F(AttributeHistogramTest, estimate_threshold_descending_basic) {
    build_uniform(0, 1023, 10);
    // Want 5000 docs descending: should be roughly value 523
    int64_t threshold = _hist.estimate_threshold_for_count(5000, false);
    EXPECT_GT(threshold, 400);
    EXPECT_LT(threshold, 700);
}

TEST_F(AttributeHistogramTest, estimate_threshold_target_zero) {
    build_uniform(0, 99, 10);
    EXPECT_EQ(99, _hist.estimate_threshold_for_count(0, true));
    EXPECT_EQ(0, _hist.estimate_threshold_for_count(0, false));
}

TEST_F(AttributeHistogramTest, estimate_threshold_target_exceeds_total) {
    build_uniform(0, 99, 10);
    EXPECT_EQ(99, _hist.estimate_threshold_for_count(99999, true));
    EXPECT_EQ(0, _hist.estimate_threshold_for_count(99999, false));
}

// Edge case: very large value range (near INT64_MAX)
TEST_F(AttributeHistogramTest, large_value_range_no_overflow) {
    int64_t min_val = -1000000000LL;
    int64_t max_val = 1000000000LL;
    _hist.reset(min_val, max_val);
    EXPECT_TRUE(_hist.is_valid());
    _hist.add(0, 1000);
    _hist.add(min_val, 500);
    _hist.add(max_val, 500);
    EXPECT_EQ(2000u, _hist.total_count());

    uint32_t est = _hist.estimate(min_val, max_val);
    EXPECT_EQ(2000u, est);
}

TEST_F(AttributeHistogramTest, extreme_int64_range) {
    int64_t min_val = std::numeric_limits<int64_t>::min() / 2;
    int64_t max_val = std::numeric_limits<int64_t>::max() / 2;
    _hist.reset(min_val, max_val);
    EXPECT_TRUE(_hist.is_valid());
    _hist.add(0, 1000);
    EXPECT_EQ(1000u, _hist.total_count());
    uint32_t est = _hist.estimate(-100, 100);
    EXPECT_GE(est, 0u); // just ensure no crash
}

// Test with num_buckets=1
TEST_F(AttributeHistogramTest, single_bucket_histogram) {
    AttributeHistogram h(1);
    h.reset(0, 100);
    h.add(50, 1000);
    EXPECT_EQ(1000u, h.estimate(0, 100));
    // Partial range should give fractional estimate
    uint32_t half_est = h.estimate(0, 50);
    EXPECT_GT(half_est, 0u);
    EXPECT_LT(half_est, 1000u);
}

// Test skewed distribution estimation accuracy
TEST_F(AttributeHistogramTest, skewed_distribution_accuracy) {
    _hist.reset(0, 999);
    // Zipf-like: most docs at low values
    for (int64_t v = 0; v < 1000; ++v) {
        uint32_t count = static_cast<uint32_t>(10000.0 / (v + 1));
        if (count > 0) {
            _hist.add(v, count);
        }
    }
    uint32_t total = _hist.total_count();
    // Estimate for low range should be large proportion of total
    uint32_t low_est = _hist.estimate(0, 99);
    EXPECT_GT(low_est, total / 3); // most mass in [0,99] for Zipf

    // Estimate for high range should be small
    uint32_t high_est = _hist.estimate(900, 999);
    EXPECT_LT(high_est, total / 10);
}

GTEST_MAIN_RUN_ALL_TESTS()
