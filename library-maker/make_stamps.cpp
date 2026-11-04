// Writes the generic stamps of the bundled Library (library/<category>/) so
// each one can be made again the same way and is funkpower's own
// work by construction:
//
//   bw-make-stamps <library dir>
//
// Each stamp is drawn in half-block pixels (two to a cell, so a pixel is
// square), plus a few glyph cells for rain and snow, then
// trimmed to its part and written with encodeStamp and writeStamp, as
// Save to Library writes one. Pixels left undrawn are blank cells, the
// stamp's transparent area; black inside a part is written as black █.
// The stamps it doesn't draw (the frames and the ones cut from pieces) are
// left as they are.
#include "blockwright/halfblock.h"
#include "blockwright/library.h"
#include "blockwright/palette.h"
#include "blockwright/selection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace bw;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr const char* kAuthor = "funkpower";
constexpr const char* kDate = "20261104";

// A grid of half-block pixels, w wide and h high (h even: two to a cell),
// with glyph cells laid over it. -1 is no pixel.
struct Pix {
    int w, h;
    std::vector<int> p;
    struct Glyph {
        int x, row;
        char32_t ch;
        std::uint8_t fg;
    };
    std::vector<Glyph> glyphs;

    Pix(int w_, int rows) : w(w_), h(rows * 2), p(static_cast<size_t>(w_) * rows * 2, -1) {}
    bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int at(int x, int y) const { return in(x, y) ? p[static_cast<size_t>(y) * w + x] : -1; }
    void set(int x, int y, int c) {
        if (in(x, y))
            p[static_cast<size_t>(y) * w + x] = c;
    }
    // Every pixel whose centre passes `inside`.
    void fill(const std::function<bool(double, double)>& inside, int c) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (inside(x + 0.5, y + 0.5))
                    set(x, y, c);
    }
    // Recolours drawn pixels whose centre passes `inside`.
    void tint(const std::function<bool(double, double)>& inside, int c) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (at(x, y) >= 0 && inside(x + 0.5, y + 0.5))
                    set(x, y, c);
    }
    void rect(int x0, int y0, int x1, int y1, int c) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                set(x, y, c);
    }
    void ellipse(double cx, double cy, double rx, double ry, int c) {
        fill([=](double x, double y) { return sq((x - cx) / rx) + sq((y - cy) / ry) <= 1.0; }, c);
    }
    void disc(double cx, double cy, double r, int c) { ellipse(cx, cy, r, r, c); }
    void poly(const std::vector<std::pair<double, double>>& pts, int c) {
        fill([&](double x, double y) { return insidePoly(pts, x, y); }, c);
    }
    // A bar `width` pixels thick from (x0, y0) to (x1, y1).
    void bar(double x0, double y0, double x1, double y1, double width, int c) {
        const double dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
        const double nx = -dy / len * width / 2, ny = dx / len * width / 2;
        poly({{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}}, c);
    }
    void glyph(int x, int row, char32_t ch, std::uint8_t fg) { glyphs.push_back({x, row, ch, fg}); }

    static double sq(double v) { return v * v; }
    static bool insidePoly(const std::vector<std::pair<double, double>>& pts, double x, double y) {
        bool in = false;
        for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
            const auto [xi, yi] = pts[i];
            const auto [xj, yj] = pts[j];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                in = !in;
        }
        return in;
    }
};

// Clumps of foliage, cloud or bush: each lump drawn in `dark`, then lit in
// `lit` where a smaller disc up and to the left of it reaches (the light is
// top left), so every lump is a lit unit with a dark seam below and right.
struct Lump {
    double x, y, r;
};
void lumps(Pix& px, const std::vector<Lump>& ls, int dark, int lit) {
    for (const Lump& l : ls) {
        px.disc(l.x, l.y, l.r, dark);
        px.fill([&](double x, double y) {
            return Pix::sq(x - l.x) + Pix::sq(y - l.y) <= l.r * l.r &&
                   Pix::sq(x - (l.x - l.r * 0.3)) + Pix::sq(y - (l.y - l.r * 0.3)) <= Pix::sq(l.r * 0.72);
        }, lit);
    }
}

