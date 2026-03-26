// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/searchlib/diskindex/nlt_dictionary.h>
#include <vespa/searchlib/index/dummyfileheadercontext.h>
#include <vespa/searchlib/index/postinglistcounts.h>
#include <vespa/vespalib/gtest/gtest.h>
#include <filesystem>
#include <random>
#include <set>

using search::diskindex::NltDictionaryFileSeqWrite;
using search::diskindex::NltDictionaryFileSeqRead;
using search::diskindex::NltDictionaryFileRandRead;
using search::index::PostingListCounts;
using search::index::PostingListOffsetAndCounts;
using search::index::DictionaryFileSeqRead;
using search::TuneFileSeqWrite;
using search::TuneFileSeqRead;
using search::TuneFileRandRead;

namespace search::diskindex {

namespace {

struct TestWord {
    vespalib::string word;
    PostingListCounts counts;
    uint64_t expectedOffset;
};

using search::index::DummyFileHeaderContext;

std::vector<TestWord> makeTestWords() {
    std::vector<TestWord> words;
    uint64_t offset = 0;
    auto add = [&](const char *w, uint64_t numDocs, uint64_t bitLen) {
        PostingListCounts c;
        c._numDocs = numDocs;
        c._bitLength = bitLen;
        words.push_back({vespalib::string(w), c, offset});
        offset += bitLen;
    };
    // Must be sorted!
    add("adapter", 100, 1000);
    add("battery", 200, 2000);
    add("bluetooth", 50, 500);
    add("cable", 300, 3000);
    add("charger", 150, 1500);
    add("cover", 80, 800);
    add("green", 400, 4000);
    add("leather", 60, 600);
    add("phone", 500, 5000);
    add("pink", 75, 750);
    add("screen", 250, 2500);
    add("tablet", 120, 1200);
    add("waterproof", 90, 900);
    add("wireless", 180, 1800);
    return words;
}

vespalib::string test_dir() {
    return "nlt_test_tmp";
}

} // anonymous namespace

class NltDictionaryTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::create_directories(test_dir().c_str());
    }
    void TearDown() override {
        std::filesystem::remove_all(test_dir().c_str());
    }
};

TEST_F(NltDictionaryTest, write_and_rand_read)
{
    auto words = makeTestWords();
    vespalib::string name = test_dir() + "/dict1";
    DummyFileHeaderContext ctx;

    // Write
    {
        NltDictionaryFileSeqWrite writer;
        TuneFileSeqWrite tune;
        ASSERT_TRUE(writer.open(name, tune, ctx));
        for (const auto &w : words) {
            writer.writeWord(w.word, w.counts);
        }
        ASSERT_TRUE(writer.close());
    }

    // Random read
    {
        NltDictionaryFileRandRead reader;
        TuneFileRandRead tune;
        ASSERT_TRUE(reader.open(name, tune));
        EXPECT_EQ(words.size(), reader.getNumWordIds());

        // Positive lookups
        for (size_t i = 0; i < words.size(); ++i) {
            uint64_t wordNum = 0;
            PostingListOffsetAndCounts oac;
            bool found = reader.lookup(words[i].word, wordNum, oac);
            EXPECT_TRUE(found) << "word: " << words[i].word;
            EXPECT_EQ(i + 1, wordNum); // 1-based
            EXPECT_EQ(words[i].expectedOffset, oac._offset);
            EXPECT_EQ(words[i].counts._numDocs, oac._counts._numDocs);
            EXPECT_EQ(words[i].counts._bitLength, oac._counts._bitLength);
        }

        // Negative lookups
        uint64_t wordNum = 0;
        PostingListOffsetAndCounts oac;
        EXPECT_FALSE(reader.lookup("nonexistent", wordNum, oac));
        EXPECT_FALSE(reader.lookup("zzz", wordNum, oac));
        EXPECT_FALSE(reader.lookup("", wordNum, oac));
        EXPECT_FALSE(reader.lookup("bat", wordNum, oac)); // prefix only

        ASSERT_TRUE(reader.close());
    }
}

