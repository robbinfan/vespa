// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
//
// L1 standalone harness for lazy top-K sort optimization.
//
// NOT part of the Vespa build. Compile with:
//   g++ -O2 -std=c++17 -Wall -Wextra -o sort_bench sort_bench.cpp
//
// This harness replicates just enough of searchlib's sort primitives
// (convertForSort / serializeForSort / memcmp comparator) to prototype and
// differentially test the lazy-encoding top-K path before porting it to
// sortresults.cpp. See README.md for details.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Encoding primitives — mirror staging_vespalib/src/vespa/vespalib/util/sort.h
//    Only the templates we actually use in proton's fixed-width attribute
//    serialization path.
// ---------------------------------------------------------------------------
namespace bench {

// Portable big-endian store (== vespalib::nbo::n2h + memcpy in the real code).
template <typename U>
inline void store_be(U v, uint8_t *dst) {
    for (int i = int(sizeof(U)) - 1; i >= 0; --i) {
        dst[i] = uint8_t(v & 0xFF);
        v >>= 8;
    }
}

template <typename T, bool asc>
class convertForSort;

template <>
class convertForSort<int32_t, true> {
public:
    using InputType = int32_t;
    using UIntType = uint32_t;
    static inline UIntType convert(int32_t v) {
        return uint32_t(v) ^ (uint32_t(std::numeric_limits<int32_t>::max()) + 1);
    }
};

template <>
class convertForSort<int32_t, false> {
public:
    using InputType = int32_t;
    using UIntType = uint32_t;
    static inline UIntType convert(int32_t v) {
        return uint32_t(v) ^ uint32_t(std::numeric_limits<int32_t>::max());
    }
};

template <>
class convertForSort<int64_t, true> {
public:
    using InputType = int64_t;
    using UIntType = uint64_t;
    static inline UIntType convert(int64_t v) {
        return uint64_t(v) ^ (uint64_t(std::numeric_limits<int64_t>::max()) + 1);
    }
};

template <>
class convertForSort<int64_t, false> {
public:
    using InputType = int64_t;
    using UIntType = uint64_t;
    static inline UIntType convert(int64_t v) {
        return uint64_t(v) ^ uint64_t(std::numeric_limits<int64_t>::max());
    }
};

template <>
class convertForSort<double, true> {
public:
    using InputType = double;
    using UIntType = uint64_t;
    static inline UIntType convert(double value) {
        union { double f; UIntType u; } val;
        val.f = value;
        return (int64_t(val.u) >= 0)
                   ? (val.u ^ (uint64_t(std::numeric_limits<int64_t>::max()) + 1))
                   : (val.u ^ std::numeric_limits<uint64_t>::max());
    }
};

template <>
class convertForSort<double, false> {
public:
    using InputType = double;
    using UIntType = uint64_t;
    static inline UIntType convert(double value) {
        union { double f; UIntType u; } val;
        val.f = value;
        return (int64_t(val.u) >= 0)
                   ? (val.u ^ uint64_t(std::numeric_limits<int64_t>::max()))
                   : val.u;
    }
};

template <typename C>
inline uint32_t serializeForSort(typename C::InputType v, uint8_t *dst) {
    typename C::UIntType u = C::convert(v);
    store_be(u, dst);
    return sizeof(u);
}

// ---------------------------------------------------------------------------
// 2. Fake attribute storage. Each attribute is just a dense vector of values;
//    `serializeForAsc<T>(docId, dst)` writes the memcmp-sortable bytes for a
//    single doc into `dst` and returns the number of bytes written. This is
//    the exact shape of the real `IAttributeVector::serializeForAscendingSort`.
// ---------------------------------------------------------------------------

struct IFieldEncoder {
    virtual ~IFieldEncoder() = default;
    virtual uint32_t width() const = 0;
    virtual void encode(uint32_t docId, uint8_t *dst) const = 0;
    virtual const char *name() const = 0;
};

template <typename T, bool asc>
struct NumericField : IFieldEncoder {
    std::string _name;
    std::vector<T> _values;
    NumericField(std::string n, std::vector<T> v) : _name(std::move(n)), _values(std::move(v)) {}
    uint32_t width() const override { return sizeof(typename convertForSort<T, asc>::UIntType); }
    void encode(uint32_t docId, uint8_t *dst) const override {
        serializeForSort<convertForSort<T, asc>>(_values[docId], dst);
    }
    const char *name() const override { return _name.c_str(); }
};

// SlowField simulates an expensive encoder (UCA collator / string hash / etc).
// Each call burns `extraWorkNs` nanoseconds worth of dependent loads before
// writing the actual sortable bytes. This is how we model the cost ratio
// between cheap numeric encoding and expensive string encoding. The produced
// bytes are still memcmp-sortable so all the downstream algorithms work.
struct SlowField : IFieldEncoder {
    std::string _name;
    std::vector<uint64_t> _values;
    // A side buffer large enough to defeat the L1 dcache so each encode
    // produces a predictable amount of work.
    mutable std::vector<uint64_t> _cacheBuster;
    uint32_t _extraLoads;
    SlowField(std::string n, std::vector<uint64_t> v, uint32_t extraLoads)
        : _name(std::move(n)), _values(std::move(v)), _extraLoads(extraLoads) {
        _cacheBuster.resize(1u << 20); // 8 MiB
        for (size_t i = 0; i < _cacheBuster.size(); ++i) _cacheBuster[i] = i * 2654435761u;
    }
    uint32_t width() const override { return sizeof(uint64_t); }
    void encode(uint32_t docId, uint8_t *dst) const override {
        // Burn cycles: chase dependent loads into the cache-buster buffer.
        // The loop must genuinely feed its result into the stored bytes,
        // otherwise the compiler will DCE the whole thing.
        uint64_t acc = _values[docId];
        uint32_t mask = uint32_t(_cacheBuster.size() - 1);
        for (uint32_t i = 0; i < _extraLoads; ++i) {
            acc = _cacheBuster[uint32_t(acc) & mask] ^ (acc * 0x9E3779B97F4A7C15ull);
        }
        // Use the hashed value AS the sort key. That keeps the ordering
        // stable and memcmp-sortable, while forcing the compiler to actually
        // run the loop.
        store_be(convertForSort<int64_t, true>::convert(int64_t(acc)), dst);
    }
    const char *name() const override { return _name.c_str(); }
};

// ---------------------------------------------------------------------------
// 3. Mock SortData — shape matches FastS_SortSpec::SortData enough for our
//    purposes (docId + idx/len into a shared byte buffer).
// ---------------------------------------------------------------------------
struct SortData {
    uint32_t docId;
    uint32_t idx;
    uint32_t len;
};

struct Comparator {
    const uint8_t *base;
    bool operator()(const SortData &a, const SortData &b) const {
        uint32_t l = std::min(a.len, b.len);
        int r = std::memcmp(base + a.idx, base + b.idx, l);
        if (r) return r < 0;
        return a.len < b.len;
    }
};

// ---------------------------------------------------------------------------
// 4. Three sort paths.
// ---------------------------------------------------------------------------

// Fully encode every hit, one field at a time, into a flat buffer. Returns
// total bytes written. Equivalent to `FastS_SortSpec::initSortData`.
static uint32_t encode_all(const std::vector<const IFieldEncoder *> &fields,
                           const std::vector<uint32_t> &docIds, std::vector<uint8_t> &buf,
                           std::vector<SortData> &sd) {
    uint32_t width = 0;
    for (auto *f : fields) width += f->width();
    uint32_t n = uint32_t(docIds.size());
    buf.assign(size_t(width) * n, 0);
    sd.resize(n);
    uint32_t idx = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *p = buf.data() + idx;
        for (auto *f : fields) {
            f->encode(docIds[i], p);
            p += f->width();
        }
        sd[i].docId = docIds[i];
        sd[i].idx = idx;
        sd[i].len = width;
        idx += width;
    }
    return idx;
}

