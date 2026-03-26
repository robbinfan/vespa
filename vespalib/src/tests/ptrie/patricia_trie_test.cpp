// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/vespalib/ptrie/patricia_trie.h>
#include <vespa/vespalib/gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using vespalib::ptrie::PatriciaTrie;
using vespalib::ptrie::ShardedPatriciaTrie;
using vespalib::stringref;

class PatriciaTrieTest : public ::testing::Test {
protected:
    PatriciaTrie trie;
};

// ============================================================================
// Basic insert and find
// ============================================================================

TEST_F(PatriciaTrieTest, empty_trie_has_zero_size)
{
    EXPECT_EQ(0u, trie.size());
    uint32_t val;
    EXPECT_FALSE(trie.find("anything", val));
}

TEST_F(PatriciaTrieTest, insert_single_key)
{
    EXPECT_TRUE(trie.insert("hello", 42));
    EXPECT_EQ(1u, trie.size());

    uint32_t val = 0;
    EXPECT_TRUE(trie.find("hello", val));
    EXPECT_EQ(42u, val);
}

TEST_F(PatriciaTrieTest, insert_duplicate_returns_false_and_updates_value)
{
    EXPECT_TRUE(trie.insert("hello", 42));
    EXPECT_FALSE(trie.insert("hello", 99));
    EXPECT_EQ(1u, trie.size());

    uint32_t val = 0;
    EXPECT_TRUE(trie.find("hello", val));
    EXPECT_EQ(99u, val);
}

TEST_F(PatriciaTrieTest, find_missing_key_returns_false)
{
    trie.insert("hello", 1);
    uint32_t val;
    EXPECT_FALSE(trie.find("world", val));
    EXPECT_FALSE(trie.find("hell", val));
    EXPECT_FALSE(trie.find("helloo", val));
    EXPECT_FALSE(trie.find("h", val));
    EXPECT_FALSE(trie.find("", val));
}

// ============================================================================
// Path compression and splitting
// ============================================================================

TEST_F(PatriciaTrieTest, insert_keys_with_common_prefix)
{
    trie.insert("abc", 1);
    trie.insert("abd", 2);
    trie.insert("xyz", 3);

    uint32_t val;
    EXPECT_TRUE(trie.find("abc", val)); EXPECT_EQ(1u, val);
    EXPECT_TRUE(trie.find("abd", val)); EXPECT_EQ(2u, val);
    EXPECT_TRUE(trie.find("xyz", val)); EXPECT_EQ(3u, val);
    EXPECT_EQ(3u, trie.size());
}

TEST_F(PatriciaTrieTest, insert_prefix_of_existing_key)
{
    trie.insert("abcdef", 1);
    trie.insert("abc", 2);

    uint32_t val;
    EXPECT_TRUE(trie.find("abcdef", val)); EXPECT_EQ(1u, val);
    EXPECT_TRUE(trie.find("abc", val));    EXPECT_EQ(2u, val);
    EXPECT_FALSE(trie.find("ab", val));
    EXPECT_EQ(2u, trie.size());
}

TEST_F(PatriciaTrieTest, insert_key_that_extends_existing)
{
    trie.insert("abc", 1);
    trie.insert("abcdef", 2);

    uint32_t val;
    EXPECT_TRUE(trie.find("abc", val));    EXPECT_EQ(1u, val);
    EXPECT_TRUE(trie.find("abcdef", val)); EXPECT_EQ(2u, val);
    EXPECT_EQ(2u, trie.size());
}

TEST_F(PatriciaTrieTest, empty_key)
{
    trie.insert("", 100);
    trie.insert("a", 200);

    uint32_t val;
    EXPECT_TRUE(trie.find("", val));  EXPECT_EQ(100u, val);
    EXPECT_TRUE(trie.find("a", val)); EXPECT_EQ(200u, val);
    EXPECT_EQ(2u, trie.size());
}

TEST_F(PatriciaTrieTest, many_branches_at_same_node)
{
    for (int c = 0; c < 26; ++c) {
        std::string key = "prefix_";
        key += static_cast<char>('a' + c);
        trie.insert(key, static_cast<uint32_t>(c));
    }
    EXPECT_EQ(26u, trie.size());

    for (int c = 0; c < 26; ++c) {
        uint32_t val;
        std::string key = "prefix_";
        key += static_cast<char>('a' + c);
        EXPECT_TRUE(trie.find(key, val));
        EXPECT_EQ(static_cast<uint32_t>(c), val);
    }
}

