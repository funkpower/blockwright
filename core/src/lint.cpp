#include "blockwright/lint.h"

#include "lint_fallback.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <set>
#include <tuple>
#include <utility>

namespace bw::lint {

namespace {

// The box clipped to the canvas; empty if nothing is left.
std::optional<Box> clip(const Canvas& canvas, std::optional<Box> box) {
    Box b = box.value_or(Box{0, 0, canvas.width() - 1, canvas.height() - 1});
    if (b.x0 > b.x1)
        std::swap(b.x0, b.x1);
    if (b.y0 > b.y1)
        std::swap(b.y0, b.y1);
    b.x0 = std::max(b.x0, 0);
    b.y0 = std::max(b.y0, 0);
    b.x1 = std::min(b.x1, canvas.width() - 1);
    b.y1 = std::min(b.y1, canvas.height() - 1);
    if (b.x0 > b.x1 || b.y0 > b.y1)
        return std::nullopt;
    return b;
}

std::string at(int x, int y) { return "cell " + std::to_string(x) + "," + std::to_string(y); }

thread_local long long readCount = 0;
#ifdef BW_TEST_HOOKS
thread_local long long indexCount = 0;
#endif

bool solidRung(int g) { return g % kRungsPerStep == 0; }
bool blackRung(const Ramp& ramp, int g) {
    return solidRung(g) && ramp.colours[static_cast<size_t>(g / kRungsPerStep)] == Black;
}

LadderFinding wideFinding(int x, int y, int width, int maxRung, int nx, int ny) {
    return {LadderFinding::Kind::WideRung, x, y,
            at(x, y) + ": a rung " + std::to_string(width) + " cells wide (at most " + std::to_string(maxRung) + ")",
            nx, ny};
}

// A shade of two colours of this ramp that aren't neighbours, judged on its
// colours, blinking or not. Other colours are another ramp's.
bool nonNeighbourShade(const Ramp& ramp, const Cell& c) {
    const bool shade = c.ch == kShadeLight || c.ch == kShadeMedium || c.ch == kShadeDark;
    if (!shade || !c.fg.isIndexed() || !c.bg.isIndexed() || c.fg.index == c.bg.index)
        return false;
    const int f = ramp.indexOf(c.fg.index), bi = ramp.indexOf(c.bg.index);
    return f >= 0 && bi >= 0 && std::abs(f - bi) != 1;
}

LadderFinding shadeFinding(int x, int y) {
    return {LadderFinding::Kind::NonNeighbourShade, x, y, at(x, y) + ": a shade of two colours that aren't ramp neighbours"};
}

// A speck: g against its neighbours' rungs (-1 off the ladder or the
// canvas). Empty when it isn't one; true when it is lighter than all of them.
std::optional<bool> speck(const Ramp& ramp, int g, const int (&nb)[4]) {
    if (g < 0 || blackRung(ramp, g) || g == ramp.topRung())
        return std::nullopt;
    int seen = 0, above = 0, below = 0;
    for (const int n : nb) {
        if (n < 0)
            continue;
        ++seen;
        above += n > g;
        below += n < g;
    }
    if (seen >= 3 && (above == seen || below == seen))
        return below == seen;
    return std::nullopt;
}

// A speck that is the gradient's dither (ditherExempt), or else speck().
std::optional<bool> speck(const Ramp& ramp, int g, const int (&nb)[4], bool dithered) {
    if (ditherExempt(dithered, g, nb))
        return std::nullopt;
    return speck(ramp, g, nb);
}

LadderFinding speckFinding(int x, int y, bool lighter) {
    return {LadderFinding::Kind::Speckle, x, y,
            at(x, y) + ": a speck, " + (lighter ? "lighter" : "darker") + " than every neighbour"};
}

LadderFinding skipFinding(int x, int y, int a, int c, int nx, int ny) {
    return {LadderFinding::Kind::RungSkip, x, y,
            at(x, y) + ": rung " + std::to_string(a) + " next to rung " + std::to_string(c) + " at " + at(nx, ny) +
                " (more than one rung apart)",
            nx, ny};
}

// The box's findings as a whole, at its corner.
LadderFinding oneGlyphFinding(int x, int y) {
    return {LadderFinding::Kind::OneGlyph, x, y, "the shading uses one glyph only"};
}

LadderFinding noiseFinding(int x, int y, double noise, double maxNoise) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "noise %.3f (at most %.3f)", noise, maxNoise);
    return {LadderFinding::Kind::Noise, x, y, buf};
}

// A width rule of 0 cells judges one-cell runs, whose step across a row and
// down a column end at the same cell, so the two can't be told apart.
void checkOptions(const LadderOptions& options) {
    if (options.maxRung < 1)
        throw LintError("maxRung " + std::to_string(options.maxRung) + " is below 1");
}

// The line rules checkLadder and the reader at a change share, over a grid
// of rungs. G has w and h, rung(x, y) (-1 off the ladder; on the grid
// only), and across(x, y, cap) and down(x, y, cap): the run through a cell
// across its row and down its column, counted no further than cap.
template <class G>
class Rules {
public:
    Rules(const Ramp& ramp, const G& grid, int maxRung) : ramp_(ramp), g_(grid), max_(maxRung) {}

    // -1 off the ladder or the grid
    int rung(int x, int y) const { return x < 0 || y < 0 || x >= g_.w || y >= g_.h ? -1 : g_.rung(x, y); }

    // The run [x, end) across row y is a step of a gradient (below) long
    // enough to be wide.
    bool rowStep(int x, int end, int y) const {
        const auto cellAt = [y](int i) { return std::pair{i, y}; };
        const auto runAt = [this](int cx, int cy) { return g_.across(cx, cy, g_.w); };
        return judged(rung(x, y), end - x, g_.w, x, end - 1, cellAt, runAt);
    }
    bool columnStep(int x, int y, int end) const {
        const auto cellAt = [x](int i) { return std::pair{x, i}; };
        const auto runAt = [this](int cx, int cy) { return g_.down(cx, cy, g_.h); };
        return judged(rung(x, y), end - y, g_.h, y, end - 1, cellAt, runAt);
    }
    // The width of the run [x, end) across row y where it is a wide rung, 0
    // where it isn't.
    int rowWidth(int x, int end, int y) const {
        if (!rowStep(x, end, y))
            return 0;
        const int len = end - x;
        int tallest = 0;
        for (int xx = x; xx < end && tallest < len; ++xx)
            tallest = std::max(tallest, g_.down(xx, y, len));
        const int width = std::min(len, tallest);
        return width > max_ ? width : 0;
    }
    // emit(x, end, width) for each wide run across row y.
    template <class Emit>
    void rowWide(int y, Emit&& emit) const {
        for (int x = 0, end = 0; x < g_.w; x = end) {
            end = x + g_.across(x, y, g_.w);
            if (const int width = rowWidth(x, end, y))
                emit(x, end, width);
        }
    }
    // emit(y, end, width) for each wide run down column x that no row
    // reports a cell of (reported(x, y)).
    template <class Reported, class Emit>
    void columnWide(int x, Reported&& reported, Emit&& emit) const {
        for (int y = 0, end = 0; y < g_.h; y = end) {
            end = y + g_.down(x, y, g_.h);
            if (!columnStep(x, y, end))
                continue;
            const int len = end - y;
            int widest = 0;
            for (int yy = y; yy < end && widest < len; ++yy)
                widest = std::max(widest, g_.across(x, yy, len));
            const int width = std::min(len, widest);
            bool seen = false;
            for (int yy = y; yy < end && width > max_ && !seen; ++yy)
                seen = reported(x, yy);
            if (width > max_ && !seen)
                emit(y, end, width);
        }
    }
    // emit(i, j, a, c, first, last) for each skip along row y (rowSkips) or
    // column x: i the last cell of the level at rung a, j the first of the
    // next, at c; the level at a starts at first and the one at c ends at
    // last.
    template <class Emit>
    void rowSkips(int y, Emit&& emit) const {
        skips(g_.w, [y](int i) { return std::pair{i, y}; }, emit);
    }
    template <class Emit>
    void columnSkips(int x, Emit&& emit) const {
        skips(g_.h, [x](int i) { return std::pair{x, i}; }, emit);
    }

private:
    // a gradient ends here: off the ladder or black
    bool ends(int g) const { return g < 0 || blackRung(ramp_, g); }
    // Outside the shape, for the end-level rule: off the grid, off the
    // ladder or black.
    bool outsideAt(int x, int y) const { return ends(rung(x, y)); }

