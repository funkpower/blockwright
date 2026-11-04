#pragma once
// What a cell holds (PLAN §16, Storage): one code point, the continuation
// (the right half of a wide glyph), or a cluster, a base code point and the
// zero-width code points after it, kept in a process-wide pool.

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>

namespace bw {

// A cell's glyph, 32 bits: up to U+10FFFF a single code point, 0x110000 the
// continuation, above it the id of a cluster in a ClusterPool. A conversion
// from char32_t makes only code points: anything above U+10FFFF becomes
// U+FFFD, so the continuation comes only from continuation() and an id only
// from a pool's intern. Ids are process-local: never written to a file, never
// ordered (== and no <), and only hashed in memory (bits()). A glyph turns
// into a code point only through codePoint(), so every place that treats one
// as a code point says what it does with the continuation and a cluster.
class Glyph {
public:
    static constexpr std::uint32_t kContinuationBits = 0x110000;

    constexpr Glyph() = default;
    constexpr Glyph(char32_t cp) : bits_(cp <= 0x10FFFF ? std::uint32_t(cp) : 0xFFFDu) {}

    // The right half of a wide glyph: no glyph of its own.
    static constexpr Glyph continuation() { return Glyph(Raw{}, kContinuationBits); }

    constexpr bool isCodePoint() const { return bits_ < kContinuationBits; }
    constexpr bool isContinuation() const { return bits_ == kContinuationBits; }
    constexpr bool isCluster() const { return bits_ > kContinuationBits; }

    // The code point, or nothing for the continuation or a cluster.
    constexpr std::optional<char32_t> codePoint() const {
        if (isCodePoint())
            return char32_t(bits_);
        return std::nullopt;
    }
    // The code point, or `other` for the continuation or a cluster: for a
    // test that one code point passes (is it a shade, a half block, a
    // space?) and that nothing else does.
    constexpr char32_t codePointOr(char32_t other) const { return isCodePoint() ? char32_t(bits_) : other; }


    // The raw value, for an in-memory hash or cache key in this process only.
    constexpr std::uint32_t bits() const { return bits_; }

    friend constexpr bool operator==(const Glyph&, const Glyph&) = default;

private:
    friend class ClusterPool;
    struct Raw {};
    constexpr Glyph(Raw, std::uint32_t bits) : bits_(bits) {}

    std::uint32_t bits_ = U' ';
};
static_assert(sizeof(Glyph) == 4);

// A value above U+10FFFF, so no case of a switch on a code point matches it:
// `switch (cell.ch.codePointOr(kNoCodePoint))` sends the continuation and a
// cluster to the default.
inline constexpr char32_t kNoCodePoint = 0xFFFFFFFF;

// For test failure messages: "U+0041 'A'", "continuation", or a cluster's
// code points ("cluster U+0065 U+0301").
std::ostream& operator<<(std::ostream& os, Glyph g);

// The most code points a cluster holds, its base included.
inline constexpr int kMaxClusterCodePoints = 16;

// The code points a glyph stands for, base first: one for a code point, a
// cluster's own, none for the continuation.
struct GlyphCodePoints {
    std::array<char32_t, kMaxClusterCodePoints> cps{};
    int size = 0;

    const char32_t* begin() const { return cps.data(); }
    const char32_t* end() const { return cps.data() + size; }
    char32_t operator[](int i) const { return cps[static_cast<std::size_t>(i)]; }
    bool empty() const { return size == 0; }
};

// Whether intern may refuse a new cluster once the pool holds kLossyCap
// clusters: a path that can keep a cluster's base and report its marks
// dropped (the UTF-8 reader, the IRC view's decodeLine, typing) is capped;
// a project load, whose own code-point limit bounds its table, isn't, so
// whether a project opens never depends on what the session read before.
enum class PoolCap { Capped, Uncapped };

// Whether a cluster of two or more code points may hold `cp`: a Unicode
// scalar value that isn't a control (C0 but U+0000, DEL, C1, U+2028,
// U+2029). What ClusterPool::intern takes, so a reader can check a sequence
// before it interns anything.
bool clusterMayHold(char32_t cp);

// An append-only, interned set of clusters, each with its code points and
// width (its base's, codePointWidth). Lookups take no lock and are safe from
// any thread while others intern: the chunk directory is a fixed array of
// pointers, each chunk (1024 entries, then twice the one before) allocated
// when first needed and never moved or freed, and an id is checked against
// the published count, so an id the pool doesn't hold reads as U+FFFD.
class ClusterPool {
public:
    // Interning past this many clusters fails on PoolCap::Capped paths.
    static constexpr std::size_t kLossyCap = 65536;

