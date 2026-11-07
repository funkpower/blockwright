// Format > Canvas Size... headless (PLAN §5, #397): the action, the dialog,
// the resize as one undo step that gives back the layers, the selection and
// the cursor, and the sizes it refuses. main() is in test_app.cpp.

#include "util.h"

#include "blockwright/canvas_size.h"
#include "blockwright/composite.h"

#include "canvas_size_dialog.h"
#include "canvas_view.h"
#include "document.h"
#include "irc_meter.h"
#include "main_window.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QBuffer>
#include <QImage>
#include <QInputMethodEvent>
#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QUndoStack>

#include <functional>
#include <vector>
#include <memory>

using namespace bw;

namespace {

QAction* action(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

QMenu* menuNamed(MainWindow& w, const QString& title) {
    for (QAction* m : w.menuBar()->actions())
        if (m->menu() && m->text().remove(QLatin1Char('&')) == title)
            return m->menu();
    return nullptr;
}

// Fills in the dialog Format > Canvas Size... opens, then presses `button`.
void runDialog(MainWindow& w, const std::function<void(CanvasSizeDialog*)>& fill,
               QDialogButtonBox::StandardButton button = QDialogButtonBox::Ok) {
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* d = qobject_cast<CanvasSizeDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        fill(d);
        ran = true;
        d->buttons()->button(button)->click();
    });
    action(w, "canvasSize")->trigger();
    CHECK(ran);
}

std::shared_ptr<const std::string> png(int w, int h) {
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer out(&bytes);
    REQUIRE(out.open(QIODevice::WriteOnly));
    REQUIRE(image.save(&out, "PNG"));
    return std::make_shared<const std::string>(bytes.toStdString());
}

// A 10x6 canvas with marked cells, a canvas-sized ink layer moved to
// (2, 1), and a reference picture at (12, 20) canvas pixels, 40x32; the ink
// layer is active.
void paint(MainWindow& w) {
    QString error;
    REQUIRE(w.newCanvas(QSize(10, 6), true, &error));
    Document* doc = w.document();
    CellEdit e;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 10; ++x) {
            Cell c;
            c.ch = char32_t(U'a' + (x + 3 * y) % 26);
            c.fg = Color::indexed(std::uint8_t(1 + x % 15));
            e.changes.push_back({x, y, doc->canvas().at(x, y), c});
        }
    REQUIRE(doc->edit(e));
    const LayerId ink = doc->addLayer(LayerKind::Cells);
    REQUIRE(ink);
    LayerProperties props = doc->layers().find(ink)->props;
    props.x = 2;
    props.y = 1;
    REQUIRE(doc->setLayerProperties(ink, props, QStringLiteral("Move")));
    const LayerId ref = doc->addReferenceLayer(ReferenceImage{png(4, 4), 12, 20, 40, 32, 50});
    REQUIRE(ref);
    REQUIRE(doc->setActiveLayer(ink));
    REQUIRE(doc->layers().size() == 3);
    REQUIRE(doc->layers()[1].bounds() == CellRect{2, 1, 10, 6});
    REQUIRE(doc->layers()[2].props.kind == LayerKind::Reference);
    REQUIRE(doc->layers()[2].props.reference);
}

// The renderer draws the document's canvas at its size.
bool showsCanvas(MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    const QSize cs = w.renderer().cellSize();
    return w.renderer().imageSize() == QSize(c.width() * cs.width(), c.height() * cs.height());
}

} // namespace

TEST_CASE("Format > Canvas Size... sits above Target with no default key") {
    MainWindow w;
    QAction* size = action(w, "canvasSize");
    QMenu* format = menuNamed(w, QStringLiteral("Format"));
    REQUIRE(format);
    const QList<QAction*> items = format->actions();
    const qsizetype at = items.indexOf(size);
    REQUIRE(at >= 0);
    CHECK(size->text() == QStringLiteral("Canvas &Size..."));
    CHECK(size->shortcut().isEmpty());
    REQUIRE(at + 1 < items.size());
    CHECK(items[at + 1]->isSeparator());
}

