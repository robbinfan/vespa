// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
// Standalone correctness test for NestedLoudsTrie (NLT).

#include <vespa/vespalib/succinct/nested_louds_trie.h>
#include <cstdio>
#include <random>
#include <set>
#include <vector>

using vespalib::succinct::NestedLoudsTrie;

static int test_count = 0;
static int fail_count = 0;

#define EXPECT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); test_count++; \
    if (_a != _b) { fprintf(stderr, "FAIL %s:%d: %s=%lu != %s=%lu\n", __FILE__, __LINE__, #a, (unsigned long)_a, #b, (unsigned long)_b); fail_count++; } \
} while(0)
#define EXPECT_TRUE(x) do { test_count++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fail_count++; } } while(0)
#define EXPECT_FALSE(x) EXPECT_TRUE(!(x))

namespace {

std::unique_ptr<NestedLoudsTrie> build_nlt(const std::vector<std::string> &keys) {
    NestedLoudsTrie::Builder builder;
    for (const auto &k : keys) builder.add(k);
    return builder.finish();
}

void test_empty() {
    auto nlt = build_nlt({});
    EXPECT_TRUE(nlt->empty());
    EXPECT_EQ(0u, nlt->num_keys());
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("anything"));
    fprintf(stderr, "  PASS: test_empty\n");
}

void test_single_key() {
    auto nlt = build_nlt({"hello"});
    EXPECT_EQ(1u, nlt->num_keys());
    EXPECT_EQ(0u, nlt->lookup("hello"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("hell"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("helloo"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("world"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup(""));
    fprintf(stderr, "  PASS: test_single_key\n");
}

void test_multiple_keys() {
    std::vector<std::string> keys = {"apple", "application", "apply", "banana", "band", "bandana"};
    auto nlt = build_nlt(keys);
    EXPECT_EQ(6u, nlt->num_keys());
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(i, nlt->lookup(keys[i]));
    }
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("app"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("ban"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("cherry"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup(""));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("applications"));
    fprintf(stderr, "  PASS: test_multiple_keys\n");
}

void test_common_prefix() {
    std::vector<std::string> keys = {
        "/electronics/phones",
        "/electronics/phones/apple",
        "/electronics/phones/apple/iphone15",
        "/electronics/phones/samsung",
        "/electronics/tablets",
        "/electronics/tablets/ipad"
    };
    auto nlt = build_nlt(keys);
    EXPECT_EQ(6u, nlt->num_keys());
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(i, nlt->lookup(keys[i]));
    }
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("/electronics"));
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("/food"));
    fprintf(stderr, "  PASS: test_common_prefix\n");
}

void test_ordinal_order() {
    std::vector<std::string> keys = {"a", "aa", "aab", "ab", "b", "ba", "bb"};
    auto nlt = build_nlt(keys);
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(i, nlt->lookup(keys[i]));
    }
    fprintf(stderr, "  PASS: test_ordinal_order\n");
}

void test_single_char_keys() {
    std::vector<std::string> keys;
    for (int c = 'a'; c <= 'z'; ++c) keys.push_back(std::string(1, (char)c));
    auto nlt = build_nlt(keys);
    EXPECT_EQ(26u, nlt->num_keys());
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(i, nlt->lookup(keys[i]));
    }
    EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup("A"));
    fprintf(stderr, "  PASS: test_single_char_keys\n");
}

void test_iterator_basic() {
    std::vector<std::string> keys = {"apple", "application", "apply", "banana", "band"};
    auto nlt = build_nlt(keys);
    auto it = nlt->begin();
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_TRUE(it.valid());
        EXPECT_EQ(i, it.ordinal());
        EXPECT_TRUE(it.key() == keys[i]);
        it.next();
    }
    EXPECT_FALSE(it.valid());
    fprintf(stderr, "  PASS: test_iterator_basic\n");
}