// A fixed scatter for texture: irregular, but the same on every run.
unsigned scatter(int x, int y) {
    unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// The pixels as cells, trimmed to the drawn ones.
CellBlock cells(const Pix& px) {
    const int rows = px.h / 2;
    std::vector<Cell> out(static_cast<size_t>(px.w) * rows);
    for (int r = 0; r < rows; ++r)
        for (int x = 0; x < px.w; ++x) {
            const int t = px.at(x, 2 * r), b = px.at(x, 2 * r + 1);
            Cell c;
            if (t >= 0 || b >= 0) {
                c = halfBlockCell(HalfPair{std::uint8_t(std::max(t, 0)), std::uint8_t(std::max(b, 0))}, true);
                if (isBlank(c)) {  // black drawn inside the part is a black █, not a hole
                    c.ch = U'█';
                    c.fg = Color::indexed(Black);
                    c.bg = Color::indexed(Black);
                }
            }
            out[static_cast<size_t>(r) * px.w + x] = c;
        }
    for (const Pix::Glyph& g : px.glyphs) {
        Cell& c = out[static_cast<size_t>(g.row) * px.w + g.x];
        c = Cell{};
        c.ch = g.ch;
        c.fg = Color::indexed(g.fg);
    }
    int x0 = px.w, y0 = rows, x1 = -1, y1 = -1;
    for (int r = 0; r < rows; ++r)
        for (int x = 0; x < px.w; ++x)
            if (!isBlank(out[static_cast<size_t>(r) * px.w + x])) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, r);
                y1 = std::max(y1, r);
            }
    if (x1 < 0)
        throw std::runtime_error("nothing drawn");
    CellBlock block(x1 - x0 + 1, y1 - y0 + 1);
    for (int r = y0; r <= y1; ++r)
        for (int x = x0; x <= x1; ++x)
            block.cells[static_cast<size_t>(r - y0) * block.width + (x - x0)] = out[static_cast<size_t>(r) * px.w + x];
    return block;
}

// ---- trees ------------------------------------------------------------------

Pix roundTree() {
    Pix px(16, 11);
    px.rect(7, 13, 8, 21, Brown);
    px.rect(6, 20, 9, 21, Brown);
    px.bar(8, 15, 11.5, 11, 1.6, Brown);
    lumps(px, {{8, 9, 5}, {4.5, 9.5, 3.8}, {11.5, 9.5, 3.8}, {6, 5, 4}, {10.5, 5.5, 3.8}, {8, 12, 3.5}}, Green, LightGreen);
    return px;
}

Pix oakTree() {
    Pix px(28, 12);
    px.rect(12, 14, 15, 23, Brown);
    px.rect(10, 22, 17, 23, Brown);
    px.rect(15, 14, 15, 23, DarkGrey);  // the trunk's shaded side
    px.bar(13, 17, 6, 11, 2, Brown);
    px.bar(14, 17, 21, 11, 2, Brown);
    lumps(px, {{14, 8, 6}, {7, 10, 5}, {21, 10, 5}, {9.5, 5, 4.5}, {18.5, 5, 4.5}, {3.5, 12.5, 3.5},
               {24.5, 12.5, 3.5}, {14, 3.5, 3.5}},
          Green, LightGreen);
    return px;
}

Pix birchTree() {
    Pix px(12, 14);
    px.rect(5, 6, 6, 27, White);
    px.rect(6, 6, 6, 27, Grey);  // the trunk's shaded side
    for (const int y : {18, 22, 25})
        px.rect(5, y, 6, y, DarkGrey);  // the bark's dark marks
    px.ellipse(6, 7.5, 5.5, 7.5, Green);
    px.tint([](double x, double y) { return Pix::sq((x - 4.6) / 4) + Pix::sq((y - 6) / 5.2) <= 1; }, LightGreen);
    return px;
}

Pix pineTree() {
    Pix px(13, 14);
    px.rect(5, 23, 7, 27, Brown);
    // tiers from the top: apex y, base y, half-width at the base
    const double tiers[4][3] = {{0, 8, 3.5}, {4, 14, 4.8}, {9, 19, 5.8}, {13, 24, 6.5}};
    for (int i = 3; i >= 0; --i) {
        const auto [top, base, half] = std::tuple{tiers[i][0], tiers[i][1], tiers[i][2]};
        px.poly({{6.5, top}, {6.5 + half, base}, {6.5 - half, base}}, Green);
        px.poly({{6.5, top}, {6.5 - half * 0.15, base}, {6.5 - half, base}}, LightGreen);
    }
    return px;
}