// --- Baseline: encode all, std::sort all.
static void baseline_sort(const std::vector<const IFieldEncoder *> &fields,
                          const std::vector<uint32_t> &docIds, uint32_t topn,
                          std::vector<uint8_t> &buf, std::vector<SortData> &sd) {
    encode_all(fields, docIds, buf, sd);
    Comparator cmp{buf.data()};
    if (topn >= sd.size()) {
        std::sort(sd.begin(), sd.end(), cmp);
    } else {
        std::sort(sd.begin(), sd.end(), cmp);
    }
}

// --- Phase 1: encode all, partial_sort top-K.
static void partial_sort_path(const std::vector<const IFieldEncoder *> &fields,
                              const std::vector<uint32_t> &docIds, uint32_t topn,
                              std::vector<uint8_t> &buf, std::vector<SortData> &sd) {
    encode_all(fields, docIds, buf, sd);
    Comparator cmp{buf.data()};
    if (topn < sd.size()) {
        std::partial_sort(sd.begin(), sd.begin() + topn, sd.end(), cmp);
    } else {
        std::sort(sd.begin(), sd.end(), cmp);
    }
}

// --- Phase 2: lazy top-K.
//   1) Encode only field 0 for all N hits into a scratch buffer.
//   2) Use std::nth_element on a proxy index array to find the K-th smallest
//      field-0 value. The candidate set C is every hit whose field-0 value is
//      <= that pivot.
//   3) If |C| > maxCandidates (= 4*topn), abandon and fall back to full encode.
//   4) Otherwise fully encode fields 0..F-1 for C only into the real buffer,
//      std::sort C, and we are done. Hits outside C never get their tail
//      fields encoded at all.
// Reusable scratch storage for the lazy path. Hoisted out of the function so
// repeat calls in the benchmark loop don't re-allocate. In the real port these
// would become members of FastS_SortSpec (or heap-allocated once per query).
struct LazyScratch {
    std::vector<uint8_t> field0Bytes;  // N * w0 bytes
    std::vector<uint32_t> idx;         // N uint32_t
    std::vector<uint32_t> candDocIds;  // <= maxCandidates
};

