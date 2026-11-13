#include "codescope/query_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

#include "codescope/path_utils.hpp"
#include "codescope/tokenizer.hpp"

namespace codescope {
namespace {

struct CaseInsensitiveHash {
    std::size_t operator()(char c) const noexcept { return std::hash<char>{}(ascii_lower(c)); }
};

struct CaseInsensitiveEqual {
    bool operator()(char a, char b) const noexcept { return ascii_lower(a) == ascii_lower(b); }
};

std::string_view strip_cr(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

// Up to `count` lines that end right before `line_start`, oldest first.
std::vector<std::string> lines_before(const char* begin, const char* line_start, std::size_t count) {
    std::vector<std::string> out;
    const char* cursor = line_start;  // start of the line after the one we want
    while (out.size() < count && cursor > begin) {
        const char* prev_end = cursor - 1;  // the '\n' that ends the previous line
        const char* prev_start = prev_end;
        while (prev_start > begin && *(prev_start - 1) != '\n') {
            --prev_start;
        }
        out.emplace_back(strip_cr(std::string_view(prev_start, static_cast<std::size_t>(prev_end - prev_start))));
        cursor = prev_start;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// Up to `count` lines that start right after `line_end` (which points at the
// line's '\n' or at `end`).
std::vector<std::string> lines_after(const char* line_end, const char* end, std::size_t count) {
    std::vector<std::string> out;
    const char* cursor = line_end;
    while (out.size() < count && cursor < end) {
        const char* next_start = cursor + 1;
        if (next_start >= end) {
            break;  // file ends with a newline: there is no further line
        }
        const auto* nl = static_cast<const char*>(
            std::memchr(next_start, '\n', static_cast<std::size_t>(end - next_start)));
        const char* next_end = nl ? nl : end;
        out.emplace_back(strip_cr(std::string_view(next_start, static_cast<std::size_t>(next_end - next_start))));
        cursor = next_end;
    }
    return out;
}

// Appends every (non-overlapping) occurrence found by `searcher` to `out`,
// one LineMatch per line, stopping after `limit` lines. Sets `more` when a
// further matching line exists beyond the limit.
template <class Searcher>
void collect_matches(std::string_view text, const Searcher& searcher, std::size_t context, std::size_t limit,
                     FileResult& out, bool& more) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();

    const char* cursor = begin;
    const char* counted_upto = begin;  // newlines before this point are counted
    const char* line_start = begin;
    std::uint32_t line_number = 1;

    while (cursor < end) {
        const auto [match_begin, match_end] = searcher(cursor, end);
        if (match_begin == end || match_begin == match_end) {
            break;
        }

        // Advance the line counter to the line containing match_begin.
        while (const auto* nl = static_cast<const char*>(
                   std::memchr(counted_upto, '\n', static_cast<std::size_t>(match_begin - counted_upto)))) {
            ++line_number;
            line_start = nl + 1;
            counted_upto = line_start;
        }
        counted_upto = match_begin;

        const auto* nl = static_cast<const char*>(
            std::memchr(match_begin, '\n', static_cast<std::size_t>(end - match_begin)));
        const char* line_end = nl ? nl : end;
        const std::string_view line = strip_cr(std::string_view(line_start, static_cast<std::size_t>(line_end - line_start)));

        if (out.matches.empty() || out.matches.back().line_number != line_number) {
            if (out.matches.size() >= limit) {
                more = true;
                return;
            }
            LineMatch lm;
            lm.line_number = line_number;
            lm.line = std::string(line);
            if (context > 0) {
                lm.before = lines_before(begin, line_start, context);
                lm.after = lines_after(line_end, end, context);
            }
            out.matches.push_back(std::move(lm));
        }

        const auto start = static_cast<std::uint32_t>(std::min<std::size_t>(
            static_cast<std::size_t>(match_begin - line_start), line.size()));
        const auto stop = static_cast<std::uint32_t>(std::min<std::size_t>(
            static_cast<std::size_t>(match_end - line_start), line.size()));
        out.matches.back().ranges.emplace_back(start, stop);

        cursor = match_end;
    }
}

std::vector<std::string> normalize_extensions(const std::vector<std::string>& extensions) {
    std::vector<std::string> out;
    for (const std::string& ext : extensions) {
        if (ext.empty()) {
            continue;
        }
        std::string e = ascii_lower(ext);
        if (e.front() != '.') {
            e.insert(e.begin(), '.');
        }
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace

SearchResponse QueryEngine::search(const SearchOptions& options) const {
    const auto started = std::chrono::steady_clock::now();
    SearchResponse response;

    const std::string_view query = options.query;
    if (!query.empty() && options.max_results > 0) {
        // 1. Candidates from the index (or every document for short queries).
        std::vector<DocId> ids;
        if (auto candidates = index_.candidates(query)) {
            ids = std::move(*candidates);
        } else {
            response.used_index = false;
            ids = docs_.live_ids();
        }
        response.candidates = ids.size();

        const std::vector<std::string> extensions = normalize_extensions(options.extensions);
        const char* const q_begin = query.data();
        const char* const q_end = q_begin + query.size();
        const std::boyer_moore_horspool_searcher<const char*> exact(q_begin, q_end);
        const std::boyer_moore_horspool_searcher<const char*, CaseInsensitiveHash, CaseInsensitiveEqual> folded(
            q_begin, q_end);

        for (std::size_t i = 0; i < ids.size(); ++i) {
            const Document* doc = docs_.get(ids[i]);
            if (doc == nullptr || !doc->content) {
                continue;
            }
            // 2. Cheap filters before touching the content.
            if (!options.path_prefix.empty() && !doc->path.starts_with(options.path_prefix)) {
                continue;
            }
            if (!extensions.empty() &&
                std::find(extensions.begin(), extensions.end(), extension_of(doc->path)) == extensions.end()) {
                continue;
            }

            // 3. Verify: the index only says "may contain".
            FileResult file{doc->path, {}};
            bool more = false;
            const std::size_t remaining = options.max_results - response.total_matches;
            if (options.case_sensitive) {
                collect_matches(*doc->content, exact, options.context_lines, remaining, file, more);
            } else {
                collect_matches(*doc->content, folded, options.context_lines, remaining, file, more);
            }
            if (!file.matches.empty()) {
                response.total_matches += file.matches.size();
                response.results.push_back(std::move(file));
            }
            if (more || (response.total_matches >= options.max_results && i + 1 < ids.size())) {
                response.truncated = true;
                break;
            }
        }
    }

    response.took_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return response;
}

}  // namespace codescope
