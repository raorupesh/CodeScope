#pragma once

// Facade used by the CLI and the HTTP layer.
//
// Concurrency model: searches take a shared_ptr copy of the current snapshot
// (under a mutex held only for the pointer copy) and run without locks.
// Re-indexing builds a new snapshot off to the side and swaps the pointer.
// A search never sees a half-built index, and the snapshot it holds stays
// alive (so its string_views stay valid) until the search finishes.

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

struct ServiceStats {
    std::size_t docs = 0;
    std::size_t trigrams = 0;
    std::size_t index_bytes = 0;    // approximate posting-list memory
    std::uint64_t content_bytes = 0;
    IngestStats last_run;           // stats of the most recent (re)index
    bool reindexing = false;
    bool loaded_from_disk = false;  // startup reused a saved index
    std::uint64_t generation = 0;   // number of snapshots published
    std::string last_error;
};

class SearchService {
public:
    SearchService(std::unique_ptr<IFileSource> source, ServiceConfig config);
    ~SearchService();

    SearchService(const SearchService&) = delete;
    SearchService& operator=(const SearchService&) = delete;

    // Loads the saved index if one is configured and valid, then runs an
    // incremental pass so the result matches the files on disk.
    IngestStats initialize();

    // Synchronous incremental re-index: build, swap, save.
    IngestStats reindex();

    // Runs reindex() on a background thread. Returns false (and does
    // nothing) if a background re-index is already running.
    bool reindex_async();

    bool reindexing() const { return reindexing_.load(); }

    SearchResponse search(const SearchOptions& options) const;

    // `path` is normalized and must name an indexed file; std::nullopt
    // otherwise. Content comes from the in-memory snapshot, never from a
    // client-controlled filesystem path.
    std::optional<FileView> file(std::string_view path) const;

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
