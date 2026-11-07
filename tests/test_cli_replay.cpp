// `blockwright-cli replay` (issue #208): a steps file applied through the core
// brushes, shapes, selection and layer operations. The tour in
// data/replay/tour.json takes every kind of step and writes, every run, the
// bytes saved beside it in data/replay/tour/.

#include "loss.h"
#include "read_file.h"
#include "replay.h"

#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/brushes/smudge.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/glyph.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/steps.h"
#include "blockwright/target.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace bw;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

const fs::path kReplay = fs::path(BW_TEST_DATA) / "replay";
const fs::path kSamples = BW_SAMPLES_SOURCE;

bwcli::replay::Outcome replayed(const std::string& json, const fs::path& dir = kReplay) {
    const steps::Script s = steps::parse(json);
    return bwcli::replay::run(s, bwcli::replay::start(s, dir, kSamples));
}

// A blank 20x10 start and these steps.
bwcli::replay::Outcome blank(const std::string& steps) {
    return replayed(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [)" + steps + "]}");
}

// Why the replay stopped, or "(replayed)".
std::string failure(const std::string& json) {
    try {
        replayed(json);
    } catch (const bwcli::Failure& f) {
        return f.message;
    }
    return "(replayed)";
}

std::string onBlank(const std::string& steps) {
    return failure(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [)" + steps + "]}");
}

std::string ansBytes(const project::Project& p) {
    return writeArt(project::toAnsi(p), ArtFormat::Ansi, TextEncoding::Cp437).bytes;
}

Canvas flat(const project::Project& p) { return project::toAnsi(p).canvas; }

const Layer& layerNamed(const project::Project& p, const std::string& name) {
    for (const Layer& l : p.layers.layers())
        if (l.props.name == name)
            return l;
    FAIL("no layer " << name);
    throw;
}

std::vector<std::string> layerNames(const project::Project& p) {
    std::vector<std::string> names;
    for (const Layer& l : p.layers.layers())
        names.push_back(l.props.name);
    return names;
}

} // namespace

TEST_CASE("the tour writes the bytes saved for it, every step a capture", "[replay]") {
    const steps::Script s = steps::parse(bwcli::readFile((kReplay / "tour.json").string()));
    const bwcli::replay::Outcome out = bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples));
    // every step in the tour changes the canvas
    CHECK(out.notes.empty());

    std::set<std::string> saved;
    for (const auto& e : fs::directory_iterator(kReplay / "tour"))
        saved.insert(e.path().filename().string());
    std::set<std::string> written;
    for (const auto& c : out.captures) {
        INFO(c.stem);
        written.insert(c.stem + ".ans");
        const fs::path golden = kReplay / "tour" / (c.stem + ".ans");
        REQUIRE(fs::exists(golden));
        CHECK(ansBytes(c.project) == bwcli::readFile(golden.string()));
    }
    CHECK(written == saved);
    CHECK(out.captures.size() == s.steps.size() + 3);  // three before-after steps
    CHECK(out.captures.front().stem == "01-type");
    CHECK(out.captures.back().stem == "43-delete");
}

TEST_CASE("a replay writes the same bytes every run", "[replay]") {
    const std::string json = bwcli::readFile((kReplay / "tour.json").string());
    const auto a = replayed(json), b = replayed(json);
    REQUIRE(a.captures.size() == b.captures.size());
    for (std::size_t i = 0; i < a.captures.size(); ++i) {
        INFO(a.captures[i].stem);
        CHECK(a.captures[i].stem == b.captures[i].stem);
        CHECK(ansBytes(a.captures[i].project) == ansBytes(b.captures[i].project));
    }
}

TEST_CASE("step numbers are as wide as the last one's, at least two", "[replay]") {
    CHECK(bwcli::replay::stepNumber(3, 9) == "03");
    CHECK(bwcli::replay::stepNumber(12, 40) == "12");
    CHECK(bwcli::replay::stepNumber(7, 120) == "007");
}

TEST_CASE("keyboard text goes in at its cell and wraps to the start column", "[replay]") {
    const auto out = blank(R"({"name": "say", "action": "keyboard", "fg": 14, "bg": 1, "at": [3, 2], "text": "hi\nyo"})");
    REQUIRE(out.captures.size() == 1);
    const Canvas c = flat(out.captures[0].project);
    CHECK(c.at(3, 2).ch == U'h');
    CHECK(c.at(4, 2).ch == U'i');
    CHECK(c.at(3, 3).ch == U'y');
    CHECK(c.at(4, 3).ch == U'o');
    CHECK(c.at(3, 2).fg == Color::indexed(14));
    CHECK(c.at(3, 2).bg == Color::indexed(1));
    CHECK(c.at(5, 2).ch == U' ');
}

TEST_CASE("a filled rectangle's pixels take the pen, the right button its background", "[replay]") {
    const auto out = blank(R"({"name": "a", "action": "rectangle", "filled": true, "fg": 12, "from": [0, 0], "to": [3, 3]},
                              {"name": "b", "action": "rectangle", "filled": true, "bg": 9, "button": "right",
                               "from": [10, 0], "to": [11, 1]})");
    const Canvas c = flat(out.captures[1].project);
    for (int x = 0; x <= 3; ++x)
        for (int y = 0; y <= 1; ++y)
            CHECK(halfBlocks(c.at(x, y)) == HalfPair{12, 12});
    CHECK(halfBlocks(c.at(10, 0)) == HalfPair{9, 9});
    CHECK(halfBlocks(c.at(4, 0)) != HalfPair{12, 12});
}

TEST_CASE("a copied selection pastes where the step says", "[replay]") {
    const auto out = blank(R"({"name": "say", "action": "keyboard", "at": [0, 0], "text": "ab"},
                              {"name": "pick", "action": "select", "rect": [0, 0, 2, 1]},
                              {"name": "copy", "action": "copy"},
                              {"name": "drop", "action": "paste", "at": [5, 4]})");
    const Canvas c = flat(out.captures.back().project);
    CHECK(c.at(5, 4).ch == U'a');
    CHECK(c.at(6, 4).ch == U'b');
    CHECK(c.at(0, 0).ch == U'a');
}

