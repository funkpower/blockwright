// The Frame tool (#345) in the main window: a drag draws a frame style to
// fit, as one undo step, through the alpha lock and mirror mode.

#include "canvas_view.h"
#include "document.h"
#include "frame_tool.h"
#include "main_window.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/frame.h"
#include "blockwright/layer.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QComboBox>
#include <QMouseEvent>
#include <QUndoStack>

using namespace bw;

namespace {

void mouse(CanvasView* v, QEvent::Type type, QPoint cell, Qt::MouseButton button = Qt::LeftButton) {
    const QPointF at = v->cellRect(cell).center();
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

// A drag between two cells, through a cell between them.
void drag(CanvasView* v, QPoint from, QPoint to, Qt::MouseButton button = Qt::LeftButton) {
    mouse(v, QEvent::MouseButtonPress, from, button);
    mouse(v, QEvent::MouseMove, (from + to) / 2, button);
    mouse(v, QEvent::MouseMove, to, button);
    mouse(v, QEvent::MouseButtonRelease, to, button);
}

FrameTool* useFrame(MainWindow& w, const char* style) {
    auto* a = w.findChild<QAction*>(QStringLiteral("frameTool"));
    REQUIRE(a);
    a->trigger();
    auto* t = qobject_cast<FrameTool*>(w.tools()->active());
    REQUIRE(t);
    t->setStyle(QString::fromLatin1(style));
    REQUIRE(t->styleId() == QString::fromLatin1(style));
    t->setPenColours(false);
    return t;
}

// `base` with `frame` laid on it at (x, y), its transparent cells left.
Canvas laid(Canvas base, const Canvas& frame, int x, int y) {
    for (int fy = 0; fy < frame.height(); ++fy)
        for (int fx = 0; fx < frame.width(); ++fx)
            if (!frame.at(fx, fy).transparent && base.contains(x + fx, y + fy))
                base.at(x + fx, y + fy) = frame.at(fx, fy);
    return base;
}

} // namespace

TEST_CASE("the Frame tool draws its style to fit the drag as one undo step") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    const Canvas blank = doc->canvas();
    const FrameStyle& single = *findFrameStyle("single");
    useFrame(w, "single");

    drag(v, {9, 6}, {2, 1});
    CHECK(doc->canvas() == laid(blank, *drawFrame(single, 8, 6), 2, 1));
    CHECK(doc->undoStack()->count() == 1);
    doc->undoStack()->undo();
    CHECK(doc->canvas() == blank);

    // the open middle leaves what is there
    REQUIRE(doc->edit(CellEdit{{{4, 3, blank.at(4, 3), Cell{U'x', Color::indexed(Red), Color::indexed(Black)}}}}));
    const Canvas marked = doc->canvas();
    drag(v, {2, 1}, {9, 6});
    CHECK(doc->canvas() == laid(marked, *drawFrame(single, 8, 6), 2, 1));
    CHECK(doc->canvas().at(4, 3).ch == Glyph(U'x'));
}

TEST_CASE("a drag smaller than the style grows to its minimum, away from the press") {
    const FrameStyle& shaded = *findFrameStyle("shaded");
    const int mw = shaded.minimumWidth(), mh = shaded.minimumHeight();
    REQUIRE(mw > 2);
    CHECK(FrameTool::frameRect(shaded, {10, 10}, {11, 11}) == QRect(10, 10, mw, mh));
    CHECK(FrameTool::frameRect(shaded, {10, 10}, {9, 9}) == QRect(11 - mw, 11 - mh, mw, mh));
    CHECK(FrameTool::frameRect(shaded, {10, 10}, {10, 10}) == QRect(10, 10, mw, mh));
    CHECK(FrameTool::frameRect(shaded, {10, 10}, {30, 9}) == QRect(10, 11 - mh, 21, mh));

    MainWindow w;
    Document* doc = w.document();
    const Canvas blank = doc->canvas();
    useFrame(w, "shaded");
    drag(w.canvasView(), {12, 12}, {11, 11});
    CHECK(doc->canvas() == laid(blank, *drawFrame(shaded, mw, mh), 13 - mw, 13 - mh));
    CHECK(doc->undoStack()->count() == 1);
}

