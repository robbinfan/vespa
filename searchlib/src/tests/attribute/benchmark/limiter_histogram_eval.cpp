// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

/**
 * Evaluation benchmark: histogram-guided match-phase limiter accuracy.
 *
 * Simulates the match-phase limiter flow at different hit_ratios and measures:
 *   1. Threshold accuracy: how close is the histogram-derived value bound
 *      to covering exactly want_hits documents?
 *   2. Safety margin: is k = 2 * want_hits enough across distributions?
 *   3. Under-coverage rate: how often does the range cover FEWER than
 *      want_hits docs (causing result quality loss)?
 *   4. Over-coverage ratio: how much larger is the actual range coverage
 *      vs want_hits (waste, but better than under-coverage)?
 *
 * The key question from Zhang Jie: "估不准问题不大" (inaccuracy is ok for
 * match-phase since it's adaptive) but "hitratio 30%+ 收益巨大" (benefit
 * is huge for high hit_ratio). This benchmark quantifies both claims.
 *
 * Flow being evaluated (mirrors AttributeLimiter::create_search):
 *   1. Given want_hits = max_hits / match_freq
 *   2. k = want_hits * multiplier (we test 1x, 1.5x, 2x, 3x)
 *   3. threshold = histogram.estimate_threshold_for_count(k, ascending)
 *   4. actual_covered = count docs in [min, threshold] (ascending)
 *   5. Compare actual_covered vs want_hits
 */

#include <vespa/searchlib/attribute/attribute_histogram.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using search::attribute::AttributeHistogram;

// ---------------------------------------------------------------------------
// Data distribution generators (same as range_estimation_benchmark.cpp)
// ---------------------------------------------------------------------------

std::vector<int64_t> gen_uniform(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> dist(lo, hi);
    std::vector<int64_t> v(n);
    for (auto &x : v) x = dist(rng);
    return v;
}

std::vector<int64_t> gen_zipf(size_t n, int64_t lo, int64_t hi, double skew, uint32_t seed) {
    std::mt19937_64 rng(seed);
    int64_t range = hi - lo + 1;
    std::vector<double> cdf(range);
    double sum = 0;
    for (int64_t i = 0; i < range; ++i) {
        sum += 1.0 / std::pow(i + 1, skew);
        cdf[i] = sum;
    }
    for (auto &c : cdf) c /= sum;
    std::uniform_real_distribution<double> udist(0.0, 1.0);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        double u = udist(rng);
        auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
        x = lo + std::distance(cdf.begin(), it);
        if (x > hi) x = hi;
    }
    return v;
}

std::vector<int64_t> gen_normal(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
    std::mt19937_64 rng(seed);
    double mean = (lo + hi) / 2.0;
    double stddev = (hi - lo) / 6.0;
    std::normal_distribution<double> dist(mean, stddev);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        double val = dist(rng);
        x = std::max(lo, std::min(hi, static_cast<int64_t>(std::round(val))));
    }
    return v;
}

std::vector<int64_t> gen_exponential(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
    std::mt19937_64 rng(seed);
    double lambda = 5.0 / (hi - lo);
    std::exponential_distribution<double> dist(lambda);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        double val = dist(rng);
        x = lo + static_cast<int64_t>(val);
        if (x > hi) x = lo + (x - lo) % (hi - lo + 1);
    }
    return v;
}

// ---------------------------------------------------------------------------
// Ground truth: count docs with value <= threshold (ascending)
//               or value >= threshold (descending)
// ---------------------------------------------------------------------------

uint32_t count_in_range(const std::vector<int64_t> &sorted_values,
                        int64_t threshold, bool ascending) {
    if (ascending) {
        // count docs where value <= threshold
        auto it = std::upper_bound(sorted_values.begin(), sorted_values.end(), threshold);
        return static_cast<uint32_t>(it - sorted_values.begin());
    } else {
        // count docs where value >= threshold
        auto it = std::lower_bound(sorted_values.begin(), sorted_values.end(), threshold);
        return static_cast<uint32_t>(sorted_values.end() - it);
    }
}

// ---------------------------------------------------------------------------
// Evaluate one configuration
// ---------------------------------------------------------------------------

struct EvalResult {
    std::string dist_name;
    double      hit_ratio;
    double      multiplier;
    bool        ascending;
    uint32_t    want_hits;        // max_hits / hit_ratio
    uint32_t    k;                // want_hits * multiplier
    int64_t     threshold;        // histogram-derived value bound
    uint32_t    actual_covered;   // ground truth: docs covered by range
    double      coverage_ratio;   // actual_covered / want_hits
    bool        under_covered;    // actual_covered < want_hits
    uint32_t    docs_saved;       // total_docs - actual_covered (docs NOT evaluated)
    double      reduction_pct;    // docs_saved / total_docs * 100
};