// ============================================================================
// Remove
// ============================================================================

TEST_F(PatriciaTrieTest, remove_single_key)
{
    trie.insert("hello", 42);
    EXPECT_TRUE(trie.remove("hello"));
    EXPECT_EQ(0u, trie.size());

    uint32_t val;
    EXPECT_FALSE(trie.find("hello", val));
}

TEST_F(PatriciaTrieTest, remove_nonexistent_returns_false)
{
    trie.insert("hello", 42);
    EXPECT_FALSE(trie.remove("world"));
    EXPECT_EQ(1u, trie.size());
}

TEST_F(PatriciaTrieTest, remove_triggers_path_compression)
{
    trie.insert("abc", 1);
    trie.insert("abd", 2);
    trie.insert("abe", 3);

    EXPECT_TRUE(trie.remove("abd"));
    EXPECT_EQ(2u, trie.size());

    uint32_t val;
    EXPECT_TRUE(trie.find("abc", val)); EXPECT_EQ(1u, val);
    EXPECT_FALSE(trie.find("abd", val));
    EXPECT_TRUE(trie.find("abe", val)); EXPECT_EQ(3u, val);

    EXPECT_TRUE(trie.remove("abc"));
    EXPECT_EQ(1u, trie.size());
    EXPECT_TRUE(trie.find("abe", val)); EXPECT_EQ(3u, val);
}

TEST_F(PatriciaTrieTest, remove_branch_value_keeps_children)
{
    trie.insert("ab", 1);
    trie.insert("abc", 2);
    trie.insert("abd", 3);

    EXPECT_TRUE(trie.remove("ab"));
    EXPECT_EQ(2u, trie.size());

    uint32_t val;
    EXPECT_FALSE(trie.find("ab", val));
    EXPECT_TRUE(trie.find("abc", val)); EXPECT_EQ(2u, val);
    EXPECT_TRUE(trie.find("abd", val)); EXPECT_EQ(3u, val);
}

TEST_F(PatriciaTrieTest, remove_all_then_reinsert)
{
    trie.insert("a", 1);
    trie.insert("b", 2);
    trie.insert("c", 3);

    EXPECT_TRUE(trie.remove("a"));
    EXPECT_TRUE(trie.remove("b"));
    EXPECT_TRUE(trie.remove("c"));
    EXPECT_EQ(0u, trie.size());

    EXPECT_TRUE(trie.insert("x", 10));
    EXPECT_EQ(1u, trie.size());
    uint32_t val;
    EXPECT_TRUE(trie.find("x", val));
    EXPECT_EQ(10u, val);
}

// ============================================================================
// Iterator (lexicographic order)
// ============================================================================

TEST_F(PatriciaTrieTest, iterator_empty_trie)
{
    auto it = trie.begin();
    EXPECT_FALSE(it.valid());
}

TEST_F(PatriciaTrieTest, iterator_single_entry)
{
    trie.insert("hello", 42);
    auto it = trie.begin();
    EXPECT_TRUE(it.valid());
    EXPECT_EQ("hello", it.getKey());
    EXPECT_EQ(42u, it.getData());
    ++it;
    EXPECT_FALSE(it.valid());
}

TEST_F(PatriciaTrieTest, iterator_yields_lexicographic_order)
{
    std::vector<std::string> keys = {"banana", "apple", "cherry", "avocado", "blueberry"};
    for (size_t i = 0; i < keys.size(); ++i) {
        trie.insert(keys[i], static_cast<uint32_t>(i + 1));
    }

    std::vector<std::string> sorted_keys = keys;
    std::sort(sorted_keys.begin(), sorted_keys.end());

    std::vector<std::string> iterated_keys;
    for (auto it = trie.begin(); it.valid(); ++it) {
        iterated_keys.emplace_back(it.getKey().data(), it.getKey().size());
    }

    EXPECT_EQ(sorted_keys, iterated_keys);
}

TEST_F(PatriciaTrieTest, iterator_with_prefix_keys)
{
    trie.insert("a", 1);
    trie.insert("ab", 2);
    trie.insert("abc", 3);
    trie.insert("b", 4);

    std::vector<std::string> expected = {"a", "ab", "abc", "b"};
    std::vector<std::string> actual;
    for (auto it = trie.begin(); it.valid(); ++it) {
        actual.emplace_back(it.getKey().data(), it.getKey().size());
    }
    EXPECT_EQ(expected, actual);
}

