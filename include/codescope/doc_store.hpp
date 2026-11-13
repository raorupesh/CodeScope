#pragma once

// Document metadata and content, addressed by DocId.
//
// Content is kept in memory (shared_ptr<const std::string>) so verification
// never touches the disk and snapshots can share unchanged file contents
// cheaply. Removed documents leave a tombstone so DocIds stay stable.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codescope/types.hpp"

namespace codescope {

struct Document {
    std::string path;  // repository-relative, '/' separators
    std::int64_t mtime = 0;
    std::uint64_t size = 0;
    std::shared_ptr<const std::string> content;  // null until loaded
    bool removed = false;
};

class DocStore {
public:
    // Appends a document and returns its id (ids are assigned in increasing
    // order). Any live document with the same path must be removed first.
    DocId add(Document doc);

    // Tombstones `id`; it keeps its slot but is no longer findable.
    void remove(DocId id);

    void set_content(DocId id, std::shared_ptr<const std::string> content);

    // nullptr if `id` is out of range or removed.
    const Document* get(DocId id) const;

    std::optional<DocId> find(std::string_view path) const;

    std::size_t live_count() const { return by_path_.size(); }
    std::size_t slot_count() const { return docs_.size(); }

    // Total bytes of loaded content across live documents.
    std::uint64_t content_bytes() const;

    // Live document ids in ascending order.
    std::vector<DocId> live_ids() const;

private:
    std::vector<Document> docs_;
    std::unordered_map<std::string, DocId> by_path_;
};

}  // namespace codescope
