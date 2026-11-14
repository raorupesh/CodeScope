#include "codescope/symbols.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "codescope/doc_store.hpp"
#include "codescope/path_utils.hpp"

namespace codescope {
namespace {

constexpr std::size_t kMaxLineLength = 400;  // skip generated/minified lines
constexpr std::size_t kSignatureLookahead = 8;

bool ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool ident_char(char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool starts_with_word(std::string_view s, std::string_view word) {
    return s.starts_with(word) && (s.size() == word.size() || !ident_char(s[word.size()]));
}

// Identifier at the start of `s` (empty if none).
std::string_view leading_identifier(std::string_view s) {
    std::size_t n = 0;
    if (n < s.size() && ident_start(s[n])) {
        while (n < s.size() && ident_char(s[n])) {
            ++n;
        }
    }
    return s.substr(0, n);
}

// Possibly qualified name ("a::b::c") at the start of `s`.
std::string_view leading_qualified_name(std::string_view s) {
    std::size_t n = 0;
    while (true) {
        const std::string_view id = leading_identifier(s.substr(n));
        if (id.empty()) {
            break;
        }
        n += id.size();
        if (s.substr(n).starts_with("::") && n + 2 < s.size() && ident_start(s[n + 2])) {
            n += 2;
            continue;
        }
        break;
    }
    return s.substr(0, n);
}

std::string_view last_component(std::string_view qualified) {
    const std::size_t pos = qualified.rfind("::");
    return pos == std::string_view::npos ? qualified : qualified.substr(pos + 2);
}

bool is_all_caps_macro(std::string_view id) {
    if (id.size() < 2) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}

// Skips a balanced (...) or [[...]] group at the start of `s`.
std::string_view skip_group(std::string_view s, char open, char close) {
    if (s.empty() || s.front() != open) {
        return s;
    }
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == open) {
            ++depth;
        } else if (s[i] == close && --depth == 0) {
            return trim(s.substr(i + 1));
        }
    }
    return {};
}

// Drops leading template<...>, attributes and specifiers (inline, static, ...).
std::string_view strip_leading_noise(std::string_view t) {
    for (bool changed = true; changed;) {
        changed = false;
        if (starts_with_word(t, "template")) {
            std::string_view rest = trim(t.substr(8));
            if (!rest.empty() && rest.front() == '<') {
                int depth = 0;
                std::size_t i = 0;
                for (; i < rest.size(); ++i) {
                    if (rest[i] == '<') {
                        ++depth;
                    } else if (rest[i] == '>' && --depth == 0) {
                        break;
                    }
                }
                t = i < rest.size() ? trim(rest.substr(i + 1)) : std::string_view{};
                changed = true;
            }
        }
        if (t.starts_with("[[")) {
            t = skip_group(t, '[', ']');
            changed = true;
        }
        for (const std::string_view word : {"export", "extern \"C\"", "inline", "static", "constexpr", "consteval",
                                            "virtual", "explicit", "friend", "typedef"}) {
            if (starts_with_word(t, word) || (word.back() == '"' && t.starts_with(word))) {
                t = trim(t.substr(word.size()));
                changed = true;
            }
        }
    }
    return t;
}

// Strips comments; `in_block` carries /* */ state across lines.
std::string strip_comments(std::string_view line, bool& in_block) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (in_block) {
            if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                in_block = false;
                ++i;
            }
            continue;
        }
        if (line[i] == '/' && i + 1 < line.size()) {
            if (line[i + 1] == '/') {
                break;
            }
            if (line[i + 1] == '*') {
                in_block = true;
                ++i;
                continue;
            }
        }
        out += line[i];
    }
    return out;
}