TEST_CASE("the Frame tool recolours to the pen, and the right button erases") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    const Canvas blank = doc->canvas();
    const FrameStyle& dbl = *findFrameStyle("double");
    FrameTool* t = useFrame(w, "double");
    v->setForeground(Yellow);
    v->setBackground(Blue);
    t->setPenColours(true);
    drag(v, {1, 1}, {6, 4});
    const Canvas drawn = doc->canvas();
    // a line keeps the background under it, in the pen colours too
    CHECK(drawn == laid(blank, recolourFrame(*drawFrame(dbl, 6, 4), Color::indexed(Yellow), blank.at(1, 1).bg), 1, 1));
    CHECK(drawn.at(1, 1).fg == Color::indexed(Yellow));
    CHECK(drawn.at(1, 1).bg == blank.at(1, 1).bg);

    drag(v, {1, 1}, {6, 4}, Qt::RightButton);
    CHECK(doc->canvas() == blank);
    CHECK(doc->undoStack()->count() == 2);
}

TEST_CASE("Tool Options picks the Frame tool's style and colours") {
    MainWindow w;
    FrameTool* t = useFrame(w, "single");
    QComboBox* style = w.toolOptions()->frameStyleBox();
    QComboBox* colours = w.toolOptions()->frameColoursBox();
    CHECK(style->count() == int(bundledFrameStyles().size()));
    CHECK(style->isVisibleTo(w.toolOptions()));
    CHECK_FALSE(style->itemIcon(0).isNull());
    style->setCurrentIndex(style->findData(QStringLiteral("hazard")));
    CHECK(t->styleId() == QStringLiteral("hazard"));
    colours->setCurrentIndex(colours->findData(true));
    CHECK(t->penColours());
    t->setStyle(QStringLiteral("bevel"));
    CHECK(style->currentData().toString() == QStringLiteral("bevel"));
}

TEST_CASE("the Frame tool keeps the alpha lock and repeats under mirror mode") {
    SECTION("alpha lock") {
        MainWindow w;
        Document* doc = w.document();
        doc->addLayer(LayerKind::Cells);
        useFrame(w, "single");
        // a clear layer but for one cell where the frame's corner lands
        const Cell clear = transparentCell();
        REQUIRE(doc->layerCanvas().at(3, 3) == clear);
        REQUIRE(doc->edit(CellEdit{{{2, 2, clear, Cell{}}}}));
        w.toolOptions()->alphaLockBox()->setChecked(true);
        drag(w.canvasView(), {2, 2}, {8, 6});
        CHECK(doc->layerCanvas().at(2, 2).ch == Glyph(U'┌'));
        CHECK(doc->layerCanvas().at(3, 2) == clear);
        CHECK(doc->layerCanvas().at(8, 6) == clear);
    }
    SECTION("mirror mode") {
        MainWindow w;
        Document* doc = w.document();
        const int last = doc->canvas().width() - 1;
        auto* mirror = w.findChild<QAction*>(QStringLiteral("mirrorMode"));
        REQUIRE(mirror);
        mirror->trigger();
        REQUIRE(w.canvasView()->mirror());
        useFrame(w, "single");
        drag(w.canvasView(), {1, 1}, {4, 3});
        CHECK(doc->canvas().at(1, 1).ch == Glyph(U'┌'));
        // the image across writes the corner mirrored
        CHECK(doc->canvas().at(last - 1, 1).ch == Glyph(U'┐'));
        CHECK(doc->canvas().at(last - 4, 3).ch == Glyph(U'└'));
        CHECK(doc->undoStack()->count() == 1);
    }
}

