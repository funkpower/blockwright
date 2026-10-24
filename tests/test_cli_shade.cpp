// `blockwright-cli shade` and the editor's ramp brush (issue #44): the CLI
// shades with the document's iCE colours, as the editor does, and turns them
// on only with --ice.

#include "read_file.h"
#include "shade.h"

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/ramp.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace bw;

namespace {

Ramp fire() { return *findRamp("fire"); }  // Black Red LightRed Yellow White

// A row of every rung of the ramp, then a blank, as a file with iCE colours
// `ice` holds it once parsed: with them off a bright background is stored as
// its dark colour, blinking (iceCell).
AnsiDocument ladder(const Ramp& r, bool ice) {
    AnsiDocument d;
    d.ice = ice;
    d.canvas = Canvas(r.topRung() + 2, 1);
    for (int i = 0; i <= r.topRung(); ++i)
        d.canvas.at(i, 0) = iceCell(rungCell(r, i), ice);
    return d;
}

// The editor's ramp brush (RampShadeTool): a stroke of RampShadeBrush moving
// a whole colour at full strength, scaled by the dock's strength, with the
// document's iCE colours, dragged along each row of the box. The editor
// itself, opening a file and shading it through the view, is compared with
// the CLI in test_app_cli_shade.cpp.
Canvas editorShade(const AnsiDocument& doc, const Ramp& r, ShadeDirection dir, int steps, const lint::Box& box) {
    const RampShadeBrush brush(r, dir, kRungsPerStep);
    BrushSettings s{1, Tip::Square, double(steps) / kRungsPerStep, PressureTo::Nothing};
    StrokeOptions o;
    o.ice = doc.ice;
    Stroke stroke(doc.canvas, brush, s, o);
    for (int y = box.y0; y <= box.y1; ++y)
        for (int x = box.x0; x <= box.x1; ++x)
            stroke.addDab(Dab{x, y}, x != box.x0);
    Canvas c = doc.canvas;
    stroke.edit().apply(c);
    return c;
}

bwcli::ShadeOptions options(const Ramp& r, ShadeDirection dir = ShadeDirection::Lighten, int steps = 1) {
    bwcli::ShadeOptions o;
    o.ramp = r;
    o.direction = dir;
    o.steps = steps;
    return o;
}

// Two LightRed █ cells, the piece from the #41 review, with iCE `ice`, and
// optionally a blinking cell after them.
AnsiDocument lightRedPair(bool ice, bool blink = false) {
    AnsiDocument d;
    d.ice = ice;
    d.canvas = Canvas(4, 1);
    for (int x = 0; x < 2; ++x)
        d.canvas.at(x, 0) = rungCell(fire(), 8);
    if (blink) {
        Cell& c = d.canvas.at(3, 0);
        c.ch = U'A';
        c.fg = Color::indexed(4);
        c.attrs = attr::Blink;
    }
    return d;
}

bool anyBrightBackground(const Canvas& c) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (c.at(x, y).bg.paletteIndex() >= 8)
                return true;
    return false;
}

// `body` as a file with SAUCE and its iCE flag off, parsed.
AnsiDocument parseIceOff(const std::string& body) {
    Sauce s = Sauce::fresh(80, 1);
    s.setIce(false);
    return parseAnsi(body + encodeSauce(s, std::uint32_t(body.size())));
}

} // namespace