static void lazy_topk_sort(const std::vector<const IFieldEncoder *> &fields,
                           const std::vector<uint32_t> &docIds, uint32_t topn,
                           std::vector<uint8_t> &buf, std::vector<SortData> &sd,
                           LazyScratch &scratch, bool *fellBack = nullptr) {
    uint32_t n = uint32_t(docIds.size());
    if (fellBack) *fellBack = false;
    if (fields.empty() || topn == 0 || topn >= n) {
        baseline_sort(fields, docIds, topn, buf, sd);
        return;
    }

    // Step 1: encode field 0 only into scratch.field0Bytes.
    const IFieldEncoder *f0 = fields[0];
    uint32_t w0 = f0->width();
    scratch.field0Bytes.resize(size_t(w0) * n);
    uint8_t *sp = scratch.field0Bytes.data();
    for (uint32_t i = 0; i < n; ++i) {
        f0->encode(docIds[i], sp + size_t(i) * w0);
    }

    // Step 2: find pivot via nth_element on indices into field0Bytes.
    scratch.idx.resize(n);
    for (uint32_t i = 0; i < n; ++i) scratch.idx[i] = i;
    auto field0_less = [sp, w0](uint32_t a, uint32_t b) {
        return std::memcmp(sp + size_t(a) * w0, sp + size_t(b) * w0, w0) < 0;
    };
    std::nth_element(scratch.idx.begin(), scratch.idx.begin() + (topn - 1), scratch.idx.end(),
                     field0_less);

    // Pivot byte sequence: field-0 bytes of the (topn-1)-th element.
    const uint8_t *pivot = sp + size_t(scratch.idx[topn - 1]) * w0;

    // Step 3: collect candidate set C = { hits with field0 <= pivot }.
    // After nth_element, idx[0..topn-1] are all <= pivot, and idx[topn..n-1] may
    // still contain elements == pivot. We must pick those up too.
    scratch.candDocIds.clear();
    scratch.candDocIds.reserve(size_t(topn) * 2);
    for (uint32_t i = 0; i < topn; ++i) scratch.candDocIds.push_back(docIds[scratch.idx[i]]);
    for (uint32_t i = topn; i < n; ++i) {
        const uint8_t *p = sp + size_t(scratch.idx[i]) * w0;
        if (std::memcmp(p, pivot, w0) == 0) {
            scratch.candDocIds.push_back(docIds[scratch.idx[i]]);
        }
    }

    uint32_t maxCandidates = std::max<uint32_t>(topn * 4, topn + 16);
    if (scratch.candDocIds.size() > maxCandidates) {
        // Fall back: pivot has too many ties to make the lazy path worthwhile.
        if (fellBack) *fellBack = true;
        baseline_sort(fields, docIds, topn, buf, sd);
        return;
    }

    // Step 4: full encode for candidate set only.
    encode_all(fields, scratch.candDocIds, buf, sd);

    Comparator cmp{buf.data()};
    std::sort(sd.begin(), sd.end(), cmp);

    // Trim sd down to exactly topn. Downstream sees a shorter result but that
    // is the same contract as top-K radix: a[0..topn) sorted, rest unused.
    if (sd.size() > topn) sd.resize(topn);
}

} // namespace bench

// ---------------------------------------------------------------------------
// 5. Workload generation & benchmark driver.
// ---------------------------------------------------------------------------

struct Workload {
    // Owned field encoders.
    std::vector<std::unique_ptr<bench::IFieldEncoder>> owned;
    // Non-owning view used by the sort paths.
    std::vector<const bench::IFieldEncoder *> fields;
    // docId list (identity 0..N-1 here).
    std::vector<uint32_t> docIds;
};