TEST_CASE("layers are added, named, reordered, moved and changed as the steps say", "[replay]") {
    const auto out = blank(R"({"name": "add", "action": "newCellsLayer", "rename": "ink"},
                              {"name": "dots", "action": "newPixelLayer"},
                              {"name": "down", "action": "lowerLayer"},
                              {"name": "to-ink", "action": "setActiveLayer", "layer": "ink"},
                              {"name": "mark", "action": "keyboard", "at": [1, 1], "text": "x"},
                              {"name": "shift", "action": "move", "by": [3, 2]},
                              {"name": "props", "action": "layerProperties", "rename": "ink2", "visible": false,
                               "locked": true, "alphaLocked": true})");
    const project::Project& p = out.captures.back().project;
    const auto names = layerNames(p);
    REQUIRE(names.size() == 3);
    // bottom first: the start's layer, the pixels layer lowered under the ink
    CHECK(names[1] == "Layer 1");
    CHECK(names[2] == "ink2");
    CHECK(layerNamed(p, "Layer 1").props.kind == LayerKind::Pixels);
    const Layer& ink = layerNamed(p, "ink2");
    CHECK(ink.props.x == 3);
    CHECK(ink.props.y == 2);
    CHECK_FALSE(ink.props.visible);
    CHECK(ink.props.locked);
    CHECK(ink.props.alphaLocked);
    CHECK(ink.cells.at(1, 1).ch == U'x');
    // shown before it was hidden, one cell in and moved by (3, 2)
    CHECK(flat(out.captures[5].project).at(4, 3).ch == U'x');
    CHECK(flat(out.captures.back().project).at(4, 3).ch == U' ');
}

namespace {

// What the brush paints from `before` with these settings, as the core paints
// it, is what the replay left in `after`; and the symmetry is what makes it:
// the stroke alone paints less.
void paintsAsCore(const Canvas& before, const Canvas& after, const Brush& brush, int size, Symmetry sym,
                  const std::vector<Dab>& dabs) {
    BrushSettings settings;
    settings.size = size;
    StrokeOptions opts;
    opts.symmetry = sym;
    const CellEdit edit = paintStroke(before, brush, settings, dabs, opts);
    REQUIRE_FALSE(edit.empty());
    Canvas want = before;
    edit.apply(want);
    CHECK(after == want);
    CHECK(paintStroke(before, brush, settings, dabs, {}).changes.size() < edit.changes.size());
}

const Canvas& cellsOf(const bwcli::replay::Capture& c) { return c.project.layers.layers()[0].cells; }

} // namespace

TEST_CASE("form under mirror symmetry paints what the core brush paints", "[replay]") {
    const auto out = blank(R"({"name": "form", "action": "form", "ramp": "steel", "size": 3, "symmetry": "mirrorX",
                              "form": {"shape": "dome", "rim": true}, "points": [[2, 3], [6, 3], [6, 6], [2, 6]],
                              "capture": "before-after"})");
    REQUIRE(out.captures.size() == 2);
    Symmetry sym;
    sym.mode = SymmetryMode::MirrorX;
    FormOptions form;
    form.rim = true;
    paintsAsCore(cellsOf(out.captures[0]), cellsOf(out.captures[1]), FormShadeBrush(*findRamp("steel"), form), 3, sym,
                 {{2, 3, 1.0}, {6, 3, 1.0}, {6, 6, 1.0}, {2, 6, 1.0}});
}

// The cells one before-after step changed.
int changedCells(const bwcli::replay::Outcome& out) {
    REQUIRE(out.captures.size() == 2);
    const Canvas& before = cellsOf(out.captures[0]);
    const Canvas& after = cellsOf(out.captures[1]);
    int n = 0;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            n += !(before.at(x, y) == after.at(x, y));
    return n;
}

// One stroke on a blank start, captured before and after.
bwcli::replay::Outcome stroke(const std::string& options, const std::string& points) {
    return blank(R"({"name": "s", "capture": "before-after", )" + options + R"(, "points": )" + points + "}");
}

TEST_CASE("pressure to size scales a stroke's footprint by its points' pressure, as a stylus's (#382)",
          "[replay]") {
    const std::string glyph = R"("action": "glyph", "size": 4)";
    // 4 x 0.25 is one cell, 4 x 0.5 two, a full press four
    CHECK(changedCells(stroke(glyph + R"(, "pressure": "size")", "[[10, 5, 0.25]]")) == 1);
    CHECK(changedCells(stroke(glyph + R"(, "pressure": "size")", "[[10, 5, 0.5]]")) == 4);
    CHECK(changedCells(stroke(glyph + R"(, "pressure": "size")", "[[10, 5, 1.0]]")) == 16);
    // a point at half pressure paints what the size it scales to paints
    CHECK(cellsOf(stroke(glyph + R"(, "pressure": "size")", "[[10, 5, 0.5]]").captures[1]) ==
          cellsOf(stroke(R"("action": "glyph", "size": 2)", "[[10, 5, 0.5]]").captures[1]));
    // pressing harder where the pen is grows the dab, and easing off there
    // leaves it at the peak, as in the editor
    CHECK(changedCells(stroke(glyph + R"(, "pressure": "size")", "[[10, 5, 0.25], [10, 5, 1.0], [10, 5, 0.5]]")) ==
          16);
    // with no pressure, as at the start, it is ignored
    for (const char* none : {"", R"(, "pressure": "none")"}) {
        INFO("options" << none);
        CHECK(changedCells(stroke(glyph + none, "[[10, 5, 0.25]]")) == 16);
        CHECK(changedCells(stroke(glyph + none, "[[10, 5, 1.0]]")) == 16);
    }
}

