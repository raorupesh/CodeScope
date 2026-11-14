#pragma once

// Facade for the CLI and HTTP layer. Searches copy the current snapshot
// pointer and run lock-free; re-indexing builds a new snapshot and swaps it in.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "codescope/file_source.hpp"
#include "codescope/ingest_pipeline.hpp"
#include "codescope/snapshot.hpp"
#include "codescope/types.hpp"

namespace codescope {

struct ServiceConfig {
    std::optional<std::filesystem::path> index_file;  // persist the index here if set
    IngestOptions ingest;
};

struct FileView {
    std::string path;
    std::string language;
    std::vector<std::string> lines;
};

struct Definition {
    std::string path;
    std::uint32_t line = 0;
    std::string kind;
    std::string qualified;
    std::string text;  // the source line, trimmed
};

struct ServiceStats {
    std::size_t docs = 0;
    std::size_t trigrams = 0;
    std::size_t index_bytes = 0;  // approximate posting-list memory
    std::uint64_t content_bytes = 0;
    std::size_t symbols = 0;
    IngestStats last_run;
    bool reindexing = false;
    bool loaded_from_disk = false;
    std::uint64_t generation = 0;  // snapshots published
    std::string last_error;
};

class SearchService {
public:
    SearchService(std::unique_ptr<IFileSource> source, ServiceConfig config);
    ~SearchService();

    SearchService(const SearchService&) = delete;
    SearchService& operator=(const SearchService&) = delete;

    // Loads the saved index (if any), then brings it up to date.
    IngestStats initialize();

    IngestStats reindex();

    // False if a background re-index is already running.
    bool reindex_async();

    bool reindexing() const { return reindexing_.load(); }

    SearchResponse search(const SearchOptions& options) const;

    // Served from memory; std::nullopt for invalid or unindexed paths.
    std::optional<FileView> file(std::string_view path) const;

    // Types first, then functions; at most `limit`.
    std::vector<Definition> definitions(std::string_view symbol, std::size_t limit = 50) const;

    ServiceStats stats() const;

    std::shared_ptr<const IndexSnapshot> snapshot() const;

private:
    void publish(std::shared_ptr<const IndexSnapshot> next);
    void set_error(std::string message);

    std::unique_ptr<IFileSource> source_;
    ServiceConfig config_;

    mutable std::mutex snapshot_mu_;  // guards snapshot_, last_error_, loaded_from_disk_
    std::shared_ptr<const IndexSnapshot> snapshot_;
    std::string last_error_;
    bool loaded_from_disk_ = false;

    std::mutex reindex_mu_;  // one re-index at a time
    std::mutex worker_mu_;   // guards worker_
    std::thread worker_;
    std::atomic<bool> reindexing_{false};
    std::atomic<std::uint64_t> generation_{0};
};

}  // namespace codescope
