#include "codescope/ingest_pipeline.hpp"

#include "codescope/path_utils.hpp"

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

namespace {

std::shared_ptr<const std::vector<Symbol>> symbols_of(const std::string& path, const std::string& content) {
    if (!is_cpp_path(path)) {
        return nullptr;
    }
    return std::make_shared<const std::vector<Symbol>>(extract_symbols(path, content));
}

}  // namespace

std::shared_ptr<IndexSnapshot> IngestPipeline::run(const IndexSnapshot* previous) const {
    const auto started = std::chrono::steady_clock::now();

    auto snap = previous ? std::make_shared<IndexSnapshot>(*previous) : std::make_shared<IndexSnapshot>();
    snap->stats = IngestStats{};
    IngestStats& stats = snap->stats;
    DocStore& docs = snap->docs;
    TrigramIndex& index = snap->index;

    std::unordered_set<DocId> seen;  // live docs still present

    const std::unordered_map<std::string, FileStamp> previous_binaries = std::move(snap->binary_files);
    snap->binary_files.clear();

    const IFileSource::Listing listing = source_.list(options_.ignored_dirs);
    stats.skipped_ignored = listing.skipped_dirs;

    for (const IFileSource::Entry& entry : listing.files) {
        ++stats.files_seen;
        const std::string path = to_utf8(entry.path);
        std::optional<DocId> existing = docs.find(path);

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

        if (entry.size > options_.max_file_bytes) {
            ++stats.skipped_too_large;
            drop_existing(true);
            continue;
        }

        const FileStamp stamp{entry.mtime, static_cast<std::uint64_t>(entry.size)};
        if (!existing) {
            const auto bin = previous_binaries.find(path);
            if (bin != previous_binaries.end() && bin->second.mtime == stamp.mtime &&
                bin->second.size == stamp.size) {
                ++stats.skipped_binary;  // unchanged binary: skip without reading
                snap->binary_files.emplace(path, stamp);
                continue;
            }
        }

        std::optional<std::string> content;

        // Same mtime and size: reuse the postings.
        if (existing) {
            const Document* doc = docs.get(*existing);
            if (doc->mtime == entry.mtime && doc->size == entry.size) {
                if (doc->content) {
                    ++stats.unchanged;
                    seen.insert(*existing);
                    continue;
                }
                // Loaded from disk: read the content without re-tokenizing.
                try {
                    content = source_.read(entry.path);
                } catch (const std::exception&) {
                    ++stats.read_errors;
                    drop_existing(true);
                    continue;
                }
                if (content->size() == doc->size) {
                    docs.set_symbols(*existing, symbols_of(path, *content));
                    docs.set_content(*existing, std::make_shared<const std::string>(std::move(*content)));
                    ++stats.unchanged;
                    seen.insert(*existing);
                    continue;
                }
                // Changed since it was listed: re-index below.
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
            snap->binary_files.emplace(path, stamp);
            continue;
        }

        drop_existing(false);  // changed file: replace the old version
        Document doc;
        doc.path = path;
        doc.mtime = entry.mtime;
        doc.size = content->size();
        doc.symbols = symbols_of(path, *content);
        doc.content = std::make_shared<const std::string>(std::move(*content));
        const std::shared_ptr<const std::string> text = doc.content;
        const DocId id = docs.add(std::move(doc));
        index.add(id, *text);
        seen.insert(id);
        ++stats.indexed;
    }

    // Live docs not seen this run were deleted.
    for (const DocId id : docs.live_ids()) {
        if (!seen.contains(id)) {
            index.remove(id);
            docs.remove(id);
            ++stats.removed;
        }
    }
    index.compact();
    index.shrink_to_fit();
    snap->symbols.rebuild(docs);
    stats.symbols = snap->symbols.size();

    stats.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return snap;
}

}  // namespace codescope