TEST_CASE("pressure to strength scales a stroke's strength by its points' pressure, as a stylus's (#382)",
          "[replay]") {
    const std::string shade = R"("action": "rampShade", "ramp": "steel")";
    const auto after = [&](const std::string& options, const char* points) {
        return cellsOf(stroke(shade + options, points).captures[1]);
    };
    // half the pressure paints what half the strength does, not what full does
    const Canvas half = after(R"(, "pressure": "strength")", "[[3, 3, 0.5]]");
    CHECK(half == after(R"(, "strength": 0.5)", "[[3, 3, 0.5]]"));
    CHECK(half != after(R"(, "pressure": "strength")", "[[3, 3, 1.0]]"));
    CHECK(after(R"(, "pressure": "strength")", "[[3, 3, 1.0]]") == after("", "[[3, 3, 1.0]]"));
    // the size isn't scaled
    CHECK(changedCells(stroke(shade + R"(, "size": 3, "pressure": "strength")", "[[3, 3, 0.5]]")) == 9);
    // with none, a light touch shades as a full one
    CHECK(after(R"(, "pressure": "none")", "[[3, 3, 0.5]]") == after("", "[[3, 3, 1.0]]"));
}

TEST_CASE("the pressure mode a step sets holds for the steps after it (#382)", "[replay]") {
    const auto out = blank(R"({"name": "pick", "action": "glyph", "size": 4, "pressure": "size"},
                              {"name": "light", "action": "glyph", "points": [[3, 3, 0.25]]},
                              {"name": "off", "action": "glyph", "pressure": "none", "points": [[12, 3, 0.25]]},
                              {"name": "still-off", "action": "glyph", "points": [[3, 7, 0.25]]})");
    REQUIRE(out.captures.size() == 4);
    const auto set = [](const Canvas& c) {
        int n = 0;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                n += c.at(x, y).ch != U' ';
        return n;
    };
    CHECK(set(cellsOf(out.captures[1])) == 1);
    CHECK(set(cellsOf(out.captures[2])) == 1 + 16);
    CHECK(set(cellsOf(out.captures[3])) == 1 + 16 + 16);
}

namespace {

// The steel rungs of a row of cells, as digits.
std::string steelRungs(const Canvas& c, int y = 0) {
    std::string r;
    for (int x = 0; x < c.width(); ++x) {
        const auto rung = rungOf(*findRamp("steel"), c.at(x, y));
        r += rung ? char('0' + *rung) : '?';
    }
    return r;
}

// What the core stroke (Stroke::stamp, the editor's) paints from `before`.
Canvas asCore(const Canvas& before, const Brush& brush, const BrushSettings& settings, const std::vector<Dab>& dabs) {
    Canvas c = before;
    paintStroke(before, brush, settings, dabs).apply(c);
    return c;
}

} // namespace

TEST_CASE("a softer point on the dab's own cell carries nothing on to the smudge after it, as in the editor (#382)",
          "[replay]") {
    // The editor's tool drops [1, 0, 0.125] (BrushTool::extend); were it
    // stamped, smudge would carry it on and paint the next cell a rung
    // lighter, 877 for the editor's 876.
    const auto run = [](const char* points) {
        return replayed(std::string(R"({"version": 1, "start": {"blank": [3, 1]}, "ice": true, "steps": [
            {"name": "light", "action": "glyph", "glyph": "█", "fg": 7, "points": [[0, 0]]},
            {"name": "shade", "action": "glyph", "glyph": "▒", "fg": 7, "bg": 8, "points": [[1, 0], [2, 0]]},
            {"name": "smear", "action": "smudge", "ramp": "steel", "size": 1, "strength": 1,
             "pressure": "strength", "points": )") +
                        points + "}]}");
    };
    const auto repeat = run("[[0, 0, 1], [1, 0, 0.25], [1, 0, 0.125], [2, 0, 0.75]]");
    REQUIRE(repeat.captures.size() == 3);
    CHECK(steelRungs(cellsOf(repeat.captures[1])) == "866");
    CHECK(steelRungs(cellsOf(repeat.captures[2])) == "876");
    CHECK(cellsOf(repeat.captures[2]) ==
          cellsOf(run("[[0, 0, 1], [1, 0, 0.25], [2, 0, 0.75]]").captures[2]));
}

namespace {

// The core brush a replay step of `action` makes from the Tool Options it
// starts with (the steel ramp; every other option as it starts), as the
// editor's tools make it: written out here, not taken from replay.
std::unique_ptr<Brush> defaultBrush(const std::string& action) {
    const Ramp steel = *findRamp("steel");
    const Pen pen;
    const Compass light = BrushSettings{}.lightDirection;
    if (action == "shade")
        return std::make_unique<ShadeBrush>(pen, false);
    if (action == "rampShade")
        return std::make_unique<RampShadeBrush>(steel, ShadeDirection::Lighten, kRungsPerStep);
    if (action == "form")
        return std::make_unique<FormShadeBrush>(steel, FormOptions{});
    if (action == "soft")
        return std::make_unique<SoftShadeBrush>(steel, ShadeDirection::Lighten, 0.0);
    if (action == "despeckle")
        return std::make_unique<DespeckleBrush>(steel);
    if (action == "smudge")
        return std::make_unique<SmudgeBrush>(steel);
    if (action == "glyph") {
        Cell c;
        c.ch = kFullBlock;
        c.fg = pen.fg;
        c.bg = pen.bg;
        return std::make_unique<GlyphBrush>(c);
    }
    if (action == "colorize")
        return std::make_unique<ColorizeBrush>(pen.fg, std::nullopt);
    if (action == "outline")
        return std::make_unique<OutlineBrush>(steel, OutlineShape::Ramp);
    if (action == "rimLight")
        return std::make_unique<RimLightBrush>(steel, light, false);
    if (action == "texture")
        return std::make_unique<TextureBrush>(steel, TextureOptions{}, light);
    if (action == "spray")
        return std::make_unique<SprayBrush>(steel, SprayOptions{}, pen.fg, ShadeDirection::Lighten);
    FAIL("no brush " << action);
    throw;
}

} // namespace

TEST_CASE("a point on the dab's own cell is a dab only if it presses harder with pressure on, anywhere in a stroke "
          "(#382)",
          "[replay]") {
    // BrushTool::extend's rule, wherever the repeat falls: first, mid-stroke
    // or last. With pressure on a harder press is kept and grows the dab;
    // a softer one, or any repeat with none, is dropped. Each stroke is held
    // to the core stroke (Stroke::stamp) of the dabs the editor keeps,
    // listed by hand.
    const std::vector<Dab> softer{{30, 17, 0.6}, {33, 15, 0.6}, {36, 17, 1.0}};
    const std::vector<Dab> harder{{9, 17, 0.6}, {12, 15, 0.4}, {12, 15, 0.9}, {15, 17, 1.0}};
    const std::vector<Dab> harderDropped{{9, 17, 0.6}, {12, 15, 0.4}, {15, 17, 1.0}};
    for (const char* brush : {"shade", "rampShade", "form", "soft", "despeckle", "smudge", "glyph", "colorize",
                              "outline", "rimLight", "texture", "spray"}) {
        for (const auto& [mode, to] : {std::pair{"none", PressureTo::Nothing}, std::pair{"size", PressureTo::Size},
                                       std::pair{"strength", PressureTo::Strength}}) {
            INFO(brush << " pressure " << mode);
            const auto run = [&](const char* points) {
                return replayed(std::string(R"({"version": 1, "start": {"file": "sphere-steel.ans"}, "steps": [
                    {"name": "s", "capture": "before-after", "action": ")") +
                                    brush + R"(", "size": 5, "strength": 0.5, "ramp": "steel", "pressure": ")" +
                                    mode + R"(", "points": )" + points + "}]}",
                                fs::path(BW_TEST_DATA) / "brushes");
            };
            const auto core = [&](const bwcli::replay::Outcome& out, const std::vector<Dab>& dabs) {
                REQUIRE(out.captures.size() == 2);
                const project::Project& p = out.captures[0].project;
                StrokeOptions opts = layerStrokeOptions(p.layers.layers()[0]);
                opts.ice = p.ice;
                Canvas c = cellsOf(out.captures[0]);
                paintStroke(c, *defaultBrush(brush), BrushSettings{5, Tip::Square, 0.5, to}, dabs, opts).apply(c);
                return c;
            };
            const bool on = to != PressureTo::Nothing;
            // softer repeats at the start, mid-stroke and at the end
            const auto soft = run("[[30, 17, 0.6], [30, 17, 0.4], [33, 15, 0.6], [33, 15, 0.3], [33, 15, 0.6], "
                                  "[36, 17, 1.0], [36, 17, 0.5]]");
            CHECK(cellsOf(soft.captures[1]) == core(soft, softer));
            // a harder repeat mid-stroke, on the sphere's dark side where a
            // stronger dab has rungs to lighten: a dab with pressure on,
            // dropped with none
            const auto hard = run("[[9, 17, 0.6], [12, 15, 0.4], [12, 15, 0.9], [15, 17, 1.0]]");
            CHECK(cellsOf(hard.captures[1]) == core(hard, on ? harder : harderDropped));
        }
    }
    // and a kept harder repeat paints what its pressure says: size 5 at 0.2
    // is one cell, at 1.0 five by five
    CHECK(changedCells(stroke(R"("action": "glyph", "size": 5, "pressure": "size")", "[[10, 5, 0.2], [10, 5, 1.0]]")) ==
          25);
}

