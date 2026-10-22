#include "blockwright/lint.h"

#include <algorithm>
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

} // namespace

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
            if (const auto g = rungOf(ramp, c)) {
                rung[static_cast<size_t>(y) * w + x] = *g;
                ++r.ladderCells;
                if (*g % kRungsPerStep) {
                    ++r.shadeCells;
                    glyphs.insert(c.ch);
                }
            }
            // Judged on its colours, blinking or not: two colours of this
            // ramp that aren't neighbours. Other colours are another ramp's.
            const bool shade = c.ch == kShadeLight || c.ch == kShadeMedium || c.ch == kShadeDark;
            if (shade && c.fg.isIndexed() && c.bg.isIndexed() && c.fg.index != c.bg.index) {
                const int f = ramp.indexOf(c.fg.index), bi = ramp.indexOf(c.bg.index);
                if (f >= 0 && bi >= 0 && std::abs(f - bi) != 1)
                    r.findings.push_back({LadderFinding::Kind::NonNeighbourShade, b->x0 + x, b->y0 + y,
                                          at(b->x0 + x, b->y0 + y) +
                                              ": a shade of two colours that aren't ramp neighbours"});
            }
        }

    auto solid = [&](int g) { return g % kRungsPerStep == 0; };
    auto black = [&](int g) { return solid(g) && ramp.colours[static_cast<size_t>(g / kRungsPerStep)] == Black; };

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
    auto step = [&](int before, int g, int after) {
        if (before >= 0 && black(before))
            before = -1;
        if (after >= 0 && black(after))
            after = -1;
        return before >= 0 && after >= 0 && ((before < g && g < after) || (before > g && g > after));
    };
    auto wide = [&](int x, int y, int width) {
        r.findings.push_back({LadderFinding::Kind::WideRung, b->x0 + x, b->y0 + y,
                              at(b->x0 + x, b->y0 + y) + ": a rung " + std::to_string(width) +
                                  " cells wide (at most " + std::to_string(options.maxRung) + ")"});
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; x += across[static_cast<size_t>(y) * w + x]) {
            const int g = rungAt(x, y), end = x + across[static_cast<size_t>(y) * w + x];
            if (g < 0 || solid(g) || !step(x > 0 ? rungAt(x - 1, y) : -1, g, end < w ? rungAt(end, y) : -1))
                continue;
            int tallest = 0;
            for (int xx = x; xx < end; ++xx)
                tallest = std::max(tallest, down[static_cast<size_t>(y) * w + xx]);
            if (const int width = std::min(end - x, tallest); width > options.maxRung) {
                wide(x, y, width);
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
                wide(x, y, width);
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
    struct Level {
        int g, first, last;  // the rung, and its run's ends along the line
    };
    auto skips = [&](int n, auto&& cellAt) {
        std::vector<Level> levels;
        // l is an end level and beside the level next to it
        auto outline = [&](const Level& l, const Level& beside) {
            return l.first == l.last && solid(l.g) && l.g == beside.g - beside.g % kRungsPerStep - kRungsPerStep;
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
                if (!before && !after)
                    continue;
                const auto [x, y] = cellAt(levels[k].last);
                const auto [nx, ny] = cellAt(levels[k + 1].first);
                r.findings.push_back({LadderFinding::Kind::RungSkip, b->x0 + x, b->y0 + y,
                                      at(b->x0 + x, b->y0 + y) + ": rung " + std::to_string(a) + " next to rung " +
                                          std::to_string(c) + " at " + at(b->x0 + nx, b->y0 + ny) +
                                          " (more than one rung apart)"});
            }
            levels.clear();
        };
        for (int i = 0; i < n; ++i) {
            const auto [x, y] = cellAt(i);
            const int g = rungAt(x, y);
            if (g < 0 || black(g)) {
                judge();
                continue;
            }
            if (!levels.empty() && levels.back().g == g)
                levels.back().last = i;
            else
                levels.push_back({g, i, i});
        }
        judge();
    };
    for (int y = 0; y < h; ++y)
        skips(w, [y](int i) { return std::pair{i, y}; });
    for (int x = 0; x < w; ++x)
        skips(h, [x](int i) { return std::pair{x, i}; });

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
            const int g = rungAt(x, y);
            if (g < 0 || black(g) || g == ramp.topRung())
                continue;
            int seen = 0, above = 0, below = 0;
            const int nb[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
            for (const auto& d : nb) {
                const int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || rungAt(nx, ny) < 0)
                    continue;
                ++seen;
                above += rungAt(nx, ny) > g;
                below += rungAt(nx, ny) < g;
            }
            if (seen >= 3 && (above == seen || below == seen))
                r.findings.push_back({LadderFinding::Kind::Speckle, b->x0 + x, b->y0 + y,
                                      at(b->x0 + x, b->y0 + y) + ": a speck, " +
                                          (below == seen ? "lighter" : "darker") + " than every neighbour"});
        }

    if (r.shadeCells >= 4 && glyphs.size() == 1)
        r.findings.push_back({LadderFinding::Kind::OneGlyph, b->x0, b->y0, "the shading uses one glyph only"});

    r.noise = noise(canvas, *b);
    if (r.noise > options.maxNoise) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "noise %.3f (at most %.3f)", r.noise, options.maxNoise);
        r.findings.push_back({LadderFinding::Kind::Noise, b->x0, b->y0, buf});
    }
    return r;
}

} // namespace bw::lint