namespace {

// Rows of red, green and blue bands, dotted, on the active layer: a scene
// with no black in it for a frame to be dropped on.
void paintScene(Document* doc) {
    const Canvas& c = doc->layerCanvas();
    CellEdit e;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const int band = (x / 4) % 3;
            e.changes.push_back({x, y, c.at(x, y),
                                 Cell{(x + y) % 2 ? U'·' : U' ', Color::indexed(White),
                                      Color::indexed(band == 0 ? Red : band == 1 ? Green : Blue)}});
        }
    REQUIRE(doc->edit(e));
}

// `edit` applied to `c`.
Canvas applied(Canvas c, const CellEdit& edit) {
    for (const CellChange& ch : edit.changes)
        c.at(ch.x, ch.y) = ch.after;
    return c;
}

} // namespace

TEST_CASE("on a Pixels layer each frame style draws as one undo step or says why it can't") {
    MainWindow w;
    Document* doc = w.document();
    auto* pixels = w.findChild<QAction*>(QStringLiteral("newPixelLayer"));
    REQUIRE(pixels);
    pixels->trigger();
    REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    int drew = 0, refused = 0;
    for (const FrameStyle& s : bundledFrameStyles()) {
        INFO(s.id);
        FrameTool* t = useFrame(w, s.id.c_str());
        const Canvas layer = doc->active()->cells;
        const int steps = doc->undoStack()->index();
        const QString why = t->status();
        drag(w.canvasView(), {2, 2}, {20, 12});
        if (why.isEmpty()) {
            ++drew;
            CHECK(doc->undoStack()->index() == steps + 1);
            CHECK(doc->active()->cells != layer);
            doc->undoStack()->undo();
        } else {
            ++refused;
            CHECK(why.startsWith(QStringLiteral("The frame can't paint a Pixels layer: it holds no ")));
            CHECK(label->text() == why);
            CHECK(doc->undoStack()->index() == steps);
            CHECK(doc->active()->cells == layer);
        }
    }
    // the line and shade styles can't; heavy block, bevel and hazard can
    CHECK(drew == 3);
    CHECK(refused == int(bundledFrameStyles().size()) - 3);
    // the edges and the fill count, not only the corners
    for (const char* id : {"half-block", "button"}) {
        INFO(id);
        CHECK(useFrame(w, id)->status() ==
              QStringLiteral("The frame can't paint a Pixels layer: it holds no ") + QStringLiteral("▌"));
    }
}

