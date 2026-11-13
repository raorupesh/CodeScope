#include "codescope/serializer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace codescope {
namespace {

constexpr std::array<char, 4> kMagic{'C', 'S', 'I', 'X'};

class Writer {
public:
    explicit Writer(std::ofstream& out) : out_(out) {}

    void bytes(const void* data, std::size_t n) { out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n)); }

    void u32(std::uint32_t v) {
        const std::array<unsigned char, 4> b{
            static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
            static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
        bytes(b.data(), b.size());
    }

    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }

    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

private:
    std::ofstream& out_;
};

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return data_.size() - pos_; }

    std::string_view bytes(std::size_t n) {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return {};
        }
        const std::string_view out = data_.substr(pos_, n);
        pos_ += n;
        return out;
    }

    std::uint32_t u32() {
        const std::string_view b = bytes(4);
        if (b.size() != 4) {
            return 0;
        }
        return static_cast<std::uint32_t>(static_cast<unsigned char>(b[0])) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b[1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b[2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b[3])) << 24);
    }

    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        const std::uint64_t hi = u32();
        return lo | (hi << 32);
    }

    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::shared_ptr<IndexSnapshot> fail(std::string* error, const std::string& reason) {
    if (error != nullptr) {
        *error = reason;
    }
    return nullptr;
}

std::uint32_t checked_u32(std::size_t v, const char* what) {
    if (v > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("index too large: ") + what);
    }
    return static_cast<std::uint32_t>(v);
}

}  // namespace

void IndexSerializer::save(const IndexSnapshot& snapshot, const fs::path& file) {
    const DocStore& docs = snapshot.docs;
    const TrigramIndex& index = snapshot.index;

    // Dense renumbering of live documents. The mapping is monotonic, so
    // remapped posting lists stay sorted.
    const std::vector<DocId> live = docs.live_ids();
    std::unordered_map<DocId, DocId> remap;
    remap.reserve(live.size());
    for (std::size_t i = 0; i < live.size(); ++i) {
        remap.emplace(live[i], static_cast<DocId>(i));
    }

    // Deterministic output: trigrams in ascending order.
    std::vector<Trigram> trigrams;
    trigrams.reserve(index.trigram_count());
    for (const auto& entry : index.postings()) {
        trigrams.push_back(entry.first);
    }
    std::sort(trigrams.begin(), trigrams.end());

    fs::path tmp = file;
    tmp.replace_extension(".tmp");

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write " + tmp.string());
        }
        Writer w(out);
        w.bytes(kMagic.data(), kMagic.size());
        w.u32(kVersion);
        w.u32(checked_u32(live.size(), "documents"));
        for (const DocId id : live) {
            const Document* doc = docs.get(id);
            w.u32(checked_u32(doc->path.size(), "path"));
            w.bytes(doc->path.data(), doc->path.size());
            w.i64(doc->mtime);
            w.u64(doc->size);
        }

        // Count non-empty lists first so trigram_count is exact.
        std::vector<std::vector<DocId>> lists;
        std::vector<Trigram> kept;
        lists.reserve(trigrams.size());
        kept.reserve(trigrams.size());
        for (const Trigram t : trigrams) {
            std::vector<DocId> mapped;
            for (const DocId id : index.postings().at(t)) {
                if (index.is_removed(id)) {
                    continue;
                }
                const auto it = remap.find(id);
                if (it != remap.end()) {
                    mapped.push_back(it->second);
                }
            }
            if (!mapped.empty()) {
                kept.push_back(t);
                lists.push_back(std::move(mapped));
            }
        }

        w.u32(checked_u32(kept.size(), "trigrams"));
        for (std::size_t i = 0; i < kept.size(); ++i) {
            w.u32(kept[i]);
            w.u32(checked_u32(lists[i].size(), "posting list"));
            for (const DocId id : lists[i]) {
                w.u32(id);
            }
        }

        out.flush();
        if (!out) {
            out.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            throw std::runtime_error("write failed: " + tmp.string());
        }
    }

    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        const std::string reason = ec.message();
        fs::remove(tmp, ec);
        throw std::runtime_error("cannot replace " + file.string() + ": " + reason);
    }
}

std::shared_ptr<IndexSnapshot> IndexSerializer::load(const fs::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return fail(error, "cannot open " + file.string());
    }
    const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Reader r(data);

    const std::string_view magic = r.bytes(kMagic.size());
    if (!r.ok() || std::memcmp(magic.data(), kMagic.data(), kMagic.size()) != 0) {
        return fail(error, "bad magic");
    }
    const std::uint32_t version = r.u32();
    if (!r.ok() || version != kVersion) {
        return fail(error, "unsupported version " + std::to_string(version));
    }

    auto snap = std::make_shared<IndexSnapshot>();

    const std::uint32_t doc_count = r.u32();
    // Each doc needs at least 4 + 8 + 8 bytes; reject absurd counts early.
    if (!r.ok() || static_cast<std::uint64_t>(doc_count) * 20 > r.remaining()) {
        return fail(error, "truncated document table");
    }
    for (std::uint32_t i = 0; i < doc_count; ++i) {
        const std::uint32_t len = r.u32();
        const std::string_view path = r.bytes(len);
        Document doc;
        doc.path = std::string(path);
        doc.mtime = r.i64();
        doc.size = r.u64();
        if (!r.ok() || doc.path.empty() || snap->docs.find(doc.path)) {
            return fail(error, "corrupt document entry");
        }
        snap->docs.add(std::move(doc));
    }

    const std::uint32_t trigram_count = r.u32();
    if (!r.ok() || static_cast<std::uint64_t>(trigram_count) * 8 > r.remaining()) {
        return fail(error, "truncated trigram table");
    }
    std::unordered_set<Trigram> seen_trigrams;
    seen_trigrams.reserve(trigram_count);
    for (std::uint32_t i = 0; i < trigram_count; ++i) {
        const Trigram t = r.u32();
        const std::uint32_t len = r.u32();
        if (!r.ok() || t > 0xFFFFFFu || len == 0 || static_cast<std::uint64_t>(len) * 4 > r.remaining() ||
            !seen_trigrams.insert(t).second) {
            return fail(error, "corrupt trigram entry");
        }
        std::vector<DocId> list(len);
        for (std::uint32_t j = 0; j < len; ++j) {
            list[j] = r.u32();
            if (list[j] >= doc_count || (j > 0 && list[j] <= list[j - 1])) {
                return fail(error, "corrupt posting list");
            }
        }
        snap->index.set_postings(t, std::move(list));
    }

    if (!r.ok() || r.remaining() != 0) {
        return fail(error, "trailing or missing bytes");
    }
    return snap;
}

}  // namespace codescope
