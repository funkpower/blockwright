// Timings for the drawing tools on a big canvas (PLAN §4): fill, a filled
// rectangle dragged through five moves, a small filled rectangle whose end
// point moves back and forth 200 times, and a ramp gradient in a 20x10
// selection whose end moves the same way, on 400x2000. Hidden ([.perf]);
// run in a Release tree with `blockwright-app-tests "[perf]"`.

#include "canvas_view.h"
#include "document.h"
#include "frame_tool.h"
#include "main_window.h"
#include "tool.h"

#include "blockwright/layer.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QMouseEvent>
#include <QUndoStack>

#include <chrono>
#include <cstdio>

using namespace bw;

namespace {

void mouse(CanvasView* v, QEvent::Type type, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    const QPointF at(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
    // let the view paint what changed, as it would on screen
    v->repaint();
}

void bigCanvas(MainWindow& w) {
    AnsiDocument doc;
    doc.canvas = Canvas(400, 2000);
    w.document()->replace(std::move(doc), QString());
    w.resize(1200, 900);
    w.show();
    QApplication::processEvents();
}

double since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE("timing: fill and a filled rectangle on 400x2000", "[.perf]") {
    {
        MainWindow w;
        bigCanvas(w);
        CanvasView* v = w.canvasView();
        w.findChild<QAction*>(QStringLiteral("fillTool"))->trigger();
        const auto t0 = std::chrono::steady_clock::now();
        mouse(v, QEvent::MouseButtonPress, 3, 3);
        mouse(v, QEvent::MouseButtonRelease, 3, 3);
        const double s = since(t0);
        CHECK(w.document()->undoStack()->count() == 1);
        std::printf("perf: fill 400x2000: %.3f s\n", s);
    }
    {
        MainWindow w;
        bigCanvas(w);
        CanvasView* v = w.canvasView();
        w.findChild<QAction*>(QStringLiteral("rectangleTool"))->trigger();
        w.findChild<QAction*>(QStringLiteral("filledShapes"))->setChecked(true);
        const auto t0 = std::chrono::steady_clock::now();
        mouse(v, QEvent::MouseButtonPress, 2, 2);
        for (int i = 1; i <= 5; ++i)
            mouse(v, QEvent::MouseMove, 2 + 79 * i, 2 + 799 * i);
        const double moves = since(t0);
        mouse(v, QEvent::MouseButtonRelease, 397, 3997);
        const double s = since(t0);
        CHECK(w.document()->undoStack()->count() == 1);
        std::printf("perf: filled rect press + 5 moves: %.3f s (with release %.3f s)\n", moves, s);
    }
    {
        MainWindow w;
        bigCanvas(w);
        CanvasView* v = w.canvasView();
        w.findChild<QAction*>(QStringLiteral("rectangleTool"))->trigger();
        w.findChild<QAction*>(QStringLiteral("filledShapes"))->setChecked(true);
        const auto t0 = std::chrono::steady_clock::now();
        mouse(v, QEvent::MouseButtonPress, 2, 2);
        for (int i = 0; i < 200; ++i)
            mouse(v, QEvent::MouseMove, 3 + (i & 1), 3);
        mouse(v, QEvent::MouseButtonRelease, 3, 3);
        const double s = since(t0);
        CHECK(w.document()->undoStack()->count() == 1);
        std::printf("perf: small filled rect, 200 moves: %.3f s\n", s);
    }
    {
        MainWindow w;
        bigCanvas(w);
        CanvasView* v = w.canvasView();
        w.findChild<QAction*>(QStringLiteral("gradientTool"))->trigger();
        v->setSelection(Selection(CellRect{10, 5, 20, 10}));
        const auto t0 = std::chrono::steady_clock::now();
        mouse(v, QEvent::MouseButtonPress, 12, 14);
        mouse(v, QEvent::MouseMove, 27, 26);
        const double first = since(t0);
        for (int i = 0; i < 200; ++i)
            mouse(v, QEvent::MouseMove, 26 + (i & 1), 24 + 2 * (i & 1));
        const double moves = since(t0) - first;
        mouse(v, QEvent::MouseButtonRelease, 27, 26);
        const double s = since(t0);
        CHECK(w.document()->undoStack()->count() == 1);
        std::printf("perf: gradient in a 20x10 selection, press + first move %.3f s, 200 moves %.3f s (%.2f ms a "
                    "move), with release %.3f s\n",
                    first, moves, moves * 1000 / 200, s);
    }
}

// A 400x200 single frame dragged through five moves onto the clear middle
// of three cells layers, the bottom one painted, the top one with a patch:
// each clear cell the frame keeps the background of reads what shows below
// it (Document::shownBelow). On 80x50, where most of the frame is off the
// canvas, and on 400x2000.
TEST_CASE("timing: a 400x200 frame over three layers", "[.perf]") {
    for (const auto& [cw, ch] : {std::pair{80, 50}, std::pair{400, 2000}}) {
        MainWindow w;
        AnsiDocument a;
        a.canvas = Canvas(cw, ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                a.canvas.at(x, y) = Cell{U' ', Color::indexed(7), Color::indexed(4)};
        w.document()->replace(std::move(a), QString());
        w.resize(1200, 900);
        w.show();
        QApplication::processEvents();
        Document* doc = w.document();
        const LayerId mid = doc->addLayer(LayerKind::Cells);
        doc->addLayer(LayerKind::Cells);
        CellEdit patch;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 20; ++x)
                patch.changes.push_back({x, y, doc->layerCanvas().at(x, y), Cell{U' ', Color::indexed(7), Color::indexed(1)}});
        REQUIRE(doc->edit(patch));
        REQUIRE(doc->setActiveLayer(mid));
        w.findChild<QAction*>(QStringLiteral("frameTool"))->trigger();
        auto* t = qobject_cast<FrameTool*>(w.tools()->active());
        REQUIRE(t);
        t->setStyle(QStringLiteral("single"));
        CanvasView* v = w.canvasView();
        const int steps = doc->undoStack()->count();
        const auto t0 = std::chrono::steady_clock::now();
        mouse(v, QEvent::MouseButtonPress, 0, 0);
        for (int i = 1; i <= 5; ++i)
            mouse(v, QEvent::MouseMove, 80 * i - 1, 2 * (40 * i - 1));
        const double moves = since(t0);
        mouse(v, QEvent::MouseButtonRelease, 399, 2 * 199);
        const double s = since(t0);
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(doc->layerCanvas().at(0, 0).bg == Color::indexed(4));
        std::printf("perf: frame 400x200 on %dx%d, 3 layers: 5 moves %.3f s, total %.3f s\n", cw, ch, moves, s);
    }
}