TEST_CASE("the Canvas Size dialog holds File > New's limits and says what each side gets") {
    CanvasSizeDialog d(QSize(80, 25));
    CHECK(d.windowTitle() == QStringLiteral("Canvas Size"));
    CHECK(d.widthBox()->minimum() == 1);
    CHECK(d.widthBox()->maximum() == kMaxColumns);
    CHECK(d.heightBox()->minimum() == 1);
    CHECK(d.heightBox()->maximum() == kMaxRows);
    CHECK(d.canvasSize() == QSize(80, 25));
    CHECK(d.anchor() == Anchor::TopLeft);
    CHECK(d.anchorButton(Anchor::TopLeft)->isChecked());
    CHECK(d.summary()->text() == QStringLiteral("No change"));

    d.widthBox()->setValue(85);
    CHECK(d.summary()->text() == QStringLiteral("5 columns added on the right"));
    d.anchorButton(Anchor::Centre)->click();
    CHECK(d.anchor() == Anchor::Centre);
    CHECK_FALSE(d.anchorButton(Anchor::TopLeft)->isChecked());
    CHECK(d.summary()->text() == QStringLiteral("2 columns added on the left; 3 columns added on the right"));
    d.heightBox()->setValue(24);
    CHECK(d.summary()->text() ==
          QStringLiteral("2 columns added on the left; 3 columns added on the right; 1 row cut at the bottom"));
    d.anchorButton(Anchor::BottomRight)->click();
    CHECK(d.summary()->text() == QStringLiteral("5 columns added on the left; 1 row cut at the top"));

    // each anchor has its name and a button of its own
    for (int i = 0; i < 9; ++i) {
        QAbstractButton* b = d.anchorButton(Anchor(i));
        CHECK(b->objectName() == QStringLiteral("anchor-%1").arg(QString::fromLatin1(anchorName(Anchor(i)))));
        CHECK_FALSE(b->accessibleName().isEmpty());
        b->click();
        CHECK(d.anchor() == Anchor(i));
    }
    CHECK(canvasSizeSummary(QSize(80, 25), QSize(79, 26), Anchor::TopLeft) ==
          QStringLiteral("1 column cut on the right; 1 row added at the bottom"));
}

TEST_CASE("Canvas Size is one undo step that gives back the layers, selection and cursor") {
    MainWindow w;
    paint(w);
    Document* doc = w.document();
    CanvasView* view = w.canvasView();
    const LayerStack before = doc->layers();
    const Canvas shownBefore = doc->canvas();
    const int steps = doc->undoStack()->count();
    view->setSelection(Selection(CellRect{6, 3, 4, 3}));
    view->setCellCursor(Cursor{9, 5});
    const Selection selBefore = view->selection();
    const Cursor cursorBefore = view->cellCursor();

    // shrink to 7x4 at the bottom-right: 3 columns off the left, 2 rows off the top
    runDialog(w, [](CanvasSizeDialog* d) {
        d->widthBox()->setValue(7);
        d->heightBox()->setValue(4);
        d->setAnchor(Anchor::BottomRight);
    });
    CHECK(doc->undoStack()->count() == steps + 1);
    CHECK(doc->undoStack()->undoText() == QStringLiteral("Canvas Size"));
    CHECK(doc->isModified());
    const LayerStack after = doc->layers();
    CHECK(after == resizeCanvas(before, 7, 4, {-3, -2}, 8));
    CHECK(doc->canvas().width() == 7);
    CHECK(doc->canvas().height() == 4);
    CHECK(doc->canvas() == composite(after, {}));
    CHECK(doc->canvas().at(0, 0) == shownBefore.at(3, 2));
    // the ink layer only moved, by the shift, and the picture with it
    CHECK(after[1].bounds() == CellRect{-1, -1, 10, 6});
    REQUIRE(after[2].props.reference);
    CHECK(after[2].props.reference->x == 12 - 3 * 8);
    CHECK(after[2].props.reference->y == 20 - 2 * 16);
    CHECK(after[2].props.reference->width == 40);
    CHECK(view->selection() == Selection(CellRect{3, 1, 4, 3}));
    CHECK(view->cellCursor() == Cursor{6, 3});
    CHECK(showsCanvas(w));

    doc->undoStack()->undo();
    CHECK(doc->layers() == before);
    CHECK(doc->canvas() == shownBefore);
    CHECK(view->selection() == selBefore);
    CHECK(view->cellCursor() == cursorBefore);
    CHECK(showsCanvas(w));

    doc->undoStack()->redo();
    CHECK(doc->layers() == after);
    CHECK(view->selection() == Selection(CellRect{3, 1, 4, 3}));
    CHECK(view->cellCursor() == Cursor{6, 3});

    doc->undoStack()->undo();
    CHECK(doc->layers() == before);
}

