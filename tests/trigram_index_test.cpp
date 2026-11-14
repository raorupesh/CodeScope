#include <gtest/gtest.h>

#include <vector>

#include "codescope/trigram_index.hpp"

namespace codescope {
namespace {

using Ids = std::vector<DocId>;

class TrigramIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        index.add(0, "hello world");
        index.add(1, "help me");
        index.add(2, "world peace");
    }
    TrigramIndex index;
};

TEST_F(TrigramIndexTest, IntersectsPostingLists) {
    EXPECT_EQ(index.candidates("hel"), Ids({0, 1}));
    EXPECT_EQ(index.candidates("world"), Ids({0, 2}));
    EXPECT_EQ(index.candidates("hello"), Ids({0}));
}

TEST_F(TrigramIndexTest, CaseInsensitive) {
    EXPECT_EQ(index.candidates("HELLO"), Ids({0}));
}

TEST_F(TrigramIndexTest, UnknownTrigramGivesEmptyResult) {
    const auto c = index.candidates("xyz");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(c->empty());
}

TEST_F(TrigramIndexTest, ShortQueryCannotUseIndex) {
    EXPECT_FALSE(index.candidates("he").has_value());
    EXPECT_FALSE(index.candidates("").has_value());
}

TEST_F(TrigramIndexTest, RemoveHidesDocumentImmediately) {
    index.remove(0);
    EXPECT_TRUE(index.is_removed(0));
    EXPECT_EQ(index.candidates("hel"), Ids({1}));
    EXPECT_EQ(index.candidates("world"), Ids({2}));
}

TEST_F(TrigramIndexTest, CompactPurgesRemovedIds) {
    const std::size_t before = index.trigram_count();
    index.remove(1);  // "help me": "lp ", "p m", " me" exist only here
    index.compact();
    EXPECT_EQ(index.removed_count(), 0u);
    EXPECT_LT(index.trigram_count(), before);
    EXPECT_EQ(index.candidates("hel"), Ids({0}));
    for (const auto& [trigram, list] : index.postings()) {
        EXPECT_FALSE(list.empty());
        for (const DocId d : list.decode()) {
            EXPECT_NE(d, 1u);
        }
    }
}

TEST_F(TrigramIndexTest, CandidatesAreASupersetOfMatches) {
    // Contains "abc", "bcd" and "cde" but not the string "abcde".
    index.add(3, "abcd xcde");
    EXPECT_EQ(index.candidates("abcde"), Ids({3}));
}

TEST(TrigramIndex, OutOfOrderAndRepeatedAddsStaySortedUnique) {
    TrigramIndex index;
    index.add(5, "needle");
    index.add(2, "needle");
    index.add(5, "needle");
    EXPECT_EQ(index.candidates("needle"), Ids({2, 5}));
}

TEST(PostingList, RoundTripsAndStaysSmall) {
    // Gaps around the varint byte boundaries (128, 16384).
    const Ids ids{0, 1, 2, 129, 130, 16514, 16515, 4000000000u, 4294967295u};
    const PostingList list = PostingList::from_ids(ids);
    EXPECT_EQ(list.size(), ids.size());
    EXPECT_EQ(list.back(), 4294967295u);
    EXPECT_EQ(list.decode(), ids);

    Ids dense(1000);
    for (DocId i = 0; i < 1000; ++i) {
        dense[i] = i;
    }
    EXPECT_EQ(PostingList::from_ids(dense).bytes().size(), 1000u);  // 1 byte per id instead of 4
}

TEST(PostingList, FromBytesValidates) {
    const PostingList good = PostingList::from_ids({3, 7, 300});
    const auto copy = PostingList::from_bytes(good.bytes(), 3, 1000);
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ(*copy, good);

    EXPECT_FALSE(PostingList::from_bytes(good.bytes(), 2, 1000).has_value());    // wrong count
    EXPECT_FALSE(PostingList::from_bytes(good.bytes(), 3, 300).has_value());     // id out of range
    EXPECT_FALSE(PostingList::from_bytes({0x80}, 1, 10).has_value());            // truncated varint
    EXPECT_FALSE(PostingList::from_bytes({1, 0}, 2, 10).has_value());            // zero gap = duplicate
    EXPECT_FALSE(PostingList::from_bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, 1, ~0ull).has_value());  // too long
    EXPECT_FALSE(PostingList::from_bytes({}, 0, 10).has_value());                // empty list
}

TEST(TrigramIndex, EmptyIndex) {
    const TrigramIndex index;
    EXPECT_EQ(index.trigram_count(), 0u);
    EXPECT_EQ(index.candidates("abc"), Ids{});
}

}  // namespace
}  // namespace codescope
