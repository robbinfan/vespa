// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
//
// Benchmark: PTrie vs Vespa's actual BTree for memory index dictionary.
//
// This uses the REAL Vespa BTree (vespalib::btree::BTree) with:
// - DataStore-backed node allocation
// - GenerationHandler RCU for freeze/commit cycle
// - String comparison through EntryRef indirection (like KeyComp in FieldIndex)
//
// NOT a simplified BTree — this is the exact same code path as production.

#include <vespa/vespalib/btree/btree.h>
#include <vespa/vespalib/btree/btree.hpp>
#include <vespa/vespalib/btree/btreeroot.hpp>
#include <vespa/vespalib/btree/btreenodeallocator.hpp>
#include <vespa/vespalib/btree/btreeiterator.hpp>
#include <vespa/vespalib/btree/btreenode.hpp>
#include <vespa/vespalib/btree/btreenodestore.hpp>
#include <vespa/vespalib/datastore/buffer_type.hpp>
#include <vespa/vespalib/util/generationhandler.h>
#include <vespa/vespalib/btree/btreebuilder.hpp>
#include <vespa/vespalib/btree/btreeinserter.hpp>
#include <vespa/vespalib/btree/btreeremover.hpp>
#include <vespa/vespalib/ptrie/patricia_trie.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using vespalib::GenerationHandler;
using vespalib::datastore::EntryRef;
using namespace vespalib::btree;
using namespace vespalib::ptrie;

using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// Lightweight WordStore — mimics search::memoryindex::WordStore
// ============================================================================
// Stores words in a contiguous buffer, returns EntryRef-like 32-bit handles.
// This avoids pulling in the full searchlib dependency while faithfully
// reproducing the indirection cost of the real WordStore.

class BenchWordStore {
public:
    BenchWordStore() : _buffer(), _offsets() {
        _buffer.reserve(16 * 1024 * 1024);
        // Offset 0 is invalid sentinel.
        _buffer.push_back('\0');
        _offsets.push_back(0);
    }

    // Add a word, return a 32-bit reference (1-based index into _offsets).
    uint32_t addWord(const char* word, size_t len) {
        uint32_t ref = static_cast<uint32_t>(_offsets.size());
        _offsets.push_back(static_cast<uint32_t>(_buffer.size()));
        _buffer.insert(_buffer.end(), word, word + len);
        _buffer.push_back('\0');
        return ref;
    }

    const char* getWord(uint32_t ref) const {
        return _buffer.data() + _offsets[ref];
    }

private:
    std::vector<char>     _buffer;
    std::vector<uint32_t> _offsets;
};

// ============================================================================
// WordKey + KeyComp — exact analog of FieldIndexBase::WordKey/KeyComp
// ============================================================================

struct WordKey {
    uint32_t _wordRef;
    explicit WordKey(uint32_t ref = 0) : _wordRef(ref) {}
};

class KeyComp {
    const BenchWordStore& _store;
    const char* _word;  // For lookup-by-string (ref=0 case)

    const char* getWord(uint32_t ref) const {
        if (ref != 0) return _store.getWord(ref);
        return _word;
    }
public:
    KeyComp(const BenchWordStore& store, const char* word = "")
        : _store(store), _word(word) {}

    bool operator()(const WordKey& lhs, const WordKey& rhs) const {
        return strcmp(getWord(lhs._wordRef), getWord(rhs._wordRef)) < 0;
    }
};

// ============================================================================
// BTree type — matches production DictionaryTree exactly
// ============================================================================

using DictBTree = BTree<WordKey, uint32_t, NoAggregated, const KeyComp>;

// ============================================================================
// Test word generation
// ============================================================================

