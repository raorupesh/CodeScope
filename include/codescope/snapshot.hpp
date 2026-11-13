#pragma once

// An immutable, self-contained view of the index that searches run against.
// Re-indexing builds a new snapshot and swaps it in; readers holding the old
// shared_ptr keep using it until they finish.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "codescope/doc_store.hpp"
#include "codescope/trigram_index.hpp"

namespace codescope {

struct IngestStats {
    std::size_t files_seen = 0;         // entries returned by the file source
    std::size_t indexed = 0;            // new or changed files (re)tokenized
    std::size_t unchanged = 0;          // same mtime + size: postings reused
    std::size_t removed = 0;            // previously indexed files that are gone or now skipped
    std::size_t skipped_ignored = 0;    // under an ignored directory
    std::size_t skipped_too_large = 0;  // larger than max_file_bytes
    std::size_t skipped_binary = 0;     // NUL byte in the first 8 KB
    std::size_t read_errors = 0;
    double elapsed_ms = 0.0;
};

struct FileStamp {
    std::int64_t mtime = 0;
    std::uint64_t size = 0;
};

struct IndexSnapshot {
    DocStore docs;
    TrigramIndex index;
    IngestStats stats;
    // Files skipped as binary on the last run. An unchanged entry (same mtime
    // and size) is skipped again without being read. Not saved to disk, so
    // after a restart each binary file is read once more.
    std::unordered_map<std::string, FileStamp> binary_files;
};

}  // namespace codescope
