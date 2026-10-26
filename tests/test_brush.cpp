// Ramps, the shading ladder, the common brush machinery (footprints,
// strokes, masks, pressure, preview and the one-delta-per-stroke edit) and
// the ramp shade brush.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/ramp.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <random>
#include <set>
#include <string>

using namespace bw;

namespace {

Ramp steel() { return *findRamp("steel"); }  // k K w W

// A dark solid as it is written: a space on the colour, fg black.
Cell solid(std::uint8_t c) {
    Cell cell;
    cell.ch = U' ';
    cell.fg = Color::indexed(Black);
    cell.bg = Color::indexed(c);
    return cell;
}

Cell shade(char32_t g, std::uint8_t fg, std::uint8_t bg) {
    Cell cell;
    cell.ch = g;
    cell.fg = Color::indexed(fg);
    cell.bg = Color::indexed(bg);
    return cell;
}

Canvas filled(int w, int h, const Cell& c) {
    Canvas canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.at(x, y) = c;
    return canvas;
}

// A shade cell is a ladder cell: two ramp neighbours, either way round.
bool neighbourShade(const Ramp& r, const Cell& c) {
    if (c.ch != kShadeLight && c.ch != kShadeMedium && c.ch != kShadeDark)
        return true;
    const int f = r.indexOf(c.fg.paletteIndex()), b = r.indexOf(c.bg.paletteIndex());
    return f >= 0 && b >= 0 && std::abs(f - b) == 1;
}

std::set<std::pair<int, int>> cellsOf(const CellEdit& e) {
    std::set<std::pair<int, int>> s;
    for (const auto& c : e.changes)
        s.insert({c.x, c.y});
    return s;
}

} // namespace

TEST_CASE("the named ramps, colour for colour", "[ramp]") {
    for (const char* n : {"fire", "sunset", "ice", "steel", "skin", "grass", "sea", "toxic", "violet", "wood"}) {
        const auto r = findRamp(n);
        REQUIRE(r);
        CHECK(rampProblem(*r).empty());
        CHECK(r->colours.front() == Black);
    }
    // every ramp, colour for colour
    using C = std::vector<std::uint8_t>;
    const std::vector<std::pair<const char*, C>> expected = {
        {"fire", {Black, Red, LightRed, Yellow, White}},
        {"sunset", {Black, Blue, Magenta, LightMagenta, LightRed, Yellow}},
        {"ice", {Black, Blue, LightBlue, LightCyan, White}},
        {"grass", {Black, Green, LightGreen, Yellow}},
        {"steel", {Black, DarkGrey, Grey, White}},
        {"skin", {Black, Red, Brown, LightRed, Yellow}},
        {"sea", {Black, Blue, Cyan, LightCyan}},
        {"toxic", {Black, Green, LightGreen, LightCyan, White}},
        {"violet", {Black, Magenta, LightMagenta, White}},
        {"wood", {Black, Red, Brown, Yellow}},
    };
    REQUIRE(namedRamps().size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO(expected[i].first);
        CHECK(namedRamps()[i].name == expected[i].first);
        CHECK(namedRamps()[i].colours == expected[i].second);
    }
    CHECK_FALSE(findRamp("plaid"));
    CHECK_FALSE(rampProblem(Ramp{"", {Black}}).empty());
    CHECK_FALSE(rampProblem(Ramp{"", {Black, Red, Black}}).empty());
    CHECK_FALSE(rampProblem(Ramp{"", {Black, 16}}).empty());
}

TEST_CASE("the ladder steps A, B░A, B▒A, B▓A, B between each pair", "[ramp][ladder]") {
    // the named order; sunset is a hue ramp, not sorted by brightness
    CHECK(findRamp("sunset")->colours ==
          std::vector<std::uint8_t>{Black, Blue, Magenta, LightMagenta, LightRed, Yellow});
    const Ramp r = steel();
    REQUIRE(r.topRung() == 12);
    const std::vector<Cell> want = {
        solid(Black),
        shade(kShadeLight, DarkGrey, Black),
        shade(kShadeMedium, DarkGrey, Black),
        shade(kShadeDark, DarkGrey, Black),
        shade(kFullBlock, DarkGrey, Black),  // a bright solid is written as █
        // K is bright and w isn't: K over w keeps the background dark
        shade(kShadeDark, DarkGrey, Grey),
        shade(kShadeMedium, DarkGrey, Grey),
        shade(kShadeLight, DarkGrey, Grey),
        shade(U' ', Black, Grey),
        shade(kShadeLight, White, Grey),
        shade(kShadeMedium, White, Grey),
        shade(kShadeDark, White, Grey),
        shade(kFullBlock, White, Black),
    };
    for (int i = 0; i <= 12; ++i) {
        INFO("rung " << i);
        CHECK(rungCell(r, i) == want[static_cast<size_t>(i)]);
        CHECK(rungOf(r, want[static_cast<size_t>(i)]) == i);
    }
    // Lighten one rung per stroke from black to white, then back down.
    Canvas c = filled(1, 1, solid(Black));
    const RampShadeBrush up(r, ShadeDirection::Lighten), down(r, ShadeDirection::Darken);
    for (int i = 1; i <= 12; ++i) {
        const CellEdit e = paintStroke(c, up, {}, {{0, 0}});
        REQUIRE(e.changes.size() == 1);
        e.apply(c);
        CHECK(c.at(0, 0) == want[static_cast<size_t>(i)]);
    }
    CHECK(paintStroke(c, up, {}, {{0, 0}}).empty());  // clamped at the top
    for (int i = 11; i >= 0; --i) {
        paintStroke(c, down, {}, {{0, 0}}).apply(c);
        CHECK(c.at(0, 0) == want[static_cast<size_t>(i)]);
    }
    CHECK(paintStroke(c, down, {}, {{0, 0}}).empty());  // clamped at the bottom
}

