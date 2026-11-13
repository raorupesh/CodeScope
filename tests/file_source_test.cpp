#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "codescope/file_source.hpp"
#include "codescope/ingest_pipeline.hpp"
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
    for (const auto& e : source.list()) {
        paths.push_back(e.path.generic_string());
    }
    EXPECT_EQ(paths, (Paths{"a/y/x.hpp", "a/z.cpp", "b.txt"}));
    EXPECT_EQ(source.read("a/z.cpp"), "zed");
    EXPECT_THROW(source.read("missing.txt"), std::runtime_error);
}

TEST(FilesystemSource, RejectsMissingRoot) {
    TempDir dir;
    EXPECT_THROW(FilesystemSource(dir.path() / "does-not-exist"), std::invalid_argument);
}

TEST(FilesystemSource, IncrementalReindexSeesEditsOnDisk) {
    TempDir dir;
    dir.write("src/a.cpp", "void old_name();\n");
    dir.write("src/b.cpp", "void other();\n");

    const FilesystemSource source(dir.path());
    const IngestPipeline pipeline(source);
    const auto first = pipeline.run(nullptr);
    EXPECT_EQ(result_paths(search(*first, "old_name")), Paths{"src/a.cpp"});

    // A different size guarantees change detection even on coarse mtime clocks.
    dir.write("src/a.cpp", "void brand_new_name();\n");
    fs::remove(dir.path() / "src/b.cpp");
    const auto second = pipeline.run(first.get());
    EXPECT_EQ(second->stats.indexed, 1u);
    EXPECT_EQ(second->stats.removed, 1u);
    EXPECT_TRUE(search(*second, "old_name").results.empty());
    EXPECT_EQ(result_paths(search(*second, "brand_new_name")), Paths{"src/a.cpp"});
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
