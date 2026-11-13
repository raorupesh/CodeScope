#include "codescope/trigram_index.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "codescope/tokenizer.hpp"

namespace codescope {

void TrigramIndex::add(DocId doc, std::string_view content) {
    add_trigrams(doc, extract_trigrams(content));
}

void TrigramIndex::add_trigrams(DocId doc, const std::vector<Trigram>& trigrams) {
    removed_.erase(doc);
    for (const Trigram t : trigrams) {
        PostingList& list = postings_[t];
        if (list.empty() || list.back() < doc) {
            list.push_back(doc);  // common case: ids arrive in increasing order
        } else {
            const auto it = std::lower_bound(list.begin(), list.end(), doc);
            if (it == list.end() || *it != doc) {
                list.insert(it, doc);
            }
        }
    }
}

void TrigramIndex::remove(DocId doc) {
    removed_.insert(doc);
}

void TrigramIndex::compact() {
    if (removed_.empty()) {
        return;
    }
    for (auto it = postings_.begin(); it != postings_.end();) {
        PostingList& list = it->second;
        std::erase_if(list, [this](DocId d) { return removed_.contains(d); });
        if (list.empty()) {
            it = postings_.erase(it);
        } else {
            ++it;
        }
    }
    removed_.clear();
}

std::optional<std::vector<DocId>> TrigramIndex::candidates(std::string_view query) const {
    const std::vector<Trigram> trigrams = extract_trigrams(query);
    if (trigrams.empty()) {
        return std::nullopt;
    }

    // Gather the posting lists; a missing trigram means no document matches.
    std::vector<const PostingList*> lists;
    lists.reserve(trigrams.size());
    for (const Trigram t : trigrams) {
        const auto it = postings_.find(t);
        if (it == postings_.end()) {
            return std::vector<DocId>{};
        }
        lists.push_back(&it->second);
    }

    // Intersect shortest first: the running result can only shrink, so
    // starting small keeps every step O(|result| + |next list|).
    std::sort(lists.begin(), lists.end(),
              [](const PostingList* a, const PostingList* b) { return a->size() < b->size(); });

    std::vector<DocId> result;
    result.reserve(lists.front()->size());
    std::copy_if(lists.front()->begin(), lists.front()->end(), std::back_inserter(result),
                 [this](DocId d) { return !removed_.contains(d); });

    std::vector<DocId> scratch;
    for (std::size_t i = 1; i < lists.size() && !result.empty(); ++i) {
        scratch.clear();
        std::set_intersection(result.begin(), result.end(), lists[i]->begin(), lists[i]->end(),
                              std::back_inserter(scratch));
        result.swap(scratch);
    }
    return result;
}

std::size_t TrigramIndex::memory_bytes() const {
    std::size_t bytes = 0;
    for (const auto& [trigram, list] : postings_) {
        bytes += sizeof(trigram) + sizeof(list) + list.capacity() * sizeof(DocId);
    }
    return bytes;
}

void TrigramIndex::set_postings(Trigram trigram, PostingList docs) {
    if (docs.empty()) {
        postings_.erase(trigram);
    } else {
        postings_[trigram] = std::move(docs);
    }
}

}  // namespace codescope
