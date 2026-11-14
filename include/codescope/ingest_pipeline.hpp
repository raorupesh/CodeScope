#pragma once

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
    std::vector<std::string> ignored_dirs{".git", ".hg", ".svn", "node_modules", "build", "third_party"};
    std::uint64_t max_file_bytes = 1024 * 1024;
};

// walk -> detect changes -> read -> binary check -> tokenize -> new snapshot.
class IngestPipeline {
public:
    explicit IngestPipeline(const IFileSource& source, IngestOptions options = {});

    // Never modifies `previous`; only new or changed files (mtime/size) are tokenized.
    std::shared_ptr<IndexSnapshot> run(const IndexSnapshot* previous) const;

    const IngestOptions& options() const { return options_; }

    static bool looks_binary(std::string_view content);  // NUL in the first 8 KB

private:
    const IFileSource& source_;
    IngestOptions options_;
};

}  // namespace codescope