TEST_F(PatriciaTrieTest, iterator_with_values_check)
{
    trie.insert("a", 10);
    trie.insert("ab", 20);
    trie.insert("abc", 30);

    auto it = trie.begin();
    ASSERT_TRUE(it.valid()); EXPECT_EQ(10u, it.getData()); ++it;
    ASSERT_TRUE(it.valid()); EXPECT_EQ(20u, it.getData()); ++it;
    ASSERT_TRUE(it.valid()); EXPECT_EQ(30u, it.getData()); ++it;
    EXPECT_FALSE(it.valid());
}

// ============================================================================
// FrozenView
// ============================================================================

TEST_F(PatriciaTrieTest, frozen_view_sees_state_at_freeze_time)
{
    trie.insert("hello", 1);
    trie.insert("world", 2);
    trie.freeze();

    auto view = trie.getFrozenView();
    EXPECT_EQ(2u, view.size());

    uint32_t val;
    EXPECT_TRUE(view.find("hello", val)); EXPECT_EQ(1u, val);
    EXPECT_TRUE(view.find("world", val)); EXPECT_EQ(2u, val);
    EXPECT_FALSE(view.find("missing", val));
}

TEST_F(PatriciaTrieTest, frozen_view_does_not_see_later_inserts)
{
    trie.insert("hello", 1);
    trie.freeze();

    auto view = trie.getFrozenView();

    trie.insert("world", 2);

    uint32_t val;
    EXPECT_TRUE(view.find("hello", val));
    EXPECT_FALSE(view.find("world", val));
}

TEST_F(PatriciaTrieTest, frozen_view_iterator)
{
    trie.insert("cherry", 3);
    trie.insert("apple", 1);
    trie.insert("banana", 2);
    trie.freeze();

    auto view = trie.getFrozenView();
    std::vector<std::string> keys;
    for (auto it = view.begin(); it.valid(); ++it) {
        keys.emplace_back(it.getKey().data(), it.getKey().size());
    }
    std::vector<std::string> expected = {"apple", "banana", "cherry"};
    EXPECT_EQ(expected, keys);
}

TEST_F(PatriciaTrieTest, multiple_freeze_cycles)
{
    trie.insert("a", 1);
    trie.freeze();
    auto v1 = trie.getFrozenView();

    trie.insert("b", 2);
    trie.freeze();
    auto v2 = trie.getFrozenView();

    trie.insert("c", 3);

    EXPECT_EQ(1u, v1.size());
    EXPECT_EQ(2u, v2.size());

    uint32_t val;
    EXPECT_FALSE(v1.find("b", val));
    EXPECT_TRUE(v2.find("b", val));
    EXPECT_FALSE(v2.find("c", val));
}

// ============================================================================
// Many keys (stress basic correctness)
// ============================================================================

TEST_F(PatriciaTrieTest, insert_and_find_many_keys)
{
    const int N = 10000;
    std::vector<std::string> keys;
    keys.reserve(N);
    for (int i = 0; i < N; ++i) {
        keys.push_back("key_" + std::to_string(i));
    }

    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(trie.insert(keys[i], static_cast<uint32_t>(i)));
    }
    EXPECT_EQ(static_cast<size_t>(N), trie.size());

    for (int i = 0; i < N; ++i) {
        uint32_t val;
        ASSERT_TRUE(trie.find(keys[i], val)) << "Key not found: " << keys[i];
        EXPECT_EQ(static_cast<uint32_t>(i), val);
    }

    std::vector<std::string> sorted_keys = keys;
    std::sort(sorted_keys.begin(), sorted_keys.end());

    std::vector<std::string> iterated;
    for (auto it = trie.begin(); it.valid(); ++it) {
        iterated.emplace_back(it.getKey().data(), it.getKey().size());
    }
    EXPECT_EQ(sorted_keys, iterated);
}

