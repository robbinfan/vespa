// Realistic benchmark: NLT vs PageDict4 with multi-file/multi-level simulation
//
// PD4 modeled as 3 separate memory regions (.ssdat, .spdat, .pdat) with 7-level lookup.
// Includes flush (batch build) and fusion (merge N dictionaries) benchmarks.
// Scales up to 5M terms with cache-line analysis.

#include <vespa/vespalib/succinct/nested_louds_trie.h>
#include <vespa/vespalib/succinct/rank_select.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <queue>

static void escape(void *p) { asm volatile("" : : "g"(p) : "memory"); }
static void clobber() { asm volatile("" ::: "memory"); }

using vespalib::succinct::NestedLoudsTrie;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Realistic PageDict4 Simulation
//
// Real PD4 has 3 files, 7 levels of hierarchy:
//   .ssdat: L7 (in-memory index) -> L6 (sparse-sparse entries)
//   .spdat: L5 -> L4 -> L3 (sparse page levels)
//   .pdat:  L2 -> L1 -> L0 (page levels, L0 = leaf terms)
//
// A lookup touches all 3 files: .ssdat -> .spdat -> .pdat
// Each file is a separate mmap region, so cache lines are spread across
// 3 different virtual address ranges.
//
// We simulate this with 3 separate allocations, each representing one file.
// Lookup must jump between them, matching real cache behavior.
// ============================================================================

struct RealisticPD4 {
    static constexpr uint32_t PAGE_SIZE = 4096;
    static constexpr uint32_t TERMS_PER_PAGE = 32;      // ~32 terms fit in a 4KB page (conservative for long keys)
    static constexpr uint32_t PAGES_PER_SPARSE = 128;    // L3 covers ~128 pages
    static constexpr uint32_t SPARSE_PER_SS = 64;        // L6 covers ~64 sparse pages

    // .pdat: pages of sorted terms (L0-L2)
    // Each page: sorted terms + LCP-compressed strings + skip entries
    // In real PD4, a page is 4KB with LCP-compressed terms.
    // We use a larger buffer to handle long keys (URLs can be 80+ bytes).
    // The disk size calculation still uses PAGE_SIZE.
    static constexpr uint32_t PAGE_DATA_SIZE = 4096;
    struct PdatPage {
        uint32_t numTerms;
        uint32_t firstTermIdx;
        uint16_t termOffsets[TERMS_PER_PAGE + 1];
        char termData[PAGE_DATA_SIZE];
    };

    // .spdat: sparse pages indexing .pdat pages (L3-L5)
    struct SpdatEntry {
        uint32_t pdatPageNum;     // which .pdat page
        uint32_t wordNum;         // cumulative word count
        char word[64];            // boundary word (LCP-compressed in real PD4)
        uint8_t wordLen;
    };

    // .ssdat: sparse-sparse index over .spdat (L6-L7)
    struct SsdatEntry {
        uint32_t spdatIdx;        // index into spdat entries
        uint32_t pdatPageNum;
        uint32_t wordNum;
        char word[64];
        uint8_t wordLen;
    };

    // 3 separate memory regions — simulating 3 mmap'd files
    std::vector<PdatPage> _pdat;         // File 1: .pdat
    std::vector<SpdatEntry> _spdat;      // File 2: .spdat
    std::vector<SsdatEntry> _ssdat;      // File 3: .ssdat (L7 in-memory)

    // Full term list for verification (not part of PD4, just for benchmark)
    std::vector<std::string> _allTerms;
    size_t _memoryUsage = 0;

