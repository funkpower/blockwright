#include "blockwright/text_style.h"

#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/composite.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/style_draw.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace bw {

namespace {

constexpr std::array<std::pair<EffectKind, std::string_view>, 7> kKindNames = {{
    {EffectKind::Plate, "plate"},
    {EffectKind::Glow, "glow"},
    {EffectKind::Shadow, "shadow"},
    {EffectKind::Outline, "outline"},
    {EffectKind::Fill, "fill"},
    {EffectKind::Shade, "shade"},
    {EffectKind::Bevel, "bevel"},
}};
constexpr std::array<std::pair<ShadowForm, std::string_view>, 2> kShadowNames = {{
    {ShadowForm::Drop, "drop"},
    {ShadowForm::Extrude, "extrude"},
}};
constexpr std::array<std::pair<OutlineForm, std::string_view>, 2> kOutlineNames = {{
    {OutlineForm::Pixels, "pixels"},
    {OutlineForm::Cells, "cells"},
}};
constexpr std::array<std::pair<FillMode, std::string_view>, 4> kFillNames = {{
    {FillMode::Flat, "flat"},
    {FillMode::Rows, "rows"},
    {FillMode::Columns, "columns"},
    {FillMode::Rainbow, "rainbow"},
}};
constexpr std::array<std::pair<Compass, std::string_view>, 8> kCompassNames = {{
    {Compass::N, "n"},
    {Compass::NE, "ne"},
    {Compass::E, "e"},
    {Compass::SE, "se"},
    {Compass::S, "s"},
    {Compass::SW, "sw"},
    {Compass::W, "w"},
    {Compass::NW, "nw"},
}};

template <typename E, std::size_t N>
std::string_view nameOf(const std::array<std::pair<E, std::string_view>, N>& names, E e) {
    for (const auto& [v, n] : names)
        if (v == e)
            return n;
    return {};
}

template <typename E, std::size_t N>
std::optional<E> fromName(const std::array<std::pair<E, std::string_view>, N>& names, std::string_view s) {
    for (const auto& [v, n] : names)
        if (n == s)
            return v;
    return std::nullopt;
}

// The unit step towards a compass point: x across, y down.
std::pair<int, int> step(Compass c) {
    const auto [vx, vy] = compassVector(c);
    return {(vx > 0.1) - (vx < -0.1), (vy > 0.1) - (vy < -0.1)};
}

} // namespace

std::string_view effectKindName(EffectKind kind) { return nameOf(kKindNames, kind); }
std::optional<EffectKind> effectKindFromName(std::string_view name) { return fromName(kKindNames, name); }
std::string_view shadowFormName(ShadowForm form) { return nameOf(kShadowNames, form); }
std::optional<ShadowForm> shadowFormFromName(std::string_view name) { return fromName(kShadowNames, name); }
std::string_view outlineFormName(OutlineForm form) { return nameOf(kOutlineNames, form); }
std::optional<OutlineForm> outlineFormFromName(std::string_view name) { return fromName(kOutlineNames, name); }
std::string_view fillModeName(FillMode mode) { return nameOf(kFillNames, mode); }
std::optional<FillMode> fillModeFromName(std::string_view name) { return fromName(kFillNames, name); }
std::string_view compassPointName(Compass c) { return nameOf(kCompassNames, c); }
std::optional<Compass> compassPointFromName(std::string_view name) { return fromName(kCompassNames, name); }

