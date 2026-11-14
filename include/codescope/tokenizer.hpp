#pragma once

// Case folding is ASCII-only; other bytes (including UTF-8) are kept as is.

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

std::string ascii_lower(std::string_view text);

// Distinct trigrams of the lowercased text, sorted; empty under 3 bytes.
std::vector<Trigram> extract_trigrams(std::string_view text);

}  // namespace codescope