TEST_CASE("growing at the centre keeps every cell and moves the selection") {
    MainWindow w;
    paint(w);
    Document* doc = w.document();
    CanvasView* view = w.canvasView();
    const Canvas shownBefore = doc->canvas();
    view->setSelection(Selection(CellRect{0, 0, 10, 6}));
    QString error;
    REQUIRE(w.resizeCanvas(QSize(15, 9), Anchor::Centre, &error));
    CHECK(doc->canvas().width() == 15);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 10; ++x)
            CHECK(doc->canvas().at(x + 2, y + 1) == shownBefore.at(x, y));
    CHECK(doc->canvas().at(0, 0) == Cell{});
    CHECK(doc->canvas().at(14, 8) == Cell{});
    CHECK(view->selection() == Selection(CellRect{2, 1, 10, 6}));
}

TEST_CASE("a selection the resize cuts off goes, and the cursor stays on the canvas") {
    MainWindow w;
    paint(w);
    CanvasView* view = w.canvasView();
    view->setSelection(Selection(CellRect{8, 4, 2, 2}));
    view->setCellCursor(Cursor{9, 5});
    QString error;
    REQUIRE(w.resizeCanvas(QSize(5, 3), Anchor::TopLeft, &error));
    CHECK(view->selection().empty());
    CHECK(view->cellCursor() == Cursor{4, 2});
    w.document()->undoStack()->undo();
    CHECK(view->selection() == Selection(CellRect{8, 4, 2, 2}));
    CHECK(view->cellCursor() == Cursor{9, 5});
}

TEST_CASE("the same size, Cancel and sizes past the limits change nothing") {
    MainWindow w;
    paint(w);
    Document* doc = w.document();
    const LayerStack before = doc->layers();
    const int steps = doc->undoStack()->count();

    QString error;
    CHECK(w.resizeCanvas(QSize(10, 6), Anchor::Centre, &error));
    CHECK(doc->undoStack()->count() == steps);

    runDialog(w, [](CanvasSizeDialog* d) { d->widthBox()->setValue(40); }, QDialogButtonBox::Cancel);
    CHECK(doc->undoStack()->count() == steps);

    for (QSize s : {QSize(0, 6), QSize(kMaxColumns + 1, 6), QSize(10, kMaxRows + 1)}) {
        error.clear();
        CHECK_FALSE(w.resizeCanvas(s, Anchor::TopLeft, &error));
        CHECK(error == QStringLiteral("a canvas is 1 to 400 columns wide and 1 to 5000 rows high"));
    }
    CHECK(doc->layers() == before);
    CHECK(doc->undoStack()->count() == steps);
    CHECK(showsCanvas(w));
}

TEST_CASE("a canvas too large to display at 9 px is refused and the old one stays shown") {
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(80, 25), true, &error));
    action(w, "ninePx")->trigger();
    REQUIRE(w.renderer().cellSize().width() == 9);
    Document* doc = w.document();
    const int steps = doc->undoStack()->count();
    error.clear();
    CHECK_FALSE(w.resizeCanvas(QSize(kMaxColumns, kMaxRows), Anchor::TopLeft, &error));
    CHECK(error == QStringLiteral("the canvas is too large to display"));
    CHECK(doc->undoStack()->count() == steps);
    CHECK(doc->canvas().width() == 80);
    CHECK(showsCanvas(w));

    // at 8 px it fits
    action(w, "ninePx")->trigger();
    REQUIRE(w.renderer().cellSize().width() == 8);
    REQUIRE(w.resizeCanvas(QSize(kMaxColumns, kMaxRows), Anchor::TopLeft, &error));
    CHECK(doc->canvas().width() == kMaxColumns);
    CHECK(doc->canvas().height() == kMaxRows);
    CHECK(showsCanvas(w));
}