std::optional<std::string> textStyleProblem(const TextStyle& style) {
    if (style.effects.size() > TextStyle::kMaxEffects)
        return "the style has " + std::to_string(style.effects.size()) + " effects, over " +
               std::to_string(TextStyle::kMaxEffects);
    std::set<EffectKind> seen;
    const auto colour = [](const char* what, std::uint8_t c) -> std::optional<std::string> {
        if (c > 15)
            return std::string(what) + " colour " + std::to_string(c) + " is not a palette index 0 to 15";
        return std::nullopt;
    };
    const auto within = [](const char* what, int v, int lo, int hi) -> std::optional<std::string> {
        if (v < lo || v > hi)
            return std::string(what) + " " + std::to_string(v) + " is not " + std::to_string(lo) + " to " +
                   std::to_string(hi);
        return std::nullopt;
    };
    const auto ramp = [](const char* what, const std::string& name) -> std::optional<std::string> {
        if (!findRamp(name))
            return std::string(what) + " ramp \"" + name + "\" is not a named ramp";
        return std::nullopt;
    };
    for (const Effect& e : style.effects) {
        const EffectKind kind = e.kind();
        if (!seen.insert(kind).second)
            return "the style has two " + std::string(effectKindName(kind)) + " effects";
        std::optional<std::string> problem;
        std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, PlateEffect>) {
                    if (!(problem = colour("plate", s.colour)))
                        problem = within("plate padding", s.padding, 0, TextStyle::kMaxPadding);
                } else if constexpr (std::is_same_v<T, GlowEffect>) {
                    if (!(problem = ramp("glow", s.ramp)) &&
                        !(problem = within("glow width", s.width, 1, TextStyle::kMaxGlow)) &&
                        !(problem = within("glow brightness", s.brightness, 1, TextStyle::kMaxGlow))) {
                        const int top = findRamp(s.ramp)->topRung();
                        for (const auto& [what, v] : {std::pair{"width", s.width}, {"brightness", s.brightness}})
                            if (!problem && v > top)
                                problem = "glow " + std::string(what) + " " + std::to_string(v) + " is over ramp \"" +
                                          s.ramp + "\"'s " + std::to_string(top) + " rungs";
                    }
                } else if constexpr (std::is_same_v<T, ShadowEffect>) {
                    if (!(problem = colour("shadow", s.colour)))
                        problem = within("shadow distance", s.distance, 1, TextStyle::kMaxDistance);
                } else if constexpr (std::is_same_v<T, OutlineEffect>) {
                    problem = colour("outline", s.colour);
                } else if constexpr (std::is_same_v<T, FillEffect>) {
                    if (s.mode == FillMode::Flat)
                        problem = colour("fill", s.colour);
                    else
                        problem = ramp("fill", s.ramp);
                } else if constexpr (std::is_same_v<T, ShadeEffect>) {
                    problem = ramp("shade", s.ramp);
                } else if constexpr (std::is_same_v<T, BevelEffect>) {
                    problem = ramp("bevel", s.ramp);
                }
            },
            e.settings);
        if (problem)
            return problem;
    }
    return std::nullopt;
}

StyleMargins styleMargins(const TextStyle& style) {
    StyleMargins m;
    const int outline = style.get<OutlineEffect>() ? 1 : 0;
    const int glow = style.get<GlowEffect>() ? style.get<GlowEffect>()->width : 0;
    int left = glow, top = glow, right = glow, bottom = glow;
    if (const ShadowEffect* s = style.get<ShadowEffect>()) {
        const auto [dx, dy] = step(s->direction);
        const int across = s->distance, down = (s->distance + 1) / 2;
        if (dx < 0)
            left = std::max(left, across);
        if (dx > 0)
            right = std::max(right, across);
        if (dy < 0)
            top = std::max(top, down);
        if (dy > 0)
            bottom = std::max(bottom, down);
    }
    const int padding = style.get<PlateEffect>() ? style.get<PlateEffect>()->padding : 0;
    m.left = outline + left + padding;
    m.top = outline + top + padding;
    m.right = outline + right + padding;
    m.bottom = outline + bottom + padding;
    return m;
}