TEST_CASE("a harder press on the dab's own cell shades as hard as it presses, not as the first touch (#382)",
          "[replay]") {
    // strength 1 under pressure to strength, from a blank cell (rung 0): a
    // quarter press lightens it one rung, a full one kRungsPerStep (4). The
    // harder repeat is kept, so the cell is left at rung 4, not at the light
    // touch's 1.
    const std::string shade = R"("action": "rampShade", "ramp": "steel", "strength": 1, "pressure": "strength")";
    const auto rungs = [&](const char* points) {
        const auto out = stroke(shade, points);
        return std::string{steelRungs(cellsOf(out.captures[0]), 3)[3], steelRungs(cellsOf(out.captures[1]), 3)[3]};
    };
    CHECK(rungs("[[3, 3, 0.25]]") == "01");
    CHECK(rungs("[[3, 3, 1]]") == "04");
    CHECK(rungs("[[3, 3, 0.25], [3, 3, 1]]") == "04");
}

TEST_CASE("zero pressure scales the size to one cell and the strength to nothing, as Stroke::stamp does (#382)",
          "[replay]") {
    CHECK(changedCells(stroke(R"("action": "glyph", "size": 4, "pressure": "size")", "[[10, 5, 0]]")) == 1);
    const auto out = stroke(R"("action": "rampShade", "ramp": "steel", "pressure": "strength")", "[[3, 3, 0]]");
    const Canvas& before = cellsOf(out.captures[0]);
    const RampShadeBrush shade(*findRamp("steel"), ShadeDirection::Lighten, kRungsPerStep);
    const Canvas core = asCore(before, shade, BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength}, {{3, 3, 0.0}});
    CHECK(cellsOf(out.captures[1]) == core);
    // strength 0 at a full press paints the same
    CHECK(core == asCore(before, shade, BrushSettings{1, Tip::Square, 0.0, PressureTo::Nothing}, {{3, 3, 1.0}}));
}

TEST_CASE("a size scaled by pressure is rounded to the nearest cell, halves away from zero, as Stroke::stamp does "
          "(#382)",
          "[replay]") {
    const auto cells = [](int size, const char* p) {
        return changedCells(stroke(R"("action": "glyph", "pressure": "size", "size": )" + std::to_string(size),
                                   std::string("[[10, 5, ") + p + "]]"));
    };
    CHECK(cells(5, "0.5") == 3 * 3);   // 2.5 -> 3
    CHECK(cells(5, "0.3") == 2 * 2);   // 1.5 -> 2
    CHECK(cells(5, "0.29") == 1);      // 1.45 -> 1
    CHECK(cells(5, "0.7") == 4 * 4);   // 3.5 -> 4
    CHECK(cells(5, "0.69") == 3 * 3);  // 3.45 -> 3
    CHECK(cells(3, "0.5") == 2 * 2);   // 1.5 -> 2
    // the cells are the core's footprint at that size, a round tip's too
    const auto out = stroke(R"("action": "glyph", "tip": "round", "size": 5, "pressure": "size")", "[[10, 5, 0.7]]");
    const Canvas& before = cellsOf(out.captures[0]);
    const Canvas& after = cellsOf(out.captures[1]);
    std::set<std::pair<int, int>> painted;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            if (!(before.at(x, y) == after.at(x, y)))
                painted.insert({x, y});
    const auto tip = footprint(10, 5, 4, Tip::Round, before.width(), before.height());
    CHECK(painted == std::set<std::pair<int, int>>(tip.begin(), tip.end()));
    for (const int other : {3, 5}) {
        const auto t = footprint(10, 5, other, Tip::Round, before.width(), before.height());
        CHECK(painted != std::set<std::pair<int, int>>(t.begin(), t.end()));
    }
}