    // An outline at a gradient's end takes no part in it (PLAN §10, "the
    // end-level rule"): a run g of cells along a line (cellAt), at the end
    // of its sequence, `outer` its cell against the outside and `inner` its
    // cell against the level `beside` it. g must be the outline colour of
    // that level (outlineRung) and the run one cell thick along the line, or
    // two where they are a step of a staircase: the inner cell a filler,
    // none of its four neighbours outside; the run not half of a 2x2 block
    // of g, unless the block's other half is a line against the outside (the
    // corner where a staircase meets a straight side); and, on one side across the line, the outside beside the outer
    // cell, the line going on beside the inner cell, and on the other side
    // the line going on beside the outer cell. So the two cells join a run
    // of g one cell further out on one side to a run one cell further in on
    // the other: a 4-connected diagonal line, two cells on a row. Where the
    // level inside changes along the line, the line may go on in another
    // line colour: any solid colour but black against the outside. A band two
    // cells deep, a solid corner and a dotted outer edge are not an outline
    // but a rung like the rest. Off the grid the line may go on.
    template <class CellAt>
    bool outlineRun(int g, int beside, int outer, int inner, CellAt&& cellAt) const {
        const auto line = beside >= 0 ? outlineRung(ramp_, beside) : std::nullopt;
        if (!line || g != *line)
            return false;
        if (outer == inner)
            return true;
        if (std::abs(outer - inner) != 1)
            return false;
        const auto [ox, oy] = cellAt(outer);
        const auto [ix, iy] = cellAt(inner);
        if (outsideAt(ix - 1, iy) || outsideAt(ix + 1, iy) || outsideAt(ix, iy - 1) || outsideAt(ix, iy + 1))
            return false;
        const auto off = [&](int x, int y) { return x < 0 || y < 0 || x >= g_.w || y >= g_.h; };
        const auto is = [&](int x, int y) { return !off(x, y) && rung(x, y) == g; };
        // the line goes on in g, or in another line colour where the level
        // inside it changes (buildings.md: a CYAN rim, LCYAN on the lit
        // arc): a solid colour, not black, against the outside
        const auto goesOn = [&](int x, int y) {
            if (off(x, y) || rung(x, y) == g)
                return true;
            const int o = rung(x, y);
            if (o < 0 || !solidRung(o) || ends(o))
                return false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dy) && outsideAt(x + dx, y + dy))
                        return true;
            return false;
        };
        // across the line, one way and the other
        const int px = oy - iy, py = ox - ix;
        for (const int s : {1, -1})
            if (is(ox + s * px, oy + s * py) && is(ix + s * px, iy + s * py) &&
                !(outsideAt(ox + 2 * s * px, oy + 2 * s * py) && outsideAt(ix + 2 * s * px, iy + 2 * s * py)))
                return false;
        for (const int s : {1, -1})
            if (outsideAt(ox + s * px, oy + s * py) && goesOn(ix + s * px, iy + s * py) &&
                goesOn(ox - s * px, oy - s * py))
                return true;
        return false;
    }

    // The rung of the run before (dir -1) or after (dir 1) the run i0..i1 of
    // g along a line of n cells, or -1 when the gradient ends there: the
    // line's end, black, off the ladder, or an outline.
    template <class CellAt, class RunAt>
    int beyond(int n, int i0, int i1, int g, int dir, CellAt&& cellAt, RunAt&& runAt) const {
        const int i = dir < 0 ? i0 - 1 : i1 + 1;
        if (i < 0 || i >= n)
            return -1;
        const auto [x, y] = cellAt(i);
        const int o = rung(x, y);
        if (ends(o))
            return -1;
        const int len = runAt(x, y), j0 = dir < 0 ? i - len + 1 : i, j1 = dir < 0 ? i : i + len - 1;
        const int past = dir < 0 ? j0 - 1 : j1 + 1;
        const bool atEnd = past < 0 || past >= n || ends(rung(cellAt(past).first, cellAt(past).second));
        return atEnd && outlineRun(o, g, dir < 0 ? j0 : j1, dir < 0 ? j1 : j0, cellAt) ? -1 : o;
    }

    // Only a step of a gradient is judged for width: a run of a shade rung
    // with a lower rung at one end and a higher one at the other.
    template <class CellAt, class RunAt>
    bool judged(int g, int len, int n, int i0, int i1, CellAt&& cellAt, RunAt&& runAt) const {
        if (len <= max_ || g < 0 || solidRung(g))
            return false;
        const int before = beyond(n, i0, i1, g, -1, cellAt, runAt), after = beyond(n, i0, i1, g, 1, cellAt, runAt);
        return before >= 0 && after >= 0 && ((before < g && g < after) || (before > g && g > after));
    }

    template <class CellAt, class Emit>
    void skips(int n, CellAt&& cellAt, Emit&& emit) const {
        struct Level {
            int g, first, last;  // the rung, and its run's ends along the line
        };
        std::vector<Level> levels;
        auto judge = [&] {
            if (levels.size() >= 2 &&
                outlineRun(levels.back().g, levels[levels.size() - 2].g, levels.back().last, levels.back().first, cellAt))
                levels.pop_back();
            if (levels.size() >= 2 && outlineRun(levels.front().g, levels[1].g, levels.front().first, levels.front().last, cellAt))
                levels.erase(levels.begin());
            for (size_t k = 0; k + 1 < levels.size(); ++k) {
                const int a = levels[k].g, c = levels[k + 1].g;
                if (std::abs(c - a) < 2)
                    continue;
                const bool up = c > a;
                const bool before = k > 0 && (a > levels[k - 1].g) == up;
                const bool after = k + 2 < levels.size() && (levels[k + 2].g > c) == up;
                if (before || after)
                    emit(levels[k].last, levels[k + 1].first, a, c, levels[k].first, levels[k + 1].last);
            }
            levels.clear();
        };
        for (int i = 0; i < n; ++i) {
            const auto [x, y] = cellAt(i);
            const int g = rung(x, y);
            if (ends(g)) {
                judge();
                continue;
            }
            if (!levels.empty() && levels.back().g == g)
                levels.back().last = i;
            else
                levels.push_back({g, i, i});
        }
        judge();
    }

    const Ramp& ramp_;
    const G& g_;
    int max_;
};

// checkLadder's box: its rungs and runs, read once.
struct BoxGrid {
    int w, h;
    std::vector<int> rungs, acrossRun, downRun;
    int rung(int x, int y) const { return rungs[static_cast<size_t>(y) * w + x]; }
    int across(int x, int y, int cap) const { return std::min(acrossRun[static_cast<size_t>(y) * w + x], cap); }
    int down(int x, int y, int cap) const { return std::min(downRun[static_cast<size_t>(y) * w + x], cap); }
};

} // namespace

std::optional<int> outlineRung(const Ramp& ramp, int rung) {
    const int below = rung / kRungsPerStep - 1;
    if (rung < 0 || below < 0 || below >= int(ramp.colours.size()) || ramp.colours[static_cast<size_t>(below)] == Black)
        return std::nullopt;
    return below * kRungsPerStep;
}

