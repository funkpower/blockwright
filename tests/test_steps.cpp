// The steps file parser (blockwright/steps.h, issue #208): what it reads, and
// that what it refuses names the step, or the start, it is in.

#include "blockwright/big_text.h"
#include "blockwright/face.h"
#include "blockwright/library.h"
#include "blockwright/project.h"
#include "blockwright/steps.h"
#include "blockwright/tdf.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

using namespace bw;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;

namespace {

// A steps file starting blank with `steps` as its steps array's contents.
std::string script(const std::string& steps) {
    return R"({"version": 1, "start": {"blank": [20, 10]}, "steps": [)" + steps + "]}";
}

// What parse refuses `json` with.
std::string refusal(const std::string& json) {
    try {
        steps::parse(json);
    } catch (const steps::StepsError& e) {
        return e.what();
    }
    return "(accepted)";
}

} // namespace

TEST_CASE("a steps file reads its start, options and every step's fields", "[steps]") {
    const steps::Script s = steps::parse(R"({
        "version": 1,
        "start": {"blank": [40, 12]},
        "ice": false,
        "steps": [
            {"name": "head", "action": "outline", "ramp": "steel", "size": 3, "pressure": "size",
             "symmetry": {"mode": "radial", "folds": 6}, "button": "right",
             "points": [[1, 2], [3, 4, 0.5]], "capture": "before-after"},
            {"name": "say", "action": "keyboard", "fg": 15, "bg": 1, "at": [2, 3], "text": "hi\nyo"},
            {"name": "box", "action": "rectangle", "filled": true, "from": [0, 0], "to": [5, 4]},
            {"name": "pick", "action": "select", "rect": [1, 1, 4, 3]},
            {"name": "shift", "action": "moveSelection", "by": [2, -1], "pasteMode": "transparent"},
            {"name": "drop", "action": "paste", "at": [7, 7]},
            {"name": "dots", "action": "newPixelLayer", "rename": "dots"},
            {"name": "back", "action": "setActiveLayer", "layer": "Background"},
            {"name": "hide", "action": "layerProperties", "visible": false, "alphaLocked": true}
        ]
    })");
    CHECK(s.start.kind == steps::Start::Kind::Blank);
    CHECK(s.start.width == 40);
    CHECK(s.start.height == 12);
    REQUIRE(s.ice);
    CHECK_FALSE(*s.ice);
    REQUIRE(s.steps.size() == 9);

    const steps::Step& head = s.steps[0];
    CHECK(head.number == 1);
    CHECK(steps::stepLabel(head) == "step 1 (head)");
    CHECK(head.action == "outline");
    CHECK(head.capture == steps::Capture::BeforeAfter);
    CHECK(head.button == steps::Button::Right);
    REQUIRE(head.options.ramp);
    CHECK(head.options.ramp->name == "steel");
    CHECK(head.options.size == 3);
    CHECK(head.options.pressure == PressureTo::Size);
    REQUIRE(head.options.symmetry);
    CHECK(head.options.symmetry->mode == SymmetryMode::Radial);
    CHECK(head.options.symmetry->folds == 6);
    CHECK(head.points == std::vector<steps::Point>{{1, 2, 1.0}, {3, 4, 0.5}});

    const steps::Step& say = s.steps[1];
    CHECK(say.capture == steps::Capture::After);
    CHECK(say.options.fg == 15);
    CHECK_FALSE(say.options.pressure);
    CHECK(say.options.bg == 1);
    CHECK(say.points == std::vector<steps::Point>{{2, 3, 1.0}});
    CHECK(say.text == U"hi\nyo");

    CHECK(s.steps[2].options.filled == true);
    CHECK(s.steps[2].points == std::vector<steps::Point>{{0, 0, 1.0}, {5, 4, 1.0}});
    REQUIRE(s.steps[3].rect);
    CHECK(*s.steps[3].rect == CellRect{1, 1, 4, 3});
    CHECK(s.steps[4].dx == 2);
    CHECK(s.steps[4].dy == -1);
    CHECK(s.steps[4].options.pasteMode == PasteMode::Transparent);
    CHECK(s.steps[5].points == std::vector<steps::Point>{{7, 7, 1.0}});
    CHECK(s.steps[6].rename == "dots");
    CHECK(s.steps[7].layer == "Background");
    CHECK(s.steps[8].visible == false);
    CHECK(s.steps[8].alphaLocked == true);
    CHECK_FALSE(s.steps[8].locked);
    CHECK_FALSE(s.steps[8].rename);
}

TEST_CASE("a steps file reads the window's size and each step's screenshot", "[steps][screenshot]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 10]}, "window": [1024, 720],
        "steps": [
            {"name": "plain", "action": "selectAll"},
            {"name": "all", "action": "selectAll", "screenshot": {}},
            {"name": "dock", "action": "rampShade", "ramp": "steel",
             "screenshot": {"of": "toolOptionsDock", "when": "before"}},
            {"name": "cells", "action": "deselect", "screenshot": {"crop": {"cells": [1, 2, 3, 4]}, "when": "both"}},
            {"name": "pixels", "action": "deselect", "screenshot": {"of": "layers", "crop": {"pixels": [5, 6, 7, 8]}}}
        ]})");
    CHECK(s.windowWidth == 1024);
    CHECK(s.windowHeight == 720);
    REQUIRE(s.steps.size() == 5);
    using Shot = steps::Screenshot;
    CHECK_FALSE(s.steps[0].screenshot);

    REQUIRE(s.steps[1].screenshot);
    const Shot& all = *s.steps[1].screenshot;
    CHECK(all.of == "window");
    CHECK(all.crop == Shot::Crop::None);
    CHECK(all.when == Shot::When::After);

    REQUIRE(s.steps[2].screenshot);
    CHECK(s.steps[2].screenshot->of == "toolOptionsDock");
    CHECK(s.steps[2].screenshot->when == Shot::When::Before);
    CHECK(s.steps[2].options.ramp->name == "steel");

    REQUIRE(s.steps[3].screenshot);
    const Shot& cells = *s.steps[3].screenshot;
    CHECK(cells.crop == Shot::Crop::Cells);
    CHECK((cells.x == 1 && cells.y == 2 && cells.width == 3 && cells.height == 4));
    CHECK(cells.when == Shot::When::Both);

    REQUIRE(s.steps[4].screenshot);
    const Shot& pixels = *s.steps[4].screenshot;
    CHECK(pixels.of == "layers");
    CHECK(pixels.crop == Shot::Crop::Pixels);
    CHECK((pixels.x == 5 && pixels.y == 6 && pixels.width == 7 && pixels.height == 8));

    // without a "window", the window's replay picks its size
    CHECK_FALSE(steps::parse(script(R"({"name": "a", "action": "selectAll"})")).windowWidth);
}

