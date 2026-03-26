// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
//
// Integration test: verifies PatriciaTrie works correctly for the
// Vespa FieldIndex dictionary workflow (batch insert, freeze, frozen lookup,
// ordered iteration, concurrent SWMR).
//
// Simulates the exact usage patterns from:
//   searchlib/memoryindex/field_index.cpp
//   searchlib/memoryindex/ordered_field_index_inserter.cpp
//
// Compile: g++ -O2 -std=c++17 -pthread -I../../src -o ptrie_fieldindex_test ptrie_fieldindex_test.cpp ../../src/vespa/vespalib/ptrie/patricia_trie.cpp

#include <vespa/vespalib/ptrie/patricia_trie.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using vespalib::ptrie::PatriciaTrie;

// ============================================================================
// Simulate FieldIndex components
// ============================================================================

// Simulates WordStore: maps word → ref (uint32_t), and ref → word.
class MockWordStore {
public:
    uint32_t addWord(const std::string& word) {
        auto it = _word_to_ref.find(word);
        if (it != _word_to_ref.end()) return it->second;
        uint32_t ref = static_cast<uint32_t>(_words.size());
        _words.push_back(word);
        _word_to_ref[word] = ref;
        return ref;
    }
    const std::string& getWord(uint32_t ref) const { return _words[ref]; }
    size_t size() const { return _words.size(); }
private:
    std::vector<std::string> _words;
    std::map<std::string, uint32_t> _word_to_ref;
};

// Simulates PostingListStore: maps postingRef → set of docIds.
class MockPostingStore {
public:
    uint32_t create() {
        uint32_t ref = static_cast<uint32_t>(_postings.size());
        _postings.emplace_back();
        return ref;
    }
    void addDoc(uint32_t ref, uint32_t docId) {
        _postings[ref].insert(docId);
    }
    void removeDoc(uint32_t ref, uint32_t docId) {
        _postings[ref].erase(docId);
    }
    const std::set<uint32_t>& getDocs(uint32_t ref) const {
        return _postings[ref];
    }
    size_t size() const { return _postings.size(); }
private:
    std::vector<std::set<uint32_t>> _postings;
};

// PatriciaTrie-based dictionary for FieldIndex.
// Key: word string (stored in trie)
// Value: posting list reference (uint32_t, stored as trie value)
//
// This replaces BTree<WordKey, PostingListPtr, NoAggregated, KeyComp>
// with a direct string-keyed trie. No WordKey indirection needed —
// the trie IS the word→postingRef mapping.
class PTrieDictionary {
public:
    PTrieDictionary() : _num_entries(0) {}

    bool insert(const std::string& word, uint32_t postingRef) {
        bool added = _trie.insert(word, postingRef);
        if (added) ++_num_entries;
        return added;
    }

    void updatePostingRef(const std::string& word, uint32_t postingRef) {
        _trie.insert(word, postingRef);
    }

    bool find(const std::string& word, uint32_t& postingRef) const {
        return _trie.find(word, postingRef);
    }

    void freeze() { _trie.freeze(); }

    bool findFrozen(const std::string& word, uint32_t& postingRef) const {
        auto view = _trie.getFrozenView();
        return view.find(word, postingRef);
    }

    PatriciaTrie::Iterator begin() const { return _trie.begin(); }

    size_t size() const { return _num_entries; }
    void compact() { _trie.compact(); }
    void reserveArena(size_t bytes) { _trie.reserveArena(bytes); }

private:
    PatriciaTrie _trie;
    size_t _num_entries;
};

