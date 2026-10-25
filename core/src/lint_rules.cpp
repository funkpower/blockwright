#include "blockwright/lint_rules.h"

#include "blockwright/cp437.h"
#include "blockwright/halfblock.h"
#include "blockwright/irc.h"
#include "blockwright/ramp.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

namespace bw::lint {

namespace {

using Kind = Finding::Kind;

Finding make(Kind kind, int x, int y, int x2, int y2, std::string message) {
    Finding f;
    f.kind = kind;
    f.x = x, f.y = y, f.x2 = x2, f.y2 = y2;
    f.message = std::move(message);
    return f;
}

// A space, NUL or 0xFF: a cell that shows only its background.
bool blankGlyph(char32_t ch) { return ch == U' ' || ch == 0 || ch == U' '; }
// The block elements (▀ ▄ █ ▌ ▐ ░ ▒ ▓ and the rest of U+2580-U+259F).
bool blockGlyph(char32_t ch) { return ch >= 0x2580 && ch <= 0x259f; }
// Text and line drawing: a glyph drawn in its foreground on its background.
bool textGlyph(char32_t ch) { return !blankGlyph(ch) && !blockGlyph(ch); }

// The background a cell shows: with iCE off a bright one blinks on its dark
// colour.
std::uint8_t shownBg(const Cell& c, bool ice) {
    const std::uint8_t bg = c.bg.paletteIndex();
    return ice ? bg : bg & 7;
}

// The half-block pixels of the canvas as they show, two rows per cell row;
// -1 where a cell doesn't hold pixels.
class Pixels {
public:
    Pixels(const Canvas& canvas, bool ice) : w_(canvas.width()), h_(2 * canvas.height()) {
        px_.assign(std::size_t(w_) * std::size_t(h_), -1);
        for (int y = 0; y < canvas.height(); ++y)
            for (int x = 0; x < w_; ++x) {
                const Cell& c = canvas.at(x, y);
                if (c.transparent)
                    continue;
                if (const auto p = halfBlocks(c, ice)) {
                    set(x, 2 * y, p->top);
                    set(x, 2 * y + 1, p->bottom);
                }
            }
    }
    int width() const { return w_; }
    int height() const { return h_; }
    // -1 off the canvas or outside a pixel cell
    int at(int x, int py) const {
        return x < 0 || py < 0 || x >= w_ || py >= h_ ? -1 : px_[std::size_t(py) * std::size_t(w_) + std::size_t(x)];
    }

private:
    void set(int x, int py, int c) { px_[std::size_t(py) * std::size_t(w_) + std::size_t(x)] = c; }
    int w_, h_;
    std::vector<int> px_;
};

// What a cell looks like: a flat colour (one-colour pixel cells and blanks)
// as (' ', colour, -1), a two-colour pixel cell as ('▀', top, bottom), and
// anything else as its glyph and colours. Off the canvas: a look no cell has.
using Look = std::tuple<char32_t, int, int>;
const Look kNoLook{U'￿', -2, -2};

Look lookOf(const Canvas& canvas, int x, int y, bool ice) {
    if (!canvas.contains(x, y))
        return kNoLook;
    const Cell& c = canvas.at(x, y);
    if (c.transparent)
        return Look{U' ', Black, -1};
    if (const auto p = halfBlocks(c, ice))
        return p->top == p->bottom ? Look{U' ', p->top, -1} : Look{kUpperHalf, p->top, p->bottom};
    return Look{c.ch, c.fg.paletteIndex(), shownBg(c, ice)};
}

// Whether `c` is a lighter rung than `d` on a named ramp that has both: a
// highlight on that colour's form.
bool lighterOn(int c, int d) {
    for (const Ramp& r : namedRamps()) {
        const auto ci = std::find(r.colours.begin(), r.colours.end(), c);
        const auto di = std::find(r.colours.begin(), r.colours.end(), d);
        if (ci != r.colours.end() && di != r.colours.end() && ci > di)
            return true;
    }
    return false;
}

void lonePixels(const Pixels& p, std::vector<Finding>& out) {
    for (int py = 1; py + 1 < p.height(); ++py)
        for (int x = 1; x + 1 < p.width(); ++x) {
            const int c = p.at(x, py), d = p.at(x - 1, py - 1);
            if (c < 0 || d < 0 || d == c || d == Black || !lighterOn(c, d))
                continue;
            bool alone = true;
            for (int dy = -1; alone && dy <= 1; ++dy)
                for (int dx = -1; alone && dx <= 1; ++dx)
                    alone = (dx == 0 && dy == 0) || p.at(x + dx, py + dy) == d;
            if (alone)
                out.push_back(make(Kind::LonePixel, x, py / 2, x, py / 2,
                                   "a lone glint: one " + std::string(py % 2 ? "bottom" : "top") + " pixel of " +
                                       std::to_string(c) + " on " + std::to_string(d) +
                                       " reads as a stray; a highlight wants a 2-5 px cluster"));
        }
}

void gaps(const Canvas& canvas, bool ice, std::vector<Finding>& out) {
    // along: the line's direction; across: the other
    auto check = [&](int x, int y, int ax, int ay) {
        auto look = [&](int i, int j) { return lookOf(canvas, x + i * ax + j * ay, y + i * ay + j * ax, ice); };
        const Look l = look(-1, 0), g = look(0, 0);
        if (l == kNoLook || g == l || look(-2, 0) != l || look(1, 0) != l || look(2, 0) != l)
            return false;
        // one cell thick: across the line beside the gap is something else.
        // Off the canvas is not something else: there is no telling how
        // thick the line is, and a texture along the edge would pass.
        for (const int i : {-1, 1})
            for (const int j : {-1, 1})
                if (look(i, j) == l || look(i, j) == kNoLook)
                    return false;
        // a dashed line breaks again a few cells on, the line on both sides
        // of that break too: that is its pattern. (Past the line's end is
        // not a break.)
        for (int i = 3; i <= 6; ++i)
            for (const int s : {-1, 1})
                if (look(s * i, 0) == g && look(s * (i - 1), 0) == l && look(s * (i + 1), 0) == l)
                    return false;
        // and the gap shows what is across the line on one side
        for (const int j : {-1, 1})
            if (look(-1, j) == g && look(0, j) == g && look(1, j) == g)
                return true;
        return false;
    };
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x) {
            const bool across = check(x, y, 1, 0), down = check(x, y, 0, 1);
            if (across || down)
                out.push_back(make(Kind::Gap, x, y, x, y,
                                   std::string("a one-cell break in a line ") + (across ? "across row " : "down column ") +
                                       std::to_string(across ? y : x)));
        }
}

void merges(const Canvas& canvas, bool ice, std::vector<Finding>& out) {
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x) {
            const Cell& c = canvas.at(x, y);
            if (c.transparent || !textGlyph(c.ch))
                continue;
            const int fg = c.fg.paletteIndex();
            if (fg == shownBg(c, ice))
                out.push_back(make(Kind::Merge, x, y, x, y,
                                   "the glyph is in its own background colour " + std::to_string(fg) +
                                       ": it can't be seen"));
        }
}

