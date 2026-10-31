// The soft brush on canvases larger than its windows: round-4 repros, dab
// order, lint findings past the windows, and the property tests at sizes up
// to 41 (PLAN §10, "Soft").

#include "blockwright/ansi.h"
#include "portable_random.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/lint.h"
#include "blockwright/palette.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace bw;
using Kind = lint::LadderFinding::Kind;

namespace {

Ramp steel() { return *findRamp("steel"); }  // k K w W

Canvas filled(int w, int h, const Cell& c) {
    Canvas canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.at(x, y) = c;
    return canvas;
}

// A canvas of rungs from a function of the cell, clamped to the ramp.
template <class F> Canvas field(const Ramp& r, int w, int h, F rung) {
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = rungCell(r, std::clamp(int(rung(x, y)), 0, r.topRung()));
    return c;
}

std::shared_ptr<const Mask> shared(Mask m) { return std::make_shared<const Mask>(std::move(m)); }

Mask disc(int w, int h, double cx, double cy, double across) {
    Mask m(w, h);
    const double r = across / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx, dy = (y - cy) * 2;
            if (dx * dx + dy * dy <= r * r)
                m.set(x, y);
        }
    return m;
}

std::string rungMap(const Canvas& c, const Ramp& r) {
    std::ostringstream out;
    for (int y = 0; y < c.height(); ++y) {
        for (int x = 0; x < c.width(); ++x) {
            const auto g = rungOf(r, c.at(x, y));
            out << (!g ? '.' : *g < 10 ? char('0' + *g) : char('a' + *g - 10));
        }
        out << '\n';
    }
    return out.str();
}

std::vector<int> rungs(const Canvas& c, const Ramp& r) {
    std::vector<int> v;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            v.push_back(rungOf(r, c.at(x, y)).value_or(-1));
    return v;
}

// A finding as the lint names it over the whole canvas: its kind, its cell,
// its other end (a skip's other cell, a wide rung's last) and its message.
using FindingKey = std::tuple<int, int, int, int, int, std::string>;

std::set<FindingKey> findingKeys(const Canvas& c, const Ramp& r) {
    std::set<FindingKey> out;
    for (const auto& f : lint::checkLadder(c, r).findings)
        if (f.kind != Kind::OneGlyph && f.kind != Kind::Noise)
            out.insert({int(f.kind), f.x, f.y, f.nx, f.ny, f.message});
    return out;
}

// What `after` adds to `before` by the lint's own rule (the maintainer's
// ruling on PR #59): new by kind and place, a wide rung unless `before` had
// one the same way, on the same line, of the same rung, sharing a cell with
// it; one glyph and noise as the whole canvas reads.
std::vector<std::string> added(const Canvas& before, const Canvas& after, const Ramp& r) {
    std::vector<std::pair<int, int>> changed;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (!(before.at(x, y) == after.at(x, y)))
                changed.emplace_back(x, y);
    std::vector<std::string> out;
    for (const auto& f : lint::addedFindingsOnce(before, after, r, changed))
        out.push_back(f.message + " (to " + std::to_string(f.nx) + "," + std::to_string(f.ny) + ")");
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& m : v)
        s += m + "\n";
    return s;
}

const std::array<const char*, 5> kRamps{"steel", "fire", "ice", "toxic", "sunset"};

// The soft brush, checked after every dab: the canvas so far adds no lint
// finding to the start (added(), over the whole canvas), every cell has
// moved only the brush's way from where the stroke began and is no further
// back than any earlier dab of the stroke left it, and nothing it may not
// paint has changed. The first failure is kept.
class CheckedSoft : public Brush {
public:
    CheckedSoft(const SoftShadeBrush& brush, const Canvas& start, ShadeDirection dir)
        : brush_(brush), start_(start), dir_(dir) {}
    std::string_view name() const override { return brush_.name(); }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override {
        brush_.dab(stroke, d, size, strength);
        ++dabs;
        if (!failure.empty())
            return;
        const Ramp& r = brush_.ramp();
        const Canvas now = stroke.preview();
        std::ostringstream o;
        for (const auto& m : added(start_, now, r))
            o << "added: " << m << "\n";
        const int sign = dir_ == ShadeDirection::Lighten ? 1 : -1;
        if (best_.empty())
            best_.assign(size_t(now.width()) * size_t(now.height()), -1000);
        for (int y = 0; y < now.height(); ++y)
            for (int x = 0; x < now.width(); ++x) {
                // never back from where an earlier dab of the stroke had it
                if (const auto g = rungOf(r, stroke.options().ice ? now.at(x, y) : iceCell(now.at(x, y), false))) {
                    int& best = best_[size_t(y) * size_t(now.width()) + size_t(x)];
                    if (sign * *g < best)
                        o << "cell " << x << "," << y << " moved back from where an earlier dab left it\n";
                    best = std::max(best, sign * *g);
                }
                if (now.at(x, y) == start_.at(x, y))
                    continue;
                if (!stroke.paintable(x, y))
                    o << "cell " << x << "," << y << " changed outside what the stroke may paint\n";
                const auto was = rungOf(r, stroke.options().ice ? start_.at(x, y) : iceCell(start_.at(x, y), false));
                const auto is = rungOf(r, stroke.options().ice ? now.at(x, y) : iceCell(now.at(x, y), false));
                if (!was || !is || sign * (*is - *was) < 0)
                    o << "cell " << x << "," << y << " moved against the brush\n";
            }
        if (!o.str().empty())
            failure = "dab " + std::to_string(dabs) + " at " + std::to_string(d.x) + "," + std::to_string(d.y) +
                      " size " + std::to_string(size) + "\n" + rungMap(now, r) + o.str();
    }
    mutable int dabs = 0;
    mutable std::string failure;

private:
    const SoftShadeBrush& brush_;
    const Canvas& start_;
    ShadeDirection dir_;
    mutable std::vector<int> best_;
};

// A start for the big-canvas strokes: a flat field, a gradient one of
// eight ways at one to six cells a rung, or long plateaus (runs of one rung
// far longer than the brush's windows) stepping a rung or two.
enum class Start { Flat, Gradient, Plateaus };

Canvas startCanvas(Start kind, const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int top = r.topRung();
    switch (kind) {
    case Start::Flat:
        return filled(w, h, rungCell(r, pick(1, top - 1)));
    case Start::Gradient: {
        constexpr int ways[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        const auto& way = ways[pick(0, 7)];
        const int every = pick(1, 6), from = pick(1, top / 2);
        return field(r, w, h, [&](int x, int y) {
            const int along = way[0] * (way[0] < 0 ? x - (w - 1) : x) + way[1] * (way[1] < 0 ? y - (h - 1) : y);
            return std::clamp(from + std::abs(along) / every, 1, top);
        });
    }
    case Start::Plateaus: {
        const bool across = pick(0, 1);
        const int every = pick(30, 90), step = pick(1, 2), from = pick(1, top / 2);
        return field(r, w, h, [&](int x, int y) { return std::clamp(from + step * ((across ? x : y) / every), 1, top); });
    }
    }
    return Canvas(w, h);
}

} // namespace

