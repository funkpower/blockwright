// The smudge brush (PLAN §10, "Shading", blend/smudge): what it lays, what
// stops it, mirrors, and that no stamp adds a ladder lint finding.

#include "blockwright/ansi.h"
#include "portable_random.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/smudge.h"
#include "blockwright/lint.h"
#include "blockwright/palette.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace bw;

namespace {

Ramp steel() { return *findRamp("steel"); }  // k K w W

// One row of rungs, a hex digit a cell.
Canvas row(const Ramp& r, const std::string& rungs) {
    Canvas c(int(rungs.size()), 1);
    for (size_t x = 0; x < rungs.size(); ++x)
        c.at(int(x), 0) = rungCell(r, std::stoi(std::string(1, rungs[x]), nullptr, 16));
    return c;
}

std::string rungMap(const Canvas& c, const Ramp& r) {
    std::ostringstream out;
    for (int y = 0; y < c.height(); ++y) {
        if (y)
            out << '\n';
        for (int x = 0; x < c.width(); ++x) {
            const auto g = rungOf(r, c.at(x, y));
            out << (!g ? '.' : *g < 10 ? char('0' + *g) : char('a' + *g - 10));
        }
    }
    return out.str();
}

std::shared_ptr<const Mask> shared(Mask m) { return std::make_shared<const Mask>(std::move(m)); }

std::vector<std::string> added(const Canvas& before, const Canvas& after, const Ramp& r) {
    std::vector<std::pair<int, int>> changed;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (!(before.at(x, y) == after.at(x, y)))
                changed.emplace_back(x, y);
    std::vector<std::string> out;
    for (const auto& f : lint::addedFindingsOnce(before, after, r, changed))
        out.push_back(f.message + " at " + std::to_string(f.x) + "," + std::to_string(f.y));
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& m : v)
        s += m + "\n";
    return s;
}

Canvas smudged(const Canvas& start, const Ramp& r, const std::vector<Dab>& dabs, double strength, int size = 1,
               StrokeOptions o = {}) {
    BrushSettings s;
    s.size = size;
    s.strength = strength;
    Canvas c = start;
    paintStroke(start, SmudgeBrush(r), s, dabs, o).apply(c);
    return c;
}

} // namespace

TEST_CASE("smudgeMix goes strength of the way from the cell to what the brush carries", "[smudge]") {
    CHECK(smudgeMix(6, 8.0, 0.5) == Catch::Approx(7.0));
    CHECK(smudgeMix(4, 8.0, 1.0) == Catch::Approx(8.0));
    CHECK(smudgeMix(4, 8.0, 0.0) == Catch::Approx(4.0));
    CHECK(smudgeMix(8, 4.0, 0.25) == Catch::Approx(7.0));
}

TEST_CASE("smudgeRung takes the nearest rung, a half away from where the cell was", "[smudge]") {
    CHECK(smudgeRung(7.0, 6) == 7);
    CHECK(smudgeRung(6.5, 6) == 7);
    CHECK(smudgeRung(6.5, 7) == 6);
    CHECK(smudgeRung(6.25, 6) == 6);
    CHECK(smudgeRung(6.75, 6) == 7);
    CHECK(smudgeRung(5.5, 6) == 5);
    CHECK(smudgeRung(6.4, 7) == 6);
}

TEST_CASE("a smudge drags a plane over the edge of the next, fading as it goes", "[smudge]") {
    const Ramp r = steel();
    const Canvas start = row(r, "888888666666");
    // picked up at 1, carried over the 8s, laid 7 then 7 (6.5, away from 6)
    // then fading to 6.25 and on
    CHECK(rungMap(smudged(start, r, {{1, 0}, {10, 0}}, 0.5), r) == "888888776666");
    // the other way it drags the 6s into the 8s
    CHECK(rungMap(smudged(start, r, {{10, 0}, {1, 0}}, 0.5), r) == "888877666666");
}

TEST_CASE("a smudge builds up: going back over the stroke pulls the other way", "[smudge]") {
    const Ramp r = steel();
    const Canvas start = row(r, "888888666666");
    // back from 10 carrying just over 6: 7 at 7 (6.51), 6 and 5, then 8
    CHECK(rungMap(smudged(start, r, {{1, 0}, {10, 0}, {1, 0}}, 0.5), r) == "888887776666");
}

TEST_CASE("a smudge lays nothing where it starts, or at strength 0", "[smudge]") {
    const Ramp r = steel();
    const Canvas start = row(r, "888888666666");
    BrushSettings s;
    s.size = 5;
    s.strength = 1.0;
    CHECK(paintStroke(start, SmudgeBrush(r), s, {{6, 0}}).empty());
    s.strength = 0.0;
    CHECK(paintStroke(start, SmudgeBrush(r), s, {{0, 0}, {11, 0}, {0, 0}}).empty());
}

