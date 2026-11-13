// M1 smoke tests: prove the build, the test runner and CI are wired up.

#include <gtest/gtest.h>

#include <type_traits>

#include "codescope/types.hpp"
#include "codescope/version.hpp"

namespace codescope {
namespace {

TEST(Smoke, VersionIsSet) {
    EXPECT_FALSE(version().empty());
}

TEST(Smoke, IdTypesAreFourBytes) {
    static_assert(sizeof(DocId) == 4);
    static_assert(sizeof(Trigram) == 4);
    static_assert(std::is_unsigned_v<Trigram>);
}

TEST(Smoke, SearchOptionsDefaults) {
    const SearchOptions opts;
    EXPECT_FALSE(opts.case_sensitive);
    EXPECT_EQ(opts.max_results, 200u);
    EXPECT_EQ(opts.context_lines, 1u);
    EXPECT_TRUE(opts.extensions.empty());
    EXPECT_TRUE(opts.path_prefix.empty());
}

}  // namespace
}  // namespace codescope