TEST_CASE("soft brush repros from round 4: Codex round 4, finding 4: dab order", "[shading][soft]") {
    // 1 1 1 3 lightened a rung at (2,0) and (3,0), one dab each. Round 4
    // made both orders paint the same; round 6 (Claude N2) made a stroke
    // never take back paint an earlier dab put down, and the two conflict:
    // (2,0) first puts 2 at (2,0), and (3,0) can then only go to 3 (a 4
    // there would skip from 2); (3,0) first puts 4 at (3,0), and (2,0) can
    // then go nowhere (a 2 there would skip to the 4). Neither stroke adds a
    // finding, and each keeps what its first dab painted.
    const Ramp r = steel();
    const Canvas start = field(r, 4, 1, [](int x, int) { return x == 3 ? 3 : 1; });
    BrushSettings s;
    s.size = 1;
    s.tip = Tip::Square;
    s.strength = 0.25;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 1.0);
    auto paint = [&](std::vector<Dab> dabs) {
        Canvas c = start;
        paintStroke(start, brush, s, dabs).apply(c);
        return c;
    };
    const Canvas ab = paint({{2, 0}, {3, 0}}), ba = paint({{3, 0}, {2, 0}});
    INFO("(2,0) then (3,0): " << rungMap(ab, r) << "(3,0) then (2,0): " << rungMap(ba, r));
    CHECK(rungMap(ab, r) == "1123\n");
    CHECK(rungMap(ba, r) == "1114\n");
    CHECK(added(start, ab, r).empty());
    CHECK(added(start, ba, r).empty());
}

TEST_CASE("soft brush: dabs within four cells of each other, in one order and in reverse", "[shading][soft]") {
    // Unjoined dabs at one size and strength, played in one order and then
    // in reverse. Round 5 had dabs within four cells of each other paint
    // the same either way, by letting a later dab take back what an earlier
    // one painted. Since round 6 no dab takes paint back (Claude N2), so the
    // order can matter wherever an earlier dab's paint blocks a later one
    // (1 1 1 3 above). Both orders add no finding and take no paint back
    // (CheckedSoft), and most strokes still come out the same: 48 of 300
    // differ since round 9 (Codex round 8, P2) solves only a dab that laid
    // nothing a second time (43 in round 8, which solved nearly every dab
    // twice; 49 under round 7's guard, 46 under round 6's), 25 of them with
    // the dabs together.
    std::mt19937 rng(2026102401);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    int differ = 0, close = 0, painted = 0;
    std::string first, wrong;
    for (int k = 0; k < 300; ++k) {
        const bool big = k % 5 == 0, together = k % 2 == 1;
        const int w = big ? 160 : 40, h = big ? 60 : 20;
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const Canvas start = startCanvas(Start(pick(0, 2)), r, w, h, rng);
        StrokeOptions o;
        o.ice = pick(0, 1);
        if (pick(0, 2) == 0)
            o.selection = shared(disc(w, h, pick(0, w - 1), pick(0, h - 1), pick(6, 40)));
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        const SoftShadeBrush brush(r, dir, hard);
        BrushSettings s;
        s.size = pick(1, big ? 41 : 9);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        std::vector<Dab> dabs;
        const int spread = together ? 2 : big ? 30 : 8;
        const int cx = pick(0, w - 1), cy = pick(0, h - 1);
        for (int i = 0, n = pick(2, 5); i < n; ++i)
            dabs.push_back({cx + pick(-spread, spread), cy + pick(-spread / 2, spread / 2)});
        auto play = [&](bool reversed) {
            CheckedSoft checked(brush, start, dir);
            Stroke st(start, checked, s, o);
            for (size_t i = 0; i < dabs.size(); ++i)
                st.addDab(dabs[reversed ? dabs.size() - 1 - i : i], false);
            if (wrong.empty())
                wrong = checked.failure;
            return st.preview();
        };
        const Canvas a = play(false), b = play(true);
        painted += !(a == start);
        if (a == b)
            continue;
        close += together;
        if (!differ++ || (together && close == 1)) {
            std::ostringstream out;
            out << "stroke " << k << ": " << r.name << " ice " << o.ice << " dir " << int(dir) << " hardness "
                << hard << " size " << s.size << " tip " << int(s.tip) << " strength " << s.strength
                << "\n";
            for (const Dab& d : dabs)
                out << "(" << d.x << "," << d.y << ") ";
            out << "\nforward\n" << rungMap(a, r) << "reversed\n" << rungMap(b, r);
            first = out.str();
        }
    }
    INFO(differ << " of 300 strokes differ, " << close << " of them with the dabs together; " << painted
                << " painted\n" << first << wrong);
    CHECK(wrong.empty());
    CHECK(differ * 5 < 300);
    CHECK(painted > 150);
}

TEST_CASE("soft brush: Codex round 5, finding 1: a dab inside the last one's footprint", "[shading][soft]") {
    // 6 6 9 9 9 9 9 lightened a rung by a size 6 hard square at (4,0) and
    // (5,0). (4,0) first reaches (1,0), which can't move without a finding,
    // so the dab gives way; (5,0) then wants nothing deeper than the stroke
    // already does and changes nothing. (5,0) first leaves (1,0) out, and
    // round 6 painted 6 6 10 10 10 10 10; since round 7 the guard reads one
    // glyph and noise too, and that row shades with one glyph (6 and 10 are
    // both the half shade) where the start used two, so it gives way as
    // well. Neither order adds a finding.
    const Ramp r = steel();
    const Canvas start = field(r, 7, 1, [](int x, int) { return x < 2 ? 6 : 9; });
    BrushSettings s;
    s.size = 6;
    s.tip = Tip::Square;
    s.strength = 0.25;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 1.0);
    auto play = [&](int a, int b) {
        Stroke st(start, brush, s);
        st.addDab({a, 0}, false);
        st.addDab({b, 0}, false);
        return st.preview();
    };
    const Canvas ab = play(4, 5), ba = play(5, 4);
    INFO("(4,0) then (5,0): " << rungMap(ab, r) << "(5,0) then (4,0): " << rungMap(ba, r));
    CHECK(rungMap(ab, r) == "6699999\n");
    CHECK(rungMap(ba, r) == "6699999\n");
    CHECK(added(start, ab, r).empty());
    CHECK(added(start, ba, r).empty());
}