TEST_CASE("black, the selection and the alpha lock stop the smear", "[smudge]") {
    const Ramp r = steel();
    SECTION("black is ink: the carry drops and starts again past it") {
        const Canvas start = row(r, "8888880666666");
        CHECK(rungMap(smudged(start, r, {{1, 0}, {11, 0}}, 0.5), r) == "8888880666666");
    }
    SECTION("off the ramp the same") {
        Canvas start = row(r, "8888886666666");
        start.at(6, 0) = Cell{U'x'};
        Canvas after = smudged(start, r, {{1, 0}, {11, 0}}, 0.5);
        CHECK(after == start);
    }
    SECTION("outside the selection") {
        const Canvas start = row(r, "888888666666");
        StrokeOptions o;
        o.selection = shared(Mask::rect(12, 1, 0, 0, 6, 0));
        CHECK(rungMap(smudged(start, r, {{1, 0}, {10, 0}}, 0.5, 1, o), r) == "888888766666");
    }
    SECTION("a transparent cell under the alpha lock") {
        Canvas start = row(r, "888888666666");
        start.at(7, 0).transparent = true;
        StrokeOptions o;
        o.lockTransparent = true;
        const Canvas after = smudged(start, r, {{1, 0}, {10, 0}}, 0.5, 1, o);
        CHECK(rungMap(after, r) == "888888766666");
        CHECK(after.at(7, 0) == start.at(7, 0));
        // without the lock too: it shows the layer beneath, not its look
        CHECK(smudged(start, r, {{1, 0}, {10, 0}}, 0.5) == after);
    }
}

TEST_CASE("with iCE off a smudge reads a bright background as off the ramp and writes none", "[smudge]") {
    const Ramp r = steel();
    Canvas start = row(r, "888888666666");
    // 6 the way round that needs a bright background (dark grey): blinking
    // with iCE off
    Cell bright = start.at(7, 0);
    std::swap(bright.fg, bright.bg);
    bright.ch = U'▒';
    start.at(7, 0) = bright;
    REQUIRE(rungOf(r, bright) == 6);
    REQUIRE(!(iceCell(bright, false) == bright));
    StrokeOptions o;
    o.ice = false;
    const Canvas after = smudged(start, r, {{1, 0}, {10, 0}}, 0.5, 1, o);
    CHECK(after.at(7, 0) == bright);
    CHECK(rungOf(r, after.at(6, 0)) == 7);
    for (int x = 0; x < after.width(); ++x)
        if (!(after.at(x, 0) == start.at(x, 0)))
            CHECK(iceCell(after.at(x, 0), false) == after.at(x, 0));
}

TEST_CASE("under a mirror a smudge keeps a symmetric canvas symmetric", "[smudge]") {
    const Ramp r = steel();
    const Canvas start = row(r, "888888666666666666888888");
    StrokeOptions o;
    o.symmetry.mode = SymmetryMode::MirrorX;
    const Canvas after = smudged(start, r, {{1, 0}, {10, 0}}, 0.5, 1, o);
    CHECK(rungMap(after, r) == "888888776666666677888888");

    // a wider tip on a taller canvas, both mirrors
    const Canvas field = [&] {
        Canvas c(20, 8);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 20; ++x) {
                const int dx = std::min(x, 19 - x), dy = std::min(y, 7 - y);
                c.at(x, y) = rungCell(r, std::clamp(4 + dx / 2 + dy / 2, 4, 12));
            }
        return c;
    }();
    o.symmetry.mode = SymmetryMode::MirrorXY;
    const Canvas both = smudged(field, r, {{1, 1}, {8, 3}, {2, 3}}, 0.75, 3, o);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 20; ++x) {
            CHECK(both.at(x, y) == both.at(19 - x, y));
            CHECK(both.at(x, y) == both.at(x, 7 - y));
        }
}

namespace {

// Whether a canvas reads the same mirrored across columns and across rows.
bool symmetric(const Canvas& c) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (!(c.at(x, y) == c.at(c.width() - 1 - x, y)) || !(c.at(x, y) == c.at(x, c.height() - 1 - y)))
                return false;
    return true;
}

Canvas rows(const Ramp& r, const std::vector<std::string>& lines) {
    Canvas c(int(lines.front().size()), int(lines.size()));
    for (size_t y = 0; y < lines.size(); ++y) {
        const Canvas one = row(r, lines[y]);
        for (int x = 0; x < c.width(); ++x)
            c.at(x, int(y)) = one.at(x, 0);
    }
    return c;
}