void dottedDiagonals(const Pixels& p, std::vector<Finding>& out) {
    const int w = p.width(), h = p.height();
    auto same = [&](int x, int py, int c) { return p.at(x, py) == c; };
    auto corners = [&](int x, int py, int c) {
        return same(x - 1, py - 1, c) + same(x + 1, py - 1, c) + same(x - 1, py + 1, c) + same(x + 1, py + 1, c);
    };
    // a pixel of a 1 px diagonal line: no side neighbour of its colour, one
    // or two corner ones
    auto dot = [&](int x, int py) {
        const int c = p.at(x, py);
        if (c < 0 || same(x - 1, py, c) || same(x + 1, py, c) || same(x, py - 1, c) || same(x, py + 1, c))
            return false;
        const int n = corners(x, py, c);
        return n >= 1 && n <= 2;
    };
    std::vector<char> seen(std::size_t(w) * std::size_t(h), 0);
    std::vector<std::pair<int, int>> stack, line;
    for (int py = 0; py < h; ++py)
        for (int x = 0; x < w; ++x) {
            if (seen[std::size_t(py) * std::size_t(w) + std::size_t(x)] || !dot(x, py))
                continue;
            const int c = p.at(x, py);
            line.clear();
            stack.assign(1, {x, py});
            seen[std::size_t(py) * std::size_t(w) + std::size_t(x)] = 1;
            while (!stack.empty()) {
                const auto [cx, cy] = stack.back();
                stack.pop_back();
                line.push_back({cx, cy});
                for (const int dy : {-1, 1})
                    for (const int dx : {-1, 1}) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || p.at(nx, ny) != c)
                            continue;
                        char& s = seen[std::size_t(ny) * std::size_t(w) + std::size_t(nx)];
                        if (s || !dot(nx, ny))
                            continue;
                        s = 1;
                        stack.push_back({nx, ny});
                    }
            }
            if (line.size() < 4)
                continue;
            // top end to bottom end, in reading order of the pixels
            const auto [first, last] = std::minmax_element(line.begin(), line.end(), [](const auto& a, const auto& b) {
                return std::pair{a.second, a.first} < std::pair{b.second, b.first};
            });
            Finding f = make(Kind::DottedDiagonal, first->first, first->second / 2, last->first, last->second / 2,
                             "a 1 px diagonal of " + std::to_string(line.size()) + " pixels in colour " +
                                 std::to_string(c) + " touching only at corners renders dotted; join it 4-connected");
            for (const auto& [px, py] : line)
                f.cells.push_back({px, py / 2});
            std::sort(f.cells.begin(), f.cells.end(), [](const auto& a, const auto& b) {
                return std::pair{a.second, a.first} < std::pair{b.second, b.first};
            });
            f.cells.erase(std::unique(f.cells.begin(), f.cells.end()), f.cells.end());
            out.push_back(std::move(f));
        }
}