TEST_CASE("soft brush: dabs of one size within four cells of each other, in a random order", "[shading][soft]") {
    // Codex round 5, finding 1. Every dab's footprint lies within four
    // cells of every other's; strengths differ (pressure) and the order is
    // a random shuffle. Round 5 settled every cell from the stroke's wants
    // alone, taking back what earlier dabs painted, so any order painted
    // the same. Since round 6 no dab takes paint back (Claude N2), and the
    // order can matter: 51 of 300 differ since round 9 solves only a dab
    // that laid nothing a second time (43 in round 8, 51 under round 7's
    // guard, 52 under round 6's). Both orders add no finding and take no paint back
    // (CheckedSoft).
    std::mt19937 rng(2026102501);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    int differ = 0, painted = 0;
    std::string first, wrong;
    for (int k = 0; k < 300; ++k) {
        const bool big = k % 4 == 0;
        const int w = big ? 160 : 40, h = big ? 60 : 20;
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const Canvas start = startCanvas(Start(pick(0, 2)), r, w, h, rng);
        StrokeOptions o;
        o.ice = pick(0, 1);
        if (pick(0, 2) == 0)
            o.selection = shared(disc(w, h, pick(0, w - 1), pick(0, h - 1), pick(6, 40)));
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        const SoftShadeBrush brush(r, dir, hard);
        BrushSettings s;
        s.size = pick(1, big ? 41 : 11);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        s.pressure = PressureTo::Strength;
        std::vector<Dab> dabs;
        const int cx = pick(0, w - 1), cy = pick(0, h - 1);
        for (int i = 0, n = pick(2, 5); i < n; ++i)
            dabs.push_back({cx + pick(-2, 2), cy + pick(-2, 2), 0.25 * pick(1, 4)});
        auto play = [&](const std::vector<Dab>& order) {
            CheckedSoft checked(brush, start, dir);
            Stroke st(start, checked, s, o);
            for (const Dab& d : order)
                st.addDab(d, false);
            if (wrong.empty())
                wrong = checked.failure;
            return st.preview();
        };
        std::vector<Dab> shuffled = dabs;
        bwtest::shuffle(shuffled.begin(), shuffled.end(), rng);
        const Canvas a = play(dabs), b = play(shuffled);
        painted += !(a == start);
        if (a == b)
            continue;
        if (!differ++) {
            std::ostringstream out;
            out << "stroke " << k << ": " << r.name << " ice " << o.ice << " dir " << int(dir) << " hardness "
                << hard << " size " << s.size << " tip " << int(s.tip) << " strength " << s.strength << "\n";
            for (const auto* v : {&dabs, &shuffled}) {
                for (const Dab& d : *v)
                    out << "(" << d.x << "," << d.y << "," << d.pressure << ") ";
                out << "\n";
            }
            out << "in order\n" << rungMap(a, r) << "shuffled\n" << rungMap(b, r);
            first = out.str();
        }
    }
    INFO(differ << " of 300 strokes differ; " << painted << " painted\n" << first << wrong);
    CHECK(wrong.empty());
    CHECK(differ * 5 < 300);
    CHECK(painted > 150);
}

namespace {

// The cell a mirror maps a cell to.
std::pair<int, int> mirrored(SymmetryMode m, int x, int y, int w, int h) {
    const bool mx = m == SymmetryMode::MirrorX || m == SymmetryMode::MirrorXY;
    const bool my = m == SymmetryMode::MirrorY || m == SymmetryMode::MirrorXY;
    return {mx ? w - 1 - x : x, my ? h - 1 - y : y};
}

// The cells of the canvas the mirror moves that differ from their image.
std::string asymmetry(const Canvas& c, SymmetryMode m) {
    std::ostringstream out;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const auto [mx, my] = mirrored(m, x, y, c.width(), c.height());
            if (!(c.at(x, y) == c.at(mx, my)))
                out << "cell " << x << "," << y << " isn't the image of " << mx << "," << my << "\n";
        }
    return out.str();
}

// The soft brush under a mirror, checked after every stamp: each leaves the
// stroke symmetric. The first stamp that doesn't is kept.
class MirrorChecked : public Brush {
public:
    MirrorChecked(const SoftShadeBrush& brush, SymmetryMode mode) : brush_(brush), mode_(mode) {}
    std::string_view name() const override { return brush_.name(); }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override {
        brush_.dab(stroke, d, size, strength);
        ++stamps;
        if (!failure.empty())
            return;
        if (const std::string wrong = asymmetry(stroke.preview(), mode_); !wrong.empty())
            failure = "stamp " + std::to_string(stamps) + " at " + std::to_string(d.x) + "," + std::to_string(d.y) +
                      " image " + std::to_string(d.image) + " flips " + std::to_string(d.flipX) +
                      std::to_string(d.flipY) + " size " + std::to_string(size) + "\n" +
                      rungMap(stroke.preview(), brush_.ramp()) + wrong;
    }
    mutable int stamps = 0;
    mutable std::string failure;

private:
    const SoftShadeBrush& brush_;
    SymmetryMode mode_;
};

} // namespace

TEST_CASE("soft brush: Claude round 5, P3-D: a mirror stroke on a symmetric row stays symmetric",
          "[shading][soft][symmetry]") {
    const Ramp r = steel();
    const std::string row = "3552345115432553";
    const Canvas start = field(r, 16, 1, [&](int x, int) { return row[size_t(x)] - '0'; });
    REQUIRE(asymmetry(start, SymmetryMode::MirrorX).empty());
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    s.strength = 0.5;
    StrokeOptions o;
    o.symmetry = {SymmetryMode::MirrorX};
    Canvas c = start;
    paintStroke(start, SoftShadeBrush(r, ShadeDirection::Lighten, 0.0), s, {{0, 0}}, o).apply(c);
    INFO(rungMap(c, r));
    CHECK(asymmetry(c, SymmetryMode::MirrorX).empty());
    CHECK(added(start, c, r).empty());
}

TEST_CASE("soft brush: mirror strokes on symmetric canvases stay symmetric", "[shading][soft][symmetry]") {
    // Mirror X, Y and both, about a cell (odd sides) and between two (even
    // sides), with dabs joined and not.
    std::mt19937 rng(2026102502);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    constexpr SymmetryMode kModes[3] = {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY};
    int failed = 0, painted = 0;
    std::string first;
    for (int k = 0; k < 300; ++k) {
        const SymmetryMode mode = kModes[k % 3];
        const int w = pick(9, 48), h = pick(5, 24);
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const Canvas half = startCanvas(Start(pick(0, 2)), r, w, h, rng);
        Canvas start(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const auto [mx, my] = mirrored(mode, x, y, w, h);
                start.at(x, y) = half.at(std::min(x, mx), std::min(y, my));
            }
        StrokeOptions o;
        o.ice = pick(0, 1);
        o.symmetry = {mode};
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        BrushSettings s;
        s.size = pick(1, 13);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        s.pressure = std::array{PressureTo::Nothing, PressureTo::Strength, PressureTo::Size}[size_t(pick(0, 2))];
        const bool join = pick(0, 1);
        const SoftShadeBrush brush(r, dir, hard);
        const MirrorChecked checked(brush, mode);
        Stroke st(start, checked, s, o);
        std::vector<Dab> dabs;
        for (int i = 0, n = pick(1, 4); i < n; ++i) {
            dabs.push_back({pick(-2, w + 1), pick(-2, h + 1), 0.05 * pick(1, 20)});
            st.addDab(dabs.back(), join);
        }
        const Canvas c = st.preview();
        painted += !(c == start);
        const std::string wrong = checked.failure + joined(added(start, c, r));
        if (wrong.empty())
            continue;
        if (!failed++) {
            std::ostringstream out;
            out << "stroke " << k << ": mode " << int(mode) << " " << w << "x" << h << " " << r.name << " ice "
                << o.ice << " dir " << int(dir) << " hardness " << hard << " size " << s.size << " tip "
                << int(s.tip) << " strength " << s.strength << " pressure " << int(s.pressure) << " join " << join
                << "\n";
            for (const Dab& d : dabs)
                out << "(" << d.x << "," << d.y << "," << d.pressure << ") ";
            out << "\nstart\n"
                << rungMap(start, r) << "after\n" << rungMap(c, r) << wrong;
            first = out.str();
        }
    }
    INFO(failed << " of 300 strokes failed; " << painted << " painted\n" << first);
    CHECK(failed == 0);
    CHECK(painted > 150);
}

