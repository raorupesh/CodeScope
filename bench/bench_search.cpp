// Benchmark harness: index build time, index size, memory, and per-query
// latency (p50/p95). Prints Markdown tables ready to paste into the README.
//
//   codescope_bench <repo> [--queries FILE] [--iterations N] [--limit N]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "codescope/file_source.hpp"
#include "codescope/ingest_pipeline.hpp"
#include "codescope/query_engine.hpp"
#include "codescope/serializer.hpp"

#ifndef CODESCOPE_BENCH_QUERIES
#define CODESCOPE_BENCH_QUERIES "bench/queries.txt"
#endif

namespace fs = std::filesystem;
using namespace codescope;

namespace {

struct Memory {
    std::optional<double> rss_mb;
    std::optional<double> peak_mb;
};

Memory memory_usage() {
    Memory m;
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        const auto read_kb = [&line](std::string_view key) -> std::optional<double> {
            if (!line.starts_with(key)) {
                return std::nullopt;
            }
            return std::stod(line.substr(key.size())) / 1024.0;
        };
        if (auto v = read_kb("VmRSS:")) {
            m.rss_mb = v;
        } else if (auto peak = read_kb("VmHWM:")) {
            m.peak_mb = peak;
        }
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        m.rss_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        m.peak_mb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return m;
}

std::string fmt_mb(std::optional<double> v) {
    if (!v) {
        return "n/a";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", *v);
    return buf;
}

double percentile(std::vector<double> samples, double p) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(samples.size())));
    return samples[std::clamp<std::size_t>(rank, 1, samples.size()) - 1];
}

std::size_t parse_count(std::string_view text) {
    std::size_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size() || value == 0) {
        throw std::invalid_argument("expected a positive integer, got '" + std::string(text) + "'");
    }
    return value;
}

std::vector<std::string> read_queries(const fs::path& file) {
    std::ifstream in(file);
    if (!in) {
        throw std::runtime_error("cannot read queries file " + file.string());
    }
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

std::string compiler() {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

int run(int argc, char* argv[]) {
    if (argc < 2 || std::string_view(argv[1]) == "-h" || std::string_view(argv[1]) == "--help") {
        std::cerr << "usage: codescope_bench <repo> [--queries FILE] [--iterations N] [--limit N]\n";
        return 2;
    }
    const fs::path repo = argv[1];
    fs::path queries_file = CODESCOPE_BENCH_QUERIES;
    std::size_t iterations = 100;
    std::size_t limit = 200;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view value = argv[i + 1];
        if (flag == "--queries") {
            queries_file = fs::path(std::string(value));
        } else if (flag == "--iterations") {
            iterations = parse_count(value);
        } else if (flag == "--limit") {
            limit = parse_count(value);
        } else {
            throw std::invalid_argument("unknown option " + std::string(flag));
        }
    }
    const std::vector<std::string> queries = read_queries(queries_file);

    // ---- Index build -------------------------------------------------------
    const FilesystemSource source(repo);
    const IngestPipeline pipeline(source);
    const auto snapshot = pipeline.run(nullptr);
    const IngestStats& st = snapshot->stats;
    const Memory after_build = memory_usage();

    // ---- On-disk size and load time ----------------------------------------
    const fs::path index_file = fs::temp_directory_path() / "codescope_bench_index.bin";
    IndexSerializer::save(*snapshot, index_file);
    const auto disk_bytes = fs::file_size(index_file);
    const auto load_start = std::chrono::steady_clock::now();
    const auto loaded = IndexSerializer::load(index_file);
    const double load_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_start).count();
    std::error_code ec;
    fs::remove(index_file, ec);
    if (!loaded) {
        throw std::runtime_error("reloading the saved index failed");
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "## Environment\n\n"
              << "- Repository: `" << source.root().string() << "`\n"
              << "- Compiler: " << compiler() << "\n"
#ifdef NDEBUG
              << "- Build: optimized (NDEBUG)\n"
#else
              << "- Build: debug (numbers not representative)\n"
#endif
              << "- Hardware threads: " << std::thread::hardware_concurrency() << "\n\n";

    std::cout << "## Index\n\n"
              << "| Metric | Value |\n|---|---|\n"
              << "| Files indexed | " << snapshot->docs.live_count() << " |\n"
              << "| Source bytes indexed | " << snapshot->docs.content_bytes() << " ("
              << static_cast<double>(snapshot->docs.content_bytes()) / (1024.0 * 1024.0) << " MB) |\n"
              << "| Files skipped (ignored / binary / too large) | " << st.skipped_ignored << " / "
              << st.skipped_binary << " / " << st.skipped_too_large << " |\n"
              << "| Distinct trigrams | " << snapshot->index.trigram_count() << " |\n"
              << "| Build time (single-threaded) | " << st.elapsed_ms << " ms |\n"
              << "| Index size on disk | " << static_cast<double>(disk_bytes) / (1024.0 * 1024.0) << " MB |\n"
              << "| Index load time (without contents) | " << load_ms << " ms |\n"
              << "| Posting-list memory (approx.) | "
              << static_cast<double>(snapshot->index.memory_bytes()) / (1024.0 * 1024.0) << " MB |\n"
              << "| Process RSS after build (peak) | " << fmt_mb(after_build.rss_mb) << " ("
              << fmt_mb(after_build.peak_mb) << ") |\n\n";

    // ---- Query latency -----------------------------------------------------
    const QueryEngine engine(snapshot->index, snapshot->docs);
    std::vector<double> all;
    std::cout << "## Queries (" << iterations << " runs each, case-insensitive, limit " << limit << ")\n\n"
              << "| Query | Files | Lines | Candidates | p50 ms | p95 ms |\n|---|---:|---:|---:|---:|---:|\n";
    for (const std::string& q : queries) {
        SearchOptions options;
        options.query = q;
        options.max_results = limit;
        std::vector<double> samples;
        samples.reserve(iterations);
        SearchResponse last;
        for (std::size_t i = 0; i < iterations; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            last = engine.search(options);
            samples.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        all.insert(all.end(), samples.begin(), samples.end());
        std::cout << "| `" << q << "` | " << last.results.size() << " | " << last.total_matches
                  << (last.truncated ? "+" : "") << " | " << last.candidates << " | " << percentile(samples, 50)
                  << " | " << percentile(samples, 95) << " |\n";
    }
    std::cout << "\n**All queries:** p50 " << percentile(all, 50) << " ms, p95 " << percentile(all, 95) << " ms\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "codescope_bench: " << e.what() << '\n';
        return 1;
    }
}
