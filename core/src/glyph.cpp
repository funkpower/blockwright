#include "blockwright/glyph.h"

#include "blockwright/width.h"

#include <atomic>
#include <bit>
#include <cstdio>
#include <mutex>
#include <new>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace bw {

namespace {

struct Entry {
    std::array<char32_t, kMaxClusterCodePoints> cps;
    std::uint8_t size;
    std::int8_t width;
};

// Chunk k holds kFirstChunk << k entries; 22 chunks hold more than the
// 0xFFFFFFFF - 0x110000 ids there are.
constexpr int kChunks = 22;
constexpr std::size_t kFirstChunk = 1024;
constexpr std::size_t kMaxEntries = std::size_t(0xFFFFFFFFu) - Glyph::kContinuationBits;
static_assert((kFirstChunk << kChunks) - kFirstChunk >= kMaxEntries);

struct Place {
    int chunk;
    std::size_t offset;
};

constexpr Place place(std::size_t index) {
    const int k = std::bit_width(index / kFirstChunk + 1) - 1;
    return {k, index - ((kFirstChunk << k) - kFirstChunk)};
}
static_assert(place(0).chunk == 0 && place(1023).offset == 1023 && place(1024).chunk == 1 &&
              place(1024).offset == 0 && place(3071).chunk == 1 && place(3072).chunk == 2 &&
              place(3072).offset == 0);

bool scalar(char32_t cp) { return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF); }

// A control a cluster may not hold: C0 but U+0000 (a cell's blank), DEL,
// C1, and the line and paragraph separators.
bool control(char32_t cp) {
    return (cp >= 0x01 && cp < 0x20) || (cp >= 0x7F && cp < 0xA0) || cp == 0x2028 || cp == 0x2029;
}

} // namespace

bool clusterMayHold(char32_t cp) { return scalar(cp) && !control(cp); }

struct ClusterPool::Impl {
    // Published in order: a chunk pointer before the count that covers its
    // first entry, an entry's contents before the count that covers it.
    std::array<std::atomic<Entry*>, kChunks> chunks{};
    std::atomic<std::size_t> count{0};
    std::mutex adding;
    std::unordered_map<std::u32string, std::uint32_t> index;  // under `adding`

    ~Impl() {
        for (auto& c : chunks)
            delete[] c.load();
    }

    const Entry* find(Glyph g) const {
        if (!g.isCluster())
            return nullptr;
        const std::size_t i = g.bits() - Glyph::kContinuationBits - 1;
        if (i >= count.load(std::memory_order_acquire))
            return nullptr;
        const Place p = place(i);
        return chunks[static_cast<std::size_t>(p.chunk)].load(std::memory_order_acquire) + p.offset;
    }
};

ClusterPool::ClusterPool() : d_(std::make_unique<Impl>()) {}
ClusterPool::~ClusterPool() = default;

std::optional<Glyph> ClusterPool::intern(std::span<const char32_t> cps, PoolCap cap) {
    return intern(cps, cap == PoolCap::Capped ? kLossyCap : kMaxEntries);
}

std::optional<Glyph> ClusterPool::intern(std::span<const char32_t> cps, std::size_t limit) {
    if (cps.empty() || cps.size() > std::size_t(kMaxClusterCodePoints))
        throw std::invalid_argument("a cluster holds 1 to 16 code points");
    for (char32_t cp : cps)
        if (!scalar(cp))
            throw std::invalid_argument("a cluster holds Unicode scalar values only");
    if (cps.size() == 1)
        return Glyph(cps[0]);
    for (char32_t cp : cps)
        if (control(cp))
            throw std::invalid_argument("a cluster holds no control");
    std::u32string key(cps.begin(), cps.end());
    const std::lock_guard lock(d_->adding);
    if (const auto it = d_->index.find(key); it != d_->index.end())
        return Glyph(Glyph::Raw{}, it->second);
    const std::size_t n = d_->count.load(std::memory_order_relaxed);
    if (n >= kMaxEntries || n >= limit)
        return std::nullopt;
    try {
        const Place p = place(n);
        auto& slot = d_->chunks[static_cast<std::size_t>(p.chunk)];
        Entry* chunk = slot.load(std::memory_order_relaxed);
        if (!chunk) {
            chunk = new Entry[kFirstChunk << p.chunk];
            slot.store(chunk, std::memory_order_release);
        }
        Entry& e = chunk[p.offset];
        e.cps.fill(0);
        std::copy(cps.begin(), cps.end(), e.cps.begin());
        e.size = static_cast<std::uint8_t>(cps.size());
        e.width = static_cast<std::int8_t>(codePointWidth(cps[0]));
        const auto bits = static_cast<std::uint32_t>(Glyph::kContinuationBits + 1 + n);
        d_->index.emplace(std::move(key), bits);
        d_->count.store(n + 1, std::memory_order_release);
        return Glyph(Glyph::Raw{}, bits);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

GlyphCodePoints ClusterPool::codePoints(Glyph g) const {
    GlyphCodePoints out;
    if (g.isContinuation())
        return out;
    if (const std::optional<char32_t> cp = g.codePoint()) {
        out.cps[0] = *cp;
        out.size = 1;
    } else if (const Entry* e = d_->find(g)) {
        out.cps = e->cps;
        out.size = e->size;
    } else {
        out.cps[0] = U'�';
        out.size = 1;
    }
    return out;
}

int ClusterPool::width(Glyph g) const {
    if (g.isContinuation())
        return 0;
    if (const std::optional<char32_t> cp = g.codePoint())
        return codePointWidth(*cp);
    const Entry* e = d_->find(g);
    return e ? e->width : codePointWidth(U'�');
}

std::size_t ClusterPool::size() const { return d_->count.load(std::memory_order_acquire); }

ClusterPool& clusterPool() {
    // never destroyed: a cell's id stays good until the process ends
    static ClusterPool* pool = new ClusterPool;
    return *pool;
}

GlyphCodePoints codePoints(Glyph g) {
    if (const std::optional<char32_t> cp = g.codePoint())
        return GlyphCodePoints{{*cp}, 1};
    return clusterPool().codePoints(g);
}

int glyphColumns(Glyph g) {
    if (const std::optional<char32_t> cp = g.codePoint())
        return codePointWidth(*cp);
    return clusterPool().width(g);
}

std::ostream& operator<<(std::ostream& os, Glyph g) {
    if (g.isContinuation())
        return os << "continuation";
    const GlyphCodePoints cps = codePoints(g);
    char buf[16];
    if (g.isCluster())
        os << "cluster";
    for (int i = 0; i < cps.size; ++i) {
        std::snprintf(buf, sizeof buf, "U+%04X", static_cast<unsigned>(cps[i]));
        os << (g.isCluster() ? " " : "") << buf;
    }
    if (const std::optional<char32_t> cp = g.codePoint(); cp && *cp >= 0x20 && *cp < 0x7F)
        os << " '" << static_cast<char>(*cp) << "'";
    return os;
}

} // namespace bw