static std::vector<std::string> generateWords(int n, unsigned seed = 42) {
    // Generate realistic term vocabulary with prefix sharing.
    std::mt19937 rng(seed);
    std::vector<std::string> words;
    words.reserve(n);

    // Common prefixes to simulate real text indexing.
    const char* prefixes[] = {
        "the", "comp", "inter", "pre", "pro", "re", "un", "dis",
        "over", "under", "out", "multi", "semi", "auto", "anti",
        "micro", "macro", "super", "sub", "trans", "cross", "self",
        "post", "non", "mis", "counter", "co", "de", "fore", "mid"
    };
    const int num_prefixes = sizeof(prefixes) / sizeof(prefixes[0]);

    const char* suffixes[] = {
        "tion", "ment", "ness", "able", "ible", "ful", "less", "ous",
        "ive", "ing", "ated", "ical", "ity", "ism", "ist", "ize",
        "ward", "wise", "like", "ship", "dom", "hood", "ery", "ance"
    };
    const int num_suffixes = sizeof(suffixes) / sizeof(suffixes[0]);

    std::uniform_int_distribution<int> prefix_dist(0, num_prefixes - 1);
    std::uniform_int_distribution<int> suffix_dist(0, num_suffixes - 1);
    std::uniform_int_distribution<int> mid_len(2, 6);
    std::uniform_int_distribution<int> char_dist(0, 25);

    for (int i = 0; i < n; ++i) {
        std::string word = prefixes[prefix_dist(rng)];
        int ml = mid_len(rng);
        for (int j = 0; j < ml; ++j) {
            word += static_cast<char>('a' + char_dist(rng));
        }
        word += suffixes[suffix_dist(rng)];
        // Add numeric suffix to ensure uniqueness.
        word += std::to_string(i);
        words.push_back(std::move(word));
    }
    return words;
}

// ============================================================================
// Prevent compiler from optimizing away results
// ============================================================================

static void doNotOptimize(uint32_t val) {
    asm volatile("" : : "r"(val) : "memory");
}

// ============================================================================
// BTree benchmark: batch insert + freeze + frozen find
// ============================================================================

struct BTreeResult {
    double insert_ns_per_op;
    double find_ns_per_op;
    size_t memory_bytes;
    double memory_per_entry;
};

static BTreeResult benchBTree(const std::vector<std::string>& words, int batch_size,
                               int deferred_interval = 1) {
    const int N = static_cast<int>(words.size());

    // Phase 1: Build word store (shared infrastructure, not timed).
    BenchWordStore store;
    std::vector<uint32_t> refs;
    refs.reserve(N);
    for (const auto& w : words) {
        refs.push_back(store.addWord(w.data(), w.size()));
    }

    // Phase 2: BTree dictionary with full Vespa RCU lifecycle.
    DictBTree dict;
    GenerationHandler gen_handler;

    auto t0 = Clock::now();

    int batches_since_freeze = 0;
    for (int i = 0; i < N; i += batch_size) {
        int end = std::min(i + batch_size, N);
        KeyComp cmp(store);

        // Insert batch.
        for (int j = i; j < end; ++j) {
            dict.insert(WordKey(refs[j]), static_cast<uint32_t>(j), cmp);
        }

        batches_since_freeze++;
        if (batches_since_freeze >= deferred_interval) {
            // Full Vespa commit cycle: freeze + transferHoldLists + incGeneration + trimHoldLists
            dict.getAllocator().freeze();
            dict.getAllocator().transferHoldLists(gen_handler.getCurrentGeneration());
            gen_handler.incGeneration();
            gen_handler.updateFirstUsedGeneration();
            dict.getAllocator().trimHoldLists(gen_handler.getFirstUsedGeneration());
            batches_since_freeze = 0;
        }
    }

    // Final freeze if pending.
    if (batches_since_freeze > 0) {
        dict.getAllocator().freeze();
        dict.getAllocator().transferHoldLists(gen_handler.getCurrentGeneration());
        gen_handler.incGeneration();
        gen_handler.updateFirstUsedGeneration();
        dict.getAllocator().trimHoldLists(gen_handler.getFirstUsedGeneration());
    }

    auto t1 = Clock::now();
    double insert_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;

    // Phase 3: Frozen find benchmark.
    auto frozen = dict.getFrozenView();

    auto t2 = Clock::now();
    uint32_t found = 0;
    for (int i = 0; i < N; ++i) {
        KeyComp cmp(store, words[i].c_str());
        auto itr = frozen.find(WordKey(0), cmp);
        if (itr.valid()) {
            doNotOptimize(itr.getData());
            found++;
        }
    }
    auto t3 = Clock::now();
    double find_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / N;

    // Memory usage.
    auto mem = dict.getMemoryUsage();
    size_t total_mem = mem.usedBytes();

    fprintf(stderr, "  BTree found %u/%d entries\n", found, N);

    return {insert_ns, find_ns, total_mem, static_cast<double>(total_mem) / N};
}