TEST_CASE("the dialog's refusal is told in a message box") {
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(80, 25), true, &error));
    action(w, "ninePx")->trigger();
    bool told = false;
    QString text;
    // the message box opens once the dialog closes
    QTimer::singleShot(0, &w, [&] {
        auto* d = qobject_cast<CanvasSizeDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        d->widthBox()->setValue(kMaxColumns);
        d->heightBox()->setValue(kMaxRows);
        QTimer::singleShot(0, &w, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                told = true;
                text = box->text();
                box->button(QMessageBox::Ok)->click();
            }
        });
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    action(w, "canvasSize")->trigger();
    CHECK(told);
    CHECK(text == QStringLiteral("The canvas can't be made that size: the canvas is too large to display."));
    CHECK(w.document()->canvas().width() == 80);
}

TEST_CASE("the IRC meter measures the rows where the resize put them") {
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(10, 6), true, &error));
    // row y holds y + 1 letters, so each row's line is its own length
    CellEdit e;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x <= y; ++x) {
            Cell c;
            c.ch = U'x';
            e.changes.push_back({x, y, w.document()->canvas().at(x, y), c});
        }
    REQUIRE(w.document()->edit(e));
    IrcMeter* meter = w.ircMeter();
    std::vector<std::size_t> before;
    for (int y = 0; y < 6; ++y)
        before.push_back(meter->bytes(y));
    REQUIRE(before[0] != before[5]);
    // 3 rows added at the top
    REQUIRE(w.resizeCanvas(QSize(10, 9), Anchor::Bottom, &error));
    QApplication::processEvents();
    for (int y = 0; y < 6; ++y)
        CHECK(meter->bytes(y + 3) == before[std::size_t(y)]);
    w.document()->undoStack()->undo();
    QApplication::processEvents();
    for (int y = 0; y < 6; ++y)
        CHECK(meter->bytes(y) == before[std::size_t(y)]);
}

TEST_CASE("ids given since a resize aren't given again after its undo") {
    // the round-1 review's repro
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(10, 6), true, &error));
    Document* doc = w.document();
    REQUIRE(doc->layers().size() == 1);
    REQUIRE(doc->layers().nextId() == 2);
    REQUIRE(w.resizeCanvas(QSize(12, 6), Anchor::TopLeft, &error));
    CHECK(doc->addLayer(LayerKind::Cells) == 2);
    doc->undoStack()->undo();
    doc->undoStack()->undo();
    CHECK(doc->layers().width() == 10);
    CHECK(doc->layers().nextId() == 3);
    CHECK(doc->addLayer(LayerKind::Cells) == 3);


    // and through a redo of the resize
    doc->undoStack()->undo();
    REQUIRE(w.resizeCanvas(QSize(12, 6), Anchor::TopLeft, &error));
    CHECK(doc->addLayer(LayerKind::Cells) == 4);
    doc->undoStack()->undo();
    doc->undoStack()->undo();
    doc->undoStack()->redo();
    CHECK(doc->layers().width() == 12);
    CHECK(doc->layers().nextId() == 5);
    CHECK(doc->addLayer(LayerKind::Cells) == 5);
}