    ClusterPool();
    ~ClusterPool();
    ClusterPool(const ClusterPool&) = delete;
    ClusterPool& operator=(const ClusterPool&) = delete;

    // The glyph holding `cps`: the code point itself when there is one,
    // else the pool's id for the sequence, added if new. Nothing when the
    // pool can't take a new one (PoolCap::Capped and kLossyCap reached, or
    // it is out of ids or memory). Throws std::invalid_argument for an empty
    // sequence, one longer than kMaxClusterCodePoints, or one holding a
    // surrogate or a value above U+10FFFF: no conversion mints an id from a
    // non-scalar. A sequence of two or more also throws if it holds a
    // control anywhere (C0 but U+0000, DEL, C1, U+2028, U+2029); its base
    // may be zero-width (a lone mark, which lint names).
    std::optional<Glyph> intern(std::span<const char32_t> cps, PoolCap cap);
    // intern with a cap of `limit` clusters in place of kLossyCap, for a
    // capped path whose tests can't fill 65,536 entries.
    std::optional<Glyph> intern(std::span<const char32_t> cps, std::size_t limit);

    // The code points of `g` (GlyphCodePoints).
    GlyphCodePoints codePoints(Glyph g) const;
    // The cell width of `g`: codePointWidth of a code point, a cluster's
    // base's, 0 for the continuation.
    int width(Glyph g) const;
    // The clusters the pool holds.
    std::size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The process-wide pool every cell's cluster lives in. It never frees, and is
// shared by every test in a process: no test asserts an id or its size, only
// how its size changes across a call.
ClusterPool& clusterPool();

// The process-wide pool's codePoints and width.
GlyphCodePoints codePoints(Glyph g);
int glyphColumns(Glyph g);

// The cluster a reader is building (PLAN §16, Reading): the code points of
// the last cell it wrote, while a zero-width code point may still join them.
// It goes into the pool once, when it is finished, so no prefix of it is
// interned. The UTF-8 .ans reader and the IRC view's decodeLine share it;
// each decides when its cursor has moved, which finishes it.
class ClusterReader {
public:
    // `poolCap`: the cap intern is given (ClusterPool::kLossyCap, lowered by
    // tests).
    explicit ClusterReader(std::size_t poolCap = ClusterPool::kLossyCap) : cap_(poolCap) {}

    // Whether a zero-width code point read now joins a cluster.
    bool open() const { return size_ > 0; }
    // Starts a cluster on `cp`, the cell just written, unless `cp` is a
    // control (C0, NUL included, DEL, C1, U+2028, U+2029), which no mark
    // joins. Finish the one before first.
    void start(char32_t cp);
    // Adds zero-width `cp` to the open cluster; false when it already holds
    // kMaxClusterCodePoints, and `cp` is dropped.
    bool join(char32_t cp);
    // Ends the cluster. A cluster of two or more code points the pool takes
    // is its glyph; one it refuses (past the cap) sets `*refused` and gives
    // nothing, the cell keeping its base. A lone code point gives nothing.
    std::optional<Glyph> finish(bool* refused);

private:
    std::size_t cap_;
    std::array<char32_t, kMaxClusterCodePoints> cps_{};
    int size_ = 0;
};

} // namespace bw