LadderFinding::Place LadderFinding::place() const {
    if (kind == Kind::WideRung)
        return ny == y ? Place{kind, x, y, x + 1, y} : Place{kind, x, y, x, y + 1};
    return {kind, x, y, nx, ny};
}

std::vector<std::vector<std::optional<std::uint8_t>>> pixels(const Canvas& canvas, std::optional<Box> box) {
    std::vector<std::vector<std::optional<std::uint8_t>>> grid;
    const auto b = clip(canvas, box);
    if (!b)
        return grid;
    for (int y = b->y0; y <= b->y1; ++y) {
        std::vector<std::optional<std::uint8_t>> top, bot;
        for (int x = b->x0; x <= b->x1; ++x) {
            const Cell& c = canvas.at(x, y);
            const std::uint8_t fg = c.fg.paletteIndex(), bg = c.bg.paletteIndex();
            std::optional<std::uint8_t> t, u;
            switch (c.ch.codePointOr(kNoCodePoint)) {
            case kFullBlock: t = u = fg; break;
            case U' ': t = u = bg; break;
            case kUpperHalf: t = fg, u = bg; break;
            case kLowerHalf: t = bg, u = fg; break;
            default: break;
            }
            top.push_back(t);
            bot.push_back(u);
        }
        grid.push_back(std::move(top));
        grid.push_back(std::move(bot));
    }
    return grid;
}

double noise(const Canvas& canvas, std::optional<Box> box) {
    const auto b = clip(canvas, box);
    if (!b)
        return 0.0;
    auto grid = pixels(canvas, *b);
    // As in the written file: the writer trims each row's
    // trailing black spaces: those pixels aren't there.
    for (int y = b->y0; y <= b->y1; ++y) {
        int len = canvas.width();
        while (len > 0 && canvas.at(len - 1, y).ch == U' ' && canvas.at(len - 1, y).bg.paletteIndex() == Black)
            --len;
        const size_t keep = static_cast<size_t>(std::clamp(len - b->x0, 0, b->x1 - b->x0 + 1));
        for (const int half : {0, 1})
            grid[static_cast<size_t>(2 * (y - b->y0) + half)].resize(keep);
    }
    auto px = [&](size_t y, size_t x) -> std::optional<std::uint8_t> {
        return x < grid[y].size() ? grid[y][x] : std::nullopt;
    };
    int lonely = 0, total = 0;
    for (size_t y = 1; y + 1 < grid.size(); ++y)
        for (size_t x = 1; x + 1 < grid[y].size(); ++x) {
            const auto c = grid[y][x];
            if (!c || *c == Black)
                continue;
            const std::optional<std::uint8_t> around[] = {px(y - 1, x), px(y + 1, x), grid[y][x - 1], grid[y][x + 1]};
            if (std::any_of(std::begin(around), std::end(around), [](const auto& a) { return !a; }))
                continue;
            ++total;
            lonely += std::all_of(std::begin(around), std::end(around), [&](const auto& a) { return a != c; });
        }
    return total ? double(lonely) / total : 0.0;
}

Detail detail(const Canvas& canvas, std::optional<Box> box) {
    Detail d;
    // The rows as in the written file: the writer trims
    // each row's trailing black spaces, and blank rows at the top and the
    // bottom go. The box indexes what is left.
    std::vector<int> trimmed;
    for (int y = 0; y < canvas.height(); ++y) {
        int n = canvas.width();
        while (n > 0 && canvas.at(n - 1, y).ch == U' ' && canvas.at(n - 1, y).bg.paletteIndex() == Black)
            --n;
        trimmed.push_back(n);
    }
    int top = 0, bottom = canvas.height();
    while (top < bottom && trimmed[size_t(top)] == 0)
        ++top;
    while (bottom > top && trimmed[size_t(bottom) - 1] == 0)
        --bottom;
    Box b{0, 0, canvas.width() - 1, bottom - top - 1};
    if (box)
        b = Box{std::max(0, box->x0), std::max(0, box->y0), std::min(canvas.width() - 1, box->x1),
                std::min(bottom - top - 1, box->y1)};
    if (b.x0 > b.x1 || b.y0 > b.y1)
        return d;
    b.y0 += top, b.y1 += top;
    std::vector<int> len;
    int longest = 0;
    for (int y = b.y0; y <= b.y1; ++y) {
        len.push_back(std::clamp(trimmed[size_t(y)] - b.x0, 0, b.x1 - b.x0 + 1));
        longest = std::max(longest, len.back());
    }
    // a look: the background of a space, the foreground of a full block,
    // or the glyph and both colours
    using Look = std::tuple<Glyph, int, int>;
    auto look = [&](int x, int y) {
        const Cell& c = canvas.at(x, y);
        if (c.ch == U' ')
            return Look{U' ', c.bg.paletteIndex(), -1};
        if (c.ch == kFullBlock)
            return Look{U' ', c.fg.paletteIndex(), -1};
        return Look{c.ch, c.fg.paletteIndex(), c.bg.paletteIndex()};
    };
    const Look black{U' ', Black, -1};
    long total = 0;
    for (int y = 0; y + 1 < static_cast<int>(len.size()); y += 2)
        for (int x = 0; x + 3 < longest; x += 4) {
            if (len[size_t(y)] < x + 4 || len[size_t(y) + 1] < x + 4)
                continue;
            Look win[8];
            int n = 0, dark = 0;
            for (int yy = y; yy <= y + 1; ++yy)
                for (int xx = x; xx < x + 4; ++xx) {
                    win[n] = look(b.x0 + xx, b.y0 + yy);
                    dark += win[n++] == black;
                }
            if (dark > 4)
                continue;
            // distinct looks by equality: a glyph has no order (PLAN §16)
            int distinct = 0;
            for (int i = 0; i < 8; ++i)
                distinct += std::find(std::begin(win), std::begin(win) + i, win[i]) == std::begin(win) + i;
            ++d.windows;
            d.flat += distinct == 1;
            total += distinct;
        }
    if (d.windows) {
        d.mean = std::nearbyint(100.0 * double(total) / d.windows) / 100.0;
        d.flatPercent = static_cast<int>(std::nearbyint(100.0 * d.flat / d.windows));
    }
    return d;
}

bool ditherExempt(bool dithered, int g, const int (&nb)[4]) {
    if (!dithered || g < 0)
        return false;
    for (const int n : nb)
        if (n >= 0 && std::abs(n - g) != 1)
            return false;
    return true;
}