// Codex round 4, finding 1: the lint reads each row and column through the
// cells a dab changes to its ends, so a run of a rung or of levels the
// change reaches is judged wherever it ends.
TEST_CASE("soft brush repros from round 4: Codex round 4, finding 1: a finding far along the row gives way",
          "[shading][soft]") {
    // Rung 3 then rung 5: a dab at (11,4) darkening cells a rung makes
    // 3 2 3 5 along its rows, a skip at the edge and a step of rung 3 up to
    // it, however far off the edge is.
    const Ramp r = *findRamp("fire");
    BrushSettings s;
    s.size = 2;
    s.tip = Tip::Square;
    s.strength = 0.25;
    for (const int edge : {50, 100, 150, 199}) {
        const Canvas start = field(r, 200, 8, [edge](int x, int) { return x < edge ? 3 : 5; });
        REQUIRE(findingKeys(start, r).empty());
        Canvas c = start;
        paintStroke(start, SoftShadeBrush(r, ShadeDirection::Darken, 1.0), s, {{11, 4}}).apply(c);
        const auto more = added(start, c, r);
        INFO("edge at " << edge << "\n" << rungMap(c, r) << joined(more));
        CHECK(more.empty());
    }
}

// Codex round 5, finding 4: the form brush judges its paints along every
// row and column through the cells they change.
TEST_CASE("form brush: Codex round 5, finding 4: a finding far along the row takes the paints back",
          "[shading][form]") {
    // The form's shadow darkens (10..11,3..4) a rung: 3 2 3 5 along the
    // rows, as above.
    const Ramp r = *findRamp("fire");
    for (const int edge : {50, 100, 199}) {
        const Canvas start = field(r, 200, 8, [edge](int x, int) { return x < edge ? 3 : 5; });
        REQUIRE(findingKeys(start, r).empty());
        FormOptions fo;
        fo.shape = FormShape::Plane;
        fo.rim = false;
        fo.specular = false;
        StrokeOptions o;
        o.selection = shared(Mask::rect(200, 8, 0, 0, 199, 7));
        o.ice = true;
        BrushSettings s;
        s.size = 2;
        s.tip = Tip::Square;
        s.strength = 1.0 / r.topRung();
        s.lightDirection = Compass::E;
        Canvas c = start;
        paintStroke(start, FormShadeBrush(r, fo), s, {{11, 4}}, o).apply(c);
        const auto more = added(start, c, r);
        INFO("edge at " << edge << "\n" << rungMap(c, r) << joined(more));
        CHECK(more.empty());
    }
}

TEST_CASE("soft brush repros from round 4: Claude round 4, P3-1: a gradient dab adds no finding, changed or not",
          "[shading][soft]") {
    // A rung every two rows: a size-5 round dab at (15,10) leaves everything
    // as it was, though (15,11) alone could move a rung cleanly. The brush
    // gives way cell by cell from the whole footprint and doesn't search
    // for the largest clean part of it: what it leaves is clean, but it may
    // be less than could be.
    const Ramp r = steel();
    const Canvas start = field(r, 30, 30, [](int, int y) { return std::min(12, 1 + y / 2); });
    REQUIRE(findingKeys(start, r).empty());
    BrushSettings s;
    s.size = 5;
    s.tip = Tip::Round;
    s.strength = 1.0;
    const CellEdit e = paintStroke(start, SoftShadeBrush(r, ShadeDirection::Lighten, 0.0), s, {{15, 10}});
    Canvas c = start;
    e.apply(c);
    const auto more = added(start, c, r);
    INFO(rungMap(c, r) << joined(more));
    CHECK(more.empty());
}

namespace {

struct Tally {
    int strokes = 0, dabs = 0, painted = 0, failed = 0;
    std::string first;
};

// Random soft strokes on a 160x60 canvas, larger than the brush's windows
// round a dab, checked after every dab by CheckedSoft: sizes 1-41, both
// tips, every ramp, iCE on and off, both ways, three hardnesses, pressure to
// nothing, strength and size, under no selection, a rectangle or a disc.
Tally bigStrokes(Start kind, int strokes, unsigned seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int w = 160, h = 60;
    Tally t;
    for (int k = 0; k < strokes; ++k) {
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const Canvas start = startCanvas(kind, r, w, h, rng);
        StrokeOptions o;
        o.ice = pick(0, 1);
        switch (pick(0, 2)) {
        case 1: {
            const int x0 = pick(0, w - 2), y0 = pick(0, h - 2);
            o.selection = shared(Mask::rect(w, h, x0, y0, pick(x0 + 1, w - 1), pick(y0 + 1, h - 1)));
            break;
        }
        case 2:
            o.selection = shared(disc(w, h, pick(0, w - 1), pick(0, h - 1), pick(10, 120)));
            break;
        default:
            break;
        }
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        BrushSettings s;
        s.size = pick(1, 41);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        s.pressure = std::array{PressureTo::Nothing, PressureTo::Strength, PressureTo::Size}[size_t(pick(0, 2))];
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        const SoftShadeBrush brush(r, dir, hard);
        std::vector<Dab> dabs;
        for (int i = 0, n = pick(1, 4); i < n; ++i)
            dabs.push_back({pick(-4, w + 3), pick(-4, h + 3), 0.05 * pick(1, 20)});
        CheckedSoft checked(brush, start, dir);
        t.painted += !paintStroke(start, checked, s, dabs, o).changes.empty();
        ++t.strokes;
        t.dabs += checked.dabs;
        if (checked.failure.empty())
            continue;
        if (!t.failed++) {
            std::ostringstream out;
            out << "stroke " << k << ": " << r.name << " ice " << o.ice << " dir " << int(dir) << " hardness "
                << hard << " size " << s.size << " tip " << int(s.tip) << " strength " << s.strength
                << " pressure " << int(s.pressure) << " selection " << bool(o.selection) << "\n";
            for (const Dab& d : dabs)
                out << "(" << d.x << "," << d.y << "," << d.pressure << ") ";
            out << "\nstart\n" << rungMap(start, r) << checked.failure;
            t.first = out.str();
        }
    }
    return t;
}

} // namespace