TEST_CASE("pressure multiplies a strength below 1, as Stroke::stamp does (#382)", "[replay]") {
    const std::string shade = R"("action": "rampShade", "ramp": "steel")";
    const auto out = stroke(shade + R"(, "strength": 0.5, "pressure": "strength")", "[[3, 3, 0.5]]");
    const Canvas& before = cellsOf(out.captures[0]);
    const RampShadeBrush brush(*findRamp("steel"), ShadeDirection::Lighten, kRungsPerStep);
    // 0.5 x 0.5: a quarter strength, one rung of the four a full one takes
    const Canvas quarter = asCore(before, brush, BrushSettings{1, Tip::Square, 0.25, PressureTo::Nothing}, {{3, 3, 1.0}});
    CHECK(cellsOf(out.captures[1]) == quarter);
    CHECK(cellsOf(out.captures[1]) ==
          asCore(before, brush, BrushSettings{1, Tip::Square, 0.5, PressureTo::Strength}, {{3, 3, 0.5}}));
    CHECK(rungOf(*findRamp("steel"), cellsOf(out.captures[1]).at(3, 3)) == 1);
    // not what half strength or half pressure alone paints
    CHECK(cellsOf(out.captures[1]) != cellsOf(stroke(shade + R"(, "strength": 0.5)", "[[3, 3, 1]]").captures[1]));
}

TEST_CASE("a point without a pressure presses fully under either pressure mode, as a mouse does (#382)",
          "[replay]") {
    CHECK(changedCells(stroke(R"("action": "glyph", "size": 4, "pressure": "size")", "[[10, 5]]")) == 16);
    const std::string shade = R"("action": "rampShade", "ramp": "steel", "strength": 0.5)";
    const auto out = stroke(shade + R"(, "pressure": "strength")", "[[3, 3]]");
    const RampShadeBrush brush(*findRamp("steel"), ShadeDirection::Lighten, kRungsPerStep);
    CHECK(cellsOf(out.captures[1]) == asCore(cellsOf(out.captures[0]), brush,
                                             BrushSettings{1, Tip::Square, 0.5, PressureTo::Strength}, {{3, 3}}));
    CHECK(rungOf(*findRamp("steel"), cellsOf(out.captures[1]).at(3, 3)) == 2);
}

TEST_CASE("outline under radial symmetry paints what the core brush paints", "[replay]") {
    const auto out = replayed(R"({"version": 1, "start": {"file": "sphere-steel.ans"}, "steps": [
        {"name": "outline", "action": "outline", "ramp": "steel", "symmetry": {"mode": "radial", "folds": 4},
         "points": [[36, 1], [40, 1], [44, 2]], "capture": "before-after"}]})",
                              fs::path(BW_TEST_DATA) / "brushes");
    REQUIRE(out.captures.size() == 2);
    Symmetry sym;
    sym.mode = SymmetryMode::Radial;
    sym.folds = 4;
    paintsAsCore(cellsOf(out.captures[0]), cellsOf(out.captures[1]), OutlineBrush(*findRamp("steel")), 1, sym,
                 {{36, 1, 1.0}, {40, 1, 1.0}, {44, 2, 1.0}});
}

TEST_CASE("a full block paints a pixels layer, as the glyph tool does", "[replay]") {
    const auto out = blank(R"({"name": "dots", "action": "newPixelLayer"},
                              {"name": "draw", "action": "glyph", "fg": 10, "points": [[1, 1]]})");
    CHECK(halfBlocks(flat(out.captures.back().project).at(1, 1)) == HalfPair{10, 10});
}

TEST_CASE("a sample start makes the variant asked for", "[replay]") {
    const auto day = replayed(R"({"version": 1, "start": {"sample": "01-layers-showcase", "variant": "Day"},
                                  "steps": [{"name": "all", "action": "selectAll"}]})");
    const auto saved = replayed(R"({"version": 1, "start": {"sample": "01-layers-showcase"},
                                    "steps": [{"name": "all", "action": "selectAll"}]})");
    CHECK(layerNamed(day.captures[0].project, "day sky").props.visible);
    CHECK_FALSE(layerNamed(saved.captures[0].project, "day sky").props.visible);
}

TEST_CASE("a radial centre off the canvas stops the replay, as the editor never draws about one", "[replay]") {
    const auto radial = [](const std::string& centre) {
        return onBlank(R"({"name": "a", "action": "glyph", "symmetry": {"mode": "radial", "folds": 4, "centre": )" +
                       centre + R"(}, "points": [[3, 5]]})");
    };
    CHECK(radial("[-50, 5]") == "step 1 (a): symmetry centre (-50, 5) is off the 20 x 10 canvas");
    CHECK(radial("[20, 5]") == "step 1 (a): symmetry centre (20, 5) is off the 20 x 10 canvas");
    CHECK(radial("[3, 10]") == "step 1 (a): symmetry centre (3, 10) is off the 20 x 10 canvas");
    CHECK(radial("[19, 9]") == "(replayed)");
    CHECK(radial("[0, 5]") == "(replayed)");
    // checked on the step that sets it, a step with no points too
    CHECK(onBlank(R"({"name": "set", "action": "glyph", "symmetry": {"mode": "radial", "centre": [0, 99]}})") ==
          "step 1 (set): symmetry centre (0, 99) is off the 20 x 10 canvas");
}

