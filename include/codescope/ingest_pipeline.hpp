#pragma once

// list files -> filter -> detect changes -> read -> binary check -> index
//
// The pipeline never mutates the snapshot it is given. It copies it (file
// contents are shared, not duplicated), applies the changes, and returns a
// new snapshot the caller can swap in.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "codescope/file_source.hpp"
#include "codescope/snapshot.hpp"

namespace codescope {

struct IngestOptions {
    // Directory names skipped wherever they appear in a path.
    std::vector<std::string> ignored_dirs{".git", ".hg", ".svn", "node_modules", "build", "third_party"};
    std::uint64_t max_file_bytes = 1024 * 1024;
};

class IngestPipeline {
public:
    explicit IngestPipeline(const IFileSource& source, IngestOptions options = {});

    // Builds a new snapshot. With `previous == nullptr` everything is indexed;
    // otherwise only new and changed files (by mtime or size) are tokenized,
    // and files that disappeared are removed.
    std::shared_ptr<IndexSnapshot> run(const IndexSnapshot* previous) const;

    const IngestOptions& options() const { return options_; }

    // True if a NUL byte appears in the first 8 KB.
    static bool looks_binary(std::string_view content);

    bool is_ignored(const std::filesystem::path& relative_path) const;

private:
    const IFileSource& source_;
    IngestOptions options_;
};

}  // namespace codescope
