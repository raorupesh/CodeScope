#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "codescope/file_source.hpp"
#include "codescope/http_server.hpp"
#include "codescope/search_service.hpp"
#include "codescope/version.hpp"

#ifndef CODESCOPE_DEFAULT_WEB_DIR
#define CODESCOPE_DEFAULT_WEB_DIR "web"
#endif

namespace fs = std::filesystem;
using namespace codescope;

namespace {

void print_usage(std::ostream& out) {
    out << "usage: codescope <command> [args]\n"
           "\n"
           "commands:\n"
           "  search <repo> <query> [options]   print matching lines (grep-like)\n"
           "  index  <repo> --index FILE        build or update the saved index\n"
           "  serve  <repo> [options]           start the REST API + web UI\n"
           "\n"
           "search options:\n"
           "  --case              case-sensitive match (default: insensitive)\n"
           "  --ext LIST          only these extensions, e.g. .cpp,.hpp\n"
           "  --path PREFIX       only paths starting with PREFIX, e.g. src/\n"
           "  --limit N           at most N matching lines (default 200)\n"
           "  --context N         lines of context around each match (default 0)\n"
           "\n"
           "serve options:\n"
           "  --host HOST         address to bind (default 127.0.0.1)\n"
           "  --port N            port to listen on (default 8080)\n"
           "  --web DIR           directory with the web UI (default: built-in path)\n"
           "\n"
           "common options:\n"
           "  --index FILE        load/save the index at FILE (serve default: index.bin)\n"
           "  --no-index-file     serve without loading or saving an index file\n"
           "  --ignore LIST       directory names to skip (default .git,.hg,.svn,node_modules,build,third_party)\n"
           "  --max-file-bytes N  skip files larger than N bytes (default 1048576)\n"
           "\n"
           "  -h, --help          show this help\n"
           "  -v, --version       show version\n";
}

struct Args {
    std::string command;
    std::vector<std::string> positional;
    SearchOptions search;
    ServiceConfig config;
    bool no_index_file = false;
    std::string host = "127.0.0.1";
    int port = 8080;
    std::optional<fs::path> web_dir;
};

struct UsageError {
    std::string message;
};

std::size_t parse_count(std::string_view flag, std::string_view text) {
    std::size_t value = 0;
    const char* const last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), last, value);
    if (text.empty() || ec != std::errc() || ptr != last) {
        throw UsageError{std::string(flag) + " expects a non-negative integer, got '" + std::string(text) + "'"};
    }
    return value;
}

std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(',', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            out.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

Args parse_args(int argc, char* argv[]) {
    Args args;
    args.command = argv[1];
    args.search.context_lines = 0;  // CLI default: grep-like output

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                throw UsageError{std::string(arg) + " needs a value"};
            }
            return argv[++i];
        };

        if (arg == "--case") {
            args.search.case_sensitive = true;
        } else if (arg == "--ext") {
            args.search.extensions = split_list(value());
        } else if (arg == "--path") {
            args.search.path_prefix = std::string(value());
        } else if (arg == "--limit") {
            args.search.max_results = parse_count(arg, value());
        } else if (arg == "--context") {
            args.search.context_lines = parse_count(arg, value());
        } else if (arg == "--index") {
            args.config.index_file = fs::path(std::string(value()));
        } else if (arg == "--no-index-file") {
            args.no_index_file = true;
        } else if (arg == "--ignore") {
            args.config.ingest.ignored_dirs = split_list(value());
        } else if (arg == "--max-file-bytes") {
            args.config.ingest.max_file_bytes = parse_count(arg, value());
        } else if (arg == "--host") {
            args.host = std::string(value());
        } else if (arg == "--port") {
            const std::size_t port = parse_count(arg, value());
            if (port > 65535) {
                throw UsageError{"--port must be between 0 and 65535"};
            }
            args.port = static_cast<int>(port);
        } else if (arg == "--web") {
            args.web_dir = fs::path(std::string(value()));
        } else if (arg.starts_with("--")) {
            throw UsageError{"unknown option " + std::string(arg)};
        } else {
            args.positional.emplace_back(arg);
        }
    }
    return args;
}

void print_ingest(const IngestStats& s, std::ostream& out) {
    out << "indexed " << s.indexed << ", unchanged " << s.unchanged << ", removed " << s.removed
        << " (seen " << s.files_seen << "; skipped: " << s.skipped_ignored << " ignored, " << s.skipped_binary
        << " binary, " << s.skipped_too_large << " too large; " << s.read_errors << " read errors) in "
        << s.elapsed_ms << " ms\n";
}