LadderReport checkLadder(const Canvas& canvas, const Ramp& ramp, std::optional<Box> box, const LadderOptions& options) {
    checkOptions(options);
    LadderReport r;
    const auto b = clip(canvas, box);
    if (!b)
        return r;
    const int w = b->x1 - b->x0 + 1, h = b->y1 - b->y0 + 1;
    // rung per box cell, -1 off the ladder
    BoxGrid grid{w, h, std::vector<int>(static_cast<size_t>(w) * h, -1), {}, {}};
    auto& rung = grid.rungs;
    auto rungAt = [&](int x, int y) { return rung[static_cast<size_t>(y) * w + x]; };
    std::set<char32_t> glyphs;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Cell& c = canvas.at(b->x0 + x, b->y0 + y);
            ++r.cells;
            ++readCount;
            if (const auto g = rungOf(ramp, c)) {
                rung[static_cast<size_t>(y) * w + x] = *g;
                ++r.ladderCells;
                if (*g % kRungsPerStep) {
                    ++r.shadeCells;
                    glyphs.insert(c.ch.codePoint().value());  // a shade: a code point
                }
            }
            if (nonNeighbourShade(ramp, c))
                r.findings.push_back(shadeFinding(b->x0 + x, b->y0 + y));
        }

    // The length of the run across the row and down the column each cell
    // is in.
    auto& across = grid.acrossRun;
    auto& down = grid.downRun;
    across.assign(rung.size(), 0);
    down.assign(rung.size(), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w;) {
            int end = x + 1;
            while (end < w && rungAt(end, y) == rungAt(x, y))
                ++end;
            for (int xx = x; xx < end; ++xx)
                across[static_cast<size_t>(y) * w + xx] = end - x;
            x = end;
        }
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h;) {
            int end = y + 1;
            while (end < h && rungAt(x, end) == rungAt(x, y))
                ++end;
            for (int yy = y; yy < end; ++yy)
                down[static_cast<size_t>(yy) * w + x] = end - y;
            y = end;
        }
    const Rules<BoxGrid> rules(ramp, grid, options.maxRung);

    // Rungs wider than maxRung along the gradient. Only a step of a
    // gradient is judged: a run of a shade rung with a lower rung at one end
    // and a higher one at the other, across the row or down the column. A
    // flat field and the plane at a gradient's end have no width to keep;
    // black is ink or the ground, not the dark end of a gradient, so a plane
    // that runs into it is a plane at the end too. The gradient runs across
    // the rung, so a step's width is the shorter of its run and the rung's
    // longest run the other way, in cells ("1-3 cells wide
    // along the gradient"). An outline at the gradient's end (as for skips,
    // below) ends it too: the plane inside it is the end plane. Reported
    // once per run, at its left or top end; a run down a column only when no
    // cell of it was reported across a row.
    std::vector<char> reported(rung.size(), 0);
    for (int y = 0; y < h; ++y)
        rules.rowWide(y, [&](int x, int end, int width) {
            r.findings.push_back(wideFinding(b->x0 + x, b->y0 + y, width, options.maxRung, b->x0 + end - 1, b->y0 + y));
            std::fill(reported.begin() + static_cast<long>(y) * w + x, reported.begin() + static_cast<long>(y) * w + end, 1);
        });
    for (int x = 0; x < w; ++x)
        rules.columnWide(
            x, [&](int cx, int cy) { return reported[static_cast<size_t>(cy) * w + cx] != 0; },
            [&](int y, int end, int width) {
                r.findings.push_back(wideFinding(b->x0 + x, b->y0 + y, width, options.maxRung, b->x0 + x, b->y0 + end - 1));
            });

    // Skipped rungs: along a row or a column, the ramp jumping more than one
    // rung in the middle of a gradient. Runs of one rung are a level; a jump
    // between two levels is a skip when the level before it or the one after
    // it goes on the same way, so three or more levels climb (or fall)
    // together. An outline that turns back and two planes meeting at a fold
    // (two levels, solid or not) are edges, not skips. Black and cells off
    // the ramp end a gradient: black against anything is ink, a seam or the
    // silhouette. An outline at either end of a sequence (against black,
    // off-ramp or the box edge) takes no part in the gradient (outlineRun).
    // Any other end level, a lighter one (a highlight) or a band two cells
    // deep included, is a rung like the rest.
    // Reported once per pair of cells, at the left or top one.
    for (int y = 0; y < h; ++y)
        rules.rowSkips(y, [&](int i, int j, int a, int c, int, int) {
            r.findings.push_back(skipFinding(b->x0 + i, b->y0 + y, a, c, b->x0 + j, b->y0 + y));
        });
    for (int x = 0; x < w; ++x)
        rules.columnSkips(x, [&](int i, int j, int a, int c, int, int) {
            r.findings.push_back(skipFinding(b->x0 + x, b->y0 + i, a, c, b->x0 + x, b->y0 + j));
        });

    // Speckle ("a lone non-black, non-glint pixel"): a ladder
    // cell lighter than every one of its neighbours on the ladder, or darker
    // than every one. Black is ink (a seam, an eye, a rivet, an outline) and
    // the ramp's top rung is a glint or the highlight, so neither is a speck.
    // A cell is judged only with three or more neighbours on the ladder, so
    // the corners of a gradient, the ends of a band and a cell in a canvas
    // corner aren't; a black neighbour counts. A lone pixel, a lone shade in
    // a flat field and a checkerboard of two shades are specks. Stripes of
    // two shades, a column or a row each, are not: every cell has a
    // neighbour of its own rung. That is a known gap; pixel noise
    // doesn't see shade cells either, so for now only the eye
    // catches them.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int nb[4] = {rules.rung(x, y - 1), rules.rung(x, y + 1), rules.rung(x - 1, y), rules.rung(x + 1, y)};
            if (const auto lighter = speck(ramp, rungAt(x, y), nb, canvas.at(b->x0 + x, b->y0 + y).dithered))
                r.findings.push_back(speckFinding(b->x0 + x, b->y0 + y, *lighter));
        }
    // fallback specks (lint_fallback.cpp)
    std::vector<char> fallback;
    for (int y = b->y0; y <= b->y1 && options.fallback; ++y) {
        fallback::row(canvas, ramp, *b, y, options.ice, fallback, readCount);
        for (int x = 0; x < w; ++x)
            if (fallback[size_t(x)])
                r.findings.push_back(fallback::finding(b->x0 + x, y));
    }

    if (!options.whole)
        return r;
    if (r.shadeCells >= 4 && glyphs.size() == 1)
        r.findings.push_back(oneGlyphFinding(b->x0, b->y0));
    r.noise = noise(canvas, *b);
    if (r.noise > options.maxNoise)
        r.findings.push_back(noiseFinding(b->x0, b->y0, r.noise, options.maxNoise));
    return r;
}

namespace {

// A cell's top (half 0) or bottom (half 1) pixel, as pixels() reads it.
std::optional<std::uint8_t> pixelOf(const Cell& c, int half) {
    const std::uint8_t fg = c.fg.paletteIndex(), bg = c.bg.paletteIndex();
    switch (c.ch.codePointOr(kNoCodePoint)) {
    case kFullBlock: return fg;
    case U' ': return bg;
    case kUpperHalf: return half ? bg : fg;
    case kLowerHalf: return half ? fg : bg;
    default: return std::nullopt;
    }
}

// A trailing black space, which the writer trims (as noise() reads it).
bool blank(const Cell& c) { return c.ch == U' ' && c.bg.paletteIndex() == Black; }

// The shade glyph a cell adds to the one-glyph count, if any.
std::optional<char32_t> shadeGlyph(const Ramp& ramp, const Cell& c) {
    const auto g = rungOf(ramp, c);
    if (g && *g % kRungsPerStep)
        return c.ch.codePoint();  // a shade: a code point
    return std::nullopt;
}

} // namespace

namespace {

// The index's rungs and runs as the line rules read them.
struct IndexGrid {
    int w, h;
    const std::vector<int>&rungs, &rowFirst, &rowEnd, &columnFirst, &columnEnd;
    int rung(int x, int y) const {
        ++readCount;
        return rungs[static_cast<std::size_t>(y * w + x)];
    }
    int across(int x, int y, int cap) const {
        ++readCount;
        const auto i = static_cast<std::size_t>(y * w + x);
        return std::min(rowEnd[i] - rowFirst[i], cap);
    }
    int down(int x, int y, int cap) const {
        ++readCount;
        const auto i = static_cast<std::size_t>(y * w + x);
        return std::min(columnEnd[i] - columnFirst[i], cap);
    }
};

} // namespace

void LadderIndex::Highest::reset(std::size_t n) {
    levels.clear();
    do {
        n = std::max<std::size_t>(1, (n + 63) / 64);
        levels.emplace_back(n, 0);
    } while (n > 1);
}

void LadderIndex::Highest::set(std::size_t i, bool on) {
    for (auto& level : levels) {
        std::uint64_t& word = level[i / 64];
        const bool was = word != 0;
        const std::uint64_t bit = std::uint64_t(1) << (i % 64);
        word = on ? word | bit : word & ~bit;
        // the level above has this word's bit already, or still needs it
        if (on ? was : word != 0)
            return;
        i /= 64;
    }
}