TEST_CASE("a cursor past the end of its row stays so while its cell is still the row's last") {
    // the round-1 review's repro: "ae" on a two-column row
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(2, 1), true, &error));
    Document* doc = w.document();
    CellEdit e;
    for (int x = 0; x < 2; ++x) {
        Cell c;
        c.ch = char32_t(x == 0 ? U'a' : U'e');
        e.changes.push_back({x, 0, doc->canvas().at(x, 0), c});
    }
    REQUIRE(doc->edit(e));
    CanvasView* view = w.canvasView();
    view->setCellCursor(Cursor{1, 0, true});
    REQUIRE(view->cellCursor() == Cursor{1, 0, true});

    REQUIRE(w.resizeCanvas(QSize(2, 3), Anchor::TopLeft, &error));
    CHECK(view->cellCursor() == Cursor{1, 0, true});
    doc->undoStack()->undo();
    CHECK(view->cellCursor() == Cursor{1, 0, true});
    doc->undoStack()->redo();
    CHECK(view->cellCursor() == Cursor{1, 0, true});
    doc->undoStack()->undo();

    // moved by the shift, still the row's last
    REQUIRE(w.resizeCanvas(QSize(4, 1), Anchor::Right, &error));
    CHECK(view->cellCursor() == Cursor{3, 0, true});
    doc->undoStack()->undo();
    CHECK(view->cellCursor() == Cursor{1, 0, true});

    // a column after it: no longer the row's end
    REQUIRE(w.resizeCanvas(QSize(3, 1), Anchor::TopLeft, &error));
    CHECK(view->cellCursor() == Cursor{1, 0, false});
    doc->undoStack()->undo();
    CHECK(view->cellCursor() == Cursor{1, 0, true});

    // clamped onto the canvas: not past the end of anything
    view->setCellCursor(Cursor{1, 0, true});
    REQUIRE(w.resizeCanvas(QSize(1, 1), Anchor::TopLeft, &error));
    CHECK(view->cellCursor() == Cursor{0, 0, false});
}

TEST_CASE("a resize never leaves the cursor on a continuation") {
    // the round-1 review's repro: a 6x1 canvas, the active layer moved to
    // (1, 0) holding a wide pair on its first two cells, the cursor on its lead
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(6, 1), true, &error));
    Document* doc = w.document();
    REQUIRE(doc->setTarget(Target::Irc));
    const LayerId ink = doc->addLayer(LayerKind::Cells);
    REQUIRE(ink);
    CellEdit e;
    Cell lead, cont;
    lead.ch = U'中';
    cont.ch = Glyph::continuation();
    e.changes.push_back({0, 0, doc->layerCanvas().at(0, 0), lead});
    e.changes.push_back({1, 0, doc->layerCanvas().at(1, 0), cont});
    REQUIRE(doc->edit(e));
    LayerProperties props = doc->layers().find(ink)->props;
    props.x = 1;
    REQUIRE(doc->setLayerProperties(ink, props, QStringLiteral("Move")));
    REQUIRE(doc->layerCanvas().at(2, 0).ch.isContinuation());
    CanvasView* view = w.canvasView();
    view->setCellCursor(Cursor{1, 0});
    REQUIRE(view->cellCursor() == Cursor{1, 0});

    // 4x1 at the right: the layer goes to (-1, 0), its pair kept
    REQUIRE(w.resizeCanvas(QSize(4, 1), Anchor::Right, &error));
    REQUIRE(doc->layers().find(ink)->bounds().x == -1);
    REQUIRE(doc->layerCanvas().at(0, 0).ch.isContinuation());
    const Cursor c = view->cellCursor();
    CHECK_FALSE(doc->layerCanvas().at(c.x, c.y).ch.isContinuation());
    CHECK(c == Cursor{1, 0});
    // a mark typed there has no character before it to join, and the pair
    // is kept (the round-2 review's repro)
    const Canvas kept = doc->layers().find(ink)->cells;
    const int steps = doc->undoStack()->count();
    QInputMethodEvent mark;
    mark.setCommitString(QString(QChar(0x0301)));
    QApplication::sendEvent(view, &mark);
    CHECK(doc->layers().find(ink)->cells == kept);
    CHECK(doc->undoStack()->count() == steps);
    doc->undoStack()->undo();
    CHECK(view->cellCursor() == Cursor{1, 0});
    doc->undoStack()->redo();
    CHECK(view->cellCursor() == Cursor{1, 0});
}

