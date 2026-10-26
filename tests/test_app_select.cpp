// The selection tool, mirror mode, guides and the reference image in the
// main window (PLAN §4), driven through the menus, keys and mouse.

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "reference_scale.h"
#include "select_tool.h"
#include "tool.h"

#include "system_input.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QSet>
#include <QStatusBar>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <vector>

using namespace bw;

namespace {

void press(QWidget* w, int k, const QString& text = {}) {
    QKeyEvent down(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &up);
}

void typeText(QWidget* w, const QString& text) {
    for (const QChar c : text)
        press(w, c.toUpper().unicode(), QString(c));
}

void mouse(CanvasView* v, QEvent::Type type, QPoint cell) {
    const QPointF at = v->cellRect(cell).center();
    const Qt::MouseButton button = Qt::LeftButton;
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

void drag(CanvasView* v, QPoint from, QPoint to) {
    mouse(v, QEvent::MouseButtonPress, from);
    mouse(v, QEvent::MouseMove, to);
    mouse(v, QEvent::MouseButtonRelease, to);
}

void click(CanvasView* v, QPoint cell) {
    mouse(v, QEvent::MouseButtonPress, cell);
    mouse(v, QEvent::MouseButtonRelease, cell);
}

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

// The glyphs of row `y` from column `x`, `n` cells.
QString row(MainWindow& w, int x, int y, int n) {
    QString s;
    for (int i = 0; i < n; ++i) {
        const char32_t ch = w.document()->canvas().at(x + i, y).ch;
        s += QString::fromUcs4(&ch, 1);
    }
    return s;
}

// Types `text` from (x, y) in keyboard mode.
void write(MainWindow& w, int x, int y, const QString& text) {
    w.tools()->setActive(QStringLiteral("keyboard"));
    w.canvasView()->setCellCursor({x, y});
    typeText(w.canvasView(), text);
}

} // namespace

TEST_CASE("the tool registry starts in keyboard mode and the Tools menu switches") {
    MainWindow w;
    REQUIRE(w.tools()->tools().size() == 23);
    CHECK(w.tools()->active()->id() == QStringLiteral("keyboard"));
    CHECK(w.canvasView()->tool() == w.tools()->active());
    CHECK(act(w, "keyboardTool")->isChecked());

    act(w, "selectTool")->trigger();
    CHECK(w.tools()->active() == w.selectTool());
    CHECK(w.canvasView()->tool() == w.selectTool());
    CHECK(act(w, "selectTool")->isChecked());

    w.tools()->setActive(QStringLiteral("keyboard"));
    CHECK(act(w, "keyboardTool")->isChecked());
    CHECK(w.canvasView()->tool() == w.tools()->find(QStringLiteral("keyboard")));
}

TEST_CASE("copy and paste float the cells; Enter stamps them as one undo step") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("ab"));
    const int typed = undo->count();
    CHECK_FALSE(act(w, "paste")->isEnabled());

    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    CHECK(act(w, "copy")->isEnabled());
    act(w, "copy")->trigger();
    REQUIRE(act(w, "paste")->isEnabled());

    v->setCellCursor({5, 2});
    act(w, "paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(5, 2));
    // Floating cells are only drawn until stamped.
    CHECK(row(w, 5, 2, 2) == QStringLiteral("  "));
    CHECK(undo->count() == typed);

    press(v, Qt::Key_Return);
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(row(w, 5, 2, 2) == QStringLiteral("ab"));
    CHECK(w.selectTool()->selection().bounds() == CellRect{5, 2, 2, 1});
    CHECK(undo->count() == typed + 1);

    undo->undo();
    CHECK(row(w, 5, 2, 2) == QStringLiteral("  "));
    CHECK(row(w, 0, 0, 2) == QStringLiteral("ab"));
}

TEST_CASE("paste works from keyboard mode and a click stamps it") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("xy"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    act(w, "copy")->trigger();

    w.tools()->setActive(QStringLiteral("keyboard"));
    v->setCellCursor({10, 3});
    act(w, "paste")->trigger();
    CHECK(w.tools()->active() == w.selectTool());
    REQUIRE(w.selectTool()->floating());

    click(v, {11, 3});  // on the floating cells
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(row(w, 10, 3, 2) == QStringLiteral("xy"));
}

TEST_CASE("cut clears the selection as one undo step") {
    MainWindow w;
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("cut"));
    const int typed = undo->count();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 3, 1});
    act(w, "cut")->trigger();
    CHECK(row(w, 0, 0, 3) == QStringLiteral("   "));
    CHECK(undo->count() == typed + 1);
    CHECK(w.selectTool()->clipboard().width == 3);

    w.canvasView()->setCellCursor({0, 1});
    act(w, "paste")->trigger();
    press(w.canvasView(), Qt::Key_Enter);
    CHECK(row(w, 0, 1, 3) == QStringLiteral("cut"));

    undo->undo();
    undo->undo();
    CHECK(row(w, 0, 0, 3) == QStringLiteral("cut"));
    CHECK(row(w, 0, 1, 3) == QStringLiteral("   "));
}

TEST_CASE("dragging selects a rectangle and a click deselects") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    act(w, "selectTool")->trigger();
    drag(v, {6, 4}, {2, 1});
    CHECK(w.selectTool()->selection().bounds() == CellRect{2, 1, 5, 4});
    CHECK(act(w, "flipHorizontal")->isEnabled());

    // Past the edge the selection stops at the canvas.
    drag(v, {78, 38}, {90, 45});
    CHECK(w.selectTool()->selection().bounds() == CellRect{78, 38, 2, 2});

    click(v, {20, 20});
    CHECK(w.selectTool()->selection().empty());
    CHECK(w.canvasView()->cellCursor() == Cursor{20, 20});
    CHECK_FALSE(act(w, "flipHorizontal")->isEnabled());
}

TEST_CASE("the floating selection moves with arrows and Esc puts it back") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("ab"));
    const int typed = undo->count();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});

    press(v, Qt::Key_Right);
    REQUIRE(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(1, 0));
    CHECK(row(w, 0, 0, 3) == QStringLiteral("ab "));

    press(v, Qt::Key_Escape);
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(w.selectTool()->selection().bounds() == CellRect{0, 0, 2, 1});
    CHECK(row(w, 0, 0, 3) == QStringLiteral("ab "));
    CHECK(undo->count() == typed);

    press(v, Qt::Key_Down);
    press(v, Qt::Key_Right);
    press(v, Qt::Key_Right);
    press(v, Qt::Key_Return);
    CHECK(row(w, 0, 0, 2) == QStringLiteral("  "));
    CHECK(row(w, 2, 1, 2) == QStringLiteral("ab"));
    CHECK(undo->count() == typed + 1);
    undo->undo();
    CHECK(row(w, 0, 0, 2) == QStringLiteral("ab"));
    CHECK(row(w, 2, 1, 2) == QStringLiteral("  "));

    // Enter with nothing floating is keyboard mode's: the next line.
    w.canvasView()->setCellCursor({4, 5});
    press(v, Qt::Key_Return);
    CHECK(v->cellCursor() == Cursor{0, 6});

    // Esc with nothing floating deselects.
    w.selectTool()->select({0, 0, 1, 1});
    press(v, Qt::Key_Escape);
    CHECK(w.selectTool()->selection().empty());
}

TEST_CASE("dragging the selection moves its cells") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("mv"));
    const int typed = undo->count();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});

    // A click inside the selection, without a drag, moves nothing.
    click(v, {1, 0});
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(w.selectTool()->selection().bounds() == CellRect{0, 0, 2, 1});

    drag(v, {1, 0}, {4, 3});
    REQUIRE(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(3, 3));
    CHECK(undo->count() == typed);

    // Clicking off the floating cells stamps them and starts over.
    click(v, {40, 20});
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(row(w, 0, 0, 2) == QStringLiteral("  "));
    CHECK(row(w, 3, 3, 2) == QStringLiteral("mv"));
    CHECK(undo->count() == typed + 1);
    CHECK(w.selectTool()->selection().empty());
}

