// REST API end to end over a localhost socket.

#include <gtest/gtest.h>

#include <fstream>
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

    // Returns {status, parsed body}.
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
    // No raw string literal: some MSVC versions mis-parse them inside macros.
    const json expected_before = json::array({"#include \"http.hpp\""});
    EXPECT_EQ(first["matches"][0]["before"], expected_before);
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
    const json expected_lines = json::array({"struct HttpRequest {};"});
    EXPECT_EQ(body["lines"], expected_lines);
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

TEST_F(HttpApiTest, DefinitionFindsCppSymbols) {
    const auto [status, body] = get("/api/definition", {{"symbol", "HttpRequest"}});
    ASSERT_EQ(status, 200);
    ASSERT_EQ(body["definitions"].size(), 1u);
    const json def = body["definitions"][0];
    EXPECT_EQ(def["path"], "src/net/http.hpp");
    EXPECT_EQ(def["line"], 1);
    EXPECT_EQ(def["kind"], "struct");
    EXPECT_EQ(def["text"], "struct HttpRequest {};");

    EXPECT_EQ(get("/api/definition", {{"symbol", "Nope"}}).second["definitions"].size(), 0u);
}

TEST_F(HttpApiTest, DefinitionValidatesSymbol) {
    EXPECT_EQ(get("/api/definition").first, 400);
    EXPECT_EQ(get("/api/definition", {{"symbol", "a b"}}).first, 400);
    EXPECT_EQ(get("/api/definition", {{"symbol", "x<script>"}}).first, 400);
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

TEST_F(HttpApiTest, ForeignHostHeaderIsRejected) {
    // DNS rebinding: the browser sends the attacker's host name.
    const auto res = client->Get("/api/stats", httplib::Headers{{"Host", "evil.example:" + std::to_string(port)}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);

    const auto ok = client->Get("/api/stats", httplib::Headers{{"Host", "localhost:" + std::to_string(port)}});
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
}

TEST_F(HttpApiTest, CrossOriginPostIsRejected) {
    const auto evil = client->Post("/api/reindex", httplib::Headers{{"Origin", "http://evil.example"}}, "", "text/plain");
    ASSERT_TRUE(evil);
    EXPECT_EQ(evil->status, 403);

    const std::string self = "http://127.0.0.1:" + std::to_string(port);
    const auto same = client->Post("/api/reindex", httplib::Headers{{"Origin", self}}, "", "text/plain");
    ASSERT_TRUE(same);
    EXPECT_EQ(same->status, 202);
}

TEST_F(HttpApiTest, OversizedBodyIsRejected) {
    const auto res = client->Post("/api/reindex", std::string(200 * 1024, 'x'), "text/plain");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 413);
}

TEST(HttpApiErrors, StatsDoNotExposeErrorDetails) {
    test_util::TempDir dir;
    ServiceConfig config;
    config.index_file = dir.path() / "index.bin";
    {
        std::ofstream out(*config.index_file, std::ios::binary);
        out << "corrupt";
    }
    auto source = std::make_unique<MemoryFileSource>();
    source->put("a.cpp", "int a;\n");
    SearchService service(std::move(source), config);
    service.initialize();
    ASSERT_FALSE(service.stats().last_error.empty());

    HttpServer server(service, std::nullopt);
    const int port = server.bind("127.0.0.1", 0);
    ASSERT_GT(port, 0);
    std::thread thread([&server] { server.serve(); });
    server.wait_until_ready();
    httplib::Client client("127.0.0.1", port);
    const auto res = client.Get("/api/stats");
    server.stop();
    thread.join();

    ASSERT_TRUE(res);
    EXPECT_EQ(json::parse(res->body)["last_error"], "see the server log");
}

TEST_F(HttpApiTest, SecurityHeadersAreSet) {
    const auto res = client->Get("/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->get_header_value("X-Content-Type-Options"), "nosniff");
    EXPECT_NE(res->get_header_value("Content-Security-Policy").find("frame-ancestors 'none'"), std::string::npos);
    EXPECT_EQ(res->get_header_value("X-Frame-Options"), "DENY");
}

}  // namespace
}  // namespace codescope