TEST_CASE("on a one-column canvas a continuation is the only cell, and a mark there is refused") {
    // the round-2 review's repro: 3x1, the pair's layer at (1, 0), resized
    // to 1x1 at the right, so the continuation is the row's one cell
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(3, 1), true, &error));
    Document* doc = w.document();
    REQUIRE(doc->setTarget(Target::Irc));
    const LayerId ink = doc->addLayer(LayerKind::Cells);
    REQUIRE(ink);
    CellEdit e;
    Cell lead, cont;
    lead.ch = U'\u4E2D';
    cont.ch = Glyph::continuation();
    e.changes.push_back({0, 0, doc->layerCanvas().at(0, 0), lead});
    e.changes.push_back({1, 0, doc->layerCanvas().at(1, 0), cont});
    REQUIRE(doc->edit(e));
    LayerProperties props = doc->layers().find(ink)->props;
    props.x = 1;
    REQUIRE(doc->setLayerProperties(ink, props, QStringLiteral("Move")));
    CanvasView* view = w.canvasView();
    view->setCellCursor(Cursor{1, 0});
    REQUIRE(w.resizeCanvas(QSize(1, 1), Anchor::Right, &error));
    REQUIRE(doc->layerCanvas().at(0, 0).ch.isContinuation());
    CHECK(view->cellCursor() == Cursor{0, 0});
    // shown as a space (PLAN §16)
    CHECK(doc->canvas().at(0, 0).ch == Glyph(U' '));
    const Canvas kept = doc->layers().find(ink)->cells;
    const int steps = doc->undoStack()->count();
    for (const Cursor at : {Cursor{0, 0}, Cursor{0, 0, true}}) {
        view->setCellCursor(at);
        QInputMethodEvent mark;
        mark.setCommitString(QString(QChar(0x0301)));
        QApplication::sendEvent(view, &mark);
        CHECK(doc->layers().find(ink)->cells == kept);
        CHECK(doc->undoStack()->count() == steps);
    }
}

TEST_CASE("the view keeps its scroll through a resize unless the resize moves the cursor out of sight") {
    // the round-1 review's repro: an 80x100 canvas scrolled to the bottom
    MainWindow w;
    w.resize(640, 480);
    w.show();
    QString error;
    REQUIRE(w.newCanvas(QSize(80, 100), true, &error));
    Document* doc = w.document();
    CanvasView* view = w.canvasView();
    auto* scroll = qobject_cast<QScrollArea*>(view->parentWidget()->parentWidget());
    REQUIRE(scroll);
    QScrollBar* bar = scroll->verticalScrollBar();
    const auto cursorShown = [&] {
        const QRect r = view->cellRect(view->cellCursor());
        const QRect onScreen(view->mapTo(scroll->viewport(), r.topLeft()), r.size());
        return scroll->viewport()->rect().contains(onScreen);
    };
    QApplication::processEvents();

    // the cursor on the bottom row, in view: the resize keeps the scroll
    view->setCellCursor(Cursor{0, 99});
    bar->setValue(bar->maximum());
    QApplication::processEvents();
    REQUIRE(bar->value() > 0);
    REQUIRE(cursorShown());
    const int kept = bar->value();
    REQUIRE(w.resizeCanvas(QSize(80, 102), Anchor::TopLeft, &error));
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{0, 99});
    CHECK(bar->value() == kept);
    CHECK(cursorShown());
    doc->undoStack()->undo();
    QApplication::processEvents();

    // the cursor at the top, the view at the bottom, and the resize leaves
    // the cursor's cell where it was: the scroll is kept, the cursor out of
    // view as it was (the round-2 review's case)
    view->setCellCursor(Cursor{0, 0});
    bar->setValue(bar->maximum());
    QApplication::processEvents();
    REQUIRE_FALSE(cursorShown());
    const int away = bar->value();
    REQUIRE(w.resizeCanvas(QSize(80, 102), Anchor::TopLeft, &error));
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{0, 0});
    CHECK(bar->value() == away);
    CHECK_FALSE(cursorShown());
    doc->undoStack()->undo();
    QApplication::processEvents();

    // the same with the cursor past the end of its row: refitting the view
    // to the new size doesn't drop the past-the-end mark on the way, which
    // would count as a move and scroll to it (the round-3 review's case)
    view->setCellCursor(Cursor{79, 0, true});
    bar->setValue(bar->maximum());
    QApplication::processEvents();
    REQUIRE(view->cellCursor() == Cursor{79, 0, true});
    REQUIRE_FALSE(cursorShown());
    REQUIRE(bar->value() == away);
    REQUIRE(w.resizeCanvas(QSize(80, 102), Anchor::TopLeft, &error));
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{79, 0, true});
    CHECK(bar->value() == away);
    CHECK_FALSE(cursorShown());
    doc->undoStack()->undo();
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{79, 0, true});
    CHECK(bar->value() == away);
    doc->undoStack()->redo();
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{79, 0, true});
    CHECK(bar->value() == away);
    doc->undoStack()->undo();
    QApplication::processEvents();

    // the resize moves the cursor's cell: the view follows it
    view->setCellCursor(Cursor{0, 0});
    bar->setValue(bar->maximum());
    QApplication::processEvents();
    REQUIRE_FALSE(cursorShown());
    REQUIRE(w.resizeCanvas(QSize(80, 102), Anchor::Bottom, &error));
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{0, 2});
    CHECK(cursorShown());
    doc->undoStack()->undo();
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{0, 0});
    CHECK(cursorShown());
}

