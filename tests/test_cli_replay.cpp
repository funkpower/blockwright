// `blockwright-cli replay` (issue #208): a steps file applied through the core
// brushes, shapes, selection and layer operations. The tour in
// data/replay/tour.json takes every kind of step and writes, every run, the
// bytes saved beside it in data/replay/tour/.

#include "loss.h"
#include "read_file.h"
#include "replay.h"

#include "blockwright/brushes/clone.h"
#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/brushes/smudge.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/canvas_size.h"
#include "blockwright/glyph.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/sauce.h"
#include "blockwright/steps.h"
#include "blockwright/target.h"
#include "blockwright/big_text.h"
#include "blockwright/text.h"
#include "blockwright/xbin.h"

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// The day a saved capture carries when its start has no SAUCE record
// (data/replay/sample-merged), as blockwright-cli replay writes it with
// SOURCE_DATE_EPOCH on that day; a start with a record keeps its own date.
// Without a fixed day the bytes would change with the day the test runs.
// Remake the set with that day, never with today's:
//   SOURCE_DATE_EPOCH=1794009600 blockwright-cli replay \
//     tests/data/replay/sample-merged.json -o tests/data/replay/sample-merged
// and save any new set whose start has no record with the same value.
constexpr const char* kSavedDay = "20261107";

std::string bytesOn(const project::Project& p, const std::string& day) {
    return writeArt(bwcli::replay::document(p, day), ArtFormat::Ansi, TextEncoding::Cp437).bytes;
}

std::string ansBytes(const project::Project& p) { return bytesOn(p, kSavedDay); }

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
    CHECK(out.captures.back().stem == "45-text");
}

TEST_CASE("a capture's SAUCE date is the day given when its start has no record, else the start's own",
          "[replay][sauce]") {
    // sample-merged starts from a project with no SAUCE record, the tour from a piece with one
    for (const std::string name : {"sample-merged", "tour"}) {
        INFO(name);
        const steps::Script s = steps::parse(bwcli::readFile((kReplay / (name + ".json")).string()));
        const bwcli::replay::Outcome out = bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples));
        REQUIRE_FALSE(out.captures.empty());
        CHECK(out.captures.front().project.sauce.has_value() == (name == "tour"));
        for (const auto& c : out.captures) {
            INFO(c.stem);
            const AnsiDocument a = bwcli::replay::document(c.project, "20261107");
            const AnsiDocument b = bwcli::replay::document(c.project, "19991231");
            REQUIRE(a.sauce);
            REQUIRE(b.sauce);
            if (c.project.sauce) {
                CHECK(a.sauce->date == c.project.sauce->date);
                CHECK(bytesOn(c.project, "20261107") == bytesOn(c.project, "19991231"));
            } else {
                CHECK(a.sauce->date == "20261107");
                CHECK(b.sauce->date == "19991231");
            }
            // the day changes nothing else: the canvas and the rest of the record
            Sauce rest = *b.sauce;
            rest.date = a.sauce->date;
            CHECK(encodeSauce(rest, 0) == encodeSauce(*a.sauce, 0));
            CHECK(a.canvas == b.canvas);
            // the same day twice is the same bytes
            CHECK(bytesOn(c.project, "19991231") == bytesOn(c.project, "19991231"));
        }
    }
    // with no day given, a record made for a start without one carries today's local date, as Sauce::fresh
    // dates a new piece; a day that turns over between the two reads is taken by the second
    const project::Project blank = replayed(R"({"version": 1, "start": {"blank": [4, 2]},
                                               "steps": [{"name": "a", "action": "selectAll"}]})")
                                       .captures.front()
                                       .project;
    REQUIRE_FALSE(blank.sauce);
    const std::string first = Sauce::fresh(4, 2).date;
    const std::string dated = bwcli::replay::document(blank).sauce->date;
    CHECK((dated == first || dated == Sauce::fresh(4, 2).date));
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

TEST_CASE("a glyph step a pixels layer can't hold names the glyph, or what radial symmetry turns it into (#436)",
          "[replay]") {
    const auto glyph = [](const std::string& ch, const std::string& symmetry) {
        return onBlank(R"({"name": "dots", "action": "newPixelLayer", "rename": "px"},
                          {"name": "draw", "action": "glyph", "glyph": ")" +
                       ch + "\"" + (symmetry.empty() ? "" : R"(, "symmetry": )" + symmetry) +
                       R"(, "points": [[3, 3]]})");
    };
    const std::string layer = R"(step 2 (draw): the active layer "px" is a pixels layer, which holds only █ ▀ ▄ and blanks)";
    // a quarter turn puts ▀ on its side: ▐ one way round, ▌ the other
    CHECK(glyph("▀", R"({"mode": "radial", "folds": 4})") ==
          layer + ", and radial symmetry turns ▀ on its side, into ▐ and ▌");
    CHECK(glyph("▄", R"({"mode": "radial", "folds": 8})") ==
          layer + ", and radial symmetry turns ▄ on its side, into ▌ and ▐");
    // a glyph the layer can't hold is named whatever the symmetry
    CHECK(glyph("*", "") == layer + ", not *");
    CHECK(glyph("▌", "") == layer + ", not ▌");
    CHECK(glyph("░", R"({"mode": "radial", "folds": 4})") == layer + ", not ░");
    // one that shows nothing on its own is named by its code point
    CHECK(glyph(R"(\u0301)", "") == layer + ", not U+0301");
    CHECK(glyph(R"(\u200b)", "") == layer + ", not U+200B");
    // a C1 control is no glyph at all: the step is refused before the layer (#449)
    CHECK_THROWS_WITH(glyph(R"(\u0085)", ""), "step 2 (draw): \"glyph\" must be one printable character");
    // flips and a half turn keep █ ▀ ▄ and blanks, so they paint, as without symmetry
    CHECK(glyph("▀", "") == "(replayed)");
    CHECK(glyph("▀", R"("mirrorXY")") == "(replayed)");
    CHECK(glyph("▀", R"({"mode": "radial", "folds": 2})") == "(replayed)");
    CHECK(glyph("█", R"({"mode": "radial", "folds": 4})") == "(replayed)");
    // the right button erases to a blank, which every layer holds
    CHECK(onBlank(R"({"name": "dots", "action": "newPixelLayer"},
                     {"name": "rub", "action": "glyph", "glyph": "*", "button": "right",
                      "symmetry": {"mode": "radial", "folds": 4}, "points": [[3, 3]]})") == "(replayed)");
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
         "step 2 (draw)", "pixels layer, which holds only \u2588 \u2580 \u2584 and blanks, not *"},
        {"a block turned on its side on a pixels layer",
         R"({"name": "dots", "action": "newPixelLayer"},
            {"name": "draw", "action": "glyph", "glyph": "\u2580", "symmetry": {"mode": "radial", "folds": 4},
             "points": [[1, 1]]})",
         "step 2 (draw)", "radial symmetry turns \u2580 on its side, into \u2590 and \u258c"},
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

TEST_CASE("a start file is found from the steps file's directory, never from an absolute path (#422)",
          "[replay][paths]") {
    const std::string dot = R"(, "steps": [{"name": "dot", "action": "glyph", "points": [[1, 0]]}]})";
    const auto startFrom = [&](const std::string& file) {
        return bwcli::replay::start(steps::parse(R"({"version": 1, "start": {"file": ")" + file + "\"}" + dot), kReplay,
                                    kSamples);
    };
    // relative, and through ..: the same piece either way
    const project::Project beside = startFrom("layered.bwp");
    CHECK(beside.layers.size() > 1);
    const project::Project up = startFrom("../replay/layered.bwp");
    CHECK(project::toAnsi(up).canvas == project::toAnsi(beside).canvas);
    CHECK(bwcli::replay::resolve(kReplay, "../fonts/x.tdf") == kReplay / "../fonts/x.tdf");

    // an absolute path is refused even when the file is there: the parser
    // refuses it, and start() refuses one in a Script made in code
    const std::string abs = fs::absolute(kReplay / "layered.bwp").string();
    REQUIRE(fs::exists(abs));
    const std::string says =
        "start: \"file\" \"" + abs + "\" is absolute; a steps file names its files relative to its own directory";
    try {
        startFrom(abs);
        FAIL("an absolute start was read");
    } catch (const steps::StepsError& e) {
        CHECK(std::string(e.what()) == says);
    }
    steps::Script made = steps::parse(R"({"version": 1, "start": {"file": "layered.bwp"})" + dot);
    made.start.file = abs;
    try {
        bwcli::replay::start(made, kReplay, kSamples);
        FAIL("an absolute start was read");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == says);
    }

    // a missing start names the start and the path it looked for
    try {
        startFrom("nowhere/piece.ans");
        FAIL("a missing start was read");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "start: " + (kReplay / "nowhere/piece.ans").string() + ": no such file");
    }
#ifndef _WIN32
    // a start that isn't a regular file is refused, not read: a FIFO would
    // block the read and a device would flood it
    QTemporaryDir tmp;
    const fs::path dir = QFile::encodeName(tmp.path()).toStdString();
    REQUIRE(::mkfifo((dir / "fifo.ans").c_str(), 0600) == 0);
    fs::create_symlink("/dev/zero", dir / "zero.ans");
    // art over the most an entry may hold is refused from its size, before
    // a byte is read, whatever a zip project may hold (a sparse file)
    {
        std::ofstream(dir / "big.ans", std::ios::binary) << "x";
        fs::resize_file(dir / "big.ans", project::kMaxTotalBytes + 1);
        try {
            bwcli::replay::start(steps::parse(R"({"version": 1, "start": {"file": "big.ans"})" + dot), dir, kSamples);
            FAIL("a start over the limit was read");
        } catch (const bwcli::Failure& f) {
            CHECK(f.message == "start: " + (dir / "big.ans").string() + ": larger than " +
                                   std::to_string(project::kMaxEntryBytes) + " bytes, the most it may be");
        }
        fs::remove(dir / "big.ans");
    }
    // a directory that isn't a project is no start either
    fs::create_directory(dir / "folder");
    try {
        bwcli::replay::start(steps::parse(R"({"version": 1, "start": {"file": "folder"})" + dot), dir, kSamples);
        FAIL("a directory was read as a start");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "start: " + (dir / "folder").string() + ": is a directory");
    }
    for (const std::string name : {"fifo.ans", "zero.ans"}) {
        steps::Script s = steps::parse(R"({"version": 1, "start": {"file": ")" + name + "\"}" + dot);
        try {
            bwcli::replay::start(s, dir, kSamples);
            FAIL("a start that isn't a regular file was read: " + name);
        } catch (const bwcli::Failure& f) {
            CHECK(f.message == "start: " + (dir / name).string() + ": not a regular file");
        }
    }
