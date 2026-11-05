// Wide pairs in the editor (PLAN §16, slice 2): the document settles every
// edit, the composite shows a pair only where both halves show, rotation
// refuses a pair with a status message, and mirror mode says when a pair
// gets no mirrored copy, and a tool's cells in progress draw a pair beside
// them whole (slice 7). The tests build pairs with Glyph::continuation().

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "select_tool.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QMouseEvent>
#include <QStatusBar>
#include <QUndoStack>

using namespace bw;

namespace {

const Glyph kCont = Glyph::continuation();

Cell cell(Glyph g, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = g;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

// Writes 中 and its continuation at (x, y) on the active layer, one edit.
void writePair(Document& doc, int x, int y, std::uint8_t fg = Yellow, std::uint8_t bg = Blue) {
    CellEdit e;
    e.changes.push_back({x, y, doc.layerCanvas().at(x, y), cell(U'中', fg, bg)});
    e.changes.push_back({x + 1, y, doc.layerCanvas().at(x + 1, y), cell(kCont, fg, bg)});
    REQUIRE(doc.edit(e, {}, {}));
}

bool write(Document& doc, int x, int y, const Cell& c) {
    CellEdit e;
    e.changes.push_back({x, y, doc.layerCanvas().at(x, y), c});
    return doc.edit(e, {}, {});
}

// The middle of cell x's upper half, or its lower half when py is odd.
QPointF pixelAt(CanvasView* v, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    return QPointF(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
}

void mouseAt(CanvasView* v, QEvent::Type type, int x, int py) {
    const QPointF at = pixelAt(v, x, py);
    const Qt::MouseButtons held =
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

// The pixels of cell (x, y) that differ between two grabs of the view.
int differing(CanvasView* v, const QImage& a, const QImage& b, int x, int y) {
    const QRect r = v->toWidget(v->cellRect(QPoint(x, y)));
    const qreal d = a.devicePixelRatio();
    int n = 0;
    for (int py = r.top(); py <= r.bottom(); ++py)
        for (int px = r.left(); px <= r.right(); ++px)
            n += a.pixel(int(px * d), int(py * d)) != b.pixel(int(px * d), int(py * d));
    return n;
}

} // namespace

TEST_CASE("the document settles an edit over half a pair, and undo puts the pair back", "[wide][app]") {
    MainWindow w;
    Document& doc = *w.document();
    writePair(doc, 2, 0);
    REQUIRE(doc.layerCanvas().at(3, 0).ch.isContinuation());
    REQUIRE(write(doc, 3, 0, cell(U'B', White, Red)));
    CHECK(doc.layerCanvas().at(2, 0).ch == Glyph(U' '));
    CHECK(doc.layerCanvas().at(2, 0).bg == Color::indexed(Blue));
    CHECK(doc.canvas().at(2, 0).ch == Glyph(U' '));
    doc.undoStack()->undo();
    CHECK(doc.layerCanvas().at(2, 0).ch == Glyph(U'中'));
    CHECK(doc.layerCanvas().at(3, 0).ch.isContinuation());
}

TEST_CASE("the composite of a layer over half a pair in the document", "[wide][app]") {
    MainWindow w;
    Document& doc = *w.document();
    writePair(doc, 2, 0);
    doc.addLayer(LayerKind::Cells);
    REQUIRE(write(doc, 3, 0, cell(U'X', White, Red)));
    CHECK(doc.canvas().at(2, 0).ch == Glyph(U' '));
    CHECK(doc.canvas().at(2, 0).bg == Color::indexed(Blue));
    CHECK(doc.canvas().at(3, 0).ch == Glyph(U'X'));
    doc.undoStack()->undo();
    CHECK(doc.canvas().at(2, 0).ch == Glyph(U'中'));
    CHECK(doc.canvas().at(3, 0).ch.isContinuation());
    // over the lead
    REQUIRE(write(doc, 2, 0, cell(U'Y', White, Red)));
    CHECK(doc.canvas().at(3, 0).ch == Glyph(U' '));
}

TEST_CASE("a lone wide cell survives an edit of the cell beside it in the document", "[wide][app][lone]") {
    MainWindow w;
    Document& doc = *w.document();
    REQUIRE(write(doc, 2, 0, cell(U'中', Yellow, Blue)));
    REQUIRE(write(doc, 3, 0, cell(U'x', White, Red)));
    CHECK(doc.canvas().at(2, 0).ch == Glyph(U'中'));
    REQUIRE(write(doc, 1, 0, cell(U'y', White, Red)));
    CHECK(doc.canvas().at(2, 0).ch == Glyph(U'中'));
}

TEST_CASE("rotating a selection that holds half a pair is refused with a status message", "[wide][app][rotate]") {
    MainWindow w;
    Document& doc = *w.document();
    writePair(doc, 2, 1);
    const int steps = doc.undoStack()->count();
    act(w, "selectTool")->trigger();
    for (const CellRect r : {CellRect{0, 0, 3, 3}, CellRect{3, 0, 3, 3}, CellRect{0, 0, 6, 3}}) {
        w.statusBar()->clearMessage();
        w.selectTool()->select(r);
        act(w, "rotateClockwise")->trigger();
        CHECK(doc.undoStack()->count() == steps);
        CHECK(w.selectTool()->selection().bounds() == r);
        CHECK(w.statusBar()->currentMessage().contains(QStringLiteral("wide")));
    }
    CHECK(doc.canvas().at(2, 1).ch == Glyph(U'中'));
    CHECK(doc.canvas().at(3, 1).ch.isContinuation());

    // a floating block of it, too
    w.selectTool()->select({2, 1, 2, 1});
    act(w, "copy")->trigger();
    act(w, "paste")->trigger();
    w.statusBar()->clearMessage();
    act(w, "rotateClockwise")->trigger();
    CHECK(w.statusBar()->currentMessage().contains(QStringLiteral("wide")));
}

TEST_CASE("a lone wide cell rotates in the editor", "[wide][app][rotate][lone]") {
    MainWindow w;
    Document& doc = *w.document();
    REQUIRE(write(doc, 1, 0, cell(U'中', Yellow, Blue)));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    w.statusBar()->clearMessage();
    act(w, "rotateClockwise")->trigger();
    CHECK(doc.canvas().at(0, 1).ch == Glyph(U'中'));
    CHECK_FALSE(w.statusBar()->currentMessage().contains(QStringLiteral("wide")));
}

TEST_CASE("mirror mode says when a pair over the centre column has no mirrored copy", "[wide][app][mirror]") {
    MainWindow w;
    Document& doc = *w.document();
    doc.clear(9, 2, true);
    CanvasView* v = w.canvasView();
    v->setMirror(true);
    w.statusBar()->clearMessage();
    CellEdit e;
    e.changes.push_back({3, 0, doc.layerCanvas().at(3, 0), cell(U'中', Yellow, Blue)});
    e.changes.push_back({4, 0, doc.layerCanvas().at(4, 0), cell(kCont, Yellow, Blue)});
    REQUIRE(v->applyEdit(e));
    CHECK(w.statusBar()->currentMessage().contains(QStringLiteral("mirror")));
    CHECK(doc.canvas().at(3, 0).ch == Glyph(U'中'));
    CHECK(doc.canvas().at(5, 0).ch == Glyph(U' '));

    // one clear of the centre gets a copy and no message
    w.statusBar()->clearMessage();
    CellEdit f;
    f.changes.push_back({0, 1, doc.layerCanvas().at(0, 1), cell(U'中', Yellow, Blue)});
    f.changes.push_back({1, 1, doc.layerCanvas().at(1, 1), cell(kCont, Yellow, Blue)});
    REQUIRE(v->applyEdit(f));
    CHECK(w.statusBar()->currentMessage().isEmpty());
    CHECK(doc.canvas().at(7, 1).ch == Glyph(U'中'));
    CHECK(doc.canvas().at(8, 1).ch.isContinuation());
}

// A tool's cells in progress are drawn from a rectangle of the canvas; a pair
// across its edge draws as it does on the canvas, not squeezed or cut.
TEST_CASE("a line in progress leaves a pair at the right edge of its cells as it was", "[wide][app][preview]") {
    MainWindow w;
    Document& doc = *w.document();
    doc.clear(10, 3, true);
    // the lead in the last column of the cells the line spans, its continuation past it
    writePair(doc, 6, 0, White, Red);
    CanvasView* v = w.canvasView();
    v->setZoom(2);
    act(w, "lineTool")->trigger();
    v->setCellCursor({9, 2});
    const QImage before = v->grab().toImage();
    mouseAt(v, QEvent::MouseButtonPress, 1, 1); // cell (1, 0)
    mouseAt(v, QEvent::MouseMove, 5, 3);        // cell (5, 1)
    v->setCellCursor({9, 2});
    const QImage during = v->grab().toImage();
    CHECK(differing(v, before, during, 6, 0) == 0);
    CHECK(differing(v, before, during, 7, 0) == 0);
    mouseAt(v, QEvent::MouseButtonRelease, 5, 3);
    const QImage after = v->grab().toImage();
    CHECK(differing(v, before, after, 6, 0) == 0);
    CHECK(differing(v, before, after, 7, 0) == 0);
}

TEST_CASE("a line in progress leaves a pair at the left edge of its cells as it was", "[wide][app][preview]") {
    MainWindow w;
    Document& doc = *w.document();
    doc.clear(10, 3, true);
    // the continuation in the first column of the cells the line spans, its lead before it
    writePair(doc, 1, 1, White, Red);
    CanvasView* v = w.canvasView();
    v->setZoom(2);
    act(w, "lineTool")->trigger();
    v->setCellCursor({9, 2});
    const QImage before = v->grab().toImage();
    mouseAt(v, QEvent::MouseButtonPress, 3, 1); // cell (3, 0)
    mouseAt(v, QEvent::MouseMove, 7, 3);        // cell (7, 1)
    v->setCellCursor({9, 2});
    const QImage during = v->grab().toImage();
    CHECK(differing(v, before, during, 1, 1) == 0);
    CHECK(differing(v, before, during, 2, 1) == 0);
    mouseAt(v, QEvent::MouseButtonRelease, 7, 3);
    const QImage after = v->grab().toImage();
    CHECK(differing(v, before, after, 1, 1) == 0);
    CHECK(differing(v, before, after, 2, 1) == 0);
}

TEST_CASE("a floating paste leaves pairs at both edges of its cells as they were", "[wide][app][preview]") {
    MainWindow w;
    Document& doc = *w.document();
    doc.clear(10, 4, true);
    for (int y : {2, 3})
        for (int x = 0; x < 3; ++x)
            REQUIRE(write(doc, x, y, cell(U'A' + char32_t(x), White, Black)));
    // beside the block's columns (3 to 5) in its second row, each with a
    // half in the column the cells it draws run to on that side
    writePair(doc, 1, 1, White, Red);
    writePair(doc, 6, 1, White, Red);
    CanvasView* v = w.canvasView();
    v->setZoom(2);
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 2, 3, 2});
    act(w, "copy")->trigger();
    w.selectTool()->deselect();
    v->setCellCursor({3, 0});
    const QImage before = v->grab().toImage();
    act(w, "paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    v->setCellCursor({3, 0});
    const QImage during = v->grab().toImage();
    for (int x : {1, 2, 6, 7})
        CHECK(differing(v, before, during, x, 1) == 0);
    CHECK(differing(v, before, during, 3, 1) > 0);
}
