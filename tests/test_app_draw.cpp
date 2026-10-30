// The drawing tools in the main window (PLAN §4, phase 2.1): the half-block,
// shade, glyph and colorize brushes, line, rectangle, ellipse, fill and the
// eyedropper, driven through the Tools menu and the mouse.

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "tool.h"

#include "blockwright/glyph_sets.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/shapes.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QRegion>
#include <QUndoStack>

#include <algorithm>
#include <climits>
#include <functional>

// QtGui's shortcut dispatch, as QTest's key clicks use it: the focus
// widget sees ShortcutOverride, then the window's shortcuts get the key.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods,
                                               const QString& text = QString(), bool autorep = false,
                                               ushort count = 1);

using namespace bw;

namespace {

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

void press(QWidget* w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent down(QEvent::KeyPress, k, mods);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, k, mods);
    QApplication::sendEvent(w, &up);
}

// The view pixel in the middle of half-block pixel (x, py).
QPointF pixel(CanvasView* v, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    return QPointF(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
}

void mouse(CanvasView* v, QEvent::Type type, int x, int py, Qt::MouseButton button = Qt::LeftButton) {
    const QPointF at = pixel(v, x, py);
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

// A drag through half-block pixels: press at the first, move through the
// rest, release at the last.
void drag(CanvasView* v, std::initializer_list<QPoint> points, Qt::MouseButton button = Qt::LeftButton) {
    const QPoint* p = points.begin();
    mouse(v, QEvent::MouseButtonPress, p->x(), p->y(), button);
    for (++p; p != points.end(); ++p)
        mouse(v, QEvent::MouseMove, p->x(), p->y(), button);
    const QPoint last = *(points.end() - 1);
    mouse(v, QEvent::MouseButtonRelease, last.x(), last.y(), button);
}

void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

const Cell& at(MainWindow& w, int x, int y) { return w.document()->canvas().at(x, y); }

DragTool* dragTool(MainWindow& w) {
    auto* t = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(t);
    return t;
}

// The window shown and active with the canvas focused, so its shortcuts
// fire as they would for a user.
void showActive(MainWindow& w) {
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    w.canvasView()->setFocus();
    REQUIRE(QApplication::activeWindow() == &w);
}

// A key as the application delivers it: a shortcut takes it if one
// matches and the focus widget doesn't claim it, else the widget gets it.
void key(QWidget* w, int k, Qt::KeyboardModifiers mods) {
    if (!qt_sendShortcutOverrideEvent(w, 0, k, mods))
        press(w, k, mods);
}

// The cells that differ from a blank canvas, as an edit's changes would
// list them.
int changedCells(MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    int n = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            n += !(c.at(x, y) == Cell{});
    return n;
}

// Releases the drag at `x, py` and checks the canvas holds exactly the
// edit the preview showed (on a blank canvas).
void releaseMatchesPreview(MainWindow& w, int x, int py) {
    const CellEdit shown = dragTool(w)->pending();
    REQUIRE_FALSE(shown.changes.empty());
    mouse(w.canvasView(), QEvent::MouseButtonRelease, x, py);
    for (const CellChange& c : shown.changes)
        CHECK(at(w, c.x, c.y) == c.after);
    CHECK(changedCells(w) == int(shown.changes.size()));
}

} // namespace

TEST_CASE("every drawing tool is in the Tools menu with its shortcut") {
    MainWindow w;
    struct Entry {
        const char* id;
        QKeySequence key;
    };
    const Entry entries[] = {
        {"move", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J)},
        {"halfBlock", QKeySequence(Qt::CTRL | Qt::Key_B)},
        {"shade", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)},
        {"rampShade", QKeySequence(Qt::CTRL | Qt::Key_T)},
        {"glyph", QKeySequence(Qt::CTRL | Qt::Key_G)},
        {"colorize", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
        {"line", QKeySequence(Qt::CTRL | Qt::Key_L)},
        {"rectangle", QKeySequence(Qt::CTRL | Qt::Key_U)},
        {"ellipse", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)},
        {"fill", QKeySequence(Qt::CTRL | Qt::Key_J)},
        {"eyedropper", QKeySequence(Qt::CTRL | Qt::Key_I)},
        {"outline", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)},
        {"rimLight", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L)},
        {"texture", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)},
        {"clone", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K)},
        {"spray", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)},
    };
    for (const Entry& e : entries) {
        QAction* a = act(w, QString::fromLatin1(e.id) + QStringLiteral("Tool"));
        CHECK(a->shortcut() == e.key);
        a->trigger();
        CHECK(w.tools()->active()->id() == QString::fromLatin1(e.id));
        CHECK(w.canvasView()->tool() == w.tools()->active());
        CHECK(a->isChecked());
    }
    CHECK(act(w, QStringLiteral("filledShapes"))->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
}