#endif
}

TEST_CASE("the load pass reads every file a step names before the replay runs (#422)", "[replay][paths]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const fs::path root = QFile::encodeName(tmp.path()).toStdString();
    const fs::path dir = root / "tutorial";
    fs::create_directories(dir / "fonts");
    fs::create_directories(root / "shared");
    const auto write = [](const fs::path& p, const std::string& bytes) {
        std::ofstream(p, std::ios::binary) << bytes;
    };
    write(dir / "fonts/a.tdf", "font");
    write(root / "shared/b.png", "picture");

    // no action names a file yet, so a step's files are set in code, as the
    // parser will set them for "tdf", "style" and "file"
    const auto script = [](std::vector<steps::FileRef> first, std::vector<steps::FileRef> second = {}) {
        steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [4, 2]}, "steps": [
            {"name": "title", "action": "selectAll"}, {"name": "trace", "action": "selectAll"}]})");
        s.steps[0].files = std::move(first);
        s.steps[1].files = std::move(second);
        return s;
    };
    const auto refusal = [&](steps::Script s, const fs::path& from = {}) {
        try {
            bwcli::replay::load(s, from.empty() ? dir : from);
        } catch (const bwcli::Failure& f) {
            return f.message;
        }
        return std::string("loaded");
    };

    SECTION("relative paths, .. among them, are read by their paths as written") {
        steps::Script s = script({{"tdf", "fonts/a.tdf", 4}}, {{"file", "../shared/b.png", 64}, {"tdf", "fonts/a.tdf", 8}});
        bwcli::replay::load(s, dir);
        CHECK(s.files == std::map<std::string, std::string>{{"fonts/a.tdf", "font"}, {"../shared/b.png", "picture"}});
        // and the replay runs as it would without them
        CHECK(bwcli::replay::run(s, bwcli::replay::start(s, dir, kSamples)).captures.size() == 2);
    }
    SECTION("a missing file names the step and the path") {
        CHECK(refusal(script({}, {{"file", "refs/sketch.png", 64}})) ==
              "step 2 (trace): " + (dir / "refs/sketch.png").string() + ": no such file");
    }
    SECTION("an absolute path, or one pathProblem refuses, names the step and the key") {
        const std::string abs = (dir / "fonts/a.tdf").string();
        CHECK(refusal(script({{"tdf", abs, 64}})) ==
              "step 1 (title): \"tdf\" \"" + abs + "\" is absolute; a steps file names its files relative to its own "
              "directory");
        CHECK(refusal(script({{"file", "fonts\\a.tdf", 64}})) ==
              "step 1 (title): \"file\" \"fonts\\a.tdf\" has a \\ in it; write / between directories, relative to "
              "the steps file");
    }
    SECTION("a directory or a device is not a file") {
        CHECK(refusal(script({{"tdf", "fonts", 64}})) == "step 1 (title): " + (dir / "fonts").string() + ": is a directory");
#ifndef _WIN32
        CHECK(refusal(script({{"file", "null", 64}}), "/dev") == "step 1 (title): /dev/null: not a regular file");
        REQUIRE(::mkfifo((dir / "fonts/fifo.tdf").c_str(), 0600) == 0);
        CHECK(refusal(script({{"tdf", "fonts/fifo.tdf", 64}})) ==
              "step 1 (title): " + (dir / "fonts/fifo.tdf").string() + ": not a regular file");
#endif
    }
    SECTION("a file over its step's limit is refused, and a second step's smaller limit holds") {
        CHECK(refusal(script({{"tdf", "fonts/a.tdf", 3}})) ==
              "step 1 (title): " + (dir / "fonts/a.tdf").string() + ": larger than 3 bytes, the most it may be");
        CHECK(refusal(script({{"tdf", "fonts/a.tdf", 4}}, {{"tdf", "fonts/a.tdf", 2}})) ==
              "step 2 (trace): " + (dir / "fonts/a.tdf").string() + ": larger than 2 bytes, the most it may be");
    }
#ifndef _WIN32
    SECTION("an unreadable file says why") {
        if (::geteuid() == 0)
            SKIP("root reads a file without permissions");
        const fs::path locked = dir / "fonts/locked.tdf";
        write(locked, "x");
        fs::permissions(locked, fs::perms::none);
        const std::string why = refusal(script({{"tdf", "fonts/locked.tdf", 64}}));
        fs::permissions(locked, fs::perms::owner_all);
        CHECK(why == "step 1 (title): " + locked.string() + ": can't be read: " + std::strerror(EACCES));
    }
#endif
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

namespace {

// Each anchor's shift from 4x3 to 7x6 (growing) and to 1x2 (shrinking), as
// Format > Canvas Size... places the old canvas: left/top 0, centre half the
// change (towards zero), right/bottom all of it.
struct AnchorCase {
    const char* name;
    int growX, growY, shrinkX, shrinkY;
};
constexpr AnchorCase kAnchorCases[] = {
    {"top-left", 0, 0, 0, 0},     {"top", 1, 0, -1, 0},     {"top-right", 3, 0, -3, 0},
    {"left", 0, 1, 0, 0},         {"centre", 1, 1, -1, 0},  {"right", 3, 1, -3, 0},
    {"bottom-left", 0, 3, 0, -1}, {"bottom", 1, 3, -1, -1}, {"bottom-right", 3, 3, -3, -1},
};

// A 4x3 start, every cell a letter of its own, then a canvasSize step.
bwcli::replay::Outcome resized(int w, int h, const std::string& anchor) {
    return replayed(R"({"version": 1, "start": {"blank": [4, 3]}, "steps": [
        {"name": "letters", "action": "keyboard", "at": [0, 0], "text": "abcd\nefgh\nijkl"},
        {"name": "resize", "action": "canvasSize", "size": [)" +
                    std::to_string(w) + ", " + std::to_string(h) + R"(], "anchor": ")" + anchor + R"("}]})");
}

// The cells after resized(): each old cell moved by (dx, dy), blank where none lands.
void checkShifted(const Canvas& c, int w, int h, int dx, int dy) {
    REQUIRE(c.width() == w);
    REQUIRE(c.height() == h);
    const std::u32string letters = U"abcdefghijkl";
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            INFO("cell " << x << "," << y);
            const int ox = x - dx, oy = y - dy;
            const bool old = ox >= 0 && ox < 4 && oy >= 0 && oy < 3;
            CHECK(c.at(x, y).ch == (old ? letters[std::size_t(oy * 4 + ox)] : U' '));
        }
}

} // namespace

TEST_CASE("a canvasSize step places the old canvas by its anchor, growing and shrinking, as the dialog does (#420)",
          "[replay][canvasSize]") {
    for (const AnchorCase& a : kAnchorCases) {
        INFO(a.name);
        const auto grown = resized(7, 6, a.name);
        REQUIRE(grown.captures.size() == 2);
        checkShifted(flat(grown.captures[1].project), 7, 6, a.growX, a.growY);
        // the capture before it is the old size
        CHECK(flat(grown.captures[0].project).width() == 4);

        const auto shrunk = resized(1, 2, a.name);
        checkShifted(flat(shrunk.captures[1].project), 1, 2, a.shrinkX, a.shrinkY);
        // the core's own shift is the same
        const CellShift g = anchorShift(4, 3, 7, 6, *anchorFromName(a.name));
        CHECK((g.dx == a.growX && g.dy == a.growY));
        const CellShift s = anchorShift(4, 3, 1, 2, *anchorFromName(a.name));
        CHECK((s.dx == a.shrinkX && s.dy == a.shrinkY));
    }
}

TEST_CASE("a canvasSize step with no anchor keeps the top-left corner, as the dialog opens", "[replay][canvasSize]") {
    const auto out = replayed(R"({"version": 1, "start": {"blank": [4, 3]}, "steps": [
        {"name": "letters", "action": "keyboard", "at": [0, 0], "text": "abcd\nefgh\nijkl"},
        {"name": "resize", "action": "canvasSize", "size": [6, 5]}]})");
    checkShifted(flat(out.captures[1].project), 6, 5, 0, 0);
}

TEST_CASE("a canvasSize step's golden: a piece grown from its top right and cut to its bottom right writes the saved bytes (#420)",
          "[replay][canvasSize]") {
    const auto out = replayed(R"({"version": 1, "start": {"file": "../brushes/sphere-steel.ans"}, "steps": [
        {"name": "grow", "action": "canvasSize", "size": [84, 38], "anchor": "top-right"},
        {"name": "shrink", "action": "canvasSize", "size": [50, 20], "anchor": "bottom-right"}]})");
    REQUIRE(out.captures.size() == 2);
    CHECK(ansBytes(out.captures[0].project) == bwcli::readFile((kReplay / "canvas-size" / "01-grow.ans").string()));
    CHECK(ansBytes(out.captures[1].project) == bwcli::readFile((kReplay / "canvas-size" / "02-shrink.ans").string()));
}

TEST_CASE("a canvasSize step moves the selection with the cells and cuts it to the canvas (#420)",
          "[replay][canvasSize]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [10, 8]}, "steps": [
        {"name": "box", "action": "select", "rect": [2, 3, 5, 4]},
        {"name": "grow", "action": "canvasSize", "size": [14, 10], "anchor": "bottom-right"},
        {"name": "cut", "action": "canvasSize", "size": [8, 6], "anchor": "top-left"},
        {"name": "gone", "action": "canvasSize", "size": [3, 3], "anchor": "top-left"}]})");
    std::vector<Selection> after;
    bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples), [&](const bwcli::replay::Moment& m) {
        if (!m.before)
            after.push_back(m.selection);
    });
    REQUIRE(after.size() == 4);
    CHECK(after[0].bounds() == CellRect{2, 3, 4, 2});
    CHECK(after[1].bounds() == CellRect{6, 5, 4, 2});  // moved by (4, 2)
    CHECK(after[2].bounds() == CellRect{6, 5, 2, 1});  // what is left on the 8x6 canvas
    CHECK(after[3].empty());                          // nothing is left on 3x3
}