TEST_CASE("Delete clears the selection, or the cell in keyboard mode") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("abcd"));
    const int typed = undo->count();

    act(w, "selectTool")->trigger();
    w.selectTool()->select({1, 0, 2, 1});
    press(v, Qt::Key_Delete);
    CHECK(row(w, 0, 0, 4) == QStringLiteral("a  d"));
    CHECK(undo->count() == typed + 1);
    CHECK_FALSE(w.selectTool()->selection().empty());

    undo->undo();
    act(w, "deleteSelection")->trigger();
    CHECK(row(w, 0, 0, 4) == QStringLiteral("a  d"));
    undo->undo();

    // With nothing selected Delete is keyboard mode's.
    w.selectTool()->deselect();
    v->setCellCursor({3, 0});
    press(v, Qt::Key_Delete);
    CHECK(row(w, 0, 0, 4) == QStringLiteral("abc "));

    w.tools()->setActive(QStringLiteral("keyboard"));
    v->setCellCursor({0, 0});
    press(v, Qt::Key_Delete);
    CHECK(row(w, 0, 0, 4) == QStringLiteral(" bc "));
}

TEST_CASE("flips and rotations from the menu are one undo step each") {
    MainWindow w;
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QString::fromUtf8("▀▌/"));  // ▀▌/
    const int typed = undo->count();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 3, 1});

    act(w, "flipHorizontal")->trigger();
    CHECK(row(w, 0, 0, 3) == QString::fromUtf8("\\▐▀"));  // \▐▀
    CHECK(undo->count() == typed + 1);
    undo->undo();
    CHECK(row(w, 0, 0, 3) == QString::fromUtf8("▀▌/"));

    act(w, "flipVertical")->trigger();
    CHECK(row(w, 0, 0, 3) == QString::fromUtf8("▄▌\\"));  // ▄▌\ .
    undo->undo();

    act(w, "rotateClockwise")->trigger();
    CHECK(w.selectTool()->selection().bounds() == CellRect{0, 0, 1, 3});
    CHECK(row(w, 0, 0, 3) == QString::fromUtf8("▐  "));  // ▀ turns to ▐
    CHECK(row(w, 0, 1, 1) == QString::fromUtf8("▀"));    // ▌ turns to ▀
    CHECK(undo->count() == typed + 1);
    undo->undo();
    // Undo puts the cells back, not the selection.
    w.selectTool()->select({0, 0, 3, 1});

    act(w, "rotateCounterClockwise")->trigger();
    CHECK(w.selectTool()->selection().bounds() == CellRect{0, 0, 1, 3});
    CHECK(row(w, 0, 0, 1) == QString::fromUtf8("\\"));
    CHECK(row(w, 0, 2, 1) == QString::fromUtf8("▌"));  // ▀ turns to ▌
    undo->undo();
    CHECK(row(w, 0, 0, 3) == QString::fromUtf8("▀▌/"));
    CHECK(undo->index() == typed);
}

TEST_CASE("a floating selection flips without an edit until stamped") {
    MainWindow w;
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("(]"));
    const int typed = undo->count();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    act(w, "copy")->trigger();
    w.canvasView()->setCellCursor({0, 1});
    act(w, "paste")->trigger();
    act(w, "flipHorizontal")->trigger();
    CHECK(undo->count() == typed);
    press(w.canvasView(), Qt::Key_Return);
    CHECK(row(w, 0, 1, 2) == QStringLiteral("[)"));
    CHECK(undo->count() == typed + 1);
}

TEST_CASE("the paste mode is chosen in the Selection menu") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    CHECK(act(w, "pasteNormal")->isChecked());
    write(w, 0, 0, QStringLiteral("a c"));  // the middle cell is blank
    write(w, 0, 1, QStringLiteral("xyz"));
    write(w, 0, 2, QStringLiteral("x z "));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 3, 1});
    act(w, "copy")->trigger();

    act(w, "pasteTransparent")->trigger();
    CHECK(w.selectTool()->pasteMode() == PasteMode::Transparent);
    CHECK(act(w, "pasteTransparent")->isChecked());
    v->setCellCursor({0, 1});
    act(w, "paste")->trigger();
    press(v, Qt::Key_Return);
    CHECK(row(w, 0, 1, 3) == QStringLiteral("ayc"));

    act(w, "pasteUnder")->trigger();
    CHECK(w.selectTool()->pasteMode() == PasteMode::Under);
    v->setCellCursor({1, 2});
    act(w, "paste")->trigger();
    press(v, Qt::Key_Return);
    CHECK(row(w, 0, 2, 4) == QStringLiteral("xazc"));  // only into the blank cells

    act(w, "pasteNormal")->trigger();
    v->setCellCursor({0, 2});
    act(w, "paste")->trigger();
    press(v, Qt::Key_Return);
    CHECK(row(w, 0, 2, 4) == QStringLiteral("a cc"));
}

TEST_CASE("mirror mode repeats typing at the mirrored column") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    QAction* mirror = act(w, "mirrorMode");
    CHECK_FALSE(mirror->isChecked());
    mirror->trigger();
    CHECK(v->mirror());

    write(w, 0, 0, QStringLiteral("/("));
    CHECK(row(w, 0, 0, 2) == QStringLiteral("/("));
    CHECK(row(w, 78, 0, 2) == QStringLiteral(")\\"));
    undo->undo();
    CHECK(row(w, 78, 0, 2) == QStringLiteral(" \\"));
    CHECK(row(w, 0, 0, 2) == QStringLiteral("/ "));

    // Delete goes through it too.
    v->setCellCursor({0, 0});
    press(v, Qt::Key_Delete);
    CHECK(row(w, 79, 0, 1) == QStringLiteral(" "));

    v->setMirror(false);
    CHECK_FALSE(mirror->isChecked());
    write(w, 5, 0, QStringLiteral("q"));
    CHECK(row(w, 74, 0, 1) == QStringLiteral(" "));
}

TEST_CASE("mirror mode applies to a stamped selection") {
    MainWindow w;
    write(w, 0, 0, QStringLiteral("<"));
    act(w, "mirrorMode")->trigger();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    act(w, "copy")->trigger();
    w.canvasView()->setCellCursor({1, 3});
    act(w, "paste")->trigger();
    press(w.canvasView(), Qt::Key_Return);
    CHECK(row(w, 1, 3, 1) == QStringLiteral("<"));
    CHECK(row(w, 78, 3, 1) == QStringLiteral(">"));
}

TEST_CASE("switching tools stamps a floating selection and keeps the selection; a new canvas drops it") {
    MainWindow w;
    write(w, 0, 0, QStringLiteral("k"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    press(w.canvasView(), Qt::Key_Right);
    REQUIRE(w.selectTool()->floating());
    act(w, "keyboardTool")->trigger();
    CHECK(row(w, 0, 0, 2) == QStringLiteral(" k"));
    CHECK(w.selectTool()->selection().bounds() == CellRect{1, 0, 1, 1});

    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 4, 4});
    press(w.canvasView(), Qt::Key_Down);
    REQUIRE(w.selectTool()->floating());
    w.document()->clear();
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(w.selectTool()->selection().empty());
    CHECK(w.document()->undoStack()->count() == 0);
}

