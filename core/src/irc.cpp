#include "blockwright/irc.h"

#include "blockwright/cp437.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

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

// The cell as the row encoder takes it. Not a Unicode scalar value: drawn as
// '?' rather than fail. A C1 control too: some clients read C2 9B as a CSI
// introducer.
MircCell encodable(const Cell& c) {
    MircCell m = toMirc(drawn(c));
    if (m.ch > 0x10ffff || (m.ch >= 0xd800 && m.ch <= 0xdfff) || (m.ch >= 0x80 && m.ch <= 0x9f))
        m.ch = U'?';
    return m;
}

bool same(const MircCell& a, const MircCell& b) { return a.ch == b.ch && a.fg == b.fg && a.bg == b.bg; }

bool blank(char32_t ch) { return ch == U' ' || ch == U'\u00a0'; }

// The one colour a cell shows, if it shows one: as the row encoder's ways().
std::optional<std::uint8_t> solidColour(const MircCell& c) {
    if (blank(c.ch))
        return c.bg;
    if (c.ch == U'█' || c.fg == c.bg)
        return c.fg;
    return std::nullopt;
}

// The colours a cell shows: its one colour if solid, else both. Returns how
// many.
int shownColours(const MircCell& c, std::array<std::uint8_t, 2>& out) {
    if (const auto s = solidColour(c)) {
        out[0] = *s;
        return 1;
    }
    out = {c.fg, c.bg};
    return 2;
}

// Whether cell x of `row`, fit from `cells`, shows only colours that it
// showed in `cells` or that its left neighbour shows in `row` now: what each
// of fit's steps keeps, and what a later change beside it must not break.
bool ownOrLeft(std::span<const MircCell> cells, const std::vector<MircCell>& row, std::size_t x) {
    if (x >= row.size() || same(row[x], cells[x]))
        return true;
    std::array<std::uint8_t, 4> ok{};
    std::array<std::uint8_t, 2> part{};
    int n = 0;
    for (int i = shownColours(cells[x], part), k = 0; k < i; ++k)
        ok[std::size_t(n++)] = part[std::size_t(k)];
    if (x > 0)
        for (int i = shownColours(row[x - 1], part), k = 0; k < i; ++k)
            ok[std::size_t(n++)] = part[std::size_t(k)];
    std::array<std::uint8_t, 2> mine{};
    for (int i = shownColours(row[x], mine), k = 0; k < i; ++k)
        if (std::find(ok.begin(), ok.begin() + n, mine[std::size_t(k)]) == ok.begin() + n)
            return false;
    return true;
}

// The quarters of a two-colour cell its foreground covers; 0 for a glyph fit
// leaves alone.
int fgQuarters(char32_t ch) {
    switch (ch) {
    case U'▀': case U'▄': case U'▌': case U'▐': case U'▒': return 2;
    case U'░': return 1;
    case U'▓': return 3;
    default: return 0;
    }
}

// The row as it is encoded: trailing black spaces dropped, unless padded,
// and then a black last cell drawn as a block.
std::string encodeCells(std::span<const MircCell> cells, const EncodeOptions& opts) {
    std::size_t n = cells.size();
    if (!opts.pad)
        while (n > 0 && cells[n - 1].ch == U' ' && cells[n - 1].bg == kMircBlack)
            --n;
    const auto row = cells.first(n);
    const bool solidEnd = opts.pad && n > 0 && solidColour(row.back()) == kMircBlack;
    return encodeMircRow(row, opts.swapShades, solidEnd);
}

std::uint64_t distance(std::uint8_t a, std::uint8_t b) {
    const auto ch = [](std::uint32_t rgb, int shift) { return std::int64_t((rgb >> shift) & 255); };
    std::uint64_t d = 0;
    for (const int shift : {16, 8, 0}) {
        const std::int64_t x = ch(kMircRgb[a], shift) - ch(kMircRgb[b], shift);
        d += std::uint64_t(x * x);
    }
    return d;
}

// One change fit may make: cell x becomes `cell` at a cost of `cost`.
struct Step {
    std::uint64_t cost;
    std::size_t x;
    MircCell cell;
};