// A stroke with strength set through pressure (PressureTo::Strength).
Canvas smudgedUnder(const Canvas& start, const Ramp& r, std::vector<Dab> dabs, double strength, double pressure,
                    int size, SymmetryMode mode) {
    BrushSettings s;
    s.size = size;
    s.strength = strength;
    s.pressure = PressureTo::Strength;
    for (Dab& d : dabs)
        d.pressure = pressure;
    StrokeOptions o;
    o.symmetry.mode = mode;
    Canvas c = start;
    paintStroke(start, SmudgeBrush(r), s, dabs, o).apply(c);
    return c;
}

} // namespace

// Where mirror images overlap, a cell takes three or four proposals, which
// its mirror partners receive in another order; the mean must not depend on
// it, or a tie rounds one way on one side and the other on the other.
TEST_CASE("under a mirror a smudge keeps a symmetric canvas symmetric whatever order its images lay in", "[smudge]") {
    const Ramp r = steel();
    StrokeOptions o;
    o.symmetry.mode = SymmetryMode::MirrorXY;
    // Codex's case: the means at x=1 and x=4 came out 1.5 and 1.4999999999999998
    const Canvas codex = smudged(row(r, "213312"), r, {{2, 0}, {3, 0}}, 1.0 / 3.0, 7, o);
    {
        INFO(rungMap(codex, r));
        CHECK(symmetric(codex));
    }

    // the same strength reached through pressure, on a taller canvas
    const Canvas pressed =
        smudgedUnder(rows(r, {"31213", "42424", "31213"}), r, {{2, 2}, {4, 2}}, 1.0, 1.0 / 3.0, 7, SymmetryMode::MirrorXY);
    {
        INFO(rungMap(pressed, r));
        CHECK(symmetric(pressed));
    }

    // half strength at two-thirds pressure, a longer stroke
    const Canvas half = smudgedUnder(rows(r, {"34244243", "33122133", "33122133", "34244243"}), r,
                                     {{5, 0}, {1, 3}, {3, 1}, {1, 2}}, 0.5, 2.0 / 3.0, 5, SymmetryMode::MirrorXY);
    {
        INFO(rungMap(half, r));
        CHECK(symmetric(half));
    }

    // a sweep: symmetric canvases, each mirror, strengths in thirds, sixths,
    // sevenths, ninths and twelfths, set or reached through pressure
    std::mt19937 rng(2026102403);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    int broken = 0;
    for (int k = 0; k < 10000; ++k) {
        const int w = pick(4, 8), h = pick(1, 4);
        Canvas start(w, h);
        for (int y = 0; y < (h + 1) / 2; ++y)
            for (int x = 0; x < (w + 1) / 2; ++x) {
                const Cell c = rungCell(r, pick(1, 4));
                start.at(x, y) = start.at(w - 1 - x, y) = start.at(x, h - 1 - y) = start.at(w - 1 - x, h - 1 - y) = c;
            }
        const SymmetryMode mode = std::array{SymmetryMode::MirrorX, SymmetryMode::MirrorY,
                                             SymmetryMode::MirrorXY}[size_t(pick(0, 2))];
        const int den = std::array{3, 6, 7, 9, 12}[size_t(pick(0, 4))], num = pick(1, den - 1);
        const double strength = pick(0, 1) ? 1.0 : 0.5, pressure = double(num) / den;
        std::vector<Dab> dabs;
        for (int n = pick(2, 5); n > 0; --n)
            dabs.push_back({pick(0, w - 1), pick(0, h - 1)});
        const Canvas after = smudgedUnder(start, r, dabs, strength, pressure, pick(2, 7), mode);
        // MirrorX keeps columns, MirrorY rows; the start is symmetric both ways
        bool ok = true;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (mode != SymmetryMode::MirrorY)
                    ok = ok && after.at(x, y) == after.at(w - 1 - x, y);
                if (mode != SymmetryMode::MirrorX)
                    ok = ok && after.at(x, y) == after.at(x, h - 1 - y);
            }
        if (!ok && ++broken <= 3)
            FAIL_CHECK("stroke " << k << " broke symmetry\n" << rungMap(start, r) << "\nto\n" << rungMap(after, r));
    }
    CHECK(broken == 0);
}