TEST_CASE("move refuses a hidden layer, as the Move tool does", "[replay]") {
    CHECK(onBlank(R"({"name": "add", "action": "newCellsLayer"},
                     {"name": "dot", "action": "glyph", "points": [[1, 1]]},
                     {"name": "hide", "action": "layerProperties", "visible": false},
                     {"name": "shift", "action": "move", "by": [2, 0]})") ==
          "step 4 (shift): the active layer \"Layer 1\" is hidden");
    CHECK_THAT(onBlank(R"({"name": "lock", "action": "layerProperties", "locked": true},
                          {"name": "shift", "action": "move", "by": [2, 0]})"),
               ContainsSubstring("step 2 (shift): the active layer") && ContainsSubstring("is locked"));
}

TEST_CASE("move on a reference layer moves its picture by canvas pixels, as the Move tool's drag does", "[replay]") {
    const steps::Script script = steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [
        {"name": "to-ref", "action": "setActiveLayer", "layer": "sketch"},
        {"name": "shift", "action": "move", "by": [-7, 12]}]})");
    project::Project p;
    p.layers = LayerStack::fromCanvas(Canvas(20, 10));
    Layer ref = makeLayer(LayerKind::Reference, 20, 10, "sketch");
    ref.props.reference = ReferenceImage{};
    ref.props.reference->x = 5;
    ref.props.reference->y = -3;
    ref.props.reference->width = 160;
    ref.props.reference->height = 160;
    p.layers.add(std::move(ref), 1);

    const auto out = bwcli::replay::run(script, p);
    const Layer& moved = layerNamed(out.captures.back().project, "sketch");
    CHECK(moved.props.reference->x == -2);
    CHECK(moved.props.reference->y == 9);
    CHECK(moved.props.reference->width == 160);
    CHECK(moved.props.reference->height == 160);
    // the layer's own offset is not what the drag changes
    CHECK(moved.props.x == 0);
    CHECK(moved.props.y == 0);

    // with no picture there is nothing to move
    p.layers[1].props.reference.reset();
    try {
        bwcli::replay::run(script, p);
        FAIL("a reference layer with no picture moved");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "step 2 (shift): the active layer \"sketch\" is a reference layer with no picture");
    }
}

TEST_CASE("keyboard text is NFC-composed first, as keyboard mode types it", "[replay]") {
    const auto out = blank(R"({"name": "say", "action": "keyboard", "at": [2, 2], "text": "e\u0301t\u00e9"})");
    const Canvas c = flat(out.captures[0].project);
    CHECK(c.at(2, 2).ch == U'\u00e9');
    CHECK(c.at(3, 2).ch == U't');
    CHECK(c.at(4, 2).ch == U'\u00e9');
    CHECK(c.at(5, 2).ch == U' ');
}

TEST_CASE("keyboard text follows the document's typing policy, as the editor's typing does (#350 slice 2)",
          "[replay]") {
    const steps::Script script = steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [
        {"name": "star", "action": "keyboard", "at": [0, 0], "text": "★"},
        {"name": "wide", "action": "keyboard", "at": [0, 1], "text": "a中b"},
        {"name": "mark", "action": "keyboard", "at": [0, 2], "text": "q\u0302x"}]})");
    const project::Project blankStart = bwcli::replay::start(script, kReplay, kSamples);
    const auto with = [&](Target target, TextEncoding encoding) {
        project::Project p = blankStart;
        p.target = target;
        p.ansEncoding = encoding;
        return p;
    };

    // ANSI with a CP437 .ans: ★ has no CP437 code, as the editor refuses it
    try {
        bwcli::replay::run(script, with(Target::Ansi, TextEncoding::Cp437));
        FAIL("ANSI with a CP437 .ans typed ★");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message ==
              "step 1 (star): the text holds a character keyboard mode can't type (U+2605; target: ANSI, CP437)");
    }

    // the IRC target or a UTF-8 .ans types it, 中 as a pair and q and U+0302 as one cluster
    const struct {
        Target target;
        TextEncoding encoding;
    } unicode[] = {{Target::Ansi, TextEncoding::Utf8}, {Target::Irc, TextEncoding::Cp437}, {Target::Irc, TextEncoding::Utf8}};
    for (const auto& u : unicode) {
        INFO(std::string(targetName(u.target)) << (u.encoding == TextEncoding::Utf8 ? " UTF-8" : " CP437"));
        const auto out = bwcli::replay::run(script, with(u.target, u.encoding));
        REQUIRE(out.captures.size() == 3);
        CHECK(out.notes.empty());
        const Canvas c = flat(out.captures.back().project);
        CHECK(c.at(0, 0).ch == U'★');
        CHECK(c.at(1, 0).ch == U' ');
        CHECK(c.at(0, 1).ch == U'a');
        CHECK(c.at(1, 1).ch == U'中');
        CHECK(c.at(2, 1).ch.isContinuation());
        CHECK(c.at(3, 1).ch == U'b');
        CHECK(c.at(0, 2).ch.isCluster());
        const GlyphCodePoints q = codePoints(c.at(0, 2).ch);
        CHECK(std::u32string(q.begin(), q.end()) == U"q\u0302");
        CHECK(c.at(1, 2).ch == U'x');
    }

    // and 中, on a blank start (ANSI with a CP437 .ans), is refused too
    CHECK_THAT(failure(R"({"version": 1, "start": {"blank": [20, 10]},
                           "steps": [{"name": "wide", "action": "keyboard", "at": [0, 0], "text": "中"}]})"),
               ContainsSubstring("step 1 (wide): the text holds a character keyboard mode can't type (U+4E2D"));
}