TEST_CASE("soft strokes on a canvas past the lint window add no finding, dab by dab: flat fields", "[shading][soft]") {
    // Claude round 4, P3-4; Codex round 4, finding 1.
    const Tally t = bigStrokes(Start::Flat, 150, 2026102411);
    INFO(t.failed << " of " << t.strokes << " strokes (" << t.dabs << " dabs, " << t.painted << " painted) failed\n"
                   << t.first);
    CHECK(t.failed == 0);
    CHECK(t.painted * 2 > t.strokes);
}

// Codex round 5, the sweep counts: a dab whose cells can't settle without
// a finding the start didn't have gives way entirely, so no stroke adds
// one (round 4 pinned 2 and 17 of 150 here).
TEST_CASE("soft strokes on a canvas past the lint window add no finding, dab by dab: gradients",
          "[shading][soft]") {
    const Tally t = bigStrokes(Start::Gradient, 150, 2026102412);
    INFO(t.failed << " of " << t.strokes << " strokes (" << t.dabs << " dabs, " << t.painted << " painted) failed\n"
                   << t.first);
    CHECK(t.failed == 0);
    CHECK(t.painted * 4 > t.strokes);
}

TEST_CASE("soft strokes on a canvas past the lint window add no finding, dab by dab: plateaus",
          "[shading][soft]") {
    const Tally t = bigStrokes(Start::Plateaus, 150, 2026102413);
    INFO(t.failed << " of " << t.strokes << " strokes (" << t.dabs << " dabs, " << t.painted << " painted) failed\n"
                   << t.first);
    CHECK(t.failed == 0);
    // a dab that gives way leaves all its cells, so fewer strokes paint
    // than when it kept the clean ones (76 of 150 then, 74 under round
    // 6's guard, 84 under round 7's, 85 since round 8's retry of a dab
    // that laid nothing, and still 85 in rounds 9 and 10)
    CHECK(t.painted * 5 > t.strokes * 2);
}

TEST_CASE("soft brush on a canvas past the lint window: re-shading near the dab equals re-shading the whole stroke",
          "[shading][soft]") {
    std::mt19937 rng(2026102414);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int w = 160, h = 60;
    int differ = 0, painted = 0;
    std::string first;
    for (int k = 0; k < 80; ++k) {
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const Canvas start = startCanvas(Start(pick(0, 2)), r, w, h, rng);
        StrokeOptions o;
        o.ice = pick(0, 1);
        if (pick(0, 2) == 0)
            o.selection = shared(disc(w, h, pick(0, w - 1), pick(0, h - 1), pick(10, 120)));
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        BrushSettings s;
        s.size = pick(1, 41);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        std::vector<Dab> dabs;
        for (int i = 0, n = pick(1, 4); i < n; ++i)
            dabs.push_back({pick(-4, w + 3), pick(-4, h + 3)});
        SoftShadeBrush near(r, dir, hard), whole(r, dir, hard);
        whole.reshadeWhole(true);
        Canvas a = start, b = start;
        paintStroke(start, near, s, dabs, o).apply(a);
        paintStroke(start, whole, s, dabs, o).apply(b);
        painted += !(a == start);
        if (a == b)
            continue;
        if (!differ++) {
            std::ostringstream out;
            out << "stroke " << k << ": " << r.name << " ice " << o.ice << " dir " << int(dir) << " hardness " << hard
                << " size " << s.size << " tip " << int(s.tip) << " strength " << s.strength << "\n";
            for (const Dab& d : dabs)
                out << "(" << d.x << "," << d.y << ") ";
            out << "\nnear\n" << rungMap(a, r) << "whole\n" << rungMap(b, r);
            first = out.str();
        }
    }
    INFO(differ << " of 80 strokes differ; " << painted << " painted\n" << first);
    CHECK(differ == 0);
    CHECK(painted > 40);
}

TEST_CASE("soft brush: Codex round 6, finding 1; Claude round 6, N1: a wide rung that turns from across to down",
          "[shading][soft]") {
    // Rung 9 over (2..5,2..5) with rung 8 before it and rung 10 after it
    // across row 2 and down column 2: row 2's step of 9 is four cells wide,
    // and so is column 2's, but column 2's isn't judged where row 2 reports
    // its cell. Darkening (6,2) to 9 takes row 2's finding away and lays
    // column 2's from the same cell: the same kind, cell and message, the
    // other end moved from (5,2) to (2,5).
    const Ramp r = steel();
    const Canvas start = field(r, 9, 9, [](int x, int y) {
        if (x >= 2 && x <= 5 && y >= 2 && y <= 5)
            return 9;
        if ((x == 1 && y == 2) || (x == 2 && y == 1))
            return 8;
        if ((x == 6 && y == 2) || (x == 2 && y == 6))
            return 10;
        return 0;
    });
    BrushSettings s;
    s.size = 1;
    s.tip = Tip::Square;
    s.strength = 0.25;
    Canvas c = start;
    paintStroke(start, SoftShadeBrush(r, ShadeDirection::Darken, 1.0), s, {{6, 2}}).apply(c);
    // the run down column 2 is a finding the start didn't have: the
    // start's run there is across, and a wide rung is new unless the start
    // had one the same way (the ruling on PR #59)
    Canvas turned = start;
    turned.at(6, 2) = rungCell(r, 9);
    bool down = false;
    for (const auto& f : lint::addedFindingsOnce(start, turned, r, {{6, 2}}))
        down = down || (f.kind == Kind::WideRung && f.x == 2 && f.y == 2 && f.nx == 2 && f.ny == 5);
    REQUIRE(down);
    const auto more = added(start, c, r);
    INFO(rungMap(start, r) << "after\n" << rungMap(c, r) << joined(more));
    CHECK(c == start);
    CHECK(more.empty());
}

// The guard keys a finding as the lint does (the ruling on PR #59): by
// kind and place, not by message. A skip that stays between the same two
// cells is the start's, though the rungs either side (and so the message)
// changed; round 6 keyed on the message and refused the dab.
TEST_CASE("soft brush: a finding at the start's place with another message is the start's", "[shading][soft]") {
    const Ramp r = steel();
    const Canvas start = field(r, 3, 1, [](int x, int) { return 4 + 2 * x; });
    Canvas lighter = start;
    lighter.at(1, 0) = rungCell(r, 7);
    // by message, the skip from (0,0) is new; by place it is the start's
    const auto startFound = lint::checkLadder(start, r).findings;
    const auto nowFound = lint::checkLadder(lighter, r).findings;
    auto skipAt0 = [](const std::vector<lint::LadderFinding>& fs) {
        for (const auto& f : fs)
            if (f.kind == Kind::RungSkip && f.x == 0 && f.y == 0 && f.nx == 1 && f.ny == 0)
                return f.message;
        return std::string();
    };
    REQUIRE(!skipAt0(startFound).empty());
    REQUIRE(!skipAt0(nowFound).empty());
    REQUIRE(skipAt0(startFound) != skipAt0(nowFound));
    REQUIRE(added(start, lighter, r).empty());

    BrushSettings s;
    s.size = 1;
    s.tip = Tip::Square;
    s.strength = 0.25;
    StrokeOptions o;
    o.ice = true;
    Canvas c = start;
    paintStroke(start, SoftShadeBrush(r, ShadeDirection::Lighten, 1.0), s, {{1, 0}}, o).apply(c);
    INFO(rungMap(c, r));
    CHECK(rungOf(r, c.at(1, 0)) == 7);
    CHECK(added(start, c, r).empty());
}

