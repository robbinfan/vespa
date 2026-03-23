/**
 * Standalone Range Query Estimation Benchmark
 * ============================================
 *
 * Compares four estimation methods for range queries:
 *   1. Uniform  — Vespa's original: (totalDocs / dictSize) * uniqueValuesInRange
 *   2. Sampling — New approach: sample 16 posting list sizes, extrapolate
 *   3. Histogram — Equi-width histogram (like DBMS statistics)
 *   4. BKD-style — Block-based estimation mimicking Elasticsearch's BKD-tree
 *
 * Simulates Vespa's posting list structure (sorted dictionary + per-value
 * doc counts) to faithfully reproduce the estimation paths without requiring
 * a full Vespa build.
 *
 * Compile: g++ -std=c++17 -O2 -o range_bench range_estimation_standalone_bench.cpp
 * Run:     ./range_bench
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Simulated posting list dictionary (mirrors Vespa's EnumPostingTree)
// ============================================================================

struct PostingEntry {
    int64_t  value;      // unique value
    uint32_t doc_count;  // number of documents with this value (posting list size)
};

struct SimulatedDictionary {
    std::vector<PostingEntry> entries;  // sorted by value
    uint32_t total_docs;

    void build(const std::vector<int64_t> &data) {
        total_docs = data.size();
        std::map<int64_t, uint32_t> counts;
        for (auto v : data) counts[v]++;
        entries.clear();
        entries.reserve(counts.size());
        for (auto &[val, cnt] : counts) {
            entries.push_back({val, cnt});
        }
    }

    uint32_t dict_size() const { return entries.size(); }

    // Find range [lo, hi] in dictionary, return iterator positions
    std::pair<uint32_t, uint32_t> lookup_range(int64_t lo, int64_t hi) const {
        auto lower = std::lower_bound(entries.begin(), entries.end(), lo,
            [](const PostingEntry &e, int64_t v) { return e.value < v; });
        auto upper = std::upper_bound(entries.begin(), entries.end(), hi,
            [](int64_t v, const PostingEntry &e) { return v < e.value; });
        return {std::distance(entries.begin(), lower),
                std::distance(entries.begin(), upper)};
    }
};

// ============================================================================
// Estimation Method 1: Uniform (Vespa original)
//   Formula: (totalDocs / dictSize) * uniqueValuesInRange
// ============================================================================

uint32_t estimate_uniform(const SimulatedDictionary &dict, int64_t lo, int64_t hi) {
    auto [begin, end] = dict.lookup_range(lo, hi);
    uint32_t unique_in_range = end - begin;
    if (unique_in_range == 0) return 0;
    float docs_per_unique = static_cast<float>(dict.total_docs) /
                            static_cast<float>(dict.dict_size());
    return static_cast<uint32_t>(docs_per_unique * unique_in_range);
}

// ============================================================================
// Estimation Method 2: Sampling (New Vespa approach)
//   Sample NUM_SAMPLES posting list sizes at regular intervals, extrapolate
// ============================================================================

uint32_t estimate_sampling(const SimulatedDictionary &dict, int64_t lo, int64_t hi,
                           uint32_t num_samples = 16) {
    auto [begin, end] = dict.lookup_range(lo, hi);
    uint32_t unique_in_range = end - begin;
    if (unique_in_range == 0) return 0;
    if (unique_in_range == 1) return dict.entries[begin].doc_count;

    uint32_t step = unique_in_range / num_samples;
    if (step == 0) step = 1;

    uint32_t samples_taken = 0;
    size_t sampled_sum = 0;
    uint32_t next_sample = 0;
    for (uint32_t i = begin; i < end && samples_taken < num_samples; ++i) {
        uint32_t pos = i - begin;
        if (pos == next_sample) {
            sampled_sum += dict.entries[i].doc_count;
            ++samples_taken;
            next_sample += step;
        }
    }
    if (samples_taken == 0) return 0;
    double avg = static_cast<double>(sampled_sum) / samples_taken;
    return static_cast<uint32_t>(avg * unique_in_range);
}

// ============================================================================
// Estimation Method 3: Equi-width Histogram (DBMS-style statistics)
// ============================================================================

class HistogramEstimator {
    std::vector<uint32_t> _buckets;
    int64_t  _lo, _hi;
    uint32_t _num_buckets;
    double   _bucket_width;
public:
    HistogramEstimator(int64_t lo, int64_t hi, uint32_t num_buckets)
        : _buckets(num_buckets, 0), _lo(lo), _hi(hi),
          _num_buckets(num_buckets),
          _bucket_width(static_cast<double>(hi - lo + 1) / num_buckets)
    {}

    void add(int64_t value) {
        uint32_t idx = static_cast<uint32_t>((value - _lo) / _bucket_width);
        if (idx >= _num_buckets) idx = _num_buckets - 1;
        _buckets[idx]++;
    }

    uint32_t estimate(int64_t q_lo, int64_t q_hi) const {
        if (q_lo > _hi || q_hi < _lo) return 0;
        q_lo = std::max(q_lo, _lo);
        q_hi = std::min(q_hi, _hi);
        double sum = 0;
        for (uint32_t i = 0; i < _num_buckets; ++i) {
            double bkt_lo = _lo + i * _bucket_width;
            double bkt_hi = bkt_lo + _bucket_width;
            double overlap_lo = std::max(bkt_lo, static_cast<double>(q_lo));
            double overlap_hi = std::min(bkt_hi, static_cast<double>(q_hi + 1));
            if (overlap_hi > overlap_lo) {
                double frac = (overlap_hi - overlap_lo) / _bucket_width;
                sum += frac * _buckets[i];
            }
        }
        return static_cast<uint32_t>(sum);
    }

    uint32_t memory_bytes() const { return _num_buckets * sizeof(uint32_t); }
};

// ============================================================================
// Estimation Method 4: BKD-tree-style block estimator
//   Simulates ES's BKD-tree: sorted values split into leaf blocks,
//   each block tracks (min_val, max_val, doc_count).
// ============================================================================

class BKDEstimator {
    struct Block {
        int64_t  min_val, max_val;
        uint32_t count;
    };
    std::vector<Block> _blocks;
    uint32_t _block_size;
public:
    BKDEstimator(uint32_t block_size) : _block_size(block_size) {}

    void build(std::vector<int64_t> sorted) {
        _blocks.clear();
        for (size_t i = 0; i < sorted.size(); i += _block_size) {
            size_t end = std::min(i + _block_size, sorted.size());
            _blocks.push_back({sorted[i], sorted[end - 1],
                              static_cast<uint32_t>(end - i)});
        }
    }

    uint32_t estimate(int64_t q_lo, int64_t q_hi) const {
        uint32_t sum = 0;
        for (auto &b : _blocks) {
            if (b.max_val < q_lo || b.min_val > q_hi) continue;
            if (b.min_val >= q_lo && b.max_val <= q_hi) {
                sum += b.count;
            } else {
                double range = std::max(1.0, static_cast<double>(b.max_val - b.min_val + 1));
                double ol = std::max(static_cast<double>(q_lo), static_cast<double>(b.min_val));
                double oh = std::min(static_cast<double>(q_hi), static_cast<double>(b.max_val));
                sum += static_cast<uint32_t>((oh - ol + 1) / range * b.count);
            }
        }
        return sum;
    }

    uint32_t num_blocks() const { return _blocks.size(); }
    uint32_t memory_bytes() const { return _blocks.size() * 16; } // 2x int64 + uint32 + pad
};

// ============================================================================
// Estimation Method 5: Exact count (ground truth)
// ============================================================================

uint32_t count_exact(const SimulatedDictionary &dict, int64_t lo, int64_t hi) {
    auto [begin, end] = dict.lookup_range(lo, hi);
    uint32_t sum = 0;
    for (uint32_t i = begin; i < end; ++i) {
        sum += dict.entries[i].doc_count;
    }
    return sum;
}

// ============================================================================
// Data distribution generators
// ============================================================================

std::vector<int64_t> gen_uniform(size_t n, int64_t lo, int64_t hi, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> d(lo, hi);
    std::vector<int64_t> v(n);
    for (auto &x : v) x = d(rng);
    return v;
}

std::vector<int64_t> gen_zipf(size_t n, int64_t lo, int64_t hi, double skew, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    int64_t range = hi - lo + 1;
    std::vector<double> cdf(range);
    double sum = 0;
    for (int64_t i = 0; i < range; ++i) {
        sum += 1.0 / std::pow(i + 1, skew);
        cdf[i] = sum;
    }
    for (auto &c : cdf) c /= sum;
    std::uniform_real_distribution<double> ud(0.0, 1.0);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        double u = ud(rng);
        x = lo + std::distance(cdf.begin(), std::lower_bound(cdf.begin(), cdf.end(), u));
        if (x > hi) x = hi;
    }
    return v;
}

std::vector<int64_t> gen_normal(size_t n, int64_t lo, int64_t hi, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    double mean = (lo + hi) / 2.0;
    double stddev = (hi - lo) / 6.0;
    std::normal_distribution<double> d(mean, stddev);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        x = std::max(lo, std::min(hi, static_cast<int64_t>(std::round(d(rng)))));
    }
    return v;
}

std::vector<int64_t> gen_bimodal(size_t n, int64_t lo, int64_t hi, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    double r = hi - lo;
    std::normal_distribution<double> d1(lo + r * 0.25, r / 12.0);
    std::normal_distribution<double> d2(lo + r * 0.75, r / 12.0);
    std::bernoulli_distribution coin(0.5);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        double val = coin(rng) ? d1(rng) : d2(rng);
        x = std::max(lo, std::min(hi, static_cast<int64_t>(std::round(val))));
    }
    return v;
}

std::vector<int64_t> gen_exponential(size_t n, int64_t lo, int64_t hi, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    double lambda = 5.0 / (hi - lo);
    std::exponential_distribution<double> d(lambda);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        x = lo + static_cast<int64_t>(d(rng));
        if (x > hi) x = lo + (x - lo) % (hi - lo + 1);
    }
    return v;
}

std::vector<int64_t> gen_lognormal(size_t n, int64_t lo, int64_t hi, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    double log_range = std::log(hi - lo + 1);
    std::lognormal_distribution<double> d(log_range * 0.5, log_range * 0.25);
    std::vector<int64_t> v(n);
    for (auto &x : v) {
        x = lo + static_cast<int64_t>(d(rng));
        if (x > hi) x = hi;
        if (x < lo) x = lo;
    }
    return v;
}

// ============================================================================
// Benchmark driver
// ============================================================================

struct RangeQuery {
    int64_t lo, hi;
    std::string label;
};

struct Result {
    std::string dist_name;
    std::string range_label;
    uint32_t actual;
    uint32_t uniform_est, sampling_est, hist_est, bkd_est;
    double uniform_err, sampling_err, hist_err, bkd_err;
    double uniform_us, sampling_us, hist_us, bkd_us, exact_us;
};

inline double err_pct(uint32_t est, uint32_t actual) {
    if (actual == 0) return est == 0 ? 0.0 : 999.9;
    return 100.0 * std::abs(static_cast<double>(est) - actual) / actual;
}

using Clock = std::chrono::high_resolution_clock;

template<typename F>
std::pair<uint32_t, double> timed(F &&f, int repeats = 100) {
    // Warm up
    uint32_t result = f();
    auto t0 = Clock::now();
    for (int i = 0; i < repeats; ++i) {
        result = f();
    }
    auto t1 = Clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / repeats;
    return {result, us};
}

void run_benchmark(uint32_t num_docs, int64_t value_lo, int64_t value_hi,
                   uint32_t hist_buckets, uint32_t bkd_block_size) {
    int64_t range = value_hi - value_lo;

    std::cout << "\n================================================================"
              << "================================================================\n";
    std::cout << "  BENCHMARK: " << num_docs << " docs, values in ["
              << value_lo << ", " << value_hi << "], "
              << hist_buckets << " histogram buckets, BKD block=" << bkd_block_size << "\n";
    std::cout << "================================================================"
              << "================================================================\n";

    struct DistConfig {
        std::string name;
        std::function<std::vector<int64_t>()> gen;
    };

    std::vector<DistConfig> dists = {
        {"Uniform",      [&]{ return gen_uniform(num_docs, value_lo, value_hi); }},
        {"Zipf(1.0)",    [&]{ return gen_zipf(num_docs, value_lo, value_hi, 1.0); }},
        {"Zipf(1.5)",    [&]{ return gen_zipf(num_docs, value_lo, value_hi, 1.5); }},
        {"Normal",       [&]{ return gen_normal(num_docs, value_lo, value_hi); }},
        {"Bimodal",      [&]{ return gen_bimodal(num_docs, value_lo, value_hi); }},
        {"Exponential",  [&]{ return gen_exponential(num_docs, value_lo, value_hi); }},
        {"LogNormal",    [&]{ return gen_lognormal(num_docs, value_lo, value_hi); }},
    };

    std::vector<RangeQuery> queries = {
        {value_lo,                              value_lo + range / 100,      "1% low"},
        {value_lo + range/2 - range/200,        value_lo + range/2 + range/200, "1% mid"},
        {value_hi - range / 100,                value_hi,                    "1% high"},
        {value_lo,                              value_lo + range / 20,       "5% low"},
        {value_lo + range*45/100,               value_lo + range*55/100,     "10% mid"},
        {value_hi - range / 10,                 value_hi,                    "10% high"},
        {value_lo,                              value_lo + range / 2,        "50% low-half"},
    };

    std::vector<Result> all_results;

    for (auto &dc : dists) {
        auto data = dc.gen();

        // Build structures
        SimulatedDictionary dict;
        dict.build(data);

        HistogramEstimator hist(value_lo, value_hi, hist_buckets);
        for (auto v : data) hist.add(v);

        BKDEstimator bkd(bkd_block_size);
        auto sorted = data;
        std::sort(sorted.begin(), sorted.end());
        bkd.build(std::move(sorted));

        for (auto &q : queries) {
            auto [uni_est, uni_us] = timed([&]{ return estimate_uniform(dict, q.lo, q.hi); });
            auto [samp_est, samp_us] = timed([&]{ return estimate_sampling(dict, q.lo, q.hi); });
            auto [h_est, h_us] = timed([&]{ return hist.estimate(q.lo, q.hi); });
            auto [b_est, b_us] = timed([&]{ return bkd.estimate(q.lo, q.hi); });
            auto [exact, exact_us] = timed([&]{ return count_exact(dict, q.lo, q.hi); });

            Result r;
            r.dist_name = dc.name;
            r.range_label = q.label;
            r.actual = exact;
            r.uniform_est = uni_est;
            r.sampling_est = samp_est;
            r.hist_est = h_est;
            r.bkd_est = b_est;
            r.uniform_err = err_pct(uni_est, exact);
            r.sampling_err = err_pct(samp_est, exact);
            r.hist_err = err_pct(h_est, exact);
            r.bkd_err = err_pct(b_est, exact);
            r.uniform_us = uni_us;
            r.sampling_us = samp_us;
            r.hist_us = h_us;
            r.bkd_us = b_us;
            r.exact_us = exact_us;
            all_results.push_back(r);
        }
    }

    // ========================================================================
    // TABLE 1: Detailed accuracy
    // ========================================================================

    std::cout << "\n\n";
    std::cout << "TABLE 1: ESTIMATION ACCURACY (estimated hits vs actual hits, error %)\n";
    std::cout << std::string(130, '=') << "\n";

    std::cout << std::left
              << std::setw(12) << "Distrib"
              << std::setw(14) << "Range"
              << std::right
              << std::setw(10) << "Actual"
              << " |"
              << std::setw(10) << "Uniform"
              << std::setw(8) << "Err%"
              << " |"
              << std::setw(10) << "Sampling"
              << std::setw(8) << "Err%"
              << " |"
              << std::setw(10) << "Histgrm"
              << std::setw(8) << "Err%"
              << " |"
              << std::setw(10) << "BKD"
              << std::setw(8) << "Err%"
              << "\n";
    std::cout << std::string(130, '-') << "\n";

    std::string prev_dist;
    for (auto &r : all_results) {
        if (r.dist_name != prev_dist) {
            if (!prev_dist.empty()) std::cout << std::string(130, '.') << "\n";
            prev_dist = r.dist_name;
        }
        std::cout << std::left
                  << std::setw(12) << r.dist_name
                  << std::setw(14) << r.range_label
                  << std::right << std::fixed
                  << std::setw(10) << r.actual
                  << " |"
                  << std::setw(10) << r.uniform_est
                  << std::setw(7) << std::setprecision(1) << r.uniform_err << "%"
                  << " |"
                  << std::setw(10) << r.sampling_est
                  << std::setw(7) << std::setprecision(1) << r.sampling_err << "%"
                  << " |"
                  << std::setw(10) << r.hist_est
                  << std::setw(7) << std::setprecision(1) << r.hist_err << "%"
                  << " |"
                  << std::setw(10) << r.bkd_est
                  << std::setw(7) << std::setprecision(1) << r.bkd_err << "%"
                  << "\n";
    }

    // ========================================================================
    // TABLE 2: Mean error per distribution
    // ========================================================================

    std::cout << "\n\n";
    std::cout << "TABLE 2: MEAN ABSOLUTE ERROR % BY DISTRIBUTION\n";
    std::cout << std::string(75, '=') << "\n";

    std::map<std::string, std::vector<double>> e_uni, e_samp, e_hist, e_bkd;
    for (auto &r : all_results) {
        e_uni[r.dist_name].push_back(r.uniform_err);
        e_samp[r.dist_name].push_back(r.sampling_err);
        e_hist[r.dist_name].push_back(r.hist_err);
        e_bkd[r.dist_name].push_back(r.bkd_err);
    }
    auto avg = [](const std::vector<double> &v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };
    auto maxv = [](const std::vector<double> &v) {
        return *std::max_element(v.begin(), v.end());
    };

    std::cout << std::left << std::setw(14) << "Distribution"
              << std::right
              << std::setw(10) << "Uniform"
              << std::setw(11) << "Sampling"
              << std::setw(11) << "Histogram"
              << std::setw(11) << "BKD"
              << "     "
              << std::setw(10) << "Samp/Uni"
              << "\n";
    std::cout << std::string(75, '-') << "\n";

    double g_uni = 0, g_samp = 0, g_hist = 0, g_bkd = 0;
    size_t g_n = 0;
    for (auto &[name, _] : e_uni) {
        double mu = avg(e_uni[name]), ms = avg(e_samp[name]);
        double mh = avg(e_hist[name]), mb = avg(e_bkd[name]);
        double improvement = (mu > 0) ? ms / mu : 0;
        std::cout << std::left << std::setw(14) << name
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(9) << mu << "%"
                  << std::setw(10) << ms << "%"
                  << std::setw(10) << mh << "%"
                  << std::setw(10) << mb << "%"
                  << "     " << std::setprecision(2) << std::setw(9) << improvement << "x"
                  << "\n";
        for (auto x : e_uni[name]) { g_uni += x; g_n++; }
        for (auto x : e_samp[name]) g_samp += x;
        for (auto x : e_hist[name]) g_hist += x;
        for (auto x : e_bkd[name]) g_bkd += x;
    }
    std::cout << std::string(75, '-') << "\n";
    double imp = (g_uni > 0) ? (g_samp / g_n) / (g_uni / g_n) : 0;
    std::cout << std::left << std::setw(14) << "OVERALL"
              << std::right << std::fixed << std::setprecision(1)
              << std::setw(9) << g_uni / g_n << "%"
              << std::setw(10) << g_samp / g_n << "%"
              << std::setw(10) << g_hist / g_n << "%"
              << std::setw(10) << g_bkd / g_n << "%"
              << "     " << std::setprecision(2) << std::setw(9) << imp << "x"
              << "\n";

    // ========================================================================
    // TABLE 3: Latency comparison
    // ========================================================================

    std::cout << "\n\n";
    std::cout << "TABLE 3: ESTIMATION LATENCY (microseconds, avg over 100 runs)\n";
    std::cout << std::string(80, '=') << "\n";

    std::map<std::string, std::vector<double>> l_uni, l_samp, l_hist, l_bkd, l_exact;
    for (auto &r : all_results) {
        l_uni[r.dist_name].push_back(r.uniform_us);
        l_samp[r.dist_name].push_back(r.sampling_us);
        l_hist[r.dist_name].push_back(r.hist_us);
        l_bkd[r.dist_name].push_back(r.bkd_us);
        l_exact[r.dist_name].push_back(r.exact_us);
    }

    std::cout << std::left << std::setw(14) << "Distribution"
              << std::right
              << std::setw(12) << "Uniform"
              << std::setw(12) << "Sampling"
              << std::setw(12) << "Histogram"
              << std::setw(12) << "BKD"
              << std::setw(12) << "Exact"
              << "\n";
    std::cout << std::string(80, '-') << "\n";

    for (auto &[name, _] : l_uni) {
        std::cout << std::left << std::setw(14) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(11) << avg(l_uni[name]) << "u"
                  << std::setw(11) << avg(l_samp[name]) << "u"
                  << std::setw(11) << avg(l_hist[name]) << "u"
                  << std::setw(11) << avg(l_bkd[name]) << "u"
                  << std::setw(11) << avg(l_exact[name]) << "u"
                  << "\n";
    }

    // ========================================================================
    // TABLE 4: ANN filter strategy impact
    // ========================================================================

    std::cout << "\n\n";
    std::cout << "TABLE 4: ANN FILTER STRATEGY DECISIONS (brute_force_limit=0.05)\n";
    std::cout << "  If filter hit_ratio < 5% -> BRUTE_FORCE, else -> HNSW_APPROX\n";
    std::cout << std::string(120, '=') << "\n";

    double bf_limit = 0.05;
    uint32_t wrong_uni = 0, wrong_samp = 0, wrong_hist = 0, wrong_bkd = 0;

    std::cout << std::left
              << std::setw(12) << "Distrib"
              << std::setw(14) << "Range"
              << std::right
              << std::setw(10) << "TrueRatio"
              << std::setw(10) << "UniRatio"
              << std::setw(10) << "SampRatio"
              << "  "
              << std::left
              << std::setw(14) << "TrueStrategy"
              << std::setw(14) << "UniStrategy"
              << std::setw(14) << "SampStrategy"
              << std::setw(14) << "Verdict"
              << "\n";
    std::cout << std::string(120, '-') << "\n";

    for (auto &r : all_results) {
        double true_r = static_cast<double>(r.actual) / num_docs;
        double uni_r = static_cast<double>(r.uniform_est) / num_docs;
        double samp_r = static_cast<double>(r.sampling_est) / num_docs;

        auto strategy = [&](double ratio) -> std::string {
            return (ratio < bf_limit) ? "BRUTE_FORCE" : "HNSW_APPROX";
        };

        std::string ts = strategy(true_r), us = strategy(uni_r), ss = strategy(samp_r);
        bool uc = (us == ts), sc = (ss == ts);
        if (!uc) wrong_uni++;
        if (!sc) wrong_samp++;

        std::string verdict;
        if (uc && sc)       verdict = "Both OK";
        else if (!uc && sc) verdict = "** Samp WINS";
        else if (uc && !sc) verdict = "** Uni WINS";
        else                verdict = "Both WRONG";

        std::cout << std::left
                  << std::setw(12) << r.dist_name
                  << std::setw(14) << r.range_label
                  << std::right << std::fixed << std::setprecision(4)
                  << std::setw(10) << true_r
                  << std::setw(10) << uni_r
                  << std::setw(10) << samp_r
                  << "  " << std::left
                  << std::setw(14) << ts
                  << std::setw(14) << us
                  << std::setw(14) << ss
                  << verdict
                  << "\n";
    }
    std::cout << "\nWrong strategy: Uniform=" << wrong_uni
              << "  Sampling=" << wrong_samp
              << "  out of " << all_results.size() << " queries\n";

    // ========================================================================
    // TABLE 5: Memory overhead
    // ========================================================================

    std::cout << "\n\n";
    std::cout << "TABLE 5: MEMORY & COMPLEXITY COMPARISON\n";
    std::cout << std::string(80, '=') << "\n";

    std::cout << std::left << std::setw(20) << "Method"
              << std::setw(18) << "Extra Memory"
              << std::setw(14) << "Time Cmplx"
              << "Notes\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::setw(20) << "Uniform (old)"
              << std::setw(18) << "0 B"
              << std::setw(14) << "O(1)"
              << "Uses dict stats only; inaccurate on skew\n";
    std::cout << std::setw(20) << "Sampling (new)"
              << std::setw(18) << "0 B"
              << std::setw(14) << "O(N_uniq)"
              << "Iterates dict, 16 frozenSize() calls\n";
    std::cout << std::setw(20) << "Histogram"
              << std::setw(18) << (std::to_string(hist_buckets * 4) + " B/attr")
              << std::setw(14) << "O(buckets)"
              << "Requires maintenance on updates\n";
    std::cout << std::setw(20) << "BKD-tree (ES)"
              << std::setw(18) << "~16 B/block"
              << std::setw(14) << "O(blocks)"
              << "Requires sorted storage, rebuild on updates\n";
    std::cout << std::setw(20) << "Exact count"
              << std::setw(18) << "0 B"
              << std::setw(14) << "O(N_uniq)"
              << "Ground truth; too slow for large ranges\n";

    // ========================================================================
    // TABLE 6: BKD vs B-tree architectural comparison
    // ========================================================================

    std::cout << "\n\n";
    std::cout << "TABLE 6: BKD-TREE vs B-TREE ARCHITECTURAL COMPARISON\n";
    std::cout << std::string(90, '=') << "\n";

    std::cout << std::left << std::setw(25) << "Aspect"
              << std::setw(32) << "B-tree + Posting Lists"
              << "BKD-tree (Lucene/ES)\n";
    std::cout << std::string(90, '-') << "\n";
    std::cout << std::setw(25) << "Range estimation"
              << std::setw(32) << "Needs sampling/histogram"
              << "Native: leaf block counting\n";
    std::cout << std::setw(25) << "Point lookup"
              << std::setw(32) << "O(log N) exact"
              << "O(log N) but higher constant\n";
    std::cout << std::setw(25) << "Range scan"
              << std::setw(32) << "Merge posting lists"
              << "Sequential leaf block scan\n";
    std::cout << std::setw(25) << "Update cost"
              << std::setw(32) << "O(log N) per update"
              << "Requires periodic rebuild\n";
    std::cout << std::setw(25) << "Memory layout"
              << std::setw(32) << "Pointer-based tree nodes"
              << "Cache-friendly packed blocks\n";
    std::cout << std::setw(25) << "Multi-dim support"
              << std::setw(32) << "Single dimension only"
              << "Native k-d partitioning\n";
    std::cout << std::setw(25) << "Write amplification"
              << std::setw(32) << "Low (in-place updates)"
              << "High (segment merges)\n";
    std::cout << std::setw(25) << "Real-time updates"
              << std::setw(32) << "Yes (immediate)"
              << "Batch-oriented (LSM merge)\n";
    std::cout << std::setw(25) << "Vespa compatibility"
              << std::setw(32) << "Native, well integrated"
              << "Would require major refactor\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int, char**) {
    std::cout << "######################################################################\n";
    std::cout << "#  RANGE QUERY ESTIMATION BENCHMARK                                  #\n";
    std::cout << "#  Comparing: Uniform | Sampling | Histogram | BKD-style             #\n";
    std::cout << "#  Across: Uniform, Zipf, Normal, Bimodal, Exponential, LogNormal    #\n";
    std::cout << "######################################################################\n";

    // Scale 1: 1M docs
    run_benchmark(1'000'000, 0, 9'999, 256, 512);

    // Scale 2: 5M docs
    run_benchmark(5'000'000, 0, 99'999, 1024, 1024);

    // Scale 3: 10M docs, high cardinality
    run_benchmark(10'000'000, 0, 999'999, 2048, 2048);

    std::cout << "\n\nBenchmark complete.\n";
    return 0;
}