// The changes fit may make to cell x of `row`: into one colour it or its
// left neighbour shows, or one half or one shade colour into a colour of the
// neighbour's.
void stepsFor(const std::vector<MircCell>& row, std::size_t x, std::vector<Step>& out) {
    const MircCell& c = row[x];
    std::array<std::uint8_t, 2> left{};
    const int nLeft = x > 0 ? shownColours(row[x - 1], left) : 0;
    // A colour swapped for itself changes nothing; one swapped for another
    // of the same RGB (mIRC 1 and 88 are both black) costs nothing.
    const auto solid = [](std::uint8_t colour) { return MircCell{U' ', colour, colour}; };
    if (const auto s = solidColour(c)) {
        for (int i = 0; i < nLeft; ++i)
            if (left[i] != *s)
                out.push_back({distance(*s, left[i]) * 4, x, solid(left[i])});
        return;
    }
    const int q = fgQuarters(c.ch);
    if (q == 0)
        return;
    const std::uint64_t d = distance(c.fg, c.bg);
    out.push_back({d * std::uint64_t(4 - q), x, solid(c.fg)});
    out.push_back({d * std::uint64_t(q), x, solid(c.bg)});
    for (int i = 0; i < nLeft; ++i) {
        const std::uint8_t l = left[i];
        if (l != c.fg && l != c.bg) {
            out.push_back({distance(c.fg, l) * std::uint64_t(q), x, MircCell{c.ch, l, c.bg}});
            out.push_back({distance(c.bg, l) * std::uint64_t(4 - q), x, MircCell{c.ch, c.fg, l}});
        }
    }
}

} // namespace

std::optional<std::size_t> budgetFor(std::string_view mask, std::string_view target) {
    const auto bad = [](std::string_view s) {
        return s.empty() || s.find_first_of(std::string_view(" \r\n\0", 4)) != std::string_view::npos;
    };
    if (bad(mask) || bad(target))
        return std::nullopt;
    // ":" mask " PRIVMSG " target " :" text "\r\n"
    const std::size_t prefix = 1 + mask.size() + 9 + target.size() + 2 + 2;
    if (prefix >= 512)
        return std::nullopt;
    return 512 - prefix;
}

std::vector<MircCell> fitCells(std::span<const MircCell> cells, std::size_t budget, const EncodeOptions& opts) {
    std::vector<MircCell> row(cells.begin(), cells.end());
    const auto fits = [&] { return encodeCells(row, opts).size() <= budget; };
    if (fits())
        return row;
    // A cell may change a few times (a half block into a solid, the solid
    // into its neighbour's colour); what it costs adds up.
    constexpr int kMaxChanges = 3;
    std::vector<std::uint64_t> spent(row.size(), 0);
    std::vector<int> changes(row.size(), 0);
    std::vector<bool> now(row.size(), false);
    std::vector<Step> steps;
    bool done = false;
    while (!done) {
        steps.clear();
        for (std::size_t x = 0; x < row.size(); ++x)
            if (changes[x] < kMaxChanges)
                stepsFor(row, x, steps);
        if (steps.empty())
            break;
        std::sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) {
            return std::tie(a.cost, a.x, a.cell.ch, a.cell.fg, a.cell.bg) <
                   std::tie(b.cost, b.x, b.cell.ch, b.cell.fg, b.cell.bg);
        });
        // A twelfth of the steps at a time, as ascii.py's fit_halves, so a
        // wide row takes a few encodes rather than one per cell. A cell
        // changes once a batch, and not beside one changing in the same
        // batch, whose colours its steps were worked out from; nor in a way
        // that takes away a colour the cell right of it took from it.
        const std::size_t batch = std::max<std::size_t>(1, steps.size() / 12);
        std::fill(now.begin(), now.end(), false);
        std::size_t taken = 0;
        for (const Step& s : steps) {
            if (taken == batch)
                break;
            if (now[s.x] || (s.x > 0 && now[s.x - 1]) || (s.x + 1 < row.size() && now[s.x + 1]))
                continue;
            const MircCell was = row[s.x];
            row[s.x] = s.cell;
            if (!ownOrLeft(cells, row, s.x + 1)) {
                row[s.x] = was;
                continue;
            }
            now[s.x] = true;
            ++changes[s.x];
            spent[s.x] += s.cost;
            ++taken;
        }
        if (taken == 0)
            break;
        done = fits();
    }
    if (!done)
        return {cells.begin(), cells.end()};
    // Put back what the row fits without, the most visible change first,
    // with the changes right of it that took a colour from it, which would
    // otherwise show a colour neither they nor their neighbour has.
    std::vector<std::size_t> order;
    for (std::size_t x = 0; x < row.size(); ++x)
        if (changes[x] > 0)
            order.push_back(x);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return spent[a] > spent[b]; });
    std::vector<MircCell> kept;
    for (const std::size_t x : order) {
        if (same(row[x], cells[x]))
            continue;  // put back with a change left of it
        kept.assign(row.begin() + std::ptrdiff_t(x), row.end());
        row[x] = cells[x];
        for (std::size_t k = x + 1; !ownOrLeft(cells, row, k); ++k)
            row[k] = cells[k];
        if (!fits())
            std::copy(kept.begin(), kept.end(), row.begin() + std::ptrdiff_t(x));
    }
    return row;
}