    void build(const std::vector<std::string> &terms) {
        _allTerms = terms;
        uint32_t n = terms.size();
        if (n == 0) return;

        // Build .pdat pages
        uint32_t numPages = (n + TERMS_PER_PAGE - 1) / TERMS_PER_PAGE;
        _pdat.resize(numPages);
        memset(_pdat.data(), 0, numPages * sizeof(PdatPage));

        for (uint32_t p = 0; p < numPages; ++p) {
            auto &page = _pdat[p];
            page.firstTermIdx = p * TERMS_PER_PAGE;
            page.numTerms = std::min(TERMS_PER_PAGE, n - page.firstTermIdx);

            // Pack terms into page (simplified LCP compression)
            uint32_t offset = 0;
            for (uint32_t i = 0; i < page.numTerms; ++i) {
                uint32_t idx = page.firstTermIdx + i;
                page.termOffsets[i] = offset;
                uint32_t len = std::min((uint32_t)terms[idx].size(),
                                        (uint32_t)(PAGE_DATA_SIZE - offset - 1));
                memcpy(page.termData + offset, terms[idx].data(), len);
                page.termData[offset + len] = '\0';
                offset += len + 1;
            }
            page.termOffsets[page.numTerms] = offset;
        }

        // Build .spdat entries (one per PAGES_PER_SPARSE pages)
        uint32_t numSparse = (numPages + PAGES_PER_SPARSE - 1) / PAGES_PER_SPARSE;
        _spdat.resize(numSparse);
        for (uint32_t s = 0; s < numSparse; ++s) {
            auto &entry = _spdat[s];
            entry.pdatPageNum = s * PAGES_PER_SPARSE;
            entry.wordNum = entry.pdatPageNum * TERMS_PER_PAGE;
            uint32_t termIdx = std::min(entry.wordNum, n - 1);
            uint32_t len = std::min((uint32_t)terms[termIdx].size(), (uint32_t)63);
            memcpy(entry.word, terms[termIdx].data(), len);
            entry.word[len] = '\0';
            entry.wordLen = len;
        }

        // Build .ssdat entries (one per SPARSE_PER_SS sparse entries)
        uint32_t numSS = (numSparse + SPARSE_PER_SS - 1) / SPARSE_PER_SS;
        _ssdat.resize(numSS);
        for (uint32_t ss = 0; ss < numSS; ++ss) {
            auto &entry = _ssdat[ss];
            entry.spdatIdx = ss * SPARSE_PER_SS;
            uint32_t sparseIdx = std::min(entry.spdatIdx, numSparse - 1);
            entry.pdatPageNum = _spdat[sparseIdx].pdatPageNum;
            entry.wordNum = _spdat[sparseIdx].wordNum;
            uint32_t termIdx = std::min(entry.wordNum, n - 1);
            uint32_t len = std::min((uint32_t)terms[termIdx].size(), (uint32_t)63);
            memcpy(entry.word, terms[termIdx].data(), len);
            entry.word[len] = '\0';
            entry.wordLen = len;
        }

        _memoryUsage = _pdat.size() * sizeof(PdatPage)
                      + _spdat.size() * sizeof(SpdatEntry)
                      + _ssdat.size() * sizeof(SsdatEntry);
    }

