#include "codescope/doc_store.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace codescope {

DocId DocStore::add(Document doc) {
    if (docs_.size() >= std::numeric_limits<DocId>::max()) {
        throw std::length_error("DocStore: document id space exhausted");
    }
    const auto id = static_cast<DocId>(docs_.size());
    doc.removed = false;
    by_path_[doc.path] = id;
    docs_.push_back(std::move(doc));
    return id;
}

void DocStore::remove(DocId id) {
    if (id >= docs_.size() || docs_[id].removed) {
        return;
    }
    Document& doc = docs_[id];
    const auto it = by_path_.find(doc.path);
    if (it != by_path_.end() && it->second == id) {
        by_path_.erase(it);
    }
    doc.removed = true;
    doc.content.reset();  // release memory; the tombstone keeps only metadata
}

void DocStore::set_content(DocId id, std::shared_ptr<const std::string> content) {
    if (id < docs_.size() && !docs_[id].removed) {
        docs_[id].content = std::move(content);
    }
}

const Document* DocStore::get(DocId id) const {
    if (id >= docs_.size() || docs_[id].removed) {
        return nullptr;
    }
    return &docs_[id];
}

std::optional<DocId> DocStore::find(std::string_view path) const {
    const auto it = by_path_.find(std::string(path));
    if (it == by_path_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint64_t DocStore::content_bytes() const {
    std::uint64_t total = 0;
    for (const Document& doc : docs_) {
        if (!doc.removed && doc.content) {
            total += doc.content->size();
        }
    }
    return total;
}

std::vector<DocId> DocStore::live_ids() const {
    std::vector<DocId> ids;
    ids.reserve(by_path_.size());
    for (std::size_t i = 0; i < docs_.size(); ++i) {
        if (!docs_[i].removed) {
            ids.push_back(static_cast<DocId>(i));
        }
    }
    return ids;
}

}  // namespace codescope
