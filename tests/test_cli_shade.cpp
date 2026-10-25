// `blockwright-cli shade` and the editor's ramp brush (issue #44): the CLI
// shades with the document's iCE colours, as the editor does, and turns them
// on only with --ice. Its form, despeckle and rim-light modes (issue #107)
// against the core's form, the despeckle rule and the rim light.

#include "read_file.h"
#include "shade.h"

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/ramp.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>
#include <utility>

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

namespace {

Ramp steel() { return *findRamp("steel"); }  // k K w W

Cell blackCell() {
    Cell k;
    k.fg = Color::indexed(Black);
    k.bg = Color::indexed(Black);
    return k;
}

// A black w x h piece with iCE colours on.
AnsiDocument blackDoc(int w, int h) {
    AnsiDocument d;
    d.ice = true;
    d.canvas = Canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            d.canvas.at(x, y) = blackCell();
    return d;
}

Mask ellipse(int w, int h, double cx, double cy, double rx, double ry) {
    Mask m(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (std::pow((x - cx) / rx, 2) + std::pow((y - cy) / ry, 2) <= 1)
                m.set(x, y);
    return m;
}

// A disc `across` columns, half as many rows, three cells in from the edge.
Mask disc(int across) {
    const int w = across + 6, h = across / 2 + 6;
    return ellipse(w, h, (w - 1) / 2.0, (h - 1) / 2.0, across / 2.0, across / 4.0);
}

// A sw x sh rectangle three cells in from the edge of its canvas.
Mask rectangle(int sw, int sh) { return Mask::rect(sw + 6, sh + 6, 3, 3, sw + 2, sh + 2); }

// The mask as a piece: its cells white blocks, the rest black spaces.
Canvas maskPiece(const Mask& m) {
    Canvas c(m.width(), m.height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            c.at(x, y) = m.contains(x, y) ? rungCell(steel(), steel().topRung()) : blackCell();
    return c;
}

bwcli::ShadeOptions formOptions(FormShape shape, const Mask& m, Compass light = Compass::NW, bool glint = false) {
    bwcli::ShadeOptions o;
    o.ramp = steel();
    o.mode = bwcli::ShadeMode::Form;
    o.form = shape;
    o.light = light;
    o.glint = glint;
    o.mask = bwcli::maskOf(maskPiece(m));
    return o;
}

using Place = std::pair<int, std::pair<int, int>>;  // kind, x, y
std::set<Place> places(const lint::LadderReport& r) {
    std::set<Place> s;
    for (const auto& f : r.findings)
        s.insert({int(f.kind), {f.x, f.y}});
    return s;
}

// Checks the shaded piece against the core's form for the mask: every cell
// of the mask at its tone, nothing else changed, and no ladder finding the
// input didn't have. Returns how many cells the form laid.
int checkForm(const AnsiDocument& before, const AnsiDocument& after, const Mask& m, const FormOptions& fo,
              Compass light) {
    const std::vector<int> tones = formRungs(steel(), m, fo, light);
    int laid = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) {
            INFO(x << "," << y);
            const int t = tones[size_t(y) * m.width() + x];
            if (m.contains(x, y) && t >= 0) {
                CHECK(rungOf(steel(), after.canvas.at(x, y)) == t);
                laid += after.canvas.at(x, y) != before.canvas.at(x, y);
            } else {
                CHECK(after.canvas.at(x, y) == before.canvas.at(x, y));
            }
        }
    const auto was = places(lint::checkLadder(before.canvas, steel()));
    for (const Place& p : places(lint::checkLadder(after.canvas, steel()))) {
        INFO("finding " << p.first << " at " << p.second.first << "," << p.second.second);
        CHECK(was.count(p));
    }
    return laid;
}

} // namespace

TEST_CASE("cli shade's mask is the piece's cells that aren't blank", "[cli][shade]") {
    Canvas c(4, 1);
    c.at(0, 0) = blackCell();                    // a black space: blank
    c.at(1, 0) = rungCell(steel(), 4);           // K block
    c.at(2, 0) = blackCell();
    c.at(2, 0).ch = U'.';                        // a glyph on black
    c.at(3, 0) = blackCell();
    c.at(3, 0).attrs = attr::Blink;              // a blinking space: not blank
    const Mask m = bwcli::maskOf(c);
    CHECK_FALSE(m.contains(0, 0));
    CHECK(m.contains(1, 0));
    CHECK(m.contains(2, 0));
    CHECK(m.contains(3, 0));
}

TEST_CASE("cli shade --form dome on a disc mask lays the core's form, adding no finding", "[cli][shade][form]") {
    const Mask m = disc(30);
    for (const Compass light : {Compass::NW, Compass::SE, Compass::N})
        for (const bool glint : {false, true}) {
            INFO("light " << int(light) << (glint ? " glint" : ""));
            const AnsiDocument before = blackDoc(m.width(), m.height());
            AnsiDocument doc = before;
            const bwcli::Shaded r = bwcli::shadeDocument(doc, formOptions(FormShape::Dome, m, light, glint));
            const int laid = checkForm(before, doc, m, FormOptions{FormShape::Dome, false, glint}, light);
            CHECK(laid > 100);
            CHECK(int(r.edit.changes.size()) == laid);
            CHECK(r.box.x0 == 3);
            CHECK(r.box.x1 == 32);
        }
    // lit from the north-west: the top left lighter than the bottom right
    AnsiDocument doc = blackDoc(m.width(), m.height());
    bwcli::shadeDocument(doc, formOptions(FormShape::Dome, m));
    CHECK(*rungOf(steel(), doc.canvas.at(11, 6)) > *rungOf(steel(), doc.canvas.at(25, 14)));
}

TEST_CASE("cli shade --form cylinder and plane on a strip and a rectangle", "[cli][shade][form]") {
    struct Case {
        const char* name;
        Mask mask;
    };
    const Case cases[] = {{"strip", rectangle(40, 4)}, {"rectangle", rectangle(24, 10)}};
    for (const Case& k : cases) {
        std::vector<Canvas> shaded;
        for (const FormShape shape : {FormShape::Cylinder, FormShape::Plane, FormShape::Dome}) {
            INFO(k.name << " shape " << int(shape));
            const AnsiDocument before = blackDoc(k.mask.width(), k.mask.height());
            AnsiDocument doc = before;
            bwcli::shadeDocument(doc, formOptions(shape, k.mask));
            CHECK(checkForm(before, doc, k.mask, FormOptions{shape, false, false}, Compass::NW) > 20);
            shaded.push_back(doc.canvas);
        }
        INFO(k.name);
        CHECK(shaded[0] != shaded[1]);
        CHECK(shaded[0] != shaded[2]);
        CHECK(shaded[1] != shaded[2]);
    }
}

TEST_CASE("cli shade --form over a box is the box's form; a mask and a box intersect", "[cli][shade][form]") {
    const Mask rect = rectangle(24, 10);
    const AnsiDocument before = blackDoc(rect.width(), rect.height());
    bwcli::ShadeOptions o = formOptions(FormShape::Plane, rect);
    o.mask.reset();
    o.box = lint::Box{3, 3, 26, 12};
    AnsiDocument byBox = before;
    bwcli::shadeDocument(byBox, o);
    checkForm(before, byBox, rect, FormOptions{FormShape::Plane, false, false}, Compass::NW);

    // the disc's left half
    const Mask m = disc(30);
    Mask half(m.width(), m.height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x <= 18; ++x)
            if (m.contains(x, y))
                half.set(x, y);
    const AnsiDocument black = blackDoc(m.width(), m.height());
    bwcli::ShadeOptions h = formOptions(FormShape::Dome, m);
    h.box = lint::Box{0, 0, 18, m.height() - 1};
    AnsiDocument doc = black;
    const bwcli::Shaded r = bwcli::shadeDocument(doc, h);
    CHECK(checkForm(black, doc, half, FormOptions{}, Compass::NW) > 50);
    CHECK(r.box.x0 == 3);
    CHECK(r.box.x1 == 18);
}

TEST_CASE("cli shade refuses a mask that isn't the canvas's size", "[cli][shade]") {
    AnsiDocument doc = blackDoc(10, 4);
    bwcli::ShadeOptions o = formOptions(FormShape::Dome, rectangle(4, 1));
    try {
        bwcli::shadeDocument(doc, o);
        FAIL("no Failure thrown");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "the mask is 10x7, the canvas 10x4");
    }
}