TEST_F(PatriciaTrieTest, insert_and_remove_many_keys)
{
    const int N = 1000;
    std::vector<std::string> keys;
    for (int i = 0; i < N; ++i) {
        keys.push_back("term_" + std::to_string(i));
    }
    for (int i = 0; i < N; ++i) {
        trie.insert(keys[i], static_cast<uint32_t>(i));
    }

    for (int i = 0; i < N; i += 2) {
        EXPECT_TRUE(trie.remove(keys[i]));
    }
    EXPECT_EQ(static_cast<size_t>(N / 2), trie.size());

    for (int i = 0; i < N; ++i) {
        uint32_t val;
        if (i % 2 == 0) {
            EXPECT_FALSE(trie.find(keys[i], val)) << "Should be removed: " << keys[i];
        } else {
            EXPECT_TRUE(trie.find(keys[i], val)) << "Should exist: " << keys[i];
        }
    }
}

// ============================================================================
// Memory usage and generation management
// ============================================================================

TEST_F(PatriciaTrieTest, memory_usage_reports_nonzero_after_inserts)
{
    trie.insert("hello", 1);
    trie.insert("world", 2);
    auto usage = trie.getMemoryUsage();
    EXPECT_GT(usage.usedBytes(), 0u);
    EXPECT_GE(usage.allocatedBytes(), usage.usedBytes());
}

TEST_F(PatriciaTrieTest, dead_bytes_increase_after_updates)
{
    trie.insert("hello", 1);
    auto usage1 = trie.getMemoryUsage();

    trie.insert("hello", 2);  // COW → old node dead
    auto usage2 = trie.getMemoryUsage();

    EXPECT_GT(usage2.deadBytes(), usage1.deadBytes());
}

TEST_F(PatriciaTrieTest, transfer_and_trim_hold_lists)
{
    trie.insert("a", 1);
    trie.insert("a", 2);  // COW → dead bytes

    auto usage1 = trie.getMemoryUsage();
    EXPECT_GT(usage1.deadBytes(), 0u);

    trie.transferHoldLists(1);

    auto usage2 = trie.getMemoryUsage();
    EXPECT_GT(usage2.deadBytes(), 0u);

    trie.trimHoldLists(2);

    auto usage3 = trie.getMemoryUsage();
    EXPECT_EQ(0u, usage3.deadBytes());
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_F(PatriciaTrieTest, single_char_keys)
{
    for (int c = 0; c < 256; ++c) {
        std::string key(1, static_cast<char>(c));
        trie.insert(key, static_cast<uint32_t>(c));
    }
    EXPECT_EQ(256u, trie.size());

    for (int c = 0; c < 256; ++c) {
        uint32_t val;
        std::string key(1, static_cast<char>(c));
        EXPECT_TRUE(trie.find(key, val));
        EXPECT_EQ(static_cast<uint32_t>(c), val);
    }

    auto it = trie.begin();
    int expected = 0;
    while (it.valid()) {
        EXPECT_EQ(1u, it.getKey().size());
        EXPECT_EQ(static_cast<uint8_t>(expected), static_cast<uint8_t>(it.getKey()[0]));
        ++it;
        ++expected;
    }
    EXPECT_EQ(256, expected);
}

TEST_F(PatriciaTrieTest, long_common_prefix)
{
    std::string prefix(1000, 'a');
    trie.insert(prefix + "x", 1);
    trie.insert(prefix + "y", 2);
    trie.insert(prefix + "z", 3);

    uint32_t val;
    EXPECT_TRUE(trie.find(prefix + "x", val)); EXPECT_EQ(1u, val);
    EXPECT_TRUE(trie.find(prefix + "y", val)); EXPECT_EQ(2u, val);
    EXPECT_TRUE(trie.find(prefix + "z", val)); EXPECT_EQ(3u, val);
    EXPECT_FALSE(trie.find(prefix, val));
}

TEST_F(PatriciaTrieTest, foreach_key_visits_all_in_order)
{
    trie.insert("cherry", 3);
    trie.insert("apple", 1);
    trie.insert("banana", 2);

    std::vector<std::string> visited;
    trie.foreach_key([&](stringref key) {
        visited.emplace_back(key.data(), key.size());
    });

    std::vector<std::string> expected = {"apple", "banana", "cherry"};
    EXPECT_EQ(expected, visited);
}

TEST_F(PatriciaTrieTest, binary_data_keys)
{
    std::string k1(4, '\0');
    k1[2] = 'a';
    std::string k2(4, '\0');
    k2[2] = 'b';

    trie.insert(k1, 1);
    trie.insert(k2, 2);

    uint32_t val;
    EXPECT_TRUE(trie.find(k1, val)); EXPECT_EQ(1u, val);
    EXPECT_TRUE(trie.find(k2, val)); EXPECT_EQ(2u, val);
}

// ============================================================================
// CAS-based concurrent insert tests
// ============================================================================

TEST_F(PatriciaTrieTest, cas_insert_basic)
{
    // casInsert should behave identically to insert for single-thread use.
    EXPECT_TRUE(trie.casInsert("hello", 1));
    EXPECT_TRUE(trie.casInsert("world", 2));
    EXPECT_FALSE(trie.casInsert("hello", 3));  // duplicate

    uint32_t val;
    EXPECT_TRUE(trie.find("hello", val)); EXPECT_EQ(1u, val);
    EXPECT_TRUE(trie.find("world", val)); EXPECT_EQ(2u, val);
    EXPECT_EQ(2u, trie.size());
}

TEST_F(PatriciaTrieTest, cas_insert_two_threads_disjoint_keys)
{
    // Two threads insert disjoint key sets concurrently.
    const int N = 5000;

    // Pre-size arena: ~600 bytes per entry, 2 threads, with headroom.
    trie.reserveArena(N * 2 * 800);

    std::thread t1([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "thread1_key_" + std::to_string(i);
            trie.casInsert(key, static_cast<uint32_t>(i));
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "thread2_key_" + std::to_string(i);
            trie.casInsert(key, static_cast<uint32_t>(10000 + i));
        }
    });

    t1.join();
    t2.join();

    EXPECT_EQ(static_cast<size_t>(2 * N), trie.size());

    // Verify all keys are findable.
    uint32_t val;
    for (int i = 0; i < N; ++i) {
        std::string k1 = "thread1_key_" + std::to_string(i);
        std::string k2 = "thread2_key_" + std::to_string(i);
        ASSERT_TRUE(trie.find(k1, val)) << "Missing: " << k1;
        EXPECT_EQ(static_cast<uint32_t>(i), val);
        ASSERT_TRUE(trie.find(k2, val)) << "Missing: " << k2;
        EXPECT_EQ(static_cast<uint32_t>(10000 + i), val);
    }
}