bool is_reserved_call(std::string_view name) {
    static constexpr std::array<std::string_view, 34> kWords{
        "if", "for", "while", "switch", "return", "sizeof", "catch", "decltype", "alignof", "alignas",
        "static_assert", "defined", "new", "delete", "throw", "typeid", "noexcept", "requires", "co_return",
        "co_await", "co_yield", "else", "do", "case", "default", "goto", "operator", "assert", "__attribute__",
        "__declspec", "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast"};
    return std::find(kWords.begin(), kWords.end(), name) != kWords.end();
}

// A line holding only a return type ("static int"), as in GNU style.
bool looks_like_type_line(std::string_view prev) {
    if (prev.empty() || prev.front() == '#') {
        return false;
    }
    const char last = prev.back();
    if (!(ident_char(last) || last == '*' || last == '&' || last == '>')) {
        return false;
    }
    return std::all_of(prev.begin(), prev.end(), [](char c) {
        return ident_char(c) || c == ' ' || c == '\t' || c == '*' || c == '&' || c == '<' || c == '>' || c == ':' || c == ',';
    }) && !starts_with_word(prev, "return") && !starts_with_word(prev, "else");
}

class Extractor {
public:
    explicit Extractor(std::string_view content) {
        bool in_block = false;
        std::size_t start = 0;
        while (start <= content.size()) {
            std::size_t end = content.find('\n', start);
            if (end == std::string_view::npos) {
                end = content.size();
            }
            const std::string_view raw = content.substr(start, end - start);
            if (raw.size() > kMaxLineLength) {
                lines_.emplace_back();
            } else {
                lines_.push_back(strip_comments(raw, in_block));
            }
            if (end == content.size()) {
                break;
            }
            start = end + 1;
        }
    }

    std::vector<Symbol> run() {
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            const std::string_view t = trim(lines_[i]);
            if (t.empty()) {
                continue;
            }
            if (t.front() == '#') {
                macro(t, i);
            } else if (!type_definition(t, i) && !alias(t, i)) {
                function_definition(t, i);
            }
        }
        return std::move(symbols_);
    }