// The box clipped to the canvas; empty if nothing is left.
std::optional<Box> clip(const Canvas& canvas, Box b) {
    if (b.x0 > b.x1)
        std::swap(b.x0, b.x1);
    if (b.y0 > b.y1)
        std::swap(b.y0, b.y1);
    b.x0 = std::max(b.x0, 0), b.y0 = std::max(b.y0, 0);
    b.x1 = std::min(b.x1, canvas.width() - 1), b.y1 = std::min(b.y1, canvas.height() - 1);
    if (b.x0 > b.x1 || b.y0 > b.y1)
        return std::nullopt;
    return b;
}

void flats(const Canvas& canvas, bool ice, const Box& box, int minCells, std::vector<Finding>& out) {
    const int w = box.x1 - box.x0 + 1, h = box.y1 - box.y0 + 1;
    if (w < 4 || h < 2)
        return;
    auto k = [w](int x, int y) { return std::size_t(y) * std::size_t(w) + std::size_t(x); };
    std::vector<Look> looks(std::size_t(w) * std::size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            looks[k(x, y)] = lookOf(canvas, box.x0 + x, box.y0 + y, ice);
    // run: the cells of one look from here to the right, up to a window's 4
    std::vector<int> run(looks.size(), 1);
    for (int y = 0; y < h; ++y)
        for (int x = w - 2; x >= 0; --x)
            if (looks[k(x, y)] == looks[k(x + 1, y)])
                run[k(x, y)] = std::min(4, run[k(x + 1, y)] + 1);
    // the cells in a 4x2-cell window of one look (not black), at every offset
    std::vector<char> flat(looks.size(), 0);
    for (int y = 0; y + 1 < h; ++y)
        for (int x = 0; x + 3 < w; ++x) {
            const Look& l = looks[k(x, y)];
            if (l == Look{U' ', Black, -1} || run[k(x, y)] < 4 || run[k(x, y + 1)] < 4 || looks[k(x, y + 1)] != l)
                continue;
            for (int yy = y; yy < y + 2; ++yy)
                for (int xx = x; xx < x + 4; ++xx)
                    flat[k(xx, yy)] = 1;
        }
    std::vector<char> seen(looks.size(), 0);
    std::vector<std::pair<int, int>> stack;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (seen[k(x, y)] || !flat[k(x, y)])
                continue;
            const Look l = looks[k(x, y)];
            int n = 0, x0 = x, x1 = x, y0 = y, y1 = y;
            seen[k(x, y)] = 1;
            stack.assign(1, {x, y});
            while (!stack.empty()) {
                const auto [cx, cy] = stack.back();
                stack.pop_back();
                ++n;
                x0 = std::min(x0, cx), x1 = std::max(x1, cx), y0 = std::min(y0, cy), y1 = std::max(y1, cy);
                for (const auto& [dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
                    const int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || seen[k(nx, ny)] || !flat[k(nx, ny)] ||
                        looks[k(nx, ny)] != l)
                        continue;
                    seen[k(nx, ny)] = 1;
                    stack.push_back({nx, ny});
                }
            }
            if (n < minCells)
                continue;
            out.push_back(make(Kind::Flat, box.x0 + x0, box.y0 + y0, box.x0 + x1, box.y0 + y1,
                               "a flat area of one look, " + std::to_string(n) + " cells in " +
                                   std::to_string(x1 - x0 + 1) + "x" + std::to_string(y1 - y0 + 1) +
                                   ": model the form or give it texture"));
        }
}