TEST_CASE("guides and the reference image are drawn, never saved") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("hi"));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString plain = dir.filePath(QStringLiteral("plain.ans"));
    QString error;
    REQUIRE(w.saveFile(plain, &error));

    CHECK(act(w, "guideNone")->isChecked());
    act(w, "guide80x25")->trigger();
    CHECK(v->guide() == QSize(80, 25));
    CHECK(act(w, "guide80x25")->isChecked());

    QImage red(64, 32, QImage::Format_RGB32);
    red.fill(Qt::red);
    const QString png = dir.filePath(QStringLiteral("ref.png"));
    REQUIRE(red.save(png));
    CHECK_FALSE(act(w, "showReferenceImage")->isEnabled());
    CHECK_FALSE(w.loadReferenceImage(dir.filePath(QStringLiteral("missing.png")), &error));
    REQUIRE(w.loadReferenceImage(png, &error));
    CHECK(v->referenceVisible());
    CHECK(act(w, "showReferenceImage")->isEnabled());
    CHECK(act(w, "showReferenceImage")->isChecked());
    CHECK_FALSE(w.document()->isModified());

    // Drawn: the reference tints the black canvas red at half opacity; the
    // guide is a magenta line under row 24.
    v->setZoom(1);
    QImage shot = v->grab().toImage();
    const QColor tinted = shot.pixelColor(40, 300);  // row 18, column 5: blank
    CHECK(tinted.red() > 90);
    CHECK(tinted.red() < 170);
    CHECK(tinted.green() < 20);
    bool guide = false;
    for (int x = 0; x < 16 && !guide; ++x) {
        const QColor c = shot.pixelColor(x, 25 * 16);
        guide = c.red() > 200 && c.blue() > 200 && c.green() < 60;
    }
    CHECK(guide);

    act(w, "referenceOpacity100")->trigger();
    CHECK(v->referenceOpacity() == 1.0);
    shot = v->grab().toImage();
    CHECK(shot.pixelColor(40, 300).red() > 240);

    act(w, "showReferenceImage")->setChecked(false);
    CHECK_FALSE(v->referenceVisible());
    shot = v->grab().toImage();
    CHECK(shot.pixelColor(40, 300) == QColor(Qt::black));

    // Saved: the same bytes as without them.
    const QString overlaid = dir.filePath(QStringLiteral("overlaid.ans"));
    act(w, "showReferenceImage")->setChecked(true);
    REQUIRE(w.saveFile(overlaid, &error));
    QFile a(plain), b(overlaid);
    REQUIRE(a.open(QIODevice::ReadOnly));
    REQUIRE(b.open(QIODevice::ReadOnly));
    CHECK(a.readAll() == b.readAll());

    act(w, "clearReferenceImage")->trigger();
    CHECK(v->referenceImage().isNull());
    CHECK_FALSE(act(w, "showReferenceImage")->isEnabled());
    act(w, "guideNone")->trigger();
    CHECK(v->guide().isEmpty());
}

TEST_CASE("the select tool's shortcuts are bound") {
    // the clash check itself is "no two actions share a shortcut"
    MainWindow w;
    QSet<QString> seen;
    for (QAction* a : w.findChildren<QAction*>())
        for (const QKeySequence& seq : a->shortcuts())
            seen.insert(seq.toString(QKeySequence::PortableText));
    CHECK(seen.contains(QStringLiteral("Ctrl+C")));
    CHECK(seen.contains(QStringLiteral("Ctrl+V")));
    CHECK(seen.contains(QStringLiteral("Ctrl+M")));
}

TEST_CASE("the reference image item sits above Quit in the File menu") {
    MainWindow w;
    QAction* fileMenu = w.menuBar()->actions().constFirst();
    REQUIRE(fileMenu->menu());
    const QList<QAction*> items = fileMenu->menu()->actions();
    const qsizetype ref = items.indexOf(act(w, "openReferenceImage"));
    REQUIRE(ref > 0);
    CHECK(items.at(ref - 1)->isSeparator());
    REQUIRE(ref + 1 < items.size());
    CHECK(items.at(ref + 1)->isSeparator());
    CHECK(items.back()->shortcut() == QKeySequence(QKeySequence::Quit));
    CHECK(items.at(items.size() - 2) == items.at(ref + 1));

    QStringList menus;
    for (QAction* m : w.menuBar()->actions())
        menus << m->text();
    CHECK(menus.indexOf(QStringLiteral("&Tools")) == menus.indexOf(QStringLiteral("&Selection")) - 1);
    CHECK(menus.indexOf(QStringLiteral("&Selection")) == menus.indexOf(QStringLiteral("&Layer")) - 1);
    CHECK(menus.indexOf(QStringLiteral("&Layer")) == menus.indexOf(QStringLiteral("&Window")) - 1);
    CHECK(menus.indexOf(QStringLiteral("&Window")) == menus.indexOf(QStringLiteral("&Help")) - 1);
}

