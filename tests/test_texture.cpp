// Texture stamps (PLAN §10, "Detail"): lumps() as a brush, a seeded field
// over the canvas of lit forms on a black seam.

#include "blockwright/brush.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace bw;

namespace {

using Point = std::pair<int, int>;

Ramp steel() { return *findRamp("steel"); }  // k K w W

std::map<Point, Cell> written(const CellEdit& e) {
    std::map<Point, Cell> out;
    for (const CellChange& c : e.changes)
        out[{c.x, c.y}] = c.after;
    return out;
}

CellEdit stamp(const Canvas& c, const TextureBrush& b, int x, int y, int size, const StrokeOptions& o = {}) {
    return paintStroke(c, b, BrushSettings{size, Tip::Square}, {Dab{x, y}}, o);
}

// The whole canvas textured, as the rungs written.
std::vector<std::vector<int>> field(const Ramp& r, const TextureOptions& t, Compass light, int w, int h,
                                    bool ice = true) {
    Canvas c(w, h);
    StrokeOptions o;
    o.ice = ice;
    const auto cells = written(paintRegion(c, TextureBrush(r, t, light), {}, o));
    std::vector<std::vector<int>> out(h, std::vector<int>(w, -1));
    for (const auto& [p, cell] : cells)
        out[p.second][p.first] = rungOf(r, cell).value_or(-1);
    // a blank cell is a space on black: rung 0, not written
    for (auto& row : out)
        for (int& v : row)
            if (v < 0)
                v = 0;
    return out;
}

double seamShare(const std::vector<std::vector<int>>& f) {
    int seam = 0, all = 0;
    for (const auto& row : f)
        for (int v : row) {
            seam += v == 0;
            ++all;
        }
    return double(seam) / all;
}

// The lit cells' 4-connected pieces (each a lump's visible part, the seam
// keeping them apart), as their brightest rung.
std::vector<int> pieces(const std::vector<std::vector<int>>& f) {
    const int h = int(f.size()), w = int(f[0].size());
    std::vector<std::vector<bool>> seen(h, std::vector<bool>(w));
    std::vector<int> out;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (seen[y][x] || f[y][x] == 0)
                continue;
            int top = 0;
            std::vector<Point> todo{{x, y}};
            seen[y][x] = true;
            while (!todo.empty()) {
                const auto [px, py] = todo.back();
                todo.pop_back();
                top = std::max(top, f[py][px]);
                for (const auto [dx, dy] : {Point{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    const int nx = px + dx, ny = py + dy;
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h && !seen[ny][nx] && f[ny][nx] != 0) {
                        seen[ny][nx] = true;
                        todo.push_back({nx, ny});
                    }
                }
            }
            out.push_back(top);
        }
    return out;
}

} // namespace

TEST_CASE("a texture is one field: overlapping dabs agree and the seed fixes it", "[texture][brush]") {
    const Canvas c(60, 30);
    const TextureOptions t{TextureKind::Scales, 1.6, 7};
    const TextureBrush b(steel(), t, Compass::NW);
    const auto one = written(stamp(c, b, 20, 12, 15));
    const auto two = written(stamp(c, b, 26, 15, 15));
    int shared = 0;
    for (const auto& [p, cell] : one)
        if (two.count(p)) {
            ++shared;
            CHECK(two.at(p) == cell);
        }
    CHECK(shared > 50);
    // every cell is the field's rung at that cell
    for (const auto& [p, cell] : one)
        CHECK(cell == rungCell(steel(), textureRung(steel(), t, Compass::NW, p.first, p.second, true, 60, 30)));
    // the same seed paints the same; another doesn't
    CHECK(written(stamp(c, b, 20, 12, 15)) == one);
    const TextureBrush other(steel(), TextureOptions{TextureKind::Scales, 1.6, 8}, Compass::NW);
    CHECK(written(stamp(c, other, 20, 12, 15)) != one);
    // out-of-range settings are taken within range
    CHECK(textureRung(steel(), {TextureKind::Scales, 99.0, 7}, Compass::NW, 5, 5) ==
          textureRung(steel(), {TextureKind::Scales, kMaxTextureDensity, 7}, Compass::NW, 5, 5));
}

