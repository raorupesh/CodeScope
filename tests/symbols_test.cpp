#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codescope/doc_store.hpp"
#include "codescope/symbols.hpp"

namespace codescope {
namespace {

using Found = std::vector<std::pair<std::string, std::string>>;  // {qualified, kind}

Found definitions(const std::string& code, const std::string& path = "x.cpp") {
    Found out;
    for (const Symbol& s : extract_symbols(path, code)) {
        out.emplace_back(s.qualified, s.kind);
    }
    return out;
}

bool has(const Found& found, const std::string& name) {
    for (const auto& [qualified, kind] : found) {
        if (qualified == name) {
            return true;
        }
    }
    return false;
}

TEST(Symbols, TypeDefinitions) {
    const auto found = definitions(
        "namespace net {\n"
        "class HttpRequest {\n"
        "};\n"
        "struct Point : Base {\n"
        "};\n"
        "enum class Color : int { Red };\n"
        "union Value\n"
        "{\n"
        "};\n"
        "template <typename T>\n"
        "class Box final {};\n"
        "class FMT_API Formatter {};\n"
        "}\n");
    EXPECT_EQ(found, (Found{{"net", "namespace"},
                            {"HttpRequest", "class"},
                            {"Point", "struct"},
                            {"Color", "enum"},
                            {"Value", "union"},
                            {"Box", "class"},
                            {"Formatter", "class"}}));
}

TEST(Symbols, ForwardDeclarationsAreNotDefinitions) {
    const auto found = definitions(
        "class Forward;\n"
        "struct Point p;\n"
        "void declared(int x);\n"
        "int pure() = 0;\n"
        "Foo() = default;\n");
    EXPECT_TRUE(found.empty());
}

TEST(Symbols, FunctionDefinitions) {
    const auto found = definitions(
        "int add(int a, int b) {\n"
        "    return a + b;\n"
        "}\n"
        "const std::string& HttpRequest::url() const {\n"
        "    return url_;\n"
        "}\n"
        "HttpRequest::HttpRequest(std::string url)\n"
        "    : url_(std::move(url)) {}\n"
        "HttpRequest::~HttpRequest() {}\n"
        "static inline void helper(\n"
        "    int a,\n"
        "    int b)\n"
        "{\n"
        "}\n"
        "template <typename T> T twice(T v) { return v + v; }\n"
        "auto lambda_free() -> int {\n"
        "    return 1;\n"
        "}\n"
        "static int\n"
        "gnu_style (int x)\n"
        "{\n"
        "}\n");
    EXPECT_TRUE(has(found, "add"));
    EXPECT_TRUE(has(found, "HttpRequest::url"));
    EXPECT_TRUE(has(found, "HttpRequest::HttpRequest"));
    EXPECT_TRUE(has(found, "HttpRequest::~HttpRequest"));
    EXPECT_TRUE(has(found, "helper"));
    EXPECT_TRUE(has(found, "twice"));
    EXPECT_TRUE(has(found, "lambda_free"));
    EXPECT_TRUE(has(found, "gnu_style"));
    for (const auto& [qualified, kind] : found) {
        EXPECT_EQ(kind, "function") << qualified;
    }
}

TEST(Symbols, CallsAndControlFlowAreIgnored) {
    const auto found = definitions(
        "void f() {\n"
        "    if (x) {\n"
        "    }\n"
        "    while (running()) {\n"
        "    }\n"
        "    for (int i = 0; i < n; ++i) {\n"
        "    }\n"
        "    call(1, 2);\n"
        "    std::sort(v.begin(), v.end());\n"
        "    auto g = [](int a) {\n"
        "    };\n"
        "    obj.method(3) ;\n"
        "    return compute(4);\n"
        "    Widget w(5);\n"
        "} \n"
        "TEST(Suite, Name) {\n"
        "}\n");
    EXPECT_EQ(found, (Found{{"f", "function"}}));
}

TEST(Symbols, MacrosAndAliases) {
    const auto found = definitions(
        "#define MAX_SIZE 100\n"
        "#  define SQUARE(x) ((x) * (x))\n"
        "using Callback = std::function<void()>;\n"
        "using namespace std;\n"
        "typedef unsigned long ulong;\n");
    EXPECT_EQ(found, (Found{{"MAX_SIZE", "macro"}, {"SQUARE", "macro"}, {"Callback", "alias"}, {"ulong", "alias"}}));
}

TEST(Symbols, CommentsAreIgnored) {
    const auto found = definitions(
        "// class Commented {\n"
        "/* struct Block {\n"
        "   int fake() {\n"
        "*/\n"
        "int real() { /* body */ }\n");
    EXPECT_EQ(found, (Found{{"real", "function"}}));
}

TEST(Symbols, LineNumbersAreOneBased) {
    const auto syms = extract_symbols("a.hpp", "\n\nstruct S {\n};\n");
    ASSERT_EQ(syms.size(), 1u);
    EXPECT_EQ(syms[0].line, 3u);
    EXPECT_EQ(syms[0].name, "S");
}

TEST(Symbols, OnlyCppFiles) {
    EXPECT_TRUE(definitions("class Foo {};", "notes.md").empty());
    EXPECT_TRUE(definitions("def foo():\n  pass\n", "x.py").empty());
    EXPECT_TRUE(is_cpp_path("a/b.HPP"));
    EXPECT_TRUE(is_cpp_path("c.h"));
    EXPECT_FALSE(is_cpp_path("CMakeLists.txt"));
}

TEST(SymbolTable, FindsByNameAndQualifiedSuffix) {
    DocStore docs;
    auto add = [&](const std::string& path, const std::string& code) {
        Document doc;
        doc.path = path;
        doc.content = std::make_shared<const std::string>(code);
        doc.symbols = std::make_shared<const std::vector<Symbol>>(extract_symbols(path, code));
        docs.add(std::move(doc));
    };
    add("a.cpp", "void Foo::run() {}\nvoid run() {}\n");
    add("b.cpp", "void Bar::run() {}\n");

    SymbolTable table;
    table.rebuild(docs);
    EXPECT_EQ(table.size(), 3u);
    EXPECT_EQ(table.find("run", docs).size(), 3u);
    EXPECT_EQ(table.find("Foo::run", docs).size(), 1u);
    // No whole-component match: falls back to every "run".
    EXPECT_EQ(table.find("oo::run", docs).size(), 3u);
    EXPECT_EQ(table.find("detail::run", docs).size(), 3u);
    EXPECT_EQ(table.find("missing", docs).size(), 0u);
}

}  // namespace
}  // namespace codescope