Pix bush() {
    Pix px(14, 5);
    lumps(px, {{4, 6, 3.5}, {9.5, 6, 3.8}, {7, 4, 3.5}, {2.8, 8, 2.5}, {11.5, 8, 2.5}}, Green, LightGreen);
    px.fill([](double, double y) { return y > 9; }, -1);
    return px;
}

Pix hedge() {
    Pix px(28, 6);
    px.rect(0, 4, 27, 11, Green);
    // the clipped top: leafy lumps of uneven size and spacing
    lumps(px, {{2.5, 4.5, 2.8}, {6.5, 3.6, 3.1}, {10.2, 4.8, 2.3}, {13.4, 3.4, 3}, {17.6, 4.4, 2.6},
               {20.8, 3.8, 2.4}, {24.6, 3.5, 3.2}},
          Green, LightGreen);
    // leaves catching the light, fewer down the face
    for (int y = 5; y < 10; ++y)
        for (int x = 1; x < 26; ++x) {
            const unsigned s = scatter(x, y);
            // a leaf lies across, or hangs down across a cell boundary (a whole lit cell is a speck)
            const bool hangs = !(s & 1) && y % 2;
            const int x2 = x + (hangs ? 0 : 1), y2 = y + (hangs ? 1 : 0);
            if (px.at(x, y) == Green && px.at(x2, y2) == Green && (s >> 1) % (5 + 2 * (y - 5)) == 0) {
                px.set(x, y, LightGreen);
                px.set(x2, y2, LightGreen);
            }
        }
    // the foot in ragged shade
    for (int x = 0; x < 28; ++x) {
        px.set(x, 11, DarkGrey);
        if (scatter(x, 11) % 3 == 0)
            px.set(x, 10, DarkGrey);
    }
    return px;
}

// ---- plants -----------------------------------------------------------------

Pix grassTufts() {
    Pix px(20, 4);
    const int heights[][6] = {{3, 5, 7, 6, 4, 2}, {2, 4, 6, 5, 3, 0}, {3, 6, 8, 7, 5, 3}};
    const int starts[] = {0, 7, 13};
    for (int t = 0; t < 3; ++t)
        for (int i = 0; i < 6; ++i) {
            const int x = starts[t] + i, hgt = heights[t][i];
            for (int y = 8 - hgt; y < 8; ++y)
                px.set(x, y, i < 3 ? LightGreen : Green);
        }
    return px;
}

Pix flowers() {
    Pix px(16, 6);
    const struct {
        int x, top, petal;
    } fl[] = {{1, 1, LightRed}, {6, 4, LightMagenta}, {11, 2, White}};
    for (const auto& f : fl) {
        px.rect(f.x + 1, f.top + 4, f.x + 2, 11, Green);  // the stem
        px.rect(f.x, f.top, f.x + 3, f.top + 3, f.petal);
        px.set(f.x, f.top, -1);
        px.set(f.x + 3, f.top, -1);
        px.set(f.x, f.top + 3, -1);
        px.set(f.x + 3, f.top + 3, -1);
        px.rect(f.x + 1, f.top + 1, f.x + 2, f.top + 2, Yellow);
    }
    px.rect(4, 9, 5, 9, LightGreen);  // leaves
    px.rect(9, 10, 10, 10, LightGreen);
    return px;
}

Pix reeds() {
    Pix px(13, 9);
    const struct {
        int x, top;
        bool head;
    } rs[] = {{1, 6, false}, {3, 2, true}, {5, 8, false}, {7, 0, true}, {9, 5, true}, {11, 9, false}};
    for (const auto& r : rs) {
        for (int y = r.top; y < 18; ++y)
            px.set(r.x, y, r.x % 4 == 1 ? Green : LightGreen);
        if (r.head)
            px.rect(r.x, r.top + 4, r.x + 1, r.top + 8, Brown);
    }
    return px;
}

// ---- land -------------------------------------------------------------------