TEST_CASE("soft brush: Claude round 6, N2: going over the same dabs again takes no paint off", "[shading][soft]") {
    // A lighten stroke never leaves a cell darker than it has been at any
    // point in the stroke, and a dab the stroke already has, again at the
    // same strength, changes nothing.
    const Ramp r = steel();
    const std::array<std::string, 2> rows{"66666666464666", "73666366663666"};
    const Canvas start = field(r, 14, 2, [&](int x, int y) { return rows[size_t(y)][size_t(x)] - '0'; });
    BrushSettings s;
    s.size = 1;
    s.tip = Tip::Square;
    s.strength = 0.75;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 1.0);
    Stroke st(start, brush, s);
    st.addDab({8, 0}, false);
    st.addDab({13, 1}, false);
    const Canvas once = st.preview();
    st.addDab({8, 0}, false);
    st.addDab({13, 1}, false);
    const Canvas twice = st.preview();
    INFO("once\n" << rungMap(once, r) << "twice\n" << rungMap(twice, r));
    CHECK(rungOf(r, once.at(8, 0)) == 6);
    CHECK(rungs(twice, r) == rungs(once, r));
}

TEST_CASE("soft brush: Claude round 6, N2: a scrub back and forth paints what one pass does", "[shading][soft]") {
    const Ramp r = steel();
    const Canvas start = field(r, 60, 24, [](int x, int) { return x * 12 / 59; });
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Round;
    s.strength = 0.5;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 0.0);
    auto scrub = [&](int legs) {
        Stroke st(start, brush, s);
        for (int i = 0; i <= legs; ++i)
            st.addDab(i % 2 ? Dab{43, 18} : Dab{12, 7});
        return st.preview();
    };
    const Canvas ab = scrub(1), aba = scrub(2), abab = scrub(3);
    INFO("A to B\n" << rungMap(ab, r) << "A to B to A\n" << rungMap(aba, r) << "A to B to A to B\n" << rungMap(abab, r));
    CHECK(!(ab == start));
    CHECK(rungs(aba, r) == rungs(ab, r));
    CHECK(rungs(abab, r) == rungs(aba, r));
}

TEST_CASE("soft brush: random scrubs and replays take no paint off", "[shading][soft]") {
    // Joined scrubs A to B and back, and unjoined strokes whose dabs are
    // played again at the same or a lower pressure: the second pass
    // changes nothing, and no dab leaves a cell further back than an
    // earlier one had it (CheckedSoft).
    std::mt19937 rng(2026102301);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    int differ = 0, failed = 0, painted = 0;
    std::string first;
    for (int k = 0; k < 120; ++k) {
        const bool replay = k % 2 == 1;
        const int w = k % 6 == 0 ? 160 : 60, h = k % 6 == 0 ? 60 : 24;
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const Canvas start = startCanvas(Start(pick(0, 2)), r, w, h, rng);
        StrokeOptions o;
        o.ice = pick(0, 1);
        if (pick(0, 3) == 0)
            o.selection = shared(disc(w, h, pick(0, w - 1), pick(0, h - 1), pick(10, 60)));
        if (pick(0, 3) == 0)
            o.symmetry = {std::array{SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY}[size_t(pick(0, 2))]};
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        const SoftShadeBrush brush(r, dir, hard);
        BrushSettings s;
        s.size = pick(1, 15);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        s.pressure = replay ? std::array{PressureTo::Strength, PressureTo::Size}[size_t(pick(0, 1))] : PressureTo::Nothing;
        std::vector<Dab> dabs;
        for (int i = 0, n = replay ? pick(2, 6) : 2; i < n; ++i)
            dabs.push_back({pick(0, w - 1), pick(0, h - 1), replay ? 0.25 * pick(1, 4) : 1.0});
        CheckedSoft checked(brush, start, dir);
        Stroke st(start, checked, s, o);
        for (const Dab& d : dabs)
            st.addDab(d, !replay);
        const Canvas once = st.preview();
        if (replay) {
            std::vector<Dab> again = dabs;
            bwtest::shuffle(again.begin(), again.end(), rng);
            for (Dab& d : again)
                d.pressure *= 0.25 * pick(1, 4);
            for (const Dab& d : again)
                st.addDab(d, false);
        } else {
            st.addDab(dabs[0]);
            st.addDab(dabs[1]);
        }
        const Canvas twice = st.preview();
        painted += !(once == start);
        const bool same = once == twice;
        differ += !same;
        failed += !checked.failure.empty();
        if (first.empty() && (!same || !checked.failure.empty())) {
            std::ostringstream out;
            out << "stroke " << k << (replay ? " replay" : " scrub") << ": " << r.name << " ice " << o.ice << " dir "
                << int(dir) << " hardness " << hard << " size " << s.size << " tip " << int(s.tip) << " strength "
                << s.strength << " mirror " << int(o.symmetry.mode) << "\n";
            for (const Dab& d : dabs)
                out << "(" << d.x << "," << d.y << "," << d.pressure << ") ";
            out << "\nonce\n" << rungMap(once, r) << "twice\n" << rungMap(twice, r) << checked.failure;
            first = out.str();
        }
    }
    INFO(differ << " of 120 strokes changed on the second pass, " << failed << " failed the check; " << painted
                << " painted\n" << first);
    CHECK(differ == 0);
    CHECK(failed == 0);
    CHECK(painted > 60);
}

TEST_CASE("soft brush: Codex round 6, finding 3: a mirror dab's work doesn't grow with the gap between its images",
          "[shading][soft][symmetry]") {
    // Each image is solved in its own box. The stroke's first dab also
    // copies the canvas (once a stroke), so the dab measured is its second.
    // Under Mirror XY the images 50 rows from the top and bottom of a
    // canvas 100 tall share a box; 200 tall they don't.
    const Ramp r = steel();
    BrushSettings s;
    s.size = 9;
    s.tip = Tip::Square;
    s.strength = 0.5;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 0.0);
    auto work = [&](SymmetryMode mode, int w, int h) {
        const Canvas start = filled(w, h, rungCell(r, 4));
        StrokeOptions o;
        o.symmetry = {mode};
        Stroke st(start, brush, s, o);
        st.addDab({12, 50}, false);
        const long long from = SoftShadeBrush::cellsVisited();
        st.addDab({10, 50}, false);
        return SoftShadeBrush::cellsVisited() - from;
    };
    const long long x1 = work(SymmetryMode::MirrorX, 160, 100), x2 = work(SymmetryMode::MirrorX, 1600, 100);
    const long long xy1 = work(SymmetryMode::MirrorXY, 160, 200), xy2 = work(SymmetryMode::MirrorXY, 1600, 2000);
    INFO("mirror X: " << x1 << " on 160x100, " << x2 << " on 1600x100; mirror XY: " << xy1 << " on 160x200, " << xy2
                      << " on 1600x2000");
    CHECK(x1 > 0);
    CHECK(xy1 > 0);
    CHECK(std::abs(double(x2 - x1)) <= 0.1 * double(x1));
    CHECK(std::abs(double(xy2 - xy1)) <= 0.1 * double(xy1));
}

