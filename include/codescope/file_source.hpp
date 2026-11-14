#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace codescope {

// Where documents come from; tests substitute an in-memory fake.
class IFileSource {
public:
    struct Entry {
        std::filesystem::path path;  // relative to the root
        std::uintmax_t size = 0;
        std::int64_t mtime = 0;      // compared for equality only
    };

    struct Listing {
        std::vector<Entry> files;      // sorted by UTF-8 path
        std::size_t skipped_dirs = 0;  // pruned ignored directories
    };

    IFileSource() = default;
    IFileSource(const IFileSource&) = delete;
    IFileSource& operator=(const IFileSource&) = delete;
    virtual ~IFileSource() = default;

    // Directories named in `skip_dirs` are not entered.
    virtual Listing list(const std::vector<std::string>& skip_dirs) const = 0;

    // Throws on I/O errors.
    virtual std::string read(const std::filesystem::path& p) const = 0;
};

// Local directory tree. Symlinks are not followed, so the walk can't escape the root.
class FilesystemSource final : public IFileSource {
public:
    explicit FilesystemSource(std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }

    Listing list(const std::vector<std::string>& skip_dirs) const override;
    std::string read(const std::filesystem::path& p) const override;

private:
    bool contains(const std::filesystem::path& full) const;  // after resolving symlinks

    std::filesystem::path root_;
};

}  // namespace codescope
