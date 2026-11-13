#pragma once

// Core value types shared across CodeScope.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace codescope {

using DocId   = std::uint32_t;
using Trigram = std::uint32_t;  // 3 bytes packed: (b0 << 16) | (b1 << 8) | b2

struct SearchOptions {
    std::string query;
    bool case_sensitive = false;
    std::vector<std::string> extensions;  // e.g. {".cpp", ".hpp"}; empty = any
    std::string path_prefix;              // e.g. "src/"; empty = any
    std::size_t max_results = 200;        // cap on the total number of matching lines
    std::size_t context_lines = 1;        // lines of context above and below each match
};

struct LineMatch {
    std::uint32_t line_number = 0;  // 1-based
    std::string line;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;  // [start, end) byte offsets in `line`
    std::vector<std::string> before;  // context lines immediately above (oldest first)
    std::vector<std::string> after;   // context lines immediately below
};

struct FileResult {
    std::string path;
    std::vector<LineMatch> matches;
};

struct SearchResponse {
    std::vector<FileResult> results;
    std::size_t total_matches = 0;  // matching lines returned
    bool truncated = false;         // true if max_results cut the search short
    bool used_index = true;         // false when the query was too short for trigrams (full scan)
    std::size_t candidates = 0;     // documents that passed the trigram filter
    double took_ms = 0.0;
};

}  // namespace codescope