TEST_CASE("rungOf reads every solid and inverted form of a rung", "[ramp][ladder]") {
    const Ramp r = steel();
    CHECK(rungOf(r, shade(kFullBlock, DarkGrey, Red)) == 4);    // █ shows its foreground
    CHECK(rungOf(r, shade(kUpperHalf, Grey, Grey)) == 8);       // a half block of one colour
    CHECK(rungOf(r, shade(kShadeMedium, White, White)) == 12);  // a shade of one colour
    // The darker colour on top shows the other end of the pair: k░K is K▓k
    CHECK(rungOf(r, shade(kShadeLight, Black, DarkGrey)) == 3);
    CHECK(rungOf(r, shade(kShadeMedium, Black, DarkGrey)) == 2);
    CHECK(rungOf(r, shade(kShadeDark, Black, DarkGrey)) == 1);
    // Off the ladder: non-neighbours, other colours, half blocks, text, blink
    CHECK_FALSE(rungOf(r, shade(kShadeLight, White, Black)));
    CHECK_FALSE(rungOf(r, shade(kShadeLight, Red, Black)));
    CHECK_FALSE(rungOf(r, solid(Blue)));
    CHECK_FALSE(rungOf(r, shade(kUpperHalf, White, Black)));
    CHECK_FALSE(rungOf(r, shade(U'A', White, Black)));
    Cell blink = solid(Grey);
    blink.attrs = attr::Blink;
    CHECK_FALSE(rungOf(r, blink));
    // Only palette colours are on a ramp: an RGB colour isn't read as its
    // nearest palette colour
    Cell rgb = solid(Black);
    rgb.bg = Color::fromRgb({0x90, 0x90, 0x90});
    CHECK_FALSE(rungOf(r, rgb));
    CHECK_FALSE(stepRung(r, rgb, 1));
    rgb = shade(kShadeLight, White, Grey);
    rgb.fg = Color::fromRgb({0xff, 0xff, 0xff});
    CHECK_FALSE(rungOf(r, rgb));
    rgb = shade(kFullBlock, White, Black);
    rgb.fg = Color::fromRgb({0xff, 0xff, 0xff});
    CHECK_FALSE(rungOf(r, rgb));
    // a space shows only its background, so an RGB foreground doesn't matter
    rgb = solid(Grey);
    rgb.fg = Color::fromRgb({1, 2, 3});
    CHECK(rungOf(r, rgb) == 8);
    // and the brush leaves RGB cells alone
    Canvas c = filled(2, 1, solid(Black));
    c.at(0, 0).bg = Color::fromRgb({0x90, 0x90, 0x90});
    CHECK(cellsOf(paintRegion(c, RampShadeBrush(r, ShadeDirection::Lighten), {})) ==
          std::set<std::pair<int, int>>{{1, 0}});
}

TEST_CASE("shades keep the background dark where the ramp allows", "[ramp][ladder]") {
    const Ramp r = steel();
    CHECK(rungCell(r, 5).bg.index < 8);
    // On every named ramp, a rung has a bright background only when both of
    // its colours are bright (that needs iCE colours however it is written).
    for (const Ramp& ramp : namedRamps())
        for (int g = 0; g <= ramp.topRung(); ++g) {
            INFO(ramp.name << " rung " << g);
            const Cell c = rungCell(ramp, g);
            CHECK(rungOf(ramp, c) == g);
            const int i = g / kRungsPerStep;
            const bool bothBright = g % kRungsPerStep && ramp.colours[size_t(i)] >= 8 && ramp.colours[size_t(i) + 1] >= 8;
            if (!bothBright)
                CHECK(c.bg.index < 8);
        }

    // Stepping keeps the orientation the artist chose where it keeps the
    // background dark: K▓w (rung 5) lightens to K▒w, not w▒K.
    const Cell dark = shade(kShadeDark, DarkGrey, Grey);
    REQUIRE(rungOf(r, dark) == 5);
    const auto up = stepRung(r, dark, 1);
    REQUIRE(up);
    CHECK(up->bg.index < 8);
    CHECK(*up == shade(kShadeMedium, DarkGrey, Grey));
    // Two dark colours: either way round keeps the background dark, so the
    // cell keeps its own. b▓c on sea (rung 5) lightens to b▒c, and c░b to c▒b.
    const Ramp sea = *findRamp("sea");
    REQUIRE(rungOf(sea, shade(kShadeDark, Blue, Cyan)) == 5);
    CHECK(stepRung(sea, shade(kShadeDark, Blue, Cyan), 1) == shade(kShadeMedium, Blue, Cyan));
    CHECK(stepRung(sea, shade(kShadeLight, Cyan, Blue), 1) == shade(kShadeMedium, Cyan, Blue));
    // A bright background the other form avoids is never kept: w▓K (rung
    // 7, bg K) steps down to K▒w.
    CHECK(stepRung(r, shade(kShadeDark, Grey, DarkGrey), -1) == shade(kShadeMedium, DarkGrey, Grey));
    // Into the next pair, the new pair's own rule: K░w (rung 7) +2 is W░w.
    CHECK(stepRung(r, shade(kShadeLight, DarkGrey, Grey), 2) == shade(kShadeLight, White, Grey));
}

TEST_CASE("huge step counts saturate at the ramp's ends", "[ramp][ladder]") {
    const Ramp r = steel();
    CHECK(stepRung(r, rungCell(r, 1), INT_MAX) == rungCell(r, 12));
    CHECK(stepRung(r, rungCell(r, 11), INT_MIN) == rungCell(r, 0));
    Canvas c = filled(1, 1, rungCell(r, 1));
    paintStroke(c, RampShadeBrush(r, ShadeDirection::Lighten, INT_MAX), {}, {{0, 0}}).apply(c);
    CHECK(rungOf(r, c.at(0, 0)) == 12);
    paintStroke(c, RampShadeBrush(r, ShadeDirection::Darken, INT_MAX), {}, {{0, 0}}).apply(c);
    CHECK(rungOf(r, c.at(0, 0)) == 0);
}

