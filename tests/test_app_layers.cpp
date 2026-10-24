// The layer panel and editing the active layer (PLAN §5, Phase 3b): the
// Layers dock and the Layer menu, each change an undo step, the tools
// drawing on the active layer, erase, pixel layers, the alpha lock and
// sample merged.

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "layers_dock.h"
#include "main_window.h"
#include "select_tool.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/ansi.h"
#include "blockwright/composite.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/ramp.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QFile>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
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

void drag(CanvasView* v, std::initializer_list<QPoint> points, Qt::MouseButton button = Qt::LeftButton) {
    const QPoint* p = points.begin();
    mouse(v, QEvent::MouseButtonPress, p->x(), p->y(), button);
    for (++p; p != points.end(); ++p)
        mouse(v, QEvent::MouseMove, p->x(), p->y(), button);
    const QPoint last = *(points.end() - 1);
    mouse(v, QEvent::MouseButtonRelease, last.x(), last.y(), button);
}

void key(QWidget* w, int k, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &release);
}

// Types `ch` at cell (x, y) in keyboard mode.
void typeAt(MainWindow& w, int x, int y, QChar ch) {
    act(w, QStringLiteral("keyboardTool"))->trigger();
    w.canvasView()->setCellCursor({x, y});
    key(w.canvasView(), ch.toUpper().unicode(), QString(ch));
}

void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

QStringList rowNames(const LayersDock* d) {
    QStringList names;
    for (int i = 0; i < d->tree()->topLevelItemCount(); ++i)
        names << d->tree()->topLevelItem(i)->text(LayersDock::NameColumn);
    return names;
}

QTreeWidgetItem* rowOf(const LayersDock* d, LayerId id) {
    for (int i = 0; i < d->tree()->topLevelItemCount(); ++i)
        if (LayersDock::layerOf(d->tree()->topLevelItem(i)) == id)
            return d->tree()->topLevelItem(i);
    return nullptr;
}

std::size_t indexOf(Document* doc, LayerId id) { return doc->layers().indexOf(id).value(); }

const Layer& layerById(const Document* doc, LayerId id) {
    const Layer* l = doc->layers().find(id);
    REQUIRE(l);
    return *l;
}

} // namespace

TEST_CASE("the Layers dock lists the layers top first, the active one current", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    LayersDock* d = w.layersWidget();
    REQUIRE(d);
    CHECK(w.layersDock()->widget() == d);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Background")});
    CHECK(LayersDock::layerOf(d->tree()->currentItem()) == doc->activeLayer());
    // one layer: it can't be deleted or moved
    CHECK_FALSE(d->deleteAction()->isEnabled());
    CHECK_FALSE(d->raiseAction()->isEnabled());
    CHECK_FALSE(d->lowerAction()->isEnabled());

    const LayerId bg = doc->activeLayer();
    d->newCellsAction()->trigger();
    const LayerId cells = doc->activeLayer();
    CHECK(cells != bg);
    d->newPixelsAction()->trigger();
    const LayerId pixels = doc->activeLayer();
    CHECK(doc->active()->props.kind == LayerKind::Pixels);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Layer 2"), QStringLiteral("Layer 1"), QStringLiteral("Background")});
    CHECK(d->tree()->topLevelItem(0)->text(LayersDock::KindColumn) == QStringLiteral("Pixels"));
    CHECK(d->tree()->topLevelItem(1)->text(LayersDock::KindColumn) == QStringLiteral("Cells"));
    CHECK(LayersDock::layerOf(d->tree()->currentItem()) == pixels);
    // a new layer is the canvas's size and all transparent
    CHECK(doc->active()->bounds() == CellRect{0, 0, doc->canvas().width(), doc->canvas().height()});
    CHECK(doc->active()->cells.at(3, 3) == transparentCell());
    CHECK(d->deleteAction()->isEnabled());
    CHECK_FALSE(d->raiseAction()->isEnabled());
    CHECK(d->lowerAction()->isEnabled());

    // picking a row makes its layer active, and it is no undo step
    const int steps = doc->undoStack()->count();
    d->tree()->setCurrentItem(rowOf(d, bg));
    CHECK(doc->activeLayer() == bg);
    CHECK(doc->undoStack()->count() == steps);
    CHECK_FALSE(d->lowerAction()->isEnabled());
    // a new layer goes just above the active one
    d->newCellsAction()->trigger();
    CHECK(indexOf(doc, doc->activeLayer()) == 1);
    CHECK(rowNames(d).back() == QStringLiteral("Background"));
    CHECK(rowNames(d).at(2) == QStringLiteral("Layer 3"));
    doc->undoStack()->undo();
    CHECK(doc->activeLayer() == bg);
    CHECK(doc->layers().size() == 3);
    CHECK(d->tree()->topLevelItemCount() == 3);
}