long long LadderIndex::Highest::below(std::size_t i) const {
    std::size_t level = 0;
    for (;; ++level) {
        if (level == levels.size())
            return -1;
        const std::size_t bit = i % 64;
        const std::uint64_t mask = bit == 63 ? ~std::uint64_t(0) : (std::uint64_t(2) << bit) - 1;
        if (const std::uint64_t word = levels[level][i / 64] & mask) {
            i = i / 64 * 64 + std::size_t(std::bit_width(word) - 1);
            break;
        }
        if (i < 64)
            return -1;
        i = i / 64 - 1;  // the words before this one, a level up
    }
    while (level > 0) {
        --level;
        i = i * 64 + std::size_t(std::bit_width(levels[level][i]) - 1);
    }
    return static_cast<long long>(i);
}

LadderIndex::LadderIndex(const Canvas& canvas, const Ramp& ramp, const LadderOptions& options)
    : canvas_(canvas.width(), canvas.height()), ramp_(ramp), options_(options), w_(canvas.width()),
      h_(canvas.height()) {
    checkOptions(options);
#ifdef BW_TEST_HOOKS
    ++indexCount;
#endif
    const auto n = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    for (auto* v : {&rung_, &rowSlot_, &columnSlot_})
        v->assign(n, -1);
    for (auto* v : {&rowFirst_, &rowEnd_, &columnFirst_, &columnEnd_, &rowCount_, &columnCount_, &seen_})
        v->assign(n, 0);
    for (auto* v : {&judgedAcross_, &speck_, &shade_, &fallback_})
        v->assign(n, 0);
    rowWide_.assign(n, {});
    columnWide_.assign(n, {});
    rowHighest_.reset(n);
    columnHighest_.reset(n);
    rowSkips_.assign(static_cast<std::size_t>(h_), {});
    columnSkips_.assign(static_cast<std::size_t>(w_), {});
    length_.assign(static_cast<std::size_t>(h_), 0);
    pixel_.assign(2 * n, -1);
    counts_.assign(2 * n, 0);
    for (auto* v : {&cellMark_, &rowRunMark_, &columnRunMark_, &rowWideMark_, &columnWideMark_, &speckMark_, &shadeMark_, &fallbackMark_})
        v->assign(n, 0);
    pixelMark_.assign(2 * n, 0);
    rowMark_.assign(static_cast<std::size_t>(h_), 0);
    columnMark_.assign(static_cast<std::size_t>(w_), 0);
    nearRowMark_.assign(static_cast<std::size_t>(h_), 0);
    nearColumnMark_.assign(static_cast<std::size_t>(w_), 0);
    // everything starts empty (the blank canvas_ holds no shade, pixel or
    // run yet), and the canvas comes in as one change of every cell
    std::vector<std::pair<int, Cell>> cells;
    cells.reserve(n);
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x)
            cells.push_back({y * w_ + x, canvas.at(x, y)});
    update(cells);
}

int LadderIndex::rungAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= w_ || y >= h_)
        return -1;
    ++readCount;
    return rung_[static_cast<std::size_t>(y * w_ + x)];
}

int LadderIndex::pixelAt(int p, int x) const {
    ++readCount;
    return pixel_[static_cast<std::size_t>(p * w_ + x)];
}

// What pixel (p, x) counts towards the noise, as noise() over the whole
// canvas reads it.
int LadderIndex::contribution(int p, int x) const {
    if (p < 1 || p + 1 >= 2 * h_)
        return 0;
    const int len = length_[static_cast<std::size_t>(p / 2)];
    if (x < 1 || x + 1 >= len)
        return 0;
    const int c = pixelAt(p, x);
    if (c < 0 || c == Black)
        return 0;
    const int up = x < length_[static_cast<std::size_t>((p - 1) / 2)] ? pixelAt(p - 1, x) : -1;
    const int down = x < length_[static_cast<std::size_t>((p + 1) / 2)] ? pixelAt(p + 1, x) : -1;
    const int around[] = {up, down, pixelAt(p, x - 1), pixelAt(p, x + 1)};
    if (std::any_of(std::begin(around), std::end(around), [](int a) { return a < 0; }))
        return 0;
    return 1 + std::all_of(std::begin(around), std::end(around), [&](int a) { return a != c; });
}

void LadderIndex::bump(bool row, int slot, int by) {
    auto& count = (row ? rowCount_ : columnCount_)[static_cast<std::size_t>(slot)];
    auto& highest = row ? rowHighest_ : columnHighest_;
    ++readCount;
    count += by;
    if (count == 0 || (count == 1 && by > 0))
        highest.set(static_cast<std::size_t>(slot), count > 0);
}

void LadderIndex::placeRow(int i) {
    const int y = i / w_, a = rowFirst_[size_t(i)], len = rowEnd_[size_t(i)] - a;
    const int slot = y * w_ + a + std::min(columnEnd_[size_t(i)] - columnFirst_[size_t(i)], len) - 1;
    ++readCount;
    if (slot == rowSlot_[size_t(i)])
        return;
    if (rowSlot_[size_t(i)] >= 0)
        bump(true, rowSlot_[size_t(i)], -1);
    bump(true, slot, 1);
    rowSlot_[size_t(i)] = slot;
}

void LadderIndex::placeColumn(int i) {
    const int x = i % w_, a = columnFirst_[size_t(i)], len = columnEnd_[size_t(i)] - a;
    const int slot = x * h_ + a + std::min(rowEnd_[size_t(i)] - rowFirst_[size_t(i)], len) - 1;
    ++readCount;
    if (slot == columnSlot_[size_t(i)])
        return;
    if (columnSlot_[size_t(i)] >= 0)
        bump(false, columnSlot_[size_t(i)], -1);
    bump(false, slot, 1);
    columnSlot_[size_t(i)] = slot;
}

// The row's run starting at `start` is one the width rule judges: a shade
// rung's run longer than maxRung with a lower rung at one end and a higher
// one at the other.
bool LadderIndex::judgedRow(int start) const {
    const IndexGrid grid{w_, h_, rung_, rowFirst_, rowEnd_, columnFirst_, columnEnd_};
    const int y = start / w_, a = start % w_;
    return Rules<IndexGrid>(ramp_, grid, options_.maxRung).rowStep(a, rowEnd_[size_t(start)], y);
}

LadderIndex::Wide LadderIndex::judgeRow(int start) const {
    if (!judgedRow(start))
        return {};
    const int y = start / w_, a = start % w_, b = rowEnd_[size_t(start)];
    ++readCount;
    const int width = int(rowHighest_.below(size_t(y * w_ + b - 1)) - (y * w_ + a)) + 1;
    return width > options_.maxRung ? Wide{width, b - 1, rungAt(a, y)} : Wide{};
}

// A column's judged run is reported only when no row reports one of its
// cells, and a row reports every cell of its judged runs that a judged
// column run holds (that cell's run down is longer than maxRung).
LadderIndex::Wide LadderIndex::judgeColumn(int start) const {
    const int x = start % w_, a = start / w_, b = columnEnd_[size_t(start)], g = rungAt(x, a);
    const IndexGrid grid{w_, h_, rung_, rowFirst_, rowEnd_, columnFirst_, columnEnd_};
    if (seen_[size_t(start)] > 0 || !Rules<IndexGrid>(ramp_, grid, options_.maxRung).columnStep(x, a, b))
        return {};
    ++readCount;
    const int width = int(columnHighest_.below(size_t(x * h_ + b - 1)) - (x * h_ + a)) + 1;
    return width > options_.maxRung ? Wide{width, b - 1, g} : Wide{};
}

int LadderIndex::speckAt(int x, int y) const {
    const int nb[4] = {rungAt(x, y - 1), rungAt(x, y + 1), rungAt(x - 1, y), rungAt(x + 1, y)};
    const auto lighter = speck(ramp_, rungAt(x, y), nb, canvas_.at(x, y).dithered);
    return lighter ? (*lighter ? 1 : 2) : 0;
}