namespace {

// A size 8 round soft stroke of 60 unjoined dabs along a wave across a
// gradient, at half strength: its work (cellsVisited) and times a dab.
struct WaveRun {
    long long work = 0;
    std::vector<double> ms;
};

WaveRun wave(int w, int h, bool plateaus) {
    const Ramp r = steel();
    const Canvas start = plateaus ? field(r, w, h, [&](int x, int) { return 2 + x * 6 / w; })
                                  : field(r, w, h, [&](int x, int) { return 1 + x * 11 / (w - 1); });
    BrushSettings s;
    s.size = 8;
    s.tip = Tip::Round;
    s.strength = 0.5;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 0.0);
    Stroke st(start, brush, s);
    WaveRun run;
    const long long from = SoftShadeBrush::cellsVisited();
    for (int i = 0; i < 60; ++i) {
        const double t = i / 59.0;
        const Dab d{int(std::lround(w * (0.1 + 0.8 * t))), int(std::lround(h * (0.5 + 0.3 * std::sin(t * 6.2832 * 2))))};
        const auto t0 = std::chrono::steady_clock::now();
        st.addDab(d, false);
        run.ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    run.work = (SoftShadeBrush::cellsVisited() - from) / 60;
    return run;
}

} // namespace

TEST_CASE("soft brush: Codex round 6, finding 2; Claude round 6, N3: a dab's work grows no faster than the canvas's "
          "sides", "[shading][soft]") {
    // The brush's own work a dab (its copies, solves and rung reads, not the
    // lint's): the solve is bounded by the dab, the canvas is copied once a
    // stroke, and the start's findings are read a row or column at a time.
    long long work[3] = {};
    const std::pair<int, int> sizes[3] = {{80, 40}, {160, 100}, {400, 160}};
    for (int i = 0; i < 3; ++i)
        work[i] = wave(sizes[i].first, sizes[i].second, false).work;
    INFO("a dab's work: " << work[0] << " on 80x40, " << work[1] << " on 160x100, " << work[2] << " on 400x160");
    CHECK(work[0] > 0);
    CHECK(work[1] * 120 <= work[0] * 260);
    CHECK(work[2] * 120 <= work[0] * 560);
}

// The times PLAN §10 gives for a soft dab and for its lint, on 160x100 and
// 400x160 canvases: strokes of size 9 dabs over gradients and plateaus.
TEST_CASE("soft brush timings", "[.timing]") {
    const Ramp r = steel();
    std::mt19937 rng(2026102504);
    auto pick = [&](int a, int b) { return bwtest::uniformInt(rng, a, b); };
    for (const auto& [w, h] : {std::pair{160, 100}, std::pair{400, 160}}) {
        std::vector<double> dabMs, lintMs;
        for (int i = 0; i < 12; ++i) {
            const Canvas start = startCanvas(i % 2 ? Start::Plateaus : Start::Gradient, r, w, h, rng);
            const SoftShadeBrush brush(r, pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken, 0.0);
            BrushSettings s;
            s.size = 9;
            s.tip = Tip::Round;
            s.strength = 0.5;
            Stroke st(start, brush, s);
            int x = pick(0, w - 1), y = pick(0, h - 1);
            for (int k = 0; k < 40; ++k) {
                const Canvas before = st.preview();
                const auto t0 = std::chrono::steady_clock::now();
                st.addDab({x, y}, true);
                const auto t1 = std::chrono::steady_clock::now();
                dabMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                const Canvas after = st.preview();
                std::vector<std::pair<int, int>> changed;
                for (int yy = 0; yy < h; ++yy)
                    for (int xx = 0; xx < w; ++xx)
                        if (!(before.at(xx, yy) == after.at(xx, yy)))
                            changed.emplace_back(xx, yy);
                const auto t2 = std::chrono::steady_clock::now();
                (void)lint::addedFindingsOnce(before, after, r, changed);
                const auto t3 = std::chrono::steady_clock::now();
                lintMs.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
                x = std::clamp(x + pick(-3, 3), 0, w - 1);
                y = std::clamp(y + pick(-2, 2), 0, h - 1);
            }
        }
        auto line = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (const double t : v)
                sum += t;
            std::ostringstream o;
            o << "mean " << sum / double(v.size()) << " ms, p95 " << v[v.size() * 95 / 100] << " ms, worst "
              << v.back() << " ms";
            return o.str();
        };
        WARN(w << "x" << h << ": dab " << line(dabMs) << "; one lint of a dab's change " << line(lintMs));
        for (const bool plateaus : {false, true}) {
            const WaveRun run = wave(w, h, plateaus);
            WARN(w << "x" << h << (plateaus ? " plateaus" : " gradient") << ": a size 8 wave of 60 dabs, a dab "
                   << line(run.ms) << ", work " << run.work);
        }
    }
}

namespace {

// What `after`, changed from `before` at `cells`, adds by the lint's rule.
std::vector<lint::LadderFinding> addedAt(const Canvas& before, const Canvas& after, const Ramp& r) {
    std::vector<std::pair<int, int>> cells;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (!(before.at(x, y) == after.at(x, y)))
                cells.emplace_back(x, y);
    return lint::addedFindingsOnce(before, after, r, cells);
}

std::string listed(const std::vector<lint::LadderFinding>& v) {
    std::string s;
    for (const auto& f : v)
        s += f.message + "\n";
    return s;
}

} // namespace

TEST_CASE("soft brush: round 7, one glyph: a dab that shades a solid field with one glyph gives way",
          "[shading][soft]") {
    // A flat field of a solid rung uses no shading glyph. A hard 3x3 dab a
    // rung lighter shades its nine cells with one glyph: one glyph from
    // false to true, the ruling's finding. Round 6's guard read findings at
    // cells only and painted it; the guard now reads one glyph through the
    // stroke's lint index, and the dab gives way.
    const Ramp r = steel();
    const Canvas start = field(r, 12, 7, [](int, int) { return 8; });
    Canvas wants = start;
    for (int y = 2; y <= 4; ++y)
        for (int x = 5; x <= 7; ++x)
            wants.at(x, y) = rungCell(r, 9);
    const auto found = addedAt(start, wants, r);
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].kind == Kind::OneGlyph);
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    s.strength = 0.25;
    Canvas c = start;
    paintStroke(start, SoftShadeBrush(r, ShadeDirection::Lighten, 1.0), s, {{6, 3}}).apply(c);
    INFO(rungMap(c, r) << listed(addedAt(start, c, r)));
    CHECK(addedAt(start, c, r).empty());
    CHECK(c == start);
}