TEST_CASE("half-block brush: a drag lands on release as one undo step") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    use(w, "halfBlock");
    v->setForeground(Red);
    v->setBackground(Blue);

    mouse(v, QEvent::MouseButtonPress, 2, 0);
    mouse(v, QEvent::MouseMove, 5, 0);
    // nothing written until release
    CHECK(undo->count() == 0);
    CHECK(at(w, 3, 0) == Cell{});
    mouse(v, QEvent::MouseButtonRelease, 5, 0);
    REQUIRE(undo->count() == 1);
    for (int x = 2; x <= 5; ++x)
        CHECK(at(w, x, 0) == cell(kUpperHalf, Red, Black));
    CHECK(at(w, 1, 0) == Cell{});
    CHECK(at(w, 6, 0) == Cell{});

    // the bottom halves: the cells fill up
    drag(v, {{2, 1}, {5, 1}});
    CHECK(undo->count() == 2);
    CHECK(at(w, 3, 0) == cell(U' ', Grey, Red));

    // the right button draws the background colour
    drag(v, {{0, 4}, {0, 5}}, Qt::RightButton);
    CHECK(at(w, 0, 2) == cell(U' ', Grey, Blue));

    // a diagonal is joined 4-connected
    drag(v, {{10, 10}, {13, 13}});
    for (const auto& [x, py] : line4(10, 10, 13, 13))
        CHECK(pickColour(w.document()->canvas(), x, py, false) == Color::indexed(Red));

    undo->undo();
    undo->undo();
    undo->undo();
    CHECK(at(w, 3, 0) == cell(kUpperHalf, Red, Black));
}

TEST_CASE("drawing tools paint only in the selection, which survives tool switches") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("selectTool"))->trigger();
    v->setSelection(Selection(CellRect{2, 0, 3, 1}));
    use(w, "halfBlock");
    CHECK(v->selection().bounds() == CellRect{2, 0, 3, 1});
    v->setForeground(White);
    drag(v, {{0, 0}, {9, 0}});
    for (int x = 0; x < 10; ++x)
        CHECK((at(w, x, 0) == Cell{}) == (x < 2 || x > 4));

    use(w, "fill");
    drag(v, {{3, 1}});
    // the fill stops at the selection: only its bottom halves were black
    CHECK(at(w, 3, 0) == cell(kFullBlock, White, Black));
    CHECK(at(w, 5, 0) == Cell{});
    CHECK(at(w, 3, 1) == Cell{});
}