namespace {

// Rung `rung` of the ramp as a cell; a solid's cell as rungCell writes it.
Cell rungAt(const Ramp& ramp, int rung) { return rungCell(ramp, rung); }

// The rungs a run of targets is written as: the targets themselves with iCE
// on; with it off each walked from the rung before it (the first from the
// ramp's darkest colour) a rung at a time by stepRung, stopping short of the
// first rung that needs a bright background, so the walk never jumps past
// one and no two neighbours are more than a rung apart.
std::vector<int> walkRungs(const Ramp& ramp, const std::vector<int>& targets, bool ice) {
    std::vector<int> out;
    out.reserve(targets.size());
    int prev = 0;
    for (const int t : targets) {
        if (ice) {
            out.push_back(t);
            continue;
        }
        while (prev != t) {
            const auto c = stepRung(ramp, rungAt(ramp, prev), t > prev ? 1 : -1, false);
            const auto r = c ? rungOf(ramp, *c) : std::nullopt;
            if (!r || std::abs(*r - prev) != 1)
                break;
            prev = *r;
        }
        out.push_back(prev);
    }
    return out;
}

// The palette colour's perceived brightness, for ranking.
int luma(std::uint8_t i) {
    const Rgb& c = kPalette[i & 15];
    return 299 * c.r + 587 * c.g + 114 * c.b;
}

// The ramp step a background at step `i` shows: the step itself with iCE
// on; with it off, which shows no bright background, the nearest step at or
// below it whose colour is dark.
std::size_t backStep(const Ramp& ramp, std::size_t i, bool ice) {
    if (!ice)
        while (i > 0 && ramp.colours[i] >= 8)
            --i;
    return i;
}

// A source cell's foreground and background as ranks (-1 for black, which
// stays step 0), for the cells whose two colours differ.
using RankPair = std::pair<int, int>;
// Each pair and how many cells it is on.
using RankPairs = std::map<RankPair, int>;

// How badly `steps` (a step for each rank) draws the cells of `pairs`: how
// many cells come out one colour over itself, and how many with their
// foreground and background in the other order on the ramp (or one step).
std::pair<int, int> rankFaults(const Ramp& ramp, const std::vector<std::size_t>& steps, const RankPairs& pairs,
                               bool ice) {
    int same = 0, turned = 0;
    for (const auto& [pair, cells] : pairs) {
        const auto [f, b] = pair;
        const std::size_t fs = f < 0 ? 0 : steps[static_cast<std::size_t>(f)];
        const std::size_t bs = backStep(ramp, b < 0 ? 0 : steps[static_cast<std::size_t>(b)], ice);
        if (ramp.colours[fs] == (ramp.colours[bs] & (ice ? 15 : 7)))
            same += cells;
        if (fs == bs || (f < b) != (fs < bs))
            turned += cells;
    }
    return {same, turned};
}

// The ramp step for each of `n` colours ranked dark to light past black
// (#184), drawing the cells of `pairs`. First the consecutive steps ending
// at `top`, or failing that at the highest step below it, whose cells keep
// their two colours apart and in rank order; ranks below step 1 take step 1.
// With iCE off a background colour shows the dark step at or below its own
// (backStep), so the top window can merge two colours of one cell (blue on
// cyan under steel: grey on grey). When no window keeps every cell, the
// steps (each at most `top`, rising with rank) that keep the most cells
// apart, then the most in order, then put the fewest colours on black, then
// leave the fewest gaps, then reach highest.
std::vector<std::size_t> rankSteps(const Ramp& ramp, int top, int n, const RankPairs& pairs, bool ice) {
    const int lowest = std::min(1, top);
    std::vector<std::size_t> steps(static_cast<std::size_t>(n));
    for (int t = top; t >= lowest; --t) {
        for (int i = 0; i < n; ++i)
            steps[static_cast<std::size_t>(i)] = static_cast<std::size_t>(std::max(lowest, t - (n - 1 - i)));
        if (rankFaults(ramp, steps, pairs, ice) == std::pair{0, 0})
            return steps;
    }
    for (int i = 0; i < n; ++i)
        steps[static_cast<std::size_t>(i)] = static_cast<std::size_t>(std::max(lowest, top - (n - 1 - i)));
    // every rising run of n steps in 0..top: C(n + top, n), at most 15504
    // for a named ramp (15 colours, 6 steps); a longer search keeps the top
    // window
    double runs = 1;
    for (int i = 1; i <= n; ++i)
        runs = runs * (top + i) / i;
    if (n == 0 || runs > 1e5)
        return steps;
    using Score = std::array<int, 5>;
    const auto score = [&](const std::vector<std::size_t>& s) {
        const auto [same, turned] = rankFaults(ramp, s, pairs, ice);
        int black = 0, gaps = 0;
        for (int i = 0; i < n; ++i) {
            black += s[static_cast<std::size_t>(i)] == 0;
            if (i > 0)
                gaps += std::abs(int(s[static_cast<std::size_t>(i)]) - int(s[static_cast<std::size_t>(i - 1)]) - 1);
        }
        return Score{same, turned, black, gaps, -int(s.back())};
    };
    Score best = score(steps);
    std::vector<std::size_t> s(static_cast<std::size_t>(n), 0);
    for (;;) {
        if (const Score sc = score(s); sc < best) {
            best = sc;
            steps = s;
        }
        int i = n - 1;
        while (i >= 0 && int(s[static_cast<std::size_t>(i)]) == top)
            --i;
        if (i < 0)
            break;
        const std::size_t v = s[static_cast<std::size_t>(i)] + 1;
        for (int j = i; j < n; ++j)
            s[static_cast<std::size_t>(j)] = v;
    }
    return steps;
}

// One effect's layer of the style, the output's size.
Layer effectLayer(LayerKind kind, int w, int h) { return makeLayer(kind, w, h); }

// The style drawn, with its working grids, in output coordinates.
class Styler {
public:
    Styler(const StyleInput& in, const TextStyle& style, bool ice, StyleMargins m)
        : in_(in), style_(style), ice_(ice), ox_(m.left), oy_(m.top) {
        const int iw = in.mask.width, ih = in.mask.height / 2;
        w_ = iw + m.left + m.right;
        h_ = ih + m.top + m.bottom;
        ink_.assign(static_cast<std::size_t>(w_) * h_ * 2, 0);
        src_.assign(static_cast<std::size_t>(w_) * h_, 0);
        for (int py = 0; py < 2 * ih; ++py)
            for (int px = 0; px < iw; ++px)
                if (in.mask.at(px, py))
                    ink_[pix(px + ox_, py + 2 * oy_)] = 1;
        for (int y = 0; y < std::min(ih, in.source.height()); ++y)
            for (int x = 0; x < std::min(iw, in.source.width()); ++x)
                if (!in.source.at(x, y).transparent)
                    src_[cell(x + ox_, y + oy_)] = 1;
        for (const auto& [first, end] : in.bands)
            bands_.emplace_back(std::max(0, first) + 2 * oy_, std::min(2 * ih, end) + 2 * oy_);
    }