TEST_CASE("soft brush: round 7, noise: a dab that raises the noise over the limit gives way", "[shading][soft]") {
    // A field of solid rung 8 with ten half blocks, white over dark grey,
    // across row 1: each is two lonely pixels, 20 of 836, noise 0.024, over
    // the 0.02 limit. Shading a 3x3 patch takes its pixels and its
    // neighbours' out of the count, so the noise goes up (0.025): the
    // ruling's noise finding. Two shade cells in the bottom row, out of the
    // noise's reach, keep the shading two glyphs. Round 6 painted it; the
    // dab now gives way.
    const Ramp r = steel();
    Canvas start = field(r, 40, 12, [](int x, int y) { return y == 11 && x < 2 ? 9 + x : 8; });
    for (int x = 2; x <= 20; x += 2) {
        Cell& half = start.at(x, 1);
        half.ch = U'▀';
        half.fg = Color::indexed(15);
        half.bg = Color::indexed(8);
    }
    Canvas wants = start;
    for (int y = 6; y <= 8; ++y)
        for (int x = 29; x <= 31; ++x)
            wants.at(x, y) = rungCell(r, 9);
    const auto found = addedAt(start, wants, r);
    INFO(listed(found));
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].kind == Kind::Noise);
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    s.strength = 0.25;
    Canvas c = start;
    paintStroke(start, SoftShadeBrush(r, ShadeDirection::Lighten, 1.0), s, {{30, 7}}).apply(c);
    INFO(rungMap(c, r) << listed(addedAt(start, c, r)));
    CHECK(addedAt(start, c, r).empty());
    CHECK(c == start);
}

TEST_CASE("soft brush round 8, P3-A: a dab that gave way entirely doesn't hold back a later one", "[shading][soft]") {
    // A at (29,12) can't shade the dome's edge without a finding and gives
    // way; B at (31,13) alone darkens row 13. A's wants went with its paint,
    // so A then B paints what B alone does.
    const Ramp r = *findRamp("fire");
    const std::array<std::string, 7> dome{
        "4444444444444ggggggg44444444444444444444", "44444444444ggggggggggg444444444444444444",
        "4444444444gfffffffffffg44444444444444444", "4444444440feeeeeeeeeeef04444444444444444",
        "44444444440ddddddddddd044444444444444444", "4444444444400ccccccc00444444444444444444",
        "4444444444444000000044444444444444444444"};
    const Canvas start = field(r, 40, 20, [&](int x, int y) {
        if (y < 10 || y > 16)
            return 4;
        const char c = dome[size_t(y - 10)][size_t(x)];
        return c <= '9' ? c - '0' : c - 'a' + 10;
    });
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Round;
    s.strength = 0.5;
    StrokeOptions o;
    o.ice = false;
    const SoftShadeBrush brush(r, ShadeDirection::Darken, 0.0);
    auto paint = [&](std::vector<Dab> dabs) {
        Stroke st(start, brush, s, o);
        for (const Dab& d : dabs)
            st.addDab(d, false);
        return st.preview();
    };
    auto row13 = [&](const Canvas& c) { return rungMap(c, r).substr(13 * 41 + 26, 9); };
    const Canvas a = paint({{29, 12}}), b = paint({{31, 13}}), ab = paint({{29, 12}, {31, 13}});
    REQUIRE(rungs(a, r) == rungs(start, r));
    REQUIRE(row13(b) == "444433344");
    INFO("A then B\n" << rungMap(ab, r));
    CHECK(rungs(ab, r) == rungs(b, r));
    CHECK(added(start, ab, r).empty());
}

TEST_CASE("soft brush round 9, Codex P2: a dab that laid something isn't solved again", "[shading][soft]") {
    // The second dab's first solve lightens (0,0) from 4 to 5. A dab that
    // laid nothing is solved again with the stroke's unmoved wants set
    // aside (round 8, P3-A); this one laid a cell, so it keeps its 5 and
    // isn't deepened by a second solve.
    const Ramp r = *findRamp("steel");
    Canvas start(3, 1);
    start.at(0, 0) = rungCell(r, 4);
    start.at(1, 0) = rungCell(r, 12);
    start.at(2, 0) = rungCell(r, 4);
    BrushSettings s;
    s.size = 1;
    s.strength = 1.0;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 1.0);
    Stroke stroke(start, brush, s);
    stroke.addDab({1, 0}, false);
    stroke.addDab({0, 0}, false);
    CHECK(rungMap(stroke.preview(), r) == "5c4\n");
    CHECK(added(start, stroke.preview(), r).empty());
}

// The soft brush's timings beside the form and despeckle brushes', on
// 160x100 and 400x160 (PLAN §10): each brush's wave of 60 unjoined size-8
// dabs across a gradient and 12 random walks of 40 joined size-9 dabs, at
// half strength, every dab timed, the first included.
TEST_CASE("shading brush timings", "[.timing]") {
    const Ramp r = steel();
    auto line = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (const double t : v)
            sum += t;
        std::ostringstream o;
        o << "mean " << sum / double(v.size()) << " / p95 " << v[v.size() * 95 / 100] << " / worst " << v.back()
          << " ms";
        return o.str();
    };
    const SoftShadeBrush soft(r, ShadeDirection::Darken, 0.0);
    const FormShadeBrush form(r);
    const DespeckleBrush despeckle(r);
    const std::pair<const char*, const Brush*> brushes[] = {{"soft", &soft}, {"form", &form}, {"despeckle", &despeckle}};
    for (const auto& [w, h] : {std::pair{160, 100}, std::pair{400, 160}})
        for (const auto& [name, brush] : brushes) {
            std::vector<double> wave, walk;
            {
                const Canvas start = field(r, w, h, [&](int x, int) { return 1 + x * 11 / (w - 1); });
                BrushSettings s;
                s.size = 8;
                s.tip = Tip::Round;
                s.strength = 0.5;
                Stroke st(start, *brush, s);
                for (int i = 0; i < 60; ++i) {
                    const double t = i / 59.0;
                    const Dab d{int(std::lround(w * (0.1 + 0.8 * t))),
                                int(std::lround(h * (0.5 + 0.3 * std::sin(t * 6.2832 * 2))))};
                    const auto t0 = std::chrono::steady_clock::now();
                    st.addDab(d, false);
                    wave.push_back(
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                }
            }
            std::mt19937 rng(2026102508);
            auto pick = [&](int a, int b) { return bwtest::uniformInt(rng, a, b); };
            for (int i = 0; i < 12; ++i) {
                const Canvas start = startCanvas(i % 2 ? Start::Plateaus : Start::Gradient, r, w, h, rng);
                BrushSettings s;
                s.size = 9;
                s.tip = Tip::Round;
                s.strength = 0.5;
                Stroke st(start, *brush, s);
                int x = pick(0, w - 1), y = pick(0, h - 1);
                for (int k = 0; k < 40; ++k) {
                    const auto t0 = std::chrono::steady_clock::now();
                    st.addDab({x, y}, true);
                    walk.push_back(
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                    x = std::clamp(x + pick(-3, 3), 0, w - 1);
                    y = std::clamp(y + pick(-2, 2), 0, h - 1);
                }
            }
            WARN(name << " " << w << "x" << h << ": wave a dab " << line(wave) << "; walk a dab " << line(walk));
        }
}
