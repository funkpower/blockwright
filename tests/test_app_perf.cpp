// Timings for the drawing tools on a big canvas (PLAN §4): fill, a filled
// rectangle dragged through five moves, and a small filled rectangle whose
// end point moves back and forth 200 times, on 400x2000. Hidden ([.perf]);
// run in a Release tree with `blockwright-app-tests "[perf]"`.

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "tool.h"

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
}