// ============================================================================
// PTrie benchmark: batch insert + freeze + frozen find
// ============================================================================

struct PTrieResult {
    double insert_ns_per_op;
    double find_ns_per_op;
    size_t memory_bytes;
    double memory_per_entry;
};

static PTrieResult benchPTrie(const std::vector<std::string>& words, int batch_size,
                               int deferred_interval = 1) {
    const int N = static_cast<int>(words.size());

    PatriciaTrie trie;
    // Pre-reserve to avoid reallocation during concurrent reads.
    trie.reserveArena(static_cast<size_t>(N) * 400);

    auto t0 = Clock::now();

    int batches_since_freeze = 0;
    for (int i = 0; i < N; i += batch_size) {
        int end = std::min(i + batch_size, N);

        for (int j = i; j < end; ++j) {
            trie.insert(vespalib::stringref(words[j].data(), words[j].size()),
                       static_cast<uint32_t>(j));
        }

        batches_since_freeze++;
        if (batches_since_freeze >= deferred_interval) {
            trie.freeze();
            batches_since_freeze = 0;
        }
    }

    if (batches_since_freeze > 0) {
        trie.freeze();
    }

    auto t1 = Clock::now();
    double insert_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;

    // Phase 3: Frozen find benchmark.
    auto frozen = trie.getFrozenView();

    auto t2 = Clock::now();
    uint32_t found = 0;
    for (int i = 0; i < N; ++i) {
        uint32_t val;
        if (frozen.find(vespalib::stringref(words[i].data(), words[i].size()), val)) {
            doNotOptimize(val);
            found++;
        }
    }
    auto t3 = Clock::now();
    double find_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / N;

    auto mem = trie.getMemoryUsage();
    size_t total_mem = mem.usedBytes();

    fprintf(stderr, "  PTrie found %u/%d entries\n", found, N);

    return {insert_ns, find_ns, total_mem, static_cast<double>(total_mem) / N};
}

// ============================================================================
// Concurrent frozen find benchmark (simulates query threads)
// ============================================================================

