#include "lint_fallback.h"

#include "blockwright/halfblock.h"

#include <algorithm>
#include <string>

namespace bw::lint::fallback {

namespace {

bool sameCell(const Cell& a, const Cell& b) { return a.ch == b.ch && a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs; }

// A cell's rung for the rule, -1 off the ladder, off the box or black: black
// is ink (a silhouette, an outline), not the shading.
int shadingRung(const Canvas& canvas, const Ramp& ramp, const Box& box, int x, int y, long long& reads) {
    if (x < box.x0 || y < box.y0 || x > box.x1 || y > box.y1)
        return -1;
    ++reads;
    const auto g = rungOf(ramp, canvas.at(x, y));
    if (!g || (*g % kRungsPerStep == 0 && ramp.colours[size_t(*g / kRungsPerStep)] == Black))
        return -1;
    return *g;
}

// Whether `colour` is one rung g shows: its solid colour, ░'s background, ▓'s foreground, and
// both of ▒'s; with iCE off and the other half bright, also the dark
// counterpart of either colour of its step, where a bright one can't show.
bool shows(const Ramp& ramp, int g, std::uint8_t colour, std::uint8_t other, bool ice) {
    const auto i = size_t(g / kRungsPerStep);
    const int k = g % kRungsPerStep;
    const std::uint8_t low = ramp.colours[i], high = k ? ramp.colours[i + 1] : low;
    if (colour == (k == 3 ? high : low) || (k == 2 && colour == high))
        return true;
    return !ice && other >= 8 && (colour == (low & 7) || colour == (high & 7));
}

// Whether `colour` is one a shade cell of rung g can fall back to when a
// pixel cuts it: either colour of its glyph, and with iCE off their dark
// counterparts. A solid cell has nothing to fall back to.
bool fallsTo(const Ramp& ramp, int g, std::uint8_t colour, bool ice) {
    if (g % kRungsPerStep == 0)
        return false;
    const auto i = size_t(g / kRungsPerStep);
    const std::uint8_t low = ramp.colours[i], high = ramp.colours[i + 1];
    return colour == low || colour == high || (!ice && (colour == (low & 7) || colour == (high & 7)));
}

// Whether the cell at x, y shows `colour` in its bottom half (its top when
// `bottom` is false): the half of the cell beside it facing it goes on
// into it, as a shape's edge goes on into the shape.
bool continues(const Canvas& canvas, const Box& box, int x, int y, bool bottom, std::uint8_t colour, bool ice,
               long long& reads) {
    if (y < box.y0 || y > box.y1)
        return false;
    ++reads;
    const auto halves = halfBlocks(canvas.at(x, y), ice);
    return halves && (bottom ? halves->bottom : halves->top) == colour;
}

// The rule at x, y, whose run of equal cells along the row is x0..x1.
bool judge(const Canvas& canvas, const Ramp& ramp, const Box& box, int x, int y, int x0, int x1, bool ice,
           long long& reads) {
    const Cell& c = canvas.at(x, y);
    if (c.ch != kUpperHalf && c.ch != kLowerHalf)
        return false;
    const auto halves = halfBlocks(c, ice);
    if (!halves || halves->top == halves->bottom)
        return false;
    // the run's ends, and the cells above and below: the shading the cell
    // was cut from
    const int left = shadingRung(canvas, ramp, box, x0 - 1, y, reads),
              right = shadingRung(canvas, ramp, box, x1 + 1, y, reads),
              facing[2] = {shadingRung(canvas, ramp, box, x, y - 1, reads),
                           shadingRung(canvas, ramp, box, x, y + 1, reads)};
    auto shade = [](int g) { return g >= 0 && g % kRungsPerStep != 0; };
    if (!shade(left) && !shade(right) && !shade(facing[0]) && !shade(facing[1]))
        return false;
    // A half is judged against the cell it faces: the shade there is that
    // rung, the one two of it and the ends agree on, or failing that one
    // rung either way of each. A judged half fits when it shows a colour of
    // one of those rungs, has fallen when it shows another colour one of
    // them could fall back to, and was drawn otherwise. The cell is a speck
    // when every half judged has fallen and a half not judged goes on into
    // the cell it faces: the pixel is the edge of a shape there. A cell
    // with a drawn half was drawn there: a bevel over its outline, a frame's
    // border, a stripe. A lone pixel cut into the shading, its other half
    // fallen, can't be told from one, so it isn't flagged; nor is a lone
    // pixel whose other half faces ink, the canvas's edge or a cell off the
    // ladder.
    int judged = 0;
    for (const int half : {0, 1}) {
        const int g = facing[half];
        const std::uint8_t colour = half ? halves->bottom : halves->top, other = half ? halves->top : halves->bottom;
        auto unjudged = [&] { return continues(canvas, box, x, half ? y + 1 : y - 1, !half, colour, ice, reads); };
        if (g < 0) {
            if (!unjudged())
                return false;
            continue;
        }
        int lo = g - 1, hi = g + 1;
        if (g == left || g == right)
            lo = hi = g;
        else if (left >= 0 && left == right)
            lo = hi = left;
        else
            for (const int e : {left, right})
                if (e >= 0) {
                    lo = std::max(lo, e - 1);
                    hi = std::min(hi, e + 1);
                }
        lo = std::max(lo, 0);
        hi = std::min(hi, ramp.topRung());
        if (lo > hi) {
            if (!unjudged())
                return false;
            continue;
        }
        ++judged;
        bool falls = false;
        for (int r = lo; r <= hi; ++r) {
            if (shows(ramp, r, colour, other, ice))
                return false;
            falls = falls || fallsTo(ramp, r, colour, ice);
        }
        if (!falls)
            return false;
    }
    return judged > 0;
}

} // namespace

bool speckAt(const Canvas& canvas, const Ramp& ramp, const Box& box, int x, int y, bool ice, long long& reads) {
    const Cell& c = canvas.at(x, y);
    ++reads;
    if (c.ch != kUpperHalf && c.ch != kLowerHalf)
        return false;
    int x0 = x, x1 = x;
    while (x0 > box.x0 && (++reads, sameCell(canvas.at(x0 - 1, y), c)))
        --x0;
    while (x1 < box.x1 && (++reads, sameCell(canvas.at(x1 + 1, y), c)))
        ++x1;
    return judge(canvas, ramp, box, x, y, x0, x1, ice, reads);
}

void row(const Canvas& canvas, const Ramp& ramp, const Box& box, int y, bool ice, std::vector<char>& out,
                 long long& reads) {
    out.assign(size_t(box.x1 - box.x0 + 1), 0);
    for (int x0 = box.x0; x0 <= box.x1;) {
        const Cell& c = canvas.at(x0, y);
        int x1 = x0;
        while (x1 < box.x1 && (++reads, sameCell(canvas.at(x1 + 1, y), c)))
            ++x1;
        ++reads;
        if (c.ch == kUpperHalf || c.ch == kLowerHalf)
            for (int x = x0; x <= x1; ++x)
                out[size_t(x - box.x0)] = judge(canvas, ramp, box, x, y, x0, x1, ice, reads);
        x0 = x1 + 1;
    }
}

LadderFinding finding(int x, int y) {
    return {LadderFinding::Kind::FallbackSpeck, x, y,
            "cell " + std::to_string(x) + "," + std::to_string(y) +
                ": a fallback speck, a half block in a colour the shading beside it doesn't show but falls back to "
                "where a pixel cuts it"};
}

} // namespace bw::lint::fallback