TEST_CASE("a texture is lit lumps on a black seam, brighter toward the light", "[texture][brush]") {
    const auto lean = [](Compass light) {
        // across: how often the left of two lit neighbours is the lighter,
        // less how often the right is; down the same for the upper
        const auto f = field(steel(), {}, light, 80, 40);
        int across = 0, down = 0;
        for (int y = 0; y + 1 < 40; ++y)
            for (int x = 0; x + 1 < 80; ++x) {
                const int a = f[y][x];
                if (a == 0)
                    continue;
                if (const int r = f[y][x + 1]; r != 0)
                    across += (a > r) - (a < r);
                if (const int d = f[y + 1][x]; d != 0)
                    down += (a > d) - (a < d);
            }
        return std::pair{across, down};
    };
    const auto f = field(steel(), {}, Compass::NW, 80, 40);
    const double seam = seamShare(f);
    CHECK(seam > 0.15);
    CHECK(seam < 0.6);
    const auto lumps = pieces(f);
    CHECK(lumps.size() > 30);
    // lit from the top left: the left and upper cells are the lighter
    const auto [nwAcross, nwDown] = lean(Compass::NW);
    CHECK(nwAcross > 0);
    CHECK(nwDown > 0);
    const auto [seAcross, seDown] = lean(Compass::SE);
    CHECK(seAcross < 0);
    CHECK(seDown < 0);
    const auto [eAcross, eDown] = lean(Compass::E);
    CHECK(eAcross < 0);
}

TEST_CASE("density sets how many lumps cover a stretch", "[texture][brush]") {
    const auto at = [](double d) { return field(steel(), {TextureKind::Scales, d, 3}, Compass::NW, 80, 40); };
    CHECK(pieces(at(0.4)).size() < pieces(at(1.0)).size());
    CHECK(pieces(at(1.0)).size() < pieces(at(1.6)).size());
    // sparse lumps leave most of it black
    CHECK(seamShare(at(kMinTextureDensity)) > 0.6);
}

TEST_CASE("each kind lights its lumps its own way", "[texture][brush]") {
    const Ramp r = steel();
    const int top = r.topRung();
    // sparse, so few lumps hide another's peak
    const auto tops = [&](TextureKind k) {
        std::map<int, int> n;
        for (int seed = 1; seed <= 3; ++seed)
            for (int v : pieces(field(r, {k, 0.4, seed}, Compass::NW, 80, 40)))
                ++n[v];
        return n;
    };
    // scales: every lump peaks at the top rung, bar a few whose peak another hides
    auto s = tops(TextureKind::Scales);
    CHECK(s[top] > 3 * (s[top - 1] + s[top - 2]));
    // foliage: leaves lit one by one, at three heights
    auto f = tops(TextureKind::Foliage);
    CHECK(f[top] > 20);
    CHECK(f[top - 1] > 20);
    CHECK(f[top - 2] > 20);
    // hoard: coins two under the top, a glint on about a third
    auto h = tops(TextureKind::Hoard);
    CHECK(h[top] > 20);
    CHECK(h[top - 2] > h[top]);
    CHECK(h[top - 1] * 5 < h[top]);
    // and they differ
    CHECK(field(r, {TextureKind::Scales, 1.6, 1}, Compass::NW, 40, 20) !=
          field(r, {TextureKind::Hoard, 1.6, 1}, Compass::NW, 40, 20));
}

TEST_CASE("a texture adds no skip, speck or broken shade on any named ramp", "[texture][brush][lint]") {
    for (const Ramp& r : namedRamps())
        for (const TextureKind k : {TextureKind::Scales, TextureKind::Foliage, TextureKind::Hoard})
            for (const double d : {0.5, 1.6, 2.5}) {
                INFO(r.name << " kind " << int(k) << " density " << d);
                Canvas c(80, 40);
                const TextureBrush b(r, {k, d, 11}, Compass::NE);
                const CellEdit e = paintRegion(c, b, {});
                for (const CellChange& ch : e.changes)
                    c.at(ch.x, ch.y) = ch.after;
                lint::LadderOptions o;
                o.whole = false;
                for (const lint::LadderFinding& f : lint::checkLadder(c, r, {}, o).findings) {
                    INFO(f.message << " at " << f.x << "," << f.y);
                    CHECK(f.kind != lint::LadderFinding::Kind::RungSkip);
                    CHECK(f.kind != lint::LadderFinding::Kind::Speckle);
                    CHECK(f.kind != lint::LadderFinding::Kind::NonNeighbourShade);
                }
            }
}

