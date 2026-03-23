/**
 * Timestamp Histogram Accuracy Benchmark
 * =======================================
 *
 * Simulates the ads timestamp filtering scenario:
 *   - Documents span 30/90/365 days of timestamps
 *   - Queries filter "last N hours/days" (sliding window at right edge)
 *   - Distribution patterns: uniform, bursty (daytime peaks), with deletions
 *
 * Tests equi-width histogram accuracy and proposes improvements.
 *
 * Compile: g++ -std=c++17 -O2 -o ts_bench timestamp_histogram_bench.cpp
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// Equi-width histogram (current implementation)
// ============================================================================

class EquiWidthHistogram {
    std::vector<uint32_t> _buckets;
    int64_t _min_val, _max_val;
    uint32_t _num_buckets;
    double _bucket_width;
    uint32_t _total;
public:
    EquiWidthHistogram(int64_t lo, int64_t hi, uint32_t num_buckets)
        : _buckets(num_buckets, 0), _min_val(lo), _max_val(hi),
          _num_buckets(num_buckets),
          _bucket_width(std::max(1.0, static_cast<double>(hi - lo + 1) / num_buckets)),
          _total(0) {}

    void add(int64_t v, uint32_t count = 1) {
        uint32_t idx = static_cast<uint32_t>((v - _min_val) / _bucket_width);
        if (idx >= _num_buckets) idx = _num_buckets - 1;
        _buckets[idx] += count;
        _total += count;
    }

    uint32_t estimate(int64_t lo, int64_t hi) const {
        if (lo > _max_val || hi < _min_val) return 0;
        lo = std::max(lo, _min_val);
        hi = std::min(hi, _max_val);
        double sum = 0;
        uint32_t lo_b = std::min(static_cast<uint32_t>((lo - _min_val) / _bucket_width), _num_buckets - 1);
        uint32_t hi_b = std::min(static_cast<uint32_t>((hi - _min_val) / _bucket_width), _num_buckets - 1);
        if (lo_b == hi_b) {
            double bw = _bucket_width;
            double bl = _min_val + lo_b * bw;
            double frac = (std::min(bl + bw, (double)(hi + 1)) - std::max(bl, (double)lo)) / bw;
            return static_cast<uint32_t>(frac * _buckets[lo_b]);
        }
        // Partial first
        { double bl = _min_val + lo_b * _bucket_width;
          double bh = bl + _bucket_width;
          sum += (bh - std::max(bl, (double)lo)) / _bucket_width * _buckets[lo_b]; }
        // Full middle
        for (uint32_t i = lo_b + 1; i < hi_b; ++i) sum += _buckets[i];
        // Partial last
        { double bl = _min_val + hi_b * _bucket_width;
          double bh = bl + _bucket_width;
          sum += (std::min(bh, (double)(hi + 1)) - bl) / _bucket_width * _buckets[hi_b]; }
        return static_cast<uint32_t>(sum);
    }

    double bucket_width_seconds() const { return _bucket_width; }
    uint32_t total() const { return _total; }
};

// ============================================================================
// Two-level histogram: coarse global + fine recent window
// ============================================================================

class TwoLevelHistogram {
    // Coarse level: covers entire range
    EquiWidthHistogram _coarse;
    // Fine level: covers only the recent window (e.g., last 7 days)
    EquiWidthHistogram _fine;
    int64_t _fine_start;  // start of fine-grained window
    int64_t _fine_end;    // end of fine-grained window

public:
    TwoLevelHistogram(int64_t lo, int64_t hi, uint32_t coarse_buckets,
                      int64_t fine_start, int64_t fine_end, uint32_t fine_buckets)
        : _coarse(lo, hi, coarse_buckets),
          _fine(fine_start, fine_end, fine_buckets),
          _fine_start(fine_start), _fine_end(fine_end) {}

    void add(int64_t v, uint32_t count = 1) {
        _coarse.add(v, count);
        if (v >= _fine_start && v <= _fine_end) {
            _fine.add(v, count);
        }
    }

    uint32_t estimate(int64_t lo, int64_t hi) const {
        // If query falls entirely within fine window, use fine histogram
        if (lo >= _fine_start && hi <= _fine_end) {
            return _fine.estimate(lo, hi);
        }
        // If query partially overlaps fine window
        if (lo < _fine_start && hi >= _fine_start && hi <= _fine_end) {
            uint32_t coarse_part = _coarse.estimate(lo, _fine_start - 1);
            uint32_t fine_part = _fine.estimate(_fine_start, hi);
            return coarse_part + fine_part;
        }
        // Otherwise use coarse
        return _coarse.estimate(lo, hi);
    }

    double coarse_bucket_width() const { return _coarse.bucket_width_seconds(); }
    double fine_bucket_width() const { return _fine.bucket_width_seconds(); }
};

// ============================================================================
// Data generators for ad timestamp scenarios
// ============================================================================

// Uniform distribution across time span
std::vector<int64_t> gen_uniform_ts(size_t n, int64_t start, int64_t end, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> d(start, end);
    std::vector<int64_t> v(n);
    for (auto& x : v) x = d(rng);
    return v;
}

// Bursty distribution: daytime has 3x more ads than nighttime
// Simulates real ad creation patterns
std::vector<int64_t> gen_bursty_ts(size_t n, int64_t start, int64_t end, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> udist(0.0, 1.0);
    std::uniform_int_distribution<int64_t> day_dist(0, (end - start) / 86400);

    std::vector<int64_t> v(n);
    for (auto& x : v) {
        int64_t day_offset = day_dist(rng) * 86400;
        double u = udist(rng);
        int64_t hour;
        if (u < 0.75) {
            // 75% of ads during daytime (8am-10pm = 14 hours)
            std::uniform_int_distribution<int64_t> daytime(8 * 3600, 22 * 3600);
            hour = daytime(rng);
        } else {
            // 25% during nighttime (10pm-8am = 10 hours)
            std::uniform_int_distribution<int64_t> nighttime(0, 10 * 3600 - 1);
            hour = (22 * 3600 + nighttime(rng)) % 86400;
        }
        x = start + day_offset + hour;
        if (x > end) x = end;
        if (x < start) x = start;
    }
    return v;
}

// Recent-heavy: 50% of docs in last 20% of time span (ads expire, recent ones dominate)
std::vector<int64_t> gen_recent_heavy(size_t n, int64_t start, int64_t end, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::bernoulli_distribution recent(0.5);
    int64_t range = end - start;
    std::uniform_int_distribution<int64_t> recent_dist(start + range * 80 / 100, end);
    std::uniform_int_distribution<int64_t> old_dist(start, end);

    std::vector<int64_t> v(n);
    for (auto& x : v) {
        x = recent(rng) ? recent_dist(rng) : old_dist(rng);
    }
    return v;
}

// Exponential decay: most docs are very recent
std::vector<int64_t> gen_exp_decay(size_t n, int64_t start, int64_t end, uint32_t seed = 42) {
    std::mt19937_64 rng(seed);
    double range = end - start;
    double lambda = 3.0 / range;  // ~95% within range
    std::exponential_distribution<double> d(lambda);

    std::vector<int64_t> v(n);
    for (auto& x : v) {
        double offset = d(rng);
        x = end - static_cast<int64_t>(offset);
        if (x < start) x = start;
    }
    return v;
}

// ============================================================================
// Benchmark driver
// ============================================================================

struct QuerySpec {
    std::string label;
    int64_t offset_from_end;  // query is [end - offset, end]
};

struct Result {
    std::string dist;
    std::string query;
    uint32_t actual;
    uint32_t equi_est;
    uint32_t twolevel_est;
    double equi_err;
    double twolevel_err;
    double equi_buckets_covered;
    double fine_buckets_covered;
};

double err_pct(uint32_t est, uint32_t actual) {
    if (actual == 0) return est == 0 ? 0.0 : 999.9;
    return 100.0 * std::abs(static_cast<double>(est) - actual) / actual;
}

uint32_t count_exact(const std::vector<int64_t>& data, int64_t lo, int64_t hi) {
    uint32_t count = 0;
    for (auto v : data) {
        if (v >= lo && v <= hi) count++;
    }
    return count;
}

void run_scenario(const std::string& scenario_name,
                  uint32_t num_docs, int64_t span_days,
                  uint32_t equi_buckets, uint32_t fine_buckets,
                  int64_t fine_window_days) {
    int64_t now = 1700000000;  // some reference timestamp (seconds)
    int64_t start = now - span_days * 86400;
    int64_t end = now;
    int64_t fine_start = now - fine_window_days * 86400;

    std::cout << "\n================================================================\n";
    std::cout << "  " << scenario_name << "\n";
    std::cout << "  Docs: " << num_docs
              << ", Span: " << span_days << " days"
              << ", Equi-width: " << equi_buckets << " buckets"
              << ", Fine: " << fine_buckets << " buckets over last " << fine_window_days << " days\n";
    std::cout << "================================================================\n";

    struct DistConfig {
        std::string name;
        std::function<std::vector<int64_t>()> gen;
    };

    std::vector<DistConfig> dists = {
        {"Uniform",      [&]{ return gen_uniform_ts(num_docs, start, end); }},
        {"Bursty(day)",  [&]{ return gen_bursty_ts(num_docs, start, end); }},
        {"RecentHeavy",  [&]{ return gen_recent_heavy(num_docs, start, end); }},
        {"ExpDecay",     [&]{ return gen_exp_decay(num_docs, start, end); }},
    };

    std::vector<QuerySpec> queries = {
        {"last 1 hour",    3600},
        {"last 6 hours",   6 * 3600},
        {"last 24 hours",  24 * 3600},
        {"last 3 days",    3 * 86400},
        {"last 7 days",    7 * 86400},
        {"last 14 days",   14 * 86400},
        {"last 30 days",   30 * 86400},
    };

    std::vector<Result> all_results;

    for (auto& dc : dists) {
        auto data = dc.gen();

        // Build equi-width histogram
        EquiWidthHistogram equi(start, end, equi_buckets);
        for (auto v : data) equi.add(v);

        // Build two-level histogram
        TwoLevelHistogram twolevel(start, end, equi_buckets / 2,
                                   fine_start, end, fine_buckets);
        for (auto v : data) twolevel.add(v);

        for (auto& q : queries) {
            if (q.offset_from_end > span_days * 86400) continue;

            int64_t qlo = end - q.offset_from_end;
            int64_t qhi = end;

            uint32_t actual = count_exact(data, qlo, qhi);
            uint32_t e_est = equi.estimate(qlo, qhi);
            uint32_t t_est = twolevel.estimate(qlo, qhi);

            double equi_bw = equi.bucket_width_seconds();
            double fine_bw = twolevel.fine_bucket_width();
            double equi_buckets_covered = q.offset_from_end / equi_bw;
            double fine_buckets_covered = q.offset_from_end / fine_bw;

            all_results.push_back({
                dc.name, q.label, actual,
                e_est, t_est,
                err_pct(e_est, actual), err_pct(t_est, actual),
                equi_buckets_covered, fine_buckets_covered
            });
        }
    }

    // Print results
    std::cout << "\n";
    std::cout << std::left
              << std::setw(14) << "Distrib"
              << std::setw(16) << "Query"
              << std::right
              << std::setw(10) << "Actual"
              << " |"
              << std::setw(10) << "EquiW"
              << std::setw(8) << "Err%"
              << std::setw(8) << "#Bkts"
              << " |"
              << std::setw(10) << "TwoLevel"
              << std::setw(8) << "Err%"
              << std::setw(8) << "#Bkts"
              << "\n";
    std::cout << std::string(100, '-') << "\n";

    std::string prev_dist;
    for (auto& r : all_results) {
        if (r.dist != prev_dist && !prev_dist.empty())
            std::cout << std::string(100, '.') << "\n";
        prev_dist = r.dist;

        std::cout << std::left
                  << std::setw(14) << r.dist
                  << std::setw(16) << r.query
                  << std::right << std::fixed
                  << std::setw(10) << r.actual
                  << " |"
                  << std::setw(10) << r.equi_est
                  << std::setw(7) << std::setprecision(1) << r.equi_err << "%"
                  << std::setw(7) << std::setprecision(1) << r.equi_buckets_covered
                  << " |"
                  << std::setw(10) << r.twolevel_est
                  << std::setw(7) << std::setprecision(1) << r.twolevel_err << "%"
                  << std::setw(7) << std::setprecision(1) << r.fine_buckets_covered
                  << "\n";
    }

    // Aggregate
    std::cout << "\n";
    std::map<std::string, std::vector<double>> e_errs, t_errs;
    for (auto& r : all_results) {
        e_errs[r.dist].push_back(r.equi_err);
        t_errs[r.dist].push_back(r.twolevel_err);
    }
    auto avg = [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };

    std::cout << std::left << std::setw(14) << "Distribution"
              << std::right << std::setw(15) << "EquiW MeanErr"
              << std::setw(18) << "TwoLevel MeanErr"
              << std::setw(15) << "Improvement"
              << "\n";
    std::cout << std::string(62, '-') << "\n";
    double ge = 0, gt = 0; size_t gn = 0;
    for (auto& [name, _] : e_errs) {
        double me = avg(e_errs[name]), mt = avg(t_errs[name]);
        double imp = (me > 0) ? (1.0 - mt/me) * 100 : 0;
        std::cout << std::left << std::setw(14) << name
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(14) << me << "%"
                  << std::setw(17) << mt << "%"
                  << std::setw(13) << imp << "%"
                  << "\n";
        for (auto x : e_errs[name]) { ge += x; gn++; }
        for (auto x : t_errs[name]) gt += x;
    }
    std::cout << std::string(62, '-') << "\n";
    double gi = (ge > 0) ? (1.0 - (gt/gn)/(ge/gn)) * 100 : 0;
    std::cout << std::left << std::setw(14) << "OVERALL"
              << std::right << std::fixed << std::setprecision(1)
              << std::setw(14) << ge/gn << "%"
              << std::setw(17) << gt/gn << "%"
              << std::setw(13) << gi << "%"
              << "\n";

    // Bucket resolution summary
    double equi_bw = static_cast<double>(span_days * 86400) / equi_buckets;
    double fine_bw = static_cast<double>(fine_window_days * 86400) / fine_buckets;
    std::cout << "\nBucket resolution:\n";
    std::cout << "  Equi-width: 1 bucket = " << std::fixed << std::setprecision(1)
              << equi_bw << " sec = " << equi_bw/3600 << " hours\n";
    std::cout << "  Fine level: 1 bucket = " << fine_bw << " sec = "
              << fine_bw/60 << " min\n";

    // ANN filter strategy impact
    std::cout << "\nANN filter strategy decisions (brute_force_limit=0.05):\n";
    uint32_t wrong_equi = 0, wrong_twolevel = 0;
    for (auto& r : all_results) {
        double true_ratio = static_cast<double>(r.actual) / num_docs;
        double equi_ratio = static_cast<double>(r.equi_est) / num_docs;
        double twolevel_ratio = static_cast<double>(r.twolevel_est) / num_docs;
        auto strat = [](double ratio) { return ratio < 0.05 ? "BF" : "HNSW"; };
        if (std::string(strat(equi_ratio)) != strat(true_ratio)) wrong_equi++;
        if (std::string(strat(twolevel_ratio)) != strat(true_ratio)) wrong_twolevel++;
    }
    std::cout << "  Wrong decisions: EquiWidth=" << wrong_equi
              << "  TwoLevel=" << wrong_twolevel
              << "  out of " << all_results.size() << " queries\n";
}

int main() {
    std::cout << "###############################################\n";
    std::cout << "#  TIMESTAMP HISTOGRAM ACCURACY BENCHMARK     #\n";
    std::cout << "#  Simulating ads timestamp filtering          #\n";
    std::cout << "###############################################\n";

    // Scenario 1: 30 days of ads, 5M docs
    run_scenario("30 DAYS OF ADS (5M docs)",
                 5'000'000, 30, 1024, 1024, 7);

    // Scenario 2: 90 days of ads, 10M docs
    run_scenario("90 DAYS OF ADS (10M docs)",
                 10'000'000, 90, 1024, 1024, 7);

    // Scenario 3: 365 days of ads, 20M docs
    run_scenario("365 DAYS OF ADS (20M docs)",
                 20'000'000, 365, 1024, 1024, 7);

    // Scenario 4: 30 days but with more equi-width buckets
    run_scenario("30 DAYS - MORE BUCKETS (5M docs, 4096 equi-width)",
                 5'000'000, 30, 4096, 1024, 7);

    std::cout << "\n\nBenchmark complete.\n";
    return 0;
}
