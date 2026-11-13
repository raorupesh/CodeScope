#pragma once

// Shared helpers for the unit tests: an in-memory fake repository and a
// self-deleting temporary directory.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "codescope/file_source.hpp"
#include "codescope/query_engine.hpp"
#include "codescope/snapshot.hpp"

namespace codescope::test_util {

class MemoryFileSource final : public IFileSource {
public:
    void put(const std::string& path, std::string content, std::int64_t mtime = 1) {
        files_[path] = File{std::move(content), mtime};
    }

    void erase(const std::string& path) { files_.erase(path); }

    // Makes read() throw for `path` (simulates an I/O error).
    void make_unreadable(const std::string& path) { unreadable_.insert(path); }

    std::size_t reads() const { return reads_.load(); }

    std::vector<Entry> list() const override {
        std::vector<Entry> out;
        for (const auto& [path, file] : files_) {  // std::map: already sorted
            out.push_back(Entry{path, file.content.size(), file.mtime});
        }
        return out;
    }

    std::string read(const std::filesystem::path& p) const override {
        ++reads_;
        const std::string key = p.generic_string();
        if (unreadable_.contains(key)) {
            throw std::runtime_error("simulated read error: " + key);
        }
        const auto it = files_.find(key);
        if (it == files_.end()) {
            throw std::runtime_error("no such file: " + key);
        }
        return it->second.content;
    }

private:
    struct File {
        std::string content;
        std::int64_t mtime = 0;
    };
    std::map<std::string, File> files_;
    std::set<std::string> unreadable_;
    mutable std::atomic<std::size_t> reads_{0};
};

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("codescope_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    void write(const std::string& relative, const std::string& content) const {
        const auto full = path_ / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        out << content;
    }

private:
    std::filesystem::path path_;
};

inline SearchResponse search(const IndexSnapshot& snap, const std::string& query, bool case_sensitive = false) {
    SearchOptions options;
    options.query = query;
    options.case_sensitive = case_sensitive;
    return QueryEngine(snap.index, snap.docs).search(options);
}

inline std::vector<std::string> result_paths(const SearchResponse& response) {
    std::vector<std::string> out;
    for (const FileResult& f : response.results) {
        out.push_back(f.path);
    }
    return out;
}

}  // namespace codescope::test_util
