#include "codescope/file_source.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace codescope {

FilesystemSource::FilesystemSource(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        throw std::invalid_argument("not a directory: " + root_.string());
    }
    root_ = fs::canonical(root_);
}

std::vector<IFileSource::Entry> FilesystemSource::list() const {
    std::vector<Entry> entries;
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(root_, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        throw std::runtime_error("cannot list " + root_.string() + ": " + ec.message());
    }
    for (const auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;  // unreadable entry; keep walking
        }
        const fs::directory_entry& entry = *it;
        std::error_code entry_ec;
        if (entry.is_symlink(entry_ec) || !entry.is_regular_file(entry_ec)) {
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
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.path.generic_string() < b.path.generic_string(); });
    return entries;
}

std::string FilesystemSource::read(const fs::path& p) const {
    std::ifstream in(root_ / p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + p.generic_string());
    }
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        throw std::runtime_error("read error on " + p.generic_string());
    }
    return content;
}

}  // namespace codescope
