// A steps file's clone step (#418) against the editor's Clone Stamp: the
// same source, Tool Options and strokes on the same layered start give the
// same layers, the same source and the same distance. main() is in
// test_app.cpp.

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "replay.h"
#include "scratch_settings.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/brushes/clone.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/project.h"
#include "blockwright/steps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QMouseEvent>

#include <string>
#include <vector>

using namespace bw;

namespace {

constexpr int kW = 20, kH = 10;

// Every cell differs from its neighbours: a letter for the column, colours
// for the row.
Layer pattern(const std::string& name, char32_t first) {
    Layer l = makeLayer(LayerKind::Cells, kW, kH, name);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            Cell& c = l.cells.at(x, y);
            c.ch = char32_t(first + x);
            c.transparent = false;
            c.fg = Color::indexed(std::uint8_t(1 + (x + y) % 15));
            c.bg = Color::indexed(std::uint8_t(y % 8));
        }
    return l;
}

// From the bottom: Background (the pattern), "ghost" (hidden, other
// letters), "dots" (a pixels layer with half blocks in its left half),
// "ink" (letters in a band, transparent elsewhere).
project::Project layered() {
    project::Project p;
    p.layers = LayerStack(kW, kH);
    p.layers.add(pattern("Background", U'a'));
    Layer ghost = pattern("ghost", U'A');
    ghost.props.visible = false;
    p.layers.add(std::move(ghost));
    Layer dots = makeLayer(LayerKind::Pixels, kW, kH, "dots");
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW / 2; ++x)
            if ((x + y) % 3 != 0) {
                Cell& c = dots.cells.at(x, y);
                c.ch = (x + y) % 3 == 1 ? kUpperHalf : kFullBlock;
                c.transparent = false;
                c.fg = Color::indexed(std::uint8_t(9 + x % 6));
                c.bg = Color::indexed(std::uint8_t(y % 8));
            }
    p.layers.add(std::move(dots));
    Layer ink = makeLayer(LayerKind::Cells, kW, kH, "ink");
    for (int y = 1; y < 5; ++y)
        for (int x = 1; x < 10; ++x) {
            Cell& c = ink.cells.at(x, y);
            c.ch = char32_t(U'0' + (x + y) % 10);
            c.transparent = false;
            c.fg = Color::indexed(std::uint8_t(10 + y % 5));
        }
    p.layers.add(std::move(ink));
    return p;
}

LayerId idOf(const LayerStack& s, const std::string& name) {
    for (const Layer& l : s.layers())
        if (l.props.name == name)
            return l.props.id;
    FAIL("no layer " << name);
    return 0;
}

QPointF centreOf(CanvasView* v, int x, int y) { return QPointF(v->cellRect(QPoint(x, y)).center()); }

void mouse(CanvasView* v, QEvent::Type type, QPoint cell, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
    const QPointF at = centreOf(v, cell.x(), cell.y());
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held, mods);
    QApplication::sendEvent(v, &e);
}

// Ctrl+click on a cell: the clone's source.
void ctrlClick(CanvasView* v, QPoint cell) {
    mouse(v, QEvent::MouseButtonPress, cell, Qt::LeftButton, Qt::ControlModifier);
    mouse(v, QEvent::MouseButtonRelease, cell, Qt::LeftButton, Qt::ControlModifier);
}

void stroke(CanvasView* v, const std::vector<QPoint>& cells, Qt::MouseButton button) {
    mouse(v, QEvent::MouseButtonPress, cells.front(), button, Qt::NoModifier);
    for (std::size_t i = 1; i < cells.size(); ++i)
        mouse(v, QEvent::MouseMove, cells[i], button, Qt::NoModifier);
    mouse(v, QEvent::MouseButtonRelease, cells.back(), button, Qt::NoModifier);
}

std::string points(const std::vector<QPoint>& cells) {
    std::string s = "[";
    for (const QPoint& p : cells)
        s += (s.size() > 1 ? ", [" : "[") + std::to_string(p.x()) + ", " + std::to_string(p.y()) + "]";
    return s + "]";
}

// The clone's Aligned, forgotten before and after, from the scratch
// settings only.
struct FreshCloneSettings {
    static QString dir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }
    FreshCloneSettings() {
        REQUIRE(bwtest::settingsInScratch(dir()));
        appSettings()->remove(QStringLiteral("clone"));
    }
    ~FreshCloneSettings() {
        if (bwtest::settingsInScratch(dir()))
            appSettings()->remove(QStringLiteral("clone"));
    }
};

struct LayerCase {
    const char* name;
    const char* active;
    bool alphaLocked;
    bool selection;
};

} // namespace

