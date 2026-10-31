// Canvas-wide Alt+drag layer moves, ahead of every tool's mouse handling.
#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "select_tool.h"
#include "system_input.h"
#include "tool.h"

#include "blockwright/composite.h"
#include "blockwright/project.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStatusBar>
#include <QTabletEvent>
#include <QUndoStack>

using namespace bw;

namespace {
void setup(MainWindow& w, QPoint offset = {2, 2}) {
    project::Project p;
    p.layers = LayerStack(12, 8);
    Layer bg = makeLayer(LayerKind::Cells, 12, 8, "Background");
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 12; ++x)
            bg.cells.at(x, y).ch = U'.';
    p.layers.add(std::move(bg));
    Layer top = makeLayer(LayerKind::Cells, 3, 2, "Moving", offset.x(), offset.y());
    top.cells.at(0, 0) = Cell{U'A', Color::indexed(White), Color::indexed(Blue)};
    top.cells.at(2, 1) = Cell{U'B', Color::indexed(Yellow), Color::indexed(Black)};
    const LayerId id = p.layers.add(std::move(top));
    w.document()->replace(std::move(p), {}, FileForm::ProjectZip);
    REQUIRE(w.document()->setActiveLayer(id));
}

void mouse(CanvasView* v, QEvent::Type type, QPoint cell,
           Qt::KeyboardModifiers mods = Qt::AltModifier) {
    const QPointF at = v->toWidget(v->cellRect(cell)).center();
    QMouseEvent e(type, at, v->mapToGlobal(at),
                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(v, &e);
}

bool tablet(CanvasView* v, QEvent::Type type, QPoint cell,
            Qt::KeyboardModifiers mods = Qt::AltModifier) {
    const QPointF at = v->toWidget(v->cellRect(cell)).center();
    QTabletEvent e(type, bwtest::systemStylus(), at, v->mapToGlobal(at),
                   type == QEvent::TabletRelease ? 0.0 : 1.0, 0, 0, 0, 0, 0, mods,
                   type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                   type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
    e.setAccepted(false);
    QApplication::sendEvent(v, &e);
    return e.isAccepted();
}

QPoint offset(const Document* d) { return {d->active()->props.x, d->active()->props.y}; }

Canvas shifted(const Document* d, QPoint to) {
    auto p = d->project();
    auto* layer = p.layers.find(d->activeLayer());
    layer->props.x = to.x();
    layer->props.y = to.y();
    return composite(p.layers, CompositeOptions{.ice = p.ice});
}

void start(MainWindow& w) {
    mouse(w.canvasView(), QEvent::MouseButtonPress, {3, 3});
    mouse(w.canvasView(), QEvent::MouseMove, {6, 4});
    REQUIRE(w.statusBar()->currentMessage() == QStringLiteral("Move Layer: dx 3, dy 1 | x 5, y 3"));
    REQUIRE(w.document()->canvas() == shifted(w.document(), {5, 3}));
}
}

TEST_CASE("Alt drag previews whole-cell offsets and commits one undo step", "[layers][layer-move]") {
    MainWindow w;
    setup(w, {-1, 2});
    auto* d = w.document();
    auto* v = w.canvasView();
    v->setZoom(3);
    const Canvas cells = d->active()->cells, before = d->canvas();
    QRegion dirty;
    QObject::connect(d, &Document::cellsChanged, &w, [&](const QRegion& r) { dirty |= r; });
    mouse(v, QEvent::MouseButtonPress, {3, 3});
    mouse(v, QEvent::MouseMove, {5, 1});
    REQUIRE(w.statusBar()->currentMessage() == QStringLiteral("Move Layer: dx 2, dy -2 | x 1, y 0"));
    CHECK(d->canvas() == shifted(d, {1, 0}));
    CHECK(dirty.contains(QPoint(1, 3))); // a visible cell left behind
    CHECK(dirty.contains(QPoint(1, 0))); // its new position
    CHECK(d->active()->cells == cells);
    CHECK(d->undoStack()->count() == 0);
    CHECK_FALSE(d->isModified());
    // Releasing Alt before the button still completes the move. The release
    // itself supplies the last cell, even without a preceding mouse move.
    mouse(v, QEvent::MouseButtonRelease, {6, 1}, Qt::NoModifier);
    CHECK(offset(d) == QPoint(2, 0));
    CHECK(d->active()->cells == cells);
    CHECK(d->canvas() == shifted(d, {2, 0}));
    REQUIRE(d->undoStack()->count() == 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Move Layer"));
    CHECK(d->isModified());
    d->undoStack()->undo();
    CHECK(offset(d) == QPoint(-1, 2));
    CHECK(d->canvas() == before);
    CHECK_FALSE(d->isModified());
    d->undoStack()->redo();
    CHECK(offset(d) == QPoint(2, 0));
    CHECK(d->canvas() == shifted(d, {2, 0}));
    CHECK(d->active()->cells == cells);
}

TEST_CASE("Escape during Alt drag drops the preview without an edit", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    const Canvas before = w.document()->canvas();
    start(w);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(w.canvasView(), &esc);
    mouse(w.canvasView(), QEvent::MouseButtonRelease, {6, 4});
    CHECK(offset(w.document()) == QPoint(2, 2));
    CHECK(w.document()->canvas() == before);
    CHECK(w.document()->undoStack()->count() == 0);
    CHECK_FALSE(w.document()->isModified());
    CHECK(w.statusBar()->currentMessage().isEmpty());
}

TEST_CASE("the undo shortcut during Alt drag cancels without undoing history", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    bool history = false;
    SECTION("an empty undo stack") {}
    SECTION("an earlier command") {
        REQUIRE(d->renameLayer(d->activeLayer(), "Keep this"));
        history = true;
    }
    const Canvas before = d->canvas();
    REQUIRE(bwtest::showExposed(&w));
    w.canvasView()->setFocus();
    start(w);
    bwtest::systemKey(w.canvasView(), Qt::Key_Z, Qt::ControlModifier);
    mouse(w.canvasView(), QEvent::MouseButtonRelease, {6, 4});
    CHECK(offset(d) == QPoint(2, 2));
    CHECK(d->canvas() == before);
    CHECK(d->active()->props.name == (history ? "Keep this" : "Moving"));
    CHECK(d->undoStack()->index() == (history ? 1 : 0));
    CHECK(d->undoStack()->count() == (history ? 1 : 0));
    CHECK_FALSE(d->undoStack()->canRedo());
    CHECK(w.statusBar()->currentMessage().isEmpty());
}

TEST_CASE("Alt drag refuses locked and hidden layers with a reason", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    auto props = d->active()->props;
    QString reason;
    SECTION("locked") { props.locked = true; reason = QStringLiteral("locked"); }
    SECTION("hidden") { props.visible = false; reason = QStringLiteral("hidden"); }
    REQUIRE(d->setLayerProperties(d->activeLayer(), props, QStringLiteral("Set test state")));
    const Canvas before = d->canvas(), cells = d->active()->cells;
    mouse(w.canvasView(), QEvent::MouseButtonPress, {3, 3});
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("Cannot move layer: ") + reason);
    mouse(w.canvasView(), QEvent::MouseMove, {6, 4});
    mouse(w.canvasView(), QEvent::MouseButtonRelease, {6, 4});
    CHECK(offset(d) == QPoint(2, 2));
    CHECK(d->canvas() == before);
    CHECK(d->active()->cells == cells);
    CHECK(d->undoStack()->count() == 1);
}

TEST_CASE("Alt drag precedes every tool without painting or changing the clone source", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    auto* v = w.canvasView();
    auto* clone = w.findChild<CloneTool*>();
    REQUIRE(clone);
    v->setTool(clone);
    mouse(v, QEvent::MouseButtonPress, {2, 2}, Qt::ControlModifier);
    mouse(v, QEvent::MouseButtonRelease, {2, 2}, Qt::ControlModifier);
    REQUIRE(clone->source().source == std::make_pair(2, 2));
    const auto source = clone->source();
    const Canvas cells = d->active()->cells;
    for (Tool* tool : w.tools()->tools()) {
        DYNAMIC_SECTION(tool->id().toStdString()) {
            v->setTool(tool);
            start(w);
            CHECK_FALSE(tool->dragging());
            mouse(v, QEvent::MouseButtonRelease, {6, 4});
            CHECK(offset(d) == QPoint(5, 3));
            CHECK(d->active()->cells == cells);
            CHECK(clone->source().source == source.source);
            CHECK(clone->source().anchor == source.anchor);
            CHECK(d->undoStack()->count() == 1);
        }
    }
}

TEST_CASE("Alt layer moves cancel on focus loss and external changes", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    const Canvas before = d->canvas();
    start(w);
    SECTION("popup focus") {
        QFocusEvent lost(QEvent::FocusOut, Qt::PopupFocusReason);
        QApplication::sendEvent(w.canvasView(), &lost);
    }
    SECTION("window focus") {
        QFocusEvent lost(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(w.canvasView(), &lost);
    }
    SECTION("property command") {
        REQUIRE(d->renameLayer(d->activeLayer(), "Renamed"));
        d->undoStack()->undo();
    }
    SECTION("save preparation") { w.canvasView()->finishTool(); }
    SECTION("switch tool") { w.canvasView()->setTool(nullptr); }
    SECTION("new document with the same ids") { setup(w); }
    mouse(w.canvasView(), QEvent::MouseButtonRelease, {6, 4});
    CHECK(offset(d) == QPoint(2, 2));
    CHECK(d->canvas() == before);
    CHECK_FALSE(d->isModified());
    CHECK(w.statusBar()->currentMessage().isEmpty());
}

TEST_CASE("Alt drag back to its start adds no undo step", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    const Canvas before = w.document()->canvas();
    start(w);
    mouse(w.canvasView(), QEvent::MouseButtonRelease, {3, 3});
    CHECK(offset(w.document()) == QPoint(2, 2));
    CHECK(w.document()->canvas() == before);
    CHECK(w.document()->undoStack()->count() == 0);
    CHECK_FALSE(w.document()->isModified());
}

TEST_CASE("external undo removes an Alt preview from the current layer bounds", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    auto props = d->active()->props;
    props.x = 4;
    REQUIRE(d->setLayerProperties(d->activeLayer(), props, QStringLiteral("Earlier move")));
    const Canvas cells = d->active()->cells;
    mouse(w.canvasView(), QEvent::MouseButtonPress, {3, 3});
    mouse(w.canvasView(), QEvent::MouseMove, {6, 4});
    REQUIRE(w.statusBar()->currentMessage() == QStringLiteral("Move Layer: dx 3, dy 1 | x 7, y 3"));
    REQUIRE(d->canvas() == shifted(d, {7, 3}));
    d->undoStack()->undo();
    mouse(w.canvasView(), QEvent::MouseButtonRelease, {6, 4});
    CHECK(offset(d) == QPoint(2, 2));
    CHECK(d->canvas() == shifted(d, {2, 2}));
    CHECK(d->active()->cells == cells);
    CHECK(d->undoStack()->count() == 1);
    CHECK(d->undoStack()->index() == 0);
    CHECK(w.statusBar()->currentMessage().isEmpty());
}

TEST_CASE("external undo removing the layer also removes its Alt preview", "[layers][layer-move]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    TextSource text;
    text.text = U"C";
    text.face.name = "mini";
    const LayerId moving = d->addTextLayer(text, 0, 0);
    REQUIRE(moving);
    mouse(w.canvasView(), QEvent::MouseButtonPress, {3, 3});
    mouse(w.canvasView(), QEvent::MouseMove, {6, 4});
    REQUIRE(w.statusBar()->currentMessage() == QStringLiteral("Move Layer: dx 3, dy 1 | x 3, y 1"));
    REQUIRE(d->canvas() == shifted(d, {3, 1}));
    d->undoStack()->undo();
    mouse(w.canvasView(), QEvent::MouseButtonRelease, {6, 4});
    CHECK_FALSE(d->layers().find(moving));
    CHECK(d->canvas() == composite(d->project().layers));
    CHECK(d->undoStack()->index() == 0);
    CHECK(d->undoStack()->count() == 1);
    CHECK(w.statusBar()->currentMessage().isEmpty());
}

TEST_CASE("Alt click lands a floating paste as an undoable edit", "[layers][layer-move][layer-move-review]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    auto* v = w.canvasView();
    auto action = [&](const char* name) {
        auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
        REQUIRE(a);
        return a;
    };
    action("selectTool")->trigger();
    w.selectTool()->select({2, 2, 1, 1});
    action("copy")->trigger();
    v->setCellCursor({4, 3});
    action("paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    const Canvas before = d->active()->cells;
    REQUIRE(d->undoStack()->count() == 0);
    QPoint release(3, 3);
    SECTION("zero distance Alt click") {}
    SECTION("Alt drag moves the stamped paste with its layer") { release = {6, 4}; }
    mouse(v, QEvent::MouseButtonPress, {3, 3});
    mouse(v, QEvent::MouseButtonRelease, release);
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(d->active()->cells.at(2, 1) == before.at(0, 0));
    const bool moved = release != QPoint(3, 3);
    REQUIRE(d->undoStack()->count() == (moved ? 2 : 1));
    if (moved) {
        CHECK(offset(d) == QPoint(5, 3));
        d->undoStack()->undo();
    }
    CHECK(offset(d) == QPoint(2, 2));
    d->undoStack()->undo();
    CHECK(d->active()->cells == before);
    d->undoStack()->redo();
    CHECK(d->active()->cells.at(2, 1) == before.at(0, 0));
}

TEST_CASE("Alt pen drag moves the layer without painting", "[layers][layer-move][layer-move-review]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    auto* v = w.canvasView();
    w.tools()->setActive(QStringLiteral("halfBlock"));
    const Canvas cells = d->active()->cells;
    tablet(v, QEvent::TabletPress, {3, 3});
    CHECK_FALSE(w.tools()->active()->dragging());
    tablet(v, QEvent::TabletMove, {6, 4});
    bool cancelled = false;
    SECTION("release commits even after Alt is released") {}
    SECTION("mouse events cannot interrupt a pen move") {
        mouse(v, QEvent::MouseButtonPress, {1, 1});
        mouse(v, QEvent::MouseMove, {0, 0});
        mouse(v, QEvent::MouseButtonRelease, {0, 0});
    }
    SECTION("Escape cancels a pen move") {
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(v, &esc);
        cancelled = true;
    }
    SECTION("popup focus cancels a pen move") {
        QFocusEvent lost(QEvent::FocusOut, Qt::PopupFocusReason);
        QApplication::sendEvent(v, &lost);
        cancelled = true;
    }
    tablet(v, QEvent::TabletRelease, {6, 4}, Qt::NoModifier);
    CHECK(d->active()->cells == cells);
    CHECK(offset(d) == (cancelled ? QPoint(2, 2) : QPoint(5, 3)));
    REQUIRE(d->undoStack()->count() == (cancelled ? 0 : 1));
    if (cancelled) {
        CHECK(w.statusBar()->currentMessage().isEmpty());
        return;
    }
    CHECK(d->undoStack()->undoText() == QStringLiteral("Move Layer"));
    d->undoStack()->undo();
    CHECK(offset(d) == QPoint(2, 2));
    CHECK(d->active()->cells == cells);
}

TEST_CASE("a refused Alt move clears its status on release or cancel", "[layers][layer-move][layer-move-review]") {
    MainWindow w;
    setup(w);
    auto* d = w.document();
    auto* v = w.canvasView();
    const bool locked = GENERATE(true, false);
    const bool cancel = GENERATE(true, false);
    CAPTURE(locked, cancel);
    auto props = d->active()->props;
    props.locked = locked;
    props.visible = locked;
    const QString reason = locked ? QStringLiteral("locked") : QStringLiteral("hidden");
    REQUIRE(d->setLayerProperties(d->activeLayer(), props, QStringLiteral("Set test state")));
    mouse(v, QEvent::MouseButtonPress, {3, 3});
    REQUIRE(w.statusBar()->currentMessage() == QStringLiteral("Cannot move layer: ") + reason);
    if (!cancel) {
        mouse(v, QEvent::MouseButtonRelease, {3, 3});
    } else {
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(v, &esc);
    }
    CHECK(w.statusBar()->currentMessage().isEmpty());
    props.locked = false;
    props.visible = true;
    REQUIRE(d->setLayerProperties(d->activeLayer(), props, QStringLiteral("Make movable")));
    CHECK(w.statusBar()->currentMessage().isEmpty());
    CHECK(offset(d) == QPoint(2, 2));
    CHECK(d->undoStack()->count() == 2);
}

// #237: the window torn down while an Alt-drag of a cell layer is shown.
// Destroying the document clears its undo stack, which ends a drag; nothing
// said on the way out may reach the renderer or a document already gone.
TEST_CASE("a window closed mid Alt-drag of a cell layer tears down cleanly", "[layers][layer-move]") {
    auto* w = new MainWindow;
    w->show();
    // focused, the view also loses the focus as the window goes
    const bool focused = GENERATE(false, true);
    CAPTURE(focused);
    if (focused) {
        w->activateWindow();
        QApplication::processEvents();
        w->canvasView()->setFocus();
        REQUIRE(QApplication::focusWidget() == w->canvasView());
    }
    setup(*w);
    // an edit on the undo stack, so that destroying the stack clears it
    REQUIRE(w->document()->renameLayer(w->document()->activeLayer(), QStringLiteral("Moved")));
    REQUIRE(w->document()->undoStack()->count() == 1);
    start(*w);
    REQUIRE(w->canvasView()->shownOffset(w->document()->activeLayer()));
    // The drag ends first, a repaint with everything still there; then the
    // undo stack clears as it goes, and neither it nor the document says
    // so: no one following them is whole by then.
    Document* d = w->document();
    REQUIRE(d->isModified());
    int heard = 0;
    const auto count = [&heard] { ++heard; };
    QObject::connect(d, &Document::modifiedChanged, count);
    QObject::connect(d->undoStack(), &QUndoStack::indexChanged, count);
    QObject::connect(d->undoStack(), &QUndoStack::cleanChanged, count);
    delete w;
    CHECK(heard == 0);
}