namespace {

// A 10x6 project at 9 px cells with a layer of each kind a resize treats
// its own way: a canvas-sized cells and pixels layer (they follow the
// canvas), a smaller cells layer off the corner and a reference picture
// (they move by the shift).
project::Project layered() {
    project::Project p;
    p.layers = LayerStack(10, 6);
    Layer bg = makeLayer(LayerKind::Cells, 10, 6, "Background");
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 10; ++x)
            bg.cells.at(x, y).ch = char32_t(U'a' + (x + y) % 26);
    p.layers.add(std::move(bg));
    Layer dots = makeLayer(LayerKind::Pixels, 10, 6, "dots");
    dots.cells.at(9, 5).ch = kFullBlock;
    p.layers.add(std::move(dots));
    Layer patch = makeLayer(LayerKind::Cells, 3, 2, "patch", 4, 1);
    patch.cells.at(0, 0).ch = U'#';
    p.layers.add(std::move(patch));
    Layer ref = makeLayer(LayerKind::Reference, 10, 6, "sketch");
    ref.props.reference = ReferenceImage{};
    ref.props.reference->x = 5;
    ref.props.reference->y = -3;
    ref.props.reference->width = 160;
    ref.props.reference->height = 96;
    p.layers.add(std::move(ref));
    p.sauce = Sauce{};
    p.sauce->flags = 2 << 1;  // 9 px letter spacing
    return p;
}

} // namespace

TEST_CASE("a canvasSize step resizes every layer of a project as the dialog does, and later steps see the new size "
          "(#420)",
          "[replay][canvasSize]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [1, 1]}, "steps": [
        {"name": "grow", "action": "canvasSize", "size": [16, 9], "anchor": "bottom-right"},
        {"name": "to-bg", "action": "setActiveLayer", "layer": "Background"},
        {"name": "corner", "action": "glyph", "glyph": "X", "points": [[15, 8]]}]})");
    const project::Project p = layered();
    const auto out = bwcli::replay::run(s, p);
    REQUIRE(out.captures.size() == 3);
    const project::Project& grown = out.captures[0].project;
    CHECK(grown.layers.width() == 16);
    CHECK(grown.layers.height() == 9);
    // canvas-sized cells and pixels layers follow the canvas, their cells moved by (6, 3)
    const Layer& bg = layerNamed(grown, "Background");
    CHECK(bg.bounds() == CellRect{0, 0, 16, 9});
    CHECK(bg.cells.at(6, 3).ch == U'a');
    CHECK(bg.cells.at(15, 8).ch == char32_t(U'a' + 14));
    CHECK(bg.cells.at(5, 3).ch == U' ');
    const Layer& dots = layerNamed(grown, "dots");
    CHECK(dots.bounds() == CellRect{0, 0, 16, 9});
    CHECK(dots.cells.at(15, 8).ch == kFullBlock);
    // a smaller layer keeps its size and moves by the shift
    CHECK(layerNamed(grown, "patch").bounds() == CellRect{10, 4, 3, 2});
    // a reference picture moves by the shift in canvas pixels: 6 cells of 9 px across, 3 of 16 px down
    const Layer& ref = layerNamed(grown, "sketch");
    CHECK(ref.props.reference->x == 5 + 6 * 9);
    CHECK(ref.props.reference->y == -3 + 3 * 16);
    // the same as the core's resize the dialog runs
    CHECK(grown.layers == resizeCanvas(p.layers, 16, 9, anchorShift(10, 6, 16, 9, Anchor::BottomRight), 9));
    // the new corner is on the canvas for the steps after
    CHECK(layerNamed(out.captures[2].project, "Background").cells.at(15, 8).ch == U'X');
}

TEST_CASE("a canvasSize step's project writes and reads back at the new size, as replay --project writes it (#420)",
          "[replay][canvasSize]") {
    const fs::path dir = fs::path(BW_TEST_TMP) / "replay-canvas-size";
    fs::remove_all(dir);
    fs::create_directories(dir);
    project::Project start = layered();
    // a reference layer with no picture writes as one
    start.layers[3].props.reference.reset();
    project::writeZip(start, dir / "start.bwp");
    const auto out = replayed(R"({"version": 1, "start": {"file": "start.bwp"}, "steps": [
        {"name": "shrink", "action": "canvasSize", "size": [7, 4], "anchor": "centre"}]})",
                              dir);
    REQUIRE(out.captures.size() == 1);
    const project::Project& shrunk = out.captures[0].project;
    {
        std::ofstream f(dir / "01-shrink.bwp", std::ios::binary);
        f << project::zipBytes(shrunk);
    }
    const project::Project back = project::read(dir / "01-shrink.bwp");
    CHECK(back.layers.width() == 7);
    CHECK(back.layers.height() == 4);
    CHECK(back.layers == shrunk.layers);
    CHECK(layerNames(back) == std::vector<std::string>{"Background", "dots", "patch", "sketch"});
    // shrinking 10x6 to 7x4 about the centre takes (-1, -1)
    CHECK(layerNamed(back, "Background").cells.at(0, 0).ch == char32_t(U'a' + 2));
    CHECK(layerNamed(back, "patch").bounds() == CellRect{3, 0, 3, 2});
    fs::remove_all(dir);
}

TEST_CASE("a canvasSize step refuses what the dialog refuses, and a later point off the new canvas, named (#420)",
          "[replay][canvasSize]") {
    // a point the old canvas had and the new one hasn't
    CHECK(onBlank(R"({"name": "cut", "action": "canvasSize", "size": [5, 5]},
                     {"name": "far", "action": "glyph", "points": [[7, 2]]})") ==
          "step 2 (far): cell (7, 2) is off the 5 x 5 canvas");
    // more cells than a project holds: nine canvas-sized layers at the largest size
    std::string layers;
    for (int i = 0; i < 8; ++i)
        layers += R"({"name": "l)" + std::to_string(i) + R"(", "action": "newCellsLayer"}, )";
    CHECK_THAT(onBlank(layers + R"({"name": "huge", "action": "canvasSize", "size": [400, 5000]})"),
               ContainsSubstring("step 9 (huge): the canvas can't be made 400 x 5000: "));
    // the canvas's own size changes nothing, and says so
    const auto same = blank(R"({"name": "same", "action": "canvasSize", "size": [20, 10], "anchor": "centre"})");
    REQUIRE(same.notes.size() == 1);
    CHECK(same.notes[0] == "step 1 (same): changes nothing");
    CHECK(flat(same.captures[0].project).width() == 20);
}

TEST_CASE("a canvasSize step pulls a set symmetry centre onto the new canvas, as the view does (#420 review)",
          "[replay][canvasSize]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [
        {"name": "sym", "action": "glyph", "glyph": "X", "symmetry": {"mode": "radial", "folds": 2, "centre": [10, 4]}},
        {"name": "cut", "action": "canvasSize", "size": [10, 10]},
        {"name": "dot", "action": "glyph", "points": [[9, 2]]}]})");
    std::vector<std::optional<std::pair<int, int>>> centres;
    const auto out = bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples), [&](const bwcli::replay::Moment& m) {
        if (!m.before)
            centres.push_back(m.settings.symmetry.centre);
    });
    REQUIRE(centres.size() == 3);
    CHECK(centres[0] == std::pair{10, 4});
    CHECK(centres[1] == std::pair{9, 4});
    // the dot turns about (9, 4), so its image is at (9, 6); about the old
    // (10, 4) it would be at (11, 6), off the canvas
    const Canvas c = flat(out.captures.back().project);
    CHECK(c.at(9, 2).ch == U'X');
    CHECK(c.at(9, 6).ch == U'X');
    // a centre the new canvas still holds stays where it is
    const steps::Script grown = steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [
        {"name": "sym", "action": "glyph", "symmetry": {"mode": "radial", "folds": 2, "centre": [10, 4]}},
        {"name": "grow", "action": "canvasSize", "size": [30, 12], "anchor": "centre"}]})");
    std::optional<std::pair<int, int>> kept;
    bwcli::replay::run(grown, bwcli::replay::start(grown, kReplay, kSamples), [&](const bwcli::replay::Moment& m) {
        if (!m.before)
            kept = m.settings.symmetry.centre;
    });
    CHECK(kept == std::pair{10, 4});
}

namespace {

// A 20x10 cells canvas whose every cell differs from its neighbours: a
// letter for the column, colours for the row.
Layer patternLayer(const std::string& name, char32_t first = U'a') {
    Layer l = makeLayer(LayerKind::Cells, 20, 10, name);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x) {
            Cell& c = l.cells.at(x, y);
            c.ch = char32_t(first + x);
            c.transparent = false;
            c.fg = Color::indexed(std::uint8_t(1 + y % 15));
            c.bg = Color::indexed(std::uint8_t(y % 8));
        }
    return l;
}

project::Project patterned() {
    project::Project p;
    p.layers = LayerStack(20, 10);
    p.layers.add(patternLayer("Background"));
    return p;
}

const Canvas& patternCells() {
    static const Canvas c = patterned().layers.layers()[0].cells;
    return c;
}

bwcli::replay::Outcome onPattern(const std::string& steps, project::Project start = patterned()) {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [)" + steps + "]}");
    return bwcli::replay::run(s, std::move(start));
}

std::string refusedOnPattern(const std::string& steps, project::Project start = patterned()) {
    try {
        onPattern(steps, std::move(start));
    } catch (const bwcli::Failure& f) {
        return f.message;
    }
    return "(replayed)";
}

