#pragma once

// REST API and static web UI over SearchService (cpp-httplib):
//   GET /health, /api/search, /api/file, /api/definition, /api/stats; POST /api/reindex

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codescope/search_service.hpp"

namespace codescope {

class HttpServer {
public:
    // Requests are accepted only for localhost, 127.0.0.1, [::1] and
    // `extra_hosts` ("*" accepts any Host header).
    HttpServer(SearchService& service, std::optional<std::filesystem::path> web_root,
               std::vector<std::string> extra_hosts = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Port 0 picks a free port. Returns the bound port, or -1 on failure.
    int bind(const std::string& host, int port);

    // Blocks until stop() is called.
    bool serve();

    void stop();

    void wait_until_ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace codescope
