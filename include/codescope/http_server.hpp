#pragma once

// REST API + static web UI on top of SearchService (cpp-httplib).
//
//   GET  /health          -> {status}
//   GET  /api/search      q, case, ext, path, limit, context
//   GET  /api/file        path
//   GET  /api/stats
//   POST /api/reindex     -> 202, runs in the background
//
// httplib stays out of this header so only http_server.cpp depends on it.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "codescope/search_service.hpp"

namespace codescope {

class HttpServer {
public:
    // `web_root`: directory served at "/" (index.html, app.js, ...), if any.
    HttpServer(SearchService& service, std::optional<std::filesystem::path> web_root);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds the listening socket. Port 0 picks a free port. Returns the
    // bound port, or -1 on failure.
    int bind(const std::string& host, int port);

    // Serves requests until stop() is called. Requires a successful bind().
    bool serve();

    // Thread-safe; makes serve() return.
    void stop();

    // Blocks until serve() is accepting connections.
    void wait_until_ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace codescope