    Canvas draw() {
        LayerStack stack(w_, h_);
        grown_ = ink_;
        std::optional<Layer> outline = outlineLayer();
        if (const PlateEffect* p = style_.get<PlateEffect>())
            stack.add(plateLayer(*p));
        if (const GlowEffect* g = style_.get<GlowEffect>())
            stack.add(glowLayer(*g));
        if (const ShadowEffect* s = style_.get<ShadowEffect>())
            stack.add(shadowLayer(*s));
        if (outline)
            stack.add(std::move(*outline));
        // the letters as Fill leaves them, which Shade and Bevel read
        std::vector<Layer> letters = fillLayers();
        if (const ShadeEffect* s = style_.get<ShadeEffect>())
            if (auto l = shadeLayer(*s, letters))
                letters.push_back(std::move(*l));
        if (const BevelEffect* b = style_.get<BevelEffect>())
            if (auto l = bevelLayer(*b, letters))
                letters.push_back(std::move(*l));
        for (Layer& l : letters)
            stack.add(std::move(l));
        Canvas out = composite(stack, CompositeOptions{ice_, transparentCell()});
        // whatever effect drew it (a flat or ramp fill, shade, bevel), a
        // cell keeps its source cell's dither mark only while it looks as
        // that cell did; any other cell has none
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                Cell& c = out.at(x, y);
                c.dithered = c.dithered && source(x, y) && keepDitherMark(sourceCell(x, y), c).dithered;
            }
        return out;
    }

private:
    std::size_t pix(int px, int py) const { return static_cast<std::size_t>(py) * w_ + px; }
    std::size_t cell(int x, int y) const { return static_cast<std::size_t>(y) * w_ + x; }
    bool inPix(int px, int py) const { return px >= 0 && py >= 0 && px < w_ && py < 2 * h_; }
    bool ink(int px, int py) const { return inPix(px, py) && ink_[pix(px, py)]; }
    bool cellInk(int x, int y) const { return ink(x, 2 * y) || ink(x, 2 * y + 1); }
    bool source(int x, int y) const { return src_[cell(x, y)]; }
    const Cell& sourceCell(int x, int y) const { return in_.source.at(x - ox_, y - oy_); }
    // A full ink cell Shade and Bevel read: both pixels ink, and a source
    // cell only when it is █.
    bool fullInk(int x, int y) const {
        return ink(x, 2 * y) && ink(x, 2 * y + 1) && (!source(x, y) || sourceCell(x, y).ch == kFullBlock);
    }

    static void setPixel(Layer& l, int px, int py, std::uint8_t colour) {
        Cell& c = l.cells.at(px, py / 2);
        c = setLayerPixel(c, py & 1, colour);
    }

    Layer plateLayer(const PlateEffect& p) const {
        Layer l = effectLayer(LayerKind::Cells, w_, h_);
        const CellRect box{in_.box.x + ox_ - p.padding, in_.box.y + oy_ - p.padding, in_.box.width + 2 * p.padding,
                           in_.box.height + 2 * p.padding};
        const CellRect r = intersect(box, CellRect{0, 0, w_, h_});
        const Cell c = halfBlockCell(HalfPair{p.colour, p.colour}, ice_);
        for (int y = r.y; y < r.y + r.height; ++y)
            for (int x = r.x; x < r.x + r.width; ++x)
                l.cells.at(x, y) = c;
        return l;
    }

    // The outline's layer, growing the mask Glow and Shadow read by its
    // ring; empty when it is off. The ring stays out of source cells: the
    // letters cover them (a TheDraw glyph's blank cells, an open notch or a
    // counter), so a ring there would show nothing and cast a shadow from
    // nothing.
    std::optional<Layer> outlineLayer() {
        const OutlineEffect* o = style_.get<OutlineEffect>();
        if (!o)
            return std::nullopt;
        Layer l = effectLayer(LayerKind::Pixels, w_, h_);
        if (o->form == OutlineForm::Pixels) {
            for (int py = 0; py < 2 * h_; ++py)
                for (int px = 0; px < w_; ++px) {
                    if (ink(px, py) || source(px, py / 2))
                        continue;
                    bool near = false;
                    for (int dy = -1; dy <= 1 && !near; ++dy)
                        for (int dx = -1; dx <= 1 && !near; ++dx)
                            near = ink(px + dx, py + dy);
                    if (near) {
                        setPixel(l, px, py, o->colour);
                        grown_[pix(px, py)] = 1;
                    }
                }
        } else {
            for (int y = 0; y < h_; ++y)
                for (int x = 0; x < w_; ++x) {
                    if (cellInk(x, y) || source(x, y))
                        continue;
                    bool near = false;
                    for (int dy = -1; dy <= 1 && !near; ++dy)
                        for (int dx = -1; dx <= 1 && !near; ++dx)
                            near = x + dx >= 0 && y + dy >= 0 && x + dx < w_ && y + dy < h_ && cellInk(x + dx, y + dy);
                    if (near) {
                        setPixel(l, x, 2 * y, o->colour);
                        setPixel(l, x, 2 * y + 1, o->colour);
                        grown_[pix(x, 2 * y)] = grown_[pix(x, 2 * y + 1)] = 1;
                    }
                }
        }
        return l;
    }