TEST_CASE("adding, deleting and moving layers are undo steps", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    LayersDock* d = w.layersWidget();
    QUndoStack* undo = doc->undoStack();
    const LayerId bg = doc->activeLayer();

    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId a = doc->activeLayer();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("New Cells Layer"));
    CHECK(doc->isModified());
    undo->undo();
    CHECK(doc->layers().size() == 1);
    CHECK(doc->activeLayer() == bg);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Background")});
    undo->redo();
    CHECK(doc->activeLayer() == a);
    CHECK(doc->layers().size() == 2);

    // delete: the layer below becomes active; undo puts it back, active
    typeAt(w, 0, 0, QLatin1Char('B'));
    act(w, QStringLiteral("deleteLayer"))->trigger();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Delete Layer"));
    CHECK(doc->layers().size() == 1);
    CHECK(doc->activeLayer() == bg);
    CHECK(doc->canvas().at(0, 0) == Cell{});
    undo->undo();
    CHECK(doc->activeLayer() == a);
    CHECK(indexOf(doc, a) == 1);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    // the last layer can't go
    doc->setActiveLayer(bg);
    CHECK(doc->removeLayer(a));
    CHECK_FALSE(doc->removeLayer(bg));
    CHECK_FALSE(act(w, QStringLiteral("deleteLayer"))->isEnabled());
    undo->undo();

    // raise and lower
    doc->setActiveLayer(bg);
    act(w, QStringLiteral("raiseLayer"))->trigger();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Raise Layer"));
    CHECK(indexOf(doc, bg) == 1);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Background"), QStringLiteral("Layer 1")});
    CHECK(doc->canvas().at(0, 0) == Cell{});  // the opaque background now covers the B
    act(w, QStringLiteral("lowerLayer"))->trigger();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Lower Layer"));
    CHECK(indexOf(doc, bg) == 0);
    undo->undo();
    CHECK(indexOf(doc, bg) == 1);
    undo->undo();
    CHECK(indexOf(doc, bg) == 0);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    CHECK_FALSE(doc->moveLayer(bg, 0));  // already there
}

TEST_CASE("renaming, hiding and alpha-locking a layer in the dock are undo steps", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    LayersDock* d = w.layersWidget();
    QUndoStack* undo = doc->undoStack();
    typeAt(w, 0, 0, QLatin1Char('A'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId top = doc->activeLayer();
    typeAt(w, 0, 0, QLatin1Char('B'));
    REQUIRE(doc->canvas().at(0, 0).ch == U'B');

    QTreeWidgetItem* row = rowOf(d, top);
    REQUIRE(row);
    row->setText(LayersDock::NameColumn, QStringLiteral("  ink "));
    CHECK(doc->active()->props.name == "ink");
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Rename Layer"));
    CHECK(rowOf(d, top)->text(LayersDock::NameColumn) == QStringLiteral("ink"));
    // an empty name is refused and the row put back
    const int steps = undo->count();
    rowOf(d, top)->setText(LayersDock::NameColumn, QStringLiteral("   "));
    CHECK(undo->count() == steps);
    CHECK(rowOf(d, top)->text(LayersDock::NameColumn) == QStringLiteral("ink"));

    // hiding it shows what is under it
    rowOf(d, top)->setCheckState(LayersDock::NameColumn, Qt::Unchecked);
    CHECK_FALSE(doc->active()->props.visible);
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Hide Layer"));
    CHECK(doc->canvas().at(0, 0).ch == U'A');
    undo->undo();
    CHECK(doc->active()->props.visible);
    CHECK(rowOf(d, top)->checkState(LayersDock::NameColumn) == Qt::Checked);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    undo->undo();
    CHECK(doc->active()->props.name == "Layer 1");
    CHECK(rowOf(d, top)->text(LayersDock::NameColumn) == QStringLiteral("Layer 1"));

    // the alpha lock is the layer's, and Tool Options shows the active one's
    use(w, "halfBlock");
    QCheckBox* lock = w.toolOptions()->alphaLockBox();
    CHECK_FALSE(lock->isChecked());
    rowOf(d, top)->setCheckState(LayersDock::AlphaLockColumn, Qt::Checked);
    CHECK(doc->active()->props.alphaLocked);
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Alpha Lock On"));
    CHECK(lock->isChecked());
    CHECK(w.canvasView()->lockTransparent());
    const LayerId bg = doc->layers()[0].props.id;
    d->tree()->setCurrentItem(rowOf(d, bg));
    CHECK_FALSE(lock->isChecked());
    CHECK_FALSE(w.canvasView()->lockTransparent());
    lock->setChecked(true);
    CHECK(doc->layers()[0].props.alphaLocked);
    CHECK(rowOf(d, bg)->checkState(LayersDock::AlphaLockColumn) == Qt::Checked);
    undo->undo();
    CHECK_FALSE(doc->layers()[0].props.alphaLocked);
    CHECK_FALSE(lock->isChecked());
}

TEST_CASE("the tools draw on the active layer, and erasing shows the layer beneath", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    typeAt(w, 1, 0, QLatin1Char('a'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    typeAt(w, 0, 0, QLatin1Char('B'));
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    CHECK(doc->layers()[0].cells.at(0, 0).ch == U'A');
    CHECK(doc->layerCanvas().at(0, 0).ch == U'B');
    CHECK(doc->layerCanvas().at(1, 0) == transparentCell());
    CHECK(doc->canvas().at(1, 0).ch == U'a');  // the layer's hole shows the background

    // Delete clears the top layer's cell to transparent: the A shows again
    v->setCellCursor({0, 0});
    key(v, Qt::Key_Delete);
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0).ch == U'A');
    doc->undoStack()->undo();
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    CHECK(doc->layers()[0].cells.at(0, 0).ch == U'A');

    // the eraser (the glyph brush's right button) leaves the same hole
    use(w, "glyph");
    drag(v, {{0, 0}}, Qt::RightButton);
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0).ch == U'A');

    // on the bottom layer, erasing leaves a blank cell, as before layers
    doc->setActiveLayer(doc->layers()[0].props.id);
    CHECK(doc->blankCell() == Cell{});
    v->setCellCursor({1, 0});
    act(w, QStringLiteral("keyboardTool"))->trigger();
    key(v, Qt::Key_Delete);
    CHECK(doc->layers()[0].cells.at(1, 0) == Cell{});
    CHECK(doc->canvas().at(1, 0) == Cell{});
}

