// The steps file parser (blockwright/steps.h, issue #208): what it reads, and
// that what it refuses names the step, or the start, it is in.

#include "blockwright/steps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

using namespace bw;
using Catch::Matchers::ContainsSubstring;

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