private:
    void add(std::string_view qualified, std::string_view kind, std::size_t i) {
        Symbol s;
        s.qualified = std::string(qualified);
        s.name = std::string(last_component(qualified));
        s.kind = std::string(kind);
        s.line = static_cast<std::uint32_t>(i + 1);
        symbols_.push_back(std::move(s));
    }

    std::string_view next_nonempty(std::size_t i) const {
        for (std::size_t j = i + 1; j < lines_.size() && j <= i + 3; ++j) {
            const std::string_view t = trim(lines_[j]);
            if (!t.empty()) {
                return t;
            }
        }
        return {};
    }

    std::string_view prev_nonempty(std::size_t i) const {
        for (std::size_t j = i; j-- > 0 && j + 3 >= i;) {
            const std::string_view t = trim(lines_[j]);
            if (!t.empty()) {
                return t;
            }
        }
        return {};
    }

    void macro(std::string_view t, std::size_t i) {
        std::string_view rest = trim(t.substr(1));
        if (!starts_with_word(rest, "define")) {
            return;
        }
        rest = trim(rest.substr(6));
        const std::string_view name = leading_identifier(rest);
        if (!name.empty()) {
            add(name, "macro", i);
        }
    }

    bool type_definition(std::string_view t, std::size_t i) {
        t = strip_leading_noise(t);
        std::string_view kind;
        for (const std::string_view kw : {"class", "struct", "union", "enum", "namespace"}) {
            if (starts_with_word(t, kw)) {
                kind = kw;
                break;
            }
        }
        if (kind.empty()) {
            return false;
        }
        std::string_view rest = trim(t.substr(kind.size()));
        if (kind == "enum" && (starts_with_word(rest, "class") || starts_with_word(rest, "struct"))) {
            rest = trim(rest.substr(starts_with_word(rest, "class") ? 5 : 6));
        }
        // Skip attributes and export macros: [[deprecated]] class FMT_API name
        for (bool changed = true; changed;) {
            changed = false;
            if (rest.starts_with("[[")) {
                rest = skip_group(rest, '[', ']');
                changed = true;
            }
            for (const std::string_view attr : {"alignas", "__declspec", "__attribute__"}) {
                if (starts_with_word(rest, attr)) {
                    rest = skip_group(trim(rest.substr(attr.size())), '(', ')');
                    changed = true;
                }
            }
            const std::string_view id = leading_identifier(rest);
            const std::string_view after_id = trim(rest.substr(id.size()));
            if (is_all_caps_macro(id) && !leading_identifier(after_id).empty()) {
                rest = after_id;
                changed = true;
            }
        }
        const std::string_view name = leading_qualified_name(rest);
        if (name.empty()) {
            return true;  // anonymous namespace/struct: nothing to record
        }
        std::string_view after = trim(rest.substr(name.size()));
        if (starts_with_word(after, "final")) {
            after = trim(after.substr(5));
        }
        bool definition = false;
        if (!after.empty()) {
            definition = after.front() == '{' || (after.front() == ':' && !after.starts_with("::"));
        } else {
            const std::string_view next = next_nonempty(i);
            definition = !next.empty() && (next.front() == '{' || next.front() == ':');
        }
        if (definition) {
            add(name, kind, i);
        }
        // "struct Foo* make() {" may still be a function.
        return definition;
    }

    bool alias(std::string_view t, std::size_t i) {
        if (starts_with_word(t, "using")) {
            std::string_view rest = trim(t.substr(5));
            const std::string_view name = leading_identifier(rest);
            if (!name.empty() && name != "namespace" && trim(rest.substr(name.size())).starts_with('=')) {
                add(name, "alias", i);
            }
            return true;
        }
        if (starts_with_word(t, "typedef")) {
            if (t.find('(') == std::string_view::npos && !t.empty() && t.back() == ';') {
                std::string_view body = trim(t.substr(0, t.size() - 1));
                std::size_t end = body.size();
                std::size_t begin = end;
                while (begin > 0 && ident_char(body[begin - 1])) {
                    --begin;
                }
                if (begin < end) {
                    add(body.substr(begin, end - begin), "alias", i);
                }
            }
            return true;
        }
        return false;
    }

    void function_definition(std::string_view line, std::size_t i) {
        const std::string_view t = strip_leading_noise(line);
        const std::size_t paren = t.find('(');
        if (paren == std::string_view::npos || paren == 0) {
            return;
        }
        // Name before '(': ident, a::b or ~Foo.
        std::size_t end = paren;
        while (end > 0 && (t[end - 1] == ' ' || t[end - 1] == '\t')) {
            --end;
        }
        std::size_t begin = end;
        while (begin > 0 && (ident_char(t[begin - 1]) || t[begin - 1] == ':' || t[begin - 1] == '~')) {
            --begin;
        }
        std::string_view qualified = t.substr(begin, end - begin);
        while (qualified.starts_with(':')) {
            qualified.remove_prefix(1);  // "::foo" (global qualifier)
            ++begin;
        }
        const std::string_view name = last_component(qualified);
        const std::string_view bare = name.starts_with('~') ? name.substr(1) : name;
        if (bare.empty() || !ident_start(bare.front()) || is_reserved_call(bare) ||
            qualified.find(":::") != std::string_view::npos) {
            return;
        }
        if (begin >= 8 && t.substr(0, begin).ends_with("operator")) {
            return;
        }

        // The prefix must look like a return type.
        const std::string_view prefix = trim(t.substr(0, begin));
        if (!prefix.empty()) {
            if (prefix.find_first_of("=;\"'(){},") != std::string_view::npos || prefix.ends_with('.') ||
                prefix.ends_with("->")) {
                return;
            }
            const char last = prefix.back();
            if (!(ident_char(last) || last == '*' || last == '&' || last == '>')) {
                return;
            }
            for (const std::string_view word : {"return", "else", "case", "goto", "throw", "delete", "new", "co_return", "using"}) {
                if (starts_with_word(prefix, word)) {
                    return;
                }
            }
        } else if (qualified.find("::") == std::string_view::npos && !looks_like_type_line(prev_nonempty(i))) {
            return;  // a bare call statement such as "foo(1);"
        }

        // Find the matching ')' (maybe on a later line), then look for a body.
        int depth = 0;
        std::size_t line_index = i;
        std::string_view cur = t.substr(paren);
        std::optional<std::string_view> after;
        for (std::size_t steps = 0; steps <= kSignatureLookahead && !after; ++steps) {
            for (std::size_t k = 0; k < cur.size(); ++k) {
                if (cur[k] == '(') {
                    ++depth;
                } else if (cur[k] == ')' && --depth == 0) {
                    after = trim(cur.substr(k + 1));
                    break;
                }
            }
            if (!after) {
                if (++line_index >= lines_.size()) {
                    return;
                }
                cur = lines_[line_index];
            }
        }
        if (!after || !has_body(*after, line_index)) {
            return;
        }
        add(qualified, "function", i);
    }

    // '{' or a ctor ':' means a definition; ';' or '=' means a declaration.
    bool has_body(std::string_view after, std::size_t line_index) const {
        const std::size_t pos = after.find_first_of("{;=:");
        if (pos != std::string_view::npos) {
            const char c = after[pos];
            if (c == ':') {
                return !after.substr(pos).starts_with("::");  // ctor init list
            }
            return c == '{';
        }
        if (after.find_first_of("()") != std::string_view::npos) {
            return false;  // e.g. a macro invocation followed by more calls
        }
        const std::string_view next = next_nonempty(line_index);
        return !next.empty() && (next.front() == '{' || (next.front() == ':' && !next.starts_with("::")));
    }

    std::vector<std::string> lines_;
    std::vector<Symbol> symbols_;
};

}  // namespace