void test_random_correctness() {
    // 10K random terms: all lookups correct, negatives return NOT_FOUND
    std::mt19937 rng(42);
    std::set<std::string> unique;
    while (unique.size() < 10000) {
        std::string s;
        uint32_t len = 3 + (rng() % 20);
        for (uint32_t i = 0; i < len; ++i) s += 'a' + (rng() % 26);
        unique.insert(s);
    }
    std::vector<std::string> keys(unique.begin(), unique.end());
    auto nlt = build_nlt(keys);
    EXPECT_EQ(keys.size(), nlt->num_keys());

    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(i, nlt->lookup(keys[i]));
    }
    // Negative lookups
    for (int i = 0; i < 1000; ++i) {
        std::string s;
        uint32_t len = 3 + (rng() % 20);
        for (uint32_t j = 0; j < len; ++j) s += 'a' + (rng() % 26);
        if (unique.find(s) == unique.end()) {
            EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt->lookup(s));
        }
    }
    fprintf(stderr, "  PASS: test_random_correctness (10K terms)\n");
}

void test_serialization() {
    std::vector<std::string> keys = {"apple", "application", "apply", "banana", "band", "bandana"};
    auto nlt = build_nlt(keys);

    size_t ser_size = nlt->serialized_size();
    std::vector<uint8_t> buf(ser_size);
    nlt->serialize(buf.data());

    auto nlt2 = std::make_unique<NestedLoudsTrie>();
    nlt2->load(buf.data(), buf.size());

    EXPECT_EQ(nlt->num_keys(), nlt2->num_keys());
    for (const auto &k : keys) {
        EXPECT_EQ(nlt->lookup(k), nlt2->lookup(k));
    }
    fprintf(stderr, "  PASS: test_serialization\n");
}

void test_mmap_roundtrip() {
    // Build NLT, serialize, then setup_mmap (zero-copy) and verify all lookups
    std::mt19937 rng(99);
    std::set<std::string> unique;
    while (unique.size() < 5000) {
        std::string s;
        uint32_t len = 3 + (rng() % 20);
        for (uint32_t i = 0; i < len; ++i) s += 'a' + (rng() % 26);
        unique.insert(s);
    }
    std::vector<std::string> keys(unique.begin(), unique.end());
    auto nlt = build_nlt(keys);

    // Serialize
    size_t ser_size = nlt->serialized_size();
    std::vector<uint8_t> buf(ser_size);
    nlt->serialize(buf.data());

    // Load via setup_mmap (zero-copy: pointers into buf)
    auto nlt_mmap = std::make_unique<NestedLoudsTrie>();
    size_t consumed = nlt_mmap->setup_mmap(buf.data(), buf.size());
    EXPECT_EQ(ser_size, consumed);
    EXPECT_EQ(keys.size(), nlt_mmap->num_keys());

    // Verify all positive lookups
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(i, nlt_mmap->lookup(keys[i]));
    }

    // Verify negative lookups
    for (int i = 0; i < 500; ++i) {
        std::string s;
        uint32_t len = 3 + (rng() % 20);
        for (uint32_t j = 0; j < len; ++j) s += 'a' + (rng() % 26);
        if (unique.find(s) == unique.end()) {
            EXPECT_EQ(NestedLoudsTrie::NOT_FOUND, nlt_mmap->lookup(s));
        }
    }

    // Verify iterator works on mmap-backed NLT
    auto it = nlt_mmap->begin();
    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_TRUE(it.valid());
        EXPECT_TRUE(it.key() == keys[i]);
        it.next();
    }
    EXPECT_FALSE(it.valid());

    fprintf(stderr, "  PASS: test_mmap_roundtrip (5K terms)\n");
}

} // anonymous namespace

int main() {
    fprintf(stderr, "=== NLT Correctness Tests ===\n");
    test_empty();
    test_single_key();
    test_multiple_keys();
    test_common_prefix();
    test_ordinal_order();
    test_single_char_keys();
    test_iterator_basic();
    test_random_correctness();
    test_serialization();
    test_mmap_roundtrip();

    fprintf(stderr, "\n=== Results: %d tests, %d failures ===\n", test_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
