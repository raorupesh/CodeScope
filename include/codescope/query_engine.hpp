#pragma once

// Candidate selection (trigram index) followed by verification (exact
// substring search over each candidate's content).

#include "codescope/doc_store.hpp"
#include "codescope/trigram_index.hpp"
#include "codescope/types.hpp"

namespace codescope {

class QueryEngine {
public:
    QueryEngine(const TrigramIndex& index, const DocStore& docs) : index_(index), docs_(docs) {}

    // Results are grouped by file in DocId order (path order after a full
    // build). Matches on the same line are merged into one LineMatch.
    SearchResponse search(const SearchOptions& options) const;

private:
    const TrigramIndex& index_;
    const DocStore& docs_;
};

}  // namespace codescope