TEST_CASE("a step's screenshot reads its scale, 1 without one (#295)", "[steps][screenshot]") {
    const steps::Script s = steps::parse(script(R"({"name": "one", "action": "selectAll", "screenshot": {}},
        {"name": "also", "action": "selectAll", "screenshot": {"scale": 1}},
        {"name": "two", "action": "selectAll", "screenshot": {"of": "layers", "scale": 2}})"));
    REQUIRE(s.steps.size() == 3);
    CHECK(s.steps[0].screenshot->scale == 1);
    CHECK(s.steps[1].screenshot->scale == 1);
    CHECK(s.steps[2].screenshot->scale == 2);
    for (const char* bad : {"0", "3", "1.5", "\"2\"", "true", "null", "[2]"}) {
        INFO(bad);
        try {
            steps::parse(script(std::string(R"({"name": "a", "action": "selectAll", "screenshot": {"scale": )") + bad +
                                "}}"));
            FAIL("accepted");
        } catch (const steps::StepsError& e) {
            CHECK(std::string(e.what()).rfind("step 1 (a): \"screenshot scale\" must be a whole number 1 to 2", 0) == 0);
        }
    }
}

TEST_CASE("a step's screenshot reads its pointer, off without one (#295)", "[steps][screenshot]") {
    const steps::Script s = steps::parse(script(R"({"name": "none", "action": "glyph", "points": [[1, 1]], "screenshot": {}},
        {"name": "off", "action": "glyph", "points": [[1, 1]], "screenshot": {"pointer": false}},
        {"name": "on", "action": "glyph", "points": [[1, 1], [4, 2]], "screenshot": {"pointer": true}})"));
    REQUIRE(s.steps.size() == 3);
    CHECK_FALSE(s.steps[0].screenshot->pointer);
    CHECK_FALSE(s.steps[1].screenshot->pointer);
    CHECK(s.steps[2].screenshot->pointer);
    for (const char* bad : {"1", "0", "\"true\"", "null", "[true]", "{}", "\"outline\""}) {
        INFO(bad);
        try {
            steps::parse(script(std::string(R"({"name": "a", "action": "glyph", "points": [[1, 1]], "screenshot": {"pointer": )") +
                                bad + "}}"));
            FAIL("accepted");
        } catch (const steps::StepsError& e) {
            CHECK(std::string(e.what()).rfind("step 1 (a): \"screenshot pointer\" must be true or false", 0) == 0);
        }
    }
}

TEST_CASE("a pointer sits at a step's last point after it and its first before it (#295)", "[steps][screenshot]") {
    const steps::Script s = steps::parse(script(R"({"name": "stroke", "action": "glyph", "points": [[1, 1], [2, 1], [5, 3]]},
        {"name": "drag", "action": "line", "from": [1, 2], "to": [9, 14]},
        {"name": "click", "action": "fill", "at": [3, 4]},
        {"name": "type", "action": "keyboard", "at": [2, 6], "text": "hi"},
        {"name": "paste", "action": "paste", "at": [7, 1]},
        {"name": "area", "action": "select", "rect": [8, 6, 2, 1]},
        {"name": "options", "action": "glyph"},
        {"name": "flip", "action": "flipHorizontal"},
        {"name": "shift", "action": "move", "by": [1, 0]},
        {"name": "look", "action": "view", "zoom": 2})"));
    const auto at = [&](int i, bool before) {
        const std::optional<steps::Point> p = steps::pointerAt(s.steps[std::size_t(i)], before);
        return p ? std::make_pair(p->x, p->y) : std::make_pair(-1, -1);
    };
    CHECK(at(0, false) == std::make_pair(5, 3));
    CHECK(at(0, true) == std::make_pair(1, 1));
    CHECK(at(1, false) == std::make_pair(9, 14));
    CHECK(at(1, true) == std::make_pair(1, 2));
    CHECK(at(2, false) == std::make_pair(3, 4));
    CHECK(at(2, true) == std::make_pair(3, 4));
    CHECK(at(3, false) == std::make_pair(2, 6));
    CHECK(at(4, false) == std::make_pair(7, 1));
    // a rect is dragged from its top-left corner to its bottom-right one,
    // whichever corners the file gives
    CHECK(at(5, true) == std::make_pair(2, 1));
    CHECK(at(5, false) == std::make_pair(8, 6));
    for (int i = 6; i < 10; ++i)
        for (const bool before : {false, true})
            CHECK(at(i, before) == std::make_pair(-1, -1));
}

TEST_CASE("a pointer on a step the pointer doesn't make is refused, naming the step (#295)", "[steps][screenshot]") {
    for (const char* step : {R"({"name": "no-points", "action": "glyph", "glyph": "#")",
                             R"({"name": "flip", "action": "flipHorizontal")",
                             R"({"name": "shift", "action": "moveSelection", "by": [1, 1])",
                             R"({"name": "layer", "action": "newCellsLayer")",
                             R"({"name": "look", "action": "view", "zoom": 2)"}) {
        INFO(step);
        const std::string name = std::string(step).substr(10, std::string(step).find('"', 10) - 10);
        try {
            steps::parse(script(std::string(step) + R"(, "screenshot": {"pointer": true}})"));
            FAIL("accepted");
        } catch (const steps::StepsError& e) {
            CHECK(std::string(e.what()).rfind("step 1 (" + name + "): \"screenshot pointer\" needs a step the pointer makes", 0) == 0);
        }
        // and without the pointer it is read
        CHECK_NOTHROW(steps::parse(script(std::string(step) + R"(, "screenshot": {"pointer": false}})")));
    }
}

TEST_CASE("a view step reads the zoom, a scroll cell and View > IRC's toggles, each left out unset (#398)",
          "[steps][view]") {
    const steps::Script s = steps::parse(script(R"(
        {"name": "all", "action": "view", "zoom": 4, "scrollTo": [12, 7], "ircFit": true, "ircPad": false,
         "ircSwapShades": true, "screenshot": {"of": "ircView"}},
        {"name": "zoom-only", "action": "view", "zoom": 8},
        {"name": "fit-off", "action": "view", "ircFit": false},
        {"name": "low", "action": "view", "zoom": 1, "scrollTo": [0, 0]},
        {"name": "plain", "action": "selectAll"})"));
    REQUIRE(s.steps.size() == 5);
    const steps::View& all = s.steps[0].view;
    CHECK(s.steps[0].action == "view");
    CHECK(all.zoom == 4);
    REQUIRE(all.scrollTo);
    CHECK((all.scrollTo->x == 12 && all.scrollTo->y == 7));
    CHECK(all.ircFit == true);
    CHECK(all.ircPad == false);
    CHECK(all.ircSwapShades == true);
    CHECK(s.steps[0].screenshot);

    const steps::View& zoom = s.steps[1].view;
    CHECK(zoom.zoom == 8);
    CHECK_FALSE(zoom.scrollTo);
    CHECK_FALSE(zoom.ircFit);
    CHECK_FALSE(zoom.ircPad);
    CHECK_FALSE(zoom.ircSwapShades);

    const steps::View& fit = s.steps[2].view;
    CHECK_FALSE(fit.zoom);
    CHECK(fit.ircFit == false);

    CHECK(s.steps[3].view.zoom == 1);
    CHECK(s.steps[3].view.scrollTo->x == 0);
    // another action has no view
    CHECK(s.steps[4].view == steps::View{});
    // the zoom levels are the editor's
    CHECK(steps::kMinZoom == 1);
    CHECK(steps::kMaxZoom == 8);
    CHECK(std::find(steps::actions().begin(), steps::actions().end(), "view") != steps::actions().end());
}

TEST_CASE("a view step's bad value is refused, naming the step (#398)", "[steps][view]") {
    const std::pair<const char*, const char*> cases[] = {
        {R"("zoom": 0)", "\"zoom\" must be a whole number 1 to 8"},
        {R"("zoom": 9)", "\"zoom\" must be a whole number 1 to 8"},
        {R"("zoom": 2.5)", "\"zoom\" must be a whole number 1 to 8"},
        {R"("zoom": "4")", "\"zoom\" must be a whole number 1 to 8, not a string"},
        {R"("scrollTo": [3])", "\"scrollTo\" must be [x, y], a cell"},
        {R"("scrollTo": {"x": 3, "y": 4})", "\"scrollTo\" must be [x, y], a cell"},
        {R"("scrollTo": [-1, 0])", "\"scrollTo x\" must be a whole number 0 to 4095"},
        {R"("scrollTo": [0, 4096])", "\"scrollTo y\" must be a whole number 0 to 4095"},
        {R"("ircFit": 1)", "\"ircFit\" must be true or false, not a number"},
        {R"("ircPad": "on")", "\"ircPad\" must be true or false, not a string"},
        {R"("ircSwapShades": null)", "\"ircSwapShades\" must be true or false, not null"},
        {R"("ircMeter": true)", "view takes no \"ircMeter\""},
        {R"("at": [1, 1])", "view takes no \"at\""},
        // the pen's Tool Options are not the window's
        {R"("zoom": 4, "fg": 2)", "view takes no \"fg\""},
        {R"("zoom": 4, "size": 3)", "view takes no \"size\""},
        {R"("ircFit": true, "pasteMode": "over")", "view takes no \"pasteMode\""},
        {R"("screenshot": {})", "view needs at least one of \"zoom\", \"scrollTo\", \"ircFit\", \"ircPad\" or \"ircSwapShades\""},
    };
    for (const auto& [fields, says] : cases) {
        INFO(fields);
        // the second step, so the error names that one
        const std::string why =
            refusal(script(std::string(R"({"name": "first", "action": "selectAll"}, {"name": "zoomed", "action": "view", )") +
                           fields + "}"));
        CHECK_THAT(why, ContainsSubstring(std::string("step 2 (zoomed): ") + says));
    }
    // and a zoom key on another action is refused as before
    CHECK_THAT(refusal(script(R"({"name": "z", "action": "glyph", "zoom": 4})")),
               ContainsSubstring("step 1 (z): glyph takes no \"zoom\""));
}

TEST_CASE("a canvasSize step reads the new size and the anchor, top-left when left out (#420)",
          "[steps][canvasSize]") {
    const steps::Script s = steps::parse(script(R"(
        {"name": "grow", "action": "canvasSize", "size": [30, 12], "anchor": "centre", "capture": "before-after"},
        {"name": "plain", "action": "canvasSize", "size": [1, 1]},
        {"name": "largest", "action": "canvasSize", "size": [400, 5000], "anchor": "bottom-right",
         "screenshot": {"of": "window"}})"));
    REQUIRE(s.steps.size() == 3);
    CHECK(s.steps[0].action == "canvasSize");
    CHECK(s.steps[0].canvasWidth == 30);
    CHECK(s.steps[0].canvasHeight == 12);
    CHECK(s.steps[0].anchor == Anchor::Centre);
    CHECK(s.steps[0].capture == steps::Capture::BeforeAfter);
    CHECK(s.steps[1].canvasWidth == 1);
    CHECK(s.steps[1].canvasHeight == 1);
    CHECK(s.steps[1].anchor == Anchor::TopLeft);
    CHECK(s.steps[2].canvasWidth == kMaxColumns);
    CHECK(s.steps[2].canvasHeight == kMaxRows);
    CHECK(s.steps[2].anchor == Anchor::BottomRight);
    CHECK(s.steps[2].screenshot);
    // no Tool Options: "size" is the canvas's, not the brush's
    CHECK_FALSE(s.steps[0].options.size);
    CHECK(std::find(steps::actions().begin(), steps::actions().end(), "canvasSize") != steps::actions().end());

    // every anchor the dialog has, by the dialog's names
    for (int i = 0; i < 9; ++i) {
        const Anchor a = Anchor(i);
        INFO(anchorName(a));
        const steps::Script one = steps::parse(script(std::string(R"({"name": "a", "action": "canvasSize", "size": [5, 5], "anchor": ")") +
                                                      std::string(anchorName(a)) + "\"}"));
        CHECK(one.steps[0].anchor == a);
    }
}

TEST_CASE("a canvasSize step's bad value is refused, naming the step (#420)", "[steps][canvasSize]") {
    const std::pair<const char*, const char*> cases[] = {
        {R"("anchor": "top")", "canvasSize needs \"size\": [width, height], the new canvas in cells"},
        {R"("size": 3)", "\"size\" must be [width, height], the new canvas in cells"},
        {R"("size": [3])", "\"size\" must be [width, height], the new canvas in cells"},
        {R"("size": [3, 2, 1])", "\"size\" must be [width, height], the new canvas in cells"},
        {R"("size": {"w": 3, "h": 2})", "\"size\" must be [width, height], the new canvas in cells"},
        {R"("size": [0, 3])", "\"size width\" must be a whole number 1 to 400"},
        {R"("size": [401, 3])", "\"size width\" must be a whole number 1 to 400"},
        {R"("size": [3.5, 2])", "\"size width\" must be a whole number 1 to 400"},
        {R"("size": ["3", 2])", "\"size width\" must be a whole number 1 to 400, not a string"},
        {R"("size": [3, 0])", "\"size height\" must be a whole number 1 to 5000"},
        {R"("size": [3, 5001])", "\"size height\" must be a whole number 1 to 5000"},
        {R"("size": [3, 2], "anchor": "middle")",
         "\"anchor\" takes top-left, top, top-right, left, centre, right, bottom-left, bottom, bottom-right, not \"middle\""},
        {R"("size": [3, 2], "anchor": "center")", "\"anchor\" takes top-left, top, top-right, left, centre, right, bottom-left, bottom, bottom-right, not \"center\""},
        {R"("size": [3, 2], "anchor": 4)", "\"anchor\" must be a string, not a number"},
        {R"("size": [3, 2], "at": [1, 1])", "canvasSize takes no \"at\""},
        {R"("size": [3, 2], "zoom": 2)", "canvasSize takes no \"zoom\""},
        {R"("size": [3, 2], "width": 3)", "canvasSize takes no \"width\""},
        // the pen's Tool Options are not the canvas's
        {R"("size": [3, 2], "fg": 2)", "canvasSize takes no \"fg\""},
        {R"("size": [3, 2], "pasteMode": "over")", "canvasSize takes no \"pasteMode\""},
        // nothing for a pointer to show
        {R"("size": [3, 2], "screenshot": {"pointer": true})",
         "\"screenshot pointer\" needs a step the pointer makes"},
    };
    for (const auto& [fields, says] : cases) {
        INFO(fields);
        const std::string why =
            refusal(script(std::string(R"({"name": "first", "action": "selectAll"}, {"name": "grow", "action": "canvasSize", )") +
                           fields + "}"));
        CHECK_THAT(why, ContainsSubstring(std::string("step 2 (grow): ") + says));
    }
    // and an anchor on another action is refused as before
    CHECK_THAT(refusal(script(R"({"name": "z", "action": "glyph", "anchor": "top"})")),
               ContainsSubstring("step 1 (z): glyph takes no \"anchor\""));
}

TEST_CASE("a steps file starts from a file or a sample", "[steps]") {
    const steps::Script f = steps::parse(R"({"version": 1, "start": {"file": "art/piece.bwp"}, "steps": [{"name": "all", "action": "selectAll"}]})");
    CHECK(f.start.kind == steps::Start::Kind::File);
    CHECK(f.start.file == "art/piece.bwp");

    const steps::Script s =
        steps::parse(R"({"version": 1, "start": {"sample": "01-layers-showcase", "variant": "Day"},
                                  "steps": [{"name": "all", "action": "selectAll"}]})");
    CHECK(s.start.kind == steps::Start::Kind::Sample);
    CHECK(s.start.sample == "01-layers-showcase");
    CHECK(s.start.variant == "Day");
}

TEST_CASE("a steps file's paths are relative to it, never absolute (#422)", "[steps]") {
    // relative, .. included: tutorials may share a folder beside theirs
    for (const char* ok : {"piece.ans", "art/piece.bwp", "../brushes/sphere-steel.ans", "./a.ans", "..", "~/x.ans"}) {
        INFO(ok);
        CHECK_FALSE(steps::pathProblem(ok));
        CHECK(steps::parse(std::string(R"({"version": 1, "start": {"file": ")") + ok +
                           R"("}, "steps": [{"name": "all", "action": "selectAll"}]})")
                  .start.file == ok);
    }
    const std::string relative = "; a steps file names its files relative to its own directory";
    const std::pair<std::string, std::string> refused[] = {
        {"", "is empty"},
        {std::string("a\0b.ans", 7), "holds a NUL character"},
        {"/tmp/piece.ans", "\"/tmp/piece.ans\" is absolute" + relative},
        {"/", "\"/\" is absolute" + relative},
        {"C:/art/piece.ans", "\"C:/art/piece.ans\" is absolute" + relative},
        {"c:piece.ans", "\"c:piece.ans\" is absolute" + relative},
        {"art\\piece.ans", "\"art\\piece.ans\" has a \\ in it; write / between directories, relative to the steps file"},
        {"\\piece.ans", "\"\\piece.ans\" has a \\ in it; write / between directories, relative to the steps file"},
        {"\\\\host\\share\\piece.ans",
         "\"\\\\host\\share\\piece.ans\" has a \\ in it; write / between directories, relative to the steps file"},
    };
    for (const auto& [path, why] : refused) {
        INFO(path);
        CHECK(steps::pathProblem(path) == why);
    }
    // the start's "file" is refused so, naming the start and the key: an
    // absolute start was taken as it stood before #422
    const auto startOf = [](const std::string& file) {
        try {
            steps::parse(R"({"version": 1, "start": {"file": )" + file +
                         R"(}, "steps": [{"name": "all", "action": "selectAll"}]})");
        } catch (const steps::StepsError& e) {
            return std::string(e.what());
        }
        return std::string("parsed");
    };
    CHECK(startOf(R"("/tmp/piece.ans")") == "start: \"file\" \"/tmp/piece.ans\" is absolute" + relative);
    CHECK(startOf(R"("C:\\art\\piece.ans")") ==
          "start: \"file\" \"C:\\art\\piece.ans\" has a \\ in it; write / between directories, relative to the "
          "steps file");
    CHECK(startOf(R"("")") == "start: \"file\" is empty");
    CHECK(startOf(R"("a\u0000b")") == "start: \"file\" holds a NUL character");
}

TEST_CASE("every action the steps file lists parses", "[steps]") {
    CHECK(steps::actions().size() > 30);
    CHECK(steps::pixelAction("halfBlock"));
    CHECK(steps::pixelAction("line"));
    CHECK_FALSE(steps::pixelAction("outline"));
    CHECK_FALSE(steps::pixelAction("keyboard"));
}

TEST_CASE("the pressure mode takes the Tool Options names (#382)", "[steps]") {
    const std::pair<const char*, PressureTo> modes[] = {
        {"none", PressureTo::Nothing}, {"size", PressureTo::Size}, {"strength", PressureTo::Strength}};
    for (const auto& [name, mode] : modes) {
        INFO(name);
        const steps::Script s =
            steps::parse(script(std::string(R"({"name": "a", "action": "glyph", "pressure": ")") + name + "\"}"));
        CHECK(s.steps[0].options.pressure == mode);
    }
}

TEST_CASE("what a steps file refuses names the step or the start", "[steps]") {
    struct Case {
        const char* what;
        std::string json;
        const char* names;
        const char* says;
    };
    const Case cases[] = {
        {"not JSON", "{", "", "JSON"},
        {"not an object", "[]", "", ""},
        {"another version", R"({"version": 2, "start": {"blank": [4, 4]}, "steps": []})", "", "version"},
        {"an unknown top-level key", R"({"version": 1, "start": {"blank": [4, 4]}, "steps": [], "x": 1})", "", "x"},
        {"no steps", R"({"version": 1, "start": {"blank": [4, 4]}, "steps": []})", "", "steps"},
        {"no start", R"({"version": 1, "steps": []})", "", "start"},
        {"two starts", R"({"version": 1, "start": {"blank": [4, 4], "file": "a.ans"}, "steps": []})", "start", ""},
        {"a blank too big", R"({"version": 1, "start": {"blank": [5000, 4]}, "steps": []})", "start", ""},
        {"ice with a file", R"({"version": 1, "start": {"file": "a.ans"}, "ice": true, "steps": []})", "ice", ""},
        {"a variant with a file", R"({"version": 1, "start": {"file": "a.ans", "variant": "Day"}, "steps": []})",
         "start", "variant"},
        {"an unknown action", script(R"({"name": "a", "action": "teleport"})"), "step 1 (a)", "teleport"},
        {"no name", script(R"({"action": "selectAll"})"), "step 1", "name"},
        {"a bad name", script(R"({"name": "A B", "action": "selectAll"})"), "step 1", "name"},
        {"a key the action doesn't take",
         script(R"({"name": "a", "action": "selectAll"}, {"name": "b", "action": "flipHorizontal", "at": [1, 1]})"),
         "step 2 (b)", "at"},
        {"an unknown key", script(R"({"name": "a", "action": "fill", "at": [1, 1], "colour": 3})"), "step 1 (a)",
         "colour"},
        {"a colour past 15", script(R"({"name": "a", "action": "fill", "at": [1, 1], "fg": 16})"), "step 1 (a)",
         "fg"},
        {"an unknown ramp", script(R"({"name": "a", "action": "rampShade", "ramp": "plaid"})"), "step 1 (a)",
         "plaid"},
        {"a size out of range", script(R"({"name": "a", "action": "shade", "size": 0})"), "step 1 (a)", "size"},
        {"folds on a mirror", script(R"({"name": "a", "action": "shade", "symmetry": {"mode": "mirrorX", "folds": 4}})"),
         "step 1 (a)", "folds"},
        {"too many folds", script(R"({"name": "a", "action": "shade", "symmetry": {"mode": "radial", "folds": 9}})"),
         "step 1 (a)", "folds"},
        {"empty points", script(R"({"name": "a", "action": "shade", "points": []})"), "step 1 (a)", "points"},
        {"an unknown pressure mode", script(R"({"name": "a", "action": "shade", "pressure": "hard"})"), "step 1 (a)",
         "pressure"},
        {"a pressure mode as a number", script(R"({"name": "a", "action": "shade", "pressure": 0.5})"), "step 1 (a)",
         "pressure"},
        {"pressure past 1", script(R"({"name": "a", "action": "shade", "points": [[1, 1, 1.5]]})"), "step 1 (a)",
         "pressure"},
        {"a point not two numbers", script(R"({"name": "a", "action": "shade", "points": [[1]]})"), "step 1 (a)",
         "point"},
        {"from without to", script(R"({"name": "a", "action": "line", "from": [0, 0]})"), "step 1 (a)", "to"},
        {"text without at", script(R"({"name": "a", "action": "keyboard", "text": "hi"})"), "step 1 (a)", "at"},
        {"select without rect", script(R"({"name": "a", "action": "select"})"), "step 1 (a)", "rect"},
        {"move without by", script(R"({"name": "a", "action": "move"})"), "step 1 (a)", "by"},
        {"paste without at", script(R"({"name": "a", "action": "paste"})"), "step 1 (a)", "at"},
        {"an empty layerProperties", script(R"({"name": "a", "action": "layerProperties"})"), "step 1 (a)", ""},
        {"setActiveLayer without a layer", script(R"({"name": "a", "action": "setActiveLayer"})"), "step 1 (a)",
         "layer"},
        {"a pencil of 5", script(R"({"name": "a", "action": "halfBlock", "pencil": 5})"), "step 1 (a)", "pencil"},
        {"an unknown capture", script(R"({"name": "a", "action": "selectAll", "capture": "during"})"), "step 1 (a)",
         "capture"},
        {"an unknown paste mode", script(R"({"name": "a", "action": "paste", "at": [0, 0], "pasteMode": "glue"})"),
         "step 1 (a)", "pasteMode"},
        {"a window too small", R"({"version": 1, "start": {"blank": [4, 4]}, "window": [100, 600], "steps": []})",
         "top level", "window width"},
        {"a window not two numbers", R"({"version": 1, "start": {"blank": [4, 4]}, "window": [800], "steps": []})",
         "top level", "window"},
        {"a screenshot not an object", script(R"({"name": "a", "action": "selectAll", "screenshot": "window"})"),
         "step 1 (a)", "screenshot"},
        {"an unknown screenshot key",
         script(R"({"name": "a", "action": "selectAll", "screenshot": {"of": "window", "zoom": 2}})"), "step 1 (a)",
         "zoom"},
        {"an empty screenshot of", script(R"({"name": "a", "action": "selectAll", "screenshot": {"of": ""}})"),
         "step 1 (a)", "of"},
        {"an unknown screenshot when",
         script(R"({"name": "a", "action": "selectAll", "screenshot": {"when": "during"}})"), "step 1 (a)", "when"},
        {"a crop in cells and pixels",
         script(R"({"name": "a", "action": "selectAll",
                    "screenshot": {"crop": {"cells": [0, 0, 1, 1], "pixels": [0, 0, 1, 1]}}})"),
         "step 1 (a)", "crop"},
        {"a crop of no size",
         script(R"({"name": "a", "action": "selectAll", "screenshot": {"crop": {"cells": [0, 0, 0, 1]}}})"),
         "step 1 (a)", "crop cells width"},
        {"a crop not four numbers",
         script(R"({"name": "a", "action": "selectAll", "screenshot": {"crop": {"pixels": [0, 0, 4]}}})"),
         "step 1 (a)", "x, y, width, height"},
    };
    for (const Case& c : cases) {
        INFO(c.what);
        const std::string why = refusal(c.json);
        CHECK(why != "(accepted)");
        CHECK_THAT(why, ContainsSubstring(c.names));
        CHECK_THAT(why, ContainsSubstring(c.says));
    }
}

TEST_CASE("a clone step reads its source, stroke, button and Aligned (#418)", "[steps][clone]") {
    const steps::Script s = steps::parse(script(R"(
        {"name": "pick", "action": "clone", "source": [3, 4]},
        {"name": "stamp", "action": "clone", "points": [[10, 2], [12, 3, 0.5]], "button": "right", "aligned": false},
        {"name": "again", "action": "clone", "source": [0, 0], "points": [[5, 5]], "size": 3},
        {"name": "more", "action": "clone", "points": [[6, 6]]})"));
    REQUIRE(s.steps.size() == 4);
    CHECK(s.steps[0].action == "clone");
    CHECK(s.steps[0].source == steps::Point{3, 4});
    CHECK(s.steps[0].points.empty());
    CHECK_FALSE(s.steps[0].options.aligned);
    CHECK_FALSE(s.steps[1].source);
    CHECK(s.steps[1].points == std::vector<steps::Point>{{10, 2}, {12, 3, 0.5}});
    CHECK(s.steps[1].button == steps::Button::Right);
    CHECK(s.steps[1].options.aligned == false);
    CHECK(s.steps[2].source == steps::Point{0, 0});
    CHECK(s.steps[2].options.size == 3);
    // a later stroke uses the source an earlier clone step set
    CHECK_FALSE(s.steps[3].source);
    CHECK(std::find(steps::actions().begin(), steps::actions().end(), "clone") != steps::actions().end());
    CHECK_FALSE(steps::pixelAction("clone"));
}

TEST_CASE("a clone step's pointer is at its source before the step, and after it when it has no stroke (#418)",
          "[steps][clone][screenshot]") {
    const steps::Script s = steps::parse(script(R"(
        {"name": "pick", "action": "clone", "source": [3, 4], "screenshot": {"pointer": true, "when": "both"}},
        {"name": "both", "action": "clone", "source": [1, 2], "points": [[10, 2], [12, 3]],
         "screenshot": {"pointer": true, "when": "both"}},
        {"name": "stroke", "action": "clone", "points": [[7, 1], [8, 9]], "screenshot": {"pointer": true}},
        {"name": "options", "action": "clone", "aligned": false})"));
    CHECK(steps::pointerAt(s.steps[0], true) == steps::Point{3, 4});
    CHECK(steps::pointerAt(s.steps[0], false) == steps::Point{3, 4});
    CHECK(steps::pointerAt(s.steps[1], true) == steps::Point{1, 2});
    CHECK(steps::pointerAt(s.steps[1], false) == steps::Point{12, 3});
    CHECK(steps::pointerAt(s.steps[2], true) == steps::Point{7, 1});
    CHECK(steps::pointerAt(s.steps[2], false) == steps::Point{8, 9});
    // a step that only sets Aligned has no pointer, and can't ask for one
    CHECK_FALSE(steps::pointerAt(s.steps[3], true));
    CHECK_THAT(refusal(script(R"({"name": "o", "action": "clone", "aligned": true, "screenshot": {"pointer": true}})")),
               Equals("step 1 (o): \"screenshot pointer\" needs a step the pointer makes, with \"points\", \"from\" "
                      "and \"to\", \"at\", select's \"rect\" or a clone step's \"source\"; this clone step has none"));
}

TEST_CASE("a clone stroke with no source on it or an earlier clone step is refused, naming the step (#418)",
          "[steps][clone]") {
    const std::string needs = "clone needs a source to copy from";
    CHECK_THAT(refusal(script(R"({"name": "stamp", "action": "clone", "points": [[1, 1]]})")),
               ContainsSubstring("step 1 (stamp): " + needs));
    // a source on a later step is too late
    CHECK_THAT(refusal(script(R"({"name": "stamp", "action": "clone", "points": [[1, 1]]},
                                 {"name": "pick", "action": "clone", "source": [2, 2]})")),
               ContainsSubstring("step 1 (stamp): " + needs));
    // only a clone step sets one: another step's points don't
    CHECK_THAT(refusal(script(R"({"name": "p", "action": "glyph", "points": [[2, 2]]},
                                 {"name": "stamp", "action": "clone", "aligned": false, "points": [[1, 1]]})")),
               ContainsSubstring("step 2 (stamp): " + needs));
    // a step with no stroke needs none: it sets options, as choosing the tool does
    CHECK_NOTHROW(steps::parse(script(R"({"name": "opts", "action": "clone", "aligned": false})")));
    // and one earlier serves every later stroke
    CHECK_NOTHROW(steps::parse(script(R"({"name": "pick", "action": "clone", "source": [2, 2]},
                                         {"name": "p", "action": "glyph", "points": [[2, 2]]},
                                         {"name": "a", "action": "clone", "points": [[1, 1]]},
                                         {"name": "b", "action": "clone", "points": [[3, 1]]})")));
}

TEST_CASE("a clone step's bad value is refused, naming the step (#418)", "[steps][clone]") {
    const std::pair<const char*, const char*> cases[] = {
        {R"("source": 3)", "\"source\""},
        {R"("source": [3])", "\"source\""},
        {R"("source": [3, 4, 1])", "\"source\""},
        {R"("source": [3, 4, 0.5])", "\"source\""},
        {R"("source": [1.5, 4])", "\"source"},
        {R"("source": ["1", 4])", "\"source"},
        {R"("source": null)", "\"source\""},
        {R"("source": [1, 1], "aligned": 1)", "\"aligned\" must be true or false"},
        {R"("source": [1, 1], "aligned": "true")", "\"aligned\" must be true or false"},
        {R"("source": [1, 1], "aligned": null)", "\"aligned\" must be true or false"},
        {R"("source": [1, 1], "at": [1, 1])", "clone takes no \"at\""},
        {R"("source": [1, 1], "from": [1, 1])", "clone takes no \"from\""},
        {R"("source": [1, 1], "sampleMerged": true)", "clone takes no \"sampleMerged\""},
        {R"("source": [1, 1], "offset": [1, 1])", "clone takes no \"offset\""},
        {R"("source": [1, 1], "Aligned": true)", "clone takes no \"Aligned\""},
        {R"("source": [1, 1], "points": [[1, 1]], "button": "middle")", "\"button\""},
    };
    for (const auto& [fields, says] : cases) {
        INFO(fields);
        const std::string why = refusal(
            script(std::string(R"({"name": "first", "action": "selectAll"}, {"name": "cl", "action": "clone", )") + fields + "}"));
        CHECK_THAT(why, ContainsSubstring(std::string("step 2 (cl): ") + says));
    }
    // and the clone's keys on another action are refused
    for (const char* action : {"glyph", "smudge", "fill", "halfBlock", "selectAll", "view", "canvasSize"}) {
        INFO(action);
        CHECK_THAT(refusal(script(std::string(R"({"name": "z", "action": ")") + action + R"(", "source": [1, 1]})")),
                   ContainsSubstring(std::string("step 1 (z): ") + action + " takes no \"source\""));
        CHECK_THAT(refusal(script(std::string(R"({"name": "z", "action": ")") + action + R"(", "aligned": true})")),
                   ContainsSubstring(std::string("step 1 (z): ") + action + " takes no \"aligned\""));
    }
}

TEST_CASE("fill and the eyedropper read sampleMerged, unset without it (#419)", "[steps][sample-merged]") {
    const steps::Script s = steps::parse(script(R"({"name": "on", "action": "fill", "sampleMerged": true, "at": [1, 1]},
        {"name": "off", "action": "eyedropper", "sampleMerged": false, "at": [1, 1]},
        {"name": "set", "action": "fill", "sampleMerged": true},
        {"name": "plain", "action": "fill", "at": [1, 1]})"));
    REQUIRE(s.steps.size() == 4);
    CHECK(s.steps[0].options.sampleMerged == true);
    CHECK(s.steps[1].options.sampleMerged == false);
    // a fill step with no "at" only sets it, as choosing the tool does
    CHECK(s.steps[2].options.sampleMerged == true);
    CHECK(s.steps[2].points.empty());
    CHECK_FALSE(s.steps[3].options.sampleMerged.has_value());
}

TEST_CASE("sampleMerged of the wrong kind, or on a step that doesn't take it, is refused naming the step (#419)",
          "[steps][sample-merged]") {
    const std::pair<std::string, std::string> wrongKind[] = {
        {R"("sampleMerged": 1)", "\"sampleMerged\" must be true or false, not a number"},
        {R"("sampleMerged": "true")", "\"sampleMerged\" must be true or false, not a string"},
        {R"("sampleMerged": null)", "\"sampleMerged\" must be true or false, not null"},
        {R"("sampleMerged": [true])", "\"sampleMerged\" must be true or false, not an array"},
    };
    for (const char* action : {"fill", "eyedropper"}) {
        for (const auto& [field, says] : wrongKind) {
            INFO(action << " " << field);
            const std::string why = refusal(script(std::string(R"({"name": "first", "action": "selectAll"},
                {"name": "pick", "action": ")") + action + R"(", "at": [1, 1], )" + field + "}"));
            CHECK_THAT(why, ContainsSubstring("step 2 (pick): " + says));
        }
    }
    // only the two tools that show Sample merged in Tool Options take it,
    // the other half-block tools and the selection's paste included
    for (const char* action : {"halfBlock", "line", "rectangle", "ellipse", "shade", "glyph", "clone", "gradient",
                               "keyboard", "paste", "selectAll", "newCellsLayer", "setActiveLayer", "layerProperties", "view",
                               "canvasSize"}) {
        INFO(action);
        const std::string why = refusal(script(std::string(R"({"name": "first", "action": "selectAll"},
            {"name": "other", "action": ")") + action + R"(", "sampleMerged": true})"));
        CHECK_THAT(why, ContainsSubstring(std::string("step 2 (other): ") + action + " takes no \"sampleMerged\""));
    }
}

TEST_CASE("a text step reads its cell, its text as typed and its Tool Options (#421)", "[steps][text]") {
    const steps::Script s = steps::parse(script(R"({"name": "say", "action": "text", "at": [2, 3], "text": "é\nB",
            "textOptions": {"face": "vga", "case": "capitals", "letterSpacing": 2, "lineSpacing": 1, "align": "centre",
                            "wrap": 12, "colour": 12, "bigStyle": "3d", "bigColours": [4, 5]}},
        {"name": "set", "action": "text", "textOptions": {"colour": 3}},
        {"name": "plain", "action": "text", "at": [0, 0], "text": "x"})"));
    REQUIRE(s.steps.size() == 3);
    const steps::Step& say = s.steps[0];
    REQUIRE(say.points.size() == 1);
    CHECK(say.points[0].x == 2);
    CHECK(say.points[0].y == 3);
    // not NFC-composed: the Text tool takes the characters as they come
    CHECK(say.text == U"é\nB");
    REQUIRE(say.options.text);
    const steps::Options::Text& t = *say.options.text;
    CHECK(t.face == "vga");
    CHECK(t.textCase == TextCase::Capitals);
    CHECK(t.letterSpacing == 2);
    CHECK(t.lineSpacing == 1);
    CHECK(t.align == TextAlign::Centre);
    CHECK(t.wrapWidth == 12);
    CHECK(t.colour == 12);
    CHECK(t.bigStyle == BigTextStyle::ThreeD);
    CHECK(t.bigColours == std::vector<std::uint8_t>{4, 5});
    // a step with options alone, and one with none
    CHECK(s.steps[1].points.empty());
    REQUIRE(s.steps[1].options.text);
    CHECK(s.steps[1].options.text->colour == 3);
    CHECK_FALSE(s.steps[1].options.text->face);
    CHECK_FALSE(s.steps[2].options.text);

    // applyTextOptions sets the keys given and keeps the rest
    TextSource src;
    src.text = U"keep";
    src.letterSpacing = 5;
    CHECK_FALSE(steps::applyTextOptions(src, *s.steps[1].options.text));
    CHECK(src.colour == 3);
    CHECK(src.letterSpacing == 5);
    CHECK(src.text == U"keep");
    steps::Options::Text plain = t;
    plain.bigStyle = BigTextStyle::None;
    plain.bigColours.reset();
    CHECK_FALSE(steps::applyTextOptions(src, plain));
    CHECK(src.face.kind == FaceRef::Kind::Pixel);
    CHECK(src.face.name == "vga");
    CHECK(src.letterSpacing == 2);
    CHECK(src.colour == 12);
    CHECK(src.text == U"keep");
}

TEST_CASE("a text step's face may be a TheDraw font, its file a path the step names (#422)", "[steps][text]") {
    const steps::Script s = steps::parse(script(R"({"name": "title", "action": "text", "at": [1, 1], "text": "A",
            "textOptions": {"face": {"tdf": "../fonts/outline.tdf", "font": "BW Outline"}, "colour": 3}},
        {"name": "pick", "action": "text", "textOptions": {"face": {"tdf": "fonts/all.tdf", "font": "BW Block", "index": 1}}},
        {"name": "back", "action": "text", "textOptions": {"face": "vga"}})"));
    REQUIRE(s.steps.size() == 3);
    REQUIRE(s.steps[0].options.text);
    const steps::Options::Text& t = *s.steps[0].options.text;
    CHECK_FALSE(t.face);
    CHECK(t.tdf == steps::Options::Text::Tdf{"../fonts/outline.tdf", "BW Outline", std::nullopt});
    CHECK(t.colour == 3);
    CHECK(s.steps[1].options.text->tdf == steps::Options::Text::Tdf{"fonts/all.tdf", "BW Block", 1});
    CHECK_FALSE(s.steps[2].options.text->tdf);
    // the step names its file for the load pass, at most a .tdf's size
    REQUIRE(s.steps[0].files.size() == 1);
    CHECK(s.steps[0].files[0].key == "textOptions face tdf");
    CHECK(s.steps[0].files[0].path == "../fonts/outline.tdf");
    CHECK(s.steps[0].files[0].maxBytes == std::size_t(kMaxTdfBytes));
    CHECK(s.steps[1].files.size() == 1);
    CHECK(s.steps[2].files.empty());

    // applied with the face the replay found in the file
    const FaceRef found{FaceRef::Kind::Tdf, "BW Outline", std::string(64, 'a'), 0};
    TextSource src;
    src.letterSpacing = 2;
    CHECK_FALSE(steps::applyTextOptions(src, t, &found));
    CHECK(src.face == found);
    CHECK(src.colour == 3);
    CHECK(src.letterSpacing == 2);
    // without it, nothing is set and the problem says why
    TextSource none;
    CHECK(steps::applyTextOptions(none, t).value_or("") ==
          "\"textOptions face\" names the TheDraw font \"BW Outline\" in ../fonts/outline.tdf, which hasn't been read");
    const FaceRef pixel{};
    CHECK(steps::applyTextOptions(none, t, &pixel));
    CHECK(none == TextSource{});
    // under a big style it is refused, as the dock's face picker is off
    steps::Options::Text big = t;
    big.colour.reset();
    big.bigStyle = BigTextStyle::Shadow;
    TextSource styled;
    const TextSource before = styled;
    CHECK(steps::applyTextOptions(styled, big, &found).value_or("") ==
          "\"textOptions face\" \"BW Outline\" (../fonts/outline.tdf) can't be picked under a big style, which draws in "
          "the block face (Tool Options' face picker is off)");
    CHECK(styled == before);
}

TEST_CASE("applyTextOptions sets a big style as Tool Options does (#421)", "[steps][text]") {
    const auto big = [](BigTextStyle style) {
        steps::Options::Text t;
        t.bigStyle = style;
        return t;
    };
    // a style picked puts its own colours in and draws in the block face
    TextSource src;
    src.face = FaceRef{FaceRef::Kind::Pixel, "vga", {}, 0};
    CHECK_FALSE(steps::applyTextOptions(src, big(BigTextStyle::Shadow)));
    CHECK(src.bigStyle == BigTextStyle::Shadow);
    CHECK(src.bigColours == bigTextDefaultColours(BigTextStyle::Shadow));
    CHECK(src.face == FaceRef{});
    // another style picked resets them; colours given win
    src.bigColours = {9, 1};
    CHECK_FALSE(steps::applyTextOptions(src, big(BigTextStyle::ThreeD)));
    CHECK(src.bigColours == bigTextDefaultColours(BigTextStyle::ThreeD));
    steps::Options::Text both = big(BigTextStyle::Shadow);
    both.bigColours = std::vector<std::uint8_t>{11, 4};
    CHECK_FALSE(steps::applyTextOptions(src, both));
    CHECK(src.bigColours == std::vector<std::uint8_t>{11, 4});
    // the same style again, or colours alone, keep the rest
    CHECK_FALSE(steps::applyTextOptions(src, big(BigTextStyle::Shadow)));
    CHECK(src.bigColours == std::vector<std::uint8_t>{11, 4});
    // none clears them
    CHECK_FALSE(steps::applyTextOptions(src, big(BigTextStyle::None)));
    CHECK(src.bigStyle == BigTextStyle::None);
    CHECK(src.bigColours.empty());
    CHECK(src.face == FaceRef{});

    // what the dock turns off is refused, and nothing is set
    const TextSource before = src;
    steps::Options::Text vga = big(BigTextStyle::Shadow);
    vga.face = "vga";
    CHECK_THAT(steps::applyTextOptions(src, vga).value_or(""),
               ContainsSubstring("\"textOptions face\" \"vga\" can't be picked under a big style"));
    steps::Options::Text block = big(BigTextStyle::Shadow);
    block.face = "block";
    CHECK_FALSE(steps::applyTextOptions(src, block));
    src = before;
    steps::Options::Text colour = big(BigTextStyle::Shadow);
    colour.colour = 4;
    CHECK_THAT(steps::applyTextOptions(src, colour).value_or(""),
               ContainsSubstring("\"textOptions colour\" can't be set under a big style"));
    steps::Options::Text colours;
    colours.bigColours = std::vector<std::uint8_t>{4};
    CHECK_THAT(steps::applyTextOptions(src, colours).value_or(""),
               ContainsSubstring("\"textOptions bigColours\" needs a big style"));
    CHECK(src == before);
}

TEST_CASE("every bundled face, case, alignment and big style is a text option (#421)", "[steps][text]") {
    for (const Face* f : bundledFaces()) {
        INFO(f->name());
        const steps::Script s = steps::parse(script(std::string(R"({"name": "f", "action": "text", "textOptions": {"face": ")") +
                                                   std::string(f->name()) + R"("}})"));
        CHECK(s.steps[0].options.text->face == std::string(f->name()));
    }
    for (int i = 0; !bigTextStyleName(BigTextStyle(i)).empty(); ++i) {
        const std::string name(bigTextStyleName(BigTextStyle(i)));
        INFO(name);
        const steps::Script s =
            steps::parse(script(R"({"name": "b", "action": "text", "textOptions": {"bigStyle": ")" + name + R"("}})"));
        CHECK(s.steps[0].options.text->bigStyle == BigTextStyle(i));
    }
    for (const char* a : {"left", "centre", "right"})
        CHECK(refusal(script(std::string(R"({"name": "a", "action": "text", "textOptions": {"align": ")") + a + R"("}})")) ==
              "(accepted)");
    CHECK(refusal(script(R"({"name": "c", "action": "text", "textOptions": {"case": "as-typed"}})")) == "(accepted)");
    // the limits themselves
    CHECK(refusal(script(R"({"name": "l", "action": "text", "textOptions": {"letterSpacing": 64, "lineSpacing": 0,
        "wrap": 65535, "colour": 15, "bigColours": [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]}})")) == "(accepted)");
    CHECK(refusal(script(R"({"name": "e", "action": "text", "textOptions": {}})")) == "(accepted)");
    CHECK(refusal(script(std::string(R"({"name": "long", "action": "text", "at": [0, 0], "text": ")") +
                         std::string(TextSource::kMaxLength, 'a') + "\"}")) == "(accepted)");
}

TEST_CASE("a text step refuses a C1 control as it refuses C0 and DEL, and types from U+00A0 (#428)", "[steps][text]") {
    for (const char* code : {"0080", "0085", "009F"}) {
        INFO(code);
        CHECK(refusal(script(std::string(R"({"name": "tx", "action": "text", "at": [1, 1], "text": "a\u)") + code +
                             R"(b"})")) ==
              std::string("step 1 (tx): \"text\" holds a control character the Text tool doesn't type (U+") + code +
                  "); only \"\\n\", Enter, starts a new line");
    }
    // the same words as for C0 and DEL
    CHECK(refusal(script(R"({"name": "tx", "action": "text", "at": [1, 1], "text": "a\u007fb"})")) ==
          "step 1 (tx): \"text\" holds a control character the Text tool doesn't type (U+007F); only \"\\n\", "
          "Enter, starts a new line");
    // the first character past C1, no-break space, and the one after it
    const steps::Script s = steps::parse(script(R"({"name": "nb", "action": "text", "at": [0, 0], "text": "a\u00a0b"},
        {"name": "ex", "action": "text", "at": [0, 4], "text": "\u00a1"})"));
    REQUIRE(s.steps.size() == 2);
    CHECK(s.steps[0].text == U"a\u00a0b");
    CHECK(s.steps[1].text == U"\u00a1");
}

TEST_CASE("the glyph option refuses a C1 control as it refuses C0 and DEL, and takes U+00A0 on (#449)", "[steps]") {
    for (const char* code : {"0085", "009B", "009F", "0000", "0007", "001B", "007F"}) {
        INFO(code);
        CHECK(refusal(script(std::string(R"({"name": "g", "action": "glyph", "glyph": "\u)") + code +
                             R"(", "points": [[1, 1]]})")) == "step 1 (g): \"glyph\" must be one printable character");
    }
    // the first character past C1, no-break space, an ordinary block and a space
    const steps::Script s = steps::parse(script(R"({"name": "nb", "action": "glyph", "glyph": "\u00a0"},
        {"name": "fb", "action": "glyph", "glyph": "\u2588"}, {"name": "sp", "action": "glyph", "glyph": " "})"));
    REQUIRE(s.steps.size() == 3);
    CHECK(s.steps[0].options.glyph == U'\u00a0');
    CHECK(s.steps[1].options.glyph == U'\u2588');
    CHECK(s.steps[2].options.glyph == U' ');
}

TEST_CASE("a text step's bad value is refused, naming the step (#421)", "[steps][text]") {
    const std::pair<std::string, std::string> cases[] = {
        {R"("at": [1, 1])", "text needs \"text\" to type at its \"at\" cell"},
        {R"("text": "hi")", "\"text\" needs an \"at\" cell to start from"},
        {R"("at": [1, 1], "text": "")", "\"text\" must be 1 to 4096 characters"},
        {R"("at": [1, 1], "text": ")" + std::string(TextSource::kMaxLength + 1, 'a') + "\"",
         "\"text\" must be 1 to 4096 characters"},
        {R"("at": [1, 1], "text": "a\tb")", "control character the Text tool doesn't type (U+0009)"},
        {R"("at": [1, 1], "text": "a\rb")", "(U+000D)"},
        {R"("at": [1, 1], "text": "a\u007fb")", "(U+007F)"},
        {R"("at": [1, 1], "text": "a\u001bb")", "(U+001B)"},
        {R"("at": [1, 1], "text": 3)", "\"text\""},
        {R"("at": [1.5, 1], "text": "a")", "\"at x\""},
        {R"("textOptions": 3)", "\"textOptions\""},
        {R"("textOptions": {"size": 2})", "\"size\""},
        {R"("textOptions": {"Face": "vga"})", "\"Face\""},
        {R"("textOptions": {"face": "comic"})",
         "\"textOptions face\" takes a bundled face (block, vga, mini) or a TheDraw font ({\"tdf\": ..., \"font\": ...}), "
         "not \"comic\""},
        {R"("textOptions": {"face": {"font": "BW Block"}})", "\"textOptions face\" needs \"tdf\", the .tdf file's path"},
        {R"("textOptions": {"face": {"tdf": "a.tdf"}})", "\"textOptions face\" needs \"font\", the font's name in the file"},
        {R"("textOptions": {"face": {"tdf": "a.tdf", "font": "X", "size": 2}})", "\"size\""},
        {R"("textOptions": {"face": {"tdf": "a.tdf", "font": ""}})", "\"textOptions face font\" is empty"},
        {R"("textOptions": {"face": {"tdf": "a.tdf", "font": 3}})", "\"textOptions face font\""},
        {R"("textOptions": {"face": {"tdf": 3, "font": "X"}})", "\"textOptions face tdf\""},
        {R"("textOptions": {"face": {"tdf": "a.tdf", "font": "X", "index": -1}})", "\"textOptions face index\""},
        {R"("textOptions": {"face": {"tdf": "a.tdf", "font": "X", "index": 256}})", "\"textOptions face index\""},
        {R"("textOptions": {"face": {"tdf": "a.tdf", "font": "X", "index": 1.5}})", "\"textOptions face index\""},
        {R"("textOptions": {"face": {"tdf": "", "font": "X"}})", "\"textOptions face tdf\" is empty"},
        {R"("textOptions": {"face": {"tdf": "/fonts/a.tdf", "font": "X"}})",
         "\"textOptions face tdf\" \"/fonts/a.tdf\" is absolute; a steps file names its files relative to its own directory"},
        {R"("textOptions": {"face": {"tdf": "C:/fonts/a.tdf", "font": "X"}})", "\"C:/fonts/a.tdf\" is absolute"},
        {R"("textOptions": {"face": {"tdf": "fonts\\a.tdf", "font": "X"}})", "has a \\ in it"},
        {R"("textOptions": {"face": []})", "\"textOptions face\""},
        {R"("textOptions": {"face": 1})", "\"textOptions face\""},
        {R"("textOptions": {"case": "lower"})", "\"textOptions case\""},
        {R"("textOptions": {"align": "center"})", "\"textOptions align\""},
        {R"("textOptions": {"letterSpacing": -1})", "\"textOptions letterSpacing\""},
        {R"("textOptions": {"letterSpacing": 65})", "\"textOptions letterSpacing\""},
        {R"("textOptions": {"lineSpacing": 1.5})", "\"textOptions lineSpacing\""},
        {R"("textOptions": {"wrap": 65536})", "\"textOptions wrap\""},
        {R"("textOptions": {"wrap": -1})", "\"textOptions wrap\""},
        {R"("textOptions": {"colour": 16})", "\"textOptions colour\""},
        {R"("textOptions": {"bigStyle": "glow"})", "\"textOptions bigStyle\""},
        {R"("textOptions": {"bigColours": 4})", "\"textOptions bigColours\" must be an array of 1 to 16"},
        {R"("textOptions": {"bigColours": []})", "\"textOptions bigColours\" must be an array of 1 to 16"},
        {R"("textOptions": {"bigColours": [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0]})",
         "\"textOptions bigColours\" must be an array of 1 to 16"},
        {R"("textOptions": {"bigColours": [16]})", "\"textOptions bigColours\""},
        {R"("at": [1, 1], "text": "a", "from": [1, 1])", "text takes no \"from\""},
        {R"("at": [1, 1], "text": "a", "sampleMerged": true)", "text takes no \"sampleMerged\""},
    };
    for (const auto& [fields, says] : cases) {
        INFO(fields);
        const std::string why = refusal(
            script(R"({"name": "first", "action": "selectAll"}, {"name": "tx", "action": "text", )" + fields + "}"));
        CHECK_THAT(why, ContainsSubstring("step 2 (tx): "));
        CHECK_THAT(why, ContainsSubstring(says));
    }
    // textOptions on another action is refused
    for (const char* action : {"keyboard", "glyph", "fill", "clone", "selectAll", "view", "canvasSize"}) {
        INFO(action);
        CHECK_THAT(refusal(script(std::string(R"({"name": "z", "action": ")") + action +
                                  R"(", "textOptions": {"colour": 1}})")),
                   ContainsSubstring(std::string("step 1 (z): ") + action + " takes no \"textOptions\""));
    }
    // keyboard keeps its own reading: NFC, with any control character in it
    const steps::Script k = steps::parse(script(R"({"name": "k", "action": "keyboard", "at": [0, 0], "text": "é"})"));
    CHECK(k.steps[0].text == U"é");
}

TEST_CASE("editText, applyTextStyle and openReferenceImage read their keys and name their files (#422)",
          "[steps][text]") {
    const steps::Script s = steps::parse(script(R"(
        {"name": "retitle", "action": "editText", "text": "Two\nlines"},
        {"name": "chrome", "action": "applyTextStyle", "style": "gold-3d"},
        {"name": "own", "action": "applyTextStyle", "style": {"file": "../styles/mine.style.json"}},
        {"name": "trace", "action": "openReferenceImage", "file": "refs/sketch.png"})"));
    REQUIRE(s.steps.size() == 4);
    CHECK(s.steps[0].text == U"Two\nlines");
    CHECK(s.steps[0].files.empty());
    CHECK(s.steps[1].stylePreset == "gold-3d");
    CHECK(s.steps[1].files.empty());
    CHECK_FALSE(s.steps[2].stylePreset);
    REQUIRE(s.steps[2].files.size() == 1);
    CHECK(s.steps[2].files[0].key == "style file");
    CHECK(s.steps[2].files[0].path == "../styles/mine.style.json");
    CHECK(s.steps[2].files[0].maxBytes == library::kMaxSidecarBytes);
    REQUIRE(s.steps[3].files.size() == 1);
    CHECK(s.steps[3].files[0].key == "file");
    CHECK(s.steps[3].files[0].path == "refs/sketch.png");
    CHECK(s.steps[3].files[0].maxBytes == project::kMaxEntryBytes);
    // editText's text is as typed, not NFC-composed, as a text step's
    const steps::Script typed = steps::parse(script(R"({"name": "e", "action": "editText", "text": "e\u0301"})"));
    CHECK(typed.steps[0].text == U"e\u0301");
}

TEST_CASE("editText, applyTextStyle and openReferenceImage refuse a bad value, naming the step (#422)",
          "[steps][text]") {
    const std::pair<std::string, std::string> cases[] = {
        {R"("action": "editText")", "editText needs \"text\", the active text layer's whole new text"},
        {R"("action": "editText", "text": "")", "\"text\" must be 1 to 4096 characters"},
        {R"("action": "editText", "text": "a\tb")", "control character the Text tool doesn't type (U+0009)"},
        {R"("action": "editText", "text": 3)", "\"text\""},
        {R"("action": "editText", "text": "a", "at": [1, 1])", "editText takes no \"at\""},
        {R"("action": "editText", "text": "a", "textOptions": {"colour": 3})", "editText takes no \"textOptions\""},
        {R"("action": "applyTextStyle")", "applyTextStyle needs \"style\""},
        {R"("action": "applyTextStyle", "style": "Chrome")", "\"style\" must be a bundled Library preset's name"},
        {R"("action": "applyTextStyle", "style": "../chrome")", "not \"../chrome\""},
        {R"("action": "applyTextStyle", "style": "")", "\"style\" must be a bundled Library preset's name"},
        {R"("action": "applyTextStyle", "style": 3)", "\"style\" must be a bundled Library preset's name or {\"file\": ...}, not a number"},
        {R"("action": "applyTextStyle", "style": {})", "\"style\" needs \"file\""},
        {R"("action": "applyTextStyle", "style": {"file": "a.style.json", "name": "x"})", "\"name\""},
        {R"("action": "applyTextStyle", "style": {"file": 3})", "\"style file\""},
        {R"("action": "applyTextStyle", "style": {"file": ""})", "\"style file\" is empty"},
        {R"("action": "applyTextStyle", "style": {"file": "/styles/a.style.json"})",
         "\"style file\" \"/styles/a.style.json\" is absolute; a steps file names its files relative to its own directory"},
        {R"("action": "applyTextStyle", "style": {"file": "C:/a.style.json"})", "is absolute"},
        {R"("action": "applyTextStyle", "style": {"file": "styles\\a.style.json"})", "has a \\ in it"},
        {R"("action": "applyTextStyle", "style": "chrome", "file": "a.png")", "applyTextStyle takes no \"file\""},
        {R"("action": "openReferenceImage")", "openReferenceImage needs \"file\", a PNG or JPEG picture's path"},
        {R"("action": "openReferenceImage", "file": 3)", "\"file\""},
        {R"("action": "openReferenceImage", "file": "")", "\"file\" is empty"},
        {R"("action": "openReferenceImage", "file": "/home/a.png")", "\"file\" \"/home/a.png\" is absolute"},
        {R"("action": "openReferenceImage", "file": "D:/a.png")", "is absolute"},
        {R"("action": "openReferenceImage", "file": "refs\\a.png")", "has a \\ in it"},
        {R"("action": "openReferenceImage", "file": "a.png", "opacity": 50)", "\"opacity\""},
        {R"("action": "openReferenceImage", "file": "a.png", "style": "chrome")", "openReferenceImage takes no \"style\""},
        {R"("action": "text", "at": [1, 1], "text": "a", "style": "chrome")", "text takes no \"style\""},
        {R"("action": "selectAll", "file": "a.png")", "selectAll takes no \"file\""},
    };
    for (const auto& [fields, says] : cases) {
        INFO(fields);
        const std::string why =
            refusal(script(R"({"name": "first", "action": "selectAll"}, {"name": "st", )" + fields + "}"));
        CHECK_THAT(why, ContainsSubstring("step 2 (st): "));
        CHECK_THAT(why, ContainsSubstring(says));
    }
}
