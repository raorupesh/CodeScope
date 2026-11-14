#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "codescope/doc_store.hpp"
#include "codescope/symbols.hpp"
#include "codescope/trigram_index.hpp"

namespace codescope {

struct IngestStats {
    std::size_t files_seen = 0;
    std::size_t indexed = 0;            // new or changed files tokenized
    std::size_t unchanged = 0;
    std::size_t removed = 0;
    std::size_t skipped_ignored = 0;    // pruned directories
    std::size_t skipped_too_large = 0;
    std::size_t skipped_binary = 0;
    std::size_t read_errors = 0;
    std::size_t symbols = 0;
    double elapsed_ms = 0.0;
};

struct FileStamp {
    std::int64_t mtime = 0;
    std::uint64_t size = 0;
};

// Immutable once published; readers keep their shared_ptr across a swap.
struct IndexSnapshot {
    DocStore docs;
    TrigramIndex index;
    IngestStats stats;
    SymbolTable symbols;
    // Binary files from the last run, so unchanged ones aren't re-read (memory only).
    std::unordered_map<std::string, FileStamp> binary_files;
};

}  // namespace codescope
