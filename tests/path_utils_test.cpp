#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "codescope/path_utils.hpp"

namespace codescope {
namespace {

TEST(NormalizeRepoPath, AcceptsAndCleansRelativePaths) {
    EXPECT_EQ(normalize_repo_path("src/a.cpp"), "src/a.cpp");
    EXPECT_EQ(normalize_repo_path("./src//a.cpp"), "src/a.cpp");
    EXPECT_EQ(normalize_repo_path("src\\net\\a.cpp"), "src/net/a.cpp");
    EXPECT_EQ(normalize_repo_path("src/./a.cpp/"), "src/a.cpp");
}

TEST(NormalizeRepoPath, RejectsPathTraversal) {
    EXPECT_EQ(normalize_repo_path("../../etc/passwd"), std::nullopt);
    EXPECT_EQ(normalize_repo_path("src/../../etc/passwd"), std::nullopt);
    EXPECT_EQ(normalize_repo_path("src/.."), std::nullopt);
    EXPECT_EQ(normalize_repo_path("..\\..\\windows\\win.ini"), std::nullopt);
}

TEST(NormalizeRepoPath, RejectsAbsoluteAndOddPaths) {
    EXPECT_EQ(normalize_repo_path("/etc/passwd"), std::nullopt);
    EXPECT_EQ(normalize_repo_path("\\\\server\\share"), std::nullopt);
    EXPECT_EQ(normalize_repo_path("C:\\Windows\\win.ini"), std::nullopt);
    EXPECT_EQ(normalize_repo_path("c:relative"), std::nullopt);
    EXPECT_EQ(normalize_repo_path(""), std::nullopt);
    EXPECT_EQ(normalize_repo_path("."), std::nullopt);
    EXPECT_EQ(normalize_repo_path(std::string("a\0b", 3)), std::nullopt);
}

TEST(ExtensionOf, LowercaseWithDot) {
    EXPECT_EQ(extension_of("src/a.CPP"), ".cpp");
    EXPECT_EQ(extension_of("archive.tar.gz"), ".gz");
    EXPECT_EQ(extension_of(".gitignore"), "");
    EXPECT_EQ(extension_of("Makefile"), "");
    EXPECT_EQ(extension_of("dir.d/file"), "");
}

TEST(LanguageOf, CommonFiles) {
    EXPECT_EQ(language_of("include/x.hpp"), "cpp");
    EXPECT_EQ(language_of("main.c"), "c");
    EXPECT_EQ(language_of("tool.py"), "python");
    EXPECT_EQ(language_of("CMakeLists.txt"), "cmake");
    EXPECT_EQ(language_of("notes.txt"), "text");
    EXPECT_EQ(language_of("unknown.zzz"), "text");
}

}  // namespace
}  // namespace codescope