TEST_CASE("with iCE off a texture writes no bright background", "[texture][brush][ice]") {
    const Ramp fire = *findRamp("fire");  // k r R Y W: R to Y needs a bright background
    const auto on = field(fire, {}, Compass::NW, 80, 40, true);
    const auto off = field(fire, {}, Compass::NW, 80, 40, false);
    int topOn = 0, topOff = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 80; ++x) {
            topOn = std::max(topOn, on[y][x]);
            topOff = std::max(topOff, off[y][x]);
        }
    CHECK(topOn == fire.topRung());
    CHECK(topOff == 8);  // solid light red, the last rung with every one under it writable
    Canvas c(80, 40);
    StrokeOptions o;
    o.ice = false;
    const CellEdit e = paintRegion(c, TextureBrush(fire, {}, Compass::NW), {}, o);
    REQUIRE_FALSE(e.empty());
    for (const CellChange& ch : e.changes)
        CHECK(ch.after.bg.paletteIndex() < 8);
}

TEST_CASE("a texture keeps to the selection and the alpha lock", "[texture][brush]") {
    Canvas c(30, 16);
    const TextureBrush b(steel(), {}, Compass::NW);
    StrokeOptions sel;
    sel.selection = std::make_shared<Mask>(Mask::rect(30, 16, 5, 4, 12, 9));
    const auto in = written(stamp(c, b, 10, 8, 21, sel));
    REQUIRE_FALSE(in.empty());
    for (const auto& [p, cell] : in) {
        CHECK(p.first >= 5);
        CHECK(p.first <= 12);
        CHECK(p.second >= 4);
        CHECK(p.second <= 9);
    }
    // transparent cells stay under the lock, opaque ones are stamped
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 30; ++x)
            c.at(x, y).transparent = x < 15;
    StrokeOptions lock;
    lock.lockTransparent = true;
    const auto locked = written(stamp(c, b, 15, 8, 31, lock));
    REQUIRE_FALSE(locked.empty());
    for (const auto& [p, cell] : locked)
        CHECK(p.first >= 15);
    // without it a transparent cell takes the texture
    const auto open = written(stamp(c, b, 15, 8, 31));
    CHECK(open.size() > locked.size());
}

TEST_CASE("a mirrored texture stroke paints the exact mirror image", "[texture][brush][symmetry]") {
    for (const TextureKind k : {TextureKind::Hoard, TextureKind::Brick, TextureKind::Noise}) {
        INFO("kind " << int(k));
        const Canvas c(40, 20);
        const TextureBrush b(steel(), {k, 1.6, 5}, Compass::NW);
        StrokeOptions o;
        o.symmetry.mode = SymmetryMode::MirrorX;
        const auto cells = written(paintStroke(c, b, BrushSettings{9, Tip::Square}, {Dab{8, 10}, Dab{12, 8}}, o));
        REQUIRE_FALSE(cells.empty());
        int left = 0;
        for (const auto& [p, cell] : cells) {
            INFO(p.first << "," << p.second);
            REQUIRE(cells.count({39 - p.first, p.second}));
            CHECK(cells.at({39 - p.first, p.second}) == cell);
            left += p.first < 20;
        }
        CHECK(left * 2 == int(cells.size()));
    }
}

namespace {

// A cell's images under a mirror mode, the cell itself first.
std::vector<Point> mirrorImages(SymmetryMode mode, Point p, int w, int h) {
    const auto [x, y] = p;
    std::vector<Point> out{p};
    if (mode == SymmetryMode::MirrorX || mode == SymmetryMode::MirrorXY)
        out.push_back({w - 1 - x, y});
    if (mode == SymmetryMode::MirrorY || mode == SymmetryMode::MirrorXY)
        out.push_back({x, h - 1 - y});
    if (mode == SymmetryMode::MirrorXY)
        out.push_back({w - 1 - x, h - 1 - y});
    return out;
}

const char* modeName(SymmetryMode m) {
    return m == SymmetryMode::MirrorX ? "mirror X" : m == SymmetryMode::MirrorY ? "mirror Y" : "mirror XY";
}

} // namespace