// The cells two canvases differ in, as "x,y: ch/fg/bg vs ch/fg/bg" lines; empty when they agree.
std::string cellDiff(const Canvas& a, const Canvas& b) {
    if (a.width() != b.width() || a.height() != b.height())
        return "sizes differ";
    std::ostringstream o;
    const auto cell = [&](const Cell& c) {
        std::ostringstream t;
        t << std::uint32_t(c.ch.codePointOr(U'?')) << "/" << int(c.fg.kind) << int(c.bg.kind) << "/" << int(c.attrs) << int(c.transparent) << "/" << int(c.fg.index) << "/" << int(c.bg.index);
        return t.str();
    };
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (!(a.at(x, y) == b.at(x, y)))
                o << x << "," << y << ": " << cell(a.at(x, y)) << " vs " << cell(b.at(x, y)) << "\n";
    return o.str();
}

// What the core clone paints: `clone` begins a stroke over `dabs` on
// `before` and lands it, as CloneTool's stroke does.
Canvas cloned(const Canvas& before, CloneSource& clone, const std::vector<Dab>& dabs, int size, Tip tip = Tip::Square,
              const StrokeOptions& opts = {}) {
    const std::optional<CloneBrush> brush = clone.beginStroke(dabs[0].x, dabs[0].y);
    REQUIRE(brush);
    BrushSettings settings;
    settings.size = size;
    settings.tip = tip;
    Canvas out = before;
    paintStroke(before, *brush, settings, dabs, opts).apply(out);
    return out;
}

} // namespace

TEST_CASE("a clone step copies what the core clone brush copies, from its source (#418)", "[replay][clone]") {
    const auto out = onPattern(R"({"name": "pick", "action": "clone", "source": [2, 3]},
        {"name": "stamp", "action": "clone", "size": 3, "points": [[12, 5], [14, 6], [15, 6]], "capture": "before-after"})");
    REQUIRE(out.captures.size() == 3);
    // setting the source changes no cell
    CHECK(cellsOf(out.captures[0]) == patternCells());
    const Canvas& before = cellsOf(out.captures[1]);
    CloneSource clone;
    clone.setSource(2, 3);
    const Canvas want = cloned(before, clone, {{12, 5, 1.0}, {14, 6, 1.0}, {15, 6, 1.0}}, 3);
    CHECK(cellDiff(cellsOf(out.captures[2]), want) == "");
    CHECK_FALSE(want == before);
    // the stroke's first dab copies the source itself
    CHECK(cellsOf(out.captures[2]).at(12, 5) == before.at(2, 3));
    CHECK(cellsOf(out.captures[2]).at(13, 6) == before.at(3, 4));
}

TEST_CASE("a source and a stroke on one step: the source is set first (#418)", "[replay][clone]") {
    const auto one = onPattern(R"({"name": "stamp", "action": "clone", "source": [4, 1], "points": [[10, 7]]})");
    const auto two = onPattern(R"({"name": "pick", "action": "clone", "source": [4, 1]},
                                  {"name": "stamp", "action": "clone", "points": [[10, 7]]})");
    CHECK(cellsOf(one.captures[0]) == cellsOf(two.captures[1]));
    CHECK(cellsOf(one.captures[0]).at(10, 7) == patternCells().at(4, 1));
}

TEST_CASE("aligned clone strokes keep the distance the first set; not aligned, each starts at the source (#418)",
          "[replay][clone]") {
    for (const bool aligned : {true, false}) {
        INFO("aligned " << aligned);
        const std::string a = aligned ? "true" : "false";
        const auto out = onPattern(R"({"name": "pick", "action": "clone", "source": [1, 1], "aligned": )" + a + R"(},
            {"name": "one", "action": "clone", "points": [[10, 2], [11, 2]]},
            {"name": "two", "action": "clone", "points": [[5, 7], [6, 7]]},
            {"name": "three", "action": "clone", "size": 2, "points": [[15, 5]]})");
        REQUIRE(out.captures.size() == 4);
        CloneSource clone;
        clone.aligned = aligned;
        clone.setSource(1, 1);
        Canvas want = cellsOf(out.captures[0]);
        want = cloned(want, clone, {{10, 2, 1.0}, {11, 2, 1.0}}, 1);
        CHECK(cellsOf(out.captures[1]) == want);
        want = cloned(want, clone, {{5, 7, 1.0}, {6, 7, 1.0}}, 1);
        CHECK(cellsOf(out.captures[2]) == want);
        want = cloned(want, clone, {{15, 5, 1.0}}, 2);
        CHECK(cellsOf(out.captures[3]) == want);
        // aligned, the second stroke reads (5, 7) + (1, 1) - (10, 2), off the canvas: nothing changes
        if (aligned)
            CHECK(cellsOf(out.captures[2]).at(5, 7) == patternCells().at(5, 7));
        else
            CHECK(cellsOf(out.captures[2]).at(5, 7) == patternCells().at(1, 1));
    }
}

TEST_CASE("a new source starts the distance over, and Aligned set on a later step holds (#418)", "[replay][clone]") {
    const auto out = onPattern(R"({"name": "pick", "action": "clone", "source": [1, 1]},
        {"name": "one", "action": "clone", "points": [[10, 2]]},
        {"name": "repick", "action": "clone", "source": [3, 3]},
        {"name": "two", "action": "clone", "points": [[12, 8]]},
        {"name": "off", "action": "clone", "aligned": false},
        {"name": "pen", "action": "glyph", "points": [[0, 9]]},
        {"name": "three", "action": "clone", "points": [[16, 4]]},
        {"name": "four", "action": "clone", "points": [[17, 6]]})");
    REQUIRE(out.captures.size() == 8);
    CHECK(cellsOf(out.captures[1]).at(10, 2) == patternCells().at(1, 1));
    CHECK(cellsOf(out.captures[3]).at(12, 8) == patternCells().at(3, 3));
    // not aligned now: every stroke starts at the source again
    CHECK(cellsOf(out.captures[6]).at(16, 4) == patternCells().at(3, 3));
    CHECK(cellsOf(out.captures[7]).at(17, 6) == patternCells().at(3, 3));
}

TEST_CASE("the distance is kept by a stroke that changes nothing, as the editor's finished stroke keeps it (#418)",
          "[replay][clone]") {
    // the first stroke copies (1, 1) onto itself; the next, aligned, copies from (8, 5)
    const auto out = onPattern(R"({"name": "pick", "action": "clone", "source": [1, 1]},
        {"name": "same", "action": "clone", "points": [[1, 1]]},
        {"name": "next", "action": "clone", "points": [[8, 5]]})");
    REQUIRE(out.captures.size() == 3);
    CHECK(cellsOf(out.captures[1]) == patternCells());
    CHECK(cellsOf(out.captures[2]).at(8, 5) == patternCells().at(8, 5));
    const auto fresh = onPattern(R"({"name": "pick", "action": "clone", "source": [1, 1]},
        {"name": "next", "action": "clone", "points": [[8, 5]]})");
    CHECK(cellsOf(fresh.captures[1]).at(8, 5) == patternCells().at(1, 1));
}

TEST_CASE("a clone stroke under mirror and radial symmetry paints what the core brush paints (#418)",
          "[replay][clone]") {
    const std::pair<const char*, SymmetryMode> modes[] = {{"mirrorX", SymmetryMode::MirrorX},
                                                          {"mirrorY", SymmetryMode::MirrorY},
                                                          {"mirrorXY", SymmetryMode::MirrorXY},
                                                          {"radial", SymmetryMode::Radial}};
    for (const auto& [name, mode] : modes) {
        for (const Tip tip : {Tip::Square, Tip::Round}) {
            const std::string tipName = tip == Tip::Round ? "round" : "square";
            INFO(name << " " << tipName);
            const auto out = onPattern(std::string(R"({"name": "s", "action": "clone", "source": [3, 2], "size": 3, "tip": ")") +
                                       tipName + R"(", "symmetry": ")" + name +
                                       R"(", "points": [[5, 3], [7, 4], [9, 4]], "capture": "before-after"})");
            REQUIRE(out.captures.size() == 2);
            StrokeOptions opts;
            opts.symmetry.mode = mode;
            const std::vector<Dab> dabs = {{5, 3, 1.0}, {7, 4, 1.0}, {9, 4, 1.0}};
            CloneSource clone;
            clone.setSource(3, 2);
            const Canvas want = cloned(cellsOf(out.captures[0]), clone, dabs, 3, tip, opts);
            CHECK(cellsOf(out.captures[1]) == want);
            // and the symmetry did something
            CloneSource plain;
            plain.setSource(3, 2);
            CHECK_FALSE(cloned(cellsOf(out.captures[0]), plain, dabs, 3, tip) == want);
        }
    }
}

TEST_CASE("a clone stroke lands only in the selection, as the editor's (#418)", "[replay][clone]") {
    const auto out = onPattern(R"({"name": "sel", "action": "select", "rect": [10, 0, 12, 9]},
        {"name": "s", "action": "clone", "source": [5, 5], "size": 5, "points": [[11, 5]]})");
    const Canvas& c = cellsOf(out.captures[1]);
    CHECK(c.at(9, 5) == patternCells().at(9, 5));
    CHECK(c.at(13, 5) == patternCells().at(13, 5));
    CHECK(c.at(10, 5) == patternCells().at(4, 5));
    CHECK(c.at(11, 5) == patternCells().at(5, 5));
    CHECK(c.at(12, 7) == patternCells().at(6, 7));
}

namespace {

// Background (the pattern), a hidden layer "ghost" over it filled with
// other letters, and "ink" on top: "xyz" at (2, 2) on transparent cells.
project::Project inked() {
    project::Project p = patterned();
    Layer ghost = patternLayer("ghost", U'A');
    ghost.props.visible = false;
    p.layers.add(std::move(ghost));
    Layer ink = makeLayer(LayerKind::Cells, 20, 10, "ink");
    for (int i = 0; i < 3; ++i) {
        ink.cells.at(2 + i, 2).ch = char32_t(U'x' + i);
        ink.cells.at(2 + i, 2).transparent = false;
        ink.cells.at(2 + i, 2).fg = Color::indexed(12);
    }
    p.layers.add(std::move(ink));
    return p;
}

} // namespace

