#include "codescope/file_source.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "codescope/path_utils.hpp"

namespace fs = std::filesystem;

namespace codescope {

FilesystemSource::FilesystemSource(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        throw std::invalid_argument("not a directory: " + to_utf8(root_));
    }
    root_ = fs::canonical(root_);
}

IFileSource::Listing FilesystemSource::list(const std::vector<std::string>& skip_dirs) const {
    Listing listing;
    std::vector<Entry>& entries = listing.files;
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        throw std::runtime_error("cannot list " + to_utf8(root_) + ": " + ec.message());
    }
    for (const auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;  // unreadable entry; keep walking
        }
        const fs::directory_entry& entry = *it;
        std::error_code entry_ec;
        if (entry.is_symlink(entry_ec)) {
            continue;
        }
        if (entry.is_directory(entry_ec)) {
            // Prune ignored directories so they are never walked.
            const std::string name = to_utf8(entry.path().filename());
            if (std::find(skip_dirs.begin(), skip_dirs.end(), name) != skip_dirs.end()) {
                it.disable_recursion_pending();
                ++listing.skipped_dirs;
            }
            continue;
        }
        if (!entry.is_regular_file(entry_ec)) {
            continue;
        }
        const auto size = entry.file_size(entry_ec);
        if (entry_ec) {
            continue;
        }
        const auto mtime = entry.last_write_time(entry_ec);
        if (entry_ec) {
            continue;
        }
        entries.push_back(Entry{
            entry.path().lexically_relative(root_),
            size,
            static_cast<std::int64_t>(mtime.time_since_epoch().count()),
        });
    }
    // UTF-8 order gives the same document order on every platform.
    std::vector<std::pair<std::string, Entry>> keyed;
    keyed.reserve(entries.size());
    for (Entry& e : entries) {
        std::string key = to_utf8(e.path);
        keyed.emplace_back(std::move(key), std::move(e));
    }
    std::sort(keyed.begin(), keyed.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    entries.clear();
    for (auto& [key, e] : keyed) {
        entries.push_back(std::move(e));
    }
    return listing;
}

bool FilesystemSource::contains(const fs::path& full) const {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(full, ec);
    if (ec) {
        return false;
    }
    const fs::path rel = resolved.lexically_relative(root_);
    return !rel.empty() && *rel.begin() != "..";
}

std::string FilesystemSource::read(const fs::path& p) const {
    const fs::path full = root_ / p;
    // Re-checked at read time: the file may have been swapped for a symlink since listing.
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(full, ec)) || !contains(full)) {
        throw std::runtime_error("refusing to read outside the repository: " + to_utf8(p));
    }
    std::ifstream in(full, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + to_utf8(p));
    }
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        throw std::runtime_error("read error on " + to_utf8(p));
    }
    return content;
}

}  // namespace codescope
