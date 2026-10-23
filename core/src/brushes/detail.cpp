#include "blockwright/brushes/detail.h"

#include "blockwright/ansi.h"
#include "blockwright/brushes/shading.h"

#include <cmath>

namespace bw {

namespace {

bool halfBlock(char32_t ch) { return ch == kUpperHalf || ch == kLowerHalf || ch == U'▌' || ch == U'▐'; }

// The ramp's ground: its first colour when that is black (the ground a
// piece is drawn on), never part of the shape; -1 when the ramp doesn't
// start at black and its first colour is part of the shape.
int groundOf(const Ramp& ramp) { return !ramp.colours.empty() && ramp.colours[0] == Black ? 0 : -1; }

// A cell of the shape as the rim-light brush reads it, and which of its
// four sub-pixels (2 x 2: ▀ and ▄ split a cell top and bottom, ▌ and ▐ left
// and right) are in the shape.
struct Part {
    bool fringe = false;  // a half block with one half the black ground
    bool in[2][2] = {{true, true}, {true, true}};  // [row][column], top left first
};

std::optional<Part> partOf(const Ramp& ramp, const Cell& cell, bool ice) {
    const Cell c = ice ? cell : iceCell(cell, false);
    const int ground = groundOf(ramp);
    if (const auto rung = rungOf(ramp, c)) {
        if (*rung % kRungsPerStep == 0 && *rung / kRungsPerStep == ground)
            return std::nullopt;
        return Part{};
    }
    // a half block whose halves are ramp colours, or one a ramp colour and
    // the other black: the fringe a shape tool or the pencil leaves
    if (!halfBlock(c.ch) || (c.attrs & attr::Blink) || !c.fg.isIndexed() || !c.bg.isIndexed())
        return std::nullopt;
    const int f = c.fg.index == Black ? ground : ramp.indexOf(c.fg.index),
              b = c.bg.index == Black ? ground : ramp.indexOf(c.bg.index);
    const bool fBlack = c.fg.index == Black, bBlack = c.bg.index == Black;
    if ((f < 0 && !fBlack) || (b < 0 && !bBlack))
        return std::nullopt;
    const bool fGround = fBlack || f == ground, bGround = bBlack || b == ground;
    if (fGround && bGround)
        return std::nullopt;
    // ▀ is the foreground on top, ▄ below, ▌ on the left, ▐ on the right
    Part p{fGround || bGround};
    const bool fIn = !fGround, bIn = !bGround;
    if (c.ch == kUpperHalf || c.ch == kLowerHalf) {
        const bool top = c.ch == kUpperHalf ? fIn : bIn, bottom = c.ch == kUpperHalf ? bIn : fIn;
        p.in[0][0] = p.in[0][1] = top;
        p.in[1][0] = p.in[1][1] = bottom;
    } else {
        const bool left = c.ch == U'▌' ? fIn : bIn, right = c.ch == U'▌' ? bIn : fIn;
        p.in[0][0] = p.in[1][0] = left;
        p.in[0][1] = p.in[1][1] = right;
    }
    return p;
}

// A full cell is on the edge when one of its eight neighbours is outside or
// a fringe; a fringe always is.
bool edgeAt(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice, const Part& p) {
    if (p.fringe)
        return true;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!(dx || dy) || !canvas.contains(x + dx, y + dy))
                continue;
            const auto q = partOf(ramp, canvas.at(x + dx, y + dy), ice);
            if (!q || q->fringe)
                return true;
        }
    return false;
}