bool is_cpp_path(std::string_view path) {
    static constexpr std::array<std::string_view, 13> kExtensions{
        ".c", ".h", ".cc", ".cpp", ".cxx", ".c++", ".hh", ".hpp", ".hxx", ".h++", ".ipp", ".inl", ".tpp"};
    const std::string ext = extension_of(path);
    return std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end();
}

std::vector<Symbol> extract_symbols(std::string_view path, std::string_view content) {
    if (!is_cpp_path(path)) {
        return {};
    }
    return Extractor(content).run();
}

void SymbolTable::rebuild(const DocStore& docs) {
    by_name_.clear();
    count_ = 0;
    for (const DocId id : docs.live_ids()) {
        const Document* doc = docs.get(id);
        if (doc == nullptr || !doc->symbols) {
            continue;
        }
        for (std::size_t k = 0; k < doc->symbols->size(); ++k) {
            by_name_[(*doc->symbols)[k].name].push_back(Location{id, static_cast<std::uint32_t>(k)});
            ++count_;
        }
    }
}

std::vector<SymbolTable::Location> SymbolTable::find(std::string_view name, const DocStore& docs) const {
    std::vector<Location> out;
    const auto it = by_name_.find(std::string(last_component(name)));
    if (it == by_name_.end()) {
        return out;
    }
    const bool qualified_query = name.find("::") != std::string_view::npos;
    if (!qualified_query) {
        return it->second;
    }
    for (const Location& loc : it->second) {
        const Document* doc = docs.get(loc.doc);
        if (doc == nullptr || !doc->symbols || loc.index >= doc->symbols->size()) {
            continue;
        }
        const std::string& q = (*doc->symbols)[loc.index].qualified;
        const bool match = q == name || (q.size() > name.size() && std::string_view(q).ends_with(name) &&
                                         std::string_view(q).substr(0, q.size() - name.size()).ends_with("::"));
        if (match) {
            out.push_back(loc);
        }
    }
    // Names inside namespace blocks are unqualified: fall back to all matches.
    return out.empty() ? it->second : out;
}

}  // namespace codescope