TEST_CASE("a pixel layer keeps each pixel's transparency and refuses glyphs", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 2, 0, QLatin1Char('Z'));
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(LightGreen);
    drag(v, {{2, 1}, {4, 1}});  // the bottom halves of row 0
    for (int x = 2; x <= 4; ++x) {
        INFO(x);
        CHECK(layerPixels(doc->active()->cells.at(x, 0)) == LayerPixels{std::nullopt, LightGreen});
        CHECK(doc->active()->cells.at(x, 0).transparent);
    }
    // the composite: the pixel below, the background's own above
    const auto halves = halfBlocks(doc->canvas().at(3, 0));
    REQUIRE(halves);
    CHECK(halves->bottom == LightGreen);
    CHECK(halves->top == Black);

    // a pixel layer holds no glyph: typing and a shade do nothing
    const int steps = doc->undoStack()->count();
    const Canvas before = doc->layerCanvas();
    typeAt(w, 6, 0, QLatin1Char('Q'));
    use(w, "shade");
    drag(v, {{6, 0}, {8, 0}});
    CHECK(doc->undoStack()->count() == steps);
    CHECK(doc->layerCanvas() == before);
    // the glyph brush's █ is two pixels, which it can hold
    use(w, "glyph");
    v->setForeground(Red);
    drag(v, {{6, 0}});
    CHECK(layerPixels(doc->active()->cells.at(6, 0)) == LayerPixels{Red, Red});

    // the glyph brush's eraser clears a cell of pixels back to transparent
    drag(v, {{3, 0}}, Qt::RightButton);
    CHECK(doc->active()->cells.at(3, 0) == transparentCell());
    CHECK(doc->canvas().at(3, 0) == Cell{});
}

TEST_CASE("a hidden or locked layer takes no edits", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId top = doc->activeLayer();
    REQUIRE(doc->editable());
    REQUIRE(doc->setLayerVisible(top, false));
    CHECK_FALSE(doc->editable());
    const int steps = doc->undoStack()->count();
    typeAt(w, 0, 0, QLatin1Char('X'));
    CHECK(doc->undoStack()->count() == steps);
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    doc->undoStack()->undo();
    CHECK(doc->editable());

    LayerProperties p = doc->active()->props;
    p.locked = true;
    REQUIRE(doc->setLayerProperties(top, p, QStringLiteral("Lock Layer")));
    CHECK_FALSE(doc->editable());
    typeAt(w, 0, 0, QLatin1Char('X'));
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0) == Cell{});
}

TEST_CASE("sample merged: the eyedropper and fill read the active layer or the composite", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // a red block on the background at (5, 0)
    Cell red;
    red.ch = kFullBlock;
    red.fg = Color::indexed(Red);
    REQUIRE(doc->edit(CellEdit{{{5, 0, Cell{}, red}}}));
    act(w, QStringLiteral("newPixelLayer"))->trigger();

    use(w, "eyedropper");
    QCheckBox* merged = w.toolOptions()->sampleMergedBox();
    CHECK(merged->isVisibleTo(w.toolOptions()));
    CHECK_FALSE(merged->isChecked());
    v->setForeground(White);
    mouse(v, QEvent::MouseButtonPress, 5, 0);
    mouse(v, QEvent::MouseButtonRelease, 5, 0);
    CHECK(v->pen().fg == Color::indexed(White));  // the layer is clear there
    merged->setChecked(true);
    CHECK(v->sampleMerged());
    mouse(v, QEvent::MouseButtonPress, 5, 0);
    mouse(v, QEvent::MouseButtonRelease, 5, 0);
    CHECK(v->pen().fg == Color::indexed(Red));

    // fill on the layer: merged, the red block bounds the black region
    use(w, "fill");
    CHECK(merged->isVisibleTo(w.toolOptions()));
    v->setForeground(Blue);
    drag(v, {{0, 0}});
    CHECK(layerPixels(doc->active()->cells.at(0, 0)) == LayerPixels{Blue, Blue});
    CHECK(doc->active()->cells.at(5, 0) == transparentCell());
    doc->undoStack()->undo();
    // not merged, the clear pixels of the layer are one region
    merged->setChecked(false);
    drag(v, {{0, 0}});
    CHECK(layerPixels(doc->active()->cells.at(5, 0)) == LayerPixels{Blue, Blue});
    CHECK(doc->layers()[0].cells.at(5, 0) == red);
}