TEST_CASE("a clone step copies the active layer's cells only, under hidden layers and over them (#418)",
          "[replay][clone][layers]") {
    const project::Project start = inked();
    const auto out = onPattern(R"({"name": "use", "action": "setActiveLayer", "layer": "ink"},
        {"name": "s", "action": "clone", "source": [3, 2], "size": 3, "points": [[12, 5], [13, 5]]})",
                               start);
    REQUIRE(out.captures.size() == 2);
    const project::Project& p = out.captures[1].project;
    CHECK(layerNamed(p, "Background").cells == patternCells());
    CHECK(layerNamed(p, "ghost").cells == layerNamed(start, "ghost").cells);
    CloneSource clone;
    clone.setSource(3, 2);
    const Canvas& inkBefore = layerNamed(start, "ink").cells;
    const Canvas want = cloned(inkBefore, clone, {{12, 5, 1.0}, {13, 5, 1.0}}, 3,
                               Tip::Square, layerStrokeOptions(layerNamed(start, "ink")));
    CHECK(layerNamed(p, "ink").cells == want);
    // the ink's letters, and its transparent cells beside them: neither the
    // hidden layer's nor the background's
    CHECK(want.at(12, 5).ch == U'y');
    CHECK(want.at(12, 6) == inkBefore.at(3, 3));
    CHECK_FALSE(layerNamed(p, "ink").cells == inkBefore);

    // on the background, under the hidden layer, it copies the background
    const auto under = onPattern(R"({"name": "use", "action": "setActiveLayer", "layer": "Background"},
        {"name": "s", "action": "clone", "source": [3, 2], "points": [[12, 5]]})",
                                 start);
    CHECK(layerNamed(under.captures[1].project, "Background").cells.at(12, 5) == patternCells().at(3, 2));
    CHECK(layerNamed(under.captures[1].project, "ink").cells == inkBefore);
}

TEST_CASE("a clone stroke refuses a hidden or locked active layer, as the editor's tools do, and keeps the alpha lock "
          "(#418)",
          "[replay][clone][layers]") {
    CHECK(refusedOnPattern(R"({"name": "use", "action": "setActiveLayer", "layer": "ghost"},
                              {"name": "s", "action": "clone", "source": [3, 2], "points": [[12, 5]]})",
                           inked()) == "step 2 (s): the active layer \"ghost\" is hidden");
    CHECK(refusedOnPattern(R"({"name": "use", "action": "setActiveLayer", "layer": "ink"},
                              {"name": "lock", "action": "layerProperties", "locked": true},
                              {"name": "s", "action": "clone", "source": [3, 2], "points": [[12, 5]]})",
                           inked()) == "step 3 (s): the active layer \"ink\" is locked");
    // a source alone is no edit: a hidden layer may take one
    CHECK_NOTHROW(onPattern(R"({"name": "use", "action": "setActiveLayer", "layer": "ghost"},
                               {"name": "s", "action": "clone", "source": [3, 2]})",
                            inked()));

    // alpha locked, the ink's transparent cells stay transparent; unlocked they take the copy
    for (const bool locked : {true, false}) {
        INFO("alpha locked " << locked);
        const auto out = onPattern(std::string(R"({"name": "use", "action": "setActiveLayer", "layer": "ink"},
            {"name": "lock", "action": "layerProperties", "alphaLocked": )") + (locked ? "true" : "false") + R"(},
            {"name": "s", "action": "clone", "source": [3, 2], "size": 3, "points": [[12, 5]]})",
                                   inked());
        const Canvas inkBefore = layerNamed(inked(), "ink").cells;
        const Canvas& after = layerNamed(out.captures.back().project, "ink").cells;
        INFO(cellDiff(after, inkBefore));
        CHECK((after == inkBefore) == locked);
    }
}

TEST_CASE("a clone source or point off the canvas stops the replay, named (#418)", "[replay][clone]") {
    CHECK(onBlank(R"({"name": "cl", "action": "clone", "source": [20, 3]})") ==
          "step 1 (cl): source cell (20, 3) is off the 20 x 10 canvas");
    CHECK(onBlank(R"({"name": "cl", "action": "clone", "source": [-1, 4]})") ==
          "step 1 (cl): source cell (-1, 4) is off the 20 x 10 canvas");
    CHECK(onBlank(R"({"name": "cl", "action": "clone", "source": [0, 10], "points": [[1, 1]]})") ==
          "step 1 (cl): source cell (0, 10) is off the 20 x 10 canvas");
    CHECK(onBlank(R"({"name": "cl", "action": "clone", "source": [0, 0], "points": [[1, 1], [20, 1]]})") ==
          "step 1 (cl): cell (20, 1) is off the 20 x 10 canvas");
    // a canvas cut to leave the source off it: the clone copies nothing from there, as the editor's
    const auto out = onPattern(R"({"name": "pick", "action": "clone", "source": [15, 8]},
        {"name": "cut", "action": "canvasSize", "size": [10, 5]},
        {"name": "s", "action": "clone", "size": 3, "points": [[2, 2]]})");
    REQUIRE(out.captures.size() == 3);
    CHECK(cellsOf(out.captures[2]) == cellsOf(out.captures[1]));
}

TEST_CASE("a clone step's golden: a piece's cells cloned aligned, then not, under mirror symmetry, write the saved "
          "bytes (#418)",
          "[replay][clone]") {
    const steps::Script s = steps::parse(bwcli::readFile((kReplay / "clone.json").string()));
    const bwcli::replay::Outcome out = bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples));
    REQUIRE(out.captures.size() == 4);
    for (const auto& c : out.captures) {
        INFO(c.stem);
        const fs::path golden = kReplay / "clone" / (c.stem + ".ans");
        REQUIRE(fs::exists(golden));
        CHECK(ansBytes(c.project) == bwcli::readFile(golden.string()));
    }
    // the source alone changes nothing, and every stroke changes the piece
    CHECK(ansBytes(out.captures[0].project) == bwcli::readFile((kReplay / ".." / "brushes" / "sphere-steel.ans").string()));
    for (std::size_t i = 1; i < out.captures.size(); ++i)
        CHECK_FALSE(ansBytes(out.captures[i].project) == ansBytes(out.captures[i - 1].project));
}

// ---- Sample merged (#419) ---------------------------------------------------
//
// data/replay/layered.bwp (made by layered-start.json) is 20 x 4, bottom
// to top:
//   ground  cells: a red bar down column 5; cell (18, 3) a red top pixel
//           over a blue bottom one; black elsewhere
//   ghost   cells, hidden: a green bar down column 10
//   paint   cells, all clear
//   dots    pixels: a magenta bar down column 15
// so the canvas shows black regions in columns 0-4 and 6-14 (the hidden
// green bar not among what it shows, the magenta one above "paint" among
// it), and the editor opens on "ground".

namespace {

bwcli::replay::Outcome layered(const std::string& steps) {
    return replayed(R"({"version": 1, "start": {"file": "layered.bwp"}, "steps": [)" + steps + "]}");
}

// Both halves of a cell as halfblock.h reads them, or nothing for a clear one.
std::optional<std::pair<int, int>> halves(const Cell& c) {
    if (c.transparent)
        return std::nullopt;
    const auto h = halfBlocks(c);
    REQUIRE(h);
    return std::pair{int(h->top), int(h->bottom)};
}

// The columns of row `y` of `layer` that hold both halves `colour`.
std::vector<int> columnsOf(const Layer& layer, int y, int colour) {
    std::vector<int> xs;
    for (int x = 0; x < layer.cells.width(); ++x)
        if (halves(layer.cells.at(x, y)) == std::pair{colour, colour})
            xs.push_back(x);
    return xs;
}

std::vector<int> span(int from, int to) {
    std::vector<int> v;
    for (int x = from; x <= to; ++x)
        v.push_back(x);
    return v;
}

// The pen after each step of `steps`, by step name.
std::map<std::string, Pen> pensAfter(const std::string& json, project::Project start) {
    std::map<std::string, Pen> pens;
    bwcli::replay::run(steps::parse(json), std::move(start), [&](const bwcli::replay::Moment& m) {
        if (!m.before)
            pens[m.step.name] = m.settings.pen;
    });
    return pens;
}

project::Project layeredStart() {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"file": "layered.bwp"},
                                             "steps": [{"name": "a", "action": "selectAll"}]})");
    return bwcli::replay::start(s, kReplay, kSamples);
}

} // namespace

TEST_CASE("the layered start is the one the Sample merged tests describe (#419)", "[replay][sample-merged]") {
    const project::Project p = layeredStart();
    REQUIRE(layerNames(p) == std::vector<std::string>{"ground", "ghost", "paint", "dots"});
    CHECK_FALSE(layerNamed(p, "ghost").props.visible);
    CHECK(layerNamed(p, "dots").props.kind == LayerKind::Pixels);
    CHECK(columnsOf(layerNamed(p, "ground"), 0, Red) == std::vector<int>{5});
    CHECK(columnsOf(layerNamed(p, "ghost"), 2, Green) == std::vector<int>{10});
    CHECK(halves(layerNamed(p, "ground").cells.at(18, 3)) == std::pair{int(Red), int(Blue)});
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 20; ++x)
            CHECK(layerNamed(p, "paint").cells.at(x, y).transparent);
    CHECK(layerPixels(layerNamed(p, "dots").cells.at(15, 3)) == LayerPixels{Magenta, Magenta});
}

