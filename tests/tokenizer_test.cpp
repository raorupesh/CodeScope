#include <gtest/gtest.h>

#include <algorithm>

#include "codescope/tokenizer.hpp"

namespace codescope {
namespace {

TEST(Tokenizer, PacksThreeBytesBigEndian) {
    EXPECT_EQ(pack_trigram('a', 'b', 'c'), 0x616263u);
    EXPECT_EQ(pack_trigram('\xff', '\x00', '\x01'), 0xff0001u);
}

TEST(Tokenizer, ShorterThanThreeHasNoTrigrams) {
    EXPECT_TRUE(extract_trigrams("").empty());
    EXPECT_TRUE(extract_trigrams("a").empty());
    EXPECT_TRUE(extract_trigrams("ab").empty());
}

TEST(Tokenizer, ExactlyThreeBytes) {
    const auto t = extract_trigrams("abc");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0], pack_trigram('a', 'b', 'c'));
}

TEST(Tokenizer, SlidingWindow) {
    const auto t = extract_trigrams("abcd");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0], pack_trigram('a', 'b', 'c'));
    EXPECT_EQ(t[1], pack_trigram('b', 'c', 'd'));
}

TEST(Tokenizer, DuplicatesRemovedAndSorted) {
    EXPECT_EQ(extract_trigrams("aaaaaa").size(), 1u);
    const auto t = extract_trigrams("zyx abc zyx abc");
    EXPECT_TRUE(std::is_sorted(t.begin(), t.end()));
    EXPECT_EQ(std::adjacent_find(t.begin(), t.end()), t.end());
}

TEST(Tokenizer, FoldsAsciiCase) {
    EXPECT_EQ(extract_trigrams("HttpRequest"), extract_trigrams("httprequest"));
    EXPECT_EQ(ascii_lower(std::string_view("MiXeD_09")), "mixed_09");
}

TEST(Tokenizer, NonAsciiBytesKeptVerbatim) {
    // "é" = C3 A9; "É" = C3 89. Only ASCII is folded, so these differ.
    const auto lower = extract_trigrams("\xC3\xA9t");
    const auto upper = extract_trigrams("\xC3\x89t");
    ASSERT_EQ(lower.size(), 1u);
    EXPECT_EQ(lower[0], pack_trigram('\xC3', '\xA9', 't'));
    EXPECT_NE(lower, upper);
}

}  // namespace
}  // namespace codescope
