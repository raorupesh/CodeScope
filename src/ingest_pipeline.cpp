#include "codescope/ingest_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <optional>
#include <unordered_set>
#include <utility>

namespace codescope {

IngestPipeline::IngestPipeline(const IFileSource& source, IngestOptions options)
    : source_(source), options_(std::move(options)) {}

bool IngestPipeline::looks_binary(std::string_view content) {
    const std::size_t n = std::min<std::size_t>(content.size(), 8192);
    return n > 0 && std::memchr(content.data(), '\0', n) != nullptr;
}

bool IngestPipeline::is_ignored(const std::filesystem::path& relative_path) const {
    for (const auto& part : relative_path.parent_path()) {
        const std::string name = part.generic_string();
        if (std::find(options_.ignored_dirs.begin(), options_.ignored_dirs.end(), name) !=
            options_.ignored_dirs.end()) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<IndexSnapshot> IngestPipeline::run(const IndexSnapshot* previous) const {
    const auto started = std::chrono::steady_clock::now();

    auto snap = previous ? std::make_shared<IndexSnapshot>(*previous) : std::make_shared<IndexSnapshot>();
    snap->stats = IngestStats{};
    IngestStats& stats = snap->stats;
    DocStore& docs = snap->docs;
    TrigramIndex& index = snap->index;

    std::unordered_set<DocId> seen;  // live documents confirmed present on this run

    for (const IFileSource::Entry& entry : source_.list()) {
        ++stats.files_seen;
        const std::string path = entry.path.generic_string();
        std::optional<DocId> existing = docs.find(path);

        // Drops the previously indexed version of this path, if any.
        const auto drop_existing = [&](bool count_as_removed) {
            if (existing) {
                index.remove(*existing);
                docs.remove(*existing);
                existing.reset();
                if (count_as_removed) {
                    ++stats.removed;
                }
            }
        };

        if (is_ignored(entry.path)) {
            ++stats.skipped_ignored;
            drop_existing(true);
            continue;
        }
        if (entry.size > options_.max_file_bytes) {
            ++stats.skipped_too_large;
            drop_existing(true);
            continue;
        }

        std::optional<std::string> content;

        // Change detection: same mtime and size means the postings are reused.
        if (existing) {
            const Document* doc = docs.get(*existing);
            if (doc->mtime == entry.mtime && doc->size == entry.size) {
                if (doc->content) {
                    ++stats.unchanged;
                    seen.insert(*existing);
                    continue;
                }
                // Loaded from the on-disk index: postings are valid but the
                // content is not in memory yet. Read it without re-tokenizing.
                try {
                    content = source_.read(entry.path);
                } catch (const std::exception&) {
                    ++stats.read_errors;
                    drop_existing(true);
                    continue;
                }
                if (content->size() == doc->size) {
                    docs.set_content(*existing, std::make_shared<const std::string>(std::move(*content)));
                    ++stats.unchanged;
                    seen.insert(*existing);
                    continue;
                }
                // The file changed after it was listed: fall through and re-index it.
            }
        }

        if (!content) {
            try {
                content = source_.read(entry.path);
            } catch (const std::exception&) {
                ++stats.read_errors;
                drop_existing(true);
                continue;
            }
        }
        if (content->size() > options_.max_file_bytes) {
            ++stats.skipped_too_large;
            drop_existing(true);
            continue;
        }
        if (looks_binary(*content)) {
            ++stats.skipped_binary;
            drop_existing(true);
            continue;
        }

        drop_existing(false);  // changed file: replace the old version
        Document doc;
        doc.path = path;
        doc.mtime = entry.mtime;
        doc.size = content->size();
        doc.content = std::make_shared<const std::string>(std::move(*content));
        const std::shared_ptr<const std::string> text = doc.content;
        const DocId id = docs.add(std::move(doc));
        index.add(id, *text);
        seen.insert(id);
        ++stats.indexed;
    }

    // Anything still live that we did not see has been deleted from the source.
    for (const DocId id : docs.live_ids()) {
        if (!seen.contains(id)) {
            index.remove(id);
            docs.remove(id);
            ++stats.removed;
        }
    }
    index.compact();

    stats.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return snap;
}

}  // namespace codescope