TEST_CASE("fill floods the active layer, or with sampleMerged what the canvas shows (#419)",
          "[replay][sample-merged]") {
    // off (the default, and false said outright), the clear "paint" layer is
    // one region: the whole layer is filled
    for (const char* key : {"", R"("sampleMerged": false, )"}) {
        INFO("key: " << key);
        const auto out = layered(std::string(R"({"name": "to-paint", "action": "setActiveLayer", "layer": "paint"},
            {"name": "fill", "action": "fill", "fg": 14, )") + key + R"("at": [0, 0]})");
        const Layer& paint = layerNamed(out.captures.back().project, "paint");
        for (int y = 0; y < 4; ++y)
            CHECK(columnsOf(paint, y, Yellow) == span(0, 19));
    }

    // on, the region is the canvas's black one in columns 0-4: the red bar
    // below bounds it
    const auto out = layered(R"({"name": "to-paint", "action": "setActiveLayer", "layer": "paint"},
        {"name": "merged", "action": "fill", "fg": 14, "sampleMerged": true, "at": [0, 0]},
        {"name": "held", "action": "fill", "fg": 11, "at": [7, 0]},
        {"name": "half", "action": "fill", "fg": 14, "at": [18, 6]},
        {"name": "off", "action": "fill", "fg": 9, "sampleMerged": false, "at": [19, 0]})");
    const auto paintAfter = [&](int capture) -> const Layer& {
        return layerNamed(out.captures[std::size_t(capture)].project, "paint");
    };
    for (int y = 0; y < 4; ++y) {
        INFO("row " << y);
        CHECK(columnsOf(paintAfter(1), y, Yellow) == span(0, 4));
        // held for the next step: the hidden green bar in column 10 bounds
        // nothing, and the magenta bar above "paint" in column 15 does,
        // though none of its colour is copied into "paint"
        CHECK(columnsOf(paintAfter(2), y, LightCyan) == span(6, 14));
        CHECK(paintAfter(2).cells.at(5, y).transparent);
        CHECK(paintAfter(2).cells.at(15, y).transparent);
    }
    // the red top pixel of (18, 3) is a region of one: the cell's other half
    // keeps the blue that showed through the clear cell
    CHECK(halves(paintAfter(3).cells.at(18, 3)) == std::pair{int(Yellow), int(Blue)});
    CHECK(paintAfter(3).cells.at(18, 2).transparent);
    CHECK(paintAfter(3).cells.at(17, 3).transparent);
    // the layers it doesn't paint are as they started
    const project::Project start = layeredStart();
    for (const char* name : {"ground", "ghost", "dots"})
        CHECK(layerNamed(out.captures[3].project, name).cells == layerNamed(start, name).cells);
    // off again: "paint"'s own clear pixels around (19, 0), columns 15-19;
    // (18, 3) took the blue under it when its top half was painted, so it
    // is no clear pixel of "paint" now and the fill leaves it
    const Layer& off = paintAfter(4);
    for (int y = 0; y < 4; ++y)
        CHECK(columnsOf(off, y, LightBlue) == (y == 3 ? std::vector<int>{15, 16, 17, 19} : span(15, 19)));
    CHECK(halves(off.cells.at(18, 3)) == std::pair{int(Yellow), int(Blue)});
    CHECK(off.cells.at(5, 0).transparent);
}

TEST_CASE("a sampleMerged fill on a pixels layer paints the region into that layer's pixels (#419)",
          "[replay][sample-merged]") {
    const auto merged = layered(R"({"name": "to-dots", "action": "setActiveLayer", "layer": "dots"},
        {"name": "fill", "action": "fill", "fg": 12, "sampleMerged": true, "at": [7, 1]})");
    const Layer& dots = layerNamed(merged.captures.back().project, "dots");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 20; ++x) {
            INFO("cell " << x << ", " << y);
            if (x >= 6 && x <= 14)
                CHECK(layerPixels(dots.cells.at(x, y)) == LayerPixels{LightRed, LightRed});
            else if (x == 15)
                CHECK(layerPixels(dots.cells.at(x, y)) == LayerPixels{Magenta, Magenta});
            else
                CHECK(dots.cells.at(x, y).transparent);
        }
    // off, the layer's own clear pixels left of its bar are the region
    const auto own = layered(R"({"name": "to-dots", "action": "setActiveLayer", "layer": "dots"},
        {"name": "fill", "action": "fill", "fg": 12, "at": [7, 1]})");
    const Layer& all = layerNamed(own.captures.back().project, "dots");
    CHECK(layerPixels(all.cells.at(0, 0)) == LayerPixels{LightRed, LightRed});
    CHECK(layerPixels(all.cells.at(14, 3)) == LayerPixels{LightRed, LightRed});
    CHECK(all.cells.at(16, 0).transparent);
    CHECK(layerPixels(all.cells.at(15, 0)) == LayerPixels{Magenta, Magenta});
}

TEST_CASE("a sampleMerged fill under the alpha lock paints no clear cell (#419)", "[replay][sample-merged]") {
    const auto out = layered(R"({"name": "to-paint", "action": "setActiveLayer", "layer": "paint"},
        {"name": "lock", "action": "layerProperties", "alphaLocked": true},
        {"name": "fill", "action": "fill", "fg": 14, "sampleMerged": true, "at": [0, 0]})");
    REQUIRE(out.notes.size() == 1);
    CHECK_THAT(out.notes[0], ContainsSubstring("step 3 (fill): changes nothing"));
}

TEST_CASE("the eyedropper picks from the active layer, or with sampleMerged from what the canvas shows (#419)",
          "[replay][sample-merged]") {
    const std::string json = R"({"version": 1, "start": {"file": "layered.bwp"}, "steps": [
        {"name": "to-paint", "action": "setActiveLayer", "layer": "paint"},
        {"name": "own", "action": "eyedropper", "at": [5, 0]},
        {"name": "below", "action": "eyedropper", "sampleMerged": true, "at": [5, 0]},
        {"name": "hidden", "action": "eyedropper", "fg": 14, "at": [10, 0]},
        {"name": "above", "action": "eyedropper", "at": [15, 1]},
        {"name": "half", "action": "eyedropper", "button": "right", "at": [18, 7]},
        {"name": "to-dots", "action": "setActiveLayer", "layer": "dots"},
        {"name": "dots-own", "action": "eyedropper", "sampleMerged": false, "fg": 14, "at": [5, 0]},
        {"name": "dots-merged", "action": "eyedropper", "sampleMerged": true, "at": [5, 0]}]})";
    const auto pens = pensAfter(json, layeredStart());
    // the clear "paint" layer has nothing at (5, 0): the pen is as it was
    CHECK(pens.at("own").fg == Color::indexed(Grey));
    CHECK(pens.at("below").fg == Color::indexed(Red));
    // the hidden green bar isn't shown: black is
    CHECK(pens.at("hidden").fg == Color::indexed(Black));
    CHECK(pens.at("above").fg == Color::indexed(Magenta));
    CHECK(pens.at("half").bg == Color::indexed(Blue));
    CHECK(pens.at("half").fg == Color::indexed(Magenta));
    CHECK(pens.at("dots-own").fg == Color::indexed(Yellow));
    CHECK(pens.at("dots-merged").fg == Color::indexed(Red));

    const auto out = replayed(json);
    REQUIRE(out.notes.size() == 2);
    CHECK_THAT(out.notes[0], ContainsSubstring("step 2 (own): pixel (5, 0) is transparent"));
    CHECK_THAT(out.notes[1], ContainsSubstring("step 8 (dots-own): pixel (5, 0) is transparent"));
}

TEST_CASE("over a shade under a pixels layer, sampleMerged reads the shade as the canvas shows it (#419)",
          "[replay][sample-merged]") {
    // a grey-on-black light shade on "ground" at (2, 1): no half block, so
    // the composite has no pixels there to flood or pick as a pixels
    // layer's own would be read
    const std::string shade = R"({"name": "shade", "action": "glyph", "glyph": "░", "fg": 7, "bg": 0, "points": [[2, 1]]},
        {"name": "to-dots", "action": "setActiveLayer", "layer": "dots"},)";
    // the editor's merged fill floods the composite as a cells layer's
    // pixels, and a shade is no pixel it can fill: nothing changes
    const auto fill = layered(shade + R"({"name": "fill", "action": "fill", "fg": 12, "sampleMerged": true, "at": [2, 2]})");
    REQUIRE(fill.notes.size() == 1);
    CHECK_THAT(fill.notes[0], ContainsSubstring("step 3 (fill): changes nothing"));
    // a merged pick takes the shade's foreground (pickColour on the canvas)
    const std::string pick = R"({"version": 1, "start": {"file": "layered.bwp"}, "steps": [)" + shade +
                             R"({"name": "pick", "action": "eyedropper", "sampleMerged": true, "fg": 14, "at": [2, 2]},
        {"name": "pick-bg", "action": "eyedropper", "button": "right", "bg": 3, "at": [2, 3]}]})";
    const auto pens = pensAfter(pick, layeredStart());
    CHECK(pens.at("pick").fg == Color::indexed(Grey));
    CHECK(pens.at("pick-bg").bg == Color::indexed(Black));
}

TEST_CASE("under IRC a left eyedropper pick takes the formatting of the cell it reads, as the editor's does (#419)",
          "[replay][sample-merged][irc]") {
    // a bold red block on the bottom layer, a clear cells layer over it
    project::Project p;
    Canvas c(4, 1);
    Cell bold;
    bold.ch = kFullBlock;
    bold.fg = Color::indexed(Red);
    bold.attrs = attr::Bold;
    c.at(1, 0) = bold;
    p.layers = LayerStack::fromCanvas(c);
    p.layers.add(makeLayer(LayerKind::Cells, 4, 1, "top"));
    p.target = Target::Irc;
    const auto pensFor = [&](const std::string& steps) {
        return pensAfter(R"({"version": 1, "start": {"blank": [4, 1]}, "steps": [)" + steps + "]}", p);
    };
    const auto pens = pensFor(R"({"name": "ground", "action": "eyedropper", "at": [1, 0]},
        {"name": "reset", "action": "eyedropper", "at": [0, 0]},
        {"name": "to-top", "action": "setActiveLayer", "layer": "top"},
        {"name": "own", "action": "eyedropper", "at": [1, 0]},
        {"name": "merged", "action": "eyedropper", "sampleMerged": true, "at": [1, 0]},
        {"name": "right", "action": "eyedropper", "button": "right", "at": [0, 0]})");
    CHECK(pens.at("ground").attrs == attr::Bold);
    CHECK(pens.at("reset").attrs == 0);
    CHECK(pens.at("own").attrs == 0);  // nothing picked from the clear cell
    CHECK(pens.at("merged").attrs == attr::Bold);
    CHECK(pens.at("merged").fg == Color::indexed(Red));
    CHECK(pens.at("right").attrs == attr::Bold);  // the right button picks no formatting
    // the glyph brush paints with the pen's formatting, as the editor's does
    const auto glyph = bwcli::replay::run(
        steps::parse(R"({"version": 1, "start": {"blank": [4, 1]}, "steps": [
            {"name": "to-top", "action": "setActiveLayer", "layer": "top"},
            {"name": "merged", "action": "eyedropper", "sampleMerged": true, "at": [1, 0]},
            {"name": "glyph", "action": "glyph", "glyph": "A", "points": [[2, 0]]}]})"),
        p);
    const Cell& a = layerNamed(glyph.captures.back().project, "top").cells.at(2, 0);
    CHECK(a.ch == U'A');
    CHECK(a.attrs == attr::Bold);
    // under ANSI the pen keeps no formatting from a pick
    p.target = Target::Ansi;
    CHECK(pensFor(R"({"name": "ground", "action": "eyedropper", "at": [1, 0]})").at("ground").attrs == 0);
}