TEST_CASE("cli shade --despeckle clears a planted speck and leaves a clean gradient", "[cli][shade][despeckle]") {
    // steel climbing a rung every two columns
    const Ramp r = steel();
    AnsiDocument clean;
    clean.ice = true;
    clean.canvas = Canvas(2 * (r.topRung() + 1), 6);
    for (int y = 0; y < clean.canvas.height(); ++y)
        for (int x = 0; x < clean.canvas.width(); ++x)
            clean.canvas.at(x, y) = rungCell(r, x / 2);
    REQUIRE(lint::checkLadder(clean.canvas, r).clean());
    bwcli::ShadeOptions o;
    o.ramp = r;
    o.mode = bwcli::ShadeMode::Despeckle;

    AnsiDocument doc = clean;
    CHECK(bwcli::shadeDocument(doc, o).edit.empty());
    CHECK(doc.canvas == clean.canvas);

    AnsiDocument speck = clean;
    speck.canvas.at(12, 3) = rungCell(r, 9);  // in the rung-6 band
    REQUIRE_FALSE(lint::checkLadder(speck.canvas, r).clean());
    const bwcli::Shaded s = bwcli::shadeDocument(speck, o);
    CHECK(s.edit.changes.size() == 1);
    CHECK(speck.canvas == clean.canvas);

    // outside the box it stays
    AnsiDocument boxed = clean;
    boxed.canvas.at(12, 3) = rungCell(r, 9);
    o.box = lint::Box{0, 0, 9, 5};
    CHECK(bwcli::shadeDocument(boxed, o).edit.empty());
    CHECK(rungOf(r, boxed.canvas.at(12, 3)) == 9);
}

