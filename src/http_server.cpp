#include "codescope/http_server.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <iostream>
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
constexpr std::size_t kMaxBodyBytes = 64 * 1024;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// "Host: example.com:8080" -> "example.com"; "[::1]:8080" -> "[::1]".
std::string host_name(std::string_view host) {
    if (host.starts_with('[')) {
        const std::size_t close = host.find(']');
        return lower(close == std::string_view::npos ? host : host.substr(0, close + 1));
    }
    return lower(host.substr(0, host.find(':')));
}

// "http://example.com:8080" -> "example.com:8080".
std::string origin_authority(std::string_view origin) {
    const std::size_t scheme = origin.find("://");
    return lower(scheme == std::string_view::npos ? origin : origin.substr(scheme + 3));
}

void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    // Source files may not be valid UTF-8: replace bad bytes rather than throw.
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
        {"symbols", s.symbols},
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
        {"last_error", s.last_error.empty() ? "" : "see the server log"},  // details may contain paths
    };
}

}  // namespace

struct HttpServer::Impl {
    SearchService& service;
    httplib::Server server;
    std::vector<std::string> allowed_hosts;  // empty or "*" entry: any host

    Impl(SearchService& svc, std::vector<std::string> hosts) : service(svc), allowed_hosts(std::move(hosts)) {}

    // Blocks DNS rebinding (an attacker's domain resolving to 127.0.0.1).
    bool host_allowed(const httplib::Request& req) const {
        if (std::find(allowed_hosts.begin(), allowed_hosts.end(), "*") != allowed_hosts.end()) {
            return true;
        }
        if (!req.has_header("Host")) {
            return false;
        }
        const std::string name = host_name(req.get_header_value("Host"));
        return std::any_of(allowed_hosts.begin(), allowed_hosts.end(),
                           [&name](const std::string& allowed) { return lower(allowed) == name; });
    }

    // Blocks cross-site POSTs: Origin, when sent, must match Host.
    static bool origin_allowed(const httplib::Request& req) {
        if (req.method != "POST" || !req.has_header("Origin")) {
            return true;
        }
        return origin_authority(req.get_header_value("Origin")) == lower(req.get_header_value("Host"));
    }

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

    void handle_definition(const httplib::Request& req, httplib::Response& res) const {
        const std::string symbol = req.get_param_value("symbol");
        const bool valid = !symbol.empty() && symbol.size() <= 200 &&
                           std::all_of(symbol.begin(), symbol.end(), [](char c) {
                               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                      c == '_' || c == ':' || c == '~';
                           });
        if (!valid) {
            send_error(res, 400, "'symbol' must be a C/C++ identifier, optionally qualified (a::b)");
            return;
        }
        json defs = json::array();
        for (const Definition& d : service.definitions(symbol)) {
            defs.push_back(json{{"path", d.path}, {"line", d.line}, {"kind", d.kind}, {"name", d.qualified}, {"text", d.text}});
        }
        send_json(res, 200, json{{"symbol", symbol}, {"definitions", std::move(defs)}});
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
            send_error(res, 404, "file not indexed");
            return;
        }
        send_json(res, 200, json{{"path", view->path}, {"language", view->language}, {"lines", view->lines}});
    }
};

HttpServer::HttpServer(SearchService& service, std::optional<std::filesystem::path> web_root,
                       std::vector<std::string> extra_hosts)
    : impl_(nullptr) {
    std::vector<std::string> hosts{"localhost", "127.0.0.1", "[::1]"};
    hosts.insert(hosts.end(), extra_hosts.begin(), extra_hosts.end());
    impl_ = std::make_unique<Impl>(service, std::move(hosts));
    httplib::Server& svr = impl_->server;
    Impl* impl = impl_.get();

    svr.set_payload_max_length(kMaxBodyBytes);
    svr.set_default_headers({
        {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"},
        {"Referrer-Policy", "no-referrer"},
        {"Cross-Origin-Resource-Policy", "same-origin"},
        {"Content-Security-Policy",
         "default-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; "
         "form-action 'self'; frame-ancestors 'none'"},
    });
    svr.set_pre_routing_handler([impl](const httplib::Request& req, httplib::Response& res) {
        if (!impl->host_allowed(req)) {
            send_error(res, 403, "host not allowed; start the server with --allow-host to permit it");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!Impl::origin_allowed(req)) {
            send_error(res, 403, "cross-origin request rejected");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, json{{"status", "ok"}});
    });
    svr.Get("/api/search", [impl](const httplib::Request& req, httplib::Response& res) {
        impl->handle_search(req, res);
    });
    svr.Get("/api/file", [impl](const httplib::Request& req, httplib::Response& res) {
        impl->handle_file(req, res);
    });
    svr.Get("/api/definition", [impl](const httplib::Request& req, httplib::Response& res) {
        impl->handle_definition(req, res);
    });
    svr.Get("/api/stats", [impl](const httplib::Request&, httplib::Response& res) {
        send_json(res, 200, stats_json(impl->service.stats()));
    });
    svr.Post("/api/reindex", [impl](const httplib::Request&, httplib::Response& res) {
        const bool started = impl->service.reindex_async();
        send_json(res, 202, json{{"status", started ? "started" : "already_running"}});
    });

    // Details are logged, not returned: they can contain local paths.
    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception& e) {
            std::cerr << "codescope: error handling " << req.path << ": " << e.what() << '\n';
        } catch (...) {
            std::cerr << "codescope: unknown error handling " << req.path << '\n';
        }
        send_error(res, 500, "internal error");
    });

    if (web_root) {
        std::error_code ec;
        if (std::filesystem::is_directory(*web_root, ec)) {
            svr.set_mount_point("/", to_utf8(*web_root));
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
