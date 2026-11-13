#include "codescope/path_utils.hpp"

#include <array>
#include <utility>
#include <vector>

#include "codescope/tokenizer.hpp"

namespace codescope {

std::optional<std::string> normalize_repo_path(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return std::nullopt;  // absolute (or UNC) path
    }
    if (path.find(':') != std::string_view::npos) {
        return std::nullopt;  // drive letter or alternate data stream
    }

    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        if (part == "..") {
            return std::nullopt;
        }
        if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    if (parts.empty()) {
        return std::nullopt;
    }

    std::string out;
    for (const std::string_view part : parts) {
        if (!out.empty()) {
            out += '/';
        }
        out += part;
    }
    return out;
}

std::string extension_of(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};  // no extension, or a dotfile such as ".gitignore"
    }
    return ascii_lower(name.substr(dot));
}

std::string language_of(std::string_view path) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 30> kLanguages{{
        {".c", "c"},           {".h", "c"},            {".cc", "cpp"},         {".cpp", "cpp"},
        {".cxx", "cpp"},       {".hh", "cpp"},         {".hpp", "cpp"},        {".hxx", "cpp"},
        {".ipp", "cpp"},       {".inl", "cpp"},        {".py", "python"},      {".js", "javascript"},
        {".mjs", "javascript"}, {".ts", "typescript"}, {".java", "java"},      {".go", "go"},
        {".rs", "rust"},       {".cs", "csharp"},      {".m", "matlab"},       {".sh", "shell"},
        {".cmake", "cmake"},   {".json", "json"},      {".yml", "yaml"},       {".yaml", "yaml"},
        {".md", "markdown"},   {".html", "html"},      {".css", "css"},        {".xml", "xml"},
        {".sql", "sql"},       {".txt", "text"},
    }};
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (name == "CMakeLists.txt") {
        return "cmake";
    }
    if (name == "Makefile" || name == "makefile") {
        return "makefile";
    }
    const std::string ext = extension_of(path);
    for (const auto& [e, lang] : kLanguages) {
        if (ext == e) {
            return std::string(lang);
        }
    }
    return "text";
}

}  // namespace codescope