Pix rocks() {
    Pix px(18, 4);
    const struct {
        double x, y, rx, ry;
    } st[] = {{5.5, 5, 5.5, 4}, {12.5, 6, 3.8, 3}, {16, 7, 2.2, 1.8}};
    for (const auto& s : st) {
        px.ellipse(s.x, s.y, s.rx, s.ry, DarkGrey);
        px.fill([&](double x, double y) {
            return Pix::sq((x - s.x) / s.rx) + Pix::sq((y - s.y) / s.ry) <= 1 &&
                   Pix::sq((x - s.x + s.rx * 0.3) / (s.rx * 0.7)) + Pix::sq((y - s.y + s.ry * 0.35) / (s.ry * 0.65)) <= 1;
        }, Grey);
    }
    px.rect(0, 8, 17, 8, -1);
    return px;
}

Pix hill() {
    Pix px(40, 7);
    px.ellipse(20, 14, 20, 13, Green);
    px.tint([](double x, double y) { return Pix::sq((x - 15) / 15) + Pix::sq((y - 11) / 9.5) <= 1; }, LightGreen);
    return px;
}

Pix cliffEdge() {
    Pix px(18, 13);
    px.poly({{0, 3}, {11, 3}, {12, 7}, {14, 9}, {13.5, 14}, {16, 17}, {16.5, 22}, {18, 26}, {0, 26}}, Grey);
    // the face turned from the light, down the drop
    px.tint([](double x, double y) { return x > 9.5 + y * 0.18; }, DarkGrey);
    for (const auto [x0, x1, y] : {std::tuple{1, 6, 10}, {2, 8, 16}, {0, 5, 21}})
        px.rect(x0, y, x1, y, DarkGrey);  // strata
    lumps(px, {{2.5, 3, 2.5}, {6.5, 2.8, 2.6}, {10.5, 3.2, 2.2}}, Green, LightGreen);
    px.rect(0, 3, 12, 4, Green);
    return px;
}


// ---- buildings --------------------------------------------------------------

Pix house() {
    Pix px(18, 10);
    px.rect(12, 0, 13, 5, DarkGrey);  // chimney
    px.poly({{4, 1}, {14, 1}, {18, 8}, {0, 8}}, Red);
    px.poly({{4, 1}, {14, 1}, {15.2, 3}, {2.8, 3}}, LightRed);  // the lit upper roof
    px.rect(2, 8, 15, 19, Grey);
    px.rect(13, 8, 15, 19, DarkGrey);  // the wall turned from the light
    px.rect(7, 13, 10, 19, Brown);     // door
    px.rect(3, 10, 5, 12, Yellow);     // lit windows
    px.rect(12, 10, 14, 12, Yellow);
    return px;
}

Pix barn() {
    Pix px(22, 10);
    const std::vector<std::pair<double, double>> outer{{0, 20}, {0, 8}, {4, 2.5}, {11, 0}, {18, 2.5}, {22, 8}, {22, 20}};
    px.poly(outer, DarkGrey);
    px.poly({{1.5, 20}, {1.5, 8.5}, {5, 4}, {11, 2}, {17, 4}, {20.5, 8.5}, {20.5, 20}}, Red);
    px.rect(8, 5, 13, 8, White);  // the hayloft door
    px.rect(9, 6, 12, 7, Black);
    px.rect(6, 11, 15, 19, White);  // the barn door, framed in white with its brace
    px.rect(7, 12, 14, 19, Red);
    px.bar(7, 12, 15, 20, 1.8, White);
    px.bar(15, 12, 7, 20, 1.8, White);
    return px;
}

Pix castleKeep() {
    Pix px(18, 13);
    for (const int x : {1, 7, 13})
        px.rect(x, 0, x + 3, 3, Grey);  // merlons
    px.rect(1, 4, 16, 5, Grey);
    px.rect(2, 6, 15, 25, Grey);
    px.tint([](double x, double) { return x > 12; }, DarkGrey);
    for (const int x : {5, 8, 11})
        px.rect(x, 9, x, 13, Black);  // arrow slits
    px.fill([](double x, double y) { return x > 5.5 && x < 12.5 && (y > 21 || Pix::sq((x - 9) / 3.5) + Pix::sq((y - 21) / 3) <= 1); },
            Black);  // the gate
    return px;
}

