#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace codescope {

// UTF-8 with '/' separators. Use instead of path::string(), which throws on
// Windows for names outside the ANSI code page.
std::string to_utf8(const std::filesystem::path& p);

// Cleans a client-supplied relative path; std::nullopt if it could escape the
// repository (absolute, drive letter, "..", NUL).
std::optional<std::string> normalize_repo_path(std::string_view path);

std::string extension_of(std::string_view path);  // lower-case, with dot; "" if none
std::string language_of(std::string_view path);

}  // namespace codescope