void run_eval(uint32_t num_docs, int64_t value_lo, int64_t value_hi) {
    std::cout << "\n================================================================\n";
    std::cout << "  HISTOGRAM-GUIDED LIMITER EVALUATION\n";
    std::cout << "  Documents:   " << num_docs << "\n";
    std::cout << "  Value range: [" << value_lo << ", " << value_hi << "]\n";
    std::cout << "================================================================\n";

    // Distributions
    struct DistData {
        std::string name;
        std::vector<int64_t> values;
    };
    std::vector<DistData> distributions;
    distributions.push_back({"Uniform",     gen_uniform(num_docs, value_lo, value_hi, 42)});
    distributions.push_back({"Zipf(1.0)",   gen_zipf(num_docs, value_lo, value_hi, 1.0, 42)});
    distributions.push_back({"Normal",      gen_normal(num_docs, value_lo, value_hi, 42)});
    distributions.push_back({"Exponential", gen_exponential(num_docs, value_lo, value_hi, 42)});

    // hit_ratios to test (Zhang Jie: "30%+ 收益巨大")
    std::vector<double> hit_ratios = {0.01, 0.05, 0.1, 0.2, 0.3, 0.5, 0.8, 1.0};

    // k multipliers to test (Zhang Jie: "有可能需要 k = 2*maxhits")
    std::vector<double> multipliers = {1.0, 1.5, 2.0, 3.0};

    constexpr uint32_t max_hits = 10000;

    std::vector<EvalResult> results;

    for (auto &dd : distributions) {
        // Build histogram
        AttributeHistogram histogram(1024);
        histogram.reset(value_lo, value_hi);
        for (auto v : dd.values) {
            histogram.add(v);
        }

        // Sort for ground truth
        auto sorted = dd.values;
        std::sort(sorted.begin(), sorted.end());

        for (double hit_ratio : hit_ratios) {
            uint32_t want_hits = static_cast<uint32_t>(
                std::min(static_cast<double>(0x7fffFFFF),
                         std::max(128.0, static_cast<double>(max_hits) / hit_ratio)));

            // Cap at num_docs
            if (want_hits > num_docs) want_hits = num_docs;

            for (double mult : multipliers) {
                for (bool ascending : {true, false}) {
                    uint32_t k = static_cast<uint32_t>(std::min(
                        static_cast<double>(num_docs),
                        static_cast<double>(want_hits) * mult));

                    int64_t threshold = histogram.estimate_threshold_for_count(k, ascending);
                    uint32_t actual = count_in_range(sorted, threshold, ascending);

                    EvalResult r;
                    r.dist_name = dd.name;
                    r.hit_ratio = hit_ratio;
                    r.multiplier = mult;
                    r.ascending = ascending;
                    r.want_hits = want_hits;
                    r.k = k;
                    r.threshold = threshold;
                    r.actual_covered = actual;
                    r.coverage_ratio = (want_hits > 0) ?
                        static_cast<double>(actual) / want_hits : 0.0;
                    r.under_covered = actual < want_hits;
                    r.docs_saved = (actual < num_docs) ? (num_docs - actual) : 0;
                    r.reduction_pct = 100.0 * r.docs_saved / num_docs;
                    results.push_back(r);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Print results: grouped by distribution, multiplier=2.0 (default), ascending
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "  COVERAGE ACCURACY (multiplier=2.0, ascending, max_hits=" << max_hits << ")\n";
    std::cout << "  want_hits = max_hits / hit_ratio, k = 2 * want_hits\n";
    std::cout << "  coverage_ratio = actual_covered / want_hits  (ideal = 1.0~2.0)\n";
    std::cout << "  under_covered = actual < want_hits  (BAD - results may be missing)\n";
    std::cout << "================================================================================\n\n";

    const int w = 12;
    std::cout << std::left << std::setw(w) << "Distrib"
              << std::right
              << std::setw(w) << "hit_ratio"
              << std::setw(w) << "want_hits"
              << std::setw(w) << "k(2x)"
              << std::setw(w) << "threshold"
              << std::setw(w) << "actual"
              << std::setw(w) << "cov_ratio"
              << std::setw(w) << "reduced%"
              << std::setw(w) << "under?"
              << "\n";
    std::cout << std::string(w * 9, '-') << "\n";

    for (auto &r : results) {
        if (r.multiplier != 2.0 || !r.ascending) continue;
        std::cout << std::left << std::setw(w) << r.dist_name
                  << std::right << std::fixed
                  << std::setw(w) << std::setprecision(2) << r.hit_ratio
                  << std::setw(w) << r.want_hits
                  << std::setw(w) << r.k
                  << std::setw(w) << r.threshold
                  << std::setw(w) << r.actual_covered
                  << std::setw(w) << std::setprecision(2) << r.coverage_ratio
                  << std::setw(w) << std::setprecision(1) << r.reduction_pct
                  << std::setw(w) << (r.under_covered ? "YES" : "no")
                  << "\n";
    }

    // ---------------------------------------------------------------------------
    // Summary: under-coverage rates by multiplier
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "  UNDER-COVERAGE RATE BY MULTIPLIER (across all distributions, hit_ratios)\n";
    std::cout << "  Under-coverage = histogram range covers fewer docs than want_hits\n";
    std::cout << "================================================================================\n\n";

    for (double mult : multipliers) {
        uint32_t total = 0, under = 0;
        double max_deficit = 0;
        for (auto &r : results) {
            if (r.multiplier != mult) continue;
            total++;
            if (r.under_covered) {
                under++;
                double deficit = 1.0 - r.coverage_ratio;
                if (deficit > max_deficit) max_deficit = deficit;
            }
        }
        std::cout << "  multiplier=" << std::fixed << std::setprecision(1) << mult
                  << "  under_rate=" << std::setprecision(1) << (100.0 * under / total) << "%"
                  << "  (" << under << "/" << total << ")"
                  << "  max_deficit=" << std::setprecision(1) << (max_deficit * 100) << "%"
                  << "\n";
    }

    // ---------------------------------------------------------------------------
    // Key insight: reduction% at different hit_ratios (with mult=2.0)
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "  DOCS REDUCTION BY HIT_RATIO (multiplier=2.0, ascending)\n";
    std::cout << "  Shows how much of the corpus is skipped by the histogram range bound\n";
    std::cout << "  Zhang Jie: \"hitratio 30%+ 收益巨大\"\n";
    std::cout << "================================================================================\n\n";

    std::cout << std::left << std::setw(15) << "hit_ratio";
    for (auto &dd : distributions) {
        std::cout << std::right << std::setw(15) << dd.name;
    }
    std::cout << "\n" << std::string(15 + 15 * distributions.size(), '-') << "\n";

    for (double hr : hit_ratios) {
        std::cout << std::left << std::setw(15) << std::fixed << std::setprecision(2) << hr;
        for (auto &dd : distributions) {
            for (auto &r : results) {
                if (r.dist_name == dd.name && r.hit_ratio == hr
                    && r.multiplier == 2.0 && r.ascending) {
                    std::cout << std::right << std::setw(14) << std::setprecision(1)
                              << r.reduction_pct << "%";
                    break;
                }
            }
        }
        std::cout << "\n";
    }

    // ---------------------------------------------------------------------------
    // Comparison: count-based vs histogram-based coverage accuracy
    // For count-based, the coverage is exact by definition (it counts
    // posting list entries). The issue is that for skewed distributions,
    // count-based covers different VALUE ranges for the same N.
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "  COUNT-BASED vs HISTOGRAM-BASED: VALUE RANGE COVERAGE\n";
    std::cout << "  count-based: covers exactly N docs but unknown value range\n";
    std::cout << "  histogram: covers ~N docs with known value bound (more predictable)\n";
    std::cout << "================================================================================\n\n";

    std::cout << std::left << std::setw(w) << "Distrib"
              << std::right
              << std::setw(w) << "hit_ratio"
              << std::setw(w) << "want_hits"
              << std::setw(w) << "hist_cover"
              << std::setw(w) << "hist_ratio"
              << std::setw(w) << "benefit"
              << "\n";
    std::cout << std::string(w * 6, '-') << "\n";

    for (auto &r : results) {
        if (r.multiplier != 2.0 || !r.ascending) continue;
        if (r.hit_ratio < 0.1) continue;  // Only show where limiter activates

        // Benefit: how many fewer docs to evaluate vs no limiting
        double no_limit_docs = num_docs * r.hit_ratio; // docs query would touch
        double with_limit_docs = std::min(static_cast<double>(r.actual_covered),
                                          no_limit_docs);
        double benefit = (no_limit_docs > 0) ?
            (no_limit_docs - with_limit_docs) / no_limit_docs * 100.0 : 0.0;

        std::cout << std::left << std::setw(w) << r.dist_name
                  << std::right << std::fixed
                  << std::setw(w) << std::setprecision(2) << r.hit_ratio
                  << std::setw(w) << r.want_hits
                  << std::setw(w) << r.actual_covered
                  << std::setw(w) << std::setprecision(2) << r.coverage_ratio
                  << std::setw(w) << std::setprecision(1) << benefit << "%"
                  << "\n";
    }
}

int main(int, char **) {
    run_eval(1000000,  0, 9999);   // 1M docs, 10K value range
    run_eval(5000000,  0, 99999);  // 5M docs, 100K value range
    return 0;
}
