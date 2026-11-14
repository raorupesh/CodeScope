#pragma once

#include "codescope/doc_store.hpp"
#include "codescope/trigram_index.hpp"
#include "codescope/types.hpp"

namespace codescope {

// Trigram candidates, then exact verification of each candidate's content.
class QueryEngine {
public:
    QueryEngine(const TrigramIndex& index, const DocStore& docs) : index_(index), docs_(docs) {}

    // Grouped by file in DocId order; matches on one line are merged.
    SearchResponse search(const SearchOptions& options) const;

private:
    const TrigramIndex& index_;
    const DocStore& docs_;
};

}  // namespace codescope
