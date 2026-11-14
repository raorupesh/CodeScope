#include "codescope/trigram_index.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "codescope/tokenizer.hpp"

namespace codescope {

void PostingList::put_varint(std::uint32_t value) {
    while (value >= 0x80) {
        bytes_.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    bytes_.push_back(static_cast<std::uint8_t>(value));
}

void PostingList::push_back(DocId id) {
    put_varint(count_ == 0 ? id : id - last_);
    last_ = id;
    ++count_;
}

PostingList PostingList::from_ids(const std::vector<DocId>& sorted_unique) {
    PostingList list;
    list.bytes_.reserve(sorted_unique.size());
    for (const DocId id : sorted_unique) {
        list.push_back(id);
    }
    return list;
}

std::optional<PostingList> PostingList::from_bytes(std::vector<std::uint8_t> bytes, std::uint32_t count,
                                                   std::uint64_t id_limit) {
    std::uint32_t seen = 0;
    std::uint64_t current = 0;
    std::size_t i = 0;
    while (i < bytes.size()) {
        std::uint64_t value = 0;
        int shift = 0;
        while (true) {
            if (i >= bytes.size() || shift > 28) {
                return std::nullopt;  // truncated or over-long varint
            }
            const std::uint8_t byte = bytes[i++];
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                break;
            }
            shift += 7;
        }
        if (seen > 0 && value == 0) {
            return std::nullopt;  // ids must be strictly increasing
        }
        current = seen == 0 ? value : current + value;
        if (current >= id_limit) {
            return std::nullopt;
        }
        ++seen;
    }
    if (seen != count || count == 0) {
        return std::nullopt;
    }
    PostingList list;
    list.bytes_ = std::move(bytes);
    list.count_ = count;
    list.last_ = static_cast<DocId>(current);
    return list;
}

std::vector<DocId> PostingList::decode() const {
    std::vector<DocId> out;
    out.reserve(count_);
    Reader reader(*this);
    for (DocId id = 0; reader.next(id);) {
        out.push_back(id);
    }
    return out;
}

void TrigramIndex::add(DocId doc, std::string_view content) {
    add_trigrams(doc, extract_trigrams(content));
}

void TrigramIndex::add_trigrams(DocId doc, const std::vector<Trigram>& trigrams) {
    removed_.erase(doc);
    for (const Trigram t : trigrams) {
        PostingList& list = postings_[t];
        if (list.empty() || list.back() < doc) {
            list.push_back(doc);  // common case: increasing ids
        } else if (list.back() != doc) {
            // Rare out-of-order insert: decode, insert, re-encode.
            std::vector<DocId> ids = list.decode();
            const auto it = std::lower_bound(ids.begin(), ids.end(), doc);
            if (it == ids.end() || *it != doc) {
                ids.insert(it, doc);
                list = PostingList::from_ids(ids);
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
        std::vector<DocId> ids = it->second.decode();
        const std::size_t before = ids.size();
        std::erase_if(ids, [this](DocId d) { return removed_.contains(d); });
        if (ids.empty()) {
            it = postings_.erase(it);
            continue;
        }
        if (ids.size() != before) {
            it->second = PostingList::from_ids(ids);
        }
        ++it;
    }
    removed_.clear();
}

void TrigramIndex::shrink_to_fit() {
    for (auto& [trigram, list] : postings_) {
        list.shrink_to_fit();
    }
}

std::optional<std::vector<DocId>> TrigramIndex::candidates(std::string_view query) const {
    const std::vector<Trigram> trigrams = extract_trigrams(query);
    if (trigrams.empty()) {
        return std::nullopt;
    }

    // A missing trigram means nothing matches.
    std::vector<const PostingList*> lists;
    lists.reserve(trigrams.size());
    for (const Trigram t : trigrams) {
        const auto it = postings_.find(t);
        if (it == postings_.end()) {
            return std::vector<DocId>{};
        }
        lists.push_back(&it->second);
    }

    // Shortest first: the result only shrinks.
    std::sort(lists.begin(), lists.end(),
              [](const PostingList* a, const PostingList* b) { return a->size() < b->size(); });

    std::vector<DocId> result = lists.front()->decode();
    std::erase_if(result, [this](DocId d) { return removed_.contains(d); });

    // Merge further lists straight from their encoded bytes.
    std::vector<DocId> scratch;
    for (std::size_t i = 1; i < lists.size() && !result.empty(); ++i) {
        scratch.clear();
        PostingList::Reader reader(*lists[i]);
        DocId next = 0;
        bool more = reader.next(next);
        for (const DocId d : result) {
            while (more && next < d) {
                more = reader.next(next);
            }
            if (!more) {
                break;
            }
            if (next == d) {
                scratch.push_back(d);
            }
        }
        result.swap(scratch);
    }
    return result;
}

std::size_t TrigramIndex::memory_bytes() const {
    std::size_t bytes = 0;
    for (const auto& [trigram, list] : postings_) {
        bytes += sizeof(trigram) + sizeof(list) + list.memory_bytes();
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