Pix lighthouse() {
    Pix px(12, 15);
    px.poly({{5, 0}, {7, 0}, {9, 3}, {3, 3}}, Red);  // cap
    px.rect(3, 3, 8, 6, DarkGrey);                   // lantern room
    px.rect(4, 4, 7, 6, Yellow);
    px.rect(5, 4, 6, 5, White);
    px.rect(1, 7, 10, 8, DarkGrey);  // gallery
    px.poly({{3, 9}, {9, 9}, {11, 30}, {1, 30}}, White);
    px.tint([](double, double y) { return int(y - 9) / 5 % 2 == 1; }, Red);
    px.tint([](double x, double y) { return x > 7.5 + (y - 9) * 0.1; }, Grey);
    px.tint([](double x, double y) { return x > 7.5 + (y - 9) * 0.1 && int(y - 9) / 5 % 2 == 1; }, Red);
    px.rect(5, 25, 6, 29, Black);  // door
    return px;
}

Pix windmill() {
    Pix px(24, 15);
    px.poly({{8, 9}, {16, 9}, {20, 30}, {4, 30}}, Brown);  // the tower
    px.tint([](double x, double y) { return x > 14.5 + (y - 9) * 0.15; }, DarkGrey);
    px.poly({{8, 9}, {12, 5}, {16, 9}}, Red);  // the cap
    px.rect(11, 26, 12, 29, Black);           // door
    px.rect(13, 19, 14, 20, Yellow);          // a lit window
    // four sails, each a panel on one side of its spar, turning together
    px.rect(11, 0, 12, 7, Grey);  // up
    px.rect(13, 0, 14, 6, White);
    px.rect(13, 7, 21, 8, Grey);  // right
    px.rect(14, 9, 21, 10, White);
    px.rect(11, 8, 12, 16, Grey);  // down
    px.rect(9, 9, 10, 16, White);
    px.rect(2, 7, 10, 8, Grey);  // left
    px.rect(2, 5, 9, 6, White);
    px.rect(11, 7, 12, 8, DarkGrey);  // hub
    return px;
}

Pix fence() {
    Pix px(26, 5);
    for (int x = 0; x <= 24; x += 6) {
        px.rect(x, 1, x + 1, 9, Brown);
        px.set(x, 0, Brown);
        px.set(x + 1, 0, Brown);
        px.rect(x, 1, x, 9, Yellow);  // the lit side of the post
    }
    for (int x = 2; x <= 23; ++x)
        if (x % 6 > 1) {
            px.rect(x, 2, x, 3, Brown);
            px.rect(x, 6, x, 7, Brown);
        }
    return px;
}

Pix stoneBridge() {
    Pix px(32, 7);
    px.rect(0, 0, 31, 1, Grey);
    px.rect(0, 2, 31, 13, DarkGrey);
    px.rect(0, 2, 31, 2, Grey);
    px.ellipse(16, 14, 10, 9, -1);
    px.fill([](double x, double y) { return y > 2 && Pix::sq((x - 16) / 10) + Pix::sq((y - 14) / 9) <= 1; }, -1);
    px.fill([](double x, double y) {
        const double d = Pix::sq((x - 16) / 11.5) + Pix::sq((y - 14) / 10.5);
        return d <= 1 && Pix::sq((x - 16) / 10) + Pix::sq((y - 14) / 9) > 1;
    }, Grey);  // the arch's ring
    for (int x = 1; x < 31; x += 3)
        px.set(x, 0, -1);  // the parapet
    px.rect(0, 0, 0, 1, Grey);
    return px;
}

// ---- water and sky ----------------------------------------------------------

Pix calmWater() {
    Pix px(38, 2);
    px.rect(4, 0, 33, 1, Blue);
    px.rect(0, 2, 37, 3, Blue);
    for (const auto [x0, x1, y] : {std::tuple{7, 12, 1}, {21, 27, 0}, {13, 19, 2}, {29, 34, 3}, {2, 6, 3}})
        px.rect(x0, y, x1, y, LightBlue);
    px.rect(23, 1, 25, 1, LightCyan);
    return px;
}


