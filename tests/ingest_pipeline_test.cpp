#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "codescope/ingest_pipeline.hpp"
#include "test_helpers.hpp"

namespace codescope {
namespace {

using test_util::MemoryFileSource;
using test_util::result_paths;
using test_util::search;
using Paths = std::vector<std::string>;

class IngestPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        repo.put("README.md", "CodeScope test repo\n");
        repo.put("src/main.cpp", "int main() { return run_server(); }\n");
        repo.put("src/server.cpp", "int run_server() { return 0; }\n");
        repo.put("node_modules/lib/index.js", "function run_server() {}\n");
        repo.put(".git/config", "[core] run_server\n");
        repo.put("assets/logo.png", std::string("\x89PNG\0\0run_server", 15));
        repo.put("data/huge.txt", std::string(200, 'x') + "run_server\n");
        options.max_file_bytes = 128;
    }

    MemoryFileSource repo;
    IngestOptions options;
};

TEST_F(IngestPipelineTest, FullBuildFiltersAndIndexes) {
    const IngestPipeline pipeline(repo, options);
    const auto snap = pipeline.run(nullptr);
    const IngestStats& s = snap->stats;

    EXPECT_EQ(s.files_seen, 7u);
    EXPECT_EQ(s.indexed, 3u);
    EXPECT_EQ(s.skipped_ignored, 2u);
    EXPECT_EQ(s.skipped_binary, 1u);
    EXPECT_EQ(s.skipped_too_large, 1u);
    EXPECT_EQ(s.read_errors, 0u);
    EXPECT_EQ(snap->docs.live_count(), 3u);

    EXPECT_EQ(result_paths(search(*snap, "run_server")), (Paths{"src/main.cpp", "src/server.cpp"}));
}

TEST_F(IngestPipelineTest, UnchangedFilesAreNotReread) {
    const IngestPipeline pipeline(repo, options);
    const auto first = pipeline.run(nullptr);
    const std::size_t reads_after_first = repo.reads();

    const auto second = pipeline.run(first.get());
    EXPECT_EQ(second->stats.indexed, 0u);
    EXPECT_EQ(second->stats.unchanged, 3u);
    EXPECT_EQ(second->stats.removed, 0u);
    EXPECT_EQ(repo.reads(), reads_after_first);
    EXPECT_EQ(result_paths(search(*second, "run_server")), (Paths{"src/main.cpp", "src/server.cpp"}));
}

TEST_F(IngestPipelineTest, ChangedMtimeIsReindexed) {
    const IngestPipeline pipeline(repo, options);
    const auto first = pipeline.run(nullptr);

    repo.put("src/server.cpp", "int start_server() { return 0; }\n", /*mtime=*/2);
    const auto second = pipeline.run(first.get());
    EXPECT_EQ(second->stats.indexed, 1u);
    EXPECT_EQ(second->stats.unchanged, 2u);
    EXPECT_EQ(result_paths(search(*second, "run_server")), Paths{"src/main.cpp"});
    EXPECT_EQ(result_paths(search(*second, "start_server")), Paths{"src/server.cpp"});
    EXPECT_EQ(second->docs.live_count(), 3u);
    EXPECT_EQ(second->index.removed_count(), 0u);  // compacted
}

TEST_F(IngestPipelineTest, ChangedSizeWithSameMtimeIsReindexed) {
    const IngestPipeline pipeline(repo, options);
    const auto first = pipeline.run(nullptr);

    repo.put("README.md", "CodeScope test repo, now longer\n", /*mtime=*/1);
    const auto second = pipeline.run(first.get());
    EXPECT_EQ(second->stats.indexed, 1u);
    EXPECT_EQ(result_paths(search(*second, "now longer")), Paths{"README.md"});
}

TEST_F(IngestPipelineTest, DeletedAndNewFiles) {
    const IngestPipeline pipeline(repo, options);
    const auto first = pipeline.run(nullptr);

    repo.erase("src/main.cpp");
    repo.put("src/extra.cpp", "void extra() { run_server(); }\n");
    const auto second = pipeline.run(first.get());
    EXPECT_EQ(second->stats.removed, 1u);
    EXPECT_EQ(second->stats.indexed, 1u);
    EXPECT_FALSE(second->docs.find("src/main.cpp").has_value());
    EXPECT_EQ(result_paths(search(*second, "run_server")), (Paths{"src/server.cpp", "src/extra.cpp"}));
}

TEST_F(IngestPipelineTest, FileThatBecomesBinaryIsRemoved) {
    const IngestPipeline pipeline(repo, options);
    const auto first = pipeline.run(nullptr);

    repo.put("src/server.cpp", std::string("run_server\0\0", 12), /*mtime=*/2);
    const auto second = pipeline.run(first.get());
    EXPECT_EQ(second->stats.skipped_binary, 2u);
    EXPECT_EQ(second->stats.removed, 1u);
    EXPECT_EQ(result_paths(search(*second, "run_server")), Paths{"src/main.cpp"});
}

TEST_F(IngestPipelineTest, PreviousSnapshotIsNotModified) {
    const IngestPipeline pipeline(repo, options);
    const auto first = pipeline.run(nullptr);

    repo.erase("src/server.cpp");
    const auto second = pipeline.run(first.get());

    // Readers still holding `first` see the old state.
    EXPECT_EQ(result_paths(search(*first, "run_server")), (Paths{"src/main.cpp", "src/server.cpp"}));
    EXPECT_EQ(result_paths(search(*second, "run_server")), Paths{"src/main.cpp"});
}

TEST_F(IngestPipelineTest, ReadErrorsAreCountedAndSkipped) {
    repo.make_unreadable("src/main.cpp");
    const IngestPipeline pipeline(repo, options);
    const auto snap = pipeline.run(nullptr);
    EXPECT_EQ(snap->stats.read_errors, 1u);
    EXPECT_EQ(result_paths(search(*snap, "run_server")), Paths{"src/server.cpp"});
}

TEST_F(IngestPipelineTest, IgnoredDirectoriesAreConfigurable) {
    options.ignored_dirs = {"src"};
    const IngestPipeline pipeline(repo, options);
    const auto snap = pipeline.run(nullptr);
    EXPECT_EQ(snap->stats.skipped_ignored, 2u);
    // node_modules and .git are no longer ignored.
    EXPECT_EQ(result_paths(search(*snap, "run_server")), (Paths{".git/config", "node_modules/lib/index.js"}));
}

TEST(IngestPipeline, BinaryDetectionLooksAtFirst8KB) {
    EXPECT_FALSE(IngestPipeline::looks_binary(""));
    EXPECT_FALSE(IngestPipeline::looks_binary("plain text"));
    EXPECT_TRUE(IngestPipeline::looks_binary(std::string("a\0b", 3)));
    EXPECT_FALSE(IngestPipeline::looks_binary(std::string(8192, 'a') + std::string(1, '\0')));
}

}  // namespace
}  // namespace codescope
