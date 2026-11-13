// End-to-end tests of the REST API over a real localhost socket.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "codescope/http_server.hpp"
#include "codescope/search_service.hpp"
#include "test_helpers.hpp"

namespace codescope {
namespace {

using nlohmann::json;
using test_util::MemoryFileSource;

class HttpApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto source = std::make_unique<MemoryFileSource>();
        source->put("src/net/client.cpp", "#include \"http.hpp\"\nHttpRequest req{url};\nreq.send();\n");
        source->put("src/net/http.hpp", "struct HttpRequest {};\n");
        source->put("docs/latin1.txt", "caf\xE9 HttpRequest\n");  // invalid UTF-8 on purpose
        service = std::make_unique<SearchService>(std::move(source), ServiceConfig{});
        service->initialize();

        server = std::make_unique<HttpServer>(*service, std::nullopt);
        port = server->bind("127.0.0.1", 0);
        ASSERT_GT(port, 0);
        thread = std::thread([this] { server->serve(); });
        server->wait_until_ready();
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    // GET `path` with query parameters; returns {status, parsed body}.
    std::pair<int, json> get(const std::string& path, const httplib::Params& params = {}) {
        const auto res = client->Get(path, params, httplib::Headers{});
        if (!res) {
            ADD_FAILURE() << "request failed: " << httplib::to_string(res.error());
            return {0, json()};
        }
        EXPECT_EQ(res->get_header_value("Content-Type"), "application/json");
        return {res->status, json::parse(res->body)};
    }

    std::unique_ptr<SearchService> service;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
    std::thread thread;
    int port = 0;
};

TEST_F(HttpApiTest, Health) {
    const auto [status, body] = get("/health");
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body["status"], "ok");
}

TEST_F(HttpApiTest, SearchReturnsGroupedResults) {
    const auto [status, body] =
        get("/api/search", {{"q", "HttpRequest"}, {"case", "1"}, {"ext", ".cpp,.hpp"}, {"limit", "50"}});
    ASSERT_EQ(status, 200);
    EXPECT_EQ(body["query"], "HttpRequest");
    EXPECT_EQ(body["total_files"], 2);
    EXPECT_TRUE(body["took_ms"].is_number());
    const json first = body["results"][0];
    EXPECT_EQ(first["path"], "src/net/client.cpp");
    EXPECT_EQ(first["matches"][0]["line_number"], 2);
    EXPECT_EQ(first["matches"][0]["line"], "HttpRequest req{url};");
    EXPECT_EQ(first["matches"][0]["ranges"], json::parse("[[0, 11]]"));
    EXPECT_EQ(first["matches"][0]["before"], json::parse(R"(["#include \"http.hpp\""])"));
}

TEST_F(HttpApiTest, SearchSurvivesInvalidUtf8) {
    const auto [status, body] = get("/api/search", {{"q", "HttpRequest"}, {"path", "docs/"}});
    ASSERT_EQ(status, 200);
    EXPECT_EQ(body["total_files"], 1);
}

TEST_F(HttpApiTest, SearchValidatesParameters) {
    EXPECT_EQ(get("/api/search").first, 400);
    EXPECT_EQ(get("/api/search", {{"q", ""}}).first, 400);
    const auto [status, body] = get("/api/search", {{"q", "x"}, {"limit", "abc"}});
    EXPECT_EQ(status, 400);
    EXPECT_TRUE(body.contains("error"));
    EXPECT_EQ(get("/api/search", {{"q", "x"}, {"limit", "0"}}).first, 400);
    EXPECT_EQ(get("/api/search", {{"q", "x"}, {"context", "-1"}}).first, 400);
}

TEST_F(HttpApiTest, FileReturnsLines) {
    const auto [status, body] = get("/api/file", {{"path", "src/net/http.hpp"}});
    ASSERT_EQ(status, 200);
    EXPECT_EQ(body["language"], "cpp");
    EXPECT_EQ(body["lines"], json::parse(R"(["struct HttpRequest {};"])"));
}

TEST_F(HttpApiTest, FileRejectsPathTraversal) {
    EXPECT_EQ(get("/api/file", {{"path", "../../etc/passwd"}}).first, 400);
    EXPECT_EQ(get("/api/file", {{"path", "/etc/passwd"}}).first, 400);
    EXPECT_EQ(get("/api/file", {{"path", "src/../../etc/passwd"}}).first, 400);
    EXPECT_EQ(get("/api/file").first, 400);
}

TEST_F(HttpApiTest, FileUnknownIs404) {
    EXPECT_EQ(get("/api/file", {{"path", "src/nope.cpp"}}).first, 404);
}

TEST_F(HttpApiTest, StatsAndReindex) {
    const auto [status, body] = get("/api/stats");
    ASSERT_EQ(status, 200);
    EXPECT_EQ(body["docs"], 3);
    EXPECT_TRUE(body["skipped"].contains("binary"));

    const auto res = client->Post("/api/reindex");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202);
    const json started = json::parse(res->body);
    EXPECT_TRUE(started["status"] == "started" || started["status"] == "already_running");
}

}  // namespace
}  // namespace codescope
