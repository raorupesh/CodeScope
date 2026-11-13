#pragma once

// Where documents come from. The pipeline only talks to IFileSource, so tests
// can feed it an in-memory fake repository.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace codescope {

class IFileSource {
public:
    struct Entry {
        std::filesystem::path path;  // relative to the source root
        std::uintmax_t size = 0;
        std::int64_t mtime = 0;      // opaque, only compared for equality
    };

    IFileSource() = default;
    IFileSource(const IFileSource&) = delete;
    IFileSource& operator=(const IFileSource&) = delete;
    virtual ~IFileSource() = default;

    // Every regular file under the root, sorted by path.
    virtual std::vector<Entry> list() const = 0;

    // Full content of the file at the relative path `p`. Throws on I/O errors.
    virtual std::string read(const std::filesystem::path& p) const = 0;
};

// Reads a directory tree from the local filesystem. Symlinks are not
// followed, so the walk cannot escape the root or loop.
class FilesystemSource final : public IFileSource {
public:
    explicit FilesystemSource(std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }

    std::vector<Entry> list() const override;
    std::string read(const std::filesystem::path& p) const override;

private:
    std::filesystem::path root_;
};

}  // namespace codescope
