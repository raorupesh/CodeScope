#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "codescope/file_source.hpp"
#include "codescope/ingest_pipeline.hpp"
#include "codescope/path_utils.hpp"
#include "test_helpers.hpp"

namespace codescope {
namespace {

namespace fs = std::filesystem;
using test_util::result_paths;
using test_util::search;
using test_util::TempDir;
using Paths = std::vector<std::string>;

TEST(FilesystemSource, ListsRegularFilesSortedAndRelative) {
    TempDir dir;
    dir.write("b.txt", "bee");
    dir.write("a/z.cpp", "zed");
    dir.write("a/y/x.hpp", "ex");

    const FilesystemSource source(dir.path());
    Paths paths;
    for (const auto& e : source.list({}).files) {
        paths.push_back(e.path.generic_string());
    }
    EXPECT_EQ(paths, (Paths{"a/y/x.hpp", "a/z.cpp", "b.txt"}));
    EXPECT_EQ(source.read("a/z.cpp"), "zed");
    EXPECT_THROW(source.read("missing.txt"), std::runtime_error);
}

TEST(FilesystemSource, PrunesIgnoredDirectoriesWithoutWalkingThem) {
    TempDir dir;
    dir.write("src/a.cpp", "a");
    dir.write("build/_deps/x/y.cpp", "y");
    dir.write("node_modules/pkg/index.js", "z");
    dir.write("src/node_modules/inner.js", "w");  // nested ignored dir

    const FilesystemSource source(dir.path());
    const auto listing = source.list({"build", "node_modules"});
    ASSERT_EQ(listing.files.size(), 1u);
    EXPECT_EQ(listing.files[0].path.generic_string(), "src/a.cpp");
    EXPECT_EQ(listing.skipped_dirs, 3u);

    EXPECT_EQ(source.list({}).files.size(), 4u);
}

TEST(FilesystemSource, RejectsMissingRoot) {
    TempDir dir;
    EXPECT_THROW(FilesystemSource(dir.path() / "does-not-exist"), std::invalid_argument);
}

TEST(FilesystemSource, RefusesFilesSwappedForSymlinks) {
    TempDir outside;
    outside.write("secret/key.txt", "top secret");
    TempDir dir;
    dir.write("a.txt", "fine");
    dir.write("sub/b.txt", "fine");
    const FilesystemSource source(dir.path());
    EXPECT_EQ(source.read("a.txt"), "fine");

    // Swap a file and a directory for symlinks after listing.
    fs::remove(dir.path() / "a.txt");
    fs::remove_all(dir.path() / "sub");
    std::error_code ec;
    fs::create_symlink(outside.path() / "secret/key.txt", dir.path() / "a.txt", ec);
    if (ec) {
        GTEST_SKIP() << "symlinks not permitted here: " << ec.message();
    }
    fs::create_directory_symlink(outside.path() / "secret", dir.path() / "sub", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_THROW(source.read("a.txt"), std::runtime_error);
    EXPECT_THROW(source.read("sub/key.txt"), std::runtime_error);
}

TEST(FilesystemSource, IncrementalReindexSeesEditsOnDisk) {
    TempDir dir;
    dir.write("src/a.cpp", "void old_name();\n");
    dir.write("src/b.cpp", "void other();\n");

    const FilesystemSource source(dir.path());
    const IngestPipeline pipeline(source);
    const auto first = pipeline.run(nullptr);
    EXPECT_EQ(result_paths(search(*first, "old_name")), Paths{"src/a.cpp"});

    // A size change is detected even with coarse mtime clocks.
    dir.write("src/a.cpp", "void brand_new_name();\n");
    fs::remove(dir.path() / "src/b.cpp");
    const auto second = pipeline.run(first.get());
    EXPECT_EQ(second->stats.indexed, 1u);
    EXPECT_EQ(second->stats.removed, 1u);
    EXPECT_TRUE(search(*second, "old_name").results.empty());
    EXPECT_EQ(result_paths(search(*second, "brand_new_name")), Paths{"src/a.cpp"});
}

TEST(FilesystemSource, HandlesNonAsciiFileNames) {
    // Regression: path::string() throws on Windows for non-ANSI names.
    TempDir dir;
    const fs::path name = fs::path(u8"日本語Dir") / fs::path(u8"ファイル.txt");
    fs::create_directories(dir.path() / name.parent_path());
    {
        std::ofstream out(dir.path() / name, std::ios::binary);
        out << "needle_unicode\n";
    }

    const FilesystemSource source(dir.path());
    const auto snap = IngestPipeline(source).run(nullptr);
    EXPECT_EQ(result_paths(search(*snap, "needle_unicode")), Paths{to_utf8(name)});
}

TEST(FilesystemSource, IndexesTheFixtureRepository) {
    const FilesystemSource source(fs::path(CODESCOPE_FIXTURES_DIR) / "sample_repo");
    const auto snap = IngestPipeline(source).run(nullptr);
    EXPECT_EQ(snap->stats.skipped_ignored, 1u);  // third_party/vendored.hpp
    EXPECT_EQ(result_paths(search(*snap, "HttpRequest")),
              (Paths{"README.md", "include/net/http_request.hpp", "src/net/http_request.cpp"}));
    EXPECT_EQ(result_paths(search(*snap, "HttpRequest", /*case_sensitive=*/true)),
              (Paths{"README.md", "include/net/http_request.hpp", "src/net/http_request.cpp"}));
    EXPECT_EQ(result_paths(search(*snap, "ttpReq")).size(), 3u);  // substring inside an identifier
}

}  // namespace
}  // namespace codescope