TEST_CASE("mirror mode repeats what the drawing tools paint") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    const int last = w.document()->canvas().width() - 1;
    act(w, QStringLiteral("mirrorMode"))->trigger();
    REQUIRE(v->mirror());
    use(w, "halfBlock");
    v->setForeground(Yellow);
    drag(v, {{1, 0}, {1, 0}});
    CHECK(at(w, 1, 0) == cell(kUpperHalf, Yellow, Black));
    CHECK(at(w, last - 1, 0) == cell(kUpperHalf, Yellow, Black));
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("Esc, the undo key, undo and a tool switch end a drag") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    use(w, "halfBlock");
    v->setForeground(Red);

    // Esc drops the drag; the release then does nothing
    mouse(v, QEvent::MouseButtonPress, 0, 0);
    mouse(v, QEvent::MouseMove, 4, 0);
    press(v, Qt::Key_Escape);
    mouse(v, QEvent::MouseButtonRelease, 4, 0);
    CHECK(undo->count() == 0);
    CHECK(at(w, 2, 0) == Cell{});

    // switching tools lands it
    mouse(v, QEvent::MouseButtonPress, 0, 0);
    mouse(v, QEvent::MouseMove, 4, 0);
    use(w, "line");
    CHECK(undo->count() == 1);
    CHECK(at(w, 2, 0) == cell(kUpperHalf, Red, Black));
    mouse(v, QEvent::MouseButtonRelease, 4, 0);
    CHECK(undo->count() == 1);

    // the undo key drops the drag and undoes nothing (C4)
    mouse(v, QEvent::MouseButtonPress, 0, 4);
    mouse(v, QEvent::MouseMove, 4, 4);
    press(v, Qt::Key_Z, Qt::ControlModifier);
    CHECK_FALSE(dragTool(w)->dragging());
    mouse(v, QEvent::MouseButtonRelease, 4, 4);
    CHECK(undo->count() == 1);
    CHECK(undo->index() == 1);
    CHECK(at(w, 2, 0) == cell(kUpperHalf, Red, Black));
    CHECK(at(w, 2, 2) == Cell{});

    // Edit > Undo mid-drag undoes the step before, and drops the drag
    mouse(v, QEvent::MouseButtonPress, 0, 4);
    mouse(v, QEvent::MouseMove, 4, 4);
    act(w, QStringLiteral("undo"))->trigger();
    CHECK_FALSE(dragTool(w)->dragging());
    mouse(v, QEvent::MouseButtonRelease, 4, 4);
    CHECK(undo->index() == 0);
    CHECK(at(w, 2, 0) == Cell{});
    CHECK(at(w, 2, 2) == Cell{});
    // and so does redo
    mouse(v, QEvent::MouseButtonPress, 0, 4);
    mouse(v, QEvent::MouseMove, 4, 4);
    act(w, QStringLiteral("redo"))->trigger();
    CHECK_FALSE(dragTool(w)->dragging());
    mouse(v, QEvent::MouseButtonRelease, 4, 4);
    CHECK(undo->index() == 1);
    CHECK(at(w, 2, 2) == Cell{});
}

TEST_CASE("the undo key mid-drag cancels the drag through the real shortcut path, even with nothing to undo") {
    // C4: with an empty stack the Undo action is disabled and its shortcut
    // doesn't fire; with a step to undo it must not fire either
    MainWindow w;
    showActive(w);
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    use(w, "halfBlock");
    v->setForeground(Red);
    REQUIRE_FALSE(act(w, QStringLiteral("undo"))->isEnabled());

    mouse(v, QEvent::MouseButtonPress, 0, 0);
    mouse(v, QEvent::MouseMove, 4, 0);
    key(v, Qt::Key_Z, Qt::ControlModifier);
    CHECK_FALSE(dragTool(w)->dragging());
    mouse(v, QEvent::MouseButtonRelease, 4, 0);
    CHECK(undo->count() == 0);
    CHECK(at(w, 2, 0) == Cell{});

    drag(v, {{0, 0}, {4, 0}});
    REQUIRE(undo->count() == 1);
    REQUIRE(act(w, QStringLiteral("undo"))->isEnabled());
    mouse(v, QEvent::MouseButtonPress, 0, 4);
    mouse(v, QEvent::MouseMove, 4, 4);
    key(v, Qt::Key_Z, Qt::ControlModifier);
    CHECK_FALSE(dragTool(w)->dragging());
    mouse(v, QEvent::MouseButtonRelease, 4, 4);
    CHECK(undo->index() == 1);
    CHECK(at(w, 2, 0) == cell(kUpperHalf, Red, Black));
    CHECK(at(w, 2, 2) == Cell{});

    // out of a drag the same key is Edit > Undo
    key(v, Qt::Key_Z, Qt::ControlModifier);
    CHECK(undo->index() == 0);
    CHECK(at(w, 2, 0) == Cell{});
}

