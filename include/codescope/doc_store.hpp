#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codescope/symbols.hpp"
#include "codescope/types.hpp"

namespace codescope {

// Contents are shared_ptr so snapshots share unchanged files.
struct Document {
    std::string path;  // repository-relative, '/' separators
    std::int64_t mtime = 0;
    std::uint64_t size = 0;
    std::shared_ptr<const std::string> content;  // null until loaded
    std::shared_ptr<const std::vector<Symbol>> symbols;
    bool removed = false;
};

// Removed documents keep their slot (tombstone) so DocIds stay stable.
class DocStore {
public:
    DocId add(Document doc);  // ids increase; remove any live doc with the same path first
    void remove(DocId id);

    void set_content(DocId id, std::shared_ptr<const std::string> content);
    void set_symbols(DocId id, std::shared_ptr<const std::vector<Symbol>> symbols);

    const Document* get(DocId id) const;  // nullptr if out of range or removed
    std::optional<DocId> find(std::string_view path) const;

    std::size_t live_count() const { return by_path_.size(); }
    std::size_t slot_count() const { return docs_.size(); }
    std::uint64_t content_bytes() const;
    std::vector<DocId> live_ids() const;  // ascending

private:
    std::vector<Document> docs_;
    std::unordered_map<std::string, DocId> by_path_;
};

}  // namespace codescope