TEST_CASE("a keyboard line is one commit, as the editor's input-method commit is (#366 review)", "[replay]") {
    const auto onIrc = [](const std::string& steps) {
        const steps::Script script =
            steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [)" + steps + "]}");
        project::Project p = bwcli::replay::start(script, kReplay, kSamples);
        p.target = Target::Irc;
        p.ansEncoding = TextEncoding::Cp437;
        return bwcli::replay::run(script, p);
    };
    const std::string seed = R"({"name": "seed", "action": "keyboard", "at": [0, 0], "text": "q"})";

    // a mark leading more text is refused, as replaceText refuses it, and q is left alone
    try {
        onIrc(seed + R"(, {"name": "mixed", "action": "keyboard", "at": [1, 0], "text": "̂x"})");
        FAIL("a leading mark joined q");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "step 2 (mixed): the text starts with a mark (U+0302) and has more after it on its "
                           "line; keyboard mode joins marks only typed alone");
    }

    // a line of marks alone joins the cell before the cursor, as typeMarks does
    const auto out = onIrc(seed + R"(, {"name": "hat", "action": "keyboard", "at": [1, 0], "text": "̂"})");
    REQUIRE(out.captures.size() == 2);
    CHECK(out.notes.empty());
    const Canvas c = flat(out.captures.back().project);
    REQUIRE(c.at(0, 0).ch.isCluster());
    const GlyphCodePoints q = codePoints(c.at(0, 0).ch);
    CHECK(std::u32string(q.begin(), q.end()) == U"q̂");
    CHECK(c.at(1, 0).ch == U' ');
}

TEST_CASE("a step that can't be applied stops the replay, named", "[replay]") {
    struct Case {
        const char* what;
        std::string steps;
        const char* names;
        const char* says;
    };
    const Case cases[] = {
        {"a point off the canvas", R"({"name": "far", "action": "shade", "points": [[20, 0]]})", "step 1 (far)", ""},
        {"a pixel off the canvas", R"({"name": "far", "action": "halfBlock", "points": [[0, 20]]})", "step 1 (far)",
         ""},
        {"a selection off the canvas", R"({"name": "sel", "action": "select", "rect": [18, 0, 20, 1]})",
         "step 1 (sel)", ""},
        {"a flip with nothing selected", R"({"name": "flip", "action": "flipHorizontal"})", "step 1 (flip)",
         "selection"},
        {"a paste with nothing copied", R"({"name": "drop", "action": "paste", "at": [0, 0]})", "step 1 (drop)", ""},
        {"a hidden layer",
         R"({"name": "hide", "action": "layerProperties", "visible": false},
            {"name": "draw", "action": "glyph", "points": [[1, 1]]})",
         "step 2 (draw)", "hidden"},
        {"a locked layer",
         R"({"name": "lock", "action": "layerProperties", "locked": true},
            {"name": "draw", "action": "fill", "at": [1, 1]})",
         "step 2 (draw)", "locked"},
        {"a glyph a pixels layer can't hold",
         R"({"name": "dots", "action": "newPixelLayer"},
            {"name": "draw", "action": "glyph", "glyph": "*", "points": [[1, 1]]})",
         "step 2 (draw)", "pixels"},
        {"a block turned on its side on a pixels layer",
         R"({"name": "dots", "action": "newPixelLayer"},
            {"name": "draw", "action": "glyph", "glyph": "\u2580", "symmetry": {"mode": "radial", "folds": 4},
             "points": [[1, 1]]})",
         "step 2 (draw)", "pixels"},
        {"a shade on a pixels layer",
         R"({"name": "dots", "action": "newPixelLayer"}, {"name": "draw", "action": "smudge", "points": [[1, 1]]})",
         "step 2 (draw)", "shades"},
        {"deleting the only layer", R"({"name": "gone", "action": "deleteLayer"})", "step 1 (gone)", ""},
        {"raising the top layer", R"({"name": "up", "action": "raiseLayer"})", "step 1 (up)", ""},
        {"an unknown layer", R"({"name": "go", "action": "setActiveLayer", "layer": "nowhere"})", "step 1 (go)",
         "nowhere"},
    };
    for (const Case& c : cases) {
        INFO(c.what);
        const std::string why = onBlank(c.steps);
        CHECK_THAT(why, ContainsSubstring(c.names));
        CHECK_THAT(why, ContainsSubstring(c.says));
    }

    CHECK_THAT(failure(R"({"version": 1, "start": {"file": "missing.ans"}, "steps": [{"name": "a", "action": "selectAll"}]})"),
               ContainsSubstring("start"));
    CHECK_THAT(failure(R"({"version": 1, "start": {"sample": "99-none"}, "steps": [{"name": "a", "action": "selectAll"}]})"),
               ContainsSubstring("99-none"));
    CHECK_THAT(failure(R"({"version": 1, "start": {"sample": "01-layers-showcase", "variant": "Dusk"},
                           "steps": [{"name": "a", "action": "selectAll"}]})"),
               ContainsSubstring("Dusk"));
}

TEST_CASE("a core error a step raises is the step's failure, not an abort", "[replay]") {
    // a valid project whose layer ids are used up: newCellsLayer's add()
    // throws LayerError, which no step check foresees
    const steps::Script script = steps::parse(
        R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [{"name": "add", "action": "newCellsLayer"}]})");
    project::Project p = bwcli::replay::start(script, kReplay, kSamples);
    p.layers.reserveIds(0);
    REQUIRE(p.layers.nextId() == 0);
    try {
        bwcli::replay::run(script, p);
        FAIL("a layer was added with the ids exhausted");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "step 1 (add): layer ids exhausted");
    }
}

TEST_CASE("a read's cleared attribute bits are worded as the app's status bar words them", "[replay][cli]") {
    // #370: "Opened %1: cleared attribute bits no earlier build wrote from %2"
    CHECK(bwcli::projectReadNotes({}).empty());
    CHECK(bwcli::projectReadNotes({.strayAttributes = 1}) ==
          std::vector<std::string>{"cleared attribute bits no earlier build wrote from 1 cell"});
    CHECK(bwcli::projectReadNotes({.strayAttributes = 3}) ==
          std::vector<std::string>{"cleared attribute bits no earlier build wrote from 3 cells"});
}