    // Realistic 7-level lookup: .ssdat -> .spdat -> .pdat
    // Each step touches a different memory region (different mmap file)
    bool lookup(const char *key, uint32_t keyLen) const {
        if (_ssdat.empty()) return false;

        // L7: Binary search on .ssdat (in-memory index)
        uint32_t ssLo = 0, ssHi = _ssdat.size();
        while (ssLo + 1 < ssHi) {
            uint32_t mid = ssLo + (ssHi - ssLo) / 2;
            int cmp = strncmp(_ssdat[mid].word, key, std::min((uint32_t)_ssdat[mid].wordLen, keyLen));
            if (cmp == 0) cmp = (int)_ssdat[mid].wordLen - (int)keyLen;
            if (cmp <= 0) ssLo = mid; else ssHi = mid;
        }
        // Now ssLo is the .ssdat entry that covers our key

        // L5-L3: Linear/binary search within .spdat range
        uint32_t spStart = _ssdat[ssLo].spdatIdx;
        uint32_t spEnd = (ssLo + 1 < _ssdat.size()) ? _ssdat[ssLo + 1].spdatIdx : _spdat.size();
        spEnd = std::min(spEnd, (uint32_t)_spdat.size());

        uint32_t spLo = spStart, spHi = spEnd;
        while (spLo + 1 < spHi) {
            uint32_t mid = spLo + (spHi - spLo) / 2;
            int cmp = strncmp(_spdat[mid].word, key, std::min((uint32_t)_spdat[mid].wordLen, keyLen));
            if (cmp == 0) cmp = (int)_spdat[mid].wordLen - (int)keyLen;
            if (cmp <= 0) spLo = mid; else spHi = mid;
        }
        // spLo is the sparse entry -> gives us the .pdat page range

        // L2-L0: Search within .pdat pages
        uint32_t pageStart = _spdat[spLo].pdatPageNum;
        uint32_t pageEnd = (spLo + 1 < _spdat.size()) ? _spdat[spLo + 1].pdatPageNum : _pdat.size();
        pageEnd = std::min(pageEnd, (uint32_t)_pdat.size());

        // Binary search on pages (L2 skip)
        uint32_t pLo = pageStart, pHi = pageEnd;
        while (pLo + 1 < pHi) {
            uint32_t mid = pLo + (pHi - pLo) / 2;
            const auto &pg = _pdat[mid];
            const char *firstWord = pg.termData + pg.termOffsets[0];
            int cmp = strcmp(firstWord, key);
            if (cmp <= 0) pLo = mid; else pHi = mid;
        }

        // L0: Linear scan within the page
        const auto &page = _pdat[pLo];
        for (uint32_t i = 0; i < page.numTerms; ++i) {
            const char *w = page.termData + page.termOffsets[i];
            int cmp = strcmp(w, key);
            if (cmp == 0) return true;
            if (cmp > 0) return false;
        }
        return false;
    }

    bool lookup(const std::string &word) const {
        return lookup(word.c_str(), word.size());
    }

    // Sequential scan for fusion
    void seqScan(size_t &count) const {
        count = 0;
        for (auto &page : _pdat) {
            for (uint32_t i = 0; i < page.numTerms; ++i) {
                const char *w = page.termData + page.termOffsets[i];
                escape((void*)w);
                ++count;
            }
        }
    }

    size_t diskSize() const {
        return _pdat.size() * PAGE_SIZE   // .pdat
             + _spdat.size() * 80         // .spdat (approx entry size)
             + _ssdat.size() * 80;        // .ssdat
    }
};

// ============================================================================
// NLT Dictionary Wrapper
// ============================================================================

struct NltDict {
    std::unique_ptr<NestedLoudsTrie> _nlt;
    size_t _memoryUsage = 0;

    void build(const std::vector<std::string> &terms) {
        NestedLoudsTrie::Builder builder;
        for (auto &t : terms) builder.add(t);
        _nlt = builder.finish();
        _memoryUsage = _nlt->memory_usage();
    }

    bool lookup(const std::string &word) const {
        return _nlt->lookup(word) != NestedLoudsTrie::NOT_FOUND;
    }

    size_t seqScan() const {
        size_t count = 0;
        auto it = _nlt->begin();
        while (it.valid()) {
            escape((void*)it.key().data());
            ++count;
            it.next();
        }
        return count;
    }
};

// ============================================================================
// Term Generators
// ============================================================================

std::vector<std::string> genRandomTerms(uint32_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::set<std::string> unique;
    while (unique.size() < n) {
        uint32_t len = 5 + (rng() % 20);
        std::string s;
        for (uint32_t i = 0; i < len; ++i) s += 'a' + (rng() % 26);
        unique.insert(s);
    }
    return {unique.begin(), unique.end()};
}

