#pragma once

// Inverted index: trigram -> sorted, unique list of documents containing it.

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codescope/types.hpp"

namespace codescope {

class TrigramIndex {
public:
    using PostingList = std::vector<DocId>;
    using PostingMap  = std::unordered_map<Trigram, PostingList>;

    // Index every trigram of `content` for `doc`. Doc ids are normally added
    // in increasing order (append); out-of-order ids are inserted in place.
    void add(DocId doc, std::string_view content);

    // Add `doc` to the posting list of each trigram (sorted, unique input).
    void add_trigrams(DocId doc, const std::vector<Trigram>& trigrams);

    // Mark `doc` deleted. It disappears from candidates() immediately; its
    // ids are purged from the posting lists by compact().
    void remove(DocId doc);

    bool is_removed(DocId doc) const { return removed_.contains(doc); }
    std::size_t removed_count() const { return removed_.size(); }

    // Purge removed docs from every posting list and drop empty lists.
    void compact();

    // Documents that may contain `query` (case-insensitive), sorted ascending.
    // std::nullopt means the query is shorter than 3 bytes, so the index
    // cannot narrow anything down and the caller must scan every document.
    std::optional<std::vector<DocId>> candidates(std::string_view query) const;

    std::size_t trigram_count() const { return postings_.size(); }

    // Approximate memory used by the posting lists, in bytes.
    std::size_t memory_bytes() const;

    // Raw access for the serializer.
    const PostingMap& postings() const { return postings_; }
    void set_postings(Trigram trigram, PostingList docs);

private:
    PostingMap postings_;
    std::unordered_set<DocId> removed_;
};

}  // namespace codescope