std::vector<LadderIndex::Skip> LadderIndex::judgeSkips(bool row, int line) const {
    std::vector<Skip> out;
    auto emit = [&](int i, int j, int a, int c, int first, int last) { out.push_back({i, j, a, c, first, last}); };
    const IndexGrid grid{w_, h_, rung_, rowFirst_, rowEnd_, columnFirst_, columnEnd_};
    const Rules<IndexGrid> rules(ramp_, grid, options_.maxRung);
    if (row)
        rules.rowSkips(line, emit);
    else
        rules.columnSkips(line, emit);
    return out;
}

const std::vector<LadderIndex::Skip>& LadderIndex::skips(bool across, int line) const {
    return across ? rowSkips_.at(size_t(line)) : columnSkips_.at(size_t(line));
}

// A skip is one of `was` between the same two cells, or between the same two
// rungs with each level sharing a cell with the other's: a level that grew
// or shrank at the boundary moves the skip, and it is the skip that was. A
// line's skips are in order along it and their first levels don't overlap,
// so the old and new lists merge in one pass.
std::vector<LadderFinding> LadderIndex::skipsAdded(bool across, int line, const std::vector<Skip>& was) const {
    std::vector<LadderFinding> out;
    std::size_t p = 0;
    for (const Skip& s : skips(across, line)) {
        while (p < was.size() && (++readCount, was[p].i < s.first))
            ++p;
        bool kept = false;
        for (std::size_t q = p; !kept && q < was.size() && (++readCount, was[q].first <= s.i); ++q)
            kept = s.matches(was[q]);
        if (!kept)
            out.push_back(skipAt(across, line, s));
    }
    return out;
}

LadderFinding LadderIndex::skipAt(bool row, int line, const Skip& s) const {
    return row ? skipFinding(s.i, line, s.a, s.c, s.j, line) : skipFinding(line, s.i, s.a, s.c, line, s.j);
}

std::vector<LadderFinding> LadderIndex::globals() const {
    std::vector<LadderFinding> out;
    if (!options_.whole)
        return out;
    if (shadeCells_ >= 4 && glyphs_.size() == 1)
        out.push_back(oneGlyphFinding(0, 0));
    if (noise() > options_.maxNoise)
        out.push_back(noiseFinding(0, 0, noise(), options_.maxNoise));
    return out;
}

