#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codescope/doc_store.hpp"
#include "codescope/query_engine.hpp"
#include "codescope/trigram_index.hpp"
#include "test_helpers.hpp"

namespace codescope {
namespace {

using Range = std::pair<std::uint32_t, std::uint32_t>;
using test_util::result_paths;

class QueryEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        add("docs/readme.md", "Use HttpRequest to send.\r\nAnother line\r\n");
        add("src/fp.cpp", "abcd xcde\n");
        add("src/net/client.cpp",
            "#include \"http.hpp\"\n"
            "HttpRequest req{url};\n"
            "auto r = req.send();\n"
            "// httprequest lower\n");
        add("src/net/http.hpp", "struct HttpRequest {\n  int x;\n};\n");
        add("src/repeat.txt", "aaa aaa\n");
    }

    DocId add(const std::string& path, const std::string& content) {
        Document doc;
        doc.path = path;
        doc.size = content.size();
        doc.content = std::make_shared<const std::string>(content);
        const DocId id = docs.add(std::move(doc));
        index.add(id, content);
        return id;
    }

    SearchResponse run(SearchOptions options) const { return QueryEngine(index, docs).search(options); }

    static SearchOptions query(std::string q) {
        SearchOptions o;
        o.query = std::move(q);
        o.context_lines = 0;
        return o;
    }

    const FileResult* find(const SearchResponse& r, const std::string& path) const {
        for (const FileResult& f : r.results) {
            if (f.path == path) {
                return &f;
            }
        }
        return nullptr;
    }

    DocStore docs;
    TrigramIndex index;
};

TEST_F(QueryEngineTest, CaseInsensitiveByDefault) {
    const auto r = run(query("httprequest"));
    EXPECT_EQ(result_paths(r),
              (std::vector<std::string>{"docs/readme.md", "src/net/client.cpp", "src/net/http.hpp"}));
    const FileResult* client = find(r, "src/net/client.cpp");
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(client->matches.size(), 2u);
    EXPECT_EQ(client->matches[0].line_number, 2u);
    EXPECT_EQ(client->matches[1].line_number, 4u);
    EXPECT_TRUE(r.used_index);
}

TEST_F(QueryEngineTest, CaseSensitiveExcludesOtherCasing) {
    auto o = query("HttpRequest");
    o.case_sensitive = true;
    const auto r = run(o);
    const FileResult* client = find(r, "src/net/client.cpp");
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(client->matches.size(), 1u);
    EXPECT_EQ(client->matches[0].line_number, 2u);
    EXPECT_EQ(r.total_matches, 3u);  // readme, client, http.hpp
}

TEST_F(QueryEngineTest, RangesAreByteOffsetsWithinTheLine) {
    const auto r = run(query("req"));
    const FileResult* client = find(r, "src/net/client.cpp");
    ASSERT_NE(client, nullptr);
    // Line 2: "HttpRequest req{url};" -> "Req" at 4, "req" at 12.
    ASSERT_GE(client->matches.size(), 1u);
    const LineMatch& m = client->matches[0];
    EXPECT_EQ(m.line, "HttpRequest req{url};");
    EXPECT_EQ(m.ranges, (std::vector<Range>{{4, 7}, {12, 15}}));
}

TEST_F(QueryEngineTest, MultipleMatchesOnOneLineAreMerged) {
    const auto r = run(query("aaa"));
    ASSERT_EQ(r.results.size(), 1u);
    ASSERT_EQ(r.results[0].matches.size(), 1u);
    EXPECT_EQ(r.results[0].matches[0].ranges, (std::vector<Range>{{0, 3}, {4, 7}}));
    EXPECT_EQ(r.total_matches, 1u);
}

TEST_F(QueryEngineTest, ContextLines) {
    auto o = query("req.send");
    o.context_lines = 1;
    const auto r = run(o);
    ASSERT_EQ(r.results.size(), 1u);
    const LineMatch& m = r.results[0].matches.at(0);
    EXPECT_EQ(m.line_number, 3u);
    EXPECT_EQ(m.before, std::vector<std::string>{"HttpRequest req{url};"});
    EXPECT_EQ(m.after, std::vector<std::string>{"// httprequest lower"});
}

TEST_F(QueryEngineTest, ContextStopsAtFileBoundaries) {
    auto o = query("struct HttpRequest");
    o.context_lines = 3;
    const auto r = run(o);
    ASSERT_EQ(r.results.size(), 1u);
    const LineMatch& m = r.results[0].matches.at(0);
    EXPECT_EQ(m.line_number, 1u);
    EXPECT_TRUE(m.before.empty());
    EXPECT_EQ(m.after, (std::vector<std::string>{"  int x;", "};"}));
}

TEST_F(QueryEngineTest, CarriageReturnsAreStripped) {
    const auto r = run(query("to send."));
    ASSERT_EQ(r.results.size(), 1u);
    EXPECT_EQ(r.results[0].matches.at(0).line, "Use HttpRequest to send.");
}

TEST_F(QueryEngineTest, ExtensionFilterNormalizesInput) {
    for (const char* ext : {".hpp", "hpp", ".HPP"}) {
        auto o = query("HttpRequest");
        o.extensions = {ext};
        EXPECT_EQ(result_paths(run(o)), std::vector<std::string>{"src/net/http.hpp"}) << ext;
    }
}

TEST_F(QueryEngineTest, PathPrefixFilter) {
    auto o = query("HttpRequest");
    o.path_prefix = "src/";
    EXPECT_EQ(result_paths(run(o)), (std::vector<std::string>{"src/net/client.cpp", "src/net/http.hpp"}));
}

TEST_F(QueryEngineTest, MaxResultsCapsMatchingLines) {
    auto o = query("httprequest");
    o.max_results = 2;
    const auto r = run(o);
    EXPECT_EQ(r.total_matches, 2u);
    EXPECT_TRUE(r.truncated);

    o.max_results = 100;
    EXPECT_FALSE(run(o).truncated);
}

TEST_F(QueryEngineTest, ShortQueryFallsBackToFullScan) {
    const auto r = run(query("Ht"));
    EXPECT_FALSE(r.used_index);
    EXPECT_EQ(r.candidates, docs.live_count());
    EXPECT_FALSE(r.results.empty());
}

TEST_F(QueryEngineTest, VerificationRejectsFalsePositives) {
    const auto r = run(query("abcde"));
    EXPECT_EQ(r.candidates, 1u);  // src/fp.cpp has every trigram...
    EXPECT_TRUE(r.results.empty());  // ...but not the string
}

TEST_F(QueryEngineTest, EmptyQueryReturnsNothing) {
    const auto r = run(query(""));
    EXPECT_TRUE(r.results.empty());
    EXPECT_EQ(r.total_matches, 0u);
}

TEST_F(QueryEngineTest, RemovedDocumentsAreNotReturned) {
    const DocId id = *docs.find("src/net/http.hpp");
    index.remove(id);
    docs.remove(id);
    EXPECT_EQ(find(run(query("HttpRequest")), "src/net/http.hpp"), nullptr);
}

TEST_F(QueryEngineTest, MatchAtEndOfFileWithoutNewline) {
    add("tail.txt", "first\nlast needle");
    const auto r = run(query("needle"));
    const FileResult* f = find(r, "tail.txt");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->matches.at(0).line_number, 2u);
    EXPECT_EQ(f->matches.at(0).ranges, (std::vector<Range>{{5, 11}}));
}

}  // namespace
}  // namespace codescope
