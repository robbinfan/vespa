// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/vespalib/succinct/rank_select.h>
#include <vespa/vespalib/gtest/gtest.h>
#include <vespa/vespalib/util/time.h>
#include <vector>
#include <random>
#include <algorithm>

using vespalib::succinct::RankSelect;

namespace {

// Brute-force reference implementation for correctness validation
uint32_t naive_rank1(const std::vector<uint64_t> &words, uint32_t size, uint32_t pos) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < pos && i < size; ++i) {
        if ((words[i / 64] >> (i % 64)) & 1) ++count;
    }
    return count;
}

uint32_t naive_select1(const std::vector<uint64_t> &words, uint32_t size, uint32_t nth) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < size; ++i) {
        if ((words[i / 64] >> (i % 64)) & 1) {
            if (count == nth) return i;
            ++count;
        }
    }
    return size;
}

} // anonymous namespace

TEST(RankSelectTest, empty_bit_vector)
{
    RankSelect rs;
    EXPECT_TRUE(rs.empty());
    EXPECT_EQ(0u, rs.size());
    EXPECT_EQ(0u, rs.count_ones());
}

TEST(RankSelectTest, single_word_all_ones)
{
    uint64_t word = 0xFFFFFFFFFFFFFFFFULL;
    RankSelect rs;
    rs.build(&word, 64);

    EXPECT_EQ(64u, rs.size());
    EXPECT_EQ(64u, rs.count_ones());
    EXPECT_EQ(0u, rs.count_zeros());

    for (uint32_t i = 0; i <= 64; ++i) {
        EXPECT_EQ(i, rs.rank1(i)) << "rank1(" << i << ")";
    }
    for (uint32_t i = 0; i < 64; ++i) {
        EXPECT_EQ(i, rs.select1(i)) << "select1(" << i << ")";
        EXPECT_TRUE(rs.test(i));
    }
}

TEST(RankSelectTest, single_word_all_zeros)
{
    uint64_t word = 0;
    RankSelect rs;
    rs.build(&word, 64);

    EXPECT_EQ(64u, rs.size());
    EXPECT_EQ(0u, rs.count_ones());
    EXPECT_EQ(64u, rs.count_zeros());

    for (uint32_t i = 0; i <= 64; ++i) {
        EXPECT_EQ(0u, rs.rank1(i));
    }
    for (uint32_t i = 0; i < 64; ++i) {
        EXPECT_EQ(i, rs.select0(i)) << "select0(" << i << ")";
        EXPECT_FALSE(rs.test(i));
    }
}

TEST(RankSelectTest, partial_word)
{
    // Only 10 bits, pattern: 1010101010
    uint64_t word = 0b1010101010;
    RankSelect rs;
    rs.build(&word, 10);

    EXPECT_EQ(10u, rs.size());
    EXPECT_EQ(5u, rs.count_ones());

    EXPECT_EQ(0u, rs.rank1(0));
    EXPECT_EQ(0u, rs.rank1(1));  // bit 0 is 0
    EXPECT_EQ(1u, rs.rank1(2));  // bit 1 is 1
    EXPECT_EQ(1u, rs.rank1(3));  // bit 2 is 0
    EXPECT_EQ(2u, rs.rank1(4));  // bit 3 is 1

    EXPECT_EQ(1u, rs.select1(0));  // first 1-bit is at position 1
    EXPECT_EQ(3u, rs.select1(1));  // second 1-bit is at position 3
}

TEST(RankSelectTest, multi_word)
{
    // 128 bits: first 64 all ones, next 64 all zeros
    std::vector<uint64_t> words = {0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL};
    RankSelect rs;
    rs.build(words.data(), 128);

    EXPECT_EQ(128u, rs.size());
    EXPECT_EQ(64u, rs.count_ones());

    EXPECT_EQ(32u, rs.rank1(32));
    EXPECT_EQ(64u, rs.rank1(64));
    EXPECT_EQ(64u, rs.rank1(96));
    EXPECT_EQ(64u, rs.rank1(128));

    EXPECT_EQ(0u, rs.select1(0));
    EXPECT_EQ(63u, rs.select1(63));
    EXPECT_EQ(128u, rs.select1(64)); // out of range

    EXPECT_EQ(64u, rs.select0(0));  // first 0-bit at position 64
    EXPECT_EQ(127u, rs.select0(63));
}

