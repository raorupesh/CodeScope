#include "codescope/http_server.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "codescope/path_utils.hpp"

namespace codescope {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxLimit = 5000;
constexpr std::size_t kMaxContext = 10;

void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    // Source files are not guaranteed to be valid UTF-8; replace bad bytes
    // instead of throwing.
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& message) {
    send_json(res, status, json{{"error", message}});
}

std::optional<std::size_t> parse_size(std::string_view text) {
    std::size_t value = 0;
    const char* const last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), last, value);
    if (ec != std::errc() || ptr != last || text.empty()) {
        return std::nullopt;
    }
    return value;
}

bool parse_bool(std::string_view text) {
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(',', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view item = text.substr(start, end - start);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (!item.empty()) {
            out.emplace_back(item);
        }
        start = end + 1;
    }
    return out;
}

double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

json match_json(const LineMatch& m) {
    json ranges = json::array();
    for (const auto& [start, end] : m.ranges) {
        ranges.push_back(json::array({start, end}));
    }
    return json{
        {"line_number", m.line_number},
        {"line", m.line},
        {"ranges", std::move(ranges)},
        {"before", m.before},
        {"after", m.after},
    };
}

json search_json(const SearchOptions& options, const SearchResponse& response) {
    json results = json::array();
    for (const FileResult& file : response.results) {
        json matches = json::array();
        for (const LineMatch& m : file.matches) {
            matches.push_back(match_json(m));
        }
        results.push_back(json{{"path", file.path}, {"matches", std::move(matches)}});
    }
    return json{
        {"query", options.query},
        {"case_sensitive", options.case_sensitive},
        {"took_ms", round3(response.took_ms)},
        {"total_files", response.results.size()},
        {"total_matches", response.total_matches},
        {"truncated", response.truncated},
        {"used_index", response.used_index},
        {"candidates", response.candidates},
        {"results", std::move(results)},
    };
}

json stats_json(const ServiceStats& s) {
    const IngestStats& r = s.last_run;
    return json{
        {"docs", s.docs},
        {"trigrams", s.trigrams},
        {"index_bytes", s.index_bytes},
        {"content_bytes", s.content_bytes},
        {"last_index_ms", round3(r.elapsed_ms)},
        {"files_seen", r.files_seen},
        {"indexed", r.indexed},
        {"unchanged", r.unchanged},
        {"removed", r.removed},
        {"read_errors", r.read_errors},
        {"skipped", json{{"binary", r.skipped_binary}, {"too_large", r.skipped_too_large}, {"ignored", r.skipped_ignored}}},
        {"reindexing", s.reindexing},
        {"loaded_from_disk", s.loaded_from_disk},
        {"generation", s.generation},
        {"last_error", s.last_error},
    };
}

}  // namespace

struct HttpServer::Impl {
    SearchService& service;
    httplib::Server server;

    explicit Impl(SearchService& svc) : service(svc) {}

    void handle_search(const httplib::Request& req, httplib::Response& res) const {
        SearchOptions options;
        options.query = req.get_param_value("q");
        if (options.query.empty()) {
            send_error(res, 400, "missing or empty query parameter 'q'");
            return;
        }
        if (req.has_param("case")) {
            options.case_sensitive = parse_bool(req.get_param_value("case"));
        }
        if (req.has_param("ext")) {
            options.extensions = split_list(req.get_param_value("ext"));
        }
        if (req.has_param("path")) {
            options.path_prefix = req.get_param_value("path");
            std::replace(options.path_prefix.begin(), options.path_prefix.end(), '\\', '/');
            while (options.path_prefix.starts_with("./")) {
                options.path_prefix.erase(0, 2);
            }
        }
        if (req.has_param("limit")) {
            const auto limit = parse_size(req.get_param_value("limit"));
            if (!limit || *limit == 0) {
                send_error(res, 400, "'limit' must be a positive integer");
                return;
            }
            options.max_results = std::min(*limit, kMaxLimit);
        }
        if (req.has_param("context")) {
            const auto context = parse_size(req.get_param_value("context"));
            if (!context) {
                send_error(res, 400, "'context' must be a non-negative integer");
                return;
            }
            options.context_lines = std::min(*context, kMaxContext);
        }
        send_json(res, 200, search_json(options, service.search(options)));
    }

    void handle_file(const httplib::Request& req, httplib::Response& res) const {
        const std::string path = req.get_param_value("path");
        if (path.empty()) {
            send_error(res, 400, "missing query parameter 'path'");
            return;
        }
        if (!normalize_repo_path(path)) {
            send_error(res, 400, "invalid path");
            return;
        }
        const std::optional<FileView> view = service.file(path);
        if (!view) {
            send_error(res, 404, "file not indexed: " + path);
            return;
        }
        send_json(res, 200, json{{"path", view->path}, {"language", view->language}, {"lines", view->lines}});
    }
};

HttpServer::HttpServer(SearchService& service, std::optional<std::filesystem::path> web_root)
    : impl_(std::make_unique<Impl>(service)) {
    httplib::Server& svr = impl_->server;
    Impl* impl = impl_.get();

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, json{{"status", "ok"}});
    });
    svr.Get("/api/search", [impl](const httplib::Request& req, httplib::Response& res) {
        impl->handle_search(req, res);
    });
    svr.Get("/api/file", [impl](const httplib::Request& req, httplib::Response& res) {
        impl->handle_file(req, res);
    });
    svr.Get("/api/stats", [impl](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, stats_json(impl->service.stats()));
    });
    svr.Post("/api/reindex", [impl](const httplib::Request&, httplib::Response& res) {
        const bool started = impl->service.reindex_async();
        send_json(res, 202, json{{"status", started ? "started" : "already_running"}});
    });

    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string message = "internal error";
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
        }
        send_error(res, 500, message);
    });

    if (web_root) {
        std::error_code ec;
        if (std::filesystem::is_directory(*web_root, ec)) {
            svr.set_mount_point("/", web_root->string());
        }
    }
}

HttpServer::~HttpServer() = default;

int HttpServer::bind(const std::string& host, int port) {
    if (port == 0) {
        return impl_->server.bind_to_any_port(host);
    }
    return impl_->server.bind_to_port(host, port) ? port : -1;
}

bool HttpServer::serve() {
    return impl_->server.listen_after_bind();
}

void HttpServer::stop() {
    impl_->server.stop();
}

void HttpServer::wait_until_ready() const {
    impl_->server.wait_until_ready();
}

}  // namespace codescope
