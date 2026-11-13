#pragma once

// Trigram extraction. The index is case-insensitive: only ASCII letters are
// folded; every other byte (including UTF-8 continuation bytes) is kept as is.

#include <string>
#include <string_view>
#include <vector>

#include "codescope/types.hpp"

namespace codescope {

constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr Trigram pack_trigram(char a, char b, char c) noexcept {
    return (static_cast<Trigram>(static_cast<unsigned char>(a)) << 16) |
           (static_cast<Trigram>(static_cast<unsigned char>(b)) << 8) |
           static_cast<Trigram>(static_cast<unsigned char>(c));
}

// ASCII-lowercased copy of `text`.
std::string ascii_lower(std::string_view text);

// All distinct trigrams of the ASCII-lowercased text, sorted ascending.
// Returns an empty vector when `text` is shorter than 3 bytes.
std::vector<Trigram> extract_trigrams(std::string_view text);

}  // namespace codescope
