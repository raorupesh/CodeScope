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
        for (const DocId d : list) {
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

TEST(TrigramIndex, EmptyIndex) {
    const TrigramIndex index;
    EXPECT_EQ(index.trigram_count(), 0u);
    EXPECT_EQ(index.candidates("abc"), Ids{});
}

}  // namespace
}  // namespace codescope
