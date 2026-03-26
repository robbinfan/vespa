// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#pragma once

#include <vespa/vespalib/util/optimized.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cassert>

namespace vespalib::succinct {

/**
 * Succinct bit vector supporting O(1) rank and O(log n) select operations.
 *
 * rank1(pos) returns the number of 1-bits in positions [0, pos).
 * select1(nth) returns the position of the nth 1-bit (0-based).
 *
 * Uses a two-level directory:
 * - Superblock (every 2048 bits): absolute cumulative popcount (uint32_t)
 * - Block (every 64 bits): relative cumulative popcount within superblock (uint16_t)
 *
 * rank1 is O(1): one superblock lookup + one block lookup + one popcount.
 * select uses binary search on superblocks + linear scan on blocks.
 *
 * Supports zero-copy mmap mode: setup_mmap() sets pointers directly into
 * memory-mapped data without copying.
 **/
class RankSelect {
public:
    static constexpr uint32_t WORD_BITS = 64;
    static constexpr uint32_t BLOCK_BITS = 64;
    static constexpr uint32_t SUPER_BLOCK_WORDS = 32; // 2048 bits
    static constexpr uint32_t SUPER_BLOCK_BITS = SUPER_BLOCK_WORDS * WORD_BITS;

    RankSelect() noexcept;
    ~RankSelect();

    RankSelect(RankSelect &&o) noexcept;
    RankSelect &operator=(RankSelect &&o) noexcept;
    RankSelect(const RankSelect &) = delete;
    RankSelect &operator=(const RankSelect &) = delete;

    uint32_t size() const noexcept { return _size; }
    uint32_t count_ones() const noexcept { return _num_ones; }
    uint32_t count_zeros() const noexcept { return _size - _num_ones; }
    bool empty() const noexcept { return _size == 0; }

    bool test(uint32_t pos) const noexcept {
        assert(pos < _size);
        return (_words[pos / WORD_BITS] >> (pos % WORD_BITS)) & 1;
    }

    /**
     * O(1) rank: count of 1-bits in [0, pos).
     * Uses superblock + block directory + one popcount instruction.
     **/
    uint32_t rank1(uint32_t pos) const noexcept {
        if (pos == 0) return 0;
        if (pos >= _size) return _num_ones;
        uint32_t word_idx = pos / WORD_BITS;
        uint32_t bit_idx = pos % WORD_BITS;
        uint32_t sb = word_idx / SUPER_BLOCK_WORDS;
        uint32_t rank = _super_blocks[sb] + _block_ranks[word_idx];
        if (bit_idx > 0) {
            rank += Optimized::popCount(_words[word_idx] & ((uint64_t{1} << bit_idx) - 1));
        }
        return rank;
    }

    uint32_t rank0(uint32_t pos) const noexcept { return pos - rank1(pos); }
    uint32_t select1(uint32_t nth) const noexcept;
    uint32_t select0(uint32_t nth) const noexcept;

    /**
     * Count the length of consecutive 1-bits starting at pos.
     * Useful for LOUDS degree computation.
     **/
    uint32_t one_seq_len(uint32_t pos) const noexcept;

    void build(const uint64_t *data, uint32_t size);

    size_t memory_usage() const noexcept;

    const uint64_t *words() const noexcept { return _words; }
    uint32_t word_count() const noexcept { return _num_words; }

    /**
     * Serialization layout:
     * [uint32_t size][uint32_t num_ones]
     * [uint32_t num_words][uint32_t num_super_blocks]
     * [words...][super_blocks...][block_ranks...]
     **/
    size_t serialized_size() const noexcept;
    void serialize(void *buf) const noexcept;
    void load(const void *buf, size_t len);

    /**
     * Zero-copy mmap setup: point directly into memory-mapped data.
     * Returns the number of bytes consumed from buf.
     * The caller must keep the underlying mmap alive while this object is in use.
     **/
    size_t setup_mmap(const void *buf, size_t len);

private:
    void build_rank_cache();
    void sync_pointers() noexcept;
    static uint32_t word_select(uint64_t word, uint32_t nth) noexcept;

    // Owned storage (populated by build() and load(), empty in mmap mode)
    std::vector<uint64_t>  _words_store;
    std::vector<uint32_t>  _super_blocks_store;
    std::vector<uint16_t>  _block_ranks_store;

    // Active data pointers (used by all read methods)
    // Point into _*_store (owned mode) or mmap memory (mmap mode)
    const uint64_t*  _words;
    const uint32_t*  _super_blocks;
    const uint16_t*  _block_ranks;
    uint32_t _num_words;
    uint32_t _num_super_blocks;

    uint32_t _size;
    uint32_t _num_ones;
    bool _mmap_backed;
};

} // namespace vespalib::succinct