TEST_CASE("the shade brush never mixes colours that aren't ramp neighbours", "[brush][ladder]") {
    // Every glyph the ladder uses plus a half block and a letter, in every
    // pair of colours: 7 glyphs x 256 colour pairs.
    const char32_t glyphs[] = {U' ', kFullBlock, kShadeLight, kShadeMedium, kShadeDark, kUpperHalf, U'A'};
    Canvas c(16 * 16, std::size(glyphs));
    for (size_t g = 0; g < std::size(glyphs); ++g)
        for (int fg = 0; fg < 16; ++fg)
            for (int bg = 0; bg < 16; ++bg)
                c.at(fg * 16 + bg, int(g)) = shade(glyphs[g], std::uint8_t(fg), std::uint8_t(bg));
    for (const Ramp& r : namedRamps())
        for (const auto dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
            for (const int steps : {1, 2, 3, 5}) {
                INFO(r.name << (dir == ShadeDirection::Lighten ? " lighten " : " darken ") << steps);
                const CellEdit e = paintRegion(c, RampShadeBrush(r, dir, steps), {});
                for (const CellChange& ch : e.changes) {
                    INFO("cell " << ch.x << "," << ch.y);
                    REQUIRE(rungOf(r, ch.before));  // only ladder cells change
                    CHECK(rungOf(r, ch.after));
                    CHECK(neighbourShade(r, ch.after));
                    const int d = *rungOf(r, ch.after) - *rungOf(r, ch.before);
                    CHECK((dir == ShadeDirection::Lighten ? d : -d) > 0);
                    CHECK(std::abs(d) <= steps);
                }
                // Every cell left alone is either off the ramp or at its end.
                Canvas after = c;
                e.apply(after);
                for (int y = 0; y < c.height(); ++y)
                    for (int x = 0; x < c.width(); ++x)
                        if (after.at(x, y) == c.at(x, y))
                            if (const auto g = rungOf(r, c.at(x, y)))
                                CHECK(*g == (dir == ShadeDirection::Lighten ? r.topRung() : 0));
            }
}

TEST_CASE("a pass steps each cell once however often the stroke crosses it", "[brush]") {
    const Canvas c = filled(10, 3, solid(Black));
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    // back and forth along the row three times
    const CellEdit e = paintStroke(c, up, {}, {{0, 1}, {9, 1}, {0, 1}, {9, 1}});
    REQUIRE(e.changes.size() == 10);
    for (const auto& ch : e.changes) {
        CHECK(ch.y == 1);
        CHECK(rungOf(steel(), ch.after) == 1);
    }
    // A second stroke is a second pass.
    Canvas d = c;
    e.apply(d);
    paintStroke(d, up, {}, {{0, 1}, {9, 1}}).apply(d);
    CHECK(rungOf(steel(), d.at(4, 1)) == 2);
}

TEST_CASE("a cell takes the strongest dab that covers it, once, from its original value", "[brush]") {
    const Canvas c = filled(9, 5, solid(Black));
    const RampShadeBrush up(steel(), ShadeDirection::Lighten, 4);
    BrushSettings s;
    s.pressure = PressureTo::Strength;
    auto rung = [&](const CellEdit& e, int x, int y) -> std::optional<int> {
        for (const CellChange& ch : e.changes)
            if (ch.x == x && ch.y == y)
                return rungOf(steel(), ch.after);
        return std::nullopt;
    };
    // A pen touches down lightly and presses harder where it is: the cell
    // ends at the peak's strength, one change from black, not 1 + 2 + 4.
    CellEdit e = paintStroke(c, up, s, {{0, 0, 0.05}, {0, 0, 0.5}, {0, 0, 1.0}});
    REQUIRE(e.changes.size() == 1);
    CHECK(e.changes[0].before == solid(Black));
    CHECK(rung(e, 0, 0) == 4);
    // A weaker dab after it leaves the cell alone.
    CHECK(rung(paintStroke(c, up, s, {{0, 0, 1.0}, {0, 0, 0.25}}), 0, 0) == 4);
    CHECK(rung(paintStroke(c, up, s, {{0, 0, 0.5}, {0, 0, 0.25}, {0, 0, 0.5}}), 0, 0) == 2);
    // A line at full pressure back over a light start repaints it, once.
    e = paintStroke(c, up, s, {{0, 2, 0.25}, {4, 2, 1.0}, {0, 2, 1.0}});
    for (int x = 0; x <= 4; ++x)
        CHECK(rung(e, x, 2) == 4);
    // A wider dab repaints the cells it shares with a weaker one at its
    // strength too, from their original value.
    s.size = 3;
    e = paintStroke(c, up, s, {{4, 2, 0.25}, {4, 2, 0.75}});
    REQUIRE(e.changes.size() == 9);
    for (const CellChange& ch : e.changes) {
        CHECK(ch.before == solid(Black));
        CHECK(rungOf(steel(), ch.after) == 3);
    }
    // Only strength repaints: the same strength crossing again is one pass.
    s.size = 1;
    s.pressure = PressureTo::Nothing;
    CHECK(rung(paintStroke(c, up, s, {{0, 0}, {3, 0}, {0, 0}}), 0, 0) == 4);
    // The preview shows what lands.
    s.pressure = PressureTo::Strength;
    Stroke st(c, up, s);
    st.addDab({1, 1, 0.25});
    st.addDab({1, 1, 1.0});
    CHECK(rungOf(steel(), st.preview().at(1, 1)) == 4);
}

TEST_CASE("with iCE off the ramp brush never writes a cell that needs a bright background", "[brush][ladder]") {
    // Every ramp, every rung in both forms as a start, every step count,
    // up and down: whatever is written shows as a rung of the ramp with
    // iCE off (a dark background, no blink).
    StrokeOptions off;
    off.ice = false;
    auto shows = [](const Ramp& r, const Cell& c) {
        return c.bg.paletteIndex() < 8 && !(c.attrs & attr::Blink) && rungOf(r, iceCell(c, false));
    };
    for (const Ramp& r : namedRamps())
        for (const auto dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
            for (int steps = 1; steps <= 4; ++steps)
                for (int k = 0; k <= r.topRung(); ++k)
                    for (const auto form : {ShadeForm::LightOnTop, ShadeForm::DarkOnTop}) {
                        INFO(r.name << (dir == ShadeDirection::Lighten ? " lighten " : " darken ") << steps
                                    << " from rung " << k << " form " << int(form));
                        const Cell start = rungCell(r, k, form);
                        const RampShadeBrush brush(r, dir, steps);
                        const CellEdit e = paintStroke(filled(1, 1, start), brush, {}, {{0, 0}}, off);
                        for (const CellChange& ch : e.changes) {
                            CHECK(shows(r, ch.after));
                            CHECK(neighbourShade(r, ch.after));
                            // never past the step, never the wrong way
                            const int d = *rungOf(r, ch.after) - k;
                            CHECK((dir == ShadeDirection::Lighten ? d : -d) > 0);
                            CHECK(std::abs(d) <= steps);
                        }
                    }
    // Sweeping a cell from black to the top and back with each step count:
    // every cell written shows as a rung, and a whole colour a pass (solid
    // to solid) still reaches both ends.
    for (const Ramp& r : namedRamps())
        for (int steps = 1; steps <= 4; ++steps) {
            INFO(r.name << " steps " << steps);
            Canvas c = filled(1, 1, solid(Black));
            for (const auto dir : {ShadeDirection::Lighten, ShadeDirection::Darken}) {
                for (int pass = 0; pass < 2 * r.topRung(); ++pass) {
                    const CellEdit e = paintStroke(c, RampShadeBrush(r, dir, steps), {}, {{0, 0}}, off);
                    for (const CellChange& ch : e.changes)
                        CHECK(shows(r, ch.after));
                    e.apply(c);
                }
                if (dir == ShadeDirection::Lighten && steps == kRungsPerStep)
                    CHECK(rungOf(r, c.at(0, 0)) == r.topRung());
            }
            CHECK(c.at(0, 0) == solid(Black));
        }
    // Every glyph in every pair of colours: what is written shows as a rung,
    // and a cell that blinks with iCE off (a bright background) is left.
    const char32_t glyphs[] = {U' ', kFullBlock, kShadeLight, kShadeMedium, kShadeDark, kUpperHalf, U'A'};
    Canvas grid(16 * 16, std::size(glyphs));
    for (size_t g = 0; g < std::size(glyphs); ++g)
        for (int fg = 0; fg < 16; ++fg)
            for (int bg = 0; bg < 16; ++bg)
                grid.at(fg * 16 + bg, int(g)) = shade(glyphs[g], std::uint8_t(fg), std::uint8_t(bg));
    for (const Ramp& r : namedRamps())
        for (const auto dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
            for (const int steps : {1, 2, 3, 4}) {
                INFO(r.name << (dir == ShadeDirection::Lighten ? " lighten " : " darken ") << steps);
                for (const CellChange& ch : paintRegion(grid, RampShadeBrush(r, dir, steps), {}, off).changes) {
                    INFO("cell " << ch.x << "," << ch.y);
                    CHECK(ch.before.bg.paletteIndex() < 8);
                    CHECK(shows(r, ch.after));
                }
            }
    // fire with iCE off: two whole colours up is LightRed; a rung more needs
    // LightRed and Yellow, two brights, either way round, so it stays.
    const Ramp fire = *findRamp("fire");
    Canvas c = filled(1, 1, solid(Black));
    for (int i = 0; i < 2; ++i)
        paintStroke(c, RampShadeBrush(fire, ShadeDirection::Lighten, 4), {}, {{0, 0}}, off).apply(c);
    REQUIRE(rungOf(fire, c.at(0, 0)) == 8);
    CHECK(paintStroke(c, RampShadeBrush(fire, ShadeDirection::Lighten, 1), {}, {{0, 0}}, off).changes.empty());
    CHECK(stepRung(fire, c.at(0, 0), 1, false) == std::nullopt);
    // Further than that stops at the last rung that shows: from Red▒LightRed
    // three rungs up is LightRed, not the rung past it.
    CHECK(stepRung(fire, rungCell(fire, 6), 3, false) == rungCell(fire, 8));
    // iCE on writes the two-bright shade as before.
    CHECK(rungOf(fire, *stepRung(fire, c.at(0, 0), 1)) == 9);
}

TEST_CASE("footprints", "[brush]") {
    using P = std::vector<std::pair<int, int>>;
    CHECK(footprint(5, 5, 1, Tip::Square) == P{{5, 5}});
    CHECK(footprint(5, 5, 1, Tip::Round) == P{{5, 5}});
    CHECK(footprint(5, 5, 0, Tip::Square) == P{{5, 5}});  // at least one cell
    CHECK(footprint(5, 5, 3, Tip::Square).size() == 9);
    // even sizes reach one cell further left and up
    CHECK(footprint(5, 5, 2, Tip::Square) == P{{4, 4}, {5, 4}, {4, 5}, {5, 5}});
    // round: about half as many rows as columns, symmetric about the dab
    const P round5 = footprint(10, 10, 5, Tip::Round);
    CHECK(round5 == P{{9, 9}, {10, 9}, {11, 9}, {8, 10}, {9, 10}, {10, 10}, {11, 10}, {12, 10}, {9, 11}, {10, 11},
                      {11, 11}});
    for (int size = 1; size <= 31; size += 2) {
        INFO("size " << size);
        const P fp = footprint(0, 0, size, Tip::Round);
        const std::set<std::pair<int, int>> s(fp.begin(), fp.end());
        for (const auto& [x, y] : fp) {
            CHECK(s.count({-x, y}));
            CHECK(s.count({x, -y}));
        }
        const auto [lo, hi] = std::minmax_element(fp.begin(), fp.end());
        CHECK(hi->second - lo->second + 1 <= size / 2 + 1);
    }
}

TEST_CASE("a mirrored tip covers the mirror image of the tip", "[brush]") {
    // A dab at the mirror column with the tip mirrored covers exactly the
    // mirror of the unmirrored dab's cells, even sizes included, in
    // row-major order; on odd sizes mirroring changes nothing.
    const int w = 23, h = 15;
    for (const Tip tip : {Tip::Square, Tip::Round})
        for (int size = 1; size <= 12; ++size)
            for (const int x : {0, 3, 11, 20, 22}) {
                INFO("size " << size << " x " << x << (tip == Tip::Round ? " round" : " square"));
                std::vector<std::pair<int, int>> want;
                for (const auto& [cx, cy] : footprint(x, 7, size, tip, w, h))
                    want.emplace_back(w - 1 - cx, cy);
                std::sort(want.begin(), want.end(), [](auto a, auto b) {
                    return a.second != b.second ? a.second < b.second : a.first < b.first;
                });
                CHECK(footprint(w - 1 - x, 7, size, tip, w, h, true) == want);
                if (size % 2)
                    CHECK(footprint(x, 7, size, tip, w, h, true) == footprint(x, 7, size, tip, w, h));
            }
    using P = std::vector<std::pair<int, int>>;
    CHECK(footprint(5, 5, 2, Tip::Square, 20, 20, true) == P{{5, 4}, {6, 4}, {5, 5}, {6, 5}});
}

namespace {
int rowsOf(int size) {
    std::set<int> rows;
    for (const auto& [x, y] : footprint(0, 0, size, Tip::Round))
        rows.insert(y);
    return int(rows.size());
}
} // namespace

TEST_CASE("round tips of even sizes stay round", "[brush]") {
    // size 4 is 4 columns by 2 rows (square on screen), not one flat row
    CHECK(rowsOf(4) == 2);
    CHECK(footprint(0, 0, 4, Tip::Round).size() == 8);
    CHECK(rowsOf(8) > rowsOf(6));
    // about half as many rows as columns, never fewer as the size grows
    for (int size = 2; size <= 40; ++size) {
        INFO("size " << size);
        CHECK(rowsOf(size) >= rowsOf(size - 1));
        CHECK(std::abs(2 * rowsOf(size) - size) <= 2);
    }
    // An even size reaches one column further left; a multiple of four (an
    // even number of rows) one row further up too. Symmetric about that centre.
    for (int size = 2; size <= 32; size += 2) {
        INFO("size " << size);
        const std::vector<std::pair<int, int>> fp = footprint(0, 0, size, Tip::Round);
        const std::set<std::pair<int, int>> s(fp.begin(), fp.end());
        const int up = size % 4 == 0 ? 1 : 0;
        for (const auto& [x, y] : fp) {
            CHECK(s.count({-1 - x, y}));
            CHECK(s.count({x, -up - y}));
        }
    }
}

TEST_CASE("line4 is 4-connected and includes both ends", "[brush]") {
    for (const auto& [x0, y0, x1, y1] : std::vector<std::array<int, 4>>{
             {0, 0, 0, 0}, {0, 0, 7, 0}, {0, 0, 0, -5}, {0, 0, 5, 5}, {3, 2, -4, 7}, {0, 0, 9, 2}, {-3, 4, 2, -8}}) {
        INFO(x0 << "," << y0 << " -> " << x1 << "," << y1);
        const auto l = line4(x0, y0, x1, y1);
        REQUIRE(l.front() == std::make_pair(x0, y0));
        REQUIRE(l.back() == std::make_pair(x1, y1));
        for (size_t i = 1; i < l.size(); ++i)
            CHECK(std::abs(l[i].first - l[i - 1].first) + std::abs(l[i].second - l[i - 1].second) == 1);
        CHECK(int(l.size()) == std::abs(x1 - x0) + std::abs(y1 - y0) + 1);
    }
}

TEST_CASE("line arithmetic doesn't overflow at the ends of int", "[brush][border]") {
    using P = std::vector<std::pair<int, int>>;
    // The part of a line inside a rectangle, found without walking the rest.
    CHECK(line4(INT_MIN, 0, INT_MAX, 0, -5, -5, 5, 5).size() == 11);
    CHECK(line4(0, 0, INT_MAX, 0, -5, -5, 5, 5) == P{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}});
    CHECK(line4(INT_MAX, 3, INT_MIN, 3, -2, -5, 2, 5) == P{{2, 3}, {1, 3}, {0, 3}, {-1, 3}, {-2, 3}});
    CHECK(line4(INT_MIN, 7, INT_MAX, 7, -5, -5, 5, 5).empty());
    CHECK(line4(INT_MAX, INT_MAX, INT_MAX, INT_MAX, 0, 0, INT_MAX, INT_MAX) == P{{INT_MAX, INT_MAX}});
    for (const auto& [x0, y0, x1, y1] : std::vector<std::array<int, 4>>{
             {INT_MIN, INT_MIN, INT_MAX, INT_MAX}, {INT_MAX, INT_MIN, INT_MIN, INT_MAX}, {INT_MIN, -3, INT_MAX, 4},
             {-9, INT_MIN, 6, INT_MAX}}) {
        INFO(x0 << "," << y0 << " -> " << x1 << "," << y1);
        const P l = line4(x0, y0, x1, y1, -5, -5, 5, 5);
        REQUIRE_FALSE(l.empty());
        CHECK(l.size() <= 22);
        for (const auto& [x, y] : l) {
            CHECK(std::abs(x) <= 5);
            CHECK(std::abs(y) <= 5);
        }
        for (size_t i = 1; i < l.size(); ++i)
            CHECK(std::abs(l[i].first - l[i - 1].first) + std::abs(l[i].second - l[i - 1].second) == 1);
    }
    // Inside the rectangle the clipped line is the line.
    CHECK(line4(3, 2, -4, 7, -10, -10, 10, 10) == line4(3, 2, -4, 7));
}

namespace {

// The line's cells inside the rectangle, by walking all of it.
std::vector<std::pair<int, int>> naiveClip(int x0, int y0, int x1, int y1, int cx0, int cy0, int cx1, int cy1) {
    std::vector<std::pair<int, int>> out;
    for (const auto& p : line4(x0, y0, x1, y1))
        if (p.first >= cx0 && p.first <= cx1 && p.second >= cy0 && p.second <= cy1)
            out.push_back(p);
    return out;
}

} // namespace

TEST_CASE("a clipped line is exactly the line's cells inside the rectangle", "[brush][border]") {
    // Rounding the ends where the line crosses the edge restarts it on
    // another slope: this one lost 5,2.
    CHECK(line4(-3, -3, 10, 7, 0, 0, 10, 10) == naiveClip(-3, -3, 10, 7, 0, 0, 10, 10));
    const auto a = line4(-3, -3, 10, 7, 0, 0, 10, 10);
    CHECK(std::find(a.begin(), a.end(), std::make_pair(5, 2)) != a.end());

    // Every direction and slope, ends inside, outside and on the edges.
    std::mt19937 rng(20261022);
    auto in = [&](int lo, int hi) { return lo + int(rng() % unsigned(hi - lo + 1)); };
    for (int n = 0; n < 5000; ++n) {
        const int cx0 = in(-3, 3), cy0 = in(-3, 3), cx1 = cx0 + in(0, 10), cy1 = cy0 + in(0, 10);
        const int x0 = in(cx0 - 40, cx1 + 40), y0 = in(cy0 - 40, cy1 + 40);
        const int x1 = in(cx0 - 40, cx1 + 40), y1 = in(cy0 - 40, cy1 + 40);
        INFO(x0 << "," << y0 << " -> " << x1 << "," << y1 << " in " << cx0 << "," << cy0 << "-" << cx1 << ","
                << cy1);
        REQUIRE(line4(x0, y0, x1, y1, cx0, cy0, cx1, cy1) == naiveClip(x0, y0, x1, y1, cx0, cy0, cx1, cy1));
    }
}

TEST_CASE("clipping a line from the ends of int gives the line's own cells", "[brush][border]") {
    using P = std::vector<std::pair<int, int>>;
    // Corner to corner: a staircase of diagonal steps, across then down.
    P diag;
    for (int x = -5; x <= 5; ++x) {
        if (x > -5)
            diag.push_back({x, x - 1});
        diag.push_back({x, x});
    }
    CHECK(line4(INT_MIN, INT_MIN, INT_MAX, INT_MAX, -5, -5, 5, 5) == diag);
    // 2^32 - 1 across and 7 down: row j starts at the column where
    // 14i + dx >= 2dx j. Column 2^31 (x = 0) is the first in row 4 (y = 1),
    // so the line steps down there, the cell beside the corner first. The
    // line itself crosses y = 0.5 at x = -0.5.
    P wide;
    for (int x = -5; x <= 0; ++x)
        wide.push_back({x, 0});
    for (int x = 0; x <= 5; ++x)
        wide.push_back({x, 1});
    CHECK(line4(INT_MIN, -3, INT_MAX, 4, -5, -5, 5, 5) == wide);
    // and the same tall: 15 across over 2^32 - 1 rows, from x = -2 to -1 in
    // row -1
    P tall;
    for (int y = -5; y <= -1; ++y)
        tall.push_back({-2, y});
    for (int y = -1; y <= 5; ++y)
        tall.push_back({-1, y});
    CHECK(line4(-9, INT_MIN, 6, INT_MAX, -5, -5, 5, 5) == tall);
    // A line 13n across and 10n down repeats every 13 columns, so near the
    // rectangle the long one has the short one's cells. Rounding where it
    // enters lost some.
    const int k = 165191049;  // 13k + 3 < 2^31
    CHECK(line4(-3 - 13 * k, -3 - 10 * k, -3 + 13 * k, -3 + 10 * k, 0, 0, 10, 10) ==
          naiveClip(-3, -3, 23, 17, 0, 0, 10, 10));
    CHECK(line4(-3 - 10 * k, -3 - 13 * k, -3 + 10 * k, -3 + 13 * k, 0, 0, 10, 10) ==
          naiveClip(-3, -3, 17, 23, 0, 0, 10, 10));
}

namespace {

// A stroke as if every dab of every joining line were stamped, the ones
// far off the canvas included.
CellEdit naiveStroke(const Canvas& c, const Brush& b, const BrushSettings& s, const std::vector<Dab>& dabs) {
    Stroke st(c, b, s);
    for (size_t k = 0; k < dabs.size(); ++k) {
        if (k == 0) {
            st.addDab(dabs[0], false);
            continue;
        }
        // The line between two dabs is the one from their lesser end (by
        // row, then column), stamped from the earlier dab.
        const Dab& a = dabs[k - 1];
        const Dab& b = dabs[k];
        auto l = std::pair{a.y, a.x} < std::pair{b.y, b.x} ? line4(a.x, a.y, b.x, b.y) : line4(b.x, b.y, a.x, a.y);
        if (std::pair{a.y, a.x} >= std::pair{b.y, b.x})
            std::reverse(l.begin(), l.end());
        for (size_t i = 1; i < l.size(); ++i)
            st.addDab({l[i].first, l[i].second, dabs[k].pressure}, false);
        if (l.size() == 1)
            st.addDab(dabs[k], false);
    }
    return st.edit();
}

} // namespace

TEST_CASE("a stroke that crosses the border paints what the whole line would", "[brush][border]") {
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    {
        // size 1 from the top right corner to off the bottom left
        const Canvas c = filled(10, 10, solid(Black));
        const auto got = cellsOf(paintStroke(c, up, {}, {{9, 0}, {-1, 11}}));
        std::set<std::pair<int, int>> want;
        for (const auto& p : naiveClip(9, 0, -1, 11, 0, 0, 9, 9))
            want.insert(p);
        CHECK(got == want);
        CHECK(got.count({1, 9}) == 1);
        CHECK(got.count({0, 8}) == 0);
    }
    {
        // a round size-4 tip that reaches in from a line mostly off the canvas
        const Canvas c = filled(5, 5, solid(Black));
        BrushSettings round;
        round.tip = Tip::Round;
        round.size = 4;
        const CellEdit e = paintStroke(c, up, round, {{-6, -10}, {1, 4}});
        CHECK(cellsOf(e).count({2, 1}) == 1);
        CHECK(e.changes == naiveStroke(c, up, round, {{-6, -10}, {1, 4}}).changes);
    }

    std::mt19937 rng(1022);
    auto in = [&](int lo, int hi) { return lo + int(rng() % unsigned(hi - lo + 1)); };
    for (int n = 0; n < 3000; ++n) {
        const Canvas c = filled(in(1, 9), in(1, 7), solid(Black));
        BrushSettings s;
        s.size = in(1, 8);
        s.tip = rng() % 2 ? Tip::Round : Tip::Square;
        std::vector<Dab> dabs(size_t(in(2, 4)));
        for (auto& d : dabs)
            d = {in(-40, c.width() + 40), in(-40, c.height() + 40)};
        std::string what = std::to_string(c.width()) + "x" + std::to_string(c.height()) + " size " +
                           std::to_string(s.size) + (s.tip == Tip::Round ? " round:" : " square:");
        for (const auto& d : dabs)
            what += " " + std::to_string(d.x) + "," + std::to_string(d.y);
        INFO(what);
        REQUIRE(paintStroke(c, up, s, dabs).changes == naiveStroke(c, up, s, dabs).changes);
    }
}

TEST_CASE("a line back over the same two dabs covers the same cells", "[brush][border]") {
    // Claude round 6, N2: a 4-connected line from B to A can step across
    // where the one from A to B steps down, so a scrub painted new cells.
    std::mt19937 rng(20261023);
    auto in = [&](int lo, int hi) { return lo + int(rng() % unsigned(hi - lo + 1)); };
    const Canvas c = filled(30, 20, solid(Black));
    for (int n = 0; n < 500; ++n) {
        const Dab a{in(-5, 34), in(-5, 24)}, b{in(-5, 34), in(-5, 24)};
        INFO(a.x << "," << a.y << " " << b.x << "," << b.y);
        const RampShadeBrush up(steel(), ShadeDirection::Lighten);
        REQUIRE(cellsOf(paintStroke(c, up, {}, {a, b})) == cellsOf(paintStroke(c, up, {}, {b, a})));
        REQUIRE(cellsOf(paintStroke(c, up, {}, {a, b, a, b})) == cellsOf(paintStroke(c, up, {}, {a, b})));
    }
}

TEST_CASE("a stroke from far off the canvas walks only the cells near it", "[brush][border]") {
    const Canvas c = filled(1, 1, solid(Black));
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    Stroke s(c, up, {});
    s.addDab({-10000000, 0});
    s.addDab({0, 0});
    CHECK(s.edit().changes.size() == 1);
    // the first dab, then the line from where it comes within reach
    CHECK(s.stamps() <= 4);
    // and across the canvas from one far side to the other
    Stroke t(c, up, {});
    t.addDab({INT_MIN, INT_MIN});
    t.addDab({INT_MAX, INT_MAX});
    CHECK(t.edit().changes.size() == 1);
    CHECK(t.stamps() <= 8);
    BrushSettings big;
    big.size = 7;
    Stroke b(c, up, big);
    b.addDab({0, -10000000});
    b.addDab({0, 10000000});
    CHECK(b.edit().changes.size() == 1);
    CHECK(b.stamps() <= 20);
}

TEST_CASE("tip sizes stop at the widest tip", "[brush][border]") {
    CHECK(kMaxBrushSize == 256);
    const Canvas c = filled(1, 1, solid(Black));
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    BrushSettings s;
    s.size = INT_MAX;
    CHECK(Stroke(c, up, s).settings().size == kMaxBrushSize);
    s.size = -5;
    CHECK(Stroke(c, up, s).settings().size == 1);
    // Across the whole of int with the biggest size: only the points within
    // reach of the canvas (the tip's half width plus one each side) are
    // stamped, so it finishes at once and the count can't overflow.
    s.size = INT_MAX;
    Stroke t(c, up, s);
    t.addDab({INT_MIN, 0});
    t.addDab({INT_MAX, 0});
    CHECK(t.edit().changes.size() == 1);
    CHECK(t.stamps() <= 2 + 2 * (kMaxBrushSize / 2 + 1) + 1);
    // the same with pressure to size, which can't grow past the cap either
    s.pressure = PressureTo::Size;
    Stroke p(c, up, s);
    p.addDab({0, INT_MIN, 1.0});
    p.addDab({0, INT_MAX, 1.0});
    CHECK(p.edit().changes.size() == 1);
    CHECK(p.stamps() <= 2 + 2 * (kMaxBrushSize / 2 + 1) + 1);
    // and a footprint is never wider than the cap
    CHECK(footprint(0, 0, INT_MAX, Tip::Square).size() == size_t(kMaxBrushSize) * kMaxBrushSize);
}

TEST_CASE("footprints at the ends of int leave out the cells past them", "[brush][border]") {
    using P = std::vector<std::pair<int, int>>;
    CHECK(footprint(INT_MAX, 0, 3, Tip::Square) ==
          P{{INT_MAX - 1, -1}, {INT_MAX, -1}, {INT_MAX - 1, 0}, {INT_MAX, 0}, {INT_MAX - 1, 1}, {INT_MAX, 1}});
    CHECK(footprint(INT_MIN, 0, 3, Tip::Square) ==
          P{{INT_MIN, -1}, {INT_MIN + 1, -1}, {INT_MIN, 0}, {INT_MIN + 1, 0}, {INT_MIN, 1}, {INT_MIN + 1, 1}});
    CHECK(footprint(0, INT_MAX, 3, Tip::Square) ==
          P{{-1, INT_MAX - 1}, {0, INT_MAX - 1}, {1, INT_MAX - 1}, {-1, INT_MAX}, {0, INT_MAX}, {1, INT_MAX}});
    CHECK(footprint(0, INT_MIN, 3, Tip::Square) ==
          P{{-1, INT_MIN}, {0, INT_MIN}, {1, INT_MIN}, {-1, INT_MIN + 1}, {0, INT_MIN + 1}, {1, INT_MIN + 1}});
    // a corner keeps only its quarter, and a round tip is cut the same way
    CHECK(footprint(INT_MAX, INT_MIN, 3, Tip::Square) ==
          P{{INT_MAX - 1, INT_MIN}, {INT_MAX, INT_MIN}, {INT_MAX - 1, INT_MIN + 1}, {INT_MAX, INT_MIN + 1}});
    CHECK(footprint(INT_MAX, 0, 5, Tip::Round) == P{{INT_MAX - 1, -1}, {INT_MAX, -1}, {INT_MAX - 2, 0},
                                                     {INT_MAX - 1, 0}, {INT_MAX, 0}, {INT_MAX - 1, 1}, {INT_MAX, 1}});
}

TEST_CASE("brushes stop at the canvas border", "[brush][border]") {
    const Canvas c = filled(6, 4, solid(Black));
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    BrushSettings big;
    big.size = 3;

    SECTION("a dab on a corner paints only the cells on the canvas") {
        CHECK(cellsOf(paintStroke(c, up, big, {{0, 0}})) == std::set<std::pair<int, int>>{{0, 0}, {1, 0}, {0, 1}, {1, 1}});
        CHECK(cellsOf(paintStroke(c, up, big, {{5, 3}})) == std::set<std::pair<int, int>>{{4, 2}, {5, 2}, {4, 3}, {5, 3}});
    }
    SECTION("a dab whose centre is off the canvas still paints the cells it covers") {
        CHECK(cellsOf(paintStroke(c, up, big, {{-1, -1}})) == std::set<std::pair<int, int>>{{0, 0}});
        CHECK(cellsOf(paintStroke(c, up, big, {{6, 4}})) == std::set<std::pair<int, int>>{{5, 3}});
    }
    SECTION("a dab wholly off the canvas changes nothing") {
        CHECK(paintStroke(c, up, big, {{-2, 1}}).empty());
        CHECK(paintStroke(c, up, big, {{1, 9}}).empty());
        CHECK(paintStroke(c, up, {}, {{-100000, -100000}}).empty());
    }
    SECTION("a stroke that leaves and re-enters the canvas paints only on it") {
        const CellEdit e = paintStroke(c, up, {}, {{-3, 1}, {8, 1}, {8, 2}, {-3, 2}});
        std::set<std::pair<int, int>> want;
        for (int x = 0; x < 6; ++x)
            want.insert({x, 1}), want.insert({x, 2});
        CHECK(cellsOf(e) == want);
    }
    SECTION("a brush bigger than the canvas covers all of it once") {
        BrushSettings huge;
        huge.size = 50;
        const CellEdit e = paintStroke(c, up, huge, {{2, 2}, {3, 2}});
        CHECK(e.changes.size() == 24);
        for (const auto& ch : e.changes)
            CHECK(rungOf(steel(), ch.after) == 1);
        // a bigger size is taken as the widest tip, which still covers it
        huge.size = 1 << 30;
        CHECK(Stroke(c, up, huge).settings().size == kMaxBrushSize);
        CHECK(paintStroke(c, up, huge, {{2, 2}}).changes.size() == 24);
        huge.tip = Tip::Round;
        CHECK(paintStroke(c, up, huge, {{-120, 2}}).changes.size() == 24);
        CHECK(paintStroke(c, up, huge, {{-100000, 2}}).empty());
    }
    SECTION("an empty canvas") {
        const Canvas none(0, 0);
        CHECK(paintStroke(none, up, big, {{0, 0}}).empty());
        CHECK(paintRegion(none, up, {}).empty());
    }
}

TEST_CASE("selection masks and the transparent lock", "[brush]") {
    Canvas c = filled(8, 3, solid(Black));
    c.at(2, 1).transparent = true;
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    const std::vector<Dab> row = {{0, 1}, {7, 1}};

    CHECK(paintStroke(c, up, {}, row).changes.size() == 8);  // no lock: a transparent cell is painted

    StrokeOptions locked;
    locked.lockTransparent = true;
    const CellEdit e = paintStroke(c, up, {}, row, locked);
    CHECK(e.changes.size() == 7);
    CHECK_FALSE(cellsOf(e).count({2, 1}));

    StrokeOptions sel;
    sel.selection = std::make_shared<const Mask>(Mask::rect(8, 3, 5, 0, 3, 2));  // corners in any order
    CHECK(cellsOf(paintStroke(c, up, {}, row, sel)) == std::set<std::pair<int, int>>{{3, 1}, {4, 1}, {5, 1}});
    CHECK(paintRegion(c, up, {}, sel).changes.size() == 9);

    // A mask clips to its canvas, and a mask smaller than the canvas
    // selects nothing beyond it.
    const Mask m = Mask::rect(8, 3, -5, -5, 1, 0);
    CHECK(m.contains(0, 0));
    CHECK(m.contains(1, 0));
    CHECK_FALSE(m.contains(-1, 0));
    CHECK_FALSE(m.contains(0, 1));
    StrokeOptions small;
    small.selection = std::make_shared<const Mask>(4, 3, true);
    CHECK(paintRegion(c, up, {}, small).changes.size() == 12);
}

TEST_CASE("a mask's bounds hold every cell it selects", "[brush]") {
    CHECK(Mask(8, 3).bounds().empty());
    CHECK(Mask(8, 3, true).bounds() == CellRect{0, 0, 8, 3});
    CHECK(Mask::rect(8, 3, 5, 0, 3, 2).bounds() == CellRect{3, 0, 3, 3});
    CHECK(Mask::rect(8, 3, -5, -5, 1, 0).bounds() == CellRect{0, 0, 2, 1});  // clipped to the canvas
    Mask m(10, 6);
    m.set(4, 2);
    CHECK(m.bounds() == CellRect{4, 2, 1, 1});
    m.set(7, 5);
    m.set(2, 3);
    m.set(20, 1);  // off the mask: nothing set
    CHECK(m.bounds() == CellRect{2, 2, 6, 4});
    // A cleared cell leaves the bounds as they were, and two masks of the
    // same cells are equal whatever their bounds.
    Mask same = m;
    same.set(0, 0);
    same.set(0, 0, false);
    CHECK(same.bounds() == CellRect{0, 0, 8, 6});
    CHECK(same == m);
}

TEST_CASE("pressure maps to size or strength", "[brush]") {
    const Canvas c = filled(20, 9, solid(Black));
    const RampShadeBrush up(steel(), ShadeDirection::Lighten, 4);
    BrushSettings s;
    s.size = 5;

    s.pressure = PressureTo::Size;
    CHECK(paintStroke(c, up, s, {{10, 4, 1.0}}).changes.size() == 25);
    CHECK(paintStroke(c, up, s, {{10, 4, 0.6}}).changes.size() == 9);  // size 3
    CHECK(paintStroke(c, up, s, {{10, 4, 0.0}}).changes.size() == 1);  // never below one cell
    // pressing harder without moving grows the dab
    CHECK(paintStroke(filled(9, 9, solid(Black)), up, s, {{4, 4, 0.2}, {4, 4, 1.0}}).changes.size() == 25);

    s.pressure = PressureTo::Strength;
    auto rungs = [&](double p) { return *rungOf(steel(), paintStroke(c, up, s, {{10, 4, p}}).changes.at(0).after); };
    CHECK(rungs(1.0) == 4);
    CHECK(rungs(0.5) == 2);
    CHECK(rungs(0.0) == 1);  // never below one rung
    CHECK(paintStroke(c, up, s, {{10, 4, 0.5}}).changes.size() == 25);

    s.pressure = PressureTo::Nothing;
    CHECK(rungs(0.1) == 4);

    // One step a pass (the default) can't scale: pressure to strength changes
    // nothing on it.
    const RampShadeBrush one(steel(), ShadeDirection::Lighten);
    s.pressure = PressureTo::Strength;
    for (const double p : {0.0, 0.3, 1.0})
        CHECK(rungOf(steel(), paintStroke(c, one, s, {{10, 4, p}}).changes.at(0).after) == 1);
}

TEST_CASE("a stroke previews live and commits as one delta", "[brush]") {
    Canvas c = filled(12, 6, solid(Grey));
    c.at(3, 2) = shade(U'x', White, Black);  // text is left alone
    const Canvas original = c;
    const RampShadeBrush down(steel(), ShadeDirection::Darken);
    BrushSettings s;
    s.size = 3;
    Stroke stroke(c, down, s);
    CHECK(stroke.edit().empty());
    CHECK(stroke.preview() == c);

    stroke.addDab({2, 2});
    const Canvas mid = stroke.preview();
    CHECK(rungOf(steel(), mid.at(2, 2)) == 7);
    CHECK(mid.at(6, 2) == c.at(6, 2));
    CHECK(c == original);  // the canvas is untouched until the edit is applied

    stroke.addDab({8, 3});
    const CellEdit e = stroke.edit();
    // row-major, each cell once
    for (size_t i = 1; i < e.changes.size(); ++i)
        CHECK(std::make_pair(e.changes[i - 1].y, e.changes[i - 1].x) < std::make_pair(e.changes[i].y, e.changes[i].x));
    CHECK_FALSE(cellsOf(e).count({3, 2}));

    Canvas applied = c;
    e.apply(applied);
    CHECK(applied == stroke.preview());
    e.revert(applied);
    CHECK(applied == original);

    // A cell painted back to its own value isn't in the delta.
    Stroke same(c, down, s);
    same.set(0, 0, c.at(0, 0));
    CHECK(same.painted(0, 0));
    CHECK(same.edit().empty());
    CHECK_FALSE(same.set(-1, 0, Cell{}));
    CHECK_FALSE(same.cellAt(12, 0));
    CHECK_FALSE(same.originalAt(0, -1));
}

TEST_CASE("shading a region of a real piece leaves what isn't on the ramp", "[brush][golden]") {
    const AnsiDocument doc = bwtest::parse(bwtest::readFile(bwtest::dataDir() / "golden/tux.ans"), 30);
    const Ramp r = steel();
    const CellEdit e = paintRegion(doc.canvas, RampShadeBrush(r, ShadeDirection::Darken), {});
    REQUIRE_FALSE(e.empty());
    for (const auto& ch : e.changes) {
        INFO("cell " << ch.x << "," << ch.y);
        CHECK(rungOf(r, ch.after) == *rungOf(r, ch.before) - 1);
        CHECK(neighbourShade(r, ch.after));
    }
    for (int y = 0; y < doc.canvas.height(); ++y)
        for (int x = 0; x < doc.canvas.width(); ++x)
            if (!rungOf(r, doc.canvas.at(x, y)))
                CHECK_FALSE(cellsOf(e).count({x, y}));
}
