#include "unifont.h"

#include <QByteArray>
#include <QFile>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace bw {

namespace {

// The decoded table (scripts/gen-unifont has the format): the code points in
// order, where each glyph's bitmap starts, and the bitmaps.
struct Table {
    std::vector<char32_t> cps;
    std::vector<std::uint32_t> offsets;  // into bits; one past the last at the end
    QByteArray bits;
};

[[noreturn]] void damaged() { throw std::runtime_error("the compiled-in Unifont table is damaged"); }

Table decode() {
    QFile f(QStringLiteral(":/blockwright/unifont.bin.z"));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("the compiled-in Unifont table is missing");
    const QByteArray raw = qUncompress(f.readAll());
    if (raw.size() < 8 || std::memcmp(raw.constData(), "BWUF", 4) != 0)
        damaged();
    const auto* p = reinterpret_cast<const std::uint8_t*>(raw.constData());
    const auto* end = p + raw.size();
    const std::uint32_t n = std::uint32_t(p[4]) | std::uint32_t(p[5]) << 8 | std::uint32_t(p[6]) << 16 |
                            std::uint32_t(p[7]) << 24;
    if (n > 0x110000)
        damaged();
    p += 8;
    Table t;
    t.cps.reserve(n);
    t.offsets.reserve(n + 1);
    std::int64_t cp = -1;
    std::uint32_t offset = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t v = 0;
        for (int shift = 0;; shift += 7) {
            if (p == end || shift > 28)
                damaged();
            const std::uint8_t b = *p++;
            v |= std::uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                break;
        }
        cp += std::int64_t(v >> 1) + 1;
        if (cp > 0x10FFFF)
            damaged();
        t.cps.push_back(char32_t(cp));
        t.offsets.push_back(offset);
        offset += (v & 1) ? 32 : 16;
    }
    t.offsets.push_back(offset);
    if (end - p != std::ptrdiff_t(offset))
        damaged();
    t.bits = raw.mid(p - reinterpret_cast<const std::uint8_t*>(raw.constData()));
    return t;
}

const Table& table() {
    static const Table t = decode();
    return t;
}

} // namespace

std::optional<UnifontGlyph> unifontGlyph(char32_t cp) {
    const Table& t = table();
    const auto it = std::lower_bound(t.cps.begin(), t.cps.end(), cp);
    if (it == t.cps.end() || *it != cp)
        return std::nullopt;
    const auto i = std::size_t(it - t.cps.begin());
    const auto* bits = reinterpret_cast<const std::uint8_t*>(t.bits.constData()) + t.offsets[i];
    return UnifontGlyph{t.offsets[i + 1] - t.offsets[i] == 32, bits};
}

std::size_t unifontGlyphCount() { return table().cps.size(); }

} // namespace bw