// The outward normal at (x, y), taken on a grid of 2 x 2 sub-pixels a
// cell, a sub-pixel half a column wide and a column tall on screen: the sum
// of the offsets from the cell's centre to the outside sub-pixels of the
// cells within three columns' distance (three columns each way on the
// cell's row, two on the rows above and below), a window round on screen so
// no direction is favoured, each sub-pixel weighted a half. A sub-pixel is
// outside when its cell is outside, or it is in a fringe's black half; off
// the canvas is not outside. Weighted so, an outside cell adds twice its
// centre's offset and a ▀/▄ fringe's black half its pixel's, so full cells
// and ▀/▄ fringes have the normals they had on the half-block pixel grid.
// The sums are multiples of an eighth and exact.
std::optional<std::pair<double, double>> normalAt(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    double gx = 0, gy = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            if (dx * dx + 4 * dy * dy > 9 || !canvas.contains(x + dx, y + dy))
                continue;
            const auto p = partOf(ramp, canvas.at(x + dx, y + dy), ice);
            // the sub-pixels' centres are a quarter column either side of
            // the cell's and half a column above and below it
            for (int r = 0; r < 2; ++r)
                for (int col = 0; col < 2; ++col)
                    if (!(p && p->in[r][col])) {
                        gx += 0.5 * (dx + (col ? 0.25 : -0.25));
                        gy += 0.5 * (2 * dy + (r ? 0.5 : -0.5));
                    }
        }
    const double len = std::hypot(gx, gy);
    if (len < 1e-9)
        return std::nullopt;
    return std::pair{gx / len, gy / len};
}

// Whether a light from `light` can reach (x, y) past its neighbours: not
// when the neighbour straight towards the light (a step along each axis the
// light comes from) is a full shape cell while the one straight away from
// it is outside or a fringe. Such a cell is taken to face away from the
// light, into the outside behind it, however its window normal sums: the
// column of a narrow shape that faces a gap can see more outside on the
// shape's far side, within three columns, than in the gap. Only that one
// pair of neighbours is checked; the known limits are in detail.h.
bool openTowards(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice, Compass light) {
    const auto [lx, ly] = compassVector(light);
    const int sx = lx > 1e-9 ? 1 : lx < -1e-9 ? -1 : 0, sy = ly > 1e-9 ? 1 : ly < -1e-9 ? -1 : 0;
    // a cell on the canvas that is outside or a fringe; off the canvas is
    // neither, nor a full shape cell
    const auto part = [&](int cx, int cy) { return partOf(ramp, canvas.at(cx, cy), ice); };
    const bool inside = canvas.contains(x + sx, y + sy) && part(x + sx, y + sy) && !part(x + sx, y + sy)->fringe;
    const bool behind = canvas.contains(x - sx, y - sy) && (!part(x - sx, y - sy) || part(x - sx, y - sy)->fringe);
    return !(inside && behind);
}

} // namespace

bool onEdge(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    if (!canvas.contains(x, y))
        return false;
    const auto p = partOf(ramp, canvas.at(x, y), ice);
    return p && edgeAt(canvas, ramp, x, y, ice, *p);
}

std::optional<std::pair<double, double>> edgeNormal(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    if (!onEdge(canvas, ramp, x, y, ice))
        return std::nullopt;
    return normalAt(canvas, ramp, x, y, ice);
}

RimLightBrush::RimLightBrush(Ramp ramp, Compass light) : ramp_(std::move(ramp)), light_(light) {}

bool RimLightBrush::facesLight(std::pair<double, double> normal, Compass light) {
    // the compass is on screen, as the normal is
    const auto [lx, ly] = compassVector(light);
    return normal.first * lx + normal.second * ly > 0.3;
}

std::optional<Cell> RimLightBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    const Canvas& c = stroke.canvas();
    const bool ice = stroke.options().ice;
    // A fringe can't go a rung lighter (a half block has one colour a half,
    // and a rung is a shade between two), so it stays; the full cells round
    // it are on the edge, and lit where their own normal faces the light
    const auto part = partOf(ramp_, c.at(x, y), ice);
    if (!part || part->fringe || !edgeAt(c, ramp_, x, y, ice, *part))
        return std::nullopt;
    const auto n = normalAt(c, ramp_, x, y, ice);
    if (!n || !facesLight(*n, light_) || !openTowards(c, ramp_, x, y, ice, light_))
        return std::nullopt;
    return stepRung(ramp_, c.at(x, y), 1, ice);
}

} // namespace bw