TEST_CASE("a clone step gives what the Clone Stamp gives: source, Aligned, size, tip, symmetry and layers (#418)",
          "[replay][clone][layers]") {
    FreshCloneSettings fresh;
    const bool aligned = GENERATE(true, false);
    const auto [symName, symMode] = GENERATE(std::pair{"none", SymmetryMode::None},
                                             std::pair{"mirrorX", SymmetryMode::MirrorX},
                                             std::pair{"mirrorY", SymmetryMode::MirrorY},
                                             std::pair{"mirrorXY", SymmetryMode::MirrorXY},
                                             std::pair{"radial", SymmetryMode::Radial});
    const auto [size, tip] = GENERATE(std::pair{1, Tip::Square}, std::pair{3, Tip::Square}, std::pair{3, Tip::Round});
    const LayerCase layer = GENERATE(LayerCase{"bottom", "Background", false, false},
                                     LayerCase{"ink over a hidden layer", "ink", false, false},
                                     LayerCase{"pixels", "dots", false, false},
                                     LayerCase{"ink, alpha locked, in a selection", "ink", true, true});
    // the right button copies as the left does: the round tip's cases take it
    const bool right = tip == Tip::Round;
    INFO("aligned " << aligned << ", " << symName << ", size " << size << (tip == Tip::Round ? " round" : " square")
                    << ", " << layer.name << (right ? ", right button" : ""));

    project::Project start = layered();
    if (layer.alphaLocked)
        start.layers.find(idOf(start.layers, layer.active))->props.alphaLocked = true;
    const QPoint source(3, 2);
    const std::vector<QPoint> one = {{8, 4}, {10, 5}, {11, 5}};
    const std::vector<QPoint> two = {{13, 7}, {14, 8}};
    const Qt::MouseButton button = right ? Qt::RightButton : Qt::LeftButton;

    // the editor
    MainWindow w;
    Document* doc = w.document();
    doc->replace(start, QString(), FileForm::ProjectZip);
    REQUIRE(doc->setActiveLayer(idOf(doc->layers(), layer.active)));
    CanvasView* v = w.canvasView();
    QAction* tool = w.findChild<QAction*>(QStringLiteral("cloneTool"));
    REQUIRE(tool);
    tool->trigger();
    REQUIRE(w.tools()->active()->id() == QStringLiteral("clone"));
    auto* clone = qobject_cast<CloneTool*>(w.tools()->active());
    REQUIRE(clone);
    v->setBrushSettings(BrushSettings{size, tip, 1.0, PressureTo::Nothing});
    v->setSymmetry(Symmetry{symMode, 4, std::nullopt});
    w.toolOptions()->cloneAlignedBox()->setChecked(aligned);
    REQUIRE(clone->aligned() == aligned);
    if (layer.selection)
        v->setSelection(Selection(CellRect{6, 3, 9, 5}));
    ctrlClick(v, source);
    REQUIRE(clone->source().source == std::pair{source.x(), source.y()});
    stroke(v, one, button);
    stroke(v, two, button);

    // the replay
    const std::string tipName = tip == Tip::Round ? "round" : "square";
    const steps::Script s = steps::parse(
        std::string(R"({"version": 1, "start": {"blank": [1, 1]}, "steps": [
            {"name": "use", "action": "setActiveLayer", "layer": ")") +
        layer.active + R"("},)" + (layer.selection ? R"({"name": "sel", "action": "select", "rect": [6, 3, 14, 7]},)" : "") +
        R"({"name": "pick", "action": "clone", "source": [3, 2], "aligned": )" + (aligned ? "true" : "false") +
        R"(, "size": )" + std::to_string(size) + R"(, "tip": ")" + tipName + R"(", "symmetry": ")" + symName + R"("},
            {"name": "one", "action": "clone", "button": ")" + (right ? "right" : "left") + R"(", "points": )" + points(one) + R"(},
            {"name": "two", "action": "clone", "button": ")" + (right ? "right" : "left") + R"(", "points": )" + points(two) + R"(}]})");
    CloneSource replayed;
    const auto out = bwcli::replay::run(s, start, [&](const bwcli::replay::Moment& m) {
        if (!m.before)
            replayed = m.settings.clone;
    });
    const project::Project& end = out.captures.back().project;
    for (const Layer& l : end.layers.layers()) {
        INFO("layer " << l.props.name);
        const Layer* e = doc->layers().find(l.props.id);
        REQUIRE(e);
        CHECK(e->cells == l.cells);
    }
    CHECK(end.layers == doc->layers());
    // the strokes changed something on the active layer, but where the
    // alpha lock and the selection keep every copy out
    CHECK_FALSE(end.layers.find(idOf(end.layers, layer.active))->cells ==
                start.layers.find(idOf(start.layers, layer.active))->cells);
    // and the same source and distance are kept for the next stroke
    CHECK(replayed.source == clone->source().source);
    CHECK(replayed.anchor == clone->source().anchor);
    CHECK(replayed.aligned == clone->source().aligned);
}