TEST_CASE("a format change, or its undo or redo, ends a drag") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    QAction* ice = act(w, QStringLiteral("iceColours"));
    REQUIRE(ice->isChecked());
    use(w, "rectangle");
    v->setForeground(Yellow);
    const auto dragAnd = [&](const std::function<void()>& during) {
        mouse(v, QEvent::MouseButtonPress, 0, 0);
        mouse(v, QEvent::MouseMove, 6, 6);
        REQUIRE(dragTool(w)->dragging());
        during();
        CHECK_FALSE(dragTool(w)->dragging());
        mouse(v, QEvent::MouseButtonRelease, 6, 6);
        CHECK(changedCells(w) == 0);
    };
    // the change itself, as its menu item or a shortcut triggers it
    dragAnd([&] { ice->trigger(); });
    CHECK_FALSE(w.document()->ansi().ice);
    REQUIRE(undo->index() == 1);
    // undoing it and redoing it
    dragAnd([&] { undo->undo(); });
    CHECK(w.document()->ansi().ice);
    dragAnd([&] { undo->redo(); });
    CHECK_FALSE(w.document()->ansi().ice);
    CHECK(undo->count() == 1);
}

TEST_CASE("toggling mirror mode or filled shapes mid-drag redoes the preview, and release lands it") {
    // C5, R5: the edit shown is the edit that lands, without a move between
    SECTION("mirror mode") {
        MainWindow w;
        CanvasView* v = w.canvasView();
        const int last = w.document()->canvas().width() - 1;
        use(w, "halfBlock");
        v->setForeground(Yellow);
        mouse(v, QEvent::MouseButtonPress, 1, 0);
        mouse(v, QEvent::MouseMove, 3, 0);
        CHECK(dragTool(w)->pending().changes.size() == 3);
        act(w, QStringLiteral("mirrorMode"))->trigger();
        REQUIRE(v->mirror());
        CHECK(dragTool(w)->pending().changes.size() == 6);
        releaseMatchesPreview(w, 3, 0);
        CHECK(at(w, last - 2, 0) == cell(kUpperHalf, Yellow, Black));

        // and off again
        mouse(v, QEvent::MouseButtonPress, 1, 4);
        mouse(v, QEvent::MouseMove, 3, 4);
        CHECK(dragTool(w)->pending().changes.size() == 6);
        act(w, QStringLiteral("mirrorMode"))->trigger();
        CHECK(dragTool(w)->pending().changes.size() == 3);
        mouse(v, QEvent::MouseButtonRelease, 3, 4);
        CHECK(at(w, last - 2, 2) == Cell{});
        CHECK(at(w, 2, 2) == cell(kUpperHalf, Yellow, Black));
    }
    SECTION("filled shapes") {
        MainWindow w;
        CanvasView* v = w.canvasView();
        use(w, "rectangle");
        v->setForeground(White);
        mouse(v, QEvent::MouseButtonPress, 0, 0);
        mouse(v, QEvent::MouseMove, 6, 7);
        const size_t outline = dragTool(w)->pending().changes.size();
        act(w, QStringLiteral("filledShapes"))->trigger();
        CHECK(dragTool(w)->pending().changes.size() > outline);
        releaseMatchesPreview(w, 6, 7);
        CHECK(at(w, 3, 2) == cell(kFullBlock, White, Black));
    }
    SECTION("each tool, mirrored") {
        for (const char* id : {"halfBlock", "shade", "rampShade", "glyph", "colorize", "line", "rectangle", "ellipse"}) {
            INFO(id);
            MainWindow w;
            CanvasView* v = w.canvasView();
            use(w, id);
            v->setForeground(LightGreen);
            mouse(v, QEvent::MouseButtonPress, 1, 1);
            mouse(v, QEvent::MouseMove, 4, 6);
            act(w, QStringLiteral("mirrorMode"))->trigger();
            releaseMatchesPreview(w, 4, 6);
        }
    }
}