std::unique_ptr<SearchService> open_service(const std::string& repo, ServiceConfig config) {
    return std::make_unique<SearchService>(std::make_unique<FilesystemSource>(repo), std::move(config));
}

void report_service_error(const SearchService& service) {
    const std::string error = service.stats().last_error;
    if (!error.empty()) {
        std::cerr << "codescope: " << error << '\n';
    }
}

int cmd_search(Args& args) {
    if (args.positional.size() != 2) {
        throw UsageError{"search needs <repo> <query>"};
    }
    args.search.query = args.positional[1];
    auto service = open_service(args.positional[0], args.config);
    const IngestStats ingest = service->initialize();
    report_service_error(*service);

    const SearchResponse response = service->search(args.search);
    for (const FileResult& file : response.results) {
        for (const LineMatch& m : file.matches) {
            auto number = m.line_number - static_cast<std::uint32_t>(m.before.size());
            for (const std::string& line : m.before) {
                std::cout << file.path << '-' << number++ << '-' << line << '\n';
            }
            std::cout << file.path << ':' << m.line_number << ':' << m.line << '\n';
            number = m.line_number + 1;
            for (const std::string& line : m.after) {
                std::cout << file.path << '-' << number++ << '-' << line << '\n';
            }
            if (!m.before.empty() || !m.after.empty()) {
                std::cout << "--\n";
            }
        }
    }
    std::cerr << response.total_matches << " matching lines in " << response.results.size() << " files"
              << (response.truncated ? " (truncated)" : "") << "; search " << response.took_ms << " ms, "
              << response.candidates << " candidate files" << (response.used_index ? "" : " (full scan)")
              << "; index " << ingest.elapsed_ms << " ms\n";
    return response.total_matches > 0 ? 0 : 1;
}

int cmd_index(Args& args) {
    if (args.positional.size() != 1) {
        throw UsageError{"index needs <repo>"};
    }
    if (!args.config.index_file) {
        throw UsageError{"index needs --index FILE"};
    }
    auto service = open_service(args.positional[0], args.config);
    const IngestStats ingest = service->initialize();
    print_ingest(ingest, std::cout);
    const ServiceStats stats = service->stats();
    std::cout << stats.docs << " documents, " << stats.trigrams << " trigrams"
              << (stats.loaded_from_disk ? " (updated saved index)" : "") << '\n';
    report_service_error(*service);
    return stats.last_error.empty() ? 0 : 1;
}

int cmd_serve(Args& args) {
    if (args.positional.size() != 1) {
        throw UsageError{"serve needs <repo>"};
    }
    if (args.no_index_file) {
        args.config.index_file.reset();
    } else if (!args.config.index_file) {
        args.config.index_file = fs::path("index.bin");
    }
    fs::path web = args.web_dir.value_or(fs::path(CODESCOPE_DEFAULT_WEB_DIR));
    std::error_code ec;
    if (!fs::is_directory(web, ec)) {
        std::cerr << "codescope: web UI directory not found (" << web.string()
                  << "); serving the API only. Use --web DIR.\n";
    }

    auto service = open_service(args.positional[0], args.config);
    std::cerr << "codescope: indexing " << args.positional[0] << " ...\n";
    print_ingest(service->initialize(), std::cerr);
    report_service_error(*service);

    HttpServer server(*service, web);
    const int port = server.bind(args.host, args.port);
    if (port < 0) {
        std::cerr << "codescope: cannot listen on " << args.host << ':' << args.port << '\n';
        return 1;
    }
    std::cerr << "codescope: listening on http://" << args.host << ':' << port << '\n';
    return server.serve() ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(std::cerr);
        return 2;
    }

    const std::string_view cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") {
        print_usage(std::cout);
        return 0;
    }
    if (cmd == "-v" || cmd == "--version") {
        std::cout << "codescope " << codescope::version() << '\n';
        return 0;
    }

    try {
        Args args = parse_args(argc, argv);
        if (cmd == "search") {
            return cmd_search(args);
        }
        if (cmd == "index") {
            return cmd_index(args);
        }
        if (cmd == "serve") {
            return cmd_serve(args);
        }
        std::cerr << "codescope: unknown command '" << cmd << "'\n\n";
        print_usage(std::cerr);
        return 2;
    } catch (const UsageError& e) {
        std::cerr << "codescope: " << e.message << "\n\n";
        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "codescope: " << e.what() << '\n';
        return 1;
    }
}