// Codex round 1 P2: where a dab's images overlap, the lower image keeps a
// cell (Brush::dab), so the texture can't hang on which image painted it.
TEST_CASE("a mirrored texture is symmetric where the images overlap", "[texture][brush][symmetry]") {
    for (const TextureKind k : {TextureKind::Scales, TextureKind::Brick, TextureKind::Noise})
        for (const SymmetryMode mode : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY})
            for (const auto [w, h] : {Point{80, 40}, Point{81, 41}}) {
                INFO("kind " << int(k) << " " << modeName(mode) << " on " << w << "x" << h);
                const TextureBrush b(steel(), {k, 1.6, 1}, Compass::NW);
                StrokeOptions o;
                o.symmetry.mode = mode;
                {
                    // Codex's case: image 0 covers every cell, so every image ties
                    Canvas c(w, h);
                    paintStroke(c, b, BrushSettings{std::max(w, h) + 1, Tip::Square}, {Dab{w / 2, h / 2}}, o).apply(c);
                    int unlike = 0;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            for (const Point q : mirrorImages(mode, {x, y}, w, h))
                                unlike += !(c.at(q.first, q.second) == c.at(x, y));
                    CHECK(unlike == 0);
                    // the canvas isn't one shade: there is a texture to mirror
                    std::set<int> rungs;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            rungs.insert(rungOf(steel(), c.at(x, y)).value_or(0));
                    CHECK(rungs.size() > 3);
                }
                {
                    // a dab off the axes, over them: part of each image lies over
                    // another's (one dab: a joining line's images aren't exact)
                    const Canvas c(w, h);
                    const auto cells = written(stamp(c, b, w / 2 - 6, h / 2 - 4, 21, o));
                    REQUIRE_FALSE(cells.empty());
                    int overlap = 0, unlike = 0;
                    for (const auto& [p, cell] : cells) {
                        // a cell left as it was (rung 0 on the blank canvas) isn't written
                        for (const Point q : mirrorImages(mode, p, w, h))
                            unlike += !cells.count(q) || !(cells.at(q) == cell);
                        overlap += (mode != SymmetryMode::MirrorY && p.first == w / 2) ||
                                   (mode != SymmetryMode::MirrorX && p.second == h / 2);
                    }
                    CHECK(unlike == 0);
                    CHECK(overlap > 0);
                }
            }
}

TEST_CASE("mirrored texture strokes agree where they overlap, and read the folded field",
          "[texture][brush][symmetry]") {
    for (const TextureKind k : {TextureKind::Foliage, TextureKind::Brick, TextureKind::Noise})
        for (const SymmetryMode mode : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY}) {
            INFO("kind " << int(k) << " " << modeName(mode));
            const TextureOptions t{k, 1.6, 9};
            const TextureBrush b(steel(), t, Compass::NE);
            const bool fx = mode != SymmetryMode::MirrorY, fy = mode != SymmetryMode::MirrorX;
            const Canvas c(61, 31);
            StrokeOptions o;
            o.symmetry.mode = mode;
            const auto one = written(stamp(c, b, 26, 12, 17, o));
            const auto two = written(stamp(c, b, 34, 18, 17, o));
            int shared = 0;
            for (const auto& [p, cell] : one)
                if (two.count(p)) {
                    ++shared;
                    CHECK(two.at(p) == cell);
                }
            CHECK(shared > 50);
            // every cell is the folded field's
            for (const auto& [p, cell] : one)
                CHECK(cell ==
                      rungCell(steel(), textureRung(steel(), t, Compass::NE, p.first, p.second, true, 61, 31, fx, fy)));
            // the fold is the field reflected into the top-left part: there,
            // off the fold's own column and row, the plain field
            for (int y = 0; y < 31; ++y)
                for (int x = 0; x < 61; ++x) {
                    const int fxX = fx ? std::min(x, 60 - x) : x, fyY = fy ? std::min(y, 30 - y) : y;
                    const int folded = textureRung(steel(), t, Compass::NE, x, y, true, 61, 31, fx, fy);
                    CHECK(folded == textureRung(steel(), t, Compass::NE, fxX, fyY, true, 61, 31, fx, fy));
                    if ((!fx || x < 29) && (!fy || y < 14))
                        CHECK(folded == textureRung(steel(), t, Compass::NE, x, y, true, 61, 31));
                }
        }
    // no fold: the plain field
    const TextureOptions t{TextureKind::Foliage, 1.6, 9};
    CHECK(textureRung(steel(), t, Compass::NE, 50, 20, true, 61, 31, false, false) ==
          textureRung(steel(), t, Compass::NE, 50, 20, true, 61, 31));
}

