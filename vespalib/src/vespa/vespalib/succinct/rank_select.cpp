// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "rank_select.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>

#ifdef __BMI2__
#include <immintrin.h>
#endif

namespace vespalib::succinct {

RankSelect::RankSelect() noexcept
    : _words_store(),
      _super_blocks_store(),
      _block_ranks_store(),
      _words(nullptr),
      _super_blocks(nullptr),
      _block_ranks(nullptr),
      _num_words(0),
      _num_super_blocks(0),
      _size(0),
      _num_ones(0),
      _mmap_backed(false)
{
}

RankSelect::~RankSelect() = default;

RankSelect::RankSelect(RankSelect &&o) noexcept
    : _words_store(std::move(o._words_store)),
      _super_blocks_store(std::move(o._super_blocks_store)),
      _block_ranks_store(std::move(o._block_ranks_store)),
      _words(nullptr),
      _super_blocks(nullptr),
      _block_ranks(nullptr),
      _num_words(o._num_words),
      _num_super_blocks(o._num_super_blocks),
      _size(o._size),
      _num_ones(o._num_ones),
      _mmap_backed(o._mmap_backed)
{
    if (_mmap_backed) {
        _words = o._words;
        _super_blocks = o._super_blocks;
        _block_ranks = o._block_ranks;
    } else {
        sync_pointers();
    }
    o._words = nullptr;
    o._super_blocks = nullptr;
    o._block_ranks = nullptr;
    o._num_words = 0;
    o._num_super_blocks = 0;
    o._size = 0;
    o._num_ones = 0;
}

RankSelect &
RankSelect::operator=(RankSelect &&o) noexcept
{
    if (this != &o) {
        _words_store = std::move(o._words_store);
        _super_blocks_store = std::move(o._super_blocks_store);
        _block_ranks_store = std::move(o._block_ranks_store);
        _num_words = o._num_words;
        _num_super_blocks = o._num_super_blocks;
        _size = o._size;
        _num_ones = o._num_ones;
        _mmap_backed = o._mmap_backed;
        if (_mmap_backed) {
            _words = o._words;
            _super_blocks = o._super_blocks;
            _block_ranks = o._block_ranks;
        } else {
            sync_pointers();
        }
        o._words = nullptr;
        o._super_blocks = nullptr;
        o._block_ranks = nullptr;
        o._num_words = 0;
        o._num_super_blocks = 0;
        o._size = 0;
        o._num_ones = 0;
    }
    return *this;
}

void
RankSelect::sync_pointers() noexcept
{
    _words = _words_store.data();
    _super_blocks = _super_blocks_store.data();
    _block_ranks = _block_ranks_store.data();
    _num_words = _words_store.size();
    _num_super_blocks = _super_blocks_store.size();
}

void
RankSelect::build(const uint64_t *data, uint32_t size)
{
    uint32_t num_words = (size + WORD_BITS - 1) / WORD_BITS;
    _size = size;
    _words_store.assign(data, data + num_words);

    // Mask out unused bits in the last word
    if (size % WORD_BITS != 0) {
        uint64_t mask = (uint64_t{1} << (size % WORD_BITS)) - 1;
        _words_store.back() &= mask;
    }

    build_rank_cache();
    sync_pointers();
    _mmap_backed = false;
}

void
RankSelect::build_rank_cache()
{
    uint32_t num_words = _words_store.size();
    uint32_t num_super_blocks = (num_words + SUPER_BLOCK_WORDS - 1) / SUPER_BLOCK_WORDS;

    _super_blocks_store.resize(num_super_blocks + 1);
    _block_ranks_store.resize(num_words);

    uint32_t cumulative = 0;
    for (uint32_t i = 0; i < num_words; ++i) {
        uint32_t sb = i / SUPER_BLOCK_WORDS;
        uint32_t block_in_sb = i % SUPER_BLOCK_WORDS;
        if (block_in_sb == 0) {
            _super_blocks_store[sb] = cumulative;
        }
        // Store cumulative popcount relative to superblock start
        _block_ranks_store[i] = static_cast<uint16_t>(cumulative - _super_blocks_store[sb]);
        cumulative += Optimized::popCount(_words_store[i]);
    }
    _super_blocks_store[num_super_blocks] = cumulative;
    _num_ones = cumulative;
}

// rank1 is inlined in the header for performance

uint32_t
RankSelect::select1(uint32_t nth) const noexcept
{
    if (nth >= _num_ones) return _size;

    // Binary search on superblocks
    uint32_t lo = 0;
    uint32_t hi = _num_super_blocks - 1;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (_super_blocks[mid] <= nth) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    uint32_t remaining = nth - _super_blocks[lo];
    uint32_t word_start = lo * SUPER_BLOCK_WORDS;
    uint32_t word_end = std::min(_num_words, word_start + SUPER_BLOCK_WORDS);

    // Use block_ranks to skip within the superblock
    for (uint32_t i = word_start; i < word_end; ++i) {
        uint32_t pc = Optimized::popCount(_words[i]);
        if (remaining < pc) {
            return i * WORD_BITS + word_select(_words[i], remaining);
        }
        remaining -= pc;
    }

    return _size;
}

uint32_t
RankSelect::select0(uint32_t nth) const noexcept
{
    uint32_t num_zeros = _size - _num_ones;
    if (nth >= num_zeros) return _size;

    // Binary search on superblocks for zeros
    uint32_t lo = 0;
    uint32_t hi = _num_super_blocks - 1;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t zeros_before_mid = (uint32_t)((uint64_t)mid * SUPER_BLOCK_BITS - _super_blocks[mid]);
        if (zeros_before_mid <= nth) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    uint32_t zeros_at_sb = (uint32_t)((uint64_t)lo * SUPER_BLOCK_BITS - _super_blocks[lo]);
    uint32_t remaining = nth - zeros_at_sb;
    uint32_t word_start = lo * SUPER_BLOCK_WORDS;
    uint32_t word_end = std::min(_num_words, word_start + SUPER_BLOCK_WORDS);

    for (uint32_t i = word_start; i < word_end; ++i) {
        uint32_t zc = WORD_BITS - Optimized::popCount(_words[i]);
        if (i == _num_words - 1 && _size % WORD_BITS != 0) {
            zc = (_size % WORD_BITS) - Optimized::popCount(_words[i]);
        }
        if (remaining < zc) {
            return i * WORD_BITS + word_select(~_words[i], remaining);
        }
        remaining -= zc;
    }

    return _size;
}

uint32_t
RankSelect::one_seq_len(uint32_t pos) const noexcept
{
    if (pos >= _size) return 0;

    uint32_t word_idx = pos / WORD_BITS;
    uint32_t bit_idx = pos % WORD_BITS;

    uint64_t w = _words[word_idx] >> bit_idx;
    uint32_t remaining_in_word = WORD_BITS - bit_idx;
    if (~w != 0) {
        uint32_t first_zero = Optimized::lsbIdx(~w);
        if (first_zero < remaining_in_word) {
            return first_zero;
        }
    }

    uint32_t len = remaining_in_word;
    ++word_idx;

    while (word_idx < _num_words) {
        if (~_words[word_idx] != 0) {
            len += Optimized::lsbIdx(~_words[word_idx]);
            return std::min(len, _size - pos);
        }
        len += WORD_BITS;
        ++word_idx;
    }

    return std::min(len, _size - pos);
}

uint32_t
RankSelect::word_select(uint64_t word, uint32_t nth) noexcept
{
#ifdef __BMI2__
    // Use pdep to scatter the nth bit, then tzcnt to find its position
    uint64_t deposited = _pdep_u64(uint64_t{1} << nth, word);
    return __builtin_ctzll(deposited);
#else
    // Portable broadword select
    for (uint32_t i = 0; i < 64; ++i) {
        if (word & (uint64_t{1} << i)) {
            if (nth == 0) return i;
            --nth;
        }
    }
    return 64;
#endif
}

size_t
RankSelect::memory_usage() const noexcept
{
    return _num_words * sizeof(uint64_t)
         + _num_super_blocks * sizeof(uint32_t)
         + _num_words * sizeof(uint16_t)
         + sizeof(RankSelect);
}

// Serialization

size_t
RankSelect::serialized_size() const noexcept
{
    return 4 * sizeof(uint32_t)
         + _num_words * sizeof(uint64_t)
         + _num_super_blocks * sizeof(uint32_t)
         + _num_words * sizeof(uint16_t);
}

void
RankSelect::serialize(void *buf) const noexcept
{
    auto *p = static_cast<uint8_t *>(buf);

    std::memcpy(p, &_size, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(p, &_num_ones, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(p, &_num_words, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(p, &_num_super_blocks, sizeof(uint32_t)); p += sizeof(uint32_t);

    std::memcpy(p, _words, _num_words * sizeof(uint64_t));
    p += _num_words * sizeof(uint64_t);

    std::memcpy(p, _super_blocks, _num_super_blocks * sizeof(uint32_t));
    p += _num_super_blocks * sizeof(uint32_t);

    std::memcpy(p, _block_ranks, _num_words * sizeof(uint16_t));
}

void
RankSelect::load(const void *buf, size_t len)
{
    auto *p = static_cast<const uint8_t *>(buf);
    uint32_t num_words, num_super_blocks;

    if (len < 4 * sizeof(uint32_t)) {
        throw std::runtime_error("RankSelect::load: buffer too small for header");
    }

    std::memcpy(&_size, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(&_num_ones, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(&num_words, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(&num_super_blocks, p, sizeof(uint32_t)); p += sizeof(uint32_t);

    size_t expected = 4 * sizeof(uint32_t)
                    + num_words * sizeof(uint64_t)
                    + num_super_blocks * sizeof(uint32_t)
                    + num_words * sizeof(uint16_t);
    if (len < expected) {
        // Backward compatible: try loading without block_ranks and rebuild
        size_t min_expected = 4 * sizeof(uint32_t)
                            + num_words * sizeof(uint64_t)
                            + num_super_blocks * sizeof(uint32_t);
        if (len < min_expected) {
            throw std::runtime_error("RankSelect::load: buffer too small for data");
        }

        _words_store.resize(num_words);
        std::memcpy(_words_store.data(), p, num_words * sizeof(uint64_t));
        p += num_words * sizeof(uint64_t);

        _super_blocks_store.resize(num_super_blocks);
        std::memcpy(_super_blocks_store.data(), p, num_super_blocks * sizeof(uint32_t));

        // Rebuild block_ranks from words
        _block_ranks_store.resize(num_words);
        for (uint32_t i = 0; i < num_words; ++i) {
            uint32_t sb = i / SUPER_BLOCK_WORDS;
            uint32_t block_in_sb = i % SUPER_BLOCK_WORDS;
            if (block_in_sb == 0) {
                _block_ranks_store[i] = 0;
            } else {
                uint32_t prev_rank = _block_ranks_store[i - 1] + Optimized::popCount(_words_store[i - 1]);
                _block_ranks_store[i] = static_cast<uint16_t>(prev_rank);
            }
        }
        sync_pointers();
        _mmap_backed = false;
        return;
    }

    _words_store.resize(num_words);
    std::memcpy(_words_store.data(), p, num_words * sizeof(uint64_t));
    p += num_words * sizeof(uint64_t);

    _super_blocks_store.resize(num_super_blocks);
    std::memcpy(_super_blocks_store.data(), p, num_super_blocks * sizeof(uint32_t));
    p += num_super_blocks * sizeof(uint32_t);

    _block_ranks_store.resize(num_words);
    std::memcpy(_block_ranks_store.data(), p, num_words * sizeof(uint16_t));

    sync_pointers();
    _mmap_backed = false;
}

size_t
RankSelect::setup_mmap(const void *buf, size_t len)
{
    auto *p = static_cast<const uint8_t *>(buf);
    uint32_t num_words, num_super_blocks;

    if (len < 4 * sizeof(uint32_t)) {
        throw std::runtime_error("RankSelect::setup_mmap: buffer too small for header");
    }

    std::memcpy(&_size, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(&_num_ones, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(&num_words, p, sizeof(uint32_t)); p += sizeof(uint32_t);
    std::memcpy(&num_super_blocks, p, sizeof(uint32_t)); p += sizeof(uint32_t);

    size_t data_size = num_words * sizeof(uint64_t)
                     + num_super_blocks * sizeof(uint32_t)
                     + num_words * sizeof(uint16_t);
    if (len < 4 * sizeof(uint32_t) + data_size) {
        throw std::runtime_error("RankSelect::setup_mmap: buffer too small for data");
    }

    // Point directly into mmap'd memory — no copies
    _words = reinterpret_cast<const uint64_t *>(p);
    _num_words = num_words;
    p += num_words * sizeof(uint64_t);

    _super_blocks = reinterpret_cast<const uint32_t *>(p);
    _num_super_blocks = num_super_blocks;
    p += num_super_blocks * sizeof(uint32_t);

    _block_ranks = reinterpret_cast<const uint16_t *>(p);
    p += num_words * sizeof(uint16_t);

    // Release any owned storage
    _words_store.clear();
    _words_store.shrink_to_fit();
    _super_blocks_store.clear();
    _super_blocks_store.shrink_to_fit();
    _block_ranks_store.clear();
    _block_ranks_store.shrink_to_fit();

    _mmap_backed = true;
    return p - static_cast<const uint8_t *>(buf);
}

} // namespace vespalib::succinct