TEST_F(NltDictionaryTest, write_and_seq_read)
{
    auto words = makeTestWords();
    vespalib::string name = test_dir() + "/dict2";
    DummyFileHeaderContext ctx;

    // Write
    {
        NltDictionaryFileSeqWrite writer;
        TuneFileSeqWrite tune;
        ASSERT_TRUE(writer.open(name, tune, ctx));
        for (const auto &w : words) {
            writer.writeWord(w.word, w.counts);
        }
        ASSERT_TRUE(writer.close());
    }

    // Sequential read
    {
        NltDictionaryFileSeqRead reader;
        TuneFileSeqRead tune;
        ASSERT_TRUE(reader.open(name, tune));

        for (size_t i = 0; i < words.size(); ++i) {
            vespalib::string word;
            uint64_t wordNum = 0;
            PostingListCounts counts;
            reader.readWord(word, wordNum, counts);

            EXPECT_EQ(words[i].word, word);
            EXPECT_EQ(i + 1, wordNum); // 1-based
            EXPECT_EQ(words[i].counts._numDocs, counts._numDocs);
            EXPECT_EQ(words[i].counts._bitLength, counts._bitLength);
        }

        // EOF
        vespalib::string word;
        uint64_t wordNum = 0;
        PostingListCounts counts;
        reader.readWord(word, wordNum, counts);
        EXPECT_EQ(DictionaryFileSeqRead::noWordNumHigh(), wordNum);

        ASSERT_TRUE(reader.close());
    }
}

TEST_F(NltDictionaryTest, segments_round_trip)
{
    vespalib::string name = test_dir() + "/dict3";
    DummyFileHeaderContext ctx;

    PostingListCounts counts;
    counts._numDocs = 1000000;
    counts._bitLength = 50000000;

    PostingListCounts::Segment seg1;
    seg1._bitLength = 25000000;
    seg1._numDocs = 500000;
    seg1._lastDoc = 499999;
    counts._segments.push_back(seg1);

    PostingListCounts::Segment seg2;
    seg2._bitLength = 25000000;
    seg2._numDocs = 500000;
    seg2._lastDoc = 999999;
    counts._segments.push_back(seg2);

    // Write
    {
        NltDictionaryFileSeqWrite writer;
        TuneFileSeqWrite tune;
        ASSERT_TRUE(writer.open(name, tune, ctx));
        PostingListCounts simple;
        simple._numDocs = 10;
        simple._bitLength = 100;
        writer.writeWord("aaa", simple);
        writer.writeWord("bbb", counts); // word with segments
        writer.writeWord("ccc", simple);
        ASSERT_TRUE(writer.close());
    }

    // Random read - verify segments survive round-trip
    {
        NltDictionaryFileRandRead reader;
        TuneFileRandRead tune;
        ASSERT_TRUE(reader.open(name, tune));

        uint64_t wordNum;
        PostingListOffsetAndCounts oac;

        ASSERT_TRUE(reader.lookup("bbb", wordNum, oac));
        EXPECT_EQ(counts._numDocs, oac._counts._numDocs);
        EXPECT_EQ(counts._bitLength, oac._counts._bitLength);
        ASSERT_EQ(2u, oac._counts._segments.size());
        EXPECT_EQ(seg1._bitLength, oac._counts._segments[0]._bitLength);
        EXPECT_EQ(seg1._numDocs, oac._counts._segments[0]._numDocs);
        EXPECT_EQ(seg1._lastDoc, oac._counts._segments[0]._lastDoc);
        EXPECT_EQ(seg2._bitLength, oac._counts._segments[1]._bitLength);
        EXPECT_EQ(seg2._numDocs, oac._counts._segments[1]._numDocs);
        EXPECT_EQ(seg2._lastDoc, oac._counts._segments[1]._lastDoc);

        ASSERT_TRUE(reader.close());
    }

    // Sequential read - verify segments survive round-trip
    {
        NltDictionaryFileSeqRead reader;
        TuneFileSeqRead tune;
        ASSERT_TRUE(reader.open(name, tune));

        vespalib::string word;
        uint64_t wordNum;
        PostingListCounts readCounts;

        reader.readWord(word, wordNum, readCounts); // aaa
        EXPECT_EQ("aaa", word);

        reader.readWord(word, wordNum, readCounts); // bbb
        EXPECT_EQ("bbb", word);
        ASSERT_EQ(2u, readCounts._segments.size());
        EXPECT_EQ(seg1._bitLength, readCounts._segments[0]._bitLength);
        EXPECT_EQ(seg1._numDocs, readCounts._segments[0]._numDocs);
        EXPECT_EQ(seg2._bitLength, readCounts._segments[1]._bitLength);

        ASSERT_TRUE(reader.close());
    }
}