TEST_CASE("mirror mode stays inside the selection, preview and edit alike") {
    // C3: the mirrored half of an edit is clipped to the selection too
    for (const char* id : {"halfBlock", "shade", "rampShade", "glyph", "colorize", "line", "rectangle", "ellipse", "fill"}) {
        INFO(id);
        MainWindow w;
        CanvasView* v = w.canvasView();
        v->setSelection(Selection(CellRect{0, 0, 10, 4}));
        act(w, QStringLiteral("mirrorMode"))->trigger();
        use(w, id);
        v->setForeground(LightGreen);
        v->setBackground(Blue);
        mouse(v, QEvent::MouseButtonPress, 1, 1);
        mouse(v, QEvent::MouseMove, 5, 5);
        for (const CellChange& c : dragTool(w)->pending().changes)
            CHECK(c.x < 10);
        mouse(v, QEvent::MouseButtonRelease, 5, 5);
        CHECK(w.document()->undoStack()->count() == 1);
        const Canvas& c = w.document()->canvas();
        for (int y = 0; y < c.height(); ++y)
            for (int x = 10; x < c.width(); ++x)
                CHECK(c.at(x, y) == Cell{});
    }
}

TEST_CASE("mirror mode with a selection clips the stroke and its mirror each on its own") {
    // R6: on a 10x1 canvas with columns 7-9 selected, every tool drawn at
    // column 1 paints its mirror at 8, and drawn at 8 paints 8 and drops
    // the mirror at 1; the preview is the edit that lands
    for (const char* id : {"halfBlock", "shade", "rampShade", "glyph", "colorize", "line", "rectangle", "ellipse"}) {
        for (const int x : {1, 8}) {
            INFO(id << " at " << x);
            MainWindow w;
            AnsiDocument doc;
            doc.canvas = Canvas(10, 1);
            w.document()->replace(std::move(doc), QString());
            CanvasView* v = w.canvasView();
            v->setSelection(Selection(CellRect{7, 0, 3, 1}));
            act(w, QStringLiteral("mirrorMode"))->trigger();
            REQUIRE(v->mirror());
            use(w, id);
            v->setForeground(LightGreen);
            mouse(v, QEvent::MouseButtonPress, x, 0);
            const CellEdit shown = dragTool(w)->pending();
            REQUIRE(shown.changes.size() == 1);
            CHECK(shown.changes[0].x == 8);
            mouse(v, QEvent::MouseButtonRelease, x, 0);
            CHECK(w.document()->undoStack()->count() == 1);
            CHECK(at(w, 8, 0) == shown.changes[0].after);
            CHECK(changedCells(w) == 1);
        }
    }
    // fill floods inside the selection only; its mirror image is painted
    // where it falls inside (over the text at 5 that bounded the flood)
    // and dropped where it falls outside
    MainWindow w;
    AnsiDocument doc;
    doc.canvas = Canvas(10, 1);
    doc.canvas.at(5, 0) = cell(U'x', White, Black);
    w.document()->replace(std::move(doc), QString());
    CanvasView* v = w.canvasView();
    v->setSelection(Selection(CellRect{2, 0, 8, 1}));
    act(w, QStringLiteral("mirrorMode"))->trigger();
    use(w, "fill");
    v->setForeground(LightGreen);
    drag(v, {{3, 0}});
    CHECK(w.document()->undoStack()->count() == 1);
    for (int x = 0; x < 10; ++x) {
        INFO(x);
        if (x >= 2 && x <= 7)
            CHECK(halfBlocks(at(w, x, 0)) == HalfPair{LightGreen, LightGreen});
        else
            CHECK(at(w, x, 0) == (x == 5 ? cell(U'x', White, Black) : Cell{}));
    }
}

TEST_CASE("mirror mode mirrors pixels and shade steps, not the cells they made") {
    // R1: the mirror cell keeps its other half, and the shade brush steps
    // the mirror cell's own level
    MainWindow w;
    CanvasView* v = w.canvasView();
    const int last = w.document()->canvas().width() - 1;
    use(w, "halfBlock");
    v->setForeground(Red);
    drag(v, {{last - 1, 1}});
    REQUIRE(halfBlocks(at(w, last - 1, 0)) == HalfPair{Black, Red});
    act(w, QStringLiteral("mirrorMode"))->trigger();
    v->setForeground(Yellow);
    drag(v, {{1, 0}});
    CHECK(halfBlocks(at(w, 1, 0)) == HalfPair{Yellow, Black});
    CHECK(halfBlocks(at(w, last - 1, 0)) == HalfPair{Yellow, Red});

    // shade: the mirror cell was ▒, the drawn one blank
    act(w, QStringLiteral("mirrorMode"))->trigger();
    use(w, "glyph");
    v->setForeground(Yellow);
    v->setBackground(Blue);
    act(w, QStringLiteral("glyphTool"))->trigger();
    qobject_cast<GlyphTool*>(w.tools()->active())->setGlyph(kShadeMedium);
    drag(v, {{last - 1, 4}});
    REQUIRE(at(w, last - 1, 2).ch == kShadeMedium);
    act(w, QStringLiteral("mirrorMode"))->trigger();
    use(w, "shade");
    drag(v, {{1, 4}});
    CHECK(at(w, 1, 2) == cell(kShadeLight, Yellow, Blue));
    CHECK(at(w, last - 1, 2) == cell(kShadeDark, Yellow, Blue));
    // the glyph brush still mirrors its cells, flipped
    use(w, "glyph");
    qobject_cast<GlyphTool*>(w.tools()->active())->setGlyph(U'▌');
    drag(v, {{2, 8}});
    CHECK(at(w, 2, 4).ch == U'▌');
    CHECK(at(w, last - 2, 4).ch == U'▐');
}