TEST_CASE("a floating selection is drawn as it will stamp") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    write(w, 0, 0, QString::fromUtf8("█A"));  // █A
    write(w, 5, 2, QStringLiteral("zz"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    act(w, "copy")->trigger();
    v->setCellCursor({5, 2});
    act(w, "paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    CHECK(row(w, 5, 2, 2) == QStringLiteral("zz"));
    const QRect cells = v->cellRect(QPoint(5, 2)) | v->cellRect(QPoint(6, 2));
    // Inside the marquee and the cursor frame.
    const QRect inside = cells.adjusted(2, 2, -2, -2);
    const QImage floating = v->grab(inside).toImage();
    press(v, Qt::Key_Return);
    REQUIRE(row(w, 5, 2, 2) == QString::fromUtf8("█A"));
    const QImage stamped = v->grab(inside).toImage();
    CHECK(floating == stamped);

    w.document()->undoStack()->undo();
    CHECK(v->grab(inside).toImage() != stamped);

    // #35: the reference image tints floating cells and the guides show over
    // them, as once stamped
    QImage red(64, 64, QImage::Format_RGB32);
    red.fill(Qt::red);
    v->setReferenceImage(red);
    v->setReferenceOpacity(0.5);
    v->setGuide({6, 2});  // a line down the left edge of column 6
    v->setCellCursor({5, 2});
    act(w, "paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    const QImage overlaid = v->grab(inside).toImage();
    bool line = false;  // dashed, so any pixel of the column
    for (int y = 0; y < overlaid.height() && !line; ++y) {
        const QColor c = overlaid.pixelColor(6 * 8 - inside.left(), y);
        line = c.red() > 200 && c.blue() > 200 && c.green() < 60;
    }
    CHECK(line);
    const QColor tinted = overlaid.pixelColor(1, 1);  // the full block, grey under red
    CHECK(tinted.red() > tinted.green() + 40);
    press(v, Qt::Key_Return);
    REQUIRE(row(w, 5, 2, 2) == QString::fromUtf8("█A"));
    CHECK(overlaid == v->grab(inside).toImage());
}

TEST_CASE("a floating selection of a tall canvas draws at 8x") {
    // #35 P1: the preview of 80x500 cells at 8x was 327,680,000 px, past
    // renderCanvas's limit, and paintEvent threw
    MainWindow w;
    AnsiDocument doc;
    doc.canvas = Canvas(80, 500);
    doc.canvas.at(0, 0).ch = U'A';
    doc.canvas.at(79, 499).ch = U'Z';
    w.document()->replace(std::move(doc), QString());
    CanvasView* v = w.canvasView();
    v->setZoom(8);
    act(w, "selectAll")->trigger();
    press(v, Qt::Key_Right);
    REQUIRE(w.selectTool()->floating());
    const QRect inside = v->cellRect(QPoint(1, 0)).adjusted(2, 2, -2, -2);
    const QImage floating = v->grab(inside).toImage();
    CHECK(v->grab(v->cellRect(QPoint(78, 499))).toImage().size() == v->cellRect(QPoint(0, 0)).size());
    press(v, Qt::Key_Return);
    REQUIRE(row(w, 0, 0, 2) == QStringLiteral(" A"));
    CHECK(floating == v->grab(inside).toImage());
}

TEST_CASE("undo under a floating selection drops it and leaves the undone cells") {
    // #35: the float kept the cells it was lifted with, and Enter wrote them
    // over what the undo put back
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("A"));
    write(w, 0, 0, QStringLiteral("B"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    press(v, Qt::Key_Right);
    REQUIRE(w.selectTool()->floating());

    undo->undo();
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(row(w, 0, 0, 2) == QStringLiteral("A "));
    CHECK(w.selectTool()->selection().bounds() == CellRect{0, 0, 1, 1});
    press(v, Qt::Key_Return);
    CHECK(row(w, 0, 0, 2) == QStringLiteral("A "));

    // redo does the same
    press(v, Qt::Key_Right);
    REQUIRE(w.selectTool()->floating());
    undo->redo();
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(row(w, 0, 0, 2) == QStringLiteral("B "));
}

TEST_CASE("typing while a selection floats stamps it first") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    write(w, 0, 0, QStringLiteral("AB"));
    const int typed = undo->count();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    press(v, Qt::Key_Right);
    REQUIRE(w.selectTool()->floating());
    v->setCellCursor({1, 0});
    typeText(v, QStringLiteral("x"));
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(row(w, 0, 0, 3) == QStringLiteral(" xB"));
    CHECK(undo->count() == typed + 2);
    undo->undo();
    CHECK(row(w, 0, 0, 3) == QStringLiteral(" AB"));
}

TEST_CASE("Esc during a drag of the floating selection ends the drag") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    mouse(v, QEvent::MouseButtonPress, {0, 0});
    mouse(v, QEvent::MouseMove, {2, 0});
    REQUIRE(w.selectTool()->floating());
    press(v, Qt::Key_Escape);
    CHECK_FALSE(w.selectTool()->floating());
    mouse(v, QEvent::MouseMove, {3, 0});
    mouse(v, QEvent::MouseButtonRelease, {3, 0});
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(w.selectTool()->selection().bounds() == CellRect{0, 0, 1, 1});
    press(v, Qt::Key_Delete);
    CHECK(row(w, 0, 0, 4) == QStringLiteral("    "));
}

TEST_CASE("a selection outlives a switch to another tool") {
    MainWindow w;
    write(w, 0, 0, QStringLiteral("k"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({2, 2, 3, 3});
    act(w, "keyboardTool")->trigger();
    act(w, "selectTool")->trigger();
    CHECK(w.selectTool()->selection().bounds() == CellRect{2, 2, 3, 3});
}

TEST_CASE("the selection outline shows while another tool is active") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("k"));
    v->setZoom(1);
    act(w, "selectTool")->trigger();
    w.selectTool()->select({2, 2, 3, 3});
    act(w, "keyboardTool")->trigger();
    REQUIRE(v->selection().bounds() == CellRect{2, 2, 3, 3});
    const QImage shot = v->grab().toImage();
    const QRect top = v->cellRect(QPoint(2, 2));
    bool white = false;
    for (int x = top.left(); x < top.left() + 3 * top.width() && !white; ++x)
        white = shot.pixelColor(x, top.top()) == QColor(Qt::white);
    CHECK(white);
    // Deselect works without the selection tool.
    act(w, "deselect")->trigger();
    CHECK(v->selection().empty());
    CHECK(w.tools()->active()->id() == QStringLiteral("keyboard"));
}

TEST_CASE("losing focus during a drag ends the drag where it started") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    mouse(v, QEvent::MouseButtonPress, {0, 0});
    mouse(v, QEvent::MouseMove, {2, 0});
    REQUIRE(w.selectTool()->floating());
    QFocusEvent out(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(v, &out);
    // The lift stays, back where the drag picked it up; the mouse no longer
    // moves it.
    CHECK(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
    CHECK(row(w, 0, 0, 3) == QStringLiteral("A  "));
    CHECK(w.selectTool()->selection().bounds() == CellRect{0, 0, 1, 1});
    mouse(v, QEvent::MouseMove, {3, 0});
    mouse(v, QEvent::MouseButtonRelease, {3, 0});
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
    press(v, Qt::Key_Escape);
    REQUIRE_FALSE(w.selectTool()->floating());

    // Without a drag, a menu or another window leaves the float alone.
    press(v, Qt::Key_Right);
    REQUIRE(w.selectTool()->floating());
    QFocusEvent popup(QEvent::FocusOut, Qt::PopupFocusReason);
    QApplication::sendEvent(v, &popup);
    QApplication::sendEvent(v, &out);
    CHECK(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(1, 0));

    // A new rectangle just ends, as far as it got.
    press(v, Qt::Key_Return);
    mouse(v, QEvent::MouseButtonPress, {4, 4});
    mouse(v, QEvent::MouseMove, {6, 5});
    QApplication::sendEvent(v, &out);
    mouse(v, QEvent::MouseMove, {9, 9});
    mouse(v, QEvent::MouseButtonRelease, {9, 9});
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(w.selectTool()->selection().bounds() == CellRect{4, 4, 3, 2});
}

TEST_CASE("losing focus while dragging a paste keeps the paste") {
    // #35: focus loss cancelled the tool, which threw the paste away
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    act(w, "copy")->trigger();
    v->setCellCursor({5, 0});
    act(w, "paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    mouse(v, QEvent::MouseButtonPress, {5, 0});
    mouse(v, QEvent::MouseMove, {7, 0});
    REQUIRE(w.selectTool()->floatingPos() == QPoint(7, 0));
    QFocusEvent out(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(v, &out);
    CHECK(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(5, 0));
    press(v, Qt::Key_Return);
    CHECK(row(w, 0, 0, 8) == QStringLiteral("A    A  "));
}

TEST_CASE("a popup that takes the release while dragging a floating selection puts it back and keeps it") {
    // The drag ends as on focus loss; Qt's own delivery, as in test_app_brush.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    const auto sys = [&](QEvent::Type type, QPoint cell, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, v->cellRect(cell).center(),
                            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held);
    };
    sys(QEvent::MouseButtonPress, {0, 0}, Qt::LeftButton);
    sys(QEvent::MouseMove, {2, 0}, Qt::LeftButton);
    REQUIRE(w.selectTool()->floatingPos() == QPoint(2, 0));
    bwtest::popupOver(v, true);
    CHECK_FALSE(w.selectTool()->dragging());
    CHECK(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
    sys(QEvent::MouseMove, {3, 0}, Qt::NoButton);
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
}

TEST_CASE("a popup that takes the release outside it or with another button held puts a dragged float back") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    const auto sys = [&](QEvent::Type type, QPoint cell, Qt::MouseButton button, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, v->cellRect(cell).center(), button, held);
    };
    sys(QEvent::MouseButtonPress, {0, 0}, Qt::LeftButton, Qt::LeftButton);
    sys(QEvent::MouseMove, {2, 0}, Qt::NoButton, Qt::LeftButton);
    REQUIRE(w.selectTool()->floatingPos() == QPoint(2, 0));
    SECTION("released outside the popup, which a click then dismisses") {
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        sys(QEvent::MouseButtonRelease, {2, 0}, Qt::LeftButton, Qt::NoButton);
        REQUIRE(m.isVisible());
        sys(QEvent::MouseButtonPress, {6, 3}, Qt::LeftButton, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, {6, 3}, Qt::LeftButton, Qt::NoButton);
    }
    SECTION("released on the popup with the right button held") {
        sys(QEvent::MouseButtonPress, {2, 0}, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        bwtest::popupOver(v, true, Qt::LeftButton, Qt::RightButton);
        sys(QEvent::MouseButtonRelease, {2, 0}, Qt::RightButton, Qt::NoButton);
    }
    CHECK_FALSE(w.selectTool()->dragging());
    CHECK(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
    sys(QEvent::MouseMove, {3, 0}, Qt::NoButton, Qt::NoButton);
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
}

TEST_CASE("a popup that opens while a floating selection is dragged puts it back, whenever it closes") {
    // Codex round 5: once the popup has had a mouse event, the release
    // reaches the window but not the canvas.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    const auto sys = [&](QEvent::Type type, QPoint cell, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, v->cellRect(cell).center(),
                            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held);
    };
    sys(QEvent::MouseButtonPress, {0, 0}, Qt::LeftButton);
    sys(QEvent::MouseMove, {2, 0}, Qt::LeftButton);
    REQUIRE(w.selectTool()->floatingPos() == QPoint(2, 0));
    SECTION("a popup that takes a move and closes before the release") {
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        sys(QEvent::MouseMove, {3, 0}, Qt::LeftButton);
        m.close();
        QApplication::processEvents();
    }
    SECTION("a popup that closes with the button still held") {
        bwtest::popupOver(v, false);
        sys(QEvent::MouseMove, {3, 0}, Qt::LeftButton);
    }
    sys(QEvent::MouseButtonRelease, {3, 0}, Qt::NoButton);
    CHECK_FALSE(w.selectTool()->dragging());
    CHECK(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
    CHECK(row(w, 0, 0, 4) == QStringLiteral("A   "));
    sys(QEvent::MouseMove, {4, 0}, Qt::NoButton);
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
}

TEST_CASE("the device that starts a selection drag owns it until the drag ends") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    act(w, "selectTool")->trigger();
    static const QPointingDevice pen(QStringLiteral("test stylus"), 4242, QInputDevice::DeviceType::Stylus,
                                     QPointingDevice::PointerType::Pen,
                                     QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const auto tablet = [&](QEvent::Type type, QPoint cell) {
        const QPointF at = v->cellRect(cell).center();
        const bool up = type == QEvent::TabletRelease;
        QTabletEvent e(type, &pen, at, v->mapToGlobal(at), up ? 0.0 : 1.0, 0, 0, 0, 0, 0, Qt::NoModifier,
                       Qt::LeftButton, up ? Qt::NoButton : Qt::LeftButton);
        QApplication::sendEvent(v, &e);
    };
    mouse(v, QEvent::MouseButtonPress, {2, 2});
    mouse(v, QEvent::MouseMove, {4, 3});
    tablet(QEvent::TabletPress, {10, 10});
    tablet(QEvent::TabletMove, {12, 12});
    tablet(QEvent::TabletRelease, {12, 12});
    mouse(v, QEvent::MouseButtonRelease, {4, 3});
    CHECK(w.selectTool()->selection().bounds() == CellRect{2, 2, 3, 2});
    // the drag over, the stylus starts the next one
    tablet(QEvent::TabletPress, {10, 10});
    tablet(QEvent::TabletMove, {12, 12});
    tablet(QEvent::TabletRelease, {12, 12});
    CHECK(w.selectTool()->selection().bounds() == CellRect{10, 10, 3, 3});
}

TEST_CASE("the undo key during a selection drag ends the drag and undoes nothing") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    QUndoStack* undo = w.document()->undoStack();
    const int typed = undo->count();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    mouse(v, QEvent::MouseButtonPress, {0, 0});
    mouse(v, QEvent::MouseMove, {2, 0});
    REQUIRE(w.selectTool()->floatingPos() == QPoint(2, 0));
    QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(v, &override);
    CHECK(override.isAccepted());
    QKeyEvent z(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(v, &z);
    CHECK(w.selectTool()->floating());
    CHECK(w.selectTool()->floatingPos() == QPoint(0, 0));
    CHECK(undo->count() == typed);
    CHECK(undo->index() == typed);
}

TEST_CASE("an action that ends a selection drag lets the other device's clicks through again (#45)") {
    // A mouse drag keeps the pen's clicks from every widget until it ends,
    // however it ends, the menus' actions included: they work on the tool
    // directly, not through the view's handlers. A drag they leave going
    // keeps them. The view takes no mouse grab for it (#101).
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("AB"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    act(w, "copy")->trigger();
    const auto sys = [&](QEvent::Type type, QPoint cell) {
        bwtest::systemMouse(v, type, v->cellRect(cell).center(),
                            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton);
    };
    // Whether a pen's click on the status bar reaches it.
    const auto penReachesBar = [&] {
        return bwtest::clickReaches(w.statusBar(), QPointF(5, 5), bwtest::systemStylus());
    };
    const auto filterFollowsDrag = [&] {
        CHECK(QWidget::mouseGrabber() == nullptr);
        CHECK(penReachesBar() == !w.selectTool()->dragging());
    };
    // the floating selection, dragged with the mouse
    sys(QEvent::MouseButtonPress, {0, 0});
    sys(QEvent::MouseMove, {2, 1});
    REQUIRE(w.selectTool()->floating());
    REQUIRE(w.selectTool()->dragging());
    REQUIRE_FALSE(penReachesBar());

    SECTION("Paste") {
        act(w, "paste")->trigger();
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
        // and focus lost afterwards finds no drag to keep either
        v->clearFocus();
        filterFollowsDrag();
    }
    SECTION("Cut") {
        act(w, "cut")->trigger();
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
    }
    SECTION("Delete") {
        act(w, "deleteSelection")->trigger();
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
    }
    SECTION("Select All") {
        act(w, "selectAll")->trigger();
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
    }
    SECTION("Deselect") {
        act(w, "deselect")->trigger();
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
    }
    SECTION("Undo and Redo from the menu") {
        act(w, "undo")->trigger();
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
        act(w, "redo")->trigger();
        filterFollowsDrag();
    }
    SECTION("a Library stamp placed") {
        QString error;
        REQUIRE(w.placeStamp(QString::fromStdString((bwtest::dataDir() / "library" / "sky" / "small-star.ans").string()),
                             {5, 5}, &error));
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
    }
    SECTION("a new canvas") {
        w.document()->clear();
        CHECK_FALSE(w.selectTool()->dragging());
        filterFollowsDrag();
    }
    SECTION("a flip, a rotation or a paste mode leaves the drag going, and the pen's clicks kept") {
        act(w, "flipHorizontal")->trigger();
        act(w, "rotateClockwise")->trigger();
        act(w, "pasteUnder")->trigger();
        CHECK(w.selectTool()->dragging());
        filterFollowsDrag();
    }
    SECTION("a modal dialog") {
        // The dialog takes focus from the canvas, which ends the drag: the
        // drag's hold on the pen's clicks doesn't outlast it into the dialog.
        QMessageBox box(QMessageBox::Information, QStringLiteral("t"), QStringLiteral("t"), QMessageBox::Ok, &w);
        bool reached = false;
        bool dragging = true;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &box, [&] {
            if (QApplication::activeModalWidget() != &box || QApplication::focusWidget() == v)
                return;
            reached = bwtest::clickReaches(&box, QPointF(5, 5), bwtest::systemStylus());
            dragging = w.selectTool()->dragging();
            box.done(0);
        });
        poll.start(5);
        QTimer::singleShot(3000, &box, [&] { box.done(1); });
        CHECK(box.exec() == 0);
        CHECK_FALSE(dragging);
        CHECK(reached);
    }
    filterFollowsDrag();
    sys(QEvent::MouseButtonRelease, {2, 1});
    CHECK_FALSE(w.selectTool()->dragging());
    CHECK(QWidget::mouseGrabber() == nullptr);
}

TEST_CASE("saving writes a floating selection where it floats") {
    // #35: Save wrote the canvas without the floating cells
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("AB"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    act(w, "copy")->trigger();
    v->setCellCursor({5, 1});
    act(w, "paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("pasted.ans"));
    QString error;
    REQUIRE(w.saveFile(path, &error));
    CHECK_FALSE(w.selectTool()->floating());
    CHECK_FALSE(w.document()->isModified());

    MainWindow reopened;
    REQUIRE(reopened.openFile(path, &error));
    CHECK(row(reopened, 5, 1, 2) == QStringLiteral("AB"));

    // A moved selection is saved moved.
    w.selectTool()->select({0, 0, 2, 1});
    press(v, Qt::Key_Down);
    press(v, Qt::Key_Down);
    REQUIRE(w.selectTool()->floating());
    REQUIRE(w.saveFile(path, &error));
    MainWindow moved;
    REQUIRE(moved.openFile(path, &error));
    CHECK(row(moved, 0, 0, 2) == QStringLiteral("  "));
    CHECK(row(moved, 0, 2, 2) == QStringLiteral("AB"));
}

TEST_CASE("a paste on a clean canvas is an unsaved change") {
    // #35: New dropped the paste without asking
    MainWindow w;
    CanvasView* v = w.canvasView();
    write(w, 0, 0, QStringLiteral("A"));
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    act(w, "copy")->trigger();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QString error;
    REQUIRE(w.saveFile(dir.filePath(QStringLiteral("clean.ans")), &error));
    v->setCellCursor({5, 0});
    act(w, "paste")->trigger();
    REQUIRE(w.selectTool()->floating());
    REQUIRE_FALSE(w.document()->isModified());

    // New asks first; Cancel keeps the canvas, with the paste stamped.
    QString asked;
    QTimer answer;
    answer.setInterval(10);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            answer.stop();
            asked = box->text();
            box->button(QMessageBox::Cancel)->click();
        }
    });
    answer.start();
    w.newFile();
    answer.stop();
    CHECK(asked == QStringLiteral("The canvas has unsaved changes."));
    CHECK(w.document()->isModified());
    CHECK_FALSE(w.selectTool()->floating());
    CHECK(row(w, 0, 0, 6) == QStringLiteral("A    A"));
}

TEST_CASE("the reference image is scaled only where it covers the canvas") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QImage tall(100, 10000, QImage::Format_RGB32);
    tall.fill(Qt::red);
    v->setReferenceImage(tall);
    v->setReferenceVisible(true);
    v->setReferenceOpacity(1.0);

    // 80x25 at 8x: the canvas size at this zoom, not the image's 5120x512000.
    w.document()->replace(AnsiDocument{Canvas(80, 25)}, QString());
    v->setZoom(8);
    const QRect cell = v->cellRect(QPoint(79, 24));
    CHECK(v->grab(cell).toImage().pixelColor(4, 4).red() > 240);
    CHECK(v->referenceCacheSize().width() <= v->width());
    CHECK(v->referenceCacheSize().height() <= v->height());

    // 80x500 at 8x would be 5120x64000: past the cap, so cached smaller and
    // drawn scaled up.
    AnsiDocument big;
    big.canvas = Canvas(80, 500);
    w.document()->replace(std::move(big), QString());
    const QImage corner = v->grab(v->cellRect(QPoint(79, 499))).toImage();
    CHECK(corner.pixelColor(4, 4).red() > 240);
    const QSize cache = v->referenceCacheSize();
    CHECK(qint64(cache.width()) * cache.height() <= CanvasView::kMaxReferencePixels);
    CHECK(cache.width() <= v->width());
    CHECK(cache.height() <= v->height());
}

TEST_CASE("the reference image keeps its aspect where it is cropped to the canvas") {
    // #35: the crop was rounded up to whole source rows and stretched to the
    // canvas height, so a 2x10 picture on 640x400 px put the edge of its
    // first row at y=200, not 320
    MainWindow w;
    w.document()->replace(AnsiDocument{Canvas(80, 25)}, QString());
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    REQUIRE(v->size() == QSize(640, 400));
    QImage stripes(2, 10, QImage::Format_RGB32);
    stripes.fill(Qt::blue);
    stripes.setPixelColor(0, 0, Qt::red);
    stripes.setPixelColor(1, 0, Qt::red);
    v->setReferenceImage(stripes);
    v->setReferenceOpacity(1.0);
    const QImage full = stripes.scaled(640, 3200, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto near = [](QColor a, QColor b) {
        return std::abs(a.red() - b.red()) <= 8 && std::abs(a.green() - b.green()) <= 8 &&
               std::abs(a.blue() - b.blue()) <= 8;
    };
    const QImage shot = v->grab().toImage();
    for (const QPoint at : {QPoint(100, 100), QPoint(100, 250), QPoint(100, 330), QPoint(500, 390)}) {
        INFO("at " << at.x() << "," << at.y() << ": " << shot.pixelColor(at).name().toStdString() << " vs "
                   << full.pixelColor(at).name().toStdString());
        CHECK(near(shot.pixelColor(at), full.pixelColor(at)));
    }
    // A repaint of part of the view (as when scrolled) draws the same.
    const QImage part = v->grab(QRect(64, 240, 128, 120)).toImage();
    for (const QPoint at : {QPoint(100, 250), QPoint(100, 330)})
        CHECK(near(part.pixelColor(at - QPoint(64, 240)), full.pixelColor(at)));

    // Shorter than the canvas: it covers only its own height.
    QImage wide(10, 4, QImage::Format_RGB32);
    wide.fill(Qt::blue);
    for (int x = 0; x < 10; ++x)
        wide.setPixelColor(x, 0, Qt::red);
    v->setReferenceImage(wide);
    const QImage fullWide = wide.scaled(640, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage shotWide = v->grab().toImage();
    for (const QPoint at : {QPoint(100, 20), QPoint(100, 70), QPoint(100, 200)})
        CHECK(near(shotWide.pixelColor(at), fullWide.pixelColor(at)));
    CHECK(shotWide.pixelColor(100, 300) == QColor(Qt::black));
}

namespace {

// Whether a and b are within `tol` in each channel.
bool within(QColor a, QColor b, int tol) {
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
           std::abs(a.blue() - b.blue()) <= tol;
}

// A picture of repeating rows: one red, then two blue.
QImage redBlueBlue(int width, int height) {
    QImage img(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        const QRgb c = y % 3 == 0 ? qRgb(255, 0, 0) : qRgb(0, 0, 255);
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        std::fill(line, line + width, c);
    }
    return img;
}

}  // namespace

TEST_CASE("the reference image is interpolated across the edge of its crop") {
    // #35: the crop kept only the source rows under the canvas, and the
    // interpolation at its bottom edge stopped at the last of them, so on
    // 640x400 px a 3x10 picture stayed blue at y=399 where the whole picture
    // blends into its green third row.
    MainWindow w;
    w.document()->replace(AnsiDocument{Canvas(80, 25)}, QString());
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    REQUIRE(v->size() == QSize(640, 400));
    QImage pic(3, 10, QImage::Format_RGB32);
    pic.fill(Qt::green);
    for (int x = 0; x < 3; ++x) {
        pic.setPixelColor(x, 0, Qt::red);
        pic.setPixelColor(x, 1, Qt::blue);
    }
    v->setReferenceImage(pic);
    v->setReferenceOpacity(1.0);

    // The whole picture at 640/3 both ways.
    QImage full(640, 400, QImage::Format_RGB32);
    full.fill(Qt::black);
    {
        QPainter p(&full);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(0, 0, 640, 10 * 640 / 3.0), pic);
    }
    CHECK(within(full.pixelColor(100, 399), QColor(0, 95, 160), 3));
    const QImage shot = v->grab().toImage();
    for (const QPoint at : {QPoint(100, 399), QPoint(100, 100), QPoint(320, 250), QPoint(320, 330), QPoint(600, 380)}) {
        INFO("at " << at.x() << "," << at.y() << ": " << shot.pixelColor(at).name().toStdString() << " vs "
                   << full.pixelColor(at).name().toStdString());
        CHECK(within(shot.pixelColor(at), full.pixelColor(at), 3));
    }
}

TEST_CASE("a reference image wider than the canvas is averaged, not sampled") {
    // #35: shrinking interpolated between the two source pixels nearest each
    // view pixel and skipped the rest, so fine detail turned into moire.
    MainWindow w;
    w.document()->replace(AnsiDocument{Canvas(80, 25)}, QString());
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    REQUIRE(v->size() == QSize(640, 400));
    v->setReferenceOpacity(1.0);

    // 1 px black and white columns, 4000 px wide: an even grey at 640.
    QImage stripes(4000, 2500, QImage::Format_RGB32);
    for (int y = 0; y < stripes.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(stripes.scanLine(y));
        for (int x = 0; x < stripes.width(); ++x)
            line[x] = (x & 1) ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
    }
    v->setReferenceImage(stripes);
    const QImage grey = v->grab().toImage();
    int lo = 255, hi = 0;
    for (int x = 0; x < 640; ++x) {
        const int g = qGray(grey.pixel(x, 200));
        lo = std::min(lo, g);
        hi = std::max(hi, g);
    }
    INFO("grey from " << lo << " to " << hi);
    CHECK(hi - lo <= 24);
    CHECK(std::abs((lo + hi) / 2 - 128) <= 12);

    // One red row then two blue, 1920x1080 at 640x360: each view row is one
    // red and two blue rows averaged.
    v->setReferenceImage(redBlueBlue(1920, 1080));
    const QImage mixed = v->grab().toImage();
    for (const QPoint at : {QPoint(16, 20), QPoint(100, 100), QPoint(320, 201), QPoint(639, 359), QPoint(500, 290)}) {
        INFO("at " << at.x() << "," << at.y() << ": " << mixed.pixelColor(at).name().toStdString());
        CHECK(within(mixed.pixelColor(at), QColor(85, 0, 170), 3));
    }
    CHECK(mixed.pixelColor(100, 380) == QColor(Qt::black));
}

TEST_CASE("a capped reference image draws the same whatever part is repainted") {
    // #35: past the cap, a repaint scaled up only the cache pixels under the
    // repainted rect, so the interpolation at its edge depended on the rect
    // and a scrolled repaint showed a seam.
    MainWindow w;
    w.document()->replace(AnsiDocument{Canvas(80, 500)}, QString());
    CanvasView* v = w.canvasView();
    v->setZoom(8);
    REQUIRE(v->size() == QSize(5120, 64000));
    v->setReferenceImage(redBlueBlue(256, 4000));
    v->setReferenceOpacity(1.0);

    const QImage tall = v->grab(QRect(64, 0, 128, 400)).toImage();
    REQUIRE(v->referenceCacheSize() != v->size());
    REQUIRE(qint64(v->referenceCacheSize().width()) * v->referenceCacheSize().height() <=
            CanvasView::kMaxReferencePixels);
    const QImage part = v->grab(QRect(64, 239, 128, 120)).toImage();
    REQUIRE(part.size() == QSize(128, 120));
    int worst = 0;
    for (int y = 0; y < part.height(); ++y)
        for (int x = 0; x < part.width(); ++x) {
            const QColor a = part.pixelColor(x, y), b = tall.pixelColor(x, 239 + y);
            worst = std::max({worst, std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                              std::abs(a.blue() - b.blue())});
        }
    CHECK(worst <= 1);
}

TEST_CASE("a capped reference image blends into the rows below the canvas") {
    // #35: the cache past the cap ended at the canvas's bottom edge, so the
    // interpolation in the last view rows read its last row over again where
    // the whole picture goes on: (156,0,98) at y=63999 where it is (134,0,120).
    MainWindow w;
    w.document()->replace(AnsiDocument{Canvas(80, 500)}, QString());
    CanvasView* v = w.canvasView();
    v->setZoom(8);
    REQUIRE(v->size() == QSize(5120, 64000));
    QImage pic(256, 4000, QImage::Format_RGB32);
    for (int y = 0; y < pic.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(pic.scanLine(y));
        std::fill(line, line + pic.width(), y % 2 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
    }
    v->setReferenceImage(pic);
    v->setReferenceOpacity(1.0);

    // The whole picture at 20x, over the last 8 rows of x 64..191.
    const QRect bottom(64, 63992, 128, 8);
    QImage full(bottom.size(), QImage::Format_RGB32);
    full.fill(Qt::black);
    {
        QPainter p(&full);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(-bottom.topLeft());
        p.drawImage(QRectF(0, 0, 5120, 80000), pic);
    }
    CHECK(within(full.pixelColor(QPoint(100, 63999) - bottom.topLeft()), QColor(134, 0, 120), 3));
    const QImage shot = v->grab(bottom).toImage();
    REQUIRE(v->referenceCacheSize() != v->size());
    REQUIRE(qint64(v->referenceCacheSize().width()) * v->referenceCacheSize().height() <=
            CanvasView::kMaxReferencePixels);
    // Rows 63993 on, where a stripe fades: the cache's own resolution
    // flattens each stripe's peak by up to 20 anywhere in the picture.
    for (const QPoint at : {QPoint(100, 63999), QPoint(64, 63999), QPoint(191, 63998), QPoint(150, 63996),
                            QPoint(80, 63994), QPoint(120, 63993)}) {
        const QPoint in = at - bottom.topLeft();
        INFO("at " << at.x() << "," << at.y() << ": " << shot.pixelColor(in).name().toStdString() << " vs "
                   << full.pixelColor(in).name().toStdString());
        CHECK(within(shot.pixelColor(in), full.pixelColor(in), 3));
    }
}

TEST_CASE("a capped reference image ending just below the canvas stays opaque to its bottom row") {
    // #35: the cache past the cap took two rows more wherever the picture
    // went on below the canvas, however little, so a row the picture only
    // partly covers came out partly transparent and the last view rows
    // faded where the whole picture is opaque: (244,244,244) at y=8191 on
    // 512x512 cells for a white 4096x8193 picture.
    struct Case {
        int cols, rows, zoom;
        QSize pic;
        int y;  // the last view row
    };
    for (const Case& c : {Case{512, 512, 1, QSize(4096, 8193), 8191},     // reduced, 1 row past
                          Case{512, 2048, 1, QSize(2048, 16385), 32767},  // reduced, 2 rows past
                          Case{80, 504, 8, QSize(768, 9677), 64511}}) {   // enlarged, 1.33 rows past
        INFO(c.cols << "x" << c.rows << " at zoom " << c.zoom << ", picture " << c.pic.width() << "x"
                    << c.pic.height());
        MainWindow w;
        w.document()->replace(AnsiDocument{Canvas(c.cols, c.rows)}, QString());
        CanvasView* v = w.canvasView();
        v->setZoom(c.zoom);
        REQUIRE(v->size().height() == c.y + 1);
        QImage pic(c.pic, QImage::Format_Grayscale8);
        pic.fill(255);
        v->setReferenceImage(pic);
        v->setReferenceOpacity(1.0);
        const QRect bottom(64, c.y - 7, 128, 8);
        const QImage shot = v->grab(bottom).toImage();
        REQUIRE(v->referenceCacheSize() != v->size());
        REQUIRE(qint64(v->referenceCacheSize().width()) * v->referenceCacheSize().height() <=
                CanvasView::kMaxReferencePixels);
        for (const QPoint at : {QPoint(100, c.y), QPoint(64, c.y), QPoint(191, c.y - 1), QPoint(150, c.y - 2)}) {
            const QPoint in = at - bottom.topLeft();
            INFO("at " << at.x() << "," << at.y() << ": " << shot.pixelColor(in).name().toStdString());
            CHECK(within(shot.pixelColor(in), QColor(255, 255, 255), 1));
        }
    }
}

TEST_CASE("a reduced reference image weighs the rows it covers in part") {
    MainWindow w;
    w.document()->replace(AnsiDocument{Canvas(80, 25)}, QString());
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    REQUIRE(v->size() == QSize(640, 400));
    v->setReferenceOpacity(1.0);

    // 1000x1000 at 640: 1.5625 source rows to a view row, so most view rows
    // take part of a red or blue row. Row 0 is red 1 and blue 0.5625.
    v->setReferenceImage(redBlueBlue(1000, 1000));
    const QImage shot = v->grab().toImage();
    const auto expected = [](int j) {
        const double a = j / 0.64, b = (j + 1) / 0.64;
        double red = 0;
        for (int y = int(a); y < b; ++y)
            if (y % 3 == 0)
                red += std::min<double>(b, y + 1) - std::max<double>(a, y);
        const double share = red / (b - a);
        return QColor(int(std::lround(255 * share)), 0, int(std::lround(255 * (1 - share))));
    };
    CHECK(within(expected(0), QColor(163, 0, 92), 1));
    for (const int j : {0, 1, 2, 3, 7, 150, 251, 399}) {
        INFO("row " << j << ": " << shot.pixelColor(320, j).name().toStdString() << " vs "
                    << expected(j).name().toStdString());
        CHECK(within(shot.pixelColor(320, j), expected(j), 2));
    }

    // Half transparent, on the black canvas: as dark at its edges as in its
    // middle, so its own alpha is averaged and not the canvas past it.
    QImage half(1000, 777, QImage::Format_ARGB32);
    half.fill(QColor(100, 50, 0, 128));
    v->setReferenceImage(half);
    const QImage over = v->grab().toImage();
    for (const QPoint at : {QPoint(8, 0), QPoint(639, 0), QPoint(320, 200), QPoint(639, 200), QPoint(320, 399),
                            QPoint(639, 399), QPoint(0, 399)}) {
        INFO("at " << at.x() << "," << at.y() << ": " << over.pixelColor(at).name().toStdString());
        CHECK(within(over.pixelColor(at), QColor(50, 25, 0), 2));
    }

    // Opaque red rows between transparent blue ones, at half size: each view
    // pixel is one of each, red at half alpha, (128,0,0) on black. Averaging
    // the colours as they are, not by their alpha, lets the blue in:
    // (64,0,64).
    QImage rows(1280, 799, QImage::Format_ARGB32);
    for (int y = 0; y < rows.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(rows.scanLine(y));
        std::fill(line, line + rows.width(), y % 2 ? qRgba(0, 0, 255, 0) : qRgba(255, 0, 0, 255));
    }
    v->setReferenceImage(rows);
    const QImage mixed = v->grab().toImage();
    for (const QPoint at : {QPoint(8, 0), QPoint(639, 0), QPoint(320, 200), QPoint(639, 200), QPoint(320, 398),
                            QPoint(0, 398)}) {
        INFO("at " << at.x() << "," << at.y() << ": " << mixed.pixelColor(at).name().toStdString());
        CHECK(within(mixed.pixelColor(at), QColor(128, 0, 0), 2));
    }
}

TEST_CASE("a reference image is converted in bounded pieces, however wide") {
    // #35: a chunk was whole rows, at least one, so a 16,777,217x1 picture
    // was converted in one piece of 16.8M px.
    for (const QSize size : {QSize(16777217, 1), QSize(5000000, 3), QSize(1048576, 3), QSize(1048577, 2),
                             QSize(4000, 2500), QSize(1, 3)}) {
        INFO(size.width() << "x" << size.height());
        const std::vector<QRect> chunks = referenceChunks(size.width(), size.height());
        qint64 area = 0;
        for (std::size_t k = 0; k < chunks.size(); ++k) {
            const QRect& c = chunks[k];
            CHECK(qint64(c.width()) * c.height() <= kMaxConvertPixels);
            CHECK(QRect(QPoint(0, 0), size).contains(c));
            for (std::size_t m = 0; m < k; ++m)
                CHECK_FALSE(chunks[m].intersects(c));
            area += qint64(c.width()) * c.height();
        }
        CHECK(area == qint64(size.width()) * size.height());
    }
    // Only the rows asked for.
    for (const QRect& c : referenceChunks(4000, 3))
        CHECK(c.bottom() < 3);
}

TEST_CASE("a reference image wider than a conversion is averaged as if whole") {
    // 2,100,000x2 onto 64x1: each view pixel is 32812.5 source columns of
    // both rows, and the pieces split some of them.
    QImage src(2100000, 2, QImage::Format_RGB32);
    const auto colour = [](int x, int y) {
        return qRgb((x / 70000) * 8 % 256, y ? 200 : 0, (x % 3) * 100);
    };
    for (int y = 0; y < src.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(src.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
            line[x] = colour(x, y);
    }
    QImage dst(64, 1, QImage::Format_ARGB32_Premultiplied);
    dst.fill(Qt::transparent);
    const double kx = 64.0 / src.width();
    REQUIRE(areaScale(src, dst, kx, 0.5));
    for (int i = 0; i < dst.width(); ++i) {
        const double a = i / kx, b = std::min<double>((i + 1) / kx, src.width());
        double r = 0, g = 0, bl = 0;
        for (int y = 0; y < 2; ++y)
            for (int x = int(a); x < b; ++x) {
                const double wgt = std::min<double>(b, x + 1) - std::max<double>(a, x);
                const QRgb c = colour(x, y);
                r += wgt * qRed(c);
                g += wgt * qGreen(c);
                bl += wgt * qBlue(c);
            }
        const double n = 2 * (b - a);
        const QColor want(int(std::lround(r / n)), int(std::lround(g / n)), int(std::lround(bl / n)));
        const QColor got = dst.pixelColor(i, 0);
        INFO("column " << i << ": " << got.name().toStdString() << " vs " << want.name().toStdString());
        CHECK(got.alpha() == 255);
        CHECK(within(got, want, 1));
    }
}

TEST_CASE("a reference row wider than a conversion keeps its level") {
    // #35: each view pixel sums 2M source pixels. In float the sum drifted
    // a level either way: white ran a little over 255 times their weight and
    // rounded to 256, which as a channel would wrap to 0 if it weren't
    // clamped, and 100 and 37 came out 99 and 36 (#37).
    for (const int level : {255, 100, 37}) {
        INFO("level " << level);
        QImage src(16777217, 1, QImage::Format_Grayscale8);
        src.fill(level);
        QImage dst(8, 1, QImage::Format_ARGB32_Premultiplied);
        dst.fill(Qt::transparent);
        REQUIRE(areaScale(src, dst, 8.0 / src.width(), 1));
        for (int i = 0; i < dst.width(); ++i) {
            const QColor got = dst.pixelColor(i, 0);
            INFO("column " << i << ": " << got.name().toStdString() << " alpha " << got.alpha());
            CHECK(got.alpha() == 255);
            CHECK(got.red() == level);
            CHECK(got.green() == level);
            CHECK(got.blue() == level);
        }
    }
}