TEST_CASE("shade under a pixel layer composites with no specks, drawn with the tools", "[layers][golden]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    const LayerId bg = doc->activeLayer();
    // a band of shade on the background, built up a rung a pass: ░ in
    // columns 0-2, ▒ in 3-5, ▓ in 6-9
    use(w, "shade");
    v->setForeground(Blue);
    v->setBackground(Black);
    drag(v, {{0, 0}, {9, 0}});
    drag(v, {{3, 0}, {9, 0}});
    drag(v, {{6, 0}, {9, 0}});
    const Canvas shaded = doc->layers()[0].cells;
    REQUIRE(shaded.at(0, 0).ch == kShadeLight);
    REQUIRE(shaded.at(3, 0).ch == kShadeMedium);
    REQUIRE(shaded.at(6, 0).ch == kShadeDark);

    // The cells of `shown` the line cuts that are specks: the covered half
    // must be the pixel and the free half the colour the band's shade
    // mostly shows (░ its background, ▒ its darker colour,
    // here the black background, on top, ▓ and █ their foreground), never
    // the fallback a flat plot leaves.
    const bool ice = doc->ansi().ice;
    const auto shows = [](const Cell& shade) {
        REQUIRE(shade.bg == Color::indexed(Black));
        return shade.ch == kShadeDark || shade.ch == kFullBlock ? shade.fg.paletteIndex() : std::uint8_t(Black);
    };
    const auto specks = [&](const Canvas& band, const Canvas& shown) {
        int n = 0;
        for (int x = 0; x < 10; ++x) {
            const auto halves = halfBlocks(shown.at(x, 0), ice);
            if (!halves || halves->bottom != Yellow || halves->top != shows(band.at(x, 0)))
                ++n;
        }
        return n;
    };
    // the check catches the specks: the same yellow line plotted flat into
    // the band leaves the ▓ cells' fallback, black, where blue should show
    Canvas flat = shaded;
    for (int x = 0; x < 10; ++x)
        flat.at(x, 0) = setHalfBlock(flat.at(x, 0), true, Yellow, ice);
    CHECK(specks(shaded, flat) == 4);

    // a yellow line of pixels on a pixel layer above it, cutting the band's
    // cells in half
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(Yellow);
    drag(v, {{0, 1}, {9, 1}});
    // the band is untouched: the pixels went to their own layer
    CHECK(doc->layers()[0].cells == shaded);
    const auto check = [&] {
        CHECK(specks(doc->layers()[0].cells, doc->canvas()) == 0);
        CompositeOptions o;
        o.ice = ice;
        CHECK(composite(doc->layers(), o) == doc->canvas());
    };
    check();

    // shading the background again under the pixels leaves them alone
    doc->setActiveLayer(bg);
    use(w, "shade");
    v->setForeground(LightBlue);
    drag(v, {{0, 0}, {9, 0}});
    CHECK_FALSE(doc->layers()[0].cells == shaded);
    for (int x = 0; x < 10; ++x)
        CHECK(layerPixels(doc->layers()[1].cells.at(x, 0)) == LayerPixels{std::nullopt, Yellow});
    check();
}

TEST_CASE("a drag in progress lands on its layer before the layers change", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId first = doc->activeLayer();
    use(w, "shade");
    mouse(v, QEvent::MouseButtonPress, 0, 0);
    mouse(v, QEvent::MouseMove, 4, 0);
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    mouse(v, QEvent::MouseButtonRelease, 4, 0);
    const Layer* l = doc->layers().find(first);
    REQUIRE(l);
    CHECK(l->cells.at(2, 0).ch != transparentCell().ch);
    CHECK_FALSE(l->cells.at(2, 0).transparent);
    CHECK(doc->active()->cells.at(2, 0) == transparentCell());
}

TEST_CASE("the Layer menu holds the dock's actions, with no shortcuts", "[layers]") {
    MainWindow w;
    QMenu* menu = nullptr;
    for (QAction* m : w.menuBar()->actions())
        if (m->text() == QStringLiteral("&Layer"))
            menu = m->menu();
    REQUIRE(menu);
    LayersDock* d = w.layersWidget();
    for (QAction* a : {d->newCellsAction(), d->newPixelsAction(), d->deleteAction(), d->raiseAction(),
                       d->lowerAction()}) {
        INFO(a->objectName().toStdString());
        CHECK(menu->actions().contains(a));
        CHECK(a->shortcut().isEmpty());
        CHECK_FALSE(a->toolTip().isEmpty());
    }
}