TEST_CASE("fill floods where it is pressed, once") {
    // N3: the release point doesn't move it
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rectangle");
    v->setForeground(White);
    drag(v, {{2, 2}, {8, 8}});
    use(w, "fill");
    v->setForeground(Blue);
    drag(v, {{5, 5}, {0, 0}, {20, 20}});
    const Canvas& c = w.document()->canvas();
    CHECK(pickColour(c, 5, 5, false) == Color::indexed(Blue));
    CHECK(pickColour(c, 0, 0, false) == Color::indexed(Black));
    CHECK(pickColour(c, 20, 20, false) == Color::indexed(Black));
    CHECK(w.document()->undoStack()->count() == 2);
}

TEST_CASE("the eyedropper and fill read colours as they show with iCE off") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    Cell bright;
    bright.bg = Color::indexed(Yellow);
    REQUIRE(w.document()->edit(CellEdit{{{0, 0, Cell{}, bright}}}));
    act(w, QStringLiteral("iceColours"))->trigger();
    REQUIRE_FALSE(w.document()->ansi().ice);
    use(w, "eyedropper");
    drag(v, {{0, 0}}, Qt::RightButton);
    CHECK(v->pen().bg == Color::indexed(Brown));
    // half-block on the top: the bottom keeps the brown it showed
    use(w, "halfBlock");
    v->setForeground(White);
    drag(v, {{0, 0}});
    // (and the blink the bright background showed as)
    Cell shown = cell(kUpperHalf, White, Brown);
    shown.attrs = attr::Blink;
    CHECK(at(w, 0, 0) == shown);
}

TEST_CASE("halfRowAt floors in 64 bits and clamps at the int range") {
    // C6, N7
    MainWindow w;
    CanvasView* v = w.canvasView();
    const long long h = v->cellRect(QPoint(0, 0)).height();
    REQUIRE(h > 0);
    const auto expect = [h](long long y) {
        long long q = (2 * y) / h;
        if ((2 * y) % h != 0 && y < 0)
            --q;
        return int(std::clamp(q, (long long)INT_MIN, (long long)INT_MAX));
    };
    for (const int y : {INT_MAX, INT_MAX - 1, INT_MIN / 2, INT_MIN / 2 - 1, INT_MIN, INT_MIN + 1, -1, 0, 1})
        CHECK(v->halfRowAt(QPoint(0, y)) == expect(y));
    CHECK(v->halfRowAt(QPoint(0, INT_MIN)) < 0);
    CHECK(v->halfRowAt(QPoint(0, -1)) == -1);
    CHECK(v->cellAt(QPoint(INT_MIN, INT_MIN)).x() < 0);
}

TEST_CASE("no action takes Ctrl+Shift+U or the Find key") {
    // N1, N2: Ctrl+Shift+U starts Unicode entry under IBus, and Ctrl+F is
    // kept for Find. "no two actions share a shortcut" (test_app_format.cpp)
    // checks the rest.
    MainWindow w;
    QList<QKeySequence> reserved = QKeySequence::keyBindings(QKeySequence::Find);
    reserved << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U);
    for (QAction* a : w.findChildren<QAction*>())
        for (const QKeySequence& seq : a->shortcuts()) {
            INFO(a->objectName().toStdString() << ": " << seq.toString().toStdString());
            CHECK_FALSE(reserved.contains(seq));
        }
}

