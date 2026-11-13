#include "codescope/search_service.hpp"

#include <exception>
#include <utility>

#include "codescope/path_utils.hpp"
#include "codescope/query_engine.hpp"
#include "codescope/serializer.hpp"

namespace codescope {

SearchService::SearchService(std::unique_ptr<IFileSource> source, ServiceConfig config)
    : source_(std::move(source)), config_(std::move(config)), snapshot_(std::make_shared<const IndexSnapshot>()) {}

SearchService::~SearchService() {
    const std::lock_guard lock(worker_mu_);
    if (worker_.joinable()) {
        worker_.join();
    }
}

IngestStats SearchService::initialize() {
    if (config_.index_file && std::filesystem::exists(*config_.index_file)) {
        std::string error;
        if (auto loaded = IndexSerializer::load(*config_.index_file, &error)) {
            publish(std::move(loaded));
            const std::lock_guard lock(snapshot_mu_);
            loaded_from_disk_ = true;
        } else {
            set_error("ignored saved index (" + error + "); rebuilding");
        }
    }
    return reindex();
}

IngestStats SearchService::reindex() {
    const std::lock_guard lock(reindex_mu_);
    const std::shared_ptr<const IndexSnapshot> previous = snapshot();
    const IngestPipeline pipeline(*source_, config_.ingest);
    std::shared_ptr<IndexSnapshot> next = pipeline.run(previous.get());
    const IngestStats run_stats = next->stats;

    const bool changed = run_stats.indexed > 0 || run_stats.removed > 0;
    publish(std::move(next));

    if (config_.index_file && (changed || !std::filesystem::exists(*config_.index_file))) {
        try {
            IndexSerializer::save(*snapshot(), *config_.index_file);
        } catch (const std::exception& e) {
            set_error(std::string("saving index failed: ") + e.what());
        }
    }
    return run_stats;
}

bool SearchService::reindex_async() {
    const std::lock_guard lock(worker_mu_);
    if (reindexing_.exchange(true)) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();  // previous run has finished; reclaim the thread
    }
    worker_ = std::thread([this] {
        try {
            reindex();
        } catch (const std::exception& e) {
            set_error(std::string("re-index failed: ") + e.what());
        }
        reindexing_.store(false);
    });
    return true;
}

SearchResponse SearchService::search(const SearchOptions& options) const {
    const std::shared_ptr<const IndexSnapshot> snap = snapshot();  // keeps the snapshot alive
    return QueryEngine(snap->index, snap->docs).search(options);
}

std::optional<FileView> SearchService::file(std::string_view path) const {
    const std::optional<std::string> normalized = normalize_repo_path(path);
    if (!normalized) {
        return std::nullopt;
    }
    const std::shared_ptr<const IndexSnapshot> snap = snapshot();
    const std::optional<DocId> id = snap->docs.find(*normalized);
    if (!id) {
        return std::nullopt;
    }
    const Document* doc = snap->docs.get(*id);
    if (doc == nullptr || !doc->content) {
        return std::nullopt;
    }

    FileView view;
    view.path = doc->path;
    view.language = language_of(doc->path);
    const std::string_view text = *doc->content;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        view.lines.emplace_back(line);
        start = end + 1;
    }
    return view;
}

ServiceStats SearchService::stats() const {
    ServiceStats out;
    const std::shared_ptr<const IndexSnapshot> snap = snapshot();
    out.docs = snap->docs.live_count();
    out.trigrams = snap->index.trigram_count();
    out.index_bytes = snap->index.memory_bytes();
    out.content_bytes = snap->docs.content_bytes();
    out.last_run = snap->stats;
    out.reindexing = reindexing_.load();
    out.generation = generation_.load();
    {
        const std::lock_guard lock(snapshot_mu_);
        out.last_error = last_error_;
        out.loaded_from_disk = loaded_from_disk_;
    }
    return out;
}

std::shared_ptr<const IndexSnapshot> SearchService::snapshot() const {
    const std::lock_guard lock(snapshot_mu_);
    return snapshot_;
}

void SearchService::publish(std::shared_ptr<const IndexSnapshot> next) {
    std::shared_ptr<const IndexSnapshot> old;
    {
        const std::lock_guard lock(snapshot_mu_);
        old = std::exchange(snapshot_, std::move(next));
    }
    ++generation_;
    // `old` is released here, outside the lock: freeing a large index can take a while.
}

void SearchService::set_error(std::string message) {
    const std::lock_guard lock(snapshot_mu_);
    last_error_ = std::move(message);
}

}  // namespace codescope
