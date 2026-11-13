#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "codescope/search_service.hpp"
#include "test_helpers.hpp"

namespace codescope {
namespace {

using test_util::MemoryFileSource;
using test_util::result_paths;
using test_util::TempDir;
using Paths = std::vector<std::string>;

SearchOptions query(const std::string& q) {
    SearchOptions o;
    o.query = q;
    return o;
}

// Owns a MemoryFileSource through the service while keeping a pointer to edit it.
struct ServiceUnderTest {
    explicit ServiceUnderTest(ServiceConfig config = {}) {
        auto source = std::make_unique<MemoryFileSource>();
        repo = source.get();
        repo->put("src/a.cpp", "void alpha();\nvoid beta();\n");
        repo->put("src/b.cpp", "void beta() { alpha(); }\n");
        service = std::make_unique<SearchService>(std::move(source), std::move(config));
    }
    MemoryFileSource* repo = nullptr;
    std::unique_ptr<SearchService> service;
};

bool wait_for_reindex(const SearchService& service) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (service.reindexing()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

TEST(SearchService, InitializeIndexesAndSearches) {
    ServiceUnderTest t;
    const IngestStats s = t.service->initialize();
    EXPECT_EQ(s.indexed, 2u);
    EXPECT_EQ(result_paths(t.service->search(query("alpha"))), (Paths{"src/a.cpp", "src/b.cpp"}));

    const ServiceStats st = t.service->stats();
    EXPECT_EQ(st.docs, 2u);
    EXPECT_GT(st.trigrams, 0u);
    EXPECT_EQ(st.generation, 1u);
    EXPECT_FALSE(st.loaded_from_disk);
}

TEST(SearchService, FileReturnsLines) {
    ServiceUnderTest t;
    t.service->initialize();
    const auto view = t.service->file("src/a.cpp");
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->language, "cpp");
    EXPECT_EQ(view->lines, (Paths{"void alpha();", "void beta();"}));
    EXPECT_TRUE(t.service->file("./src//a.cpp").has_value());  // normalized
}

TEST(SearchService, FileRejectsTraversalAndUnknownPaths) {
    ServiceUnderTest t;
    t.service->initialize();
    EXPECT_FALSE(t.service->file("../../etc/passwd").has_value());
    EXPECT_FALSE(t.service->file("/etc/passwd").has_value());
    EXPECT_FALSE(t.service->file("src/../../src/a.cpp").has_value());
    EXPECT_FALSE(t.service->file("src/missing.cpp").has_value());
}

TEST(SearchService, ReindexPicksUpChangesAndOldSnapshotsStayValid) {
    ServiceUnderTest t;
    t.service->initialize();
    const auto old_snapshot = t.service->snapshot();

    t.repo->put("src/c.cpp", "void gamma() { alpha(); }\n");
    t.repo->erase("src/b.cpp");
    const IngestStats s = t.service->reindex();
    EXPECT_EQ(s.indexed, 1u);
    EXPECT_EQ(s.removed, 1u);
    EXPECT_EQ(result_paths(t.service->search(query("alpha"))), (Paths{"src/a.cpp", "src/c.cpp"}));

    // A reader that grabbed the old snapshot still sees consistent old data.
    EXPECT_EQ(result_paths(test_util::search(*old_snapshot, "alpha")), (Paths{"src/a.cpp", "src/b.cpp"}));
}

TEST(SearchService, BackgroundReindexWhileSearching) {
    ServiceUnderTest t;
    t.service->initialize();

    std::atomic<bool> stop{false};
    std::atomic<int> searches{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const auto r = t.service->search(query("alpha"));
                EXPECT_FALSE(r.results.empty());
                ++searches;
            }
        });
    }

    for (int round = 0; round < 20; ++round) {
        t.repo->put("src/gen.cpp", "void alpha_" + std::to_string(round) + "();\n", round + 2);
        ASSERT_TRUE(t.service->reindex_async() || t.service->reindexing());
        ASSERT_TRUE(wait_for_reindex(*t.service));
    }
    stop = true;
    for (auto& th : readers) {
        th.join();
    }

    EXPECT_GT(searches.load(), 0);
    EXPECT_EQ(result_paths(t.service->search(query("alpha_19"))), Paths{"src/gen.cpp"});
}

TEST(SearchService, PersistsAndReloadsTheIndex) {
    TempDir dir;
    ServiceConfig config;
    config.index_file = dir.path() / "index.bin";

    {
        ServiceUnderTest t(config);
        t.service->initialize();
        EXPECT_TRUE(std::filesystem::exists(*config.index_file));
    }

    ServiceUnderTest t(config);
    const IngestStats s = t.service->initialize();
    EXPECT_TRUE(t.service->stats().loaded_from_disk);
    EXPECT_EQ(s.indexed, 0u);  // nothing re-tokenized
    EXPECT_EQ(s.unchanged, 2u);
    EXPECT_EQ(result_paths(t.service->search(query("beta"))), (Paths{"src/a.cpp", "src/b.cpp"}));
}

TEST(SearchService, CorruptIndexFileFallsBackToRebuild) {
    TempDir dir;
    ServiceConfig config;
    config.index_file = dir.path() / "index.bin";
    {
        std::ofstream out(*config.index_file, std::ios::binary);
        out << "definitely not an index";
    }

    ServiceUnderTest t(config);
    const IngestStats s = t.service->initialize();
    EXPECT_EQ(s.indexed, 2u);
    EXPECT_FALSE(t.service->stats().loaded_from_disk);
    EXPECT_FALSE(t.service->stats().last_error.empty());
    EXPECT_EQ(result_paths(t.service->search(query("alpha"))), (Paths{"src/a.cpp", "src/b.cpp"}));
}

}  // namespace
}  // namespace codescope
