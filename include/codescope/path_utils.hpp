#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace codescope {

// Normalizes a repository-relative path received from a client.
//
// Backslashes become '/', empty and "." components are dropped. Returns
// std::nullopt for anything that could escape the repository root: absolute
// paths, drive letters, ".." components, or embedded NUL bytes.
std::optional<std::string> normalize_repo_path(std::string_view path);

// Lower-case file extension including the dot (".cpp"), or "" if none.
std::string extension_of(std::string_view path);

// Best-effort language name for the file viewer ("cpp", "python", ...).
std::string language_of(std::string_view path);

}  // namespace codescope