// The cells of row y the IRC encoding keeps: up to the last that isn't a
// black space (encodeCanvas).
int keptLength(const Canvas& canvas, int y) {
    int n = canvas.width();
    while (n > 0) {
        const Cell& c = canvas.at(n - 1, y);
        const bool space = c.transparent || c.ch == U' ' || c.ch == 0;
        if (!space || (!c.transparent && irc::mircColor(c.bg) != 1))
            break;
        --n;
    }
    return n;
}

void ircDrops(const Canvas& canvas, std::vector<Finding>& out) {
    const int w = canvas.width(), h = canvas.height();
    std::vector<int> kept(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        kept[std::size_t(y)] = keptLength(canvas, y);
    int top = 0, bottom = h;
    while (top < bottom && kept[std::size_t(top)] == 0)
        ++top;
    while (bottom > top && kept[std::size_t(bottom) - 1] == 0)
        --bottom;
    if (top == bottom)
        return;  // nothing drawn
    auto rows = [](int a, int b) {
        return a == b ? "row " + std::to_string(a) : "rows " + std::to_string(a) + "-" + std::to_string(b);
    };
    if (top > 0)
        out.push_back(make(Kind::BlankEdgeRows, 0, 0, w - 1, top - 1,
                           rows(0, top - 1) + " above the art are blank: IRC trims them (a margin needs a "
                                              "black-on-black block)"));
    for (int y = top; y < bottom; ++y) {
        const int n = kept[std::size_t(y)];
        if (n == w)
            continue;
        const bool edge = (y > 0 && kept[std::size_t(y) - 1] == w) || (y + 1 < h && kept[std::size_t(y) + 1] == w);
        if (!edge)
            continue;
        out.push_back(make(Kind::TrailingBlack, n, y, w - 1, y,
                           n == 0 ? "row " + std::to_string(y) +
                                        " is all black, beside rows that reach the edge: IRC shows it in the "
                                        "client's background"
                                  : "cells " + std::to_string(n) + "-" + std::to_string(w - 1) + " of row " +
                                        std::to_string(y) +
                                        " are black to the edge, beside rows that reach it: IRC drops them"));
    }
    if (bottom < h)
        out.push_back(make(Kind::BlankEdgeRows, 0, bottom, w - 1, h - 1,
                           rows(bottom, h - 1) + " below the art are blank: IRC trims them (a margin needs a "
                                                 "black-on-black block)"));
}

void noCp437(const Canvas& canvas, std::vector<Finding>& out) {
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x) {
            const Cell& c = canvas.at(x, y);
            if (c.transparent || c.ch < 0x20 || c.ch == 0x7f || unicodeToCp437(c.ch))
                continue;
            char code[16];
            std::snprintf(code, sizeof code, "U+%04X", static_cast<unsigned>(c.ch));
            std::string glyph;
            if (c.ch <= 0x10ffff && !(c.ch >= 0xd800 && c.ch <= 0xdfff))
                appendUtf8(glyph, c.ch);
            out.push_back(make(Kind::NoCp437, x, y, x, y,
                               std::string(code) + (glyph.empty() ? "" : " " + glyph) +
                                   " has no CP437 code: an .ans saves it as '?'"));
        }
}

Kind fromLadder(LadderFinding::Kind k) {
    switch (k) {
    case LadderFinding::Kind::NonNeighbourShade: return Kind::NonNeighbourShade;
    case LadderFinding::Kind::WideRung: return Kind::WideRung;
    case LadderFinding::Kind::RungSkip: return Kind::RungSkip;
    case LadderFinding::Kind::Speckle: return Kind::Speckle;
    case LadderFinding::Kind::FallbackSpeck: return Kind::FallbackSpeck;
    case LadderFinding::Kind::OneGlyph: return Kind::OneGlyph;
    case LadderFinding::Kind::Noise: return Kind::Noise;
    }
    return Kind::Noise;
}