TEST_CASE("the Sample merged steps file writes the bytes saved for it (#419)", "[replay][sample-merged]") {
    const steps::Script s = steps::parse(bwcli::readFile((kReplay / "sample-merged.json").string()));
    const bwcli::replay::Outcome out = bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples));
    CHECK(out.notes.empty());
    std::set<std::string> saved;
    for (const auto& e : fs::directory_iterator(kReplay / "sample-merged"))
        saved.insert(e.path().filename().string());
    std::set<std::string> written;
    for (const auto& c : out.captures) {
        INFO(c.stem);
        written.insert(c.stem + ".ans");
        const fs::path golden = kReplay / "sample-merged" / (c.stem + ".ans");
        REQUIRE(fs::exists(golden));
        CHECK(ansBytes(c.project) == bwcli::readFile(golden.string()));
    }
    CHECK(written == saved);
}

TEST_CASE("a clone step copies the active layer's own cells, whatever sampleMerged an earlier fill left on (#418, #419)",
          "[replay][clone][sample-merged]") {
    // On the clear "paint" layer, the source (5, 0) is clear, though the
    // canvas shows the red bar of "ground" below it there: a merged read
    // would stamp red. The clone has no Sample merged, so it stamps the
    // clear cell, with or without one left on by a fill step before it.
    std::vector<std::string> bytes;
    for (const char* before : {"", R"({"name": "merged", "action": "fill", "sampleMerged": true}, )"}) {
        INFO("before: " << before);
        const auto out = layered(std::string(before) + R"({"name": "to-paint", "action": "setActiveLayer", "layer": "paint"},
            {"name": "stamp", "action": "clone", "source": [5, 0], "size": 3, "points": [[1, 1], [2, 1]]})");
        const project::Project& p = out.captures.back().project;
        REQUIRE(out.captures.back().stem.find("stamp") != std::string::npos);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 20; ++x)
                CHECK(layerNamed(p, "paint").cells.at(x, y).transparent);
        CHECK(columnsOf(layerNamed(p, "ground"), 0, Red) == std::vector<int>{5});
        bytes.push_back(ansBytes(p));
    }
    CHECK(bytes[0] == bytes[1]);
    // the clone step leaves Sample merged as it found it: a fill after it
    // still floods the canvas's black region in columns 0-4, not the whole
    // clear "paint" layer
    {
        const auto out = layered(R"({"name": "merged", "action": "fill", "sampleMerged": true},
            {"name": "to-paint", "action": "setActiveLayer", "layer": "paint"},
            {"name": "stamp", "action": "clone", "source": [5, 0], "points": [[1, 1]]},
            {"name": "flood", "action": "fill", "fg": 14, "at": [2, 2]})");
        const Layer& paint = layerNamed(out.captures.back().project, "paint");
        for (int y = 0; y < 4; ++y) {
            INFO("row " << y);
            CHECK(columnsOf(paint, y, Yellow) == span(0, 4));
        }
    }
    // and on "ground" the same stroke stamps its red bar
    const auto out = layered(R"({"name": "merged", "action": "fill", "sampleMerged": true},
        {"name": "stamp", "action": "clone", "source": [5, 0], "points": [[1, 1], [1, 2]]})");
    CHECK(columnsOf(layerNamed(out.captures.back().project, "ground"), 1, Red) == std::vector<int>{1, 5});
}

namespace {

// The text layer makeTextLayer makes of the Text tool's defaults with
// `options` and `text`, at (x, y), named `name`, from no font file.
Layer textLayer(const std::string& text, int x, int y, const std::string& name,
                const steps::Options::Text& options = {}) {
    TextSource s;
    REQUIRE_FALSE(steps::applyTextOptions(s, options));
    s.text = QString::fromStdString(text).toStdU32String();
    return makeTextLayer(std::move(s), name, x, y, FontSources{});
}

void checkSameText(const Layer& got, const Layer& want) {
    CHECK(got.props.name == want.props.name);
    REQUIRE(got.props.text);
    CHECK(*got.props.text == *want.props.text);
    CHECK(got.bounds() == want.bounds());
    CHECK(got.cells == want.cells);
}

} // namespace

TEST_CASE("a text step makes a text layer as the Text tool's first keystroke does (#421)", "[replay][text]") {
    const auto out = blank(R"({"name": "say", "action": "text", "at": [2, 1], "text": "Hi\nyo"})");
    const project::Project& p = out.captures.back().project;
    CHECK(layerNames(p) == std::vector<std::string>{"Background", "Text 1"});
    checkSameText(layerNamed(p, "Text 1"), textLayer("Hi\nyo", 2, 1, "Text 1"));
    CHECK(layerNamed(p, "Text 1").props.text->face.name == "block");
    CHECK(out.notes.empty());
    // the text shows on the canvas
    CHECK(flat(p) != flat(blank(R"({"name": "a", "action": "selectAll"})").captures.back().project));
}

TEST_CASE("text layers are named Text N, go above the active layer and become active (#421)", "[replay][text]") {
    const auto out = blank(R"({"name": "ink", "action": "newCellsLayer", "rename": "ink"},
        {"name": "low", "action": "setActiveLayer", "layer": "Background"},
        {"name": "one", "action": "text", "at": [0, 0], "text": "a"},
        {"name": "two", "action": "text", "at": [0, 8], "text": "b"},
        {"name": "top", "action": "newCellsLayer"})");
    const project::Project& p = out.captures.back().project;
    // each above the active layer, the one made before it
    CHECK(layerNames(p) == std::vector<std::string>{"Background", "Text 1", "Text 2", "Layer 1", "ink"});
    // a free name: Text 1 renamed away, the next is Text 1 again
    const auto again = blank(R"({"name": "one", "action": "text", "at": [0, 0], "text": "a"},
        {"name": "re", "action": "layerProperties", "rename": "title"},
        {"name": "two", "action": "text", "at": [0, 8], "text": "b"},
        {"name": "three", "action": "text", "at": [10, 8], "text": "c"})");
    CHECK(layerNames(again.captures.back().project) == std::vector<std::string>{"Background", "title", "Text 1", "Text 2"});
}

TEST_CASE("a text step's Tool Options set its layer and hold for later text steps (#421)", "[replay][text]") {
    const auto out = blank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB",
            "textOptions": {"face": "vga", "colour": 12, "letterSpacing": 1}},
        {"name": "two", "action": "text", "at": [1, 9], "text": "cd", "textOptions": {"case": "capitals"}})");
    const project::Project& p = out.captures.back().project;
    steps::Options::Text one;
    one.face = "vga";
    one.colour = 12;
    one.letterSpacing = 1;
    checkSameText(layerNamed(p, "Text 1"), textLayer("AB", 1, 1, "Text 1", one));
    steps::Options::Text two = one;
    two.textCase = TextCase::Capitals;
    checkSameText(layerNamed(p, "Text 2"), textLayer("cd", 1, 9, "Text 2", two));
    // the case is the layer's setting: the text is kept as typed
    CHECK(layerNamed(p, "Text 2").props.text->text == U"cd");
}

TEST_CASE("a text step with Tool Options alone restyles the active text layer, as Tool Options does (#421)",
          "[replay][text]") {
    const auto out = blank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB"},
        {"name": "red", "action": "text", "textOptions": {"colour": 4, "face": "mini"}})");
    REQUIRE(out.captures.size() == 2);
    const project::Project& p = out.captures.back().project;
    steps::Options::Text red;
    red.colour = 4;
    red.face = "mini";
    checkSameText(layerNamed(p, "Text 1"), textLayer("AB", 1, 1, "Text 1", red));
    CHECK(out.notes.empty());
    // and the next layer starts from the restyled layer's settings
    const auto next = blank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB",
            "textOptions": {"align": "right"}},
        {"name": "red", "action": "text", "textOptions": {"colour": 4}},
        {"name": "two", "action": "text", "at": [1, 7], "text": "c"})");
    steps::Options::Text both;
    both.align = TextAlign::Right;
    both.colour = 4;
    checkSameText(layerNamed(next.captures.back().project, "Text 2"), textLayer("c", 1, 7, "Text 2", both));
}

TEST_CASE("Tool Options alone on a locked text layer set the defaults from it, as the dock shows it (#421)",
          "[replay][text]") {
    // the dock shows the active text layer's settings, locked or not, and
    // sets the defaults from them; only an unlocked layer takes the change
    const auto locked = blank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB",
            "textOptions": {"letterSpacing": 2}},
        {"name": "lock", "action": "layerProperties", "locked": true},
        {"name": "red", "action": "text", "textOptions": {"colour": 4}},
        {"name": "two", "action": "text", "at": [1, 7], "text": "c"})");
    const project::Project& p = locked.captures.back().project;
    steps::Options::Text spaced;
    spaced.letterSpacing = 2;
    checkSameText(layerNamed(p, "Text 1"), textLayer("AB", 1, 1, "Text 1", spaced));
    steps::Options::Text both = spaced;
    both.colour = 4;
    checkSameText(layerNamed(p, "Text 2"), textLayer("c", 1, 7, "Text 2", both));

    // a cells layer active: the defaults are what the dock shows
    const auto cells = blank(R"({"name": "red", "action": "text", "textOptions": {"colour": 4}},
        {"name": "two", "action": "text", "at": [1, 7], "text": "c"})");
    steps::Options::Text red;
    red.colour = 4;
    checkSameText(layerNamed(cells.captures.back().project, "Text 1"), textLayer("c", 1, 7, "Text 1", red));
}

