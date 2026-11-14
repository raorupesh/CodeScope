#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codescope/types.hpp"

namespace codescope {

// Sorted, unique doc ids stored as delta + LEB128 varint bytes
// (first id, then the gap to each next id; most gaps fit in one byte).
class PostingList {
public:
    class Reader {
    public:
        explicit Reader(const PostingList& list) : data_(list.bytes_.data()), end_(data_ + list.bytes_.size()) {}

        // False at the end of the list.
        bool next(DocId& out) {
            if (data_ == end_) {
                return false;
            }
            std::uint32_t value = 0;
            int shift = 0;
            while (true) {
                const std::uint8_t byte = *data_++;
                value |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0) {
                    break;
                }
                shift += 7;
            }
            current_ = first_ ? value : current_ + value;
            first_ = false;
            out = current_;
            return true;
        }

    private:
        const std::uint8_t* data_;
        const std::uint8_t* end_;
        DocId current_ = 0;
        bool first_ = true;
    };

    static PostingList from_ids(const std::vector<DocId>& sorted_unique);

    // Validates untrusted bytes: well-formed varints, exactly `count`
    // strictly increasing ids, all below `id_limit`.
    static std::optional<PostingList> from_bytes(std::vector<std::uint8_t> bytes, std::uint32_t count,
                                                 std::uint64_t id_limit);

    // Requires empty() || id > back().
    void push_back(DocId id);

    bool empty() const { return count_ == 0; }
    std::uint32_t size() const { return count_; }
    DocId back() const { return last_; }

    std::vector<DocId> decode() const;
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::size_t memory_bytes() const { return bytes_.capacity(); }
    void shrink_to_fit() { bytes_.shrink_to_fit(); }

    bool operator==(const PostingList& other) const {
        return count_ == other.count_ && last_ == other.last_ && bytes_ == other.bytes_;
    }

private:
    void put_varint(std::uint32_t value);

    std::vector<std::uint8_t> bytes_;
    std::uint32_t count_ = 0;
    DocId last_ = 0;
};

// Inverted index: trigram -> posting list of documents containing it.
class TrigramIndex {
public:
    using PostingMap = std::unordered_map<Trigram, PostingList>;

    void add(DocId doc, std::string_view content);
    void add_trigrams(DocId doc, const std::vector<Trigram>& trigrams);  // sorted, unique

    // Hides `doc` immediately; compact() purges it from the lists.
    void remove(DocId doc);

    bool is_removed(DocId doc) const { return removed_.contains(doc); }
    std::size_t removed_count() const { return removed_.size(); }

    void compact();
    void shrink_to_fit();

    // Docs that may contain `query` (case-insensitive), ascending.
    // std::nullopt: query under 3 bytes, so every doc must be scanned.
    std::optional<std::vector<DocId>> candidates(std::string_view query) const;

    std::size_t trigram_count() const { return postings_.size(); }
    std::size_t memory_bytes() const;  // approximate

    const PostingMap& postings() const { return postings_; }
    void set_postings(Trigram trigram, PostingList docs);

private:
    PostingMap postings_;
    std::unordered_set<DocId> removed_;
};

}  // namespace codescope
