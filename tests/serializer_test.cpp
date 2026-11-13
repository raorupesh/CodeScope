#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "codescope/ingest_pipeline.hpp"
#include "codescope/serializer.hpp"
#include "test_helpers.hpp"

namespace codescope {
namespace {

namespace fs = std::filesystem;
using test_util::MemoryFileSource;
using test_util::result_paths;
using test_util::search;
using test_util::TempDir;
using Paths = std::vector<std::string>;

class SerializerTest : public ::testing::Test {
protected:
    void SetUp() override {
        repo.put("a.cpp", "int alpha() { return 1; }\n", 10);
        repo.put("b.cpp", "int beta() { return alpha(); }\n", 20);
        repo.put("c/d.hpp", "int gamma();\n", 30);
        file = dir.path() / "index.bin";
    }

    std::string read_file() const {
        std::ifstream in(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    void write_file(const std::string& bytes) const {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
    }

    MemoryFileSource repo;
    TempDir dir;
    fs::path file;
};

TEST_F(SerializerTest, SaveThenLoadGivesTheSameIndex) {
    const auto original = IngestPipeline(repo).run(nullptr);
    IndexSerializer::save(*original, file);
    EXPECT_FALSE(fs::exists(dir.path() / "index.tmp"));  // temp file renamed away

    const auto loaded = IndexSerializer::load(file);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->docs.live_count(), original->docs.live_count());
    for (const DocId id : original->docs.live_ids()) {
        const Document* a = original->docs.get(id);
        const Document* b = loaded->docs.get(id);
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(a->path, b->path);
        EXPECT_EQ(a->mtime, b->mtime);
        EXPECT_EQ(a->size, b->size);
        EXPECT_EQ(b->content, nullptr);  // contents are not stored on disk
    }
    EXPECT_EQ(loaded->index.postings(), original->index.postings());
}

TEST_F(SerializerTest, LoadedIndexIsCompletedWithoutRetokenizing) {
    IndexSerializer::save(*IngestPipeline(repo).run(nullptr), file);
    const auto loaded = IndexSerializer::load(file);
    ASSERT_NE(loaded, nullptr);

    const auto ready = IngestPipeline(repo).run(loaded.get());
    EXPECT_EQ(ready->stats.indexed, 0u);
    EXPECT_EQ(ready->stats.unchanged, 3u);
    EXPECT_EQ(result_paths(search(*ready, "alpha")), (Paths{"a.cpp", "b.cpp"}));
}

TEST_F(SerializerTest, RemovedDocumentsAreDroppedAndIdsRenumbered) {
    const auto first = IngestPipeline(repo).run(nullptr);
    repo.erase("a.cpp");
    const auto second = IngestPipeline(repo).run(first.get());
    ASSERT_EQ(second->docs.slot_count(), 3u);  // tombstone still occupies id 0
    IndexSerializer::save(*second, file);

    const auto loaded = IndexSerializer::load(file);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->docs.slot_count(), 2u);
    const auto ready = IngestPipeline(repo).run(loaded.get());
    EXPECT_EQ(ready->stats.indexed, 0u);
    EXPECT_EQ(result_paths(search(*ready, "alpha")), Paths{"b.cpp"});
    EXPECT_EQ(result_paths(search(*ready, "gamma")), Paths{"c/d.hpp"});
}

TEST_F(SerializerTest, CorruptMagicIsRejected) {
    IndexSerializer::save(*IngestPipeline(repo).run(nullptr), file);
    std::string bytes = read_file();
    bytes[0] = 'X';
    write_file(bytes);
    std::string error;
    EXPECT_EQ(IndexSerializer::load(file, &error), nullptr);
    EXPECT_EQ(error, "bad magic");
}

TEST_F(SerializerTest, OtherVersionIsRejected) {
    IndexSerializer::save(*IngestPipeline(repo).run(nullptr), file);
    std::string bytes = read_file();
    bytes[4] = static_cast<char>(IndexSerializer::kVersion + 1);
    write_file(bytes);
    EXPECT_EQ(IndexSerializer::load(file), nullptr);
}

TEST_F(SerializerTest, TruncatedFileIsRejected) {
    IndexSerializer::save(*IngestPipeline(repo).run(nullptr), file);
    const std::string bytes = read_file();
    for (const std::size_t keep : {std::size_t{0}, std::size_t{3}, std::size_t{12}, bytes.size() / 2, bytes.size() - 1}) {
        write_file(bytes.substr(0, keep));
        EXPECT_EQ(IndexSerializer::load(file), nullptr) << "kept " << keep << " bytes";
    }
}

TEST_F(SerializerTest, TrailingGarbageIsRejected) {
    IndexSerializer::save(*IngestPipeline(repo).run(nullptr), file);
    write_file(read_file() + "junk");
    EXPECT_EQ(IndexSerializer::load(file), nullptr);
}

TEST_F(SerializerTest, MissingFileIsRejected) {
    EXPECT_EQ(IndexSerializer::load(dir.path() / "nope.bin"), nullptr);
}

TEST_F(SerializerTest, SaveOverwritesExistingIndex) {
    IndexSerializer::save(*IngestPipeline(repo).run(nullptr), file);
    repo.put("e.cpp", "int delta();\n", 40);
    IndexSerializer::save(*IngestPipeline(repo).run(nullptr), file);
    const auto loaded = IndexSerializer::load(file);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->docs.live_count(), 4u);
}

}  // namespace
}  // namespace codescope