std::vector<std::string> genUrlTerms(uint32_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::set<std::string> unique;
    const char *prefixes[] = {
        "https://www.example.com/products/category/",
        "https://www.example.com/blog/posts/",
        "https://api.service.io/v2/endpoints/",
        "https://docs.platform.org/reference/",
        "https://cdn.assets.net/images/gallery/",
        "https://app.dashboard.com/settings/users/",
        "https://store.vendor.biz/items/detail/",
        "https://wiki.internal.corp/pages/",
    };
    int npfx = sizeof(prefixes) / sizeof(prefixes[0]);
    while (unique.size() < n) {
        std::string s = prefixes[rng() % npfx];
        uint32_t suffixLen = 8 + (rng() % 30);
        for (uint32_t i = 0; i < suffixLen; ++i) {
            int c = rng() % 36;
            s += (c < 26) ? ('a' + c) : ('0' + c - 26);
        }
        unique.insert(s);
    }
    return {unique.begin(), unique.end()};
}

std::vector<std::string> genNegTerms(const std::vector<std::string> &terms,
                                      uint32_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::set<std::string> termSet(terms.begin(), terms.end());
    std::vector<std::string> neg;
    while (neg.size() < n) {
        uint32_t len = 5 + (rng() % 20);
        std::string s;
        for (uint32_t i = 0; i < len; ++i) s += 'a' + (rng() % 26);
        if (termSet.find(s) == termSet.end()) neg.push_back(s);
    }
    return neg;
}

// ============================================================================
// Fusion Simulation: merge K sorted dictionaries into one
// ============================================================================