TEST_CASE("a mirrored texture adds no skip, speck or broken shade at the fold", "[texture][brush][lint][symmetry]") {
    for (const Ramp& r : namedRamps())
        for (const SymmetryMode mode : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY})
            for (const auto [w, h] : {Point{80, 40}, Point{81, 41}}) {
                INFO(r.name << " " << modeName(mode) << " on " << w << "x" << h);
                Canvas c(w, h);
                StrokeOptions o;
                o.symmetry.mode = mode;
                paintStroke(c, TextureBrush(r, {TextureKind::Scales, 1.6, 4}, Compass::NW),
                            BrushSettings{std::max(w, h) + 1, Tip::Square}, {Dab{w / 2, h / 2}}, o)
                    .apply(c);
                lint::LadderOptions lo;
                lo.whole = false;
                for (const lint::LadderFinding& f : lint::checkLadder(c, r, {}, lo).findings) {
                    INFO(f.message << " at " << f.x << "," << f.y);
                    CHECK(f.kind != lint::LadderFinding::Kind::RungSkip);
                    CHECK(f.kind != lint::LadderFinding::Kind::Speckle);
                    CHECK(f.kind != lint::LadderFinding::Kind::NonNeighbourShade);
                }
            }
}

// Radial images can't be exact on the cell grid (a turn other than a half
// is made on screen and rounded), so a radial stroke uncovers the one field
// at every image: where images overlap, it is the same field whichever
// image painted the cell.
TEST_CASE("a radial texture is the plain field wherever its images overlap", "[texture][brush][symmetry]") {
    for (const TextureKind k : {TextureKind::Hoard, TextureKind::Brick, TextureKind::Noise})
        for (const int folds : {2, 3, 4, 8}) {
            INFO("kind " << int(k) << ", " << folds << " folds");
            const TextureOptions t{k, 1.6, 3};
            const TextureBrush b(steel(), t, Compass::NW);
            const Canvas c(60, 30);
            StrokeOptions o;
            o.symmetry = Symmetry{SymmetryMode::Radial, folds, std::nullopt};
            const auto cells = written(paintStroke(c, b, BrushSettings{25, Tip::Square}, {Dab{24, 12}, Dab{28, 16}}, o));
            REQUIRE(cells.size() > 25 * 13);
            for (const auto& [p, cell] : cells)
                CHECK(cell == rungCell(steel(), textureRung(steel(), t, Compass::NW, p.first, p.second, true, 60, 30)));
        }
}

// Brick and noise (PLAN §10, "Detail"): the same field, a running bond and
// a mottled stone.

namespace {

// How often the left of two neighbours (not mortar) is the lighter, less how
// often the right is; down the same for the upper.
std::pair<int, int> lean(const std::vector<std::vector<int>>& f) {
    int across = 0, down = 0;
    for (std::size_t y = 0; y + 1 < f.size(); ++y)
        for (std::size_t x = 0; x + 1 < f[y].size(); ++x) {
            const int a = f[y][x];
            if (a == 0)
                continue;
            if (const int r = f[y][x + 1]; r != 0)
                across += (a > r) - (a < r);
            if (const int d = f[y + 1][x]; d != 0)
                down += (a > d) - (a < d);
        }
    return {across, down};
}

// The columns of a row's mortar joints.
std::vector<int> joints(const std::vector<int>& row) {
    std::vector<int> out;
    for (int x = 0; x < int(row.size()); ++x)
        if (row[x] == 0)
            out.push_back(x);
    return out;
}

} // namespace

TEST_CASE("brick and noise are one field: dabs and drags agree, and the seed fixes it", "[texture][brush]") {
    for (const TextureKind k : {TextureKind::Brick, TextureKind::Noise}) {
        INFO("kind " << int(k));
        const Canvas c(60, 30);
        const TextureOptions t{k, 1.6, 7};
        const TextureBrush b(steel(), t, Compass::NW);
        const auto one = written(stamp(c, b, 20, 12, 15));
        const auto two = written(stamp(c, b, 26, 15, 15));
        int shared = 0;
        for (const auto& [p, cell] : one)
            if (two.count(p)) {
                ++shared;
                CHECK(two.at(p) == cell);
            }
        CHECK(shared > 50);
        // a drag paints the field, and a second drag over it changes nothing
        Canvas d(60, 30);
        const std::vector<Dab> path{{8, 6}, {20, 10}, {34, 18}, {50, 22}};
        const CellEdit first = paintStroke(d, b, BrushSettings{7, Tip::Round}, path);
        REQUIRE(first.changes.size() > 150);
        for (const auto& [p, cell] : written(first))
            CHECK(cell == rungCell(steel(), textureRung(steel(), t, Compass::NW, p.first, p.second, true, 60, 30)));
        first.apply(d);
        const std::vector<Dab> across{{30, 2}, {28, 14}, {26, 28}};
        const auto second = written(paintStroke(d, b, BrushSettings{9, Tip::Round}, across));
        for (const auto& [p, cell] : written(first))
            if (second.count(p))
                CHECK(second.at(p) == cell);
        // the same seed paints the same; another doesn't
        CHECK(written(stamp(c, b, 20, 12, 15)) == one);
        const TextureBrush other(steel(), TextureOptions{k, 1.6, 8}, Compass::NW);
        CHECK(written(stamp(c, other, 20, 12, 15)) != one);
    }
}

