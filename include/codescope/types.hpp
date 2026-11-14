#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace codescope {

using DocId   = std::uint32_t;
using Trigram = std::uint32_t;  // (b0 << 16) | (b1 << 8) | b2

struct SearchOptions {
    std::string query;
    bool case_sensitive = false;
    std::vector<std::string> extensions;  // empty = any
    std::string path_prefix;              // empty = any
    std::size_t max_results = 200;        // matching lines
    std::size_t context_lines = 1;
};

struct LineMatch {
    std::uint32_t line_number = 0;  // 1-based
    std::string line;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;  // [start, end) byte offsets
    std::vector<std::string> before;
    std::vector<std::string> after;
};

struct FileResult {
    std::string path;
    std::vector<LineMatch> matches;
};

struct SearchResponse {
    std::vector<FileResult> results;
    std::size_t total_matches = 0;
    bool truncated = false;
    bool used_index = true;      // false: query too short, full scan
    std::size_t candidates = 0;  // docs that passed the trigram filter
    double took_ms = 0.0;
};

}  // namespace codescope