    Layer glowLayer(const GlowEffect& g) const {
        Layer l = effectLayer(LayerKind::Cells, w_, h_);
        const std::optional<Ramp> ramp = findRamp(g.ramp);
        if (!ramp)
            return l;
        // Chebyshev distance in cells from a cell with grown ink, up to the
        // width: a breadth-first walk over the eight neighbours
        std::vector<int> dist(static_cast<std::size_t>(w_) * h_, -1);
        std::deque<std::pair<int, int>> queue;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (grown_[pix(x, 2 * y)] || grown_[pix(x, 2 * y + 1)]) {
                    dist[cell(x, y)] = 0;
                    queue.emplace_back(x, y);
                }
        while (!queue.empty()) {
            const auto [x, y] = queue.front();
            queue.pop_front();
            const int d = dist[cell(x, y)];
            if (d == g.width)
                continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_ || dist[cell(nx, ny)] >= 0)
                        continue;
                    dist[cell(nx, ny)] = d + 1;
                    queue.emplace_back(nx, ny);
                }
        }
        // A ring past the first that can't reach the halo's edge (a cell
        // past the width, or the canvas edge) without crossing the inner
        // ring, as in a counter the halo fills or where it meets itself
        // between letters and lines, would sit darker inside the inner ring
        // (#184): it holds the inner ring's rung.
        std::vector<char> open(dist.size(), 0);
        const auto fades = [&](int x, int y) { return dist[cell(x, y)] < 0 || dist[cell(x, y)] >= 2; };
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (dist[cell(x, y)] < 0 || (fades(x, y) && (x == 0 || y == 0 || x == w_ - 1 || y == h_ - 1))) {
                    open[cell(x, y)] = 1;
                    queue.emplace_back(x, y);
                }
        while (!queue.empty()) {
            const auto [x, y] = queue.front();
            queue.pop_front();
            for (const auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_ || open[cell(nx, ny)] || !fades(nx, ny))
                    continue;
                open[cell(nx, ny)] = 1;
                queue.emplace_back(nx, ny);
            }
        }
        std::vector<int> targets;
        for (int d = 0; d <= g.width; ++d)
            targets.push_back(d == 0 ? g.brightness : std::max(1, g.brightness - d + 1));
        const std::vector<int> rungs = walkRungs(*ramp, targets, ice_);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (int d = dist[cell(x, y)]; d >= 0) {
                    if (d >= 2 && !open[cell(x, y)])
                        d = 1;
                    l.cells.at(x, y) = rungAt(*ramp, rungs[static_cast<std::size_t>(d)]);
                }
        return l;
    }

    Layer shadowLayer(const ShadowEffect& s) const {
        Layer l = effectLayer(LayerKind::Pixels, w_, h_);
        const auto [dx, dy] = step(s.direction);
        const int from = s.form == ShadowForm::Drop ? s.distance : 1;
        for (int py = 0; py < 2 * h_; ++py)
            for (int px = 0; px < w_; ++px) {
                if (!grown_[pix(px, py)])
                    continue;
                for (int k = from; k <= s.distance; ++k) {
                    const int qx = px + k * dx, qy = py + k * dy;
                    if (inPix(qx, qy))
                        setPixel(l, qx, qy, s.colour);
                }
            }
        return l;
    }

    // A line's letters: runs of columns with ink in the band, each with its
    // first and last column and its ink's first and last cell row.
    struct Letter {
        int x0, x1, y0, y1;
    };
    std::vector<Letter> letters(const std::pair<int, int>& band) const {
        std::vector<Letter> out;
        const int r0 = band.first / 2, r1 = (band.second + 1) / 2;
        std::optional<Letter> cur;
        for (int x = 0; x <= w_; ++x) {
            int top = -1, bottom = -1;
            if (x < w_)
                for (int y = r0; y < r1; ++y)
                    if (cellInk(x, y)) {
                        if (top < 0)
                            top = y;
                        bottom = y;
                    }
            if (top >= 0) {
                if (!cur)
                    cur = Letter{x, x, top, bottom};
                cur->x1 = x;
                cur->y0 = std::min(cur->y0, top);
                cur->y1 = std::max(cur->y1, bottom);
            } else if (cur) {
                out.push_back(*cur);
                cur.reset();
            }
        }
        return out;
    }

    // Rung targets for `n` positions down the ramp's ladder, light first,
    // never below rung 4.
    static std::vector<int> gradientTargets(const Ramp& ramp, int n) {
        const int top = ramp.topRung();
        const int avail = std::max(1, top - kRungsPerStep + 1);
        std::vector<int> t;
        for (int i = 0; i < n; ++i) {
            if (n <= avail)
                t.push_back(top - (avail - n) / 2 - i);
            else
                t.push_back(top - static_cast<int>(static_cast<long long>(i) * avail / n));
        }
        return t;
    }

    // The solid ramp colour nearest a position given in quarter rungs.
    static std::uint8_t nearestSolid(const Ramp& ramp, int quarters, bool lighterOnTie) {
        const int per = kRungsPerStep * 4;
        int i = quarters / per;
        const int rem = quarters % per;
        if (rem * 2 > per || (rem * 2 == per && lighterOnTie))
            ++i;
        i = std::clamp(i, std::min<int>(1, int(ramp.colours.size()) - 1), int(ramp.colours.size()) - 1);
        return ramp.colours[static_cast<std::size_t>(i)];
    }

    // Each source cell's colours mapped onto the ramp by rank (#184): black
    // stays the ramp's darkest (black on every named ramp), as ascii.py's
    // recolour keeps it; the other colours the source cells use, dark to
    // light, take consecutive ramp steps ending at the top step a Fill
    // reaches (the ramp's top with iCE on, where walkRungs stops with it
    // off), so colours a rank apart are ramp neighbours. Ranks below the
    // ramp's first step past black all take that step. A cell of two
    // colours keeps them apart and in order: the steps move down, or stop
    // being consecutive, where the top would merge them (rankSteps), and a
    // cell no choice of steps keeps apart and in order is moved (apart).
    void recolourSources(Layer& l, const Ramp& ramp) const {
        std::set<std::uint8_t> used;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (source(x, y)) {
                    const Cell& c = sourceCell(x, y);
                    used.insert(c.fg.paletteIndex());
                    used.insert(c.bg.paletteIndex());
                }
        used.erase(Black);
        std::vector<std::uint8_t> ranked(used.begin(), used.end());
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](std::uint8_t a, std::uint8_t b) { return luma(a) < luma(b); });
        std::map<std::uint8_t, int> rank{{Black, -1}};
        for (std::size_t i = 0; i < ranked.size(); ++i)
            rank[ranked[i]] = static_cast<int>(i);
        RankPairs pairs;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (source(x, y)) {
                    const Cell& c = sourceCell(x, y);
                    if (c.fg.paletteIndex() != c.bg.paletteIndex())
                        ++pairs[{rank[c.fg.paletteIndex()], rank[c.bg.paletteIndex()]}];
                }
        const int top = walkRungs(ramp, {ramp.topRung()}, ice_).front() / kRungsPerStep;
        const std::vector<std::size_t> steps = rankSteps(ramp, top, int(ranked.size()), pairs, ice_);
        const auto stepOf = [&](const Color& c) {
            const int r = rank[c.paletteIndex()];
            return r < 0 ? std::size_t{0} : steps[static_cast<std::size_t>(r)];
        };
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (source(x, y)) {
                    Cell c = sourceCell(x, y);
                    c.transparent = false;
                    std::size_t fi = stepOf(c.fg);
                    std::size_t bi = backStep(ramp, stepOf(c.bg), ice_);
                    const int rf = rank[c.fg.paletteIndex()], rb = rank[c.bg.paletteIndex()];
                    if (rf != rb && (rf > rb ? fi <= bi : fi >= bi))
                        apart(ramp, rf, rb, fi, bi);
                    c.fg = Color::indexed(ramp.colours[fi]);
                    c.bg = Color::indexed(ramp.colours[bi] & (ice_ ? 15 : 7));
                    l.cells.at(x, y) = c;
                }
    }

    // A source cell's steps, foreground `fi` over background `bi`, moved a
    // step apart in its colours' order (ranks `rf` over `rb`, -1 for black):
    // the foreground to the step past the background, up for a lighter one
    // and down for a darker; where it is black or the background is at the
    // ramp's end, the background to the nearest step it can show past the
    // foreground the other way. Black never moves. Left as it is where
    // neither can move.
    void apart(const Ramp& ramp, int rf, int rb, std::size_t& fi, std::size_t& bi) const {
        const bool up = rf > rb;
        const std::size_t n = ramp.colours.size();
        if (up && bi + 1 < n) {
            fi = bi + 1;
            return;
        }
        if (!up && rf >= 0 && bi > 0) {
            fi = bi - 1;
            return;
        }
        if (rb < 0)
            return;
        const auto shows = [&](std::size_t j) { return ice_ || ramp.colours[j] < 8; };
        if (up) {
            for (std::size_t j = fi; j-- > 0;)
                if (shows(j)) {
                    bi = j;
                    return;
                }
        } else {
            for (std::size_t j = fi + 1; j < n; ++j)
                if (shows(j)) {
                    bi = j;
                    return;
                }
        }
    }

    // The letters as Fill draws them: the cells layer (source cells, a
    // gradient's full cells) under the pixels layer (ink pixels).
    std::vector<Layer> fillLayers() const {
        Layer cells = effectLayer(LayerKind::Cells, w_, h_);
        Layer pixels = effectLayer(LayerKind::Pixels, w_, h_);
        const FillEffect* f = style_.get<FillEffect>();
        const std::optional<Ramp> ramp = f && f->mode != FillMode::Flat ? findRamp(f->ramp) : std::nullopt;
        if (!f || (f->mode != FillMode::Flat && !ramp)) {
            // as they are
            for (int y = 0; y < h_; ++y)
                for (int x = 0; x < w_; ++x) {
                    if (source(x, y)) {
                        Cell c = sourceCell(x, y);
                        c.transparent = false;
                        cells.cells.at(x, y) = c;
                    } else if (cellInk(x, y) && in_.pixels.width() > 0) {
                        pixels.cells.at(x, y) = in_.pixels.at(x - ox_, y - oy_);
                    }
                }
        } else if (f->mode == FillMode::Flat) {
            for (int y = 0; y < h_; ++y)
                for (int x = 0; x < w_; ++x) {
                    if (source(x, y)) {
                        Cell c = sourceCell(x, y);
                        c.transparent = false;
                        c.fg = Color::indexed(f->colour);
                        cells.cells.at(x, y) = c;
                        continue;
                    }
                    for (int b = 0; b < 2; ++b)
                        if (ink(x, 2 * y + b))
                            setPixel(pixels, x, 2 * y + b, f->colour);
                }
        } else {
            recolourSources(cells, *ramp);
            for (const auto& band : bands_) {
                const std::vector<Letter> ls = letters(band);
                if (ls.empty())
                    continue;
                if (f->mode == FillMode::Rainbow) {
                    const std::size_t n = ramp->colours.size();
                    for (std::size_t i = 0; i < ls.size(); ++i) {
                        const std::uint8_t colour = n > 1 ? ramp->colours[1 + i % (n - 1)] : ramp->colours[0];
                        for (int y = ls[i].y0; y <= ls[i].y1; ++y)
                            for (int x = ls[i].x0; x <= ls[i].x1; ++x)
                                if (!source(x, y))
                                    for (int b = 0; b < 2; ++b)
                                        if (ink(x, 2 * y + b))
                                            setPixel(pixels, x, 2 * y + b, colour);
                    }
                    continue;
                }
                const bool rows = f->mode == FillMode::Rows;
                int lo = std::numeric_limits<int>::max(), hi = -1;
                for (const Letter& l : ls) {
                    lo = std::min(lo, rows ? l.y0 : l.x0);
                    hi = std::max(hi, rows ? l.y1 : l.x1);
                }
                const std::vector<int> rungs = walkRungs(*ramp, gradientTargets(*ramp, hi - lo + 1), ice_);
                const int r0 = band.first / 2, r1 = (band.second + 1) / 2;
                for (int y = r0; y < r1; ++y)
                    for (int x = 0; x < w_; ++x) {
                        if (source(x, y) || !cellInk(x, y))
                            continue;
                        const int rung = rungs[static_cast<std::size_t>((rows ? y : x) - lo)];
                        const bool top = ink(x, 2 * y), bottom = ink(x, 2 * y + 1);
                        if (top && bottom) {
                            cells.cells.at(x, y) = rungAt(*ramp, rung);
                            continue;
                        }
                        // a fringe: the solid nearest its pixel, in quarter
                        // rungs (a row gradient's half a rung a pixel)
                        const int at = rung * 4 + (rows ? (top ? 2 : -2) : 0);
                        setPixel(pixels, x, 2 * y + (top ? 0 : 1), nearestSolid(*ramp, at, rows ? top : true));
                    }
            }
        }
        std::vector<Layer> out;
        out.push_back(std::move(cells));
        out.push_back(std::move(pixels));
        return out;
    }

    // The letters so far (`layers`) composited over black, `pad` columns
    // and `pad / 2` rows round them.
    Canvas lettersOnBlack(const std::vector<Layer>& layers, int padX, int padY) const {
        LayerStack s(w_ + 2 * padX, h_ + 2 * padY);
        for (const Layer& l : layers) {
            Layer m = l;
            m.props.x = padX;
            m.props.y = padY;
            s.add(std::move(m));
        }
        return composite(s, CompositeOptions{ice_, Cell{}});
    }

    std::optional<Layer> shadeLayer(const ShadeEffect& s, const std::vector<Layer>& below) const {
        const std::optional<Ramp> ramp = findRamp(s.ramp);
        if (!ramp)
            return std::nullopt;
        const Canvas sofar = lettersOnBlack(below, 0, 0);
        Layer l = effectLayer(LayerKind::Cells, w_, h_);
        const auto [lx, ly] = step(s.light);
        for (const auto& band : bands_)
            for (const Letter& letter : letters(band))
                for (int y = letter.y0; y <= letter.y1; ++y)
                    for (int x = letter.x0; x <= letter.x1; ++x) {
                        if (!fullInk(x, y))
                            continue;
                        const Cell& c = sofar.at(x, y);
                        const std::optional<int> rung = rungOf(*ramp, c);
                        if (!rung || *rung <= kRungsPerStep)
                            continue;
                        // cells from the side the light comes from
                        const int kx = lx < 0 ? x - letter.x0 : lx > 0 ? letter.x1 - x : 0;
                        const int ky = ly < 0 ? y - letter.y0 : ly > 0 ? letter.y1 - y : 0;
                        const int k = lx != 0 && ly != 0 ? (kx + ky) / 2 : kx + ky;
                        const int steps = std::min(k, *rung - kRungsPerStep);
                        if (steps <= 0)
                            continue;
                        if (const auto stepped = stepRung(*ramp, c, -steps, ice_))
                            l.cells.at(x, y) = *stepped;
                    }
        return l;
    }

    std::optional<Layer> bevelLayer(const BevelEffect& b, const std::vector<Layer>& below) const {
        const std::optional<Ramp> ramp = findRamp(b.ramp);
        if (!ramp)
            return std::nullopt;
        // the rim light reads the shape's edges: the letters alone on black,
        // far enough from the canvas edges that they light as further in
        constexpr int padX = 4, padY = 2;
        const Canvas canvas = lettersOnBlack(below, padX, padY);
        const RimLightBrush brush(*ramp, b.light);
        StrokeOptions options;
        options.ice = ice_;
        const Stroke stroke(canvas, brush, BrushSettings{}, options);
        Layer l = effectLayer(LayerKind::Cells, w_, h_);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (fullInk(x, y))
                    if (const auto lit = brush.paintCell(stroke, x + padX, y + padY, 1.0))
                        l.cells.at(x, y) = *lit;
        return l;
    }

    const StyleInput& in_;
    const TextStyle& style_;
    bool ice_;
    int ox_, oy_;
    int w_ = 0, h_ = 0;
    std::vector<std::uint8_t> ink_, grown_, src_;
    std::vector<std::pair<int, int>> bands_;
};

} // namespace

