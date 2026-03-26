// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "nlt_dictionary.h"
#include <vespa/fastos/file.h>
#include <cstring>
#include <fstream>
#include <cstdio>

namespace search::diskindex {

using index::PostingListCounts;
using index::PostingListOffsetAndCounts;
using index::PostingListParams;
using vespalib::succinct::NestedLoudsTrie;

// ============================================================================
// NltDictionaryFileSeqWrite
// ============================================================================

NltDictionaryFileSeqWrite::NltDictionaryFileSeqWrite()
    : _params(),
      _words(),
      _currentOffset(0),
      _currentAccNumDocs(0),
      _name()
{
}

NltDictionaryFileSeqWrite::~NltDictionaryFileSeqWrite() = default;

void
NltDictionaryFileSeqWrite::writeWord(vespalib::stringref word, const PostingListCounts &counts)
{
    _words.push_back({vespalib::string(word), counts, _currentOffset, _currentAccNumDocs});
    _currentOffset += counts._bitLength;
    _currentAccNumDocs += counts._numDocs;
}

bool
NltDictionaryFileSeqWrite::open(const vespalib::string &name,
                                 const TuneFileSeqWrite &,
                                 const FileHeaderContext &)
{
    _name = name;
    _words.clear();
    _currentOffset = 0;
    _currentAccNumDocs = 0;
    return true;
}

bool
NltDictionaryFileSeqWrite::close()
{
    // Build NLT from collected words
    NestedLoudsTrie::Builder builder;
    for (const auto &entry : _words) {
        builder.add(entry.word.data(), entry.word.size());
    }
    auto nlt = builder.finish();

    // Serialize NLT to file
    size_t nltSerSize = nlt->serialized_size();
    std::vector<uint8_t> nltBuf(nltSerSize);
    nlt->serialize(nltBuf.data());

    {
        std::ofstream out(_name + ".nlt", std::ios::binary);
        if (!out) {
            fprintf(stderr, "NLT: Could not open %s.nlt for write\n", _name.c_str());
            return false;
        }
        out.write(reinterpret_cast<const char *>(nltBuf.data()), nltSerSize);
    }

    // Write offsets file: fixed-width entries (32 bytes each) + segments appendix
    {
        std::ofstream out(_name + ".nlt.offsets", std::ios::binary);
        if (!out) {
            fprintf(stderr, "NLT: Could not open %s.nlt.offsets for write\n", _name.c_str());
            return false;
        }

        uint64_t numWords = _words.size();
        out.write(reinterpret_cast<const char *>(&numWords), sizeof(numWords));

        for (const auto &entry : _words) {
            uint64_t buf[4];
            buf[0] = entry.offset;
            buf[1] = entry.accNumDocs;
            buf[2] = entry.counts._numDocs;
            buf[3] = entry.counts._bitLength;
            out.write(reinterpret_cast<const char *>(buf), sizeof(buf));
        }

        // Write segment data for words that have segments
        std::vector<std::pair<uint64_t, const PostingListCounts *>> segmented;
        for (uint64_t i = 0; i < _words.size(); ++i) {
            if (!_words[i].counts._segments.empty()) {
                segmented.emplace_back(i, &_words[i].counts);
            }
        }

        uint64_t numSegmented = segmented.size();
        out.write(reinterpret_cast<const char *>(&numSegmented), sizeof(numSegmented));

        for (auto &[ordinal, counts] : segmented) {
            out.write(reinterpret_cast<const char *>(&ordinal), sizeof(ordinal));
            uint32_t numSegs = counts->_segments.size();
            out.write(reinterpret_cast<const char *>(&numSegs), sizeof(numSegs));
            for (const auto &seg : counts->_segments) {
                out.write(reinterpret_cast<const char *>(&seg._bitLength), sizeof(seg._bitLength));
                out.write(reinterpret_cast<const char *>(&seg._numDocs), sizeof(seg._numDocs));
                out.write(reinterpret_cast<const char *>(&seg._lastDoc), sizeof(seg._lastDoc));
            }
        }
    }

    _words.clear();
    return true;
}

void
NltDictionaryFileSeqWrite::setParams(const PostingListParams &params)
{
    _params = params;
}

void
NltDictionaryFileSeqWrite::getParams(PostingListParams &params)
{
    params = _params;
}

// ============================================================================
// NltDictionaryFileSeqRead
// ============================================================================

NltDictionaryFileSeqRead::NltDictionaryFileSeqRead()
    : _nlt(),
      _iterator(),
      _offsets(),
      _wordNum(0),
      _params()
{
}

NltDictionaryFileSeqRead::~NltDictionaryFileSeqRead() = default;

static std::vector<uint8_t> readFile(const vespalib::string &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    auto pos = in.tellg();
    if (pos <= 0 || !in.good()) return {};
    size_t size = static_cast<size_t>(pos);
    in.seekg(0);
    std::vector<uint8_t> buf(size);
    in.read(reinterpret_cast<char *>(buf.data()), size);
    if (!in.good()) return {};
    return buf;
}

void
NltDictionaryFileSeqRead::readWord(vespalib::string &word, uint64_t &wordNum,
                                    PostingListCounts &counts)
{
    if (!_iterator || !_iterator->valid()) {
        wordNum = noWordNumHigh();
        counts.clear();
        return;
    }

    word = _iterator->key();
    _wordNum++;
    wordNum = _wordNum;

    size_t ordinal = _iterator->ordinal();
    if (ordinal < _offsets.size()) {
        counts = _offsets[ordinal].counts;
    } else {
        counts.clear();
    }

    _iterator->next();
}

bool
NltDictionaryFileSeqRead::open(const vespalib::string &name, const TuneFileSeqRead &)
{
    // Load NLT
    auto nltBuf = readFile(name + ".nlt");
    if (nltBuf.empty()) {
        fprintf(stderr, "NLT: Could not read %s.nlt\n", name.c_str());
        return false;
    }

    _nlt = std::make_unique<NestedLoudsTrie>();
    _nlt->load(nltBuf.data(), nltBuf.size());

    // Load offsets
    auto offsetsBuf = readFile(name + ".nlt.offsets");
    if (offsetsBuf.empty()) {
        fprintf(stderr, "NLT: Could not read %s.nlt.offsets\n", name.c_str());
        return false;
    }

    const uint8_t *p = offsetsBuf.data();
    uint64_t numWords;
    std::memcpy(&numWords, p, sizeof(numWords)); p += sizeof(numWords);

    _offsets.resize(numWords);
    for (uint64_t i = 0; i < numWords; ++i) {
        uint64_t buf[4];
        std::memcpy(buf, p, sizeof(buf)); p += sizeof(buf);
        _offsets[i].offset = buf[0];
        _offsets[i].accNumDocs = buf[1];
        _offsets[i].counts._numDocs = buf[2];
        _offsets[i].counts._bitLength = buf[3];
    }

    // Load segments
    const uint8_t *end = offsetsBuf.data() + offsetsBuf.size();
    if (p + sizeof(uint64_t) <= end) {
        uint64_t numSegmented;
        std::memcpy(&numSegmented, p, sizeof(numSegmented)); p += sizeof(numSegmented);

        constexpr size_t SEG_SIZE = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
        for (uint64_t s = 0; s < numSegmented && p + sizeof(uint64_t) + sizeof(uint32_t) <= end; ++s) {
            uint64_t ordinal;
            std::memcpy(&ordinal, p, sizeof(ordinal)); p += sizeof(ordinal);
            uint32_t numSegs;
            std::memcpy(&numSegs, p, sizeof(numSegs)); p += sizeof(numSegs);

            if (ordinal < _offsets.size() && p + numSegs * SEG_SIZE <= end) {
                auto &segs = _offsets[ordinal].counts._segments;
                segs.resize(numSegs);
                for (uint32_t j = 0; j < numSegs; ++j) {
                    std::memcpy(&segs[j]._bitLength, p, sizeof(segs[j]._bitLength)); p += sizeof(segs[j]._bitLength);
                    std::memcpy(&segs[j]._numDocs, p, sizeof(segs[j]._numDocs)); p += sizeof(segs[j]._numDocs);
                    std::memcpy(&segs[j]._lastDoc, p, sizeof(segs[j]._lastDoc)); p += sizeof(segs[j]._lastDoc);
                }
            }
        }
    }

    _iterator = std::make_unique<NestedLoudsTrie::Iterator>(_nlt->begin());
    _wordNum = 0;

    return true;
}

bool
NltDictionaryFileSeqRead::close()
{
    _iterator.reset();
    _nlt.reset();
    _offsets.clear();
    _wordNum = 0;
    return true;
}

void
NltDictionaryFileSeqRead::getParams(PostingListParams &params)
{
    params = _params;
}

// ============================================================================
// NltDictionaryFileRandRead
// ============================================================================

NltDictionaryFileRandRead::NltDictionaryFileRandRead()
    : _nlt(),
      _nltFile(),
      _offsetsFile(),
      _offsetsBuf(),
      _nltBuf(),
      _offsetsData(nullptr),
      _offsetsSize(0),
      _segments(),
      _numWordIds(0)
{
}

NltDictionaryFileRandRead::~NltDictionaryFileRandRead() = default;

bool
NltDictionaryFileRandRead::lookup(vespalib::stringref word, uint64_t &wordNum,
                                   PostingListOffsetAndCounts &offsetAndCounts)
{
    if (!_nlt || _nlt->empty()) {
        wordNum = 0;
        offsetAndCounts = PostingListOffsetAndCounts();
        return false;
    }

    size_t ordinal = _nlt->lookup(word.data(), word.size());
    if (ordinal == NestedLoudsTrie::NOT_FOUND) {
        wordNum = 0;
        offsetAndCounts = PostingListOffsetAndCounts();
        return false;
    }

    size_t entry_end = 8 + (ordinal + 1) * OFFSET_ENTRY_SIZE;
    if (entry_end > _offsetsSize) {
        wordNum = 0;
        offsetAndCounts = PostingListOffsetAndCounts();
        return false;
    }

    wordNum = ordinal + 1;

    const uint8_t *entry = _offsetsData + 8 + ordinal * OFFSET_ENTRY_SIZE;
    uint64_t buf[4];
    std::memcpy(buf, entry, sizeof(buf));

    offsetAndCounts._offset = buf[0];
    offsetAndCounts._accNumDocs = buf[1];
    offsetAndCounts._counts._numDocs = buf[2];
    offsetAndCounts._counts._bitLength = buf[3];

    if (ordinal < _segments.size() && !_segments[ordinal].segments.empty()) {
        offsetAndCounts._counts._segments = _segments[ordinal].segments;
    }

    return true;
}

bool
NltDictionaryFileRandRead::open(const vespalib::string &name, const TuneFileRandRead &tuneFileRead)
{
    vespalib::string nltPath = name + ".nlt";
    vespalib::string offsetsPath = name + ".nlt.offsets";

    // Try mmap-based loading
    bool useMmap = tuneFileRead.getWantMemoryMap();

    if (useMmap) {
        int mmapFlags = tuneFileRead.getMemoryMapFlags();
        int fadvise = tuneFileRead.getAdvise();

        // Memory-map NLT file
        _nltFile = std::make_unique<FastOS_File>();
        _nltFile->enableMemoryMap(mmapFlags);
        _nltFile->setFAdviseOptions(fadvise);
        if (!_nltFile->OpenReadOnly(nltPath.c_str())) {
            _nltFile.reset();
            useMmap = false;
        }
    }

    if (useMmap && _nltFile && _nltFile->MemoryMapPtr(0) != nullptr) {
        // Zero-copy mmap path: NLT points directly into mmap'd memory
        const auto *nltData = static_cast<const uint8_t *>(_nltFile->MemoryMapPtr(0));
        size_t nltSize = _nltFile->GetSize();

        _nlt = std::make_unique<NestedLoudsTrie>();
        _nlt->setup_mmap(nltData, nltSize);

        // Memory-map offsets file
        int mmapFlags = tuneFileRead.getMemoryMapFlags();
        int fadvise = tuneFileRead.getAdvise();
        _offsetsFile = std::make_unique<FastOS_File>();
        _offsetsFile->enableMemoryMap(mmapFlags);
        _offsetsFile->setFAdviseOptions(fadvise);

        if (_offsetsFile->OpenReadOnly(offsetsPath.c_str()) &&
            _offsetsFile->MemoryMapPtr(0) != nullptr)
        {
            _offsetsData = static_cast<const uint8_t *>(_offsetsFile->MemoryMapPtr(0));
            _offsetsSize = _offsetsFile->GetSize();
        } else {
            // Offsets mmap failed — fall back to buffer for offsets only
            _offsetsFile.reset();
            _offsetsBuf = readFile(offsetsPath);
            _offsetsData = _offsetsBuf.data();
            _offsetsSize = _offsetsBuf.size();
        }

        _memoryMapped = true;
    } else {
        // Fallback: read files into owned buffers
        _nltFile.reset();

        _nltBuf = readFile(nltPath);
        if (_nltBuf.empty()) {
            fprintf(stderr, "NLT: Could not read %s\n", nltPath.c_str());
            return false;
        }

        _nlt = std::make_unique<NestedLoudsTrie>();
        _nlt->load(_nltBuf.data(), _nltBuf.size());

        _offsetsBuf = readFile(offsetsPath);
        if (_offsetsBuf.empty()) {
            fprintf(stderr, "NLT: Could not read %s\n", offsetsPath.c_str());
            return false;
        }
        _offsetsData = _offsetsBuf.data();
        _offsetsSize = _offsetsBuf.size();

        _memoryMapped = false;
    }

    // Read numWords from offsets header
    if (_offsetsSize < sizeof(uint64_t)) {
        fprintf(stderr, "NLT: Offsets file too small: %s\n", offsetsPath.c_str());
        return false;
    }
    uint64_t numWords;
    std::memcpy(&numWords, _offsetsData, sizeof(numWords));
    _numWordIds = numWords;

    // Load segments (at the end of the offsets file, always parsed into owned vectors)
    const uint8_t *p = _offsetsData + 8 + numWords * OFFSET_ENTRY_SIZE;
    const uint8_t *end = _offsetsData + _offsetsSize;
    size_t remaining = end - p;
    if (remaining >= sizeof(uint64_t)) {
        uint64_t numSegmented;
        std::memcpy(&numSegmented, p, sizeof(numSegmented)); p += sizeof(numSegmented);

        _segments.resize(numWords);
        for (uint64_t s = 0; s < numSegmented && p + sizeof(uint64_t) + sizeof(uint32_t) <= end; ++s) {
            uint64_t ordinal;
            std::memcpy(&ordinal, p, sizeof(ordinal)); p += sizeof(ordinal);
            uint32_t numSegs;
            std::memcpy(&numSegs, p, sizeof(numSegs)); p += sizeof(numSegs);

            constexpr size_t SEG_SIZE = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
            if (ordinal < numWords && p + numSegs * SEG_SIZE <= end) {
                auto &segs = _segments[ordinal].segments;
                segs.resize(numSegs);
                for (uint32_t j = 0; j < numSegs; ++j) {
                    std::memcpy(&segs[j]._bitLength, p, sizeof(segs[j]._bitLength)); p += sizeof(segs[j]._bitLength);
                    std::memcpy(&segs[j]._numDocs, p, sizeof(segs[j]._numDocs)); p += sizeof(segs[j]._numDocs);
                    std::memcpy(&segs[j]._lastDoc, p, sizeof(segs[j]._lastDoc)); p += sizeof(segs[j]._lastDoc);
                }
            }
        }
    }

    if (_memoryMapped) {
        afterOpen(*_nltFile);
    }

    return true;
}

bool
NltDictionaryFileRandRead::close()
{
    _nlt.reset();
    _offsetsData = nullptr;
    _offsetsSize = 0;
    _offsetsBuf.clear();
    _nltBuf.clear();
    _segments.clear();
    _numWordIds = 0;
    if (_nltFile) {
        _nltFile->Close();
        _nltFile.reset();
    }
    if (_offsetsFile) {
        _offsetsFile->Close();
        _offsetsFile.reset();
    }
    _memoryMapped = false;
    return true;
}

uint64_t
NltDictionaryFileRandRead::getNumWordIds() const
{
    return _numWordIds;
}

} // namespace search::diskindex
