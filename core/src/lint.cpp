#include "blockwright/lint.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <set>

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

bool solidRung(int g) { return g % kRungsPerStep == 0; }
bool blackRung(const Ramp& ramp, int g) {
    return solidRung(g) && ramp.colours[static_cast<size_t>(g / kRungsPerStep)] == Black;
}

// A step of a gradient: a lower rung at one end and a higher one at the
// other (-1 for none). Black is ink or the ground, not the dark end of a
// gradient, so a run against it has no neighbour there.
bool isStep(const Ramp& ramp, int before, int g, int after) {
    if (before >= 0 && blackRung(ramp, before))
        before = -1;
    if (after >= 0 && blackRung(ramp, after))
        after = -1;
    return before >= 0 && after >= 0 && ((before < g && g < after) || (before > g && g > after));
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

LadderFinding speckFinding(int x, int y, bool lighter) {
    return {LadderFinding::Kind::Speckle, x, y,
            at(x, y) + ": a speck, " + (lighter ? "lighter" : "darker") + " than every neighbour"};
}

// The skips along a line of n cells, rungAt(i) its rungs (-1 off the
// ladder): emit(i, j, a, c) for each, i the last cell of the level at rung
// a and j the first of the next, at rung c.
template <class RungAt, class Emit>
void skipsAlong(const Ramp& ramp, int n, RungAt&& rungAt, Emit&& emit) {
    struct Level {
        int g, first, last;  // the rung, and its run's ends along the line
    };
    std::vector<Level> levels;
    // l is an end level and beside the level next to it
    auto outline = [&](const Level& l, const Level& beside) {
        return l.first == l.last && solidRung(l.g) && l.g == beside.g - beside.g % kRungsPerStep - kRungsPerStep;
    };
    auto judge = [&] {
        if (levels.size() >= 2 && outline(levels.back(), levels[levels.size() - 2]))
            levels.pop_back();
        if (levels.size() >= 2 && outline(levels.front(), levels[1]))
            levels.erase(levels.begin());
        for (size_t k = 0; k + 1 < levels.size(); ++k) {
            const int a = levels[k].g, c = levels[k + 1].g;
            if (std::abs(c - a) < 2)
                continue;
            const bool up = c > a;
            const bool before = k > 0 && (a > levels[k - 1].g) == up;
            const bool after = k + 2 < levels.size() && (levels[k + 2].g > c) == up;
            if (before || after)
                emit(levels[k].last, levels[k + 1].first, a, c);
        }
        levels.clear();
    };
    for (int i = 0; i < n; ++i) {
        const int g = rungAt(i);
        if (g < 0 || blackRung(ramp, g)) {
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

} // namespace

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
            switch (c.ch) {
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

LadderReport checkLadder(const Canvas& canvas, const Ramp& ramp, std::optional<Box> box, const LadderOptions& options) {
    checkOptions(options);
    LadderReport r;
    const auto b = clip(canvas, box);
    if (!b)
        return r;
    const int w = b->x1 - b->x0 + 1, h = b->y1 - b->y0 + 1;
    // rung per box cell, -1 off the ladder
    std::vector<int> rung(static_cast<size_t>(w) * h, -1);
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
                    if (options.whole)
                        glyphs.insert(c.ch);
                }
            }
            if (nonNeighbourShade(ramp, c))
                r.findings.push_back(shadeFinding(b->x0 + x, b->y0 + y));
        }

    auto solid = solidRung;

    // The length of the run across the row and down the column each cell
    // is in.
    std::vector<int> across(rung.size(), 0), down(rung.size(), 0);
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

    // Rungs wider than maxRung along the gradient. Only a step of a
    // gradient is judged: a run of a shade rung with a lower rung at one end
    // and a higher one at the other, across the row or down the column. A
    // flat field and the plane at a gradient's end have no width to keep;
    // black is ink or the ground, not the dark end of a gradient, so a plane
    // that runs into it is a plane at the end too. The gradient runs across
    // the rung, so a step's width is the shorter of its run and the rung's
    // longest run the other way, in cells ("1-3 cells wide
    // along the gradient"). Reported once per run, at its left or top end; a
    // run down a column only when no cell of it was reported across a row.
    std::vector<char> reported(rung.size(), 0);
    auto step = [&](int before, int g, int after) { return isStep(ramp, before, g, after); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; x += across[static_cast<size_t>(y) * w + x]) {
            const int g = rungAt(x, y), end = x + across[static_cast<size_t>(y) * w + x];
            if (g < 0 || solid(g) || !step(x > 0 ? rungAt(x - 1, y) : -1, g, end < w ? rungAt(end, y) : -1))
                continue;
            int tallest = 0;
            for (int xx = x; xx < end; ++xx)
                tallest = std::max(tallest, down[static_cast<size_t>(y) * w + xx]);
            if (const int width = std::min(end - x, tallest); width > options.maxRung) {
                r.findings.push_back(wideFinding(b->x0 + x, b->y0 + y, width, options.maxRung, b->x0 + end - 1, b->y0 + y));
                for (int xx = x; xx < end; ++xx)
                    reported[static_cast<size_t>(y) * w + xx] = 1;
            }
        }
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; y += down[static_cast<size_t>(y) * w + x]) {
            const int g = rungAt(x, y), end = y + down[static_cast<size_t>(y) * w + x];
            if (g < 0 || solid(g) || !step(y > 0 ? rungAt(x, y - 1) : -1, g, end < h ? rungAt(x, end) : -1))
                continue;
            int widest = 0;
            bool seen = false;
            for (int yy = y; yy < end; ++yy) {
                widest = std::max(widest, across[static_cast<size_t>(yy) * w + x]);
                seen = seen || reported[static_cast<size_t>(yy) * w + x];
            }
            if (const int width = std::min(end - y, widest); width > options.maxRung && !seen)
                r.findings.push_back(wideFinding(b->x0 + x, b->y0 + y, width, options.maxRung, b->x0 + x, b->y0 + end - 1));
        }

    // Skipped rungs: along a row or a column, the ramp jumping more than one
    // rung in the middle of a gradient. Runs of one rung are a level; a jump
    // between two levels is a skip when the level before it or the one after
    // it goes on the same way, so three or more levels climb (or fall)
    // together. An outline that turns back and two planes meeting at a fold
    // (two levels, solid or not) are edges, not skips. Black and cells off
    // the ramp end a gradient: black against anything is ink, a seam or the
    // silhouette. An outline in a ramp colour takes no part in the gradient:
    // a level at either end of a sequence (against black, off-ramp or the
    // canvas edge) that is one cell wide, solid, and the ramp colour next
    // below the darker colour of the level beside it, which is what the
    // outline brush draws. Any other end level, a lighter one (a highlight)
    // included, is a rung like the rest. Reported once per pair of cells, at
    // the left or top one.
    for (int y = 0; y < h; ++y)
        skipsAlong(ramp, w, [&](int i) { return rungAt(i, y); }, [&](int i, int j, int a, int c) {
            r.findings.push_back(skipFinding(b->x0 + i, b->y0 + y, a, c, b->x0 + j, b->y0 + y));
        });
    for (int x = 0; x < w; ++x)
        skipsAlong(ramp, h, [&](int i) { return rungAt(x, i); }, [&](int i, int j, int a, int c) {
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
    auto rungOr = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? -1 : rungAt(x, y); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int nb[4] = {rungOr(x, y - 1), rungOr(x, y + 1), rungOr(x - 1, y), rungOr(x + 1, y)};
            if (const auto lighter = speck(ramp, rungAt(x, y), nb))
                r.findings.push_back(speckFinding(b->x0 + x, b->y0 + y, *lighter));
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
    switch (c.ch) {
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
        return c.ch;
    return std::nullopt;
}

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
    const auto n = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    for (auto* v : {&rung_, &rowSlot_, &columnSlot_})
        v->assign(n, -1);
    for (auto* v : {&rowFirst_, &rowEnd_, &columnFirst_, &columnEnd_, &rowCount_, &columnCount_, &seen_})
        v->assign(n, 0);
    for (auto* v : {&judgedAcross_, &speck_, &shade_})
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
    for (auto* v : {&cellMark_, &rowRunMark_, &columnRunMark_, &rowWideMark_, &columnWideMark_, &speckMark_, &shadeMark_})
        v->assign(n, 0);
    pixelMark_.assign(2 * n, 0);
    rowMark_.assign(static_cast<std::size_t>(h_), 0);
    columnMark_.assign(static_cast<std::size_t>(w_), 0);
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
    const int y = start / w_, a = start % w_, b = rowEnd_[size_t(start)], g = rungAt(a, y);
    return g >= 0 && !solidRung(g) && b - a > options_.maxRung && isStep(ramp_, rungAt(a - 1, y), g, rungAt(b, y));
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
    if (g < 0 || solidRung(g) || b - a <= options_.maxRung || !isStep(ramp_, rungAt(x, a - 1), g, rungAt(x, b)) ||
        seen_[size_t(start)] > 0)
        return {};
    ++readCount;
    const int width = int(columnHighest_.below(size_t(x * h_ + b - 1)) - (x * h_ + a)) + 1;
    return width > options_.maxRung ? Wide{width, b - 1, g} : Wide{};
}

int LadderIndex::speckAt(int x, int y) const {
    const int nb[4] = {rungAt(x, y - 1), rungAt(x, y + 1), rungAt(x - 1, y), rungAt(x + 1, y)};
    const auto lighter = speck(ramp_, rungAt(x, y), nb);
    return lighter ? (*lighter ? 1 : 2) : 0;
}

std::vector<LadderIndex::Skip> LadderIndex::skips(bool row, int line) const {
    std::vector<Skip> out;
    auto emit = [&](int i, int j, int a, int c) { out.push_back({i, j, a, c}); };
    if (row)
        skipsAlong(ramp_, w_, [&](int i) { return rungAt(i, line); }, emit);
    else
        skipsAlong(ramp_, h_, [&](int i) { return rungAt(line, i); }, emit);
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

    // which cells' runs across are judged, and how many of each column
    // run's cells are
    for (const int y : rows)
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

    // A line's skips are in order along it, so the old and new lists merge
    // in one pass: a skip is added when no old one is between the same two
    // cells.
    std::vector<LadderFinding> out;
    auto added = [&](bool row, int line, std::vector<Skip>& was) {
        std::vector<Skip> is = skips(row, line);
        auto old = was.begin();
        for (const Skip& s : is) {
            while (old != was.end() && (++readCount, old->i < s.i || (old->i == s.i && old->j < s.j)))
                ++old;
            if (old == was.end() || old->i != s.i || old->j != s.j)
                out.push_back(skipAt(row, line, s));
        }
        was = std::move(is);
    };
    for (const int y : rows)
        added(true, y, rowSkips_[size_t(y)]);
    for (const int x : columns)
        added(false, x, columnSkips_[size_t(x)]);

    for (const int i : cellsAt) {
        shadeWas.push_back({i, shade_[size_t(i)]});
        shade_[size_t(i)] = nonNeighbourShade(ramp_, canvas_.at(i % w_, i / w_));
    }
    for (const int i : specks)
        speck_[size_t(i)] = char(speckAt(i % w_, i / w_));

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
            if (!named[size_t(y * w_ + x)] && !(after.at(x, y) == canvas_.at(x, y)))
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

} // namespace bw::lint
