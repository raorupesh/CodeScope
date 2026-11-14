#pragma once

// Binary index file, little-endian:
//   "CSIX" | version u32 | doc_count u32
//     per doc:     path_len u32 | path | mtime i64 | size u64
//   trigram_count u32
//     per trigram: trigram u32 | doc_count u32 | byte_len u32 | posting bytes
// File contents are not stored; they are re-read on load.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "codescope/snapshot.hpp"

namespace codescope {

class IndexSerializer {
public:
    static constexpr std::uint32_t kVersion = 2;  // 2: delta + varint posting lists

    // Atomic: writes a temp file, then renames it over `file`. Throws on failure.
    static void save(const IndexSnapshot& snapshot, const std::filesystem::path& file);

    // nullptr if missing, corrupt or another version; `error` gets the reason.
    static std::shared_ptr<IndexSnapshot> load(const std::filesystem::path& file, std::string* error = nullptr);
};

}  // namespace codescope