static double benchConcurrentFrozenFind_BTree(const std::vector<std::string>& words,
                                               DictBTree& dict, BenchWordStore& store,
                                               GenerationHandler& gen_handler,
                                               int num_threads, int lookups_per_thread) {
    auto frozen = dict.getFrozenView();
    auto guard = gen_handler.takeGuard();

    std::atomic<uint64_t> total_found{0};
    std::vector<std::thread> threads;

    auto t0 = Clock::now();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            uint32_t found = 0;
            for (int i = 0; i < lookups_per_thread; ++i) {
                int idx = (t * lookups_per_thread + i) % static_cast<int>(words.size());
                KeyComp cmp(store, words[idx].c_str());
                auto itr = frozen.find(WordKey(0), cmp);
                if (itr.valid()) {
                    doNotOptimize(itr.getData());
                    found++;
                }
            }
            total_found.fetch_add(found, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();

    int total_ops = num_threads * lookups_per_thread;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / total_ops;
}

static double benchConcurrentFrozenFind_PTrie(const std::vector<std::string>& words,
                                               PatriciaTrie& trie,
                                               int num_threads, int lookups_per_thread) {
    auto frozen = trie.getFrozenView();

    std::atomic<uint64_t> total_found{0};
    std::vector<std::thread> threads;

    auto t0 = Clock::now();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            uint32_t found = 0;
            for (int i = 0; i < lookups_per_thread; ++i) {
                int idx = (t * lookups_per_thread + i) % static_cast<int>(words.size());
                uint32_t val;
                if (frozen.find(vespalib::stringref(words[idx].data(), words[idx].size()), val)) {
                    doNotOptimize(val);
                    found++;
                }
            }
            total_found.fetch_add(found, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();

    int total_ops = num_threads * lookups_per_thread;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / total_ops;
}

// ============================================================================
// Main benchmark
// ============================================================================

static void benchDictionaryBatchInsertAndFind() {
    const int SIZES[] = {100000, 500000, 1000000};
    const int BATCHES[] = {100, 1000, 5000};

    for (int N : SIZES) {
        auto words = generateWords(N);

        // Shuffle for random insertion order.
        std::mt19937 rng(12345);
        std::shuffle(words.begin(), words.end(), rng);

        fprintf(stderr, "\n=== N = %d ===\n", N);
        fprintf(stderr, "%-35s %10s %10s %10s %8s\n",
                "Mode", "Insert", "Find", "Memory", "B/entry");
        fprintf(stderr, "%-35s %10s %10s %10s %8s\n",
                "", "ns/op", "ns/op", "KB", "");
        fprintf(stderr, "%s\n", std::string(78, '-').c_str());

        for (int batch : BATCHES) {
            // --- Vespa BTree: freeze every batch ---
            {
                auto r = benchBTree(words, batch, 1);
                fprintf(stderr, "%-35s %10.0f %10.0f %10zu %8.0f\n",
                        (std::string("BTree freeze-every b=") + std::to_string(batch)).c_str(),
                        r.insert_ns_per_op, r.find_ns_per_op,
                        r.memory_bytes / 1024, r.memory_per_entry);
            }

            // --- Vespa BTree: deferred-50 ---
            {
                auto r = benchBTree(words, batch, 50);
                fprintf(stderr, "%-35s %10.0f %10.0f %10zu %8.0f\n",
                        (std::string("BTree deferred-50 b=") + std::to_string(batch)).c_str(),
                        r.insert_ns_per_op, r.find_ns_per_op,
                        r.memory_bytes / 1024, r.memory_per_entry);
            }

            // --- PTrie: freeze every batch ---
            {
                auto r = benchPTrie(words, batch, 1);
                fprintf(stderr, "%-35s %10.0f %10.0f %10zu %8.0f\n",
                        (std::string("PTrie freeze-every b=") + std::to_string(batch)).c_str(),
                        r.insert_ns_per_op, r.find_ns_per_op,
                        r.memory_bytes / 1024, r.memory_per_entry);
            }

            // --- PTrie: deferred-50 ---
            {
                auto r = benchPTrie(words, batch, 50);
                fprintf(stderr, "%-35s %10.0f %10.0f %10zu %8.0f\n",
                        (std::string("PTrie deferred-50 b=") + std::to_string(batch)).c_str(),
                        r.insert_ns_per_op, r.find_ns_per_op,
                        r.memory_bytes / 1024, r.memory_per_entry);
            }

            fprintf(stderr, "\n");
        }
    }
}

static void benchConcurrentFrozenFind() {
    const int N = 500000;
    const int NUM_THREADS = 4;
    const int LOOKUPS = 100000;

    auto words = generateWords(N);
    std::mt19937 rng(12345);
    std::shuffle(words.begin(), words.end(), rng);

    fprintf(stderr, "\n=== Concurrent Frozen Find: N=%d, %d threads, %d lookups/thread ===\n",
            N, NUM_THREADS, LOOKUPS);

    // Build BTree.
    BenchWordStore store;
    std::vector<uint32_t> refs;
    for (const auto& w : words) {
        refs.push_back(store.addWord(w.data(), w.size()));
    }
    DictBTree dict;
    GenerationHandler gen_handler;
    KeyComp cmp(store);
    for (int i = 0; i < N; ++i) {
        dict.insert(WordKey(refs[i]), static_cast<uint32_t>(i), cmp);
    }
    dict.getAllocator().freeze();
    dict.getAllocator().transferHoldLists(gen_handler.getCurrentGeneration());
    gen_handler.incGeneration();
    gen_handler.updateFirstUsedGeneration();
    dict.getAllocator().trimHoldLists(gen_handler.getFirstUsedGeneration());

    double btree_ns = benchConcurrentFrozenFind_BTree(words, dict, store, gen_handler,
                                                       NUM_THREADS, LOOKUPS);

    // Build PTrie.
    PatriciaTrie trie;
    trie.reserveArena(static_cast<size_t>(N) * 400);
    for (int i = 0; i < N; ++i) {
        trie.insert(vespalib::stringref(words[i].data(), words[i].size()),
                   static_cast<uint32_t>(i));
    }
    trie.freeze();

    double ptrie_ns = benchConcurrentFrozenFind_PTrie(words, trie, NUM_THREADS, LOOKUPS);

    fprintf(stderr, "BTree frozen find (%d threads): %.1f ns/op\n", NUM_THREADS, btree_ns);
    fprintf(stderr, "PTrie frozen find (%d threads): %.1f ns/op\n", NUM_THREADS, ptrie_ns);
    fprintf(stderr, "Speedup: %.1fx\n", btree_ns / ptrie_ns);
}

static void benchRCUGuardOverhead() {
    // Measure the actual GenerationHandler overhead, not a simulation.
    GenerationHandler gen_handler;
    gen_handler.incGeneration();

    const int ITERS = 10000000;

    // takeGuard + release.
    auto t0 = Clock::now();
    for (int i = 0; i < ITERS; ++i) {
        auto guard = gen_handler.takeGuard();
        doNotOptimize(static_cast<uint32_t>(guard.getGeneration()));
    }
    auto t1 = Clock::now();
    double guard_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / ITERS;

    // Acquire load only (PTrie equivalent).
    std::atomic<uint32_t> root{42};
    auto t2 = Clock::now();
    for (int i = 0; i < ITERS; ++i) {
        uint32_t r = root.load(std::memory_order_acquire);
        doNotOptimize(r);
    }
    auto t3 = Clock::now();
    double acquire_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / ITERS;

    fprintf(stderr, "\n=== RCU Guard Overhead (real GenerationHandler) ===\n");
    fprintf(stderr, "GenerationHandler takeGuard+release: %.1f ns/op\n", guard_ns);
    fprintf(stderr, "PTrie acquire load only:             %.1f ns/op\n", acquire_ns);
    fprintf(stderr, "Speedup: %.0fx\n", guard_ns / acquire_ns);

    // Multi-threaded contention.
    for (int num_threads : {1, 4, 8}) {
        std::atomic<bool> go{false};
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> total_ops{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&]() {
                while (!go.load(std::memory_order_acquire)) {}
                uint64_t ops = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    auto guard = gen_handler.takeGuard();
                    doNotOptimize(static_cast<uint32_t>(guard.getGeneration()));
                    ops++;
                }
                total_ops.fetch_add(ops, std::memory_order_relaxed);
            });
        }

        go.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : threads) t.join();

        double ns_per_op = 500.0 * 1e6 / total_ops.load();
        fprintf(stderr, "GenerationHandler takeGuard (%d threads): %.1f ns/op\n",
                num_threads, ns_per_op);

        go.store(false);
        stop.store(false);
        total_ops.store(0);
    }
}

int main(int argc, char** argv) {
    bool run_all = (argc < 2);
    std::string mode = (argc >= 2) ? argv[1] : "";

    if (run_all || mode == "--batch") {
        benchDictionaryBatchInsertAndFind();
    }
    if (run_all || mode == "--concurrent") {
        benchConcurrentFrozenFind();
    }
    if (run_all || mode == "--rcu") {
        benchRCUGuardOverhead();
    }

    if (!run_all && mode != "--batch" && mode != "--concurrent" && mode != "--rcu") {
        fprintf(stderr, "Usage: %s [--batch|--concurrent|--rcu]\n", argv[0]);
        return 1;
    }
    return 0;
}