// Whether a ramp linted has the shade's two colours as neighbours: then it
// is a rung of that ramp's ladder, whichever ramps don't have them so.
bool neighboursOnAny(const std::vector<Ramp>& ramps, const Cell& c) {
    return std::any_of(ramps.begin(), ramps.end(), [&](const Ramp& r) {
        const int f = r.indexOf(c.fg.index), b = r.indexOf(c.bg.index);
        return f >= 0 && b >= 0 && std::abs(f - b) == 1;
    });
}

bool craftShade(const LintOptions& o, const Cell& c) {
    return o.craftPairs && c.fg.isIndexed() && c.bg.isIndexed() && craftPair(c.fg.index, c.bg.index);
}

std::string shadeMessage(int x, int y, const Cell& c) {
    std::string glyph;
    appendUtf8(glyph, c.ch);
    return "cell " + std::to_string(x) + "," + std::to_string(y) + ": " + glyph + " of " +
           std::to_string(c.fg.index) + " on " + std::to_string(c.bg.index) +
           ", colours no ramp linted has as neighbours";
}

bool readingOrder(const Finding& a, const Finding& b) {
    return std::tuple{a.y, a.x, a.kind} < std::tuple{b.y, b.x, b.kind};
}

} // namespace

std::string_view kindName(Finding::Kind kind) {
    switch (kind) {
    case Kind::NonNeighbourShade: return "non-neighbour-shade";
    case Kind::WideRung: return "wide-rung";
    case Kind::RungSkip: return "rung-skip";
    case Kind::Speckle: return "speckle";
    case Kind::FallbackSpeck: return "fallback-speck";
    case Kind::OneGlyph: return "one-glyph";
    case Kind::Noise: return "noise";
    case Kind::LonePixel: return "lone-pixel";
    case Kind::Gap: return "gap";
    case Kind::Merge: return "merge";
    case Kind::DottedDiagonal: return "dotted-diagonal";
    case Kind::Flat: return "flat";
    case Kind::TrailingBlack: return "trailing-black";
    case Kind::BlankEdgeRows: return "blank-edge-rows";
    case Kind::NoCp437: return "no-cp437";
    }
    return "?";
}

bool craftPair(std::uint8_t a, std::uint8_t b) {
    if (a == Black || b == Black)
        return a != b && a != White && b != White;
    static const std::pair<std::uint8_t, std::uint8_t> steps[] = {
        {Blue, LightBlue},      // night b B
        {LightBlue, White},     // W▓B
        {Grey, DarkGrey},       // pale skin K w, the dark object's crown K▓w
        {Grey, White},          // pale skin w W
        {Red, LightRed},        // fire, red gloss r R
        {LightRed, Yellow},     // fire R Y
        {Red, Magenta},         // pockets m░r
        {Brown, LightRed},      // warm skin n R
        {Grey, LightRed},       // top planes R▓w, LRED to GREY
        {Green, LightGreen},    // green g G
        {Yellow, White},        // bright cloth Y W, Y▓W
        {Magenta, LightBlue},   // cool cloth B▓m
        {Cyan, LightBlue},      // cool cloth B▒c B▓c
        {Cyan, Grey},           // cool cloth c w
        {Magenta, DarkGrey},    // K▓m
        {Cyan, LightCyan},      // C░c
        {Red, Yellow},          // Y░r
        {Brown, Yellow},        // hair Y░n, crust Y▓n
        {DarkGrey, LightGreen}, // storm green G▒K
        {Red, DarkGrey},        // crest r▒K
        {Green, DarkGrey},      // storm swells k K g
    };
    return std::any_of(std::begin(steps), std::end(steps), [&](const auto& s) {
        return (s.first == a && s.second == b) || (s.first == b && s.second == a);
    });
}

bool ladderKind(Finding::Kind kind) { return kind <= Kind::OneGlyph; }

std::vector<Finding> checkRules(const Canvas& canvas, const RuleOptions& options) {
    std::vector<Finding> out;
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return out;
    const Pixels p(canvas, options.ice);
    lonePixels(p, out);
    gaps(canvas, options.ice, out);
    merges(canvas, options.ice, out);
    dottedDiagonals(p, out);
    if (options.box)
        if (const auto b = clip(canvas, *options.box))
            flats(canvas, options.ice, *b, std::max(1, options.minFlatCells), out);
    ircDrops(canvas, out);
    noCp437(canvas, out);
    std::stable_sort(out.begin(), out.end(), readingOrder);
    return out;
}