std::vector<LadderFinding> LadderIndex::update(const std::vector<std::pair<int, Cell>>& cells) {
    const unsigned e = ++epoch_;
    auto mark = [&](std::vector<unsigned>& marks, int i) {
        if (marks[size_t(i)] == e)
            return false;
        marks[size_t(i)] = e;
        return true;
    };
    const bool wasOneGlyph = shadeCells_ >= 4 && glyphs_.size() == 1;
    const double wasNoise = noise();
    std::vector<int> rows, columns, cellsAt, specks, pixels, rowRuns, columnRuns;
    std::vector<std::pair<int, Wide>> rowWas, columnWas;
    std::vector<std::pair<int, char>> speckWas, shadeWas;
    // per changed row, past its last cell that isn't a trailing blank
    std::vector<int> grow(size_t(h_), 0);
    auto markPixel = [&](int p, int x) {
        if (p >= 0 && x >= 0 && p < 2 * h_ && x < w_ && mark(pixelMark_, p * w_ + x))
            pixels.push_back(p * w_ + x);
    };

    // the cells themselves: rung, pixels and glyph
    for (const auto& [i, c] : cells) {
        const int x = i % w_, y = i / w_;
        ++readCount;
        Cell& held = canvas_.at(x, y);
        if (const auto glyph = shadeGlyph(ramp_, held)) {
            --shadeCells_;
            if (--glyphs_[*glyph] == 0)
                glyphs_.erase(*glyph);
        }
        held = c;
        if (const auto glyph = shadeGlyph(ramp_, held)) {
            ++shadeCells_;
            ++glyphs_[*glyph];
        }
        rung_[size_t(i)] = rungOf(ramp_, held).value_or(-1);
        for (const int half : {0, 1}) {
            const auto px = pixelOf(held, half);
            pixel_[size_t((2 * y + half) * w_ + x)] = px ? int(*px) : -1;
        }
        if (mark(rowMark_, y)) {
            rows.push_back(y);
            grow[size_t(y)] = length_[size_t(y)];
        }
        if (mark(columnMark_, x))
            columns.push_back(x);
        if (!blank(held))
            grow[size_t(y)] = std::max(grow[size_t(y)], x + 1);
        if (mark(cellMark_, i))
            cellsAt.push_back(i);
        for (const auto& [dx, dy] : {std::pair{0, 0}, {0, -1}, {0, 1}, {-1, 0}, {1, 0}})
            if (x + dx >= 0 && y + dy >= 0 && x + dx < w_ && y + dy < h_ && mark(speckMark_, (y + dy) * w_ + x + dx)) {
                specks.push_back((y + dy) * w_ + x + dx);
                speckWas.push_back({(y + dy) * w_ + x + dx, speck_[size_t((y + dy) * w_ + x + dx)]});
            }
        for (int p = 2 * y - 1; p <= 2 * y + 2; ++p)
            markPixel(p, x);
        for (const int p : {2 * y, 2 * y + 1})
            for (const int dx : {-1, 1})
                markPixel(p, x + dx);
    }
    const auto isRow = [&](int y) { return rowMark_[size_t(y)] == e; };
    const auto isColumn = [&](int x) { return columnMark_[size_t(x)] == e; };
    // The end-level rule reads two cells across a line each way (an
    // outline's staircase step and the line going on beside it), so the
    // rows and columns within two of a changed one judge their runs and
    // skips again too, their runs unchanged.
    std::vector<int> nearRows, nearColumns;
    for (const int y : rows)
        for (int yy = std::max(0, y - 2); yy <= std::min(h_ - 1, y + 2); ++yy)
            if (!isRow(yy) && mark(nearRowMark_, yy))
                nearRows.push_back(yy);
    for (const int x : columns)
        for (int xx = std::max(0, x - 2); xx <= std::min(w_ - 1, x + 2); ++xx)
            if (!isColumn(xx) && mark(nearColumnMark_, xx))
                nearColumns.push_back(xx);

    // each changed row's trimmed length: past its last cell that isn't a
    // trailing blank, which can only have moved back over blanks
    for (const int y : rows) {
        const int was = length_[size_t(y)];
        int len = grow[size_t(y)];
        while (len > 0 && (++readCount, blank(canvas_.at(len - 1, y))))
            --len;
        length_[size_t(y)] = len;
        if (len != was)
            for (int p = 2 * y - 1; p <= 2 * y + 2; ++p)
                for (int x = std::min(len, was) - 2; x <= std::max(len, was) + 1; ++x)
                    markPixel(p, x);
    }

    // runs across the changed rows and down the changed columns
    for (const int y : rows)
        for (int x = 0, end = 0; x < w_; x = end) {
            const int g = rungAt(x, y);
            for (end = x + 1; end < w_ && rungAt(end, y) == g;)
                ++end;
            for (int xx = x; xx < end; ++xx) {
                rowFirst_[size_t(y * w_ + xx)] = x;
                rowEnd_[size_t(y * w_ + xx)] = end;
            }
        }
    for (const int x : columns)
        for (int y = 0, end = 0; y < h_; y = end) {
            const int g = rungAt(x, y);
            for (end = y + 1; end < h_ && rungAt(x, end) == g;)
                ++end;
            for (int yy = y; yy < end; ++yy) {
                columnFirst_[size_t(yy * w_ + x)] = y;
                columnEnd_[size_t(yy * w_ + x)] = end;
            }
        }
    auto rowRun = [&](int x, int y) {
        if (const int s = y * w_ + rowFirst_[size_t(y * w_ + x)]; mark(rowRunMark_, s))
            rowRuns.push_back(s);
    };
    auto columnRun = [&](int x, int y) {
        if (const int s = columnFirst_[size_t(y * w_ + x)] * w_ + x; mark(columnRunMark_, s))
            columnRuns.push_back(s);
    };

    // A changed row's cells have new runs across, which the columns through
    // them read, and a changed column's cells new runs down, which the rows
    // read: rebucket them and judge the runs that read them again.
    for (const int y : rows)
        for (int x = 0; x < w_; ++x) {
            placeRow(y * w_ + x);
            placeColumn(y * w_ + x);
            if (!isColumn(x))
                columnRun(x, y);
        }
    for (const int x : columns)
        for (int y = 0; y < h_; ++y) {
            placeRow(y * w_ + x);
            placeColumn(y * w_ + x);
            if (!isRow(y))
                rowRun(x, y);
        }

    // the runs of the lines near a change, judged again
    for (const int y : nearRows)
        for (int x = 0; x < w_; x = rowEnd_[size_t(y * w_ + x)])
            rowRun(x, y);
    for (const int x : nearColumns)
        for (int y = 0; y < h_; y = columnEnd_[size_t(y * w_ + x)])
            columnRun(x, y);

    // which cells' runs across are judged, and how many of each column
    // run's cells are
    std::vector<int> judgedRows = rows;
    judgedRows.insert(judgedRows.end(), nearRows.begin(), nearRows.end());
    for (const int y : judgedRows)
        for (int x = 0; x < w_;) {
            const int end = rowEnd_[size_t(y * w_ + x)];
            const char judged = judgedRow(y * w_ + x);
            for (; x < end; ++x) {
                char& was = judgedAcross_[size_t(y * w_ + x)];
                if (was == judged)
                    continue;
                if (!isColumn(x)) {
                    seen_[size_t(columnFirst_[size_t(y * w_ + x)] * w_ + x)] += judged ? 1 : -1;
                    columnRun(x, y);
                }
                was = judged;
            }
        }
    for (const int x : columns)
        for (int y = 0; y < h_;) {
            const int start = y * w_ + x, end = columnEnd_[size_t(start)];
            int n = 0;
            for (; y < end; ++y)
                n += judgedAcross_[size_t(y * w_ + x)];
            seen_[size_t(start)] = n;
        }

    // the wide steps of the changed lines and of the runs that read them
    auto keepRow = [&](int i) {
        if (mark(rowWideMark_, i))
            rowWas.push_back({i, rowWide_[size_t(i)]});
    };
    auto keepColumn = [&](int i) {
        if (mark(columnWideMark_, i))
            columnWas.push_back({i, columnWide_[size_t(i)]});
    };
    // and a changed line's wide steps as they were, in order along it
    struct Span {
        int first, last, rung;
    };
    std::vector<std::vector<Span>> rowSpans(rows.size()), columnSpans(columns.size());
    for (std::size_t k = 0; k < rows.size(); ++k)
        for (int x = 0, y = rows[k]; x < w_; ++x) {
            keepRow(y * w_ + x);
            if (const Wide was = rowWide_[size_t(y * w_ + x)]; was.width)
                rowSpans[k].push_back({x, was.last, was.rung});
            rowWide_[size_t(y * w_ + x)] = {};
        }
    for (const int y : rows)
        for (int x = 0; x < w_; x = rowEnd_[size_t(y * w_ + x)])
            rowWide_[size_t(y * w_ + x)] = judgeRow(y * w_ + x);
    for (const int s : rowRuns) {
        keepRow(s);
        rowWide_[size_t(s)] = judgeRow(s);
    }
    for (std::size_t k = 0; k < columns.size(); ++k)
        for (int y = 0, x = columns[k]; y < h_; ++y) {
            keepColumn(y * w_ + x);
            if (const Wide was = columnWide_[size_t(y * w_ + x)]; was.width)
                columnSpans[k].push_back({y, was.last, was.rung});
            columnWide_[size_t(y * w_ + x)] = {};
        }
    for (const int x : columns)
        for (int y = 0; y < h_; y = columnEnd_[size_t(y * w_ + x)])
            columnWide_[size_t(y * w_ + x)] = judgeColumn(y * w_ + x);
    for (const int s : columnRuns) {
        keepColumn(s);
        columnWide_[size_t(s)] = judgeColumn(s);
    }

    // A skip is added unless it matches one the line had (skipsAdded).
    std::vector<LadderFinding> out;
    auto added = [&](bool row, int line, std::vector<Skip>& held) {
        std::vector<Skip> was = std::exchange(held, judgeSkips(row, line));
        for (auto& f : skipsAdded(row, line, was))
            out.push_back(std::move(f));
    };
    for (const int y : rows)
        added(true, y, rowSkips_[size_t(y)]);
    for (const int y : nearRows)
        added(true, y, rowSkips_[size_t(y)]);
    for (const int x : columns)
        added(false, x, columnSkips_[size_t(x)]);
    for (const int x : nearColumns)
        added(false, x, columnSkips_[size_t(x)]);

    for (const int i : cellsAt) {
        shadeWas.push_back({i, shade_[size_t(i)]});
        shade_[size_t(i)] = nonNeighbourShade(ramp_, canvas_.at(i % w_, i / w_));
    }
    for (const int i : specks)
        speck_[size_t(i)] = char(speckAt(i % w_, i / w_));
    // A fallback speck reads its run along the row and the cells above and
    // below it: a changed row is judged again whole, and the cells above
    // and below a changed cell on the rows that didn't change.
    std::vector<std::pair<int, char>> fallbackWas;
    const Box all{0, 0, w_ - 1, h_ - 1};
    auto fallbackIs = [&](int i, bool is) {
        if (mark(fallbackMark_, i))
            fallbackWas.push_back({i, fallback_[size_t(i)]});
        fallback_[size_t(i)] = is;
    };
    std::vector<char> fallbackRow;
    for (const int y : options_.fallback ? rows : std::vector<int>{}) {
        fallback::row(canvas_, ramp_, all, y, options_.ice, fallbackRow, readCount);
        for (int x = 0; x < w_; ++x)
            fallbackIs(y * w_ + x, fallbackRow[size_t(x)]);
    }
    for (const int i : options_.fallback ? cellsAt : std::vector<int>{})
        for (const int y : {i / w_ - 1, i / w_ + 1})
            if (y >= 0 && y < h_ && !isRow(y))
                fallbackIs(y * w_ + i % w_,
                           fallback::speckAt(canvas_, ramp_, all, i % w_, y, options_.ice, readCount));

    for (const int i : pixels) {
        const int p = i / w_, x = i % w_;
        auto& was = counts_[size_t(i)];
        const int is = contribution(p, x);
        total_ += (is > 0) - (was > 0);
        lonely_ += (is > 1) - (was > 1);
        was = static_cast<signed char>(is);
    }

    // A wide step is added unless the line had one the same way, of the
    // same rung, sharing a cell with it: narrowed or widened at either end,
    // two runs joined or one split, it is the step that was. On a changed
    // line the steps before and now merge in one pass along it, both in
    // order and neither overlapping itself. A line that didn't change keeps
    // its runs, so there a step shares cells only with its own run's slot.
    auto widened = [&](bool row, int line, const std::vector<Span>& was) {
        std::size_t p = 0;
        for (int k = 0, n = row ? w_ : h_; k < n;) {
            const auto i = size_t(row ? line * w_ + k : k * w_ + line);
            const Wide is = (row ? rowWide_ : columnWide_)[i];
            const int next = row ? rowEnd_[i] : columnEnd_[i];
            ++readCount;
            if (is.width) {
                while (p < was.size() && (++readCount, was[p].last < k))
                    ++p;
                bool kept = false;
                for (std::size_t q = p; !kept && q < was.size() && (++readCount, was[q].first <= is.last); ++q)
                    kept = was[q].rung == is.rung;
                if (!kept)
                    out.push_back(row ? wideFinding(k, line, is.width, options_.maxRung, is.last, line)
                                      : wideFinding(line, k, is.width, options_.maxRung, line, is.last));
            }
            k = next;
        }
    };
    for (std::size_t k = 0; k < rows.size(); ++k)
        widened(true, rows[k], rowSpans[k]);
    for (std::size_t k = 0; k < columns.size(); ++k)
        widened(false, columns[k], columnSpans[k]);
    for (const auto& [i, was] : rowWas)
        if (const Wide is = rowWide_[size_t(i)]; !isRow(i / w_) && is.width && !was.width)
            out.push_back(wideFinding(i % w_, i / w_, is.width, options_.maxRung, is.last, i / w_));
    for (const auto& [i, was] : columnWas)
        if (const Wide is = columnWide_[size_t(i)]; !isColumn(i % w_) && is.width && !was.width)
            out.push_back(wideFinding(i % w_, i / w_, is.width, options_.maxRung, i % w_, is.last));
    // A cell's speck or shade is one place: added where the cell had none.
    for (const auto& [i, was] : speckWas)
        if (const char is = speck_[size_t(i)]; is && !was)
            out.push_back(speckFinding(i % w_, i / w_, is == 1));
    for (const auto& [i, was] : shadeWas)
        if (shade_[size_t(i)] && !was)
            out.push_back(shadeFinding(i % w_, i / w_));
    for (const auto& [i, was] : fallbackWas)
        if (fallback_[size_t(i)] && !was)
            out.push_back(fallback::finding(i % w_, i / w_));
    if (options_.whole) {
        if (shadeCells_ >= 4 && glyphs_.size() == 1 && !wasOneGlyph)
            out.push_back(oneGlyphFinding(0, 0));
        if (noise() > options_.maxNoise && noise() > wasNoise)
            out.push_back(noiseFinding(0, 0, noise(), options_.maxNoise));
    }
    return out;
}