Pix rainCloud() {
    Pix px(22, 9);
    lumps(px, {{6, 6, 4}, {12, 4.5, 5}, {17, 6.5, 3.8}, {3, 8, 2.6}, {19.5, 8.5, 2.4}}, DarkGrey, Grey);
    px.rect(2, 9, 20, 9, DarkGrey);
    px.fill([](double, double y) { return y > 10; }, -1);
    for (int r = 5; r < 9; ++r)
        for (int x = 3 + (r % 2) * 2; x < 21; x += 4)
            px.glyph(x, r, U'/', r % 2 ? LightBlue : Blue);
    return px;
}

Pix sunRays() {
    Pix px(21, 11);
    const double cx = 10.5, cy = 10.5;
    for (int i = 0; i < 8; ++i) {
        const double a = i * kPi / 4;
        const double ca = std::cos(a), sa = std::sin(a);
        const double nx = -sa, ny = ca;
        const double w0 = 1.5, w1 = 0.8, r0 = 5.5, r1 = i % 2 ? 9.2 : 10.4;
        px.poly({{cx + r0 * ca + w0 * nx, cy + r0 * sa + w0 * ny}, {cx + r1 * ca + w1 * nx, cy + r1 * sa + w1 * ny},
                 {cx + r1 * ca - w1 * nx, cy + r1 * sa - w1 * ny}, {cx + r0 * ca - w0 * nx, cy + r0 * sa - w0 * ny}},
                Yellow);
    }
    px.disc(cx, cy, 5.2, Yellow);
    px.disc(cx - 1, cy - 0.5, 2.6, White);
    return px;
}

Pix birds() {
    Pix px(26, 6);
    // a wide bird and a small one, as pixel rows from a corner
    const std::vector<std::vector<int>> big{{0, 1, 7, 8}, {1, 2, 3, 5, 6, 7}, {3, 4, 5}};
    const std::vector<std::vector<int>> small{{0, 1, 5, 6}, {1, 2, 4, 5}, {2, 3, 4}};
    const struct {
        int x, y;
        bool big;
    } fl[] = {{0, 5, true}, {11, 0, false}, {14, 7, true}, {6, 9, false}};
    for (const auto& b : fl) {
        const auto& rows = b.big ? big : small;
        for (int r = 0; r < 3; ++r)
            for (const int x : rows[r])
                px.set(b.x + x, b.y + r, White);
    }
    return px;
}

Pix rainSheet() {
    Pix px(16, 8);
    for (int r = 0; r < 8; ++r)
        for (int x = (r * 3) % 4; x < 16; x += 4)
            px.glyph(x, r, U'/', (x + r) % 3 ? LightBlue : Blue);
    return px;
}

Pix snowfall() {
    Pix px(22, 8);
    std::uint32_t s = 7;
    const auto next = [&] { return (s = s * 1664525u + 1013904223u) >> 8; };
    for (int r = 0; r < 8; ++r)
        for (int x = int(next() % 3); x < 22; x += 3 + int(next() % 3)) {
            const int k = int(next() % 4);
            px.glyph(x, r, k == 0 ? U'*' : k == 1 ? U'∙' : U'·', k == 0 ? White : k == 1 ? White : Grey);
        }
    px.glyph(0, 0, U'*', White);
    px.glyph(21, 7, U'*', White);
    return px;
}

// ---- things -----------------------------------------------------------------

Pix sailboat() {
    Pix px(17, 10);
    px.rect(8, 0, 8, 15, Brown);  // mast
    px.poly({{9, 1}, {9, 15}, {16.5, 15}}, White);
    px.poly({{7.5, 3}, {7.5, 15}, {1.5, 15}}, Grey);
    px.poly({{0, 16}, {17, 16}, {14, 20}, {3, 20}}, Brown);
    px.rect(1, 16, 15, 16, Red);
    return px;
}



Pix signpost() {
    Pix px(18, 8);
    px.rect(8, 1, 9, 15, Brown);
    px.poly({{2, 2}, {14, 2}, {16.5, 4}, {14, 6}, {2, 6}}, Brown);    // to the right
    px.poly({{3.5, 8}, {16, 8}, {16, 12}, {3.5, 12}, {1, 10}}, Brown);  // to the left
    px.rect(2, 2, 14, 2, Yellow);
    px.rect(4, 8, 15, 8, Yellow);
    return px;
}