// ============================================================================
// Test framework
// ============================================================================

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { \
            tests_run++; \
            std::printf("  %-50s ", #name); \
            try { test_##name(); tests_passed++; std::printf("PASS\n"); } \
            catch (const std::exception& e) { tests_failed++; std::printf("FAIL: %s\n", e.what()); } \
            catch (...) { tests_failed++; std::printf("FAIL (unknown exception)\n"); } \
        } \
    } register_##name; \
    static void test_##name()

#define ASSERT_TRUE(x) do { if (!(x)) throw std::runtime_error("ASSERT_TRUE(" #x ") failed at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_FALSE(x) do { if (x) throw std::runtime_error("ASSERT_FALSE(" #x ") failed at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) throw std::runtime_error("ASSERT_EQ failed at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_GT(a, b) do { if (!((a) > (b))) throw std::runtime_error("ASSERT_GT failed at line " + std::to_string(__LINE__)); } while(0)

// ============================================================================
// Tests mirroring field_index_test.cpp patterns
// ============================================================================

TEST(empty_dictionary) {
    PTrieDictionary dict;
    ASSERT_EQ(0u, dict.size());
    uint32_t ref;
    ASSERT_FALSE(dict.find("anything", ref));
    ASSERT_FALSE(dict.findFrozen("anything", ref));
}

TEST(insert_single_word_single_doc) {
    MockPostingStore ps;
    PTrieDictionary dict;

    uint32_t pref = ps.create();
    ps.addDoc(pref, 10);
    dict.insert("hello", pref);
    ASSERT_EQ(1u, dict.size());

    // Before freeze: mutable view sees it, frozen does not.
    uint32_t ref;
    ASSERT_TRUE(dict.find("hello", ref));
    ASSERT_FALSE(dict.findFrozen("hello", ref));

    // After freeze: frozen view sees it.
    dict.freeze();
    ASSERT_TRUE(dict.findFrozen("hello", ref));
    ASSERT_EQ(pref, ref);
    auto docs = ps.getDocs(ref);
    ASSERT_TRUE(docs.count(10) > 0);
}

TEST(insert_multiple_docs_same_word) {
    MockPostingStore ps;
    PTrieDictionary dict;

    uint32_t pref = ps.create();
    ps.addDoc(pref, 10);
    ps.addDoc(pref, 20);
    ps.addDoc(pref, 30);
    dict.insert("hello", pref);
    dict.freeze();

    uint32_t ref;
    ASSERT_TRUE(dict.findFrozen("hello", ref));
    auto docs = ps.getDocs(ref);
    ASSERT_EQ(3u, docs.size());
    ASSERT_EQ(1u, dict.size());
}

TEST(insert_multiple_words) {
    MockPostingStore ps;
    PTrieDictionary dict;

    uint32_t p1 = ps.create(); ps.addDoc(p1, 1); ps.addDoc(p1, 4);
    uint32_t p2 = ps.create(); ps.addDoc(p2, 2);
    uint32_t p3 = ps.create(); ps.addDoc(p3, 3);
    dict.insert("alpha", p1);
    dict.insert("beta", p2);
    dict.insert("gamma", p3);
    dict.freeze();

    ASSERT_EQ(3u, dict.size());
    uint32_t ref;
    ASSERT_TRUE(dict.findFrozen("alpha", ref)); ASSERT_EQ(p1, ref);
    ASSERT_TRUE(dict.findFrozen("beta", ref));  ASSERT_EQ(p2, ref);
    ASSERT_TRUE(dict.findFrozen("gamma", ref)); ASSERT_EQ(p3, ref);
    ASSERT_FALSE(dict.findFrozen("delta", ref));
}

TEST(ordered_iteration_is_lexicographic) {
    PTrieDictionary dict;
    dict.insert("zebra", 1);
    dict.insert("apple", 2);
    dict.insert("mango", 3);
    dict.insert("banana", 4);

    std::vector<std::string> keys;
    auto it = dict.begin();
    while (it.valid()) {
        keys.push_back(std::string(it.getKey().data(), it.getKey().size()));
        ++it;
    }

    ASSERT_EQ(4u, keys.size());
    ASSERT_EQ(std::string("apple"), keys[0]);
    ASSERT_EQ(std::string("banana"), keys[1]);
    ASSERT_EQ(std::string("mango"), keys[2]);
    ASSERT_EQ(std::string("zebra"), keys[3]);
}

TEST(batch_commit_pattern) {
    MockPostingStore ps;
    PTrieDictionary dict;

    // Batch 1
    uint32_t pa = ps.create(); ps.addDoc(pa, 1); dict.insert("a", pa);
    uint32_t pb = ps.create(); ps.addDoc(pb, 2); dict.insert("b", pb);
    uint32_t pc = ps.create(); ps.addDoc(pc, 3); dict.insert("c", pc);
    dict.freeze();

    uint32_t ref;
    ASSERT_TRUE(dict.findFrozen("a", ref)); ASSERT_EQ(pa, ref);
    ASSERT_TRUE(dict.findFrozen("b", ref)); ASSERT_EQ(pb, ref);

    // Batch 2
    uint32_t pd = ps.create(); ps.addDoc(pd, 4); dict.insert("d", pd);
    ASSERT_FALSE(dict.findFrozen("d", ref));  // Not yet committed.
    ASSERT_TRUE(dict.find("d", ref));          // Mutable view sees it.

    dict.freeze();
    ASSERT_TRUE(dict.findFrozen("d", ref)); ASSERT_EQ(pd, ref);
}

TEST(large_batch_with_prefix_sharing) {
    PTrieDictionary dict;
    std::vector<std::string> words;
    for (int i = 0; i < 1000; ++i) {
        words.push_back("word_" + std::to_string(i));
    }
    std::mt19937 rng(42);
    std::shuffle(words.begin(), words.end(), rng);

    for (size_t i = 0; i < words.size(); ++i) {
        dict.insert(words[i], static_cast<uint32_t>(i));
    }
    dict.freeze();

    ASSERT_EQ(1000u, dict.size());

    // Verify all lookups.
    uint32_t ref;
    for (size_t i = 0; i < words.size(); ++i) {
        ASSERT_TRUE(dict.findFrozen(words[i], ref));
    }

    // Verify sorted iteration.
    std::vector<std::string> iterated;
    auto it = dict.begin();
    while (it.valid()) {
        iterated.push_back(std::string(it.getKey().data(), it.getKey().size()));
        ++it;
    }
    ASSERT_EQ(1000u, iterated.size());
    ASSERT_TRUE(std::is_sorted(iterated.begin(), iterated.end()));
}

TEST(concurrent_readers_during_write) {
    PTrieDictionary dict;
    for (int i = 0; i < 100; ++i) {
        dict.insert("word_" + std::to_string(i), static_cast<uint32_t>(i));
    }
    dict.freeze();
    dict.reserveArena(4 * 1024 * 1024);

    std::atomic<bool> done(false);
    std::atomic<size_t> reader_lookups(0);
    std::atomic<size_t> reader_hits(0);

    std::thread reader([&]() {
        uint32_t ref;
        while (!done.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 100; ++i) {
                if (dict.findFrozen("word_" + std::to_string(i), ref)) {
                    reader_hits.fetch_add(1, std::memory_order_relaxed);
                }
                reader_lookups.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Writer: insert more words + periodic freeze.
    for (int batch = 0; batch < 10; ++batch) {
        for (int i = 0; i < 100; ++i) {
            dict.insert("new_" + std::to_string(batch * 100 + i),
                        static_cast<uint32_t>(1000 + batch * 100 + i));
        }
        dict.freeze();
    }

    done.store(true, std::memory_order_relaxed);
    reader.join();

    ASSERT_GT(reader_lookups.load(), 0u);
    double hit_rate = static_cast<double>(reader_hits.load()) / reader_lookups.load();
    ASSERT_GT(hit_rate, 0.9);

    ASSERT_EQ(1100u, dict.size());

    // All words findable.
    uint32_t ref;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(dict.findFrozen("word_" + std::to_string(i), ref));
    }
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(dict.findFrozen("new_" + std::to_string(i), ref));
    }
}

TEST(update_posting_ref) {
    MockPostingStore ps;
    PTrieDictionary dict;

    uint32_t p1 = ps.create(); ps.addDoc(p1, 10);
    dict.insert("hello", p1);
    dict.freeze();

    // Update: new posting list.
    uint32_t p2 = ps.create(); ps.addDoc(p2, 20); ps.addDoc(p2, 30);
    dict.updatePostingRef("hello", p2);
    dict.freeze();

    uint32_t ref;
    ASSERT_TRUE(dict.findFrozen("hello", ref));
    ASSERT_EQ(p2, ref);
    ASSERT_EQ(2u, ps.getDocs(ref).size());
}

TEST(compact_preserves_correctness) {
    PTrieDictionary dict;
    for (int i = 0; i < 500; ++i) {
        dict.insert("term_" + std::to_string(i), static_cast<uint32_t>(i));
    }
    dict.freeze();
    dict.compact();
    dict.freeze();

    ASSERT_EQ(500u, dict.size());

    uint32_t ref;
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(dict.findFrozen("term_" + std::to_string(i), ref));
    }

    // Iteration still sorted.
    std::vector<std::string> keys;
    auto it = dict.begin();
    while (it.valid()) {
        keys.push_back(std::string(it.getKey().data(), it.getKey().size()));
        ++it;
    }
    ASSERT_TRUE(std::is_sorted(keys.begin(), keys.end()));
}

TEST(deferred_freeze_pattern) {
    PTrieDictionary dict;
    const int BATCHES = 200;
    const int WPB = 5;
    const int FREEZE_IV = 50;

    for (int b = 0; b < BATCHES; ++b) {
        for (int w = 0; w < WPB; ++w) {
            int idx = b * WPB + w;
            dict.insert("w_" + std::to_string(idx), static_cast<uint32_t>(idx));
        }
        if ((b + 1) % FREEZE_IV == 0 || b == BATCHES - 1) {
            dict.freeze();
        }
    }

    ASSERT_EQ(static_cast<size_t>(BATCHES * WPB), dict.size());
    uint32_t ref;
    for (int i = 0; i < BATCHES * WPB; ++i) {
        ASSERT_TRUE(dict.findFrozen("w_" + std::to_string(i), ref));
    }
}

TEST(realistic_term_vocabulary) {
    PTrieDictionary dict;
    std::vector<std::string> terms = {
        "the", "there", "therefore", "thermal", "thermometer",
        "search", "searching", "searched", "searcher",
        "index", "indexed", "indexing", "indexes",
        "memory", "memorize", "memorial",
        "vespa", "vessel", "vest", "veteran", "veto",
        "patricia", "patriot", "patrol", "pattern",
        "trie", "tried", "trial", "triangle", "tribute",
        "concurrent", "concurrency", "conclude", "concrete",
        "optimize", "optimized", "optimizer", "optimal", "option"
    };

    for (size_t i = 0; i < terms.size(); ++i) {
        dict.insert(terms[i], static_cast<uint32_t>(i));
    }
    dict.freeze();

    ASSERT_EQ(terms.size(), dict.size());

    uint32_t ref;
    for (const auto& t : terms) {
        ASSERT_TRUE(dict.findFrozen(t, ref));
    }

    // Verify iteration matches sorted terms.
    std::vector<std::string> iterated;
    auto it = dict.begin();
    while (it.valid()) {
        iterated.push_back(std::string(it.getKey().data(), it.getKey().size()));
        ++it;
    }
    auto sorted = terms;
    std::sort(sorted.begin(), sorted.end());
    ASSERT_EQ(sorted, iterated);
}

TEST(multi_field_simulation) {
    PTrieDictionary dict_title, dict_body;
    MockPostingStore ps_title, ps_body;

    auto insert = [](PTrieDictionary& d, MockPostingStore& ps,
                     const std::string& word, uint32_t docId) {
        uint32_t ref;
        if (!d.find(word, ref)) {
            ref = ps.create();
            d.insert(word, ref);
        }
        ps.addDoc(ref, docId);
    };

    // Doc 1
    insert(dict_title, ps_title, "vespa", 1);
    insert(dict_title, ps_title, "search", 1);
    insert(dict_body, ps_body, "vespa", 1);
    insert(dict_body, ps_body, "engine", 1);

    // Doc 2
    insert(dict_title, ps_title, "memory", 2);
    insert(dict_body, ps_body, "memory", 2);
    insert(dict_body, ps_body, "btree", 2);

    dict_title.freeze();
    dict_body.freeze();

    uint32_t ref;
    ASSERT_TRUE(dict_title.findFrozen("vespa", ref));
    ASSERT_EQ(1u, ps_title.getDocs(ref).size());
    ASSERT_TRUE(dict_body.findFrozen("engine", ref));
    ASSERT_EQ(1u, ps_body.getDocs(ref).size());
    ASSERT_FALSE(dict_title.findFrozen("engine", ref));
    ASSERT_TRUE(dict_body.findFrozen("btree", ref));
    ASSERT_EQ(1u, ps_body.getDocs(ref).size());
}

// ============================================================================
// Main: run all tests
// ============================================================================

int main() {
    std::printf("\nPatriciaTrie FieldIndex Integration Tests\n");
    std::printf("==========================================\n\n");

    // Tests are auto-registered via static initialization.
    // Force registration by referencing all register objects.
    (void)register_empty_dictionary;
    (void)register_insert_single_word_single_doc;
    (void)register_insert_multiple_docs_same_word;
    (void)register_insert_multiple_words;
    (void)register_ordered_iteration_is_lexicographic;
    (void)register_batch_commit_pattern;
    (void)register_large_batch_with_prefix_sharing;
    (void)register_concurrent_readers_during_write;
    (void)register_update_posting_ref;
    (void)register_compact_preserves_correctness;
    (void)register_deferred_freeze_pattern;
    (void)register_realistic_term_vocabulary;
    (void)register_multi_field_simulation;

    std::printf("\n==========================================\n");
    std::printf("Results: %d/%d passed, %d failed\n",
                tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
