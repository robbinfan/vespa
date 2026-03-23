// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

/**
 * Benchmark comparing range query hit estimation methods:
 *
 * 1. Uniform estimation (original calculateApproxNumHits) — assumes uniform value distribution
 * 2. Sampling estimation (new sampledHits) — samples posting list sizes at regular intervals
 * 3. Histogram estimation — maintains equi-width histogram buckets for O(1) estimation
 * 4. Exact count (countHits) — ground truth baseline
 *
 * Tests across multiple data distributions:
 *   - Uniform: equal probability for each value
 *   - Zipf (skew=1.0): power-law, heavy concentration on low values
 *   - Normal (Gaussian): bell curve centered at midpoint
 *   - Bimodal: two peaks with a valley in between
 *   - Exponential: high concentration at low values, long tail
 *
 * Also measures estimation latency and compares BKD-tree-style block counting.
 */

#include <vespa/searchlib/attribute/attributefactory.h>
#include <vespa/searchlib/attribute/integerbase.h>
#include <vespa/searchlib/query/query_term_simple.h>
#include <vespa/searchlib/queryeval/executeinfo.h>
#include <vespa/searchcommon/attribute/config.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace search;
using namespace search::attribute;

using AttributePtr = AttributeVector::SP;
using SearchContextPtr = std::unique_ptr<AttributeVector::SearchContext>;

// ---------------------------------------------------------------------------
// Data distribution generators
// ---------------------------------------------------------------------------

std::vector<int64_t> generate_uniform(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> dist(lo, hi);
    std::vector<int64_t> v(n);
    for (auto &x : v) x = dist(rng);
    return v;
}

std::vector<int64_t> generate_zipf(size_t n, int64_t lo, int64_t hi, double skew, uint32_t seed) {
    std::mt19937_64 rng(seed);
    int64_t range = hi - lo + 1;
    // Pre-compute cumulative distribution
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

std::vector<int64_t> generate_normal(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
    std::mt19937_64 rng(seed);
    double mean = (lo + hi) / 2.0;
    double stddev = (hi - lo) / 6.0;  // 99.7% within range
    std::normal_distribution<double> dist(mean, stddev);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        double val = dist(rng);
        x = std::max(lo, std::min(hi, static_cast<int64_t>(std::round(val))));
    }
    return v;
}

std::vector<int64_t> generate_bimodal(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
    std::mt19937_64 rng(seed);
    double range = hi - lo;
    double mean1 = lo + range * 0.25;
    double mean2 = lo + range * 0.75;
    double stddev = range / 12.0;
    std::normal_distribution<double> dist1(mean1, stddev);
    std::normal_distribution<double> dist2(mean2, stddev);
    std::bernoulli_distribution coin(0.5);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        double val = coin(rng) ? dist1(rng) : dist2(rng);
        x = std::max(lo, std::min(hi, static_cast<int64_t>(std::round(val))));
    }
    return v;
}