Pix lantern() {
    Pix px(7, 7);
    px.rect(2, 0, 4, 0, DarkGrey);  // the handle
    px.rect(2, 1, 2, 1, DarkGrey);
    px.rect(4, 1, 4, 1, DarkGrey);
    px.rect(2, 2, 4, 2, DarkGrey);
    px.rect(1, 3, 5, 3, DarkGrey);
    px.rect(1, 4, 5, 10, DarkGrey);
    px.rect(2, 4, 4, 10, Yellow);
    px.rect(3, 6, 3, 8, White);  // the flame
    px.rect(1, 11, 5, 12, DarkGrey);
    px.rect(2, 13, 4, 13, DarkGrey);
    return px;
}

// ---- symbols ----------------------------------------------------------------

Pix heart() {
    Pix px(9, 4);
    const char* rows[] = {".XX...XX.", "XXXX.XXXX", "XXXXXXXXX", "XXXXXXXXX", ".XXXXXXX.", "..XXXXX..", "...XXX...", "....X...."};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 9; ++x)
            if (rows[y][x] == 'X')
                px.set(x, y, x + y >= 9 ? Red : LightRed);
    px.rect(1, 1, 2, 1, White);
    px.set(1, 2, White);
    return px;
}

Pix star() {
    Pix px(13, 7);
    const double cx = 6.5, cy = 7.2, ro = 6.6, ri = 2.7;
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i < 10; ++i) {
        const double a = -kPi / 2 + i * kPi / 5, r = i % 2 ? ri : ro;
        pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    px.poly(pts, Yellow);
    return px;
}

Pix skull() {
    Pix px(11, 6);
    const char* rows[] = {"..XXXXXXX..", ".XXXXXXXXX.", "XXXXXXXXXXX", "XXXXXXXXXXX", "XX..XXX..XX", "XX..XXX..XX",
                          "XXXXX.XXXXX", ".XXXX.XXXX.", "..XXXXXXX..", "..X.X.X.X..", "..XXXXXXX..", "...XXXXX..."};
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 11; ++x)
            px.set(x, y, rows[y][x] == 'X' ? (x >= 8 || y >= 10 ? Grey : White) : -1);
    for (int y = 0; y < 12; ++y)
        for (int x = 1; x < 10; ++x)
            if (rows[y][x] == '.' && px.at(x - 1, y) >= 0 && (px.at(x + 1, y) >= 0 || px.at(x + 2, y) >= 0))
                px.set(x, y, Black);  // eyes, nose and the gaps between the teeth
    return px;
}

Pix arrow() {
    Pix px(22, 4);
    px.rect(0, 3, 14, 4, Yellow);
    px.poly({{13, -0.5}, {22, 4}, {13, 8.5}}, Yellow);
    px.rect(0, 5, 0, 5, -1);
    // the lower edge of shaft and head alike in shade (the light is above)
    const Pix lit = px;
    for (int y = 0; y < px.h; ++y)
        for (int x = 0; x < px.w; ++x)
            if (lit.at(x, y) >= 0 && lit.at(x, y + 1) < 0)
                px.set(x, y, Brown);
    return px;
}

Pix speechBubble() {
    Pix px(20, 6);
    px.rect(1, 0, 18, 8, White);
    px.rect(0, 1, 19, 7, White);
    px.poly({{4, 8}, {10, 8}, {3, 12}}, White);
    px.rect(1, 8, 18, 8, Grey);
    px.rect(19, 1, 19, 7, Grey);
    return px;
}

Pix ribbonBanner() {
    Pix px(32, 5);
    // the tails behind, each with a notch cut in its end
    px.poly({{0, 3}, {7, 3}, {7, 10}, {0, 10}, {2.5, 6.5}}, Red);
    px.poly({{25, 3}, {32, 3}, {29.5, 6.5}, {32, 10}, {25, 10}}, Red);
    px.rect(5, 0, 26, 7, LightRed);
    px.rect(5, 0, 26, 0, White);
    px.poly({{5, 8}, {7, 8}, {7, 10}}, Black);  // the folds
    px.poly({{25, 8}, {27, 8}, {25, 10}}, Black);
    return px;
}