TEST_CASE("shade brush builds up with the left button and takes down with the right") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "shade");
    v->setForeground(LightCyan);
    v->setBackground(Blue);
    drag(v, {{0, 0}, {6, 0}});
    CHECK(at(w, 0, 0) == cell(kShadeLight, LightCyan, Blue));
    CHECK(at(w, 3, 0) == cell(kShadeLight, LightCyan, Blue));
    drag(v, {{0, 0}, {2, 0}});
    CHECK(at(w, 1, 0).ch == kShadeMedium);
    CHECK(at(w, 3, 0).ch == kShadeLight);
    drag(v, {{1, 0}, {3, 0}}, Qt::RightButton);
    CHECK(at(w, 1, 0).ch == kShadeLight);
    // a take-down writes the pen's colours too, as Moebius does (N4)
    CHECK(at(w, 3, 0) == cell(U' ', LightCyan, Blue));
    CHECK(w.document()->undoStack()->count() == 3);
}

TEST_CASE("glyph brush: F-keys pick the glyph, the left button writes it and the right erases") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    auto* glyph = qobject_cast<GlyphTool*>(w.tools()->find(QStringLiteral("glyph")));
    REQUIRE(glyph);
    CHECK(glyph->glyph() == kFullBlock);
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);

    use(w, "glyph");
    CHECK(label->text().contains(QStringLiteral("█")));
    v->setCellCursor({20, 5});
    press(v, Qt::Key_F2);
    const char32_t want = glyphSetGlyph(v->glyphSet(), 1);
    CHECK(glyph->glyph() == want);
    CHECK(label->text().contains(QString::fromUcs4(&want, 1)));
    // the F-key picked the glyph; it didn't type it
    CHECK(w.document()->undoStack()->count() == 0);

    v->setForeground(LightGreen);
    drag(v, {{0, 2}, {3, 2}});
    for (int x = 0; x <= 3; ++x)
        CHECK(at(w, x, 1) == cell(want, LightGreen, Black));
    drag(v, {{1, 2}, {2, 3}}, Qt::RightButton);
    CHECK(at(w, 1, 1) == Cell{});
    CHECK(at(w, 2, 1) == Cell{});
    CHECK(at(w, 3, 1).ch == want);

    // another tool's status is its own
    use(w, "line");
    CHECK(label->text().isEmpty());
}

TEST_CASE("colorize brush recolours and keeps the glyphs") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    w.tools()->setActive(QStringLiteral("keyboard"));
    v->setCellCursor({0, 0});
    for (const char c : {'a', 'b', 'c'}) {
        QKeyEvent down(QEvent::KeyPress, QChar(c).toUpper().unicode(), Qt::NoModifier, QString(QChar(c)));
        QApplication::sendEvent(v, &down);
    }
    use(w, "colorize");
    v->setForeground(Yellow);
    v->setBackground(Red);
    drag(v, {{0, 0}, {1, 0}});
    CHECK(at(w, 0, 0) == cell(U'a', Yellow, Black));
    CHECK(at(w, 1, 0) == cell(U'b', Yellow, Black));
    CHECK(at(w, 2, 0) == cell(U'c', Grey, Black));
    drag(v, {{1, 0}, {2, 0}}, Qt::RightButton);
    CHECK(at(w, 1, 0) == cell(U'b', Yellow, Red));
    CHECK(at(w, 2, 0) == cell(U'c', Grey, Red));
}