bool LadderIndex::differsOnlyAt(const Canvas& after, const std::vector<std::pair<int, int>>& changed) const {
    if (after.width() != w_ || after.height() != h_)
        return false;
    std::vector<char> named(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0);
    for (const auto& [x, y] : changed)
        if (x >= 0 && y >= 0 && x < w_ && y < h_)
            named[size_t(y * w_ + x)] = 1;
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x)
            if (!named[size_t(y * w_ + x)] && !identical(after.at(x, y), canvas_.at(x, y)))
                return false;
    return true;
}

std::vector<LadderFinding> LadderIndex::apply(const Canvas& after, const std::vector<std::pair<int, int>>& changed) {
    if (after.width() != w_ || after.height() != h_)
        throw LintError("a " + std::to_string(after.width()) + "x" + std::to_string(after.height()) +
                        " canvas for an index of a " + std::to_string(w_) + "x" + std::to_string(h_) + " one");
#ifndef NDEBUG
    if (!differsOnlyAt(after, changed))
        throw LintError("the canvas differs at a cell the change doesn't name");
#endif
    std::vector<std::pair<int, Cell>> cells;
    cells.reserve(changed.size());
    for (const auto& [x, y] : changed)
        if (x >= 0 && y >= 0 && x < w_ && y < h_) {
            ++readCount;
            cells.push_back({y * w_ + x, after.at(x, y)});
        }
    return update(cells);
}

std::vector<LadderFinding> LadderIndex::added(const Canvas& after, const std::vector<std::pair<int, int>>& changed) {
    std::vector<std::pair<int, Cell>> back;
    back.reserve(changed.size());
    for (const auto& [x, y] : changed)
        if (x >= 0 && y >= 0 && x < w_ && y < h_) {
            ++readCount;
            back.push_back({y * w_ + x, canvas_.at(x, y)});
        }
    auto out = apply(after, changed);
    update(back);
    return out;
}

bool LadderIndex::has(const LadderFinding& f) const {
    using Kind = LadderFinding::Kind;
    if (f.kind == Kind::OneGlyph || f.kind == Kind::Noise) {
        const auto all = globals();
        return std::find(all.begin(), all.end(), f) != all.end();
    }
    if (f.x < 0 || f.y < 0 || f.x >= w_ || f.y >= h_)
        return false;
    const auto i = size_t(f.y * w_ + f.x);
    switch (f.kind) {
    case Kind::NonNeighbourShade: return shade_[i] && f == shadeFinding(f.x, f.y);
    case Kind::Speckle: return speck_[i] && f == speckFinding(f.x, f.y, speck_[i] == 1);
    case Kind::FallbackSpeck: return fallback_[i] && f == fallback::finding(f.x, f.y);
    case Kind::WideRung:
        return (rowWide_[i].width &&
                f == wideFinding(f.x, f.y, rowWide_[i].width, options_.maxRung, rowWide_[i].last, f.y)) ||
               (columnWide_[i].width &&
                f == wideFinding(f.x, f.y, columnWide_[i].width, options_.maxRung, f.x, columnWide_[i].last));
    case Kind::RungSkip:
        for (const Skip& s : rowSkips_[size_t(f.y)])
            if (f == skipAt(true, f.y, s))
                return true;
        for (const Skip& s : columnSkips_[size_t(f.x)])
            if (f == skipAt(false, f.x, s))
                return true;
        return false;
    default: return false;
    }
}

std::vector<LadderFinding> LadderIndex::wideRungs(bool across, int line) const {
    std::vector<LadderFinding> out;
    if (line < 0 || line >= (across ? h_ : w_))
        return out;
    for (int k = 0; k < (across ? w_ : h_); ++k) {
        const int x = across ? k : line, y = across ? line : k;
        const auto i = size_t(y * w_ + x);
        ++readCount;
        if (across && rowWide_[i].width)
            out.push_back(wideFinding(x, y, rowWide_[i].width, options_.maxRung, rowWide_[i].last, y));
        if (!across && columnWide_[i].width)
            out.push_back(wideFinding(x, y, columnWide_[i].width, options_.maxRung, x, columnWide_[i].last));
    }
    return out;
}

std::vector<LadderFinding> LadderIndex::findings() const {
    std::vector<LadderFinding> out;
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const auto i = size_t(y * w_ + x);
            if (shade_[i])
                out.push_back(shadeFinding(x, y));
            if (rowWide_[i].width)
                out.push_back(wideFinding(x, y, rowWide_[i].width, options_.maxRung, rowWide_[i].last, y));
            if (columnWide_[i].width)
                out.push_back(wideFinding(x, y, columnWide_[i].width, options_.maxRung, x, columnWide_[i].last));
            if (speck_[i])
                out.push_back(speckFinding(x, y, speck_[i] == 1));
            if (fallback_[i])
                out.push_back(fallback::finding(x, y));
        }
    for (int y = 0; y < h_; ++y)
        for (const Skip& s : rowSkips_[size_t(y)])
            out.push_back(skipAt(true, y, s));
    for (int x = 0; x < w_; ++x)
        for (const Skip& s : columnSkips_[size_t(x)])
            out.push_back(skipAt(false, x, s));
    const auto all = globals();
    out.insert(out.end(), all.begin(), all.end());
    return out;
}

std::vector<LadderFinding> addedFindingsOnce(const Canvas& before, const Canvas& after, const Ramp& ramp,
                                             const std::vector<std::pair<int, int>>& changed,
                                             const LadderOptions& options) {
    LadderIndex index(before, ramp, options);
    return index.apply(after, changed);
}

long long cellsRead() { return readCount; }
#ifdef BW_TEST_HOOKS
long long indexesBuilt() { return indexCount; }
#endif

} // namespace bw::lint