TEST_CASE("cli shade gives the cells the editor's ramp brush gives, iCE on and off", "[cli][shade]") {
    for (const Ramp& r : namedRamps())
        for (const bool ice : {true, false})
            for (const ShadeDirection dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
                for (int steps = 1; steps <= kRungsPerStep; ++steps) {
                    INFO(r.name << (ice ? " iCE on" : " iCE off")
                                << (dir == ShadeDirection::Lighten ? " lighten " : " darken ") << steps);
                    AnsiDocument doc = ladder(r, ice);
                    const lint::Box all{0, 0, doc.canvas.width() - 1, 0};
                    const Canvas want = editorShade(doc, r, dir, steps, all);
                    const bwcli::Shaded got = bwcli::shadeDocument(doc, options(r, dir, steps));
                    CHECK(doc.canvas == want);
                    CHECK(doc.ice == ice);
                    CHECK_FALSE(got.iceTurnedOn);
                    CHECK(got.why.empty());
                    if (!ice)
                        CHECK_FALSE(anyBrightBackground(doc.canvas));
                }
}

TEST_CASE("cli shade keeps a piece without iCE colours without them", "[cli][shade]") {
    // fire --box 0,0,1,0 on a LightRed pair: the next rung is a LightRed /
    // Yellow shade, a bright background either way round
    AnsiDocument doc = lightRedPair(false);
    bwcli::ShadeOptions o = options(fire());
    o.box = lint::Box{0, 0, 1, 0};
    const bwcli::Shaded r = bwcli::shadeDocument(doc, o);
    CHECK(r.edit.empty());
    CHECK_FALSE(r.iceTurnedOn);
    CHECK(r.why.empty());
    CHECK_FALSE(doc.ice);
    const AnsiOutput out = writeAnsi(doc);
    CHECK_FALSE(out.lossy());
    CHECK_FALSE(parseAnsi(out.bytes).ice);
    CHECK(doc.canvas == lightRedPair(false).canvas);

    SECTION("and keeps a blinking cell blinking") {
        AnsiDocument blinking = lightRedPair(false, true);
        const bwcli::Shaded b = bwcli::shadeDocument(blinking, o);
        CHECK(b.why.empty());
        CHECK_FALSE(b.iceTurnedOn);
        const AnsiOutput w = writeAnsi(blinking);
        CHECK_FALSE(w.lossy());
        CHECK(parseAnsi(w.bytes).canvas.at(3, 0).attrs & attr::Blink);
    }
}

TEST_CASE("cli shade --ice turns iCE colours on and shades past the dark backgrounds", "[cli][shade]") {
    AnsiDocument doc = lightRedPair(false);
    bwcli::ShadeOptions o = options(fire());
    o.box = lint::Box{0, 0, 1, 0};
    o.ice = true;
    const bwcli::Shaded r = bwcli::shadeDocument(doc, o);
    CHECK(r.edit.changes.size() == 2);
    CHECK(doc.canvas.at(0, 0) == rungCell(fire(), 9));
    CHECK(doc.canvas.at(0, 0).bg.paletteIndex() >= 8);
    CHECK(r.iceTurnedOn);
    CHECK(r.why.empty());
    CHECK(doc.ice);
    const AnsiOutput out = writeAnsi(doc);
    CHECK_FALSE(out.lossy());
    CHECK(parseAnsi(out.bytes).ice);

    SECTION("a blinking cell would lose its blink: the reason says so") {
        AnsiDocument blinking = lightRedPair(false, true);
        const bwcli::Shaded b = bwcli::shadeDocument(blinking, o);
        CHECK(b.why == "--ice turns iCE colours on, and 1 blinking cell would lose its blink");
        CHECK(writeAnsi(blinking).blinkDropped == 1);
    }
    SECTION("on a piece that has iCE colours it changes nothing") {
        AnsiDocument on = lightRedPair(true);
        AnsiDocument plain = lightRedPair(true);
        bwcli::ShadeOptions p = o;
        p.ice = false;
        const bwcli::Shaded a = bwcli::shadeDocument(on, o);
        bwcli::shadeDocument(plain, p);
        CHECK(on.canvas == plain.canvas);
        CHECK_FALSE(a.iceTurnedOn);
    }
}

TEST_CASE("cli shade reads a piece without the iCE flag but with bright backgrounds as the editor opens it", "[cli][shade]") {
    // a bright background the parser kept (SGR 100-107): writeAnsi writes it
    // with iCE on, and the editor opens the file with iCE on
    AnsiDocument doc = lightRedPair(false);
    doc.canvas.at(2, 0) = rungCell(fire(), 10);
    AnsiDocument editor = doc;
    REQUIRE(openAsEditor(editor).iceInferred);
    const Canvas want = editorShade(editor, fire(), ShadeDirection::Lighten, 1, {0, 0, 1, 0});
    bwcli::ShadeOptions o = options(fire());
    o.box = lint::Box{0, 0, 1, 0};
    const bwcli::Shaded r = bwcli::shadeDocument(doc, o);
    CHECK(doc.canvas == want);
    CHECK(doc.canvas.at(0, 0) == rungCell(fire(), 9));
    CHECK(r.iceTurnedOn);
}

TEST_CASE("cli shade refuses a box off the canvas", "[cli][shade]") {
    AnsiDocument doc = lightRedPair(true);
    bwcli::ShadeOptions o = options(fire());
    o.box = lint::Box{4, 0, 6, 0};
    try {
        bwcli::shadeDocument(doc, o);
        FAIL("no Failure thrown");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "the box 4,0,6,0 is off the 4x1 canvas");
    }
}