TEST_CASE("brick and noise add no skip, speck or broken shade, mirrored or not, iCE on or off",
          "[texture][brush][lint][symmetry][ice]") {
    for (const Ramp& r : namedRamps())
        for (const TextureKind k : {TextureKind::Brick, TextureKind::Noise})
            for (const double d : {kMinTextureDensity, 1.0, 1.6, kMaxTextureDensity})
                for (const bool ice : {true, false})
                    for (const SymmetryMode mode :
                         {SymmetryMode::None, SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY})
                        for (const auto [w, h] : {Point{80, 40}, Point{81, 41}})
                            for (const int seed : {2, 5}) {
                                // the light turns with the seed, round the compass
                                const Compass light = Compass((seed + int(mode) + w) % 8);
                                INFO(r.name << " kind " << int(k) << " density " << d << " iCE " << ice << " mode "
                                            << int(mode) << " on " << w << "x" << h << " seed " << seed
                                            << " light " << int(light));
                                Canvas c(w, h);
                                StrokeOptions o;
                                o.ice = ice;
                                o.symmetry.mode = mode;
                                paintStroke(c, TextureBrush(r, {k, d, seed}, light),
                                            BrushSettings{std::max(w, h) + 1, Tip::Square}, {Dab{w / 2, h / 2}}, o)
                                    .apply(c);
                                lint::LadderOptions lo;
                                lo.whole = false;
                                for (const lint::LadderFinding& f : lint::checkLadder(c, r, {}, lo).findings) {
                                    INFO(f.message << " at " << f.x << "," << f.y);
                                    CHECK(f.kind != lint::LadderFinding::Kind::RungSkip);
                                    CHECK(f.kind != lint::LadderFinding::Kind::Speckle);
                                    CHECK(f.kind != lint::LadderFinding::Kind::NonNeighbourShade);
                                }
                                CHECK(lint::noise(c) <= 0.02);
                            }
}

TEST_CASE("with iCE off brick and noise stop at the last rung with every one under it writable",
          "[texture][brush][ice]") {
    const Ramp fire = *findRamp("fire");  // k r R Y W: R to Y needs a bright background
    for (const TextureKind k : {TextureKind::Brick, TextureKind::Noise}) {
        INFO("kind " << int(k));
        int topOn = 0, topOff = 0;
        for (int seed = 1; seed <= 3; ++seed) {
            const auto on = field(fire, {k, 1.0, seed}, Compass::NW, 80, 40, true);
            const auto off = field(fire, {k, 1.0, seed}, Compass::NW, 80, 40, false);
            for (int y = 0; y < 40; ++y)
                for (int x = 0; x < 80; ++x) {
                    topOn = std::max(topOn, on[y][x]);
                    topOff = std::max(topOff, off[y][x]);
                }
        }
        CHECK(topOn > 8);
        CHECK(topOff == 8);
        Canvas c(80, 40);
        StrokeOptions o;
        o.ice = false;
        const CellEdit e = paintRegion(c, TextureBrush(fire, {k, 1.0, 1}, Compass::NW), {}, o);
        REQUIRE_FALSE(e.empty());
        for (const CellChange& ch : e.changes)
            CHECK(ch.after.bg.paletteIndex() < 8);
    }
}