TEST_CASE("a resize judges an unchanged cursor on the active layer's cells, as the view's cursor does") {
    // the round-4 review's repro: the active layer's wide pair ends row 0,
    // so the cursor past it is past the end, but in the composite a layer
    // above covers the continuation and the lead shows as a space; a
    // resize that leaves the cell in place keeps the cursor and the scroll
    MainWindow w;
    w.resize(640, 480);
    w.show();
    QString error;
    REQUIRE(w.newCanvas(QSize(80, 100), true, &error));
    Document* doc = w.document();
    REQUIRE(doc->setTarget(Target::Irc));
    const LayerId bottom = doc->activeLayer();
    REQUIRE(bottom);
    CellEdit pair;
    Cell lead, cont;
    lead.ch = U'中';
    cont.ch = Glyph::continuation();
    pair.changes.push_back({78, 0, doc->layerCanvas().at(78, 0), lead});
    pair.changes.push_back({79, 0, doc->layerCanvas().at(79, 0), cont});
    REQUIRE(doc->edit(pair));
    const LayerId top = doc->addLayer(LayerKind::Cells);
    REQUIRE(top);
    REQUIRE(doc->setActiveLayer(top));
    CellEdit cover;
    Cell x;
    x.ch = U'X';
    cover.changes.push_back({79, 0, doc->layerCanvas().at(79, 0), x});
    REQUIRE(doc->edit(cover));
    REQUIRE(doc->setActiveLayer(bottom));
    REQUIRE(atRowEnd(doc->layerCanvas(), Cursor{78, 0}));
    REQUIRE_FALSE(atRowEnd(doc->canvas(), Cursor{78, 0}));

    CanvasView* view = w.canvasView();
    auto* scroll = qobject_cast<QScrollArea*>(view->parentWidget()->parentWidget());
    REQUIRE(scroll);
    QScrollBar* bar = scroll->verticalScrollBar();
    QApplication::processEvents();
    view->setCellCursor(Cursor{78, 0, true});
    REQUIRE(view->cellCursor() == Cursor{78, 0, true});
    bar->setValue(bar->maximum());
    QApplication::processEvents();
    const int away = bar->value();
    REQUIRE(away > 0);
    int moves = 0;
    QObject::connect(view, &CanvasView::cursorMoved, view, [&] { ++moves; });

    REQUIRE(w.resizeCanvas(QSize(80, 102), Anchor::TopLeft, &error));
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{78, 0, true});
    CHECK(bar->value() == away);
    doc->undoStack()->undo();
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{78, 0, true});
    CHECK(bar->value() == away);
    doc->undoStack()->redo();
    QApplication::processEvents();
    CHECK(view->cellCursor() == Cursor{78, 0, true});
    CHECK(bar->value() == away);
    CHECK(moves == 0);
}