TEST_CASE("line, rectangle and ellipse draw the core's shapes in half-block pixels") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    const Canvas blank = w.document()->canvas();
    const int cw = blank.width(), ch = 2 * blank.height();
    v->setForeground(LightRed);
    // what the canvas should hold after drawing `px` on the blank canvas
    const auto expect = [&](const Pixels& px) {
        Canvas want = blank;
        plotPixels(blank, px, LightRed).apply(want);
        return want;
    };

    use(w, "line");
    drag(v, {{2, 1}, {9, 6}, {12, 8}});
    CHECK(w.document()->canvas() == expect(lineShape(2, 1, 12, 8, cw, ch)));
    w.document()->undoStack()->undo();

    use(w, "rectangle");
    drag(v, {{9, 7}, {3, 2}});
    CHECK(w.document()->canvas() == expect(rectShape(3, 2, 9, 7, false, cw, ch)));
    w.document()->undoStack()->undo();

    act(w, QStringLiteral("filledShapes"))->trigger();
    auto* rect = qobject_cast<ShapeTool*>(w.tools()->find(QStringLiteral("rectangle")));
    auto* ellipse = qobject_cast<ShapeTool*>(w.tools()->find(QStringLiteral("ellipse")));
    REQUIRE(rect);
    REQUIRE(ellipse);
    CHECK(rect->filled());
    CHECK(ellipse->filled());
    drag(v, {{3, 2}, {9, 7}});
    CHECK(w.document()->canvas() == expect(rectShape(3, 2, 9, 7, true, cw, ch)));
    w.document()->undoStack()->undo();

    use(w, "ellipse");
    drag(v, {{0, 0}, {8, 6}});
    CHECK(w.document()->canvas() == expect(ellipseShape(0, 0, 8, 6, true, cw, ch)));
    w.document()->undoStack()->undo();
    act(w, QStringLiteral("filledShapes"))->trigger();
    CHECK_FALSE(ellipse->filled());
    drag(v, {{0, 0}, {8, 6}});
    CHECK(w.document()->canvas() == expect(ellipseShape(0, 0, 8, 6, false, cw, ch)));
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("fill floods the half-block pixels under the pointer as one undo step") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rectangle");
    v->setForeground(White);
    drag(v, {{2, 2}, {8, 8}});
    use(w, "fill");
    v->setForeground(Blue);
    v->setBackground(Green);
    drag(v, {{5, 5}});
    const Canvas& c = w.document()->canvas();
    CHECK(pickColour(c, 5, 5, false) == Color::indexed(Blue));
    CHECK(pickColour(c, 3, 3, false) == Color::indexed(Blue));
    CHECK(pickColour(c, 2, 2, false) == Color::indexed(White));
    CHECK(pickColour(c, 0, 0, false) == Color::indexed(Black));
    CHECK(w.document()->undoStack()->count() == 2);
    // the right button fills with the background colour, outside here
    drag(v, {{0, 0}}, Qt::RightButton);
    CHECK(pickColour(c, 0, 0, false) == Color::indexed(Green));
    CHECK(pickColour(c, 79, 79, false) == Color::indexed(Green));
    CHECK(pickColour(c, 5, 5, false) == Color::indexed(Blue));
}

TEST_CASE("eyedropper picks the colour under the pointer into the pen") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "halfBlock");
    v->setForeground(Yellow);
    v->setBackground(Magenta);
    drag(v, {{1, 2}});
    v->setForeground(Blue);
    drag(v, {{1, 3}});
    v->setPen(Pen{});

    use(w, "eyedropper");
    drag(v, {{1, 2}});
    CHECK(v->pen().fg == Color::indexed(Yellow));
    drag(v, {{1, 3}}, Qt::RightButton);
    CHECK(v->pen().bg == Color::indexed(Blue));
    CHECK(v->pen().fg == Color::indexed(Yellow));
    // nothing was edited
    CHECK(w.document()->undoStack()->count() == 2);
}

TEST_CASE("changedCells is the union of the changed cells, in any order") {
    // R3: built row by row, so a full-canvas fill commits in time
    CellEdit e;
    Cell ink;
    ink.ch = U'x';
    QRegion naive;
    unsigned seed = 7;
    for (int i = 0; i < 3000; ++i) {
        seed = seed * 1103515245u + 12345u;
        const int x = int(seed >> 8) % 60, y = int(seed >> 20) % 40;
        const bool same = (seed >> 3) % 5 == 0;
        e.changes.push_back({x, y, Cell{}, same ? Cell{} : ink});
        if (!same)
            naive |= QRect(x, y, 1, 1);
    }
    CHECK(Document::changedCells(e) == naive);
    CHECK(Document::changedCells(CellEdit{}).isEmpty());
}
