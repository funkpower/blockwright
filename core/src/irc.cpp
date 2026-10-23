#include "blockwright/irc.h"

#include "blockwright/cp437.h"

#include <array>

namespace bw::irc {

namespace {

// CGA palette index -> mIRC colour of the same name and shade. The PC's dark
// red (#aa0000) is mIRC's brown (#7f0000), its brown (#aa5500) is the one
// mIRC calls orange, and its grey is light grey.
constexpr std::array<std::uint8_t, 16> kCgaToMirc = {
    1, 2, 3, 10, 5, 6, 7, 15,   // black blue green cyan red magenta brown grey
    14, 12, 9, 11, 4, 13, 8, 0, // and the bright eight
};

constexpr std::uint8_t kMircBlack = 1;

// The cell as it is drawn: a transparent cell as a blank one, as the ANSI
// writer draws it, and a C0 control or DEL as its CP437 glyph (NUL as a
// space, as the parser reads it), so no raw control byte reaches IRC, where
// ^B ^C ^O ^V ^] ^_ are formatting codes and CR, LF and NUL end the message.
Cell drawn(const Cell& c) {
    if (c.transparent)
        return Cell{};
    Cell d = c;
    if (d.ch == 0)
        d.ch = U' ';
    else if (d.ch < 0x20 || d.ch == 0x7f)
        d.ch = cp437ToUnicode(static_cast<std::uint8_t>(d.ch));
    return d;
}

MircCell toMirc(const Cell& c) {
    return {c.ch, mircColor(c.fg), mircColor(c.bg)};
}

} // namespace

std::uint8_t mircColor(const Color& c) {
    if (c.isIndexed())
        return kCgaToMirc[c.index & 15];
    return nearestMirc(std::uint32_t(c.rgb.r) << 16 | std::uint32_t(c.rgb.g) << 8 | c.rgb.b);
}

std::string encodeRow(std::span<const Cell> cells, const EncodeOptions& opts) {
    std::vector<MircCell> raw;
    raw.reserve(cells.size());
    for (const Cell& c : cells) {
        MircCell m = toMirc(drawn(c));
        // Not a Unicode scalar value: draw it as '?' rather than fail. A C1
        // control too: some clients read C2 9B as a CSI introducer.
        if (m.ch > 0x10ffff || (m.ch >= 0xd800 && m.ch <= 0xdfff) || (m.ch >= 0x80 && m.ch <= 0x9f))
            m.ch = U'?';
        raw.push_back(m);
    }
    return encodeMircRow(raw, opts.swapShades);
}

std::vector<std::string> encodeCanvas(const Canvas& canvas, const EncodeOptions& opts) {
    std::vector<std::string> lines;
    lines.reserve(static_cast<size_t>(canvas.height()));
    std::vector<Cell> row;
    row.reserve(static_cast<size_t>(canvas.width()));
    for (int y = 0; y < canvas.height(); ++y) {
        row.clear();
        for (int x = 0; x < canvas.width(); ++x)
            row.push_back(drawn(canvas.at(x, y)));
        while (!row.empty() && row.back().ch == U' ' && mircColor(row.back().bg) == kMircBlack)
            row.pop_back();
        lines.push_back(encodeRow(row, opts));
    }
    return lines;
}

std::vector<std::string> exportCanvas(const Canvas& canvas, const EncodeOptions& opts) {
    std::vector<std::string> lines = encodeCanvas(canvas, opts);
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    size_t lead = 0;
    while (lead < lines.size() && lines[lead].empty())
        ++lead;
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lead));
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

std::vector<LineSize> overBudget(const std::vector<std::string>& lines, std::size_t budget) {
    std::vector<LineSize> out;
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].size() > budget)
            out.push_back({static_cast<int>(i) + 1, lines[i].size()});
    return out;
}

} // namespace bw::irc
