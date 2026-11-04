// Wide pairs in the editor (PLAN §16, slice 2): the document settles every
// edit, the composite shows a pair only where both halves show, rotation
// refuses a pair with a status message, and mirror mode says when a pair
// gets no mirrored copy. The tests build pairs with Glyph::continuation().

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "select_tool.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
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