TEST_CASE("saving writes the visible composite", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    // lower case: the SAUCE record has capitals of its own
    typeAt(w, 0, 0, QLatin1Char('q'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    typeAt(w, 0, 0, QLatin1Char('z'));
    std::string bytes = doc->encode().bytes;
    CHECK(bytes.find('z') != std::string::npos);
    CHECK(bytes.find('q') == std::string::npos);
    doc->setLayerVisible(doc->activeLayer(), false);
    bytes = doc->encode().bytes;
    CHECK(bytes.find('q') != std::string::npos);
    CHECK(bytes.find('z') == std::string::npos);
}

TEST_CASE("moving a half pixel on a pixel layer keeps the pixel under its clear half", "[layers][pixels][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(Blue);
    drag(v, {{3, 0}, {3, 1}});  // both pixels of cell (3, 0)
    v->setForeground(Red);
    drag(v, {{0, 0}});  // the top pixel of cell (0, 0) only
    REQUIRE(layerPixels(doc->active()->cells.at(3, 0)) == LayerPixels{Blue, Blue});
    REQUIRE(layerPixels(doc->active()->cells.at(0, 0)) == LayerPixels{Red, std::nullopt});

    // lift the red pixel and move it onto the blue cell: it lands on top,
    // and the clear bottom half leaves the blue pixel under it
    use(w, "select");
    v->setSelection(Selection(CellRect{0, 0, 1, 1}));
    for (int i = 0; i < 3; ++i)
        key(v, Qt::Key_Right);
    key(v, Qt::Key_Return);
    CHECK(layerPixels(doc->active()->cells.at(3, 0)) == LayerPixels{Red, Blue});
    CHECK(doc->active()->cells.at(0, 0) == transparentCell());
}

TEST_CASE("the alpha lock keeps the glyph brush's erase off opaque cells and set pixels", "[layers][pixels]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    typeAt(w, 0, 0, QLatin1Char('B'));
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    use(w, "glyph");
    drag(v, {{0, 0}}, Qt::RightButton);
    // cells layer: the opaque B stays opaque under the lock
    CHECK_FALSE(doc->active()->cells.at(0, 0).transparent);
    CHECK(doc->canvas().at(0, 0).ch == U'B');

    // pixel layer: the lock is per pixel, so set pixels stay set
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(Blue);
    drag(v, {{2, 0}, {2, 1}});
    REQUIRE(layerPixels(doc->active()->cells.at(2, 0)) == LayerPixels{Blue, Blue});
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    use(w, "glyph");
    drag(v, {{2, 0}}, Qt::RightButton);
    CHECK(layerPixels(doc->active()->cells.at(2, 0)) == LayerPixels{Blue, Blue});

    // unlocked, the same erase clears the cell
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), false));
    drag(v, {{2, 0}}, Qt::RightButton);
    CHECK(doc->active()->cells.at(2, 0) == transparentCell());
}

TEST_CASE("a sample-merged fill on a cells layer changes only the region it found", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // background cell (0, 0): red top pixel, blue bottom pixel
    Cell two;
    two.ch = kUpperHalf;
    two.fg = Color::indexed(Red);
    two.bg = Color::indexed(Blue);
    REQUIRE(doc->edit(CellEdit{{{0, 0, Cell{}, two}}}));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    use(w, "fill");
    w.toolOptions()->sampleMergedBox()->setChecked(true);
    REQUIRE(v->sampleMerged());
    v->setForeground(Yellow);
    drag(v, {{0, 0}});  // the red pixel: a region of one pixel in the composite
    const auto halves = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(halves);
    CHECK(halves->top == Yellow);
    // the blue pixel was not in the region and still shows
    CHECK(halves->bottom == Blue);
    CHECK_FALSE(doc->active()->cells.at(0, 0).transparent);
    // the rest of the layer stays clear
    CHECK(doc->active()->cells.at(1, 0) == transparentCell());
}

TEST_CASE("a sample-merged fill takes nothing from the layers above the active one", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // bottom (0, 0): blue over blue
    Cell blue;
    blue.ch = kFullBlock;
    blue.fg = Color::indexed(Blue);
    REQUIRE(doc->edit(CellEdit{{{0, 0, Cell{}, blue}}}));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId mid = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId top = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Red);
    drag(v, {{0, 0}});  // a red top pixel on the top layer
    REQUIRE(layerPixels(layerById(doc, top).cells.at(0, 0)) == LayerPixels{Red, std::nullopt});
    REQUIRE(doc->setActiveLayer(mid));
    // fill the blue bottom pixel, sample merged, on the empty middle layer
    use(w, "fill");
    w.toolOptions()->sampleMergedBox()->setChecked(true);
    v->setForeground(Yellow);
    const int steps = doc->undoStack()->count();
    drag(v, {{0, 1}});
    CHECK(doc->undoStack()->count() == steps + 1);
    auto h = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Yellow);
    // only the middle layer changed
    CHECK(layerPixels(layerById(doc, top).cells.at(0, 0)) == LayerPixels{Red, std::nullopt});
    CHECK(doc->layers()[0].cells.at(0, 0) == blue);
    // the middle layer's other half is what showed under it (blue), not the
    // top layer's red copied down
    const auto mh = halfBlocks(layerById(doc, mid).cells.at(0, 0));
    REQUIRE(mh);
    INFO("middle layer cell top " << int(mh->top) << " bottom " << int(mh->bottom));
    CHECK(mh->top == Blue);
    CHECK(mh->bottom == Yellow);
    REQUIRE(doc->setLayerVisible(top, false));
    h = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(h);
    CHECK(h->top == Blue);
    CHECK(h->bottom == Yellow);
    // undo (visibility, then the fill) restores the middle layer in one step
    doc->undoStack()->undo();
    doc->undoStack()->undo();
    CHECK(layerById(doc, mid).cells.at(0, 0) == transparentCell());
    CHECK(doc->undoStack()->count() == steps + 2);
    CHECK(doc->undoStack()->index() == steps);
}