std::uint8_t mircColor(const Color& c) {
    if (c.isIndexed())
        return kCgaToMirc[c.index & 15];
    return nearestMirc(std::uint32_t(c.rgb.r) << 16 | std::uint32_t(c.rgb.g) << 8 | c.rgb.b);
}

std::vector<MircCell> mircRow(std::span<const Cell> cells) {
    std::vector<MircCell> raw;
    raw.reserve(cells.size());
    for (const Cell& c : cells)
        raw.push_back(encodable(c));
    return raw;
}

Cell sentCell(const Cell& c, bool ice) { return c.transparent ? c : iceCell(c, ice); }

Canvas sentCanvas(const Canvas& canvas, bool ice) {
    Canvas out = canvas;
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x)
            out.at(x, y) = sentCell(out.at(x, y), ice);
    return out;
}

std::string encodeRow(std::span<const Cell> cells, const EncodeOptions& opts) {
    return encodeMircRow(mircRow(cells), opts.swapShades);
}

Line encodeMircLine(std::span<const MircCell> cells, const EncodeOptions& opts) {
    Line line;
    if (opts.fit > 0) {
        const std::vector<MircCell> fitted = fitCells(cells, opts.fit, opts);
        for (size_t x = 0; x < cells.size(); ++x)
            line.merged += same(cells[x], fitted[x]) ? 0 : 1;
        line.text = encodeCells(fitted, opts);
        return line;
    }
    line.text = encodeCells(cells, opts);
    return line;
}

Line encodeLine(const Canvas& canvas, int y, const EncodeOptions& opts) {
    std::vector<Cell> row;
    row.reserve(static_cast<size_t>(canvas.width()));
    for (int x = 0; x < canvas.width(); ++x)
        row.push_back(canvas.at(x, y));
    return encodeMircLine(mircRow(row), opts);
}

std::vector<std::string> encodeCanvas(const Canvas& canvas, const EncodeOptions& opts) {
    std::vector<std::string> lines;
    lines.reserve(static_cast<size_t>(canvas.height()));
    for (int y = 0; y < canvas.height(); ++y)
        lines.push_back(encodeLine(canvas, y, opts).text);
    return lines;
}

std::vector<Line> exportLines(const Canvas& canvas, const EncodeOptions& opts) {
    std::vector<Line> lines;
    lines.reserve(static_cast<size_t>(canvas.height()));
    for (int y = 0; y < canvas.height(); ++y)
        lines.push_back(encodeLine(canvas, y, opts));
    trimBlankLines(lines);
    return lines;
}

void trimBlankLines(std::vector<Line>& lines) {
    while (!lines.empty() && lines.back().text.empty())
        lines.pop_back();
    size_t lead = 0;
    while (lead < lines.size() && lines[lead].text.empty())
        ++lead;
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lead));
}

std::vector<std::string> exportCanvas(const Canvas& canvas, const EncodeOptions& opts) {
    std::vector<std::string> out;
    for (Line& l : exportLines(canvas, opts))
        out.push_back(std::move(l.text));
    return out;
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