double fusionBenchPD4(const std::vector<std::vector<std::string>> &segments) {
    // Simulate PD4 fusion: K-way merge of sorted term lists
    // Each segment is a separate PD4 dictionary
    // Output: build a new PD4 from merged stream
    using Iter = std::pair<size_t, size_t>; // (segment_idx, pos_in_segment)
    auto cmp = [&](const Iter &a, const Iter &b) {
        return segments[a.first][a.second] > segments[b.first][b.second];
    };
    std::priority_queue<Iter, std::vector<Iter>, decltype(cmp)> pq(cmp);

    auto t0 = Clock::now();
    for (size_t i = 0; i < segments.size(); ++i) {
        if (!segments[i].empty()) pq.push({i, 0});
    }

    // Build output PD4
    std::vector<std::string> merged;
    merged.reserve(segments.size() * (segments.empty() ? 0 : segments[0].size()));
    std::string lastWord;

    while (!pq.empty()) {
        auto [si, pos] = pq.top();
        pq.pop();
        const auto &word = segments[si][pos];
        if (merged.empty() || word != lastWord) {
            merged.push_back(word);
            lastWord = word;
        }
        if (pos + 1 < segments[si].size()) {
            pq.push({si, pos + 1});
        }
    }

    // Build output dictionary
    RealisticPD4 output;
    output.build(merged);
    escape(&output);
    clobber();

    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double fusionBenchNLT(const std::vector<std::vector<std::string>> &segments) {
    using Iter = std::pair<size_t, size_t>;
    auto cmp = [&](const Iter &a, const Iter &b) {
        return segments[a.first][a.second] > segments[b.first][b.second];
    };
    std::priority_queue<Iter, std::vector<Iter>, decltype(cmp)> pq(cmp);

    auto t0 = Clock::now();
    for (size_t i = 0; i < segments.size(); ++i) {
        if (!segments[i].empty()) pq.push({i, 0});
    }

    // K-way merge then build NLT
    std::vector<std::string> merged;
    merged.reserve(segments.size() * (segments.empty() ? 0 : segments[0].size()));
    std::string lastWord;

    while (!pq.empty()) {
        auto [si, pos] = pq.top();
        pq.pop();
        const auto &word = segments[si][pos];
        if (merged.empty() || word != lastWord) {
            merged.push_back(word);
            lastWord = word;
        }
        if (pos + 1 < segments[si].size()) {
            pq.push({si, pos + 1});
        }
    }

    // Build NLT from merged
    NltDict output;
    output.build(merged);
    escape(&output);
    clobber();

    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// ============================================================================
// Benchmark Runner
// ============================================================================

void runLookupBench(const char *desc, const std::vector<std::string> &terms,
                    const std::vector<std::string> &negTerms, uint32_t numQueries) {
    uint32_t n = terms.size();
    fprintf(stderr, "\n━━━ %s (%u terms, %u queries) ━━━\n", desc, n, numQueries);

    // --- Build ---
    RealisticPD4 pd4;
    auto t0 = Clock::now();
    pd4.build(terms);
    auto t1 = Clock::now();
    double pd4BuildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    NltDict nlt;
    t0 = Clock::now();
    nlt.build(terms);
    t1 = Clock::now();
    double nltBuildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // --- Memory / Disk ---
    size_t rawKeyBytes = 0;
    for (auto &t : terms) rawKeyBytes += t.size();

    fprintf(stderr, "\n  ┌─ Flush (Build) ──────────────────────────────────────┐\n");
    fprintf(stderr, "  │  PD4 build:  %10.1f ms                            │\n", pd4BuildMs);
    fprintf(stderr, "  │  NLT build:  %10.1f ms  (%.1fx)                   │\n",
            nltBuildMs, nltBuildMs / pd4BuildMs);
    fprintf(stderr, "  └──────────────────────────────────────────────────────┘\n");

    fprintf(stderr, "\n  ┌─ Memory / Disk ───────────────────────────────────────┐\n");
    fprintf(stderr, "  │  Raw keys:      %12zu bytes                     │\n", rawKeyBytes);
    fprintf(stderr, "  │  PD4 memory:    %12zu bytes (%5.1f bits/key)    │\n",
            pd4._memoryUsage, pd4._memoryUsage * 8.0 / n);
    fprintf(stderr, "  │  PD4 .pdat:     %12zu pages (%zu KB)            │\n",
            pd4._pdat.size(), pd4._pdat.size() * 4);
    fprintf(stderr, "  │  PD4 .spdat:    %12zu entries                   │\n", pd4._spdat.size());
    fprintf(stderr, "  │  PD4 .ssdat:    %12zu entries                   │\n", pd4._ssdat.size());
    fprintf(stderr, "  │  PD4 disk:      %12zu bytes                     │\n", pd4.diskSize());
    fprintf(stderr, "  │  NLT memory:    %12zu bytes (%5.1f bits/key)    │\n",
            nlt._memoryUsage, nlt._memoryUsage * 8.0 / n);
    fprintf(stderr, "  │  NLT/PD4 mem:   %12.2fx                        │\n",
            (double)nlt._memoryUsage / pd4._memoryUsage);
    fprintf(stderr, "  └──────────────────────────────────────────────────────┘\n");

    // --- Prepare queries ---
    std::mt19937 rng(777);
    std::uniform_int_distribution<uint32_t> posDist(0, terms.size() - 1);
    std::uniform_int_distribution<uint32_t> negDist(0, negTerms.size() - 1);
    std::vector<uint32_t> posIdx(numQueries), negIdx(numQueries);
    for (auto &i : posIdx) i = posDist(rng);
    for (auto &i : negIdx) i = negDist(rng);

    // --- Positive lookups ---
    uint64_t cs = 0;
    t0 = Clock::now();
    for (auto idx : posIdx) {
        bool found = pd4.lookup(terms[idx]);
        cs += found;
        escape(&found);
    }
    clobber();
    t1 = Clock::now();
    double pd4PosNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / (double)numQueries;

    t0 = Clock::now();
    for (auto idx : posIdx) {
        bool found = nlt.lookup(terms[idx]);
        cs += found;
        escape(&found);
    }
    clobber();
    t1 = Clock::now();
    double nltPosNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / (double)numQueries;

    // --- Negative lookups ---
    t0 = Clock::now();
    for (auto idx : negIdx) {
        bool found = pd4.lookup(negTerms[idx]);
        cs += found;
        escape(&found);
    }
    clobber();
    t1 = Clock::now();
    double pd4NegNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / (double)numQueries;

    t0 = Clock::now();
    for (auto idx : negIdx) {
        bool found = nlt.lookup(negTerms[idx]);
        cs += found;
        escape(&found);
    }
    clobber();
    t1 = Clock::now();
    double nltNegNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / (double)numQueries;
    escape(&cs);

    fprintf(stderr, "\n  ┌─ Lookup Latency (ns/op) ────────────────────────────────────────┐\n");
    fprintf(stderr, "  │              Positive              Negative                      │\n");
    fprintf(stderr, "  │  PD4 (3-file): %8.1f ns          %8.1f ns                   │\n", pd4PosNs, pd4NegNs);
    fprintf(stderr, "  │  NLT:          %8.1f ns          %8.1f ns                   │\n", nltPosNs, nltNegNs);
    fprintf(stderr, "  │  NLT/PD4:      %8.2fx            %8.2fx                    │\n",
            nltPosNs / pd4PosNs, nltNegNs / pd4NegNs);
    fprintf(stderr, "  └──────────────────────────────────────────────────────────────────┘\n");

    // --- Cache analysis ---
    fprintf(stderr, "\n  ┌─ Cache Working Set Analysis ────────────────────────────────────┐\n");
    fprintf(stderr, "  │  PD4 mmap regions:  3 files                                    │\n");
    fprintf(stderr, "  │    .ssdat: %6zu entries x %3zu B = %8zu B  (L7 index)      │\n",
            pd4._ssdat.size(), sizeof(RealisticPD4::SsdatEntry),
            pd4._ssdat.size() * sizeof(RealisticPD4::SsdatEntry));
    fprintf(stderr, "  │    .spdat: %6zu entries x %3zu B = %8zu B  (L3-L5)        │\n",
            pd4._spdat.size(), sizeof(RealisticPD4::SpdatEntry),
            pd4._spdat.size() * sizeof(RealisticPD4::SpdatEntry));
    fprintf(stderr, "  │    .pdat:  %6zu pages   x %3u B = %8zu B  (L0-L2)        │\n",
            pd4._pdat.size(), RealisticPD4::PAGE_SIZE,
            pd4._pdat.size() * sizeof(RealisticPD4::PdatPage));
    fprintf(stderr, "  │  PD4 cache lines per lookup: ~%u (across 3 mmap regions)       │\n",
            (uint32_t)(2 + 2 + 3)); // ssdat bsearch + spdat bsearch + pdat page scan
    fprintf(stderr, "  │  NLT contiguous region:  %zu B (single allocation)             │\n",
            nlt._memoryUsage);
    fprintf(stderr, "  │  NLT cache lines per lookup: ~%u (all in one region)           │\n",
            (uint32_t)(2 + 2)); // trie walk + labels, ~4 cache lines total
    fprintf(stderr, "  └──────────────────────────────────────────────────────────────────┘\n");

    // --- Sequential scan ---
    t0 = Clock::now();
    size_t pd4Count = 0;
    pd4.seqScan(pd4Count);
    t1 = Clock::now();
    double pd4SeqMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    t0 = Clock::now();
    size_t nltCount = nlt.seqScan();
    t1 = Clock::now();
    double nltSeqMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    escape(&pd4Count); escape(&nltCount);

    fprintf(stderr, "\n  ┌─ Sequential Scan ─────────────────────────────────────────────┐\n");
    fprintf(stderr, "  │  PD4 scan: %10.2f ms (%zu terms)                           │\n",
            pd4SeqMs, pd4Count);
    fprintf(stderr, "  │  NLT scan: %10.2f ms (%zu terms)                           │\n",
            nltSeqMs, nltCount);
    fprintf(stderr, "  │  NLT/PD4:  %10.2fx                                        │\n",
            nltSeqMs / pd4SeqMs);
    fprintf(stderr, "  └────────────────────────────────────────────────────────────────┘\n");
}

void runFusionBench(const char *desc, uint32_t termsPerSegment,
                    uint32_t numSegments, uint32_t seed) {
    fprintf(stderr, "\n━━━ Fusion: %s (%u segments x %u terms) ━━━\n",
            desc, numSegments, termsPerSegment);

    std::mt19937 rng(seed);
    std::vector<std::vector<std::string>> segments(numSegments);

    // Generate segments — each has unique terms, some overlap
    auto t0 = Clock::now();
    for (uint32_t s = 0; s < numSegments; ++s) {
        segments[s] = genRandomTerms(termsPerSegment, seed + s * 1000);
    }
    auto t1 = Clock::now();
    double genMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Count total unique terms after merge
    std::set<std::string> allUnique;
    for (auto &seg : segments) {
        allUnique.insert(seg.begin(), seg.end());
    }
    uint32_t totalUnique = allUnique.size();

    fprintf(stderr, "  Segments generated: %.1f ms, %u unique terms total\n\n", genMs, totalUnique);

    // Fusion PD4
    double pd4FusionMs = fusionBenchPD4(segments);

    // Fusion NLT
    double nltFusionMs = fusionBenchNLT(segments);

    fprintf(stderr, "  ┌─ Fusion Performance ──────────────────────────────────┐\n");
    fprintf(stderr, "  │  PD4 fusion: %10.1f ms                             │\n", pd4FusionMs);
    fprintf(stderr, "  │  NLT fusion: %10.1f ms  (%.2fx)                    │\n",
            nltFusionMs, nltFusionMs / pd4FusionMs);
    fprintf(stderr, "  │  Output terms: %u unique                             │\n", totalUnique);
    fprintf(stderr, "  └──────────────────────────────────────────────────────┘\n");
}

int main() {
    fprintf(stderr, "╔═══════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  NLT vs PageDict4 Realistic Benchmark                       ║\n");
    fprintf(stderr, "║  PD4: 3-file (.ssdat/.spdat/.pdat) 7-level simulation       ║\n");
    fprintf(stderr, "║  NLT: Single contiguous structure                           ║\n");
    fprintf(stderr, "╚═══════════════════════════════════════════════════════════════╝\n");

    // ---- Scale tests: random terms ----
    uint32_t scales[] = {100000, 500000, 1000000, 2000000, 5000000};
    for (auto sz : scales) {
        auto terms = genRandomTerms(sz, 42);
        auto neg = genNegTerms(terms, std::min(200000u, sz), 999);
        uint32_t nq = std::min(1000000u, sz * 2);

        char desc[128];
        snprintf(desc, sizeof(desc), "Random alpha terms (%uK)", sz / 1000);
        runLookupBench(desc, terms, neg, nq);
    }

    // ---- URL terms at large scale ----
    {
        auto terms = genUrlTerms(1000000, 99);
        auto neg = genNegTerms(terms, 200000, 888);
        runLookupBench("URL-like terms (1M)", terms, neg, 1000000);
    }

    // ---- Fusion benchmarks ----
    fprintf(stderr, "\n\n╔═══════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  Fusion Benchmarks (merge K segments into one)              ║\n");
    fprintf(stderr, "╚═══════════════════════════════════════════════════════════════╝\n");

    // Small segments (like flush -> merge)
    runFusionBench("Small segments", 50000, 5, 100);
    runFusionBench("Medium segments", 200000, 5, 200);
    runFusionBench("Large segments", 500000, 5, 300);
    runFusionBench("Many small segments", 100000, 20, 400);

    fprintf(stderr, "\n╔═══════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║                    Benchmark Complete                        ║\n");
    fprintf(stderr, "╚═══════════════════════════════════════════════════════════════╝\n");
    return 0;
}