struct Spec {
    const char* category;
    const char* title;
    std::vector<std::string> tags;
    std::function<Pix()> draw;
};

std::vector<Spec> specs() {
    return {
        {"trees", "Round tree", {"tree", "deciduous", "summer", "green"}, roundTree},
        {"trees", "Oak tree", {"tree", "oak", "deciduous", "large", "green"}, oakTree},
        {"trees", "Birch tree", {"tree", "birch", "deciduous", "white"}, birchTree},
        {"trees", "Pine tree", {"tree", "pine", "conifer", "evergreen"}, pineTree},
        {"trees", "Bush", {"bush", "shrub", "green"}, bush},
        {"trees", "Hedge", {"hedge", "bush", "garden", "green"}, hedge},
        {"plants", "Grass tufts", {"grass", "ground", "green"}, grassTufts},
        {"plants", "Flowers", {"flowers", "garden", "spring"}, flowers},
        {"plants", "Reeds", {"reeds", "cattails", "pond", "marsh"}, reeds},
        {"land", "Rocks", {"rock", "stones", "ground"}, rocks},
        {"land", "Hill", {"hill", "grass", "green", "ground"}, hill},
        {"land", "Cliff edge", {"cliff", "rock", "edge", "grass"}, cliffEdge},
        {"buildings", "House", {"house", "home", "roof", "windows"}, house},
        {"buildings", "Barn", {"barn", "farm", "red"}, barn},
        {"buildings", "Castle keep", {"castle", "keep", "tower", "stone"}, castleKeep},
        {"buildings", "Lighthouse", {"lighthouse", "tower", "coast", "light"}, lighthouse},
        {"buildings", "Windmill", {"windmill", "mill", "farm"}, windmill},
        {"buildings", "Fence", {"fence", "wood", "farm"}, fence},
        {"buildings", "Stone bridge", {"bridge", "arch", "stone"}, stoneBridge},
        {"water", "Calm water", {"water", "calm", "lake", "sea", "band"}, calmWater},
        {"sky", "Rain cloud", {"cloud", "rain", "storm", "grey"}, rainCloud},
        {"sky", "Sun with rays", {"sun", "rays", "day"}, sunRays},
        {"sky", "Birds in flight", {"birds", "flying", "gulls"}, birds},
        {"sky", "Rain", {"rain", "weather", "storm"}, rainSheet},
        {"sky", "Snowfall", {"snow", "weather", "winter"}, snowfall},
        {"things", "Sailboat", {"boat", "sail", "sea"}, sailboat},
        {"things", "Signpost", {"sign", "post", "arrows", "wood"}, signpost},
        {"things", "Lantern", {"lantern", "light", "lamp"}, lantern},
        {"symbols", "Heart", {"heart", "love", "red"}, heart},
        {"symbols", "Star", {"star", "gold", "award"}, star},
        {"symbols", "Skull", {"skull", "bones", "danger"}, skull},
        {"symbols", "Arrow", {"arrow", "pointer", "right"}, arrow},
        {"symbols", "Speech bubble", {"speech", "bubble", "talk", "balloon"}, speechBubble},
        {"symbols", "Ribbon banner", {"ribbon", "banner", "title"}, ribbonBanner},
    };
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bw-make-stamps <library dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    try {
        for (const Spec& s : specs()) {
            const CellBlock block = cells(s.draw());
            library::StampFiles files = library::encodeStamp(s.title, s.category, s.tags, block, true);
            // The SAUCE record is the file's last 128 bytes: its author (at
            // 42, 20 bytes, space-padded) is funkpower, and its date (at 82)
            // is fixed, so a stamp made again is the same file.
            const size_t sauce = files.ans.size() - 128;
            files.ans.replace(sauce + 42, 20, std::string(kAuthor) + std::string(20 - std::strlen(kAuthor), ' '));
            files.ans.replace(sauce + 82, 8, kDate);
            const fs::path out = dir / s.category / (library::slug(s.title) + ".ans");
            fs::create_directories(out.parent_path());
            library::writeStamp(out, files);
            std::printf("%s %dx%d\n", out.lexically_relative(dir).string().c_str(), block.width, block.height);
        }
    } catch (const std::exception& e) {
        std::cerr << "bw-make-stamps: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
