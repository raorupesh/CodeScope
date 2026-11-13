#include "codescope/tokenizer.hpp"

#include <algorithm>

namespace codescope {

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = ascii_lower(c);
    }
    return out;
}

std::vector<Trigram> extract_trigrams(std::string_view text) {
    std::vector<Trigram> out;
    if (text.size() < 3) {
        return out;
    }
    out.reserve(text.size() - 2);
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        out.push_back(pack_trigram(ascii_lower(text[i]), ascii_lower(text[i + 1]), ascii_lower(text[i + 2])));
    }
    // Sort + unique is faster than a hash set for the typical file size and
    // leaves the result sorted, which the index relies on.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace codescope