TEST(RankSelectTest, one_seq_len)
{
    // Pattern: 64 ones, 1 zero, 3 ones, rest zeros
    std::vector<uint64_t> words = {0xFFFFFFFFFFFFFFFFULL, 0x000000000000000EULL};
    RankSelect rs;
    rs.build(words.data(), 128);

    EXPECT_EQ(64u, rs.one_seq_len(0));   // 64 consecutive ones from pos 0
    EXPECT_EQ(1u, rs.one_seq_len(63));   // just bit 63
    EXPECT_EQ(0u, rs.one_seq_len(64));   // bit 64 is 0
    EXPECT_EQ(3u, rs.one_seq_len(65));   // bits 65,66,67 are 1
}

TEST(RankSelectTest, cross_superblock_boundary)
{
    // Create a bit vector spanning multiple superblocks (>2048 bits)
    constexpr uint32_t total_bits = 4096;
    constexpr uint32_t num_words = total_bits / 64;
    std::vector<uint64_t> words(num_words, 0);

    // Set every other bit
    for (uint32_t i = 0; i < num_words; ++i) {
        words[i] = 0x5555555555555555ULL; // 01010101...
    }

    RankSelect rs;
    rs.build(words.data(), total_bits);

    EXPECT_EQ(total_bits, rs.size());
    EXPECT_EQ(total_bits / 2, rs.count_ones());

    // Check rank at superblock boundary (2048)
    uint32_t rank_at_2048 = rs.rank1(2048);
    EXPECT_EQ(1024u, rank_at_2048);

    // Check rank at end
    EXPECT_EQ(2048u, rs.rank1(4096));

    // Verify select across superblock boundary
    EXPECT_EQ(2048u, rs.select1(1024)); // 1024th one is at bit 2048
}

TEST(RankSelectTest, random_correctness)
{
    std::mt19937 rng(42);
    constexpr uint32_t size = 10000;
    constexpr uint32_t num_words = (size + 63) / 64;
    std::vector<uint64_t> words(num_words);

    for (auto &w : words) {
        w = ((uint64_t)rng() << 32) | rng();
    }

    RankSelect rs;
    rs.build(words.data(), size);

    // Verify rank1 against naive implementation at 200 random positions
    std::uniform_int_distribution<uint32_t> dist(0, size);
    for (int i = 0; i < 200; ++i) {
        uint32_t pos = dist(rng);
        uint32_t expected = naive_rank1(words, size, pos);
        EXPECT_EQ(expected, rs.rank1(pos)) << "rank1(" << pos << ")";
    }

    // Verify select1 against naive for first 100 ones
    uint32_t max_nth = std::min(rs.count_ones(), 100u);
    for (uint32_t i = 0; i < max_nth; ++i) {
        uint32_t expected = naive_select1(words, size, i);
        EXPECT_EQ(expected, rs.select1(i)) << "select1(" << i << ")";
    }

    // Verify rank-select identity: select1(rank1(pos)) <= pos for all set bit positions
    for (uint32_t i = 0; i < size; ++i) {
        if (rs.test(i)) {
            uint32_t r = rs.rank1(i);
            EXPECT_EQ(i, rs.select1(r)) << "rank-select identity at " << i;
        }
    }
}