LintReport lintCanvas(const Canvas& canvas, const LintOptions& options) {
    LintReport r;
    r.ice = options.ice;
    for (const Ramp& ramp : options.ramps)
        if (const std::string why = rampProblem(ramp); !why.empty())
            throw LintError("ramp " + ramp.name + ": " + why);
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return r;
    const int w = canvas.width(), h = canvas.height();
    std::vector<Finding> all;
    std::set<std::tuple<Kind, int, int, int, int>> places;  // the ladder findings' places so far
    LadderOptions lo;
    lo.fallback = true;
    lo.ice = options.ice;
    lo.maxNoise = 2.0;  // noise is judged once below, not per ramp
    for (const Ramp& ramp : options.ramps) {
        for (const LadderFinding& lf : checkLadder(canvas, ramp, std::nullopt, lo).findings) {
            if (lf.kind == LadderFinding::Kind::Noise)
                continue;
            const bool shade = lf.kind == LadderFinding::Kind::NonNeighbourShade;
            if (shade && (neighboursOnAny(options.ramps, canvas.at(lf.x, lf.y)) ||
                          craftShade(options, canvas.at(lf.x, lf.y))))
                continue;
            Finding f;
            f.kind = fromLadder(lf.kind);
            f.ramp = ramp.name;
            f.x = lf.x, f.y = lf.y;
            const bool other = lf.kind == LadderFinding::Kind::WideRung || lf.kind == LadderFinding::Kind::RungSkip;
            f.x2 = other ? lf.nx : lf.x, f.y2 = other ? lf.ny : lf.y;
            if (lf.kind == LadderFinding::Kind::OneGlyph)
                f.x2 = w - 1, f.y2 = h - 1;  // about the whole canvas
            f.message = shade ? shadeMessage(lf.x, lf.y, canvas.at(lf.x, lf.y)) : lf.message;
            if (places.insert({f.kind, f.x, f.y, f.x2, f.y2}).second)
                all.push_back(std::move(f));
        }
    }
    r.noise = noise(canvas);
    if (r.noise > options.maxNoise) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "pixel noise %.3f is over %.3f: bands, not speckle", r.noise,
                      options.maxNoise);
        all.push_back(make(Kind::Noise, 0, 0, w - 1, h - 1, msg));
    }
    RuleOptions ro;
    ro.ice = options.ice;
    ro.box = options.box;
    for (Finding& f : checkRules(canvas, ro))
        all.push_back(std::move(f));
    if (options.box) {
        Box b = *options.box;
        if (b.x0 > b.x1)
            std::swap(b.x0, b.x1);
        if (b.y0 > b.y1)
            std::swap(b.y0, b.y1);
        const auto in = [&](int x, int y) { return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1; };
        std::erase_if(all, [&](const Finding& f) {
            if (!f.cells.empty())
                return std::none_of(f.cells.begin(), f.cells.end(), [&](const auto& c) { return in(c.first, c.second); });
            const int x0 = std::min(f.x, f.x2), x1 = std::max(f.x, f.x2);
            const int y0 = std::min(f.y, f.y2), y1 = std::max(f.y, f.y2);
            return x1 < b.x0 || x0 > b.x1 || y1 < b.y0 || y0 > b.y1;
        });
    }
    std::stable_sort(all.begin(), all.end(), readingOrder);
    r.findings = std::move(all);
    return r;
}

std::string summary(const LintReport& r) {
    const std::size_t n = r.findings.size();
    std::string out = n == 0 ? std::string("clean") : std::to_string(n) + (n == 1 ? " finding" : " findings");
    std::map<Finding::Kind, int> byKind;
    for (const Finding& f : r.findings)
        ++byKind[f.kind];
    if (!byKind.empty()) {
        out += " (";
        for (auto it = byKind.begin(); it != byKind.end(); ++it)
            out += std::string(it == byKind.begin() ? "" : ", ") + std::to_string(it->second) + " " +
                   std::string(kindName(it->first));
        out += ")";
    }
    char noise[32];
    std::snprintf(noise, sizeof noise, "%.4f", r.noise);
    return out + ", noise " + noise + ", iCE " + (r.ice ? "on" : "off");
}

} // namespace bw::lint