TEST_CASE("a half pixel pasted onto a cells layer lands on top and punches no hole", "[layers][pixels][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));  // bottom: A at (0, 0)
    Cell blue;
    blue.ch = kFullBlock;
    blue.fg = Color::indexed(Blue);
    REQUIRE(doc->edit(CellEdit{{{6, 0, Cell{}, blue}}}));  // bottom: blue over blue at (6, 0)
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId cells = doc->activeLayer();
    typeAt(w, 2, 0, QLatin1Char('B'));  // cells layer: B at (2, 0)
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId pixels = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Red);
    drag(v, {{5, 0}});  // a red top pixel at cell (5, 0)
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(pixels, false));
    REQUIRE(doc->setActiveLayer(cells));
    const auto pasteAt = [&](int x) {
        v->setCellCursor({x, 0});
        w.selectTool()->paste();
        w.selectTool()->stamp();
    };

    // over the cells layer's B: the pixel replaces it, B's background below
    pasteAt(2);
    const Cell& overB = layerById(doc, cells).cells.at(2, 0);
    INFO("ch " << int(overB.ch) << " transparent " << overB.transparent);
    CHECK_FALSE(overB.transparent);
    auto h = halfBlocks(doc->canvas().at(2, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Black);

    // over a clear cell of the cells layer: the pixel shows, over the blank
    // ground the bottom layer shows there
    pasteAt(4);
    CHECK_FALSE(layerById(doc, cells).cells.at(4, 0).transparent);
    h = halfBlocks(doc->canvas().at(4, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Black);

    // over a clear cell above the bottom's blue: the clear half keeps the
    // blue that showed through it
    pasteAt(6);
    h = halfBlocks(doc->canvas().at(6, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Blue);
    CHECK(doc->layers()[0].cells.at(6, 0) == blue);

    // onto the bottom layer's A: the bottom layer stays opaque
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    pasteAt(0);
    CHECK_FALSE(doc->layers()[0].cells.at(0, 0).transparent);
    h = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Black);
}

TEST_CASE("the alpha lock keeps every edit off the layer's transparency", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    typeAt(w, 3, 0, QLatin1Char('Z'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId upper = doc->activeLayer();
    typeAt(w, 0, 0, QLatin1Char('B'));
    typeAt(w, 1, 0, QLatin1Char('C'));
    REQUIRE(doc->setLayerAlphaLocked(upper, true));
    const int steps = doc->undoStack()->count();

    // clearing a selection leaves the opaque B
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    w.selectTool()->clear();
    CHECK_FALSE(doc->active()->cells.at(0, 0).transparent);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    w.selectTool()->deselect();

    // Delete and Backspace in keyboard mode leave the opaque C
    act(w, QStringLiteral("keyboardTool"))->trigger();
    v->setCellCursor({1, 0});
    key(v, Qt::Key_Delete);
    v->setCellCursor({2, 0});
    key(v, Qt::Key_Backspace);
    CHECK(doc->canvas().at(1, 0).ch == U'C');

    // typing onto a clear cell writes nothing; over an opaque one it types
    typeAt(w, 3, 0, QLatin1Char('Q'));
    CHECK(layerById(doc, upper).cells.at(3, 0) == transparentCell());
    CHECK(doc->canvas().at(3, 0).ch == U'Z');
    typeAt(w, 1, 0, QLatin1Char('D'));
    CHECK(doc->canvas().at(1, 0).ch == U'D');
    CHECK(doc->undoStack()->count() == steps + 1);

    // a paste lands on the opaque cells and not on the clear ones
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({0, 0, 2, 1});  // B D
    w.selectTool()->copy();
    w.selectTool()->deselect();
    v->setCellCursor({1, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    CHECK(doc->canvas().at(1, 0).ch == U'B');
    CHECK(layerById(doc, upper).cells.at(2, 0) == transparentCell());

    // unlocked, the same clear goes through
    REQUIRE(doc->setLayerAlphaLocked(upper, false));
    w.selectTool()->select({0, 0, 1, 1});
    w.selectTool()->clear();
    CHECK(layerById(doc, upper).cells.at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0).ch == U'A');

    // on a locked bottom layer an erase leaves a blank, which stays opaque
    w.selectTool()->deselect();
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    act(w, QStringLiteral("keyboardTool"))->trigger();
    v->setCellCursor({3, 0});
    key(v, Qt::Key_Delete);
    CHECK(doc->layers()[0].cells.at(3, 0) == Cell{});
}

TEST_CASE("with iCE off a pasted half pixel writes no bright background on a cells layer", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->setIce(false));
    const LayerId bottom = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Yellow);
    drag(v, {{2, 1}});  // bottom: a yellow bottom pixel at cell (2, 0)
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId pixels = doc->activeLayer();
    v->setForeground(LightRed);
    drag(v, {{5, 0}});  // a light red top pixel at (5, 0)
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(pixels, false));
    REQUIRE(doc->setActiveLayer(bottom));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId cells = doc->activeLayer();
    // the half-block brush's rule: the bright red goes dark, the yellow stays
    const Cell brushed = setHalfBlock(doc->canvas().at(2, 0), false, LightRed, false);
    // (a) onto the clear cell above the yellow pixel: fitLayerEdit's half-pixel path
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got = layerById(doc, cells).cells.at(2, 0);
    INFO("(a) ch " << int(got.ch) << " fg " << int(got.fg.paletteIndex()) << " bg " << int(got.bg.paletteIndex()));
    CHECK(got.bg.paletteIndex() < 8);
    CHECK(got == brushed);
    // what the save writes there (Document::encode applies iceCell)
    CHECK_FALSE(iceCell(doc->canvas().at(2, 0), false).attrs & attr::Blink);
    // (b) onto the bottom layer's own pixel cell: pasteCell's half-pixel path
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(cells, false));
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got2 = doc->layers()[0].cells.at(2, 0);
    INFO("(b) ch " << int(got2.ch) << " fg " << int(got2.fg.paletteIndex()) << " bg " << int(got2.bg.paletteIndex()));
    CHECK(got2.bg.paletteIndex() < 8);
    CHECK(got2 == brushed);
}