// cardinality == 0 means "uniformly random over full range"; otherwise only
// that many distinct values (to exercise tie behavior). slowLoads > 0 turns
// the ~non-first fields into SlowField instances with that much work per
// encode, modelling heavy encoders (UCA collators, lowercased strings).
static Workload make_workload(uint32_t n, uint32_t numFields, uint32_t field0Cardinality,
                              uint64_t seed, uint32_t slowLoads = 0) {
    Workload w;
    std::mt19937_64 rng(seed);
    auto makeInt = [&](std::string name, uint32_t card) {
        std::vector<int32_t> v(n);
        if (card == 0) {
            std::uniform_int_distribution<int32_t> dist(std::numeric_limits<int32_t>::min(),
                                                        std::numeric_limits<int32_t>::max());
            for (auto &x : v) x = dist(rng);
        } else {
            std::vector<int32_t> vals(card);
            std::uniform_int_distribution<int32_t> dist(std::numeric_limits<int32_t>::min(),
                                                        std::numeric_limits<int32_t>::max());
            for (auto &x : vals) x = dist(rng);
            std::uniform_int_distribution<uint32_t> pick(0, card - 1);
            for (auto &x : v) x = vals[pick(rng)];
        }
        return std::unique_ptr<bench::IFieldEncoder>(
            new bench::NumericField<int32_t, true>(std::move(name), std::move(v)));
    };
    auto makeSlow = [&](std::string name) {
        std::vector<uint64_t> v(n);
        std::uniform_int_distribution<uint64_t> dist(0, ~uint64_t(0));
        for (auto &x : v) x = dist(rng);
        return std::unique_ptr<bench::IFieldEncoder>(
            new bench::SlowField(std::move(name), std::move(v), slowLoads));
    };
    w.owned.push_back(makeInt("f0", field0Cardinality));
    for (uint32_t i = 1; i < numFields; ++i) {
        if (slowLoads > 0) {
            w.owned.push_back(makeSlow("f" + std::to_string(i)));
        } else {
            w.owned.push_back(makeInt("f" + std::to_string(i), 0));
        }
    }
    for (auto &p : w.owned) w.fields.push_back(p.get());
    w.docIds.resize(n);
    for (uint32_t i = 0; i < n; ++i) w.docIds[i] = i;
    return w;
}

using Clock = std::chrono::high_resolution_clock;

static double run_one(std::function<void()> fn, int iters) {
    double best = 1e18;
    for (int i = 0; i < iters; ++i) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        if (us < best) best = us;
    }
    return best;
}

struct Row {
    uint32_t n;
    uint32_t k;
    uint32_t fields;
    uint32_t card;
    double baseline_us;
    double partial_us;
    double lazy_us;
    bool lazy_fallback;
};

static void print_header() {
    std::printf("%-10s %-8s %-6s %-10s %-14s %-14s %-14s %-10s %-10s %-s\n",
                "N", "K", "F", "card(f0)", "baseline(us)", "partial(us)", "lazy(us)",
                "part/base", "lazy/base", "notes");
    std::printf("---------------------------------------------------------------------------------"
                "---------------------------------\n");
}

static void print_row(const Row &r) {
    std::printf("%-10u %-8u %-6u %-10u %-14.1f %-14.1f %-14.1f %-10.2f %-10.2f %s\n",
                r.n, r.k, r.fields, r.card, r.baseline_us, r.partial_us, r.lazy_us,
                r.partial_us / r.baseline_us, r.lazy_us / r.baseline_us,
                r.lazy_fallback ? "lazy-fallback" : "");
}

// ---------------------------------------------------------------------------
// 6. Differential correctness test.
// ---------------------------------------------------------------------------

