// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#pragma once

#include <vespa/searchlib/index/dictionaryfile.h>
#include <vespa/searchlib/index/postinglistparams.h>
#include <vespa/vespalib/succinct/nested_louds_trie.h>
#include <memory>
#include <vector>

class FastOS_FileInterface;

namespace search::diskindex {

/**
 * NLT dictionary file for sequential write.
 *
 * Collects all words and their PostingListCounts during writeWord() calls,
 * then builds a NestedLoudsTrie and writes it along with an offsets file
 * on close(). File layout:
 *
 *   {name}.nlt          - serialized NestedLoudsTrie (term -> ordinal)
 *   {name}.nlt.offsets   - fixed-width array of PostingListOffsetAndCounts
 *
 * The offsets file enables O(1) ordinal->offset lookup. Negative lookups
 * (term not found in trie) never touch the offsets file.
 */
class NltDictionaryFileSeqWrite : public index::DictionaryFileSeqWrite {
    using PostingListCounts = index::PostingListCounts;
    using FileHeaderContext = common::FileHeaderContext;

    struct WordEntry {
        vespalib::string word;
        PostingListCounts counts;
        uint64_t offset;       // accumulated bit offset
        uint64_t accNumDocs;   // accumulated doc count
    };

    index::PostingListParams _params;
    std::vector<WordEntry> _words;
    uint64_t _currentOffset;
    uint64_t _currentAccNumDocs;
    vespalib::string _name;

public:
    NltDictionaryFileSeqWrite();
    ~NltDictionaryFileSeqWrite() override;

    void writeWord(vespalib::stringref word, const PostingListCounts &counts) override;

    bool open(const vespalib::string &name, const TuneFileSeqWrite &tune,
              const FileHeaderContext &fileHeaderContext) override;
    bool close() override;
    void setParams(const index::PostingListParams &params) override;
    void getParams(index::PostingListParams &params) override;
};

/**
 * NLT dictionary file for sequential read.
 *
 * Used during fusion to iterate over all words in lexicographic order.
 * Loads the NLT and offsets, then uses the NLT iterator to produce words.
 */
class NltDictionaryFileSeqRead : public index::DictionaryFileSeqRead {
    using PostingListCounts = index::PostingListCounts;

    struct OffsetEntry {
        uint64_t offset;
        uint64_t accNumDocs;
        PostingListCounts counts;
    };

    std::unique_ptr<vespalib::succinct::NestedLoudsTrie> _nlt;
    std::unique_ptr<vespalib::succinct::NestedLoudsTrie::Iterator> _iterator;
    std::vector<OffsetEntry> _offsets;
    uint64_t _wordNum;
    index::PostingListParams _params;

public:
    NltDictionaryFileSeqRead();
    ~NltDictionaryFileSeqRead() override;

    void readWord(vespalib::string &word, uint64_t &wordNum, PostingListCounts &counts) override;
    bool open(const vespalib::string &name, const TuneFileSeqRead &tuneFileRead) override;
    bool close() override;
    void getParams(index::PostingListParams &params) override;
};

/**
 * NLT dictionary file for random access read (search-time lookup).
 *
 * Memory-maps the NLT trie and offsets file for zero-copy access.
 * lookup() does:
 *   1. NLT trie traversal: O(key_length), typically terminates early for misses
 *   2. If found: single indexed read from offsets array
 *
 * For non-existent terms, only the trie is accessed (no I/O to offsets file).
 */
class NltDictionaryFileRandRead : public index::DictionaryFileRandRead {
    using PostingListCounts = index::PostingListCounts;
    using PostingListOffsetAndCounts = index::PostingListOffsetAndCounts;

    std::unique_ptr<vespalib::succinct::NestedLoudsTrie> _nlt;

    // Memory-mapped offsets: fixed-width entries for O(1) access
    struct OffsetEntry {
        uint64_t offset;
        uint64_t accNumDocs;
        uint64_t numDocs;
        uint64_t bitLength;
    };

    // Memory-mapped file handles
    std::unique_ptr<FastOS_FileInterface> _nltFile;
    std::unique_ptr<FastOS_FileInterface> _offsetsFile;

    // Fallback: owned buffers if mmap not available
    std::vector<uint8_t> _offsetsBuf;
    std::vector<uint8_t> _nltBuf;

    const uint8_t *_offsetsData;
    size_t _offsetsSize;

    // Variable-length segment data per word (only for words with segments)
    struct SegmentData {
        std::vector<PostingListCounts::Segment> segments;
    };
    std::vector<SegmentData> _segments; // indexed by ordinal, empty for most words

    uint64_t _numWordIds;

    static constexpr size_t OFFSET_ENTRY_SIZE = 32; // 4 x uint64_t

public:
    NltDictionaryFileRandRead();
    ~NltDictionaryFileRandRead() override;

    bool lookup(vespalib::stringref word, uint64_t &wordNum,
                PostingListOffsetAndCounts &offsetAndCounts) override;

    bool open(const vespalib::string &name, const TuneFileRandRead &tuneFileRead) override;
    bool close() override;
    uint64_t getNumWordIds() const override;
};

} // namespace search::diskindex