TEST_CASE("a pasted half pixel keeps the background a blinking letter shows under iCE", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // an iCE-off file with a blinking A on blue, opened, then iCE turned on
    REQUIRE(doc->setIce(false));
    const Cell a{U'A', Color::indexed(White), Color::indexed(Blue), attr::Blink, false};
    REQUIRE(doc->edit(CellEdit{{{2, 0, Cell{}, a}}}));
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("blink.ans"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        const std::string bytes = doc->encode().bytes;
        f.write(bytes.data(), qint64(bytes.size()));
    }
    QString err;
    REQUIRE(doc->load(path, &err));
    REQUIRE_FALSE(doc->ansi().ice);
    REQUIRE((doc->canvas().at(2, 0).attrs & attr::Blink));
    REQUIRE(doc->setIce(true));
    REQUIRE(iceCell(doc->canvas().at(2, 0), true).bg.paletteIndex() == LightBlue);  // shows on light blue
    const Cell loaded = doc->canvas().at(2, 0);
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId pixels = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Red);
    drag(v, {{5, 0}});
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(pixels, false));
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    // the half-block tools keep the background the letter shows
    REQUIRE(halfBlocks(setHalfBlock(loaded, false, Red, true), true)->bottom == LightBlue);
    const auto h = halfBlocks(iceCell(doc->canvas().at(2, 0), true), true);
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == LightBlue);
}

TEST_CASE("a move the alpha lock would cut is refused whole", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId upper = doc->activeLayer();
    typeAt(w, 0, 0, QLatin1Char('B'));
    typeAt(w, 1, 0, QLatin1Char('C'));  // upper: B C, clear from (2, 0)
    REQUIRE(doc->setLayerAlphaLocked(upper, true));
    const int steps = doc->undoStack()->count();
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    w.selectTool()->moveBy(1, 0);
    w.selectTool()->stamp();
    const auto& cells = layerById(doc, upper).cells;
    std::string row;
    for (int x = 0; x < 3; ++x)
        row += cells.at(x, 0).transparent ? '.' : char(cells.at(x, 0).ch);
    INFO("upper row after the move: " << row);
    CHECK(row == "BC.");
    CHECK(doc->undoStack()->count() == steps);
    // the selection stays on the cells that didn't move
    CHECK(v->selection().bounds().x == 0);
    CHECK(v->selection().bounds().width == 2);

    // a flip in place (B C . to . C B) would clear B's cell too
    w.selectTool()->select({0, 0, 3, 1});
    w.selectTool()->transform(Transform::FlipHorizontal);
    CHECK(cells.at(0, 0).ch == U'B');
    CHECK(cells.at(2, 0).transparent);
    CHECK(doc->undoStack()->count() == steps);
    // turned, the selection would change shape: it stays as it was
    w.selectTool()->select({0, 0, 2, 1});
    w.selectTool()->transform(Transform::RotateClockwise);
    CHECK(cells.at(1, 0).ch == U'C');
    CHECK(v->selection().bounds().width == 2);
    CHECK(doc->undoStack()->count() == steps);

    // on the locked bottom layer a lift leaves blanks, which are opaque: it moves
    w.selectTool()->deselect();
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    w.selectTool()->select({0, 0, 1, 1});
    w.selectTool()->moveBy(1, 0);
    w.selectTool()->stamp();
    CHECK(doc->layers()[0].cells.at(0, 0) == Cell{});
    CHECK(doc->layers()[0].cells.at(1, 0).ch == U'A');
}