TEST_CASE("cli shade --rim lights the edge facing the light and nothing on the far side", "[cli][shade][rim]") {
    // a w block on black
    const Ramp r = steel();
    AnsiDocument block = blackDoc(24, 12);
    for (int y = 3; y <= 8; ++y)
        for (int x = 5; x <= 18; ++x)
            block.canvas.at(x, y) = rungCell(r, 8);
    bwcli::ShadeOptions o;
    o.ramp = r;
    o.mode = bwcli::ShadeMode::Rim;
    for (const Compass light : {Compass::NW, Compass::SE}) {
        INFO("light " << int(light));
        const bool nw = light == Compass::NW;
        o.light = light;
        AnsiDocument doc = block;
        const bwcli::Shaded s = bwcli::shadeDocument(doc, o);
        CHECK(s.edit.changes.size() > 10);
        for (const CellChange& ch : s.edit.changes) {
            INFO(ch.x << "," << ch.y);
            CHECK((nw ? (ch.y == 3 || ch.x == 5) : (ch.y == 8 || ch.x == 18)));
            CHECK(rungOf(r, doc.canvas.at(ch.x, ch.y)) == 9);
        }
        // the middle of the near edges lit, the far edges not
        CHECK(rungOf(r, doc.canvas.at(11, nw ? 3 : 8)) == 9);
        CHECK(rungOf(r, doc.canvas.at(nw ? 5 : 18, 5)) == 9);
        CHECK(rungOf(r, doc.canvas.at(11, nw ? 8 : 3)) == 8);
        CHECK(rungOf(r, doc.canvas.at(nw ? 18 : 5, 5)) == 8);
    }
    // the same as the editor's rim light over the canvas
    o.light = Compass::NW;
    AnsiDocument doc = block;
    bwcli::shadeDocument(doc, o);
    Canvas want = block.canvas;
    paintRegion(block.canvas, RimLightBrush(r, Compass::NW), {}).apply(want);
    CHECK(doc.canvas == want);
}

TEST_CASE("cli shade --rim over a selection wider and taller than one dab reaches every cell", "[cli][shade][rim]") {
    // a w block 596 by 300 on black: more than two 255-cell dabs across and
    // more than one down
    const Ramp r = steel();
    AnsiDocument block = blackDoc(600, 304);
    for (int y = 2; y <= 301; ++y)
        for (int x = 2; x <= 597; ++x)
            block.canvas.at(x, y) = rungCell(r, 8);
    bwcli::ShadeOptions o;
    o.ramp = r;
    o.mode = bwcli::ShadeMode::Rim;
    o.light = Compass::SE;
    AnsiDocument doc = block;
    const bwcli::Shaded s = bwcli::shadeDocument(doc, o);
    CHECK(s.box.x0 == 0);
    CHECK(s.box.x1 == 599);
    // the far corner's edges, out of the first dab's reach, are lit
    CHECK(rungOf(r, doc.canvas.at(500, 301)) == 9);
    CHECK(rungOf(r, doc.canvas.at(597, 290)) == 9);
    Canvas want = block.canvas;
    paintRegion(block.canvas, RimLightBrush(r, Compass::SE), {}).apply(want);
    CHECK(doc.canvas == want);
}
