#pragma once

// Binary on-disk index (all integers little-endian):
//
//   magic "CSIX" | version u32 | doc_count u32
//     per doc:     path_len u32 | path bytes | mtime i64 | size u64
//   trigram_count u32
//     per trigram: trigram u32 | list_len u32 | doc_ids u32[list_len]
//
// File contents are not stored: on load every document's content is null and
// the ingest pipeline re-reads unchanged files (without re-tokenizing them).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "codescope/snapshot.hpp"

namespace codescope {

class IndexSerializer {
public:
    static constexpr std::uint32_t kVersion = 1;

    // Writes `<file>` atomically: data goes to a temporary file in the same
    // directory which is then renamed over `file`. Removed documents are
    // dropped and ids are renumbered densely. Throws std::runtime_error.
    static void save(const IndexSnapshot& snapshot, const std::filesystem::path& file);

    // Returns nullptr if the file is missing, truncated, corrupt, or has a
    // different magic/version; `error` (if given) receives the reason.
    static std::shared_ptr<IndexSnapshot> load(const std::filesystem::path& file, std::string* error = nullptr);
};

}  // namespace codescope