TEST_CASE("with iCE off an opaque pixel cell pasted onto a cells layer writes no bright background", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->setIce(false));
    const LayerId bottom = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId px = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(LightRed);
    drag(v, {{5, 0}});  // top pixel
    v->setForeground(Yellow);
    drag(v, {{5, 1}});  // bottom pixel: stored as ▀ fg 12 bg 14 (iCE on), composited as ▀ fg 12 bg 6
    REQUIRE(layerById(doc, px).cells.at(5, 0).bg.paletteIndex() == Yellow);
    REQUIRE(doc->canvas().at(5, 0).bg.paletteIndex() < 8);
    const Cell shown = doc->canvas().at(5, 0);
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(px, false));
    REQUIRE(doc->setActiveLayer(bottom));
    // (a) onto the bottom layer's own cell
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got = doc->layers()[0].cells.at(2, 0);
    INFO("(a) ch " << int(got.ch) << " fg " << int(got.fg.paletteIndex()) << " bg " << int(got.bg.paletteIndex()));
    CHECK(got.bg.paletteIndex() < 8);
    // what the save writes there (Document::encode applies iceCell)
    CHECK_FALSE(iceCell(doc->canvas().at(2, 0), false).attrs & attr::Blink);
    // as the pixel layer showed it
    CHECK(got == shown);
    // (b) onto a clear cell of an upper cells layer
    w.selectTool()->deselect();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId cells = doc->activeLayer();
    v->setCellCursor({8, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got2 = layerById(doc, cells).cells.at(8, 0);
    INFO("(b) ch " << int(got2.ch) << " fg " << int(got2.fg.paletteIndex()) << " bg " << int(got2.bg.paletteIndex()));
    CHECK_FALSE(got2.transparent);
    CHECK(got2.bg.paletteIndex() < 8);
    CHECK_FALSE(iceCell(doc->canvas().at(8, 0), false).attrs & attr::Blink);
    CHECK(got2 == shown);
}

TEST_CASE("an opaque pixel cell pasted with iCE on or onto a pixel layer keeps both bright colours", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->ansi().ice);
    const LayerId bottom = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId px = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(LightRed);
    drag(v, {{5, 0}});
    v->setForeground(Yellow);
    drag(v, {{5, 1}});
    const Cell stored = layerById(doc, px).cells.at(5, 0);
    REQUIRE(stored.bg.paletteIndex() == Yellow);
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    // iCE on: the cells layer takes the cell as the pixel layer stores it
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    CHECK(doc->layers()[0].cells.at(2, 0) == stored);
    // iCE off, onto the pixel layer: both pixels keep their colours
    REQUIRE(doc->setIce(false));
    w.selectTool()->deselect();
    REQUIRE(doc->setActiveLayer(px));
    v->setCellCursor({8, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    CHECK(layerPixels(layerById(doc, px).cells.at(8, 0)) == LayerPixels{LightRed, Yellow});
}

TEST_CASE("with iCE off a pixel block flipped or turned and pasted onto a cells layer writes no blink", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->setIce(false));
    const LayerId bottom = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId px = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(LightRed);
    drag(v, {{5, 0}});
    v->setForeground(Yellow);
    drag(v, {{5, 1}});
    act(w, QStringLiteral("selectTool"))->trigger();
    const auto blinkFree = [&](int x, const char* what) {
        const Cell got = doc->layers()[0].cells.at(x, 0);
        INFO(what << ": ch " << int(got.ch) << " fg " << int(got.fg.paletteIndex()) << " bg "
                  << int(got.bg.paletteIndex()));
        CHECK(got.bg.paletteIndex() < 8);
        CHECK_FALSE(iceCell(doc->canvas().at(x, 0), false).attrs & attr::Blink);
    };
    // lifted off the pixel layer, flipped, copied from the float
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->moveBy(0, 0);
    REQUIRE(w.selectTool()->floating());
    w.selectTool()->transform(Transform::FlipVertical);
    w.selectTool()->copy();
    w.selectTool()->cancel();
    w.selectTool()->deselect();
    REQUIRE(layerPixels(layerById(doc, px).cells.at(5, 0)) == LayerPixels{LightRed, Yellow});
    REQUIRE(doc->setLayerVisible(px, false));
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    blinkFree(2, "flipped");
    // pasted, then turned while it floats on the cells layer
    REQUIRE(doc->setLayerVisible(px, true));
    REQUIRE(doc->setActiveLayer(px));
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(px, false));
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({8, 0});
    w.selectTool()->paste();
    w.selectTool()->transform(Transform::RotateClockwise);
    w.selectTool()->stamp();
    blinkFree(8, "turned");
}