Canvas drawStyle(const StyleInput& in, const TextStyle& style, bool ice, StyleMargins margins) {
    return Styler(in, style, ice, margins).draw();
}

StyleInput styleInputOf(const Layer& layer) {
    const int w = layer.width(), h = layer.height();
    StyleInput in;
    in.mask = InkMask(w, 2 * h);
    in.pixels = makeLayer(LayerKind::Pixels, w, h).cells;
    in.source = in.pixels;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Cell& c = layer.cells.at(x, y);
            bool any = false;
            if (layer.props.kind == LayerKind::Pixels) {
                const LayerPixels p = layerPixels(c);
                if (p.top)
                    in.mask.set(x, 2 * y);
                if (p.bottom)
                    in.mask.set(x, 2 * y + 1);
                any = p.top || p.bottom;
                if (any)
                    in.pixels.at(x, y) = c;
            } else if (!isBlank(c)) {
                in.mask.set(x, 2 * y);
                in.mask.set(x, 2 * y + 1);
                Cell s = c;
                s.transparent = false;
                in.source.at(x, y) = s;
                any = true;
            }
            if (any) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    if (x1 >= 0)
        in.box = CellRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    in.bands = {{0, 2 * h}};
    return in;
}

bool styledAtComposite(const LayerProperties& props) {
    return props.style && !props.text &&
           std::any_of(props.style->effects.begin(), props.style->effects.end(), [](const Effect& e) { return e.enabled; });
}

Canvas styledCells(const Layer& layer, bool ice) {
    if (!layer.props.style)
        return layer.cells;
    return drawStyle(styleInputOf(layer), *layer.props.style, ice);
}

} // namespace bw