TEST_F(NltDictionaryTest, large_dictionary)
{
    std::mt19937 rng(42);

    // Generate 10K sorted terms
    std::set<std::string> unique;
    while (unique.size() < 10000) {
        uint32_t len = 5 + (rng() % 20);
        std::string s;
        for (uint32_t i = 0; i < len; ++i) {
            s += 'a' + (rng() % 26);
        }
        unique.insert(s);
    }
    std::vector<std::string> terms(unique.begin(), unique.end());

    vespalib::string name = test_dir() + "/dict4";
    DummyFileHeaderContext ctx;

    // Write with random counts
    std::vector<PostingListCounts> allCounts(terms.size());
    {
        NltDictionaryFileSeqWrite writer;
        TuneFileSeqWrite tune;
        ASSERT_TRUE(writer.open(name, tune, ctx));
        for (size_t i = 0; i < terms.size(); ++i) {
            PostingListCounts c;
            c._numDocs = 1 + (rng() % 10000);
            c._bitLength = c._numDocs * (5 + rng() % 20);
            allCounts[i] = c;
            writer.writeWord(vespalib::string(terms[i]), c);
        }
        ASSERT_TRUE(writer.close());
    }

    // Random read - verify all terms
    {
        NltDictionaryFileRandRead reader;
        TuneFileRandRead tune;
        ASSERT_TRUE(reader.open(name, tune));
        EXPECT_EQ(terms.size(), reader.getNumWordIds());

        uint64_t checkOffset = 0;
        for (size_t i = 0; i < terms.size(); ++i) {
            uint64_t wordNum;
            PostingListOffsetAndCounts oac;
            bool found = reader.lookup(vespalib::string(terms[i]), wordNum, oac);
            EXPECT_TRUE(found) << "term: " << terms[i];
            EXPECT_EQ(i + 1, wordNum);
            EXPECT_EQ(checkOffset, oac._offset);
            EXPECT_EQ(allCounts[i]._numDocs, oac._counts._numDocs);
            checkOffset += allCounts[i]._bitLength;
        }

        ASSERT_TRUE(reader.close());
    }

    // Sequential read - verify order and content
    {
        NltDictionaryFileSeqRead reader;
        TuneFileSeqRead tune;
        ASSERT_TRUE(reader.open(name, tune));

        for (size_t i = 0; i < terms.size(); ++i) {
            vespalib::string word;
            uint64_t wordNum;
            PostingListCounts counts;
            reader.readWord(word, wordNum, counts);
            EXPECT_EQ(vespalib::string(terms[i]), word);
            EXPECT_EQ(i + 1, wordNum);
        }

        ASSERT_TRUE(reader.close());
    }
}

TEST_F(NltDictionaryTest, negative_lookups_never_crash)
{
    vespalib::string name = test_dir() + "/dict5";
    DummyFileHeaderContext ctx;

    // Write a small dictionary
    {
        NltDictionaryFileSeqWrite writer;
        TuneFileSeqWrite tune;
        ASSERT_TRUE(writer.open(name, tune, ctx));
        PostingListCounts c;
        c._numDocs = 10;
        c._bitLength = 100;
        writer.writeWord("hello", c);
        writer.writeWord("world", c);
        ASSERT_TRUE(writer.close());
    }

    // Many negative lookups
    {
        NltDictionaryFileRandRead reader;
        TuneFileRandRead tune;
        ASSERT_TRUE(reader.open(name, tune));

        std::mt19937 rng(123);
        for (int i = 0; i < 1000; ++i) {
            uint32_t len = rng() % 30;
            std::string s;
            for (uint32_t j = 0; j < len; ++j) {
                s += char(rng() % 256);
            }
            uint64_t wordNum = 999;
            PostingListOffsetAndCounts oac;
            reader.lookup(vespalib::string(s), wordNum, oac);
            // Just verify no crash
        }

        ASSERT_TRUE(reader.close());
    }
}

TEST_F(NltDictionaryTest, empty_dictionary)
{
    vespalib::string name = test_dir() + "/dict_empty";
    DummyFileHeaderContext ctx;

    // Write empty dictionary
    {
        NltDictionaryFileSeqWrite writer;
        TuneFileSeqWrite tune;
        ASSERT_TRUE(writer.open(name, tune, ctx));
        ASSERT_TRUE(writer.close());
    }

    // Random read on empty dictionary
    {
        NltDictionaryFileRandRead reader;
        TuneFileRandRead tune;
        ASSERT_TRUE(reader.open(name, tune));
        EXPECT_EQ(0u, reader.getNumWordIds());

        uint64_t wordNum = 999;
        PostingListOffsetAndCounts oac;
        EXPECT_FALSE(reader.lookup("anything", wordNum, oac));

        ASSERT_TRUE(reader.close());
    }

    // Sequential read on empty dictionary
    {
        NltDictionaryFileSeqRead reader;
        TuneFileSeqRead tune;
        ASSERT_TRUE(reader.open(name, tune));

        vespalib::string word;
        uint64_t wordNum = 0;
        PostingListCounts counts;
        reader.readWord(word, wordNum, counts);
        EXPECT_EQ(DictionaryFileSeqRead::noWordNumHigh(), wordNum);

        ASSERT_TRUE(reader.close());
    }
}

} // namespace search::diskindex

GTEST_MAIN_RUN_ALL_TESTS()