TEST_F(PatriciaTrieTest, cas_insert_four_threads_disjoint_keys)
{
    const int N = 2000;
    const int NUM_THREADS = 4;

    trie.reserveArena(N * NUM_THREADS * 800);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < N; ++i) {
                std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
                trie.casInsert(key, static_cast<uint32_t>(t * 10000 + i));
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(static_cast<size_t>(N * NUM_THREADS), trie.size());

    uint32_t val;
    for (int t = 0; t < NUM_THREADS; ++t) {
        for (int i = 0; i < N; ++i) {
            std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
            ASSERT_TRUE(trie.find(key, val)) << "Missing: " << key;
            EXPECT_EQ(static_cast<uint32_t>(t * 10000 + i), val);
        }
    }
}

TEST_F(PatriciaTrieTest, cas_insert_overlapping_keys)
{
    // Two threads try to insert the same keys. Each key should be inserted exactly once.
    const int N = 3000;
    trie.reserveArena(N * 800);

    std::atomic<int> inserted_by_t1(0);
    std::atomic<int> inserted_by_t2(0);

    std::thread t1([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "shared_key_" + std::to_string(i);
            if (trie.casInsert(key, static_cast<uint32_t>(i))) {
                inserted_by_t1.fetch_add(1);
            }
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "shared_key_" + std::to_string(i);
            if (trie.casInsert(key, static_cast<uint32_t>(i + 10000))) {
                inserted_by_t2.fetch_add(1);
            }
        }
    });

    t1.join();
    t2.join();

    // Each key inserted exactly once, total must be N.
    EXPECT_EQ(static_cast<size_t>(N), trie.size());
    EXPECT_EQ(N, inserted_by_t1.load() + inserted_by_t2.load());
}