TEST_CASE("a frame dropped on a scene keeps the background where its style says") {
    for (const bool pen : {false, true}) {
        INFO("pen colours " << pen);
        MainWindow w;
        CanvasView* v = w.canvasView();
        Document* doc = w.document();
        paintScene(doc);
        const Canvas scene = doc->canvas();
        v->setForeground(Yellow);
        v->setBackground(Magenta);

        // a single line writes its glyphs and colour and leaves the background
        useFrame(w, "single")->setPenColours(pen);
        drag(v, {3, 2}, {17, 9});
        const Canvas drawn = doc->canvas();
        const Canvas frame = *drawFrame(*findFrameStyle("single"), 15, 8);
        for (int y = 0; y < drawn.height(); ++y)
            for (int x = 0; x < drawn.width(); ++x) {
                INFO(x << "," << y);
                CHECK(drawn.at(x, y).bg == scene.at(x, y).bg);
                const bool on = x >= 3 && x <= 17 && y >= 2 && y <= 9 && !frame.at(x - 3, y - 2).transparent;
                if (!on) {
                    CHECK(drawn.at(x, y) == scene.at(x, y));
                    continue;
                }
                CHECK(drawn.at(x, y).ch == frame.at(x - 3, y - 2).ch);
                CHECK(drawn.at(x, y).fg == (pen ? Color::indexed(Yellow) : frame.at(x - 3, y - 2).fg));
            }
        CHECK(doc->undoStack()->count() == 2);
        doc->undoStack()->undo();
        REQUIRE(doc->canvas() == scene);

        // a button paints its own background
        useFrame(w, "button")->setPenColours(pen);
        drag(v, {3, 2}, {17, 9});
        const Canvas button = *drawFrame(*findFrameStyle("button"), 15, 8);
        CHECK(doc->canvas() ==
              laid(scene, pen ? recolourFrame(button, Color::indexed(Yellow), Color::indexed(Magenta)) : button, 3, 2));
        CHECK(doc->canvas().at(5, 5).bg == (pen ? Color::indexed(Magenta) : Color::indexed(Grey)));
        doc->undoStack()->undo();

        // the preview is the edit that lands
        for (const char* id : {"single", "half-block", "shaded", "dashed", "button", "hazard"}) {
            INFO(id);
            FrameTool* t = useFrame(w, id);
            t->setPenColours(pen);
            const Canvas before = doc->layerCanvas();
            mouse(v, QEvent::MouseButtonPress, {20, 3});
            mouse(v, QEvent::MouseMove, {30, 8});
            mouse(v, QEvent::MouseMove, {41, 14});
            REQUIRE_FALSE(t->pending().changes.empty());
            const Canvas previewed = applied(before, t->pending());
            mouse(v, QEvent::MouseButtonRelease, {41, 14});
            CHECK(doc->layerCanvas() == previewed);
            doc->undoStack()->undo();
        }
    }
}

TEST_CASE("a frame on a clear layer takes the background the scene under it shows") {
    MainWindow w;
    Document* doc = w.document();
    paintScene(doc);
    const Canvas scene = doc->canvas();
    doc->addLayer(LayerKind::Cells);
    useFrame(w, "half-block");
    drag(w.canvasView(), {4, 4}, {12, 9});
    const Canvas& layer = doc->layerCanvas();
    CHECK(layer.at(4, 4).ch == Glyph(U'▄'));
    CHECK(layer.at(4, 4).bg == scene.at(4, 4).bg);
    CHECK(layer.at(12, 6).ch == Glyph(U'▐'));
    CHECK(layer.at(12, 6).bg == scene.at(12, 6).bg);
    CHECK(layer.at(8, 6).transparent);
}

// Fills the active layer's cells in `rect` with ' ' on `bg`, opaque.
void fillLayer(Document* doc, QRect rect, int bg) {
    const Canvas& c = doc->layerCanvas();
    CellEdit e;
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        for (int x = rect.left(); x <= rect.right(); ++x)
            e.changes.push_back({x, y, c.at(x, y), Cell{U' ', Color::indexed(White), Color::indexed(bg)}});
    REQUIRE(doc->edit(e));
}