TEST_CASE("brick is a running bond on black mortar, its size set by density", "[texture][brush]") {
    const auto f = field(steel(), {TextureKind::Brick, 1.6, 4}, Compass::NW, 80, 40);
    // a course is three rows of brick under a mortar row
    std::vector<std::vector<int>> courseJoints;
    for (int y = 0; y < 40; ++y) {
        INFO("row " << y);
        const auto j = joints(f[y]);
        if (y % 4 == 0) {
            CHECK(j.size() == 80);
            continue;
        }
        // a brick row is broken only by its joints, one column each, about
        // 17 columns apart
        CHECK(j.size() >= 4);
        CHECK(j.size() <= 6);
        for (std::size_t i = 1; i < j.size(); ++i) {
            CHECK(j[i] - j[i - 1] >= 15);
            CHECK(j[i] - j[i - 1] <= 19);
        }
        // a course's rows share their joints
        if (y % 4 == 1)
            courseJoints.push_back(j);
        else
            CHECK(j == courseJoints.back());
    }
    REQUIRE(courseJoints.size() == 10);
    // every other course is shifted about half a brick: no joint is over one
    // of the course above
    for (std::size_t k = 1; k < courseJoints.size(); ++k)
        for (const int x : courseJoints[k])
            for (const int above : courseJoints[k - 1]) {
                INFO("course " << k << " joint " << x << " over " << above);
                CHECK(std::abs(x - above) >= 5);
            }
    // the bricks are lit: lighter toward the light, a few rungs from a brick's
    // lit corner to its shadow, and not all one face
    const auto [nwAcross, nwDown] = lean(f);
    CHECK(nwAcross > 0);
    CHECK(nwDown > 0);
    const auto [seAcross, seDown] = lean(field(steel(), {TextureKind::Brick, 1.6, 4}, Compass::SE, 80, 40));
    CHECK(seAcross < 0);
    CHECK(seDown < 0);
    std::set<int> rungs;
    for (const auto& row : f)
        rungs.insert(row.begin(), row.end());
    CHECK(rungs.size() >= 5);
    // denser is smaller: more mortar rows
    const auto mortarRows = [](double d) {
        const auto g = field(steel(), {TextureKind::Brick, d, 4}, Compass::NW, 80, 40);
        return std::count_if(g.begin(), g.end(), [](const auto& row) { return joints(row).size() == row.size(); });
    };
    CHECK(mortarRows(kMinTextureDensity) < mortarRows(1.0));
    CHECK(mortarRows(1.0) < mortarRows(kMaxTextureDensity));
}

TEST_CASE("noise is a mottled stone: no seam, no speck, a rung a step", "[texture][brush]") {
    for (const Ramp& r : namedRamps())
        for (const double d : {kMinTextureDensity, 1.0, kMaxTextureDensity}) {
            INFO(r.name << " density " << d);
            const auto f = field(r, {TextureKind::Noise, d, 6}, Compass::NE, 81, 40);
            for (int y = 0; y < 40; ++y)
                for (int x = 0; x < 81; ++x) {
                    INFO(x << "," << y);
                    const int v = f[y][x];
                    // no seam: nothing black (a ramp of one step has no rung over 1)
                    CHECK(v >= std::min(2, r.topRung()));
                    // neighbours are at most a rung apart, and one of them is
                    // this cell's own rung (the cells pair side by side)
                    bool twin = false;
                    for (const auto [dx, dy] : {Point{1, 0}, Point{-1, 0}, Point{0, 1}, Point{0, -1}}) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= 81 || ny >= 40)
                            continue;
                        CHECK(std::abs(f[ny][nx] - v) <= 1);
                        twin |= dx != 0 && f[ny][nx] == v;
                    }
                    CHECK(twin);
                }
        }
    // mottled, not flat: several rungs, slopes facing the light lit (the
    // light moves the relief, not the stone), and a finer grain at a higher
    // density
    const auto at = [](double d, Compass light) {
        return field(steel(), {TextureKind::Noise, d, 6}, light, 80, 40);
    };
    std::set<int> rungs;
    for (const auto& row : at(1.0, Compass::NW))
        rungs.insert(row.begin(), row.end());
    CHECK(rungs.size() >= 3);
    const auto nw = at(1.0, Compass::NW), se = at(1.0, Compass::SE);
    int moved = 0, far = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 80; ++x) {
            moved += nw[y][x] != se[y][x];
            far += std::abs(nw[y][x] - se[y][x]) > 2;
        }
    CHECK(moved > 80 * 40 / 10);
    CHECK(far == 0);
    const auto changes = [](const std::vector<std::vector<int>>& f) {
        int n = 0;
        for (const auto& row : f)
            for (std::size_t x = 1; x < row.size(); ++x)
                n += row[x] != row[x - 1];
        return n;
    };
    CHECK(changes(at(kMinTextureDensity, Compass::NW)) < changes(at(1.0, Compass::NW)));
    CHECK(changes(at(1.0, Compass::NW)) < changes(at(kMaxTextureDensity, Compass::NW)));
}