TEST_CASE("a smudge over a ramp gradient adds no lint finding", "[smudge][lint]") {
    const Ramp r = steel();
    Canvas start(40, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 40; ++x)
            start.at(x, y) = rungCell(r, 4);
    Gradient g;
    g.x0 = 4, g.y0 = 0, g.x1 = 35, g.y1 = 0, g.fromRung = 4, g.toRung = 12;
    rampGradient(start, r, g).apply(start);
    REQUIRE(lint::checkLadder(start, r).findings.empty());
    bool changed = false;
    for (const double s : {0.25, 0.5, 0.75, 1.0})
        for (const int size : {1, 3, 7}) {
            const Canvas a = smudged(start, r, {{2, 5}, {37, 5}}, s, size);
            const Canvas b = smudged(start, r, {{37, 2}, {2, 7}}, s, size);
            INFO("strength " << s << " size " << size);
            CHECK(joined(added(start, a, r)).empty());
            CHECK(joined(added(start, b, r)).empty());
            changed = changed || !(a == start) || !(b == start);
        }
    CHECK(changed);
}

namespace {

// Checks every stamp of the stroke as it lands: no lint finding the start
// didn't have, nothing outside what the stroke may paint, every changed cell
// a rung of the ramp, and with iCE off no bright background.
class CheckedSmudge : public Brush {
public:
    CheckedSmudge(const SmudgeBrush& brush, const Canvas& start) : brush_(brush), start_(start) {}
    std::string_view name() const override { return brush_.name(); }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override {
        brush_.dab(stroke, d, size, strength);
        if (!failure.empty())
            return;
        const Ramp& r = brush_.ramp();
        const Canvas now = stroke.preview();
        std::ostringstream o;
        for (const auto& m : added(start_, now, r))
            o << "added: " << m << "\n";
        for (int y = 0; y < now.height(); ++y)
            for (int x = 0; x < now.width(); ++x) {
                if (now.at(x, y) == start_.at(x, y))
                    continue;
                if (!stroke.paintable(x, y))
                    o << "cell " << x << "," << y << " changed outside what the stroke may paint\n";
                if (!rungOf(r, now.at(x, y)))
                    o << "cell " << x << "," << y << " left off the ramp\n";
                if (!stroke.options().ice && !(iceCell(now.at(x, y), false) == now.at(x, y)))
                    o << "cell " << x << "," << y << " given a bright background with iCE off\n";
            }
        failure = o.str();
    }
    mutable std::string failure;

private:
    const SmudgeBrush& brush_;
    const Canvas& start_;
};

} // namespace

TEST_CASE("no stamp of a random smudge adds a lint finding", "[smudge][lint]") {
    std::mt19937 rng(2026102402);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const std::array<const char*, 5> ramps{"steel", "fire", "ice", "toxic", "sunset"};
    int painted = 0;
    for (int k = 0; k < 200; ++k) {
        const int w = 32, h = 12;
        const Ramp r = *findRamp(ramps[size_t(pick(0, 4))]);
        const int top = r.topRung();
        Canvas start(w, h);
        const int kind = pick(0, 3);
        const int lo = pick(1, top / 2), step = pick(2, 6);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int g = lo;
                if (kind == 0)  // terraced planes
                    g = lo + (x / step) * kRungsPerStep;
                else if (kind == 1)  // a ladder, a rung every `step` cells
                    g = lo + x / step + y / 3;
                else if (kind == 2)  // a bump
                    g = lo + std::max(0, 8 - std::abs(x - w / 2) / 2 - std::abs(y - h / 2));
                else  // noise
                    g = lo + pick(0, 3);
                start.at(x, y) = rungCell(r, std::clamp(g, 1, top));
            }
        StrokeOptions o;
        o.ice = pick(0, 1);
        if (pick(0, 2) == 0)
            o.selection = shared(Mask::rect(w, h, pick(0, w / 2), pick(0, h / 2), pick(w / 2, w - 1), pick(h / 2, h - 1)));
        const int sym = pick(0, 5);
        o.symmetry.mode = sym == 1 ? SymmetryMode::MirrorX
                          : sym == 2 ? SymmetryMode::MirrorY
                          : sym == 3 ? SymmetryMode::MirrorXY
                          : sym == 4 ? SymmetryMode::Radial
                                     : SymmetryMode::None;
        const SmudgeBrush brush(r);
        const CheckedSmudge checked(brush, start);
        BrushSettings s;
        s.size = pick(1, 9);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        std::vector<Dab> dabs;
        for (int n = pick(2, 5); n > 0; --n)
            dabs.push_back({pick(0, w - 1), pick(0, h - 1)});
        const CellEdit e = paintStroke(start, checked, s, dabs, o);
        painted += !e.empty();
        INFO("stroke " << k << " ramp " << r.name << " kind " << kind << "\n" << rungMap(start, r));
        CHECK(checked.failure.empty());
        if (!checked.failure.empty())
            FAIL(checked.failure);
    }
    // most strokes lay something
    CHECK(painted > 100);
}