TEST(RankSelectTest, serialization_round_trip)
{
    std::mt19937 rng(123);
    constexpr uint32_t size = 5000;
    constexpr uint32_t num_words = (size + 63) / 64;
    std::vector<uint64_t> words(num_words);
    for (auto &w : words) w = ((uint64_t)rng() << 32) | rng();

    RankSelect rs1;
    rs1.build(words.data(), size);

    // Serialize
    size_t ser_size = rs1.serialized_size();
    std::vector<uint8_t> buf(ser_size);
    rs1.serialize(buf.data());

    // Load
    RankSelect rs2;
    rs2.load(buf.data(), buf.size());

    EXPECT_EQ(rs1.size(), rs2.size());
    EXPECT_EQ(rs1.count_ones(), rs2.count_ones());

    // Verify identical rank results
    for (uint32_t pos = 0; pos <= size; pos += 7) {
        EXPECT_EQ(rs1.rank1(pos), rs2.rank1(pos));
    }
    for (uint32_t i = 0; i < std::min(rs1.count_ones(), 50u); ++i) {
        EXPECT_EQ(rs1.select1(i), rs2.select1(i));
    }
}

TEST(RankSelectTest, bench_rank1)
{
    std::mt19937 rng(99);
    constexpr uint32_t size = 1'000'000;
    constexpr uint32_t num_words = (size + 63) / 64;
    std::vector<uint64_t> words(num_words);
    for (auto &w : words) w = ((uint64_t)rng() << 32) | rng();

    RankSelect rs;
    rs.build(words.data(), size);

    // Generate random positions
    constexpr uint32_t num_queries = 1'000'000;
    std::vector<uint32_t> positions(num_queries);
    std::uniform_int_distribution<uint32_t> dist(0, size - 1);
    for (auto &p : positions) p = dist(rng);

    vespalib::Timer timer;
    uint64_t checksum = 0;
    for (auto p : positions) {
        checksum += rs.rank1(p);
    }
    auto elapsed = timer.elapsed();

    double ns_per_op = (double)vespalib::count_ns(elapsed) / num_queries;
    fprintf(stderr, "RankSelect::rank1: %.1f ns/op (1M bits, 1M queries, checksum=%lu)\n",
            ns_per_op, checksum);
    // Expect < 200ns per rank1 operation
    EXPECT_LT(ns_per_op, 2000.0); // generous bound for CI
}

TEST(RankSelectTest, bench_select1)
{
    std::mt19937 rng(77);
    constexpr uint32_t size = 1'000'000;
    constexpr uint32_t num_words = (size + 63) / 64;
    std::vector<uint64_t> words(num_words);
    for (auto &w : words) w = ((uint64_t)rng() << 32) | rng();

    RankSelect rs;
    rs.build(words.data(), size);

    constexpr uint32_t num_queries = 1'000'000;
    std::vector<uint32_t> queries(num_queries);
    std::uniform_int_distribution<uint32_t> dist(0, rs.count_ones() - 1);
    for (auto &q : queries) q = dist(rng);

    vespalib::Timer timer;
    uint64_t checksum = 0;
    for (auto q : queries) {
        checksum += rs.select1(q);
    }
    auto elapsed = timer.elapsed();

    double ns_per_op = (double)vespalib::count_ns(elapsed) / num_queries;
    fprintf(stderr, "RankSelect::select1: %.1f ns/op (1M bits, 1M queries, checksum=%lu)\n",
            ns_per_op, checksum);
    EXPECT_LT(ns_per_op, 5000.0); // generous bound for CI
}

TEST(RankSelectTest, memory_usage_overhead)
{
    constexpr uint32_t size = 1'000'000;
    constexpr uint32_t num_words = (size + 63) / 64;
    std::vector<uint64_t> words(num_words, 0x5555555555555555ULL);

    RankSelect rs;
    rs.build(words.data(), size);

    size_t raw_bits = num_words * sizeof(uint64_t);
    size_t total = rs.memory_usage();
    double overhead_pct = 100.0 * (total - raw_bits) / raw_bits;

    fprintf(stderr, "RankSelect memory: raw=%zu total=%zu overhead=%.1f%%\n",
            raw_bits, total, overhead_pct);
    // Overhead should be < 10% for reasonable sizes
    EXPECT_LT(overhead_pct, 20.0);
}

GTEST_API_ int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