std::vector<int64_t> generate_exponential(size_t n, int64_t lo, int64_t hi, uint32_t seed) {
    std::mt19937_64 rng(seed);
    double lambda = 5.0 / (hi - lo);  // scale so ~99% falls within range
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
// Histogram estimator (equi-width)
// ---------------------------------------------------------------------------

class HistogramEstimator {
    std::vector<uint32_t> _buckets;
    int64_t _lo;
    int64_t _hi;
    uint32_t _num_buckets;
    double _bucket_width;
    uint32_t _total_docs;
public:
    HistogramEstimator(int64_t lo, int64_t hi, uint32_t num_buckets)
        : _buckets(num_buckets, 0), _lo(lo), _hi(hi),
          _num_buckets(num_buckets),
          _bucket_width(static_cast<double>(hi - lo + 1) / num_buckets),
          _total_docs(0)
    {}

    void add(int64_t value) {
        uint32_t idx = static_cast<uint32_t>((value - _lo) / _bucket_width);
        if (idx >= _num_buckets) idx = _num_buckets - 1;
        _buckets[idx]++;
        _total_docs++;
    }

    uint32_t estimate(int64_t query_lo, int64_t query_hi) const {
        if (query_lo > _hi || query_hi < _lo) return 0;
        query_lo = std::max(query_lo, _lo);
        query_hi = std::min(query_hi, _hi);

        double sum = 0;
        for (uint32_t i = 0; i < _num_buckets; ++i) {
            double bkt_lo = _lo + i * _bucket_width;
            double bkt_hi = bkt_lo + _bucket_width;
            // Fraction of this bucket that overlaps the query range
            double overlap_lo = std::max(bkt_lo, static_cast<double>(query_lo));
            double overlap_hi = std::min(bkt_hi, static_cast<double>(query_hi + 1));
            if (overlap_hi > overlap_lo) {
                double frac = (overlap_hi - overlap_lo) / _bucket_width;
                sum += frac * _buckets[i];
            }
        }
        return static_cast<uint32_t>(sum);
    }

    uint32_t total_docs() const { return _total_docs; }
    uint32_t memory_bytes() const { return _num_buckets * sizeof(uint32_t); }
};

// ---------------------------------------------------------------------------
// BKD-tree-style block estimator (simulates BKD leaf block counting)
// ---------------------------------------------------------------------------

class BKDEstimator {
    // Simulates BKD-tree: sorted values split into fixed-size leaf blocks
    // Each block stores (min_value, max_value, doc_count)
    struct Block {
        int64_t min_val;
        int64_t max_val;
        uint32_t count;
    };
    std::vector<Block> _blocks;
    uint32_t _total_docs;
    uint32_t _block_size;

public:
    BKDEstimator(uint32_t block_size) : _block_size(block_size), _total_docs(0) {}

    void build(std::vector<int64_t> sorted_values) {
        _total_docs = sorted_values.size();
        _blocks.clear();
        for (size_t i = 0; i < sorted_values.size(); i += _block_size) {
            size_t end = std::min(i + _block_size, sorted_values.size());
            Block b;
            b.min_val = sorted_values[i];
            b.max_val = sorted_values[end - 1];
            b.count = end - i;
            _blocks.push_back(b);
        }
    }

    uint32_t estimate(int64_t query_lo, int64_t query_hi) const {
        uint32_t sum = 0;
        for (auto &b : _blocks) {
            if (b.max_val < query_lo || b.min_val > query_hi) {
                continue;  // no overlap
            }
            if (b.min_val >= query_lo && b.max_val <= query_hi) {
                sum += b.count;  // fully contained
            } else {
                // Partial overlap — assume uniform within block
                double range = b.max_val - b.min_val + 1;
                if (range <= 0) range = 1;
                double overlap_lo = std::max(static_cast<double>(query_lo), static_cast<double>(b.min_val));
                double overlap_hi = std::min(static_cast<double>(query_hi), static_cast<double>(b.max_val));
                double frac = (overlap_hi - overlap_lo + 1) / range;
                sum += static_cast<uint32_t>(frac * b.count);
            }
        }
        return sum;
    }

    uint32_t num_blocks() const { return _blocks.size(); }
    uint32_t memory_bytes() const { return _blocks.size() * sizeof(Block); }
};

// ---------------------------------------------------------------------------
// Utility: create attribute, populate, get estimation via Vespa's own path
// ---------------------------------------------------------------------------

struct VespaEstimation {
    uint32_t approx_hits;       // What approximateHits() returns (new sampling)
    uint32_t actual_hits;       // Ground truth from iterator
    double   approx_time_us;
    double   actual_time_us;
};

VespaEstimation vespa_estimate(AttributeVector &vec, const std::string &range_term) {
    std::vector<char> query;
    // Build a simple term query
    uint32_t term_len = range_term.size();
    std::string name = vec.getName();
    uint32_t name_len = name.size();
    // Manual query building matching the test pattern
    query.resize(1 + 2 * sizeof(uint32_t) + name_len + term_len);
    char *p = query.data();
    *p++ = ParseItem::ITEM_TERM;
    // index name length (big-endian compressed)
    p += vespalib::compress::Integer::compress(name_len, p);
    memcpy(p, name.data(), name_len);
    p += name_len;
    // term length
    p += vespalib::compress::Integer::compress(term_len, p);
    memcpy(p, range_term.data(), term_len);
    p += term_len;
    query.resize(p - query.data());

    auto sc = vec.getSearch(vespalib::stringref(query.data(), query.size()),
                            SearchContextParams());

    auto t0 = std::chrono::high_resolution_clock::now();
    uint32_t approx = sc->approximateHits();
    auto t1 = std::chrono::high_resolution_clock::now();

    sc->fetchPostings(queryeval::ExecuteInfo::TRUE);
    fef::TermFieldMatchData tfmd;
    auto it = sc->createIterator(&tfmd, true);
    it->initFullRange();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint32_t actual = 0;
    for (it->seek(1u); !it->isAtEnd(); it->seek(it->getDocId() + 1)) {
        actual++;
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    VespaEstimation res;
    res.approx_hits = approx;
    res.actual_hits = actual;
    res.approx_time_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    res.actual_time_us = std::chrono::duration<double, std::micro>(t3 - t2).count();
    return res;
}

// Compute uniform estimate the old way
uint32_t uniform_estimate(uint32_t doc_id_limit, uint32_t dict_size, uint32_t unique_values_in_range) {
    if (dict_size == 0) return 0;
    float docs_per_unique = static_cast<float>(doc_id_limit) / static_cast<float>(dict_size);
    return static_cast<uint32_t>(docs_per_unique * unique_values_in_range);
}

// ---------------------------------------------------------------------------
// Main benchmark
// ---------------------------------------------------------------------------

struct RangeQuery {
    int64_t lo;
    int64_t hi;
    std::string label;
};

struct DistResult {
    std::string dist_name;
    std::string range_label;
    uint32_t actual_hits;
    uint32_t uniform_est;
    uint32_t sampling_est;   // Vespa's new approximateHits
    uint32_t histogram_est;
    uint32_t bkd_est;
    double   uniform_err_pct;
    double   sampling_err_pct;
    double   histogram_err_pct;
    double   bkd_err_pct;
    double   vespa_approx_us;
    double   histogram_us;
    double   bkd_us;
};

double error_pct(uint32_t est, uint32_t actual) {
    if (actual == 0) return (est == 0) ? 0.0 : 100.0;
    return 100.0 * std::abs(static_cast<double>(est) - actual) / actual;
}

void run_benchmark(uint32_t num_docs, int64_t value_lo, int64_t value_hi,
                   uint32_t histogram_buckets, uint32_t bkd_block_size) {
    using Clock = std::chrono::high_resolution_clock;

    std::cout << "\n================================================================\n";
    std::cout << "  RANGE ESTIMATION BENCHMARK\n";
    std::cout << "  Documents:   " << num_docs << "\n";
    std::cout << "  Value range: [" << value_lo << ", " << value_hi << "]\n";
    std::cout << "  Histogram:   " << histogram_buckets << " buckets ("
              << (histogram_buckets * 4) << " bytes)\n";
    std::cout << "  BKD block:   " << bkd_block_size << " docs/block\n";
    std::cout << "================================================================\n";

    struct DistConfig {
        std::string name;
        std::vector<int64_t> (*gen)(size_t, int64_t, int64_t, uint32_t);
    };

    // Define distributions (wrappers to unify signature)
    auto gen_uniform_wrap = [](size_t n, int64_t lo, int64_t hi, uint32_t seed) {
        return generate_uniform(n, lo, hi, seed);
    };
    auto gen_normal_wrap = [](size_t n, int64_t lo, int64_t hi, uint32_t seed) {
        return generate_normal(n, lo, hi, seed);
    };
    auto gen_bimodal_wrap = [](size_t n, int64_t lo, int64_t hi, uint32_t seed) {
        return generate_bimodal(n, lo, hi, seed);
    };
    auto gen_exp_wrap = [](size_t n, int64_t lo, int64_t hi, uint32_t seed) {
        return generate_exponential(n, lo, hi, seed);
    };

    // Generate data for each distribution
    struct DistData {
        std::string name;
        std::vector<int64_t> values;
    };

    std::vector<DistData> distributions;
    distributions.push_back({"Uniform",     generate_uniform(num_docs, value_lo, value_hi, 42)});
    distributions.push_back({"Zipf(1.0)",   generate_zipf(num_docs, value_lo, value_hi, 1.0, 42)});
    distributions.push_back({"Normal",      generate_normal(num_docs, value_lo, value_hi, 42)});
    distributions.push_back({"Bimodal",     generate_bimodal(num_docs, value_lo, value_hi, 42)});
    distributions.push_back({"Exponential", generate_exponential(num_docs, value_lo, value_hi, 42)});

    // Define range queries as fractions of the total value range
    int64_t range_size = value_hi - value_lo;
    std::vector<RangeQuery> queries = {
        {value_lo, value_lo + range_size / 100,  "1% range (low)"},
        {value_lo + range_size / 2 - range_size / 200,
         value_lo + range_size / 2 + range_size / 200, "1% range (mid)"},
        {value_hi - range_size / 100, value_hi,  "1% range (high)"},
        {value_lo, value_lo + range_size / 10,   "10% range (low)"},
        {value_lo + range_size * 45 / 100,
         value_lo + range_size * 55 / 100,       "10% range (mid)"},
        {value_hi - range_size / 10, value_hi,   "10% range (high)"},
        {value_lo, value_lo + range_size / 2,    "50% range (low half)"},
    };

    std::vector<DistResult> all_results;

    for (auto &dd : distributions) {
        std::cout << "\n--- Distribution: " << dd.name << " ---\n";

        // 1. Create Vespa attribute
        Config cfg(BasicType::INT64, CollectionType::SINGLE);
        cfg.setFastSearch(true);
        std::string attr_name = "bench_" + dd.name;
        AttributePtr attr = AttributeFactory::createAttribute(attr_name, cfg);
        attr->addReservedDoc();
        for (uint32_t i = 0; i < num_docs; ++i) {
            AttributeVector::DocId docId;
            attr->addDoc(docId);
        }
        auto *int_attr = dynamic_cast<IntegerAttribute *>(attr.get());
        assert(int_attr);
        for (uint32_t i = 0; i < num_docs; ++i) {
            int_attr->update(i + 1, dd.values[i]);
        }
        attr->commit(true);

        // 2. Build histogram and BKD estimator
        HistogramEstimator histogram(value_lo, value_hi, histogram_buckets);
        for (auto v : dd.values) {
            histogram.add(v);
        }

        BKDEstimator bkd(bkd_block_size);
        auto sorted = dd.values;
        std::sort(sorted.begin(), sorted.end());
        bkd.build(std::move(sorted));

        // 3. Count unique values for uniform estimator
        std::map<int64_t, uint32_t> value_counts;
        for (auto v : dd.values) {
            value_counts[v]++;
        }
        uint32_t dict_size = value_counts.size();

        // 4. Run queries
        for (auto &q : queries) {
            // Vespa estimation (includes new sampling approach)
            std::ostringstream range_ss;
            range_ss << "[" << q.lo << ";" << q.hi << "]";
            auto vespa_res = vespa_estimate(*attr, range_ss.str());

            // Uniform estimate (old approach)
            uint32_t unique_in_range = 0;
            for (auto it = value_counts.lower_bound(q.lo);
                 it != value_counts.end() && it->first <= q.hi; ++it) {
                unique_in_range++;
            }
            uint32_t uni_est = uniform_estimate(num_docs + 1, dict_size, unique_in_range);

            // Histogram estimate
            auto ht0 = Clock::now();
            uint32_t hist_est = histogram.estimate(q.lo, q.hi);
            auto ht1 = Clock::now();
            double hist_us = std::chrono::duration<double, std::micro>(ht1 - ht0).count();

            // BKD estimate
            auto bt0 = Clock::now();
            uint32_t bkd_e = bkd.estimate(q.lo, q.hi);
            auto bt1 = Clock::now();
            double bkd_us = std::chrono::duration<double, std::micro>(bt1 - bt0).count();

            DistResult r;
            r.dist_name = dd.name;
            r.range_label = q.label;
            r.actual_hits = vespa_res.actual_hits;
            r.uniform_est = uni_est;
            r.sampling_est = vespa_res.approx_hits;
            r.histogram_est = hist_est;
            r.bkd_est = bkd_e;
            r.uniform_err_pct = error_pct(uni_est, vespa_res.actual_hits);
            r.sampling_err_pct = error_pct(vespa_res.approx_hits, vespa_res.actual_hits);
            r.histogram_err_pct = error_pct(hist_est, vespa_res.actual_hits);
            r.bkd_err_pct = error_pct(bkd_e, vespa_res.actual_hits);
            r.vespa_approx_us = vespa_res.approx_time_us;
            r.histogram_us = hist_us;
            r.bkd_us = bkd_us;
            all_results.push_back(r);
        }
    }

    // ---------------------------------------------------------------------------
    // Print results table
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "                          ESTIMATION ACCURACY RESULTS\n";
    std::cout << "================================================================================\n\n";

    // Column widths
    const int w_dist = 13, w_range = 22, w_actual = 10, w_est = 10, w_err = 8;

    auto print_header = [&]() {
        std::cout << std::left
                  << std::setw(w_dist) << "Distrib"
                  << std::setw(w_range) << "Range"
                  << std::right
                  << std::setw(w_actual) << "Actual"
                  << std::setw(w_est) << "Uniform"
                  << std::setw(w_err) << "Err%"
                  << std::setw(w_est) << "Sampling"
                  << std::setw(w_err) << "Err%"
                  << std::setw(w_est) << "Histgrm"
                  << std::setw(w_err) << "Err%"
                  << std::setw(w_est) << "BKD"
                  << std::setw(w_err) << "Err%"
                  << "\n";
        std::cout << std::string(w_dist + w_range + w_actual + 4 * (w_est + w_err), '-') << "\n";
    };

    print_header();
    for (auto &r : all_results) {
        std::cout << std::left
                  << std::setw(w_dist) << r.dist_name
                  << std::setw(w_range) << r.range_label
                  << std::right << std::fixed
                  << std::setw(w_actual) << r.actual_hits
                  << std::setw(w_est) << r.uniform_est
                  << std::setw(w_err) << std::setprecision(1) << r.uniform_err_pct
                  << std::setw(w_est) << r.sampling_est
                  << std::setw(w_err) << std::setprecision(1) << r.sampling_err_pct
                  << std::setw(w_est) << r.histogram_est
                  << std::setw(w_err) << std::setprecision(1) << r.histogram_err_pct
                  << std::setw(w_est) << r.bkd_est
                  << std::setw(w_err) << std::setprecision(1) << r.bkd_err_pct
                  << "\n";
    }

    // ---------------------------------------------------------------------------
    // Aggregate error per method per distribution
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "                     MEAN ABSOLUTE ERROR % BY DISTRIBUTION\n";
    std::cout << "================================================================================\n\n";

    std::map<std::string, std::vector<double>> errs_uniform, errs_sampling, errs_histogram, errs_bkd;
    for (auto &r : all_results) {
        errs_uniform[r.dist_name].push_back(r.uniform_err_pct);
        errs_sampling[r.dist_name].push_back(r.sampling_err_pct);
        errs_histogram[r.dist_name].push_back(r.histogram_err_pct);
        errs_bkd[r.dist_name].push_back(r.bkd_err_pct);
    }

    auto mean = [](const std::vector<double> &v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };

    std::cout << std::left << std::setw(15) << "Distribution"
              << std::right << std::setw(12) << "Uniform"
              << std::setw(12) << "Sampling"
              << std::setw(12) << "Histogram"
              << std::setw(12) << "BKD"
              << "\n";
    std::cout << std::string(63, '-') << "\n";

    for (auto &[name, _] : errs_uniform) {
        std::cout << std::left << std::setw(15) << name
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(11) << mean(errs_uniform[name]) << "%"
                  << std::setw(11) << mean(errs_sampling[name]) << "%"
                  << std::setw(11) << mean(errs_histogram[name]) << "%"
                  << std::setw(11) << mean(errs_bkd[name]) << "%"
                  << "\n";
    }

    // Global mean
    double g_uniform = 0, g_sampling = 0, g_histogram = 0, g_bkd = 0;
    for (auto &r : all_results) {
        g_uniform += r.uniform_err_pct;
        g_sampling += r.sampling_err_pct;
        g_histogram += r.histogram_err_pct;
        g_bkd += r.bkd_err_pct;
    }
    size_t n = all_results.size();
    std::cout << std::string(63, '-') << "\n";
    std::cout << std::left << std::setw(15) << "GLOBAL MEAN"
              << std::right << std::fixed << std::setprecision(1)
              << std::setw(11) << g_uniform / n << "%"
              << std::setw(11) << g_sampling / n << "%"
              << std::setw(11) << g_histogram / n << "%"
              << std::setw(11) << g_bkd / n << "%"
              << "\n";

    // ---------------------------------------------------------------------------
    // Latency comparison
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "                         ESTIMATION LATENCY (microseconds)\n";
    std::cout << "================================================================================\n\n";

    double sum_vespa_us = 0, sum_hist_us = 0, sum_bkd_us = 0;
    for (auto &r : all_results) {
        sum_vespa_us += r.vespa_approx_us;
        sum_hist_us += r.histogram_us;
        sum_bkd_us += r.bkd_us;
    }
    std::cout << "Vespa sampling avg: " << std::fixed << std::setprecision(1)
              << sum_vespa_us / n << " us\n";
    std::cout << "Histogram avg:      " << std::fixed << std::setprecision(1)
              << sum_hist_us / n << " us\n";
    std::cout << "BKD avg:            " << std::fixed << std::setprecision(1)
              << sum_bkd_us / n << " us\n";

    // ---------------------------------------------------------------------------
    // Memory overhead comparison
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "                           MEMORY OVERHEAD COMPARISON\n";
    std::cout << "================================================================================\n\n";

    std::cout << "Method              Extra Memory       Notes\n";
    std::cout << "-----------------------------------------------------------\n";
    std::cout << "Uniform est.        0 bytes            O(1), uses existing dict stats\n";
    std::cout << "Sampling est.       0 bytes            Iterates dict at query time\n";
    std::cout << "Histogram (256)     " << std::setw(6) << 256 * 4
              << " bytes     Per-attribute, equi-width\n";
    std::cout << "Histogram (1024)    " << std::setw(6) << 1024 * 4
              << " bytes     Per-attribute, equi-width\n";
    std::cout << "BKD-tree style      ~" << std::setw(5)
              << (num_docs / bkd_block_size) * 16
              << " bytes     Sort-based block index\n";
    std::cout << "B-tree postings     existing           Already maintained by Vespa\n";

    // ---------------------------------------------------------------------------
    // ANN filter strategy impact analysis
    // ---------------------------------------------------------------------------

    std::cout << "\n\n";
    std::cout << "================================================================================\n";
    std::cout << "              ANN FILTER STRATEGY IMPACT ANALYSIS\n";
    std::cout << "  (brute_force_limit = 0.05: if hit_ratio < 5%, switch to brute force)\n";
    std::cout << "================================================================================\n\n";

    double brute_force_limit = 0.05;

    std::cout << std::left << std::setw(13) << "Distrib"
              << std::setw(22) << "Range"
              << std::setw(12) << "True Ratio"
              << std::setw(12) << "Uni Ratio"
              << std::setw(12) << "Samp Ratio"
              << std::setw(15) << "Uni Strategy"
              << std::setw(15) << "Samp Strategy"
              << std::setw(15) << "Correct?"
              << "\n";
    std::cout << std::string(114, '-') << "\n";

    uint32_t wrong_uniform = 0, wrong_sampling = 0;
    for (auto &r : all_results) {
        double true_ratio = static_cast<double>(r.actual_hits) / num_docs;
        double uni_ratio = static_cast<double>(r.uniform_est) / num_docs;
        double samp_ratio = static_cast<double>(r.sampling_est) / num_docs;

        std::string true_strategy = (true_ratio < brute_force_limit) ? "BRUTE_FORCE" : "HNSW_APPROX";
        std::string uni_strategy = (uni_ratio < brute_force_limit) ? "BRUTE_FORCE" : "HNSW_APPROX";
        std::string samp_strategy = (samp_ratio < brute_force_limit) ? "BRUTE_FORCE" : "HNSW_APPROX";

        bool uni_correct = (uni_strategy == true_strategy);
        bool samp_correct = (samp_strategy == true_strategy);
        if (!uni_correct) wrong_uniform++;
        if (!samp_correct) wrong_sampling++;

        std::string correctness;
        if (uni_correct && samp_correct) correctness = "Both OK";
        else if (!uni_correct && samp_correct) correctness = "Samp WINS";
        else if (uni_correct && !samp_correct) correctness = "Uni WINS";
        else correctness = "Both WRONG";

        std::cout << std::left
                  << std::setw(13) << r.dist_name
                  << std::setw(22) << r.range_label
                  << std::right << std::fixed << std::setprecision(4)
                  << std::setw(12) << true_ratio
                  << std::setw(12) << uni_ratio
                  << std::setw(12) << samp_ratio
                  << "  " << std::left << std::setw(13) << uni_strategy
                  << "  " << std::setw(13) << samp_strategy
                  << "  " << correctness
                  << "\n";
    }

    std::cout << "\nWrong strategy decisions: Uniform=" << wrong_uniform
              << "  Sampling=" << wrong_sampling
              << "  (out of " << all_results.size() << " queries)\n";
}

// ---------------------------------------------------------------------------

int main(int, char **) {
    std::cout << "======================================================\n";
    std::cout << "  Range Query Estimation Benchmark\n";
    std::cout << "  Comparing: Uniform | Sampling | Histogram | BKD\n";
    std::cout << "======================================================\n";

    // Scale 1: 1M docs, value range 0-9999
    run_benchmark(1000000, 0, 9999, 256, 512);

    // Scale 2: 5M docs, value range 0-99999
    run_benchmark(5000000, 0, 99999, 1024, 1024);

    return 0;
}