static bool diff_check(const Workload &w, uint32_t topn) {
    std::vector<uint8_t> buf_a, buf_b;
    std::vector<bench::SortData> sd_a, sd_b;
    bench::LazyScratch scratch;
    bench::baseline_sort(w.fields, w.docIds, topn, buf_a, sd_a);
    bench::lazy_topk_sort(w.fields, w.docIds, topn, buf_b, sd_b, scratch);

    uint32_t k = std::min(topn, uint32_t(w.docIds.size()));
    if (sd_b.size() < k) {
        std::fprintf(stderr, "diff: lazy returned only %zu, expected %u\n", sd_b.size(), k);
        return false;
    }
    // Compare the first k entries by the actual sortdata bytes (not docId,
    // because ties may break differently). We require byte-for-byte equality
    // of the sort-key stream.
    for (uint32_t i = 0; i < k; ++i) {
        const uint8_t *pa = buf_a.data() + sd_a[i].idx;
        const uint8_t *pb = buf_b.data() + sd_b[i].idx;
        uint32_t la = sd_a[i].len;
        uint32_t lb = sd_b[i].len;
        if (la != lb || std::memcmp(pa, pb, la) != 0) {
            std::fprintf(stderr, "diff mismatch at rank %u (N=%zu, K=%u)\n", i, w.docIds.size(),
                         topn);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 7. main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    bool diff_only = false;
    uint32_t one_n = 0, one_k = 0, one_f = 0;
    uint64_t seed = 0xC0FFEE;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--diff") diff_only = true;
        else if (a == "--n" && i + 1 < argc) one_n = std::atoi(argv[++i]);
        else if (a == "--k" && i + 1 < argc) one_k = std::atoi(argv[++i]);
        else if (a == "--f" && i + 1 < argc) one_f = std::atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 1;
        }
    }

    // Differential correctness test across a grid.
    {
        std::printf("# differential correctness test ...\n");
        int cases = 0, failed = 0;
        for (uint32_t n : {100u, 1024u, 10000u, 50000u}) {
            for (uint32_t k : {1u, 10u, 100u, 1000u}) {
                if (k >= n) continue;
                for (uint32_t f : {1u, 2u, 3u}) {
                    for (uint32_t card : {0u, 16u, 1024u}) {
                        auto w = make_workload(n, f, card, seed + cases);
                        ++cases;
                        if (!diff_check(w, k)) {
                            ++failed;
                            std::fprintf(stderr, "  FAIL n=%u k=%u f=%u card=%u\n", n, k, f, card);
                        }
                    }
                }
            }
        }
        std::printf("# diff: %d/%d passed\n", cases - failed, cases);
        if (failed) return 1;
    }
    if (diff_only) return 0;

    // Benchmark grid.
    std::printf("\n# benchmark grid (best-of-5, us per sort)\n");
    print_header();

    auto bench_point = [&](uint32_t n, uint32_t k, uint32_t f, uint32_t card,
                           uint32_t slowLoads = 0) {
        auto w = make_workload(n, f, card, seed, slowLoads);
        Row r{n, k, f, card, 0, 0, 0, false};
        std::vector<uint8_t> buf;
        std::vector<bench::SortData> sd;
        bench::LazyScratch scratch;
        // Warm all buffers once so timing reflects steady-state, not first-alloc.
        bench::baseline_sort(w.fields, w.docIds, k, buf, sd);
        bench::partial_sort_path(w.fields, w.docIds, k, buf, sd);
        bench::lazy_topk_sort(w.fields, w.docIds, k, buf, sd, scratch);
        r.baseline_us = run_one([&] { bench::baseline_sort(w.fields, w.docIds, k, buf, sd); }, 5);
        r.partial_us = run_one([&] { bench::partial_sort_path(w.fields, w.docIds, k, buf, sd); }, 5);
        bool fellBack = false;
        r.lazy_us = run_one(
            [&] {
                bool fb = false;
                bench::lazy_topk_sort(w.fields, w.docIds, k, buf, sd, scratch, &fb);
                if (fb) fellBack = true;
            },
            5);
        r.lazy_fallback = fellBack;
        print_row(r);
    };

    if (one_n) {
        uint32_t k = one_k ? one_k : 20;
        uint32_t f = one_f ? one_f : 2;
        bench_point(one_n, k, f, 0);
        return 0;
    }

    // Default grid: cheap numeric fields.
    std::printf("## cheap numeric fields (all int32)\n");
    for (uint32_t n : {10000u, 100000u, 1000000u}) {
        for (uint32_t k : {10u, 100u, 1000u}) {
            for (uint32_t f : {1u, 2u, 3u}) {
                bench_point(n, k, f, 0);
            }
        }
        // Low-cardinality sweep to show fallback behavior.
        bench_point(n, 100, 2, 16);
        std::printf("\n");
    }

    // Expensive tail fields: field 0 is cheap int32, fields 1..F-1 simulate
    // string / UCA collation encode cost. This is the regime where lazy
    // really pays: the N hits pay only cheap field0 encoding, and only the
    // small candidate set pays the expensive tail encoding.
    std::printf("## expensive tail fields (field0 cheap int32, rest ~= UCA collator)\n");
    print_header();
    for (uint32_t n : {10000u, 100000u, 1000000u}) {
        for (uint32_t k : {10u, 100u}) {
            for (uint32_t f : {2u, 3u}) {
                bench_point(n, k, f, 0, /*slowLoads=*/64);
            }
        }
        std::printf("\n");
    }
    return 0;
}