TEST_CASE("a big style picked in a text step resets its colours and face as Tool Options does (#421)",
          "[replay][text]") {
    // from a vga face: the face goes to block and the style's colours come in
    const auto out = blank(R"({"name": "vga", "action": "text", "textOptions": {"face": "vga"}},
        {"name": "big", "action": "text", "at": [1, 1], "text": "a", "textOptions": {"bigColours": [9, 1], "bigStyle": "shadow"}},
        {"name": "3d", "action": "text", "textOptions": {"bigStyle": "3d"}},
        {"name": "none", "action": "text", "at": [14, 1], "text": "b", "textOptions": {"bigStyle": "none"}})");
    const project::Project& p = out.captures.back().project;
    steps::Options::Text threeD;
    threeD.bigStyle = BigTextStyle::ThreeD;
    checkSameText(layerNamed(p, "Text 1"), textLayer("a", 1, 1, "Text 1", threeD));
    const Layer& t1 = layerNamed(p, "Text 1");
    CHECK(t1.props.text->bigColours == bigTextDefaultColours(BigTextStyle::ThreeD));
    CHECK(t1.props.text->face == FaceRef{});
    // none clears the colours; the face stays block
    const Layer& t2 = layerNamed(p, "Text 2");
    CHECK(t2.props.text->bigStyle == BigTextStyle::None);
    CHECK(t2.props.text->bigColours.empty());
    CHECK(t2.props.text->face == FaceRef{});
}

TEST_CASE("Tool Options alone that change nothing on the active text layer say so (#421)", "[replay][text]") {
    const auto out = blank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB", "textOptions": {"colour": 4}},
        {"name": "same", "action": "text", "textOptions": {"colour": 4}})");
    REQUIRE(out.notes.size() == 1);
    CHECK_THAT(out.notes[0], ContainsSubstring("step 2 (same)"));
    CHECK_THAT(out.notes[0], ContainsSubstring("changes nothing"));
}
TEST_CASE("a text step with no Tool Options, or the ones the dock shows, keeps the defaults (#421)", "[replay][text]") {
    // the defaults were last set to vga; "A" (mini) made active doesn't change them
    for (const char* step : {R"({"name": "none", "action": "text"})",
                             R"({"name": "empty", "action": "text", "textOptions": {}})",
                             R"({"name": "same", "action": "text", "textOptions": {"face": "mini"}})"}) {
        INFO(step);
        const auto out = blank(std::string(R"({"name": "a", "action": "text", "at": [0, 0], "text": "A", "textOptions": {"face": "mini"}},
            {"name": "b", "action": "text", "at": [5, 0], "text": "B", "textOptions": {"face": "vga"}},
            {"name": "use", "action": "setActiveLayer", "layer": "Text 1"},
            )") + step + R"(,
            {"name": "c", "action": "text", "at": [14, 0], "text": "C"})");
        steps::Options::Text vga;
        vga.face = "vga";
        checkSameText(layerNamed(out.captures.back().project, "Text 3"), textLayer("C", 14, 0, "Text 3", vga));
    }
    // on a locked one or a cells layer too
    const auto locked = blank(R"({"name": "a", "action": "text", "at": [0, 0], "text": "A", "textOptions": {"face": "mini"}},
        {"name": "b", "action": "text", "at": [5, 0], "text": "B", "textOptions": {"face": "vga"}},
        {"name": "use", "action": "setActiveLayer", "layer": "Text 1"},
        {"name": "lock", "action": "layerProperties", "locked": true},
        {"name": "same", "action": "text", "textOptions": {"face": "mini"}},
        {"name": "c", "action": "text", "at": [14, 0], "text": "C"})");
    CHECK(layerNamed(locked.captures.back().project, "Text 3").props.text->face.name == "vga");
    REQUIRE(locked.notes.size() == 1);
    CHECK_THAT(locked.notes[0], ContainsSubstring("step 5 (same): changes nothing"));
    const auto cells = blank(R"({"name": "same", "action": "text", "textOptions": {"face": "block"}})");
    REQUIRE(cells.notes.size() == 1);
    CHECK_THAT(cells.notes[0], ContainsSubstring("step 1 (same): changes nothing"));
}
TEST_CASE("Tool Options the dock already shows change nothing on a text layer a project file left untidy (#421)",
          "[replay][text]") {
    // a project may hold a big style with no colours of its own (drawn in the
    // style's) or colours with no style; the dock shows them filled in and dropped
    for (const bool big : {true, false}) {
        INFO("big " << big);
        project::Project p;
        p.layers = LayerStack(40, 20);
        p.layers.add(makeLayer(LayerKind::Cells, 40, 20, "Background"));
        TextSource source;
        source.text = U"A";
        source.bigStyle = big ? BigTextStyle::Shadow : BigTextStyle::None;
        source.bigColours = big ? std::vector<std::uint8_t>{} : std::vector<std::uint8_t>{4};
        p.layers.add(makeTextLayer(source, "Big", 1, 1, FontSources{}));
        const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [1, 1]}, "steps": [
            {"name": "use", "action": "setActiveLayer", "layer": "Big"},
            {"name": "empty", "action": "text", "textOptions": {}},
            {"name": "left", "action": "text", "textOptions": {"align": "left"}},
            {"name": "c", "action": "text", "at": [30, 15], "text": "C"}]})");
        const auto out = bwcli::replay::run(s, p);
        REQUIRE(out.notes.size() == 2);
        CHECK_THAT(out.notes[0], ContainsSubstring("step 2 (empty): changes nothing"));
        CHECK_THAT(out.notes[1], ContainsSubstring("step 3 (left): changes nothing"));
        const project::Project& end = out.captures.back().project;
        CHECK(*layerNamed(end, "Big").props.text == source);
        checkSameText(layerNamed(end, "Text 1"), textLayer("C", 30, 15, "Text 1"));
    }
}



TEST_CASE("a text step's click on a text layer the tool would edit is refused (#421)", "[replay][text]") {
    CHECK_THAT(onBlank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB"},
                          {"name": "two", "action": "text", "at": [2, 2], "text": "c"})"),
               ContainsSubstring("step 2 (two): cell (2, 2) is on the text layer \"Text 1\""));
    // a hidden or locked text layer isn't edited by a click there: the step makes a new layer over it
    for (const char* prop : {R"("visible": false)", R"("locked": true)"}) {
        INFO(prop);
        const auto out = blank(std::string(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB"},
            {"name": "p", "action": "layerProperties", )") + prop + R"(},
            {"name": "two", "action": "text", "at": [2, 2], "text": "c"})");
        checkSameText(layerNamed(out.captures.back().project, "Text 2"), textLayer("c", 2, 2, "Text 2"));
    }
    // nor a cell outside the layer's box
    const Layer one = textLayer("AB", 1, 1, "Text 1");
    const int right = one.bounds().x + one.bounds().width;
    const auto beside = blank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "AB"},
        {"name": "two", "action": "text", "at": [)" + std::to_string(right) + R"(, 1], "text": "c"})");
    CHECK(layerNames(beside.captures.back().project).size() == 3);
}

TEST_CASE("a text step's cell off the canvas is refused (#421)", "[replay][text]") {
    CHECK_THAT(onBlank(R"({"name": "off", "action": "text", "at": [20, 0], "text": "a"})"),
               ContainsSubstring("step 1 (off)"));
    CHECK_THAT(onBlank(R"({"name": "off", "action": "text", "at": [0, 10], "text": "a"})"),
               ContainsSubstring("step 1 (off)"));
}

TEST_CASE("the text set writes the bytes saved for it (#421)", "[replay][text]") {
    const steps::Script s = steps::parse(bwcli::readFile((kReplay / "text.json").string()));
    const bwcli::replay::Outcome out = bwcli::replay::run(s, bwcli::replay::start(s, kReplay, kSamples));
    CHECK(out.notes.empty());
    std::set<std::string> saved;
    for (const auto& e : fs::directory_iterator(kReplay / "text"))
        saved.insert(e.path().filename().string());
    std::set<std::string> written;
    for (const auto& c : out.captures) {
        INFO(c.stem);
        // a blank start: no SAUCE record of its own, so the day is pinned
        CHECK_FALSE(c.project.sauce);
        written.insert(c.stem + ".ans");
        const fs::path golden = kReplay / "text" / (c.stem + ".ans");
        REQUIRE(fs::exists(golden));
        CHECK(ansBytes(c.project) == bwcli::readFile(golden.string()));
    }
    CHECK(written == saved);
    CHECK(out.captures.size() == s.steps.size());
}

TEST_CASE("Tool Options a big style turns off are refused naming the step, as the dock can't set them (#421)",
          "[replay][text]") {
    CHECK_THAT(onBlank(R"({"name": "big", "action": "text", "at": [1, 1], "text": "a",
                          "textOptions": {"face": "vga", "bigStyle": "shadow"}})"),
               ContainsSubstring("step 1 (big): \"textOptions face\" \"vga\" can't be picked under a big style"));
    CHECK_THAT(onBlank(R"({"name": "one", "action": "text", "at": [1, 1], "text": "a"},
                          {"name": "big", "action": "text", "textOptions": {"face": "vga", "bigStyle": "shadow"}})"),
               ContainsSubstring("step 2 (big): \"textOptions face\" \"vga\" can't be picked"));
    CHECK_THAT(onBlank(R"({"name": "big", "action": "text", "textOptions": {"bigStyle": "shadow"}},
                          {"name": "red", "action": "text", "at": [1, 1], "text": "a", "textOptions": {"colour": 4}})"),
               ContainsSubstring("step 2 (red): \"textOptions colour\" can't be set under a big style"));
    CHECK_THAT(onBlank(R"({"name": "cols", "action": "text", "at": [1, 1], "text": "a",
                          "textOptions": {"bigColours": [4]}})"),
               ContainsSubstring("step 1 (cols): \"textOptions bigColours\" needs a big style"));
}