TEST_CASE("cli shade opens a piece as the editor does: inferred iCE colours stay on, blink lost is counted",
          "[cli][shade]") {
    // A LightRed space (a bright background, iCE flag off) and a blinking
    // red block, as parsed: darkening the space leaves no bright background
    AnsiDocument doc = parseIceOff("\x1b[0;101m \x1b[0;5;31m\xdb");
    REQUIRE_FALSE(doc.ice);
    REQUIRE(doc.canvas.at(1, 0).attrs & attr::Blink);
    bwcli::ShadeOptions o = options(fire(), ShadeDirection::Darken);
    o.box = lint::Box{0, 0, 0, 0};
    const bwcli::Shaded r = bwcli::shadeDocument(doc, o);
    CHECK(rungOf(fire(), doc.canvas.at(0, 0)) == 7);
    CHECK_FALSE(anyBrightBackground(doc.canvas));
    CHECK(doc.ice);
    CHECK(doc.sauce->ice());
    CHECK(r.iceTurnedOn);
    CHECK(r.blinkDropped == 1);
    CHECK(r.why.empty());
    CHECK_FALSE(doc.canvas.at(1, 0).attrs & attr::Blink);
    const AnsiOutput out = writeAnsi(doc);
    CHECK(out.blinkDropped == 0);
    CHECK(parseAnsi(out.bytes).ice);
}

TEST_CASE("cli shade doesn't count a cell that lost its blink on open once it shades it, as the editor doesn't",
          "[cli][shade]") {
    // #82: the shade writes the cell afresh as a rung, which has no blink
    // with iCE colours off or on, so its claim ends with it
    AnsiDocument doc = parseIceOff("\x1b[0;101m \x1b[0;5;31m\xdb\x1b[0;5;31m\xdb");
    bwcli::ShadeOptions o = options(fire(), ShadeDirection::Darken);
    o.box = lint::Box{1, 0, 1, 0};
    const bwcli::Shaded r = bwcli::shadeDocument(doc, o);
    CHECK(rungOf(fire(), doc.canvas.at(1, 0)) == 3);
    CHECK_FALSE(doc.canvas.at(1, 0).lostBlink);
    CHECK(doc.canvas.at(2, 0).lostBlink);
    CHECK(r.blinkDropped == 1);
}

TEST_CASE("cli shade counts no blink lost on a blank glyph, as the editor doesn't", "[cli][shade]") {
    // #42: a blinking black space beside a bright background loses nothing
    // on open, so the shaded piece isn't lossy
    AnsiDocument doc = parseIceOff("\x1b[0;5;40m \x1b[0;37;104mB");
    REQUIRE(doc.canvas.at(0, 0).attrs & attr::Blink);
    bwcli::ShadeOptions o = options(fire());
    const bwcli::Shaded r = bwcli::shadeDocument(doc, o);
    CHECK(r.iceTurnedOn);
    CHECK(r.blinkDropped == 0);
    CHECK(r.why.empty());
    CHECK_FALSE(writeAnsi(doc).lossy());
}