TEST_CASE("a frame on a clear layer takes no background from the layers above it") {
    MainWindow w;
    Document* doc = w.document();
    const LayerId bottom = doc->activeLayer();
    fillLayer(doc, QRect(QPoint(0, 0), QPoint(30, 15)), Red);
    const LayerId mid = doc->addLayer(LayerKind::Cells);
    const LayerId top = doc->addLayer(LayerKind::Cells);
    REQUIRE(doc->setActiveLayer(top));
    fillLayer(doc, QRect(QPoint(2, 2), QPoint(6, 10)), Blue);
    REQUIRE(doc->setActiveLayer(mid));
    FrameTool* t = useFrame(w, "single");
    CanvasView* v = w.canvasView();

    SECTION("drawn through an opaque patch on the layer above") {
        const Canvas before = doc->layerCanvas();
        mouse(v, QEvent::MouseButtonPress, {2, 2});
        mouse(v, QEvent::MouseMove, {7, 5});
        mouse(v, QEvent::MouseMove, {12, 8});
        REQUIRE_FALSE(t->pending().changes.empty());
        const Canvas previewed = applied(before, t->pending());
        mouse(v, QEvent::MouseButtonRelease, {12, 8});
        const Canvas& layer = doc->layerCanvas();
        CHECK(layer == previewed);
        CHECK(layer.at(2, 2).ch == Glyph(U'┌'));
        // every frame cell, under the patch or not, has the red below it
        for (int y = 2; y <= 8; ++y)
            for (int x = 2; x <= 12; ++x) {
                if (layer.at(x, y).transparent)
                    continue;
                INFO(x << "," << y);
                CHECK(layer.at(x, y).bg == Color::indexed(Red));
                CHECK(layer.at(x, y).bg == doc->shownBelow(x, y).bg);
            }
        // with the patch hidden, no blue shows anywhere
        REQUIRE(doc->setLayerVisible(top, false));
        const Canvas shown = doc->canvas();
        for (int y = 0; y < shown.height(); ++y)
            for (int x = 0; x < shown.width(); ++x) {
                INFO(x << "," << y);
                CHECK(shown.at(x, y).bg != Color::indexed(Blue));
            }
    }

    SECTION("a hidden layer below gives nothing, as the fill") {
        // a green layer between the red and the active one, hidden
        REQUIRE(doc->setActiveLayer(bottom));
        const LayerId green = doc->addLayer(LayerKind::Cells);
        fillLayer(doc, QRect(QPoint(0, 0), QPoint(30, 15)), Green);
        REQUIRE(doc->setLayerVisible(green, false));
        REQUIRE(doc->setActiveLayer(mid));
        useFrame(w, "single");
        drag(v, {14, 3}, {24, 9});
        const Canvas& layer = doc->layerCanvas();
        CHECK(layer.at(14, 3).ch == Glyph(U'┌'));
        CHECK(layer.at(14, 3).bg == Color::indexed(Red));
        CHECK(layer.at(24, 9).bg == Color::indexed(Red));
        CHECK(layer.at(19, 3).bg == Color::indexed(Red));
    }
}

TEST_CASE("a frame that keeps the background keeps the alpha lock") {
    MainWindow w;
    Document* doc = w.document();
    doc->addLayer(LayerKind::Cells);
    useFrame(w, "single");
    const Cell clear = transparentCell();
    // one opaque blue cell where the frame's corner lands, the rest clear
    REQUIRE(doc->edit(CellEdit{{{2, 2, clear, Cell{U'x', Color::indexed(Red), Color::indexed(Blue)}}}}));
    w.toolOptions()->alphaLockBox()->setChecked(true);
    drag(w.canvasView(), {2, 2}, {8, 6});
    CHECK(doc->layerCanvas().at(2, 2).ch == Glyph(U'┌'));
    CHECK(doc->layerCanvas().at(2, 2).bg == Color::indexed(Blue));
    CHECK(doc->layerCanvas().at(3, 2) == clear);
    CHECK(doc->layerCanvas().at(8, 6) == clear);

    SECTION("on a Pixels layer per pixel") {
        auto* pixels = w.findChild<QAction*>(QStringLiteral("newPixelLayer"));
        REQUIRE(pixels);
        w.toolOptions()->alphaLockBox()->setChecked(false);
        pixels->trigger();
        useFrame(w, "heavy");
        // a cell with only its top pixel set where the corner lands
        const Cell top = setLayerPixel(transparentCell(), false, Cyan);
        REQUIRE(doc->edit(CellEdit{{{2, 2, doc->layerCanvas().at(2, 2), top}}}));
        w.toolOptions()->alphaLockBox()->setChecked(true);
        const int steps = doc->undoStack()->count();
        drag(w.canvasView(), {2, 2}, {8, 6});
        // █ would set the bottom pixel, which the lock keeps clear
        CHECK(doc->layerCanvas().at(2, 2) == top);
        CHECK(doc->layerCanvas().at(3, 2).transparent);
        CHECK(doc->undoStack()->count() == steps);
    }
}