TEST_CASE("a project start before 5.0 notes the attribute bits its read cleared", "[replay]") {
    // #370: data/projects/stray-attrs is a 4.0 project whose first two cells
    // hold bits no build before 5.0 wrote (bit 5; strikethrough with blink)
    // and whose third holds blink alone
    const std::string dot = R"(, "steps": [{"name": "dot", "action": "glyph", "points": [[5, 0]]}]})";
    std::vector<std::string> notes;
    const project::Project p = bwcli::replay::start(
        steps::parse(R"({"version": 1, "start": {"file": "../projects/stray-attrs"})" + dot), kReplay, kSamples, &notes);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0] == "start: " + (kReplay / "../projects/stray-attrs").string() +
                          ": cleared attribute bits no earlier build wrote from 2 cells");
    const Canvas& c = p.layers[0].cells;
    CHECK(c.at(0, 0).attrs == 0);
    CHECK(c.at(1, 0).attrs == attr::Blink);
    CHECK(c.at(2, 0).attrs == attr::Blink);

    // a clean project, and a flat file, note nothing
    for (const char* start : {"../projects/unwritable.bwp", "../brushes/sphere-steel.ans"}) {
        INFO(start);
        notes.clear();
        bwcli::replay::start(steps::parse(std::string(R"({"version": 1, "start": {"file": ")") + start + R"("})" + dot),
                             kReplay, kSamples, &notes);
        CHECK(notes.empty());
    }
}

TEST_CASE("a replay's command says its start's read notes as one line each", "[replay]") {
    // #383: blockwright-cli replay and blockwright --screenshot both start
    // through startSaying, so the two say the same note
    const std::string dot = R"(, "steps": [{"name": "dot", "action": "glyph", "points": [[5, 0]]}]})";
    const auto said = [&](const std::string& file) {
        std::ostringstream err;
        bwcli::replay::startSaying(steps::parse(R"({"version": 1, "start": {"file": ")" + file + R"("})" + dot),
                                   kReplay, kSamples, err, "prog", "s.json");
        return err.str();
    };
    CHECK(said("../projects/stray-attrs") == "prog: note: s.json: start: " +
                                                 (kReplay / "../projects/stray-attrs").string() +
                                                 ": cleared attribute bits no earlier build wrote from 2 cells\n");
    CHECK(said("../projects/formatted").empty());

    std::ostringstream err;
    CHECK_THROWS_AS(bwcli::replay::startSaying(
                        steps::parse(R"({"version": 1, "start": {"file": "nowhere.ans"})" + dot), kReplay,
                        kSamples, err, "prog", "s.json"),
                    bwcli::Failure);
    CHECK(err.str().empty());
}

TEST_CASE("a view step sets the view where it stands and changes no cell (#398)", "[replay][view]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [
        {"name": "ink", "action": "glyph", "points": [[1, 1]]},
        {"name": "near", "action": "view", "zoom": 4, "scrollTo": [5, 3], "ircFit": true},
        {"name": "pad", "action": "view", "ircPad": true, "ircSwapShades": true},
        {"name": "far", "action": "view", "zoom": 1, "ircFit": false},
        {"name": "more", "action": "glyph", "points": [[2, 2]]}]})");
    struct Seen {
        std::string name;
        bool before;
        steps::View view;
        std::string bytes;
    };
    std::vector<Seen> seen;
    const bwcli::replay::Outcome out =
        bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples), [&](const bwcli::replay::Moment& m) {
            seen.push_back({m.step.name, m.before, m.view, ansBytes(m.project)});
        });
    REQUIRE(seen.size() == 10);
    const auto at = [&](const char* name, bool before) -> const Seen& {
        for (const Seen& x : seen)
            if (x.name == name && x.before == before)
                return x;
        FAIL("no moment " << name);
        throw;
    };
    // nothing set until the first view step, and before it nothing yet
    CHECK(at("ink", false).view == steps::View{});
    CHECK(at("near", true).view == steps::View{});
    // each field the last view step that set it
    steps::View near;
    near.zoom = 4;
    near.scrollTo = steps::Point{5, 3};
    near.ircFit = true;
    CHECK(at("near", false).view == near);
    CHECK(at("pad", true).view == near);
    steps::View pad = near;
    pad.ircPad = true;
    pad.ircSwapShades = true;
    CHECK(at("pad", false).view == pad);
    steps::View far = pad;
    far.zoom = 1;
    far.ircFit = false;
    CHECK(at("far", false).view == far);
    // and it holds for the steps after it
    CHECK(at("more", true).view == far);
    CHECK(at("more", false).view == far);
    // a view step leaves the document as it was
    CHECK(at("near", false).bytes == at("ink", false).bytes);
    CHECK(at("far", false).bytes == at("ink", false).bytes);
    REQUIRE(out.captures.size() == 5);
    CHECK(ansBytes(out.captures[1].project) == ansBytes(out.captures[0].project));
    CHECK(out.notes.empty());
}

TEST_CASE("a view step's scroll cell off the canvas stops the replay, named (#398)", "[replay][view]") {
    CHECK_THAT(onBlank(R"({"name": "a", "action": "selectAll"}, {"name": "far", "action": "view", "scrollTo": [20, 0]})"),
               ContainsSubstring("step 2 (far): scrollTo cell (20, 0) is off the 20 x 10 canvas"));
    CHECK_THAT(onBlank(R"({"name": "low", "action": "view", "scrollTo": [0, 10]})"),
               ContainsSubstring("step 1 (low): scrollTo cell (0, 10) is off the 20 x 10 canvas"));
    CHECK(onBlank(R"({"name": "corner", "action": "view", "scrollTo": [19, 9]})") == "(replayed)");
}

TEST_CASE("viewStep finds a script's first view step, for the CLI to refuse (#398)", "[replay][view]") {
    const steps::Script none = steps::parse(R"({"version": 1, "start": {"blank": [4, 4]}, "steps": [
        {"name": "a", "action": "selectAll"}]})");
    CHECK(bwcli::replay::viewStep(none) == nullptr);
    const steps::Script two = steps::parse(R"({"version": 1, "start": {"blank": [4, 4]}, "steps": [
        {"name": "a", "action": "selectAll"}, {"name": "zoomed", "action": "view", "zoom": 3},
        {"name": "fit", "action": "view", "ircFit": true}]})");
    const steps::Step* v = bwcli::replay::viewStep(two);
    REQUIRE(v);
    CHECK(steps::stepLabel(*v) == "step 2 (zoomed)");
}
