#pragma once

// Go-to-definition for C/C++: a line-based heuristic (not a parser) that finds
// definitions of types, namespaces, macros, aliases and functions.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codescope/types.hpp"

namespace codescope {

class DocStore;

struct Symbol {
    std::string name;       // unqualified
    std::string qualified;  // as written, e.g. "HttpRequest::url"
    std::string kind;       // class, struct, union, enum, namespace, macro, alias, function
    std::uint32_t line = 0; // 1-based
};

bool is_cpp_path(std::string_view path);

// Empty for non-C/C++ files.
std::vector<Symbol> extract_symbols(std::string_view path, std::string_view content);

class SymbolTable {
public:
    struct Location {
        DocId doc = 0;
        std::uint32_t index = 0;  // into the document's symbols
    };

    void rebuild(const DocStore& docs);

    // "Foo::bar" prefers qualified matches, else falls back to every "bar".
    std::vector<Location> find(std::string_view name, const DocStore& docs) const;

    std::size_t size() const { return count_; }

private:
    std::unordered_map<std::string, std::vector<Location>> by_name_;
    std::size_t count_ = 0;
};

}  // namespace codescope