TEST_F(PatriciaTrieTest, cas_insert_concurrent_with_find)
{
    // Writer thread inserts while reader thread does lookups.
    const int N = 5000;
    trie.reserveArena(N * 800);

    std::atomic<bool> done(false);
    std::atomic<int> found_count(0);

    std::thread writer([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "key_" + std::to_string(i);
            trie.casInsert(key, static_cast<uint32_t>(i));
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&]() {
        uint32_t val;
        while (!done.load(std::memory_order_acquire)) {
            // Look up random keys; some may not be inserted yet.
            for (int i = 0; i < 100; ++i) {
                std::string key = "key_" + std::to_string(i % N);
                if (trie.find(key, val)) {
                    found_count.fetch_add(1);
                }
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(static_cast<size_t>(N), trie.size());
    // Reader should have found at least some keys.
    EXPECT_GT(found_count.load(), 0);
}

TEST_F(PatriciaTrieTest, cas_insert_frozen_view_isolation)
{
    // Freeze, then concurrent inserts should not be visible in frozen view.
    trie.casInsert("existing", 42);
    trie.freeze();

    const int N = 1000;
    trie.reserveArena(N * 800);

    std::thread writer([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "new_" + std::to_string(i);
            trie.casInsert(key, static_cast<uint32_t>(i));
        }
    });

    auto frozen = trie.getFrozenView();
    writer.join();

    // Frozen view should only see "existing".
    uint32_t val;
    EXPECT_TRUE(frozen.find("existing", val));
    EXPECT_EQ(42u, val);
    // New keys should NOT be in frozen view (they were added after freeze).
    EXPECT_FALSE(frozen.find("new_0", val));

    // But mutable trie should see them.
    EXPECT_TRUE(trie.find("new_0", val));
    EXPECT_EQ(static_cast<size_t>(N + 1), trie.size());
}

// ============================================================================
// ShardedPatriciaTrie tests
// ============================================================================

class ShardedPatriciaTrieTest : public ::testing::Test {
protected:
    ShardedPatriciaTrie trie;
};

TEST_F(ShardedPatriciaTrieTest, basic_insert_find)
{
    EXPECT_TRUE(trie.insert("hello", 1));
    EXPECT_TRUE(trie.insert("world", 2));
    EXPECT_FALSE(trie.insert("hello", 3));  // duplicate

    uint32_t val;
    EXPECT_TRUE(trie.find("hello", val)); EXPECT_EQ(1u, val);
    EXPECT_TRUE(trie.find("world", val)); EXPECT_EQ(2u, val);
    EXPECT_FALSE(trie.find("missing", val));
    EXPECT_EQ(2u, trie.size());
}

TEST_F(ShardedPatriciaTrieTest, remove)
{
    trie.insert("alpha", 1);
    trie.insert("beta", 2);
    EXPECT_TRUE(trie.remove("alpha"));
    EXPECT_FALSE(trie.remove("alpha"));  // already removed
    EXPECT_EQ(1u, trie.size());

    uint32_t val;
    EXPECT_FALSE(trie.find("alpha", val));
    EXPECT_TRUE(trie.find("beta", val)); EXPECT_EQ(2u, val);
}

TEST_F(ShardedPatriciaTrieTest, cas_insert_four_threads)
{
    const int N = 2000;
    const int NUM_THREADS = 4;

    trie.reserveArena(N * NUM_THREADS * 800);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < N; ++i) {
                // Keys with diverse first bytes for shard distribution.
                char prefix = 'A' + static_cast<char>(t);
                std::string key = std::string(1, prefix) + "_key_" + std::to_string(i);
                trie.casInsert(key, static_cast<uint32_t>(t * 10000 + i));
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(static_cast<size_t>(N * NUM_THREADS), trie.size());

    uint32_t val;
    for (int t = 0; t < NUM_THREADS; ++t) {
        for (int i = 0; i < N; ++i) {
            char prefix = 'A' + static_cast<char>(t);
            std::string key = std::string(1, prefix) + "_key_" + std::to_string(i);
            ASSERT_TRUE(trie.find(key, val)) << "Missing: " << key;
            EXPECT_EQ(static_cast<uint32_t>(t * 10000 + i), val);
        }
    }
}

TEST_F(ShardedPatriciaTrieTest, cas_insert_same_shard_contention)
{
    // All keys start with 'X' → same shard. Tests contention handling.
    const int N = 3000;
    trie.reserveArena(N * 2000);

    std::atomic<int> total_inserted(0);

    std::thread t1([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "X_t1_" + std::to_string(i);
            if (trie.casInsert(key, static_cast<uint32_t>(i)))
                total_inserted.fetch_add(1);
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < N; ++i) {
            std::string key = "X_t2_" + std::to_string(i);
            if (trie.casInsert(key, static_cast<uint32_t>(10000 + i)))
                total_inserted.fetch_add(1);
        }
    });

    t1.join();
    t2.join();

    // All keys are unique, so all should be inserted.
    EXPECT_EQ(2 * N, total_inserted.load());
    EXPECT_EQ(static_cast<size_t>(2 * N), trie.size());
}

// ============================================================================
// Compaction tests
// ============================================================================

TEST_F(PatriciaTrieTest, compact_empty_trie_returns_zero)
{
    EXPECT_EQ(0u, trie.compact());
}

TEST_F(PatriciaTrieTest, compact_low_dead_ratio_skips)
{
    // Insert a few keys — dead ratio will be low.
    trie.insert("a", 1);
    trie.insert("b", 2);
    trie.insert("c", 3);
    EXPECT_EQ(0u, trie.compact());  // <25% dead, should skip
}

TEST_F(PatriciaTrieTest, compact_reclaims_dead_space)
{
    // Build a trie with high dead ratio by inserting many keys.
    // COW creates dead space from old path nodes.
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        std::string key = "key_" + std::to_string(i);
        trie.insert(key, static_cast<uint32_t>(i));
    }

    auto mu_before = trie.getMemoryUsage();
    size_t reclaimed = trie.compact();

    // Should reclaim significant space.
    EXPECT_GT(reclaimed, 0u);

    auto mu_after = trie.getMemoryUsage();
    EXPECT_LT(mu_after.usedBytes(), mu_before.usedBytes());

    // All keys should still be findable after compaction.
    for (int i = 0; i < N; ++i) {
        std::string key = "key_" + std::to_string(i);
        uint32_t val = 0;
        EXPECT_TRUE(trie.find(key, val)) << "key=" << key;
        EXPECT_EQ(static_cast<uint32_t>(i), val) << "key=" << key;
    }
    EXPECT_EQ(static_cast<size_t>(N), trie.size());
}

TEST_F(PatriciaTrieTest, compact_preserves_iteration_order)
{
    std::vector<std::string> keys = {"alpha", "beta", "gamma", "delta", "epsilon"};
    std::sort(keys.begin(), keys.end());
    for (size_t i = 0; i < keys.size(); ++i) {
        trie.insert(keys[i], static_cast<uint32_t>(i));
    }

    // Force enough inserts to create dead space for compaction.
    for (int i = 0; i < 500; ++i) {
        trie.insert("padding_" + std::to_string(i), 1000 + i);
    }

    trie.compact();

    // Original keys should still be there.
    for (size_t i = 0; i < keys.size(); ++i) {
        uint32_t val = 0;
        EXPECT_TRUE(trie.find(keys[i], val));
        EXPECT_EQ(static_cast<uint32_t>(i), val);
    }

    // Iteration should be in lexicographic order.
    auto it = trie.begin();
    std::string prev;
    size_t count = 0;
    while (it.valid()) {
        std::string k(it.getKey().data(), it.getKey().size());
        if (!prev.empty()) {
            EXPECT_LT(prev, k) << "Iteration order broken: " << prev << " >= " << k;
        }
        prev = k;
        ++it;
        ++count;
    }
    EXPECT_EQ(trie.size(), count);
}

TEST_F(PatriciaTrieTest, compact_then_insert_works)
{
    for (int i = 0; i < 1000; ++i) {
        trie.insert("pre_" + std::to_string(i), i);
    }

    trie.compact();

    // Insert more after compaction.
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(trie.insert("post_" + std::to_string(i), 5000 + i));
    }

    EXPECT_EQ(1100u, trie.size());

    // Verify both old and new keys.
    uint32_t val;
    EXPECT_TRUE(trie.find("pre_0", val));
    EXPECT_EQ(0u, val);
    EXPECT_TRUE(trie.find("post_0", val));
    EXPECT_EQ(5000u, val);
}

TEST_F(PatriciaTrieTest, compact_frozen_view_invalidated)
{
    for (int i = 0; i < 1000; ++i) {
        trie.insert("k_" + std::to_string(i), i);
    }
    trie.freeze();

    trie.compact();
    // After compaction, frozen root is reset to INVALID.
    // Need to re-freeze to get a valid frozen view.
    trie.freeze();

    auto fv = trie.getFrozenView();
    EXPECT_EQ(1000u, fv.size());

    uint32_t val;
    EXPECT_TRUE(fv.find("k_0", val));
    EXPECT_EQ(0u, val);
    EXPECT_TRUE(fv.find("k_999", val));
    EXPECT_EQ(999u, val);
}

GTEST_MAIN_RUN_ALL_TESTS()