namespace {

// The ladder lint's skips, specks and broken shades on the canvas, as
// "kind at x,y: message".
std::vector<std::string> ladderFaults(const Canvas& c, const Ramp& r) {
    lint::LadderOptions lo;
    lo.whole = false;
    std::vector<std::string> out;
    for (const lint::LadderFinding& f : lint::checkLadder(c, r, {}, lo).findings)
        if (f.kind == lint::LadderFinding::Kind::RungSkip || f.kind == lint::LadderFinding::Kind::Speckle ||
            f.kind == lint::LadderFinding::Kind::NonNeighbourShade)
            out.push_back(std::to_string(int(f.kind)) + " at " + std::to_string(f.x) + "," + std::to_string(f.y) +
                          ": " + f.message);
    return out;
}

// The canvas `w` x `h` stamped all over from (cx, cy), mirrored by `mode`.
Canvas stamped(const Ramp& r, const TextureOptions& t, Compass light, int w, int h, SymmetryMode mode, bool ice,
               int cx, int cy, int size) {
    Canvas c(w, h);
    StrokeOptions o;
    o.ice = ice;
    o.symmetry.mode = mode;
    paintStroke(c, TextureBrush(r, t, light), BrushSettings{size, Tip::Square}, {Dab{cx, cy}}, o).apply(c);
    return c;
}

} // namespace

// Codex round 1 P2: where two folds meet (a mirror's centre column and row,
// or the fold at the canvas's edge), the reflected profiles make a cell
// darker than every neighbour.
TEST_CASE("a folded brick leaves no cell darker than every neighbour", "[texture][brush][lint][symmetry]") {
    const TextureOptions t{TextureKind::Brick, 0.3, 0};
    {
        // 79x37, mirror X and Y: (39,18) was rung 8 among four 9s
        const Canvas c = stamped(steel(), t, Compass::NW, 79, 37, SymmetryMode::MirrorXY, true, 39, 18, 80);
        const auto faults = ladderFaults(c, steel());
        CHECK(faults == std::vector<std::string>{});
    }
    {
        // 7x19, mirror X: (3,18) was rung 7 among three 8s on the canvas
        const Canvas c = stamped(steel(), t, Compass::NW, 7, 19, SymmetryMode::MirrorX, true, 39, 18, 80);
        const auto faults = ladderFaults(c, steel());
        CHECK(faults == std::vector<std::string>{});
    }
}

// The lint and the mirror over small and odd canvases, where a course, a
// lump or a noise block meets a fold or an edge in ways 80x40 doesn't show.
TEST_CASE("every texture is lint-clean and mirror-exact on small and odd canvases",
          "[texture][brush][lint][symmetry]") {
    const Ramp fire = *findRamp("fire");
    const std::vector<Point> sizes{{1, 1}, {2, 3}, {3, 2}, {4, 4}, {5, 7}, {7, 19}, {9, 4}, {12, 5},
                                   {13, 9}, {20, 11}, {33, 17}, {41, 6}, {79, 37}};
    int n = 0;
    for (const TextureKind k :
         {TextureKind::Scales, TextureKind::Foliage, TextureKind::Hoard, TextureKind::Brick, TextureKind::Noise})
        for (const auto [w, h] : sizes)
            for (const SymmetryMode mode :
                 {SymmetryMode::None, SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY})
                for (const double d : {kMinTextureDensity, 0.7, 1.6, kMaxTextureDensity})
                    for (const int seed : {0, 3, 8}) {
                        // the light, the ramp and iCE turn with the case
                        const Compass light = Compass((n + seed) % 8);
                        const bool ice = n % 3 != 0;
                        const Ramp& r = n % 2 ? fire : steel();
                        ++n;
                        INFO(r.name << " kind " << int(k) << " " << w << "x" << h << " mode " << int(mode)
                                    << " density " << d << " seed " << seed << " light " << int(light) << " iCE "
                                    << ice);
                        const Canvas c =
                            stamped(r, {k, d, seed}, light, w, h, mode, ice, w / 2, h / 2, 2 * std::max(w, h) + 1);
                        CHECK(ladderFaults(c, r) == std::vector<std::string>{});
                        if (mode == SymmetryMode::None)
                            continue;
                        int unlike = 0;
                        for (int y = 0; y < h; ++y)
                            for (int x = 0; x < w; ++x)
                                for (const Point q : mirrorImages(mode, {x, y}, w, h))
                                    unlike += !(c.at(q.first, q.second) == c.at(x, y));
                        CHECK(unlike == 0);
                    }
}
