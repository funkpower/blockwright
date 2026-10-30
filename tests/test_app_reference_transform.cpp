// Moving and resizing a reference layer (#178): Alt+drag, or a plain drag
// with the Move/Transform tool, moves it by canvas pixels with a live
// preview and one undo step; the tool's corner handles resize it, the
// aspect kept unless Shift is held; Tool Options takes the place, size and
// scale as numbers. It still takes no paint and no export sees it.

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "system_input.h"
#include "tool.h"
#include "tool_options.h"
#include "transform_options.h"

#include "blockwright/composite.h"
#include "blockwright/project.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace bw;

namespace {

std::shared_ptr<const std::string> encoded(const QImage& image) {
    QByteArray bytes;
    QBuffer out(&bytes);
    REQUIRE(out.open(QIODevice::WriteOnly));
    REQUIRE(image.save(&out, "PNG"));
    return std::make_shared<const std::string>(bytes.toStdString());
}

QImage filled(int w, int h, QColor c) {
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(c);
    return image;
}

// A left-button event at view pixel `at` (the default zoom is 2: a canvas
// pixel is two view pixels).
void mouse(CanvasView* v, QEvent::Type type, QPoint at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const QPointF p = v->toWidget(QRect(at, QSize(1, 1))).center();
    QMouseEvent e(type, p, v->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(v, &e);
}

void drag(CanvasView* v, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    mouse(v, QEvent::MouseButtonPress, from, mods);
    mouse(v, QEvent::MouseMove, to, mods);
    mouse(v, QEvent::MouseButtonRelease, to, mods);
}

bool tablet(CanvasView* v, QEvent::Type type, QPoint at, Qt::KeyboardModifiers mods) {
    const QPointF p = v->toWidget(QRect(at, QSize(1, 1))).center();
    QTabletEvent e(type, bwtest::systemStylus(), p, v->mapToGlobal(p), type == QEvent::TabletRelease ? 0.0 : 1.0, 0,
                   0, 0, 0, 0, mods, type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                   type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
    e.setAccepted(false);
    QApplication::sendEvent(v, &e);
    return e.isAccepted();
}

QRect place(const Document* d, LayerId id) {
    const ReferenceImage& r = *d->layers().find(id)->props.reference;
    return {r.x, r.y, r.width, r.height};
}

// The colour the view shows at canvas pixel `at`.
QColor shown(CanvasView* v, QPoint at) {
    const QImage image = v->grab().toImage();
    const QPoint p = v->toWidget(QRect(at * v->zoom(), QSize(1, 1))).topLeft();
    return image.pixelColor(p);
}

// A red 40x20 picture at (8, 8), 80x40 canvas pixels, fully opaque.
LayerId addPicture(Document* d) {
    const LayerId id = d->addReferenceLayer(ReferenceImage{encoded(filled(40, 20, Qt::red)), 8, 8, 80, 40, 100});
    REQUIRE(id);
    REQUIRE(d->activeLayer() == id);
    return id;
}

// The size a scale gives a picture whose own size is `own`, worked out as
// Tool Options does: both sides rounded, kept under the limit with the
// picture's aspect, and at least 1 px.
QSize scaledSize(QSize own, double percent) {
    constexpr int top = ReferenceImage::kMaxPlace;
    const double s = std::min(percent, 100.0 * top / std::max(own.width(), own.height())) / 100;
    const auto side = [](double v) { return int(std::clamp<long long>(std::llround(v), 1, top)); };
    return {side(own.width() * s), side(own.height() * s)};
}

// Types `text` over the number a spin box shows, as at the keyboard, and
// leaves it there, not committed.
void type(QAbstractSpinBox* box, const QString& text) {
    box->selectAll();
    for (const QChar c : text) {
        QKeyEvent e(QEvent::KeyPress, c.unicode(), Qt::NoModifier, QString(c));
        QApplication::sendEvent(box, &e);
    }
}

void press(QWidget* w, int key) {
    QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

} // namespace

TEST_CASE("Alt+drag moves a reference layer by canvas pixels, previewed, one Move Layer step", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(v->zoom() == 2);
    const LayerId id = addPicture(d);
    const int steps = d->undoStack()->count();
    const auto art = [d] { return composite(d->project().layers, CompositeOptions{.ice = d->project().ice}); };
    const Canvas before = art();

    // canvas pixel (20, 20) to (30, 25): 10 right, 5 down
    mouse(v, QEvent::MouseButtonPress, {40, 40}, Qt::AltModifier);
    mouse(v, QEvent::MouseMove, {60, 50}, Qt::AltModifier);
    // the view shows it there; the document has it only once it lands
    CHECK(place(d, id) == QRect(8, 8, 80, 40));
    CHECK(v->referenceFrame() == QRect(36, 26, 160, 80));
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("Move Layer: dx 10, dy 5 | x 18, y 13"));
    CHECK(shown(v, {95, 30}) == QColor(Qt::red));  // past the old right edge
    CHECK(shown(v, {10, 10}) != QColor(Qt::red));  // the old top-left corner
    mouse(v, QEvent::MouseButtonRelease, {60, 50}, Qt::AltModifier);

    CHECK(place(d, id) == QRect(18, 13, 80, 40));
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Move Layer"));
    CHECK(w.statusBar()->currentMessage().isEmpty());
    CHECK(shown(v, {95, 30}) == QColor(Qt::red));
    // no paint, no export: the art is what it was
    CHECK(art() == before);

    d->undoStack()->undo();
    CHECK(place(d, id) == QRect(8, 8, 80, 40));
    CHECK(shown(v, {95, 30}) != QColor(Qt::red));
    d->undoStack()->redo();
    CHECK(place(d, id) == QRect(18, 13, 80, 40));

    // a drag that ends where it started is no step
    drag(v, {40, 40}, {40, 40}, Qt::AltModifier);
    CHECK(d->undoStack()->count() == steps + 1);
}

TEST_CASE("Esc during a reference layer's move drops it with no edit", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    const LayerId id = addPicture(d);
    const int steps = d->undoStack()->count();

    mouse(v, QEvent::MouseButtonPress, {40, 40}, Qt::AltModifier);
    mouse(v, QEvent::MouseMove, {80, 60}, Qt::AltModifier);
    CHECK(v->referenceFrame() == QRect(56, 36, 160, 80));
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::AltModifier);
    QApplication::sendEvent(v, &esc);
    CHECK(v->referenceFrame() == QRect(16, 16, 160, 80));
    CHECK(w.statusBar()->currentMessage().isEmpty());
    mouse(v, QEvent::MouseMove, {100, 60}, Qt::AltModifier);
    mouse(v, QEvent::MouseButtonRelease, {100, 60}, Qt::AltModifier);
    CHECK(place(d, id) == QRect(8, 8, 80, 40));
    CHECK(d->undoStack()->count() == steps);
}

TEST_CASE("the Move/Transform tool's plain drag moves the active layer", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    w.tools()->setActive(QStringLiteral("move"));
    REQUIRE(v->tool()->movesLayer());

    // an art layer, by whole cells
    const LayerId art = d->activeLayer();
    drag(v, v->cellRect(QPoint(1, 1)).center(), v->cellRect(QPoint(4, 2)).center());
    CHECK(d->layers().find(art)->props.x == 3);
    CHECK(d->layers().find(art)->props.y == 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Move Layer"));

    // a reference layer, by canvas pixels
    const LayerId id = addPicture(d);
    drag(v, {40, 40}, {47, 43});
    CHECK(place(d, id) == QRect(11, 9, 80, 40));
    CHECK(d->undoStack()->undoText() == QStringLiteral("Move Layer"));

    // locked or hidden, it refuses, as Alt+drag does
    LayerProperties props = d->layers().find(id)->props;
    props.locked = true;
    REQUIRE(d->setLayerProperties(id, props, QStringLiteral("Lock")));
    const int steps = d->undoStack()->count();
    mouse(v, QEvent::MouseButtonPress, {40, 40});
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("Cannot move layer: locked"));
    mouse(v, QEvent::MouseMove, {60, 60});
    mouse(v, QEvent::MouseButtonRelease, {60, 60});
    CHECK(place(d, id) == QRect(11, 9, 80, 40));
    CHECK(d->undoStack()->count() == steps);
}

TEST_CASE("a corner handle resizes a reference layer, its aspect kept unless Shift is held", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    const LayerId id = addPicture(d);
    w.tools()->setActive(QStringLiteral("move"));
    // the frame's corners, in view pixels: (16, 16) and (176, 96)
    CHECK(v->referenceHandleAt({176, 96}) == 2);
    CHECK(v->referenceHandleAt({16 - CanvasView::kHandle / 2, 16}) == 0);
    CHECK(v->referenceHandleAt({175, 17}) == 1);
    CHECK(v->referenceHandleAt({17, 95}) == 3);
    CHECK(v->referenceHandleAt({96, 56}) == -1);
    const int steps = d->undoStack()->count();

    SECTION("kept") {
        // the bottom-right corner 20 right and 2 down: the width leads
        mouse(v, QEvent::MouseButtonPress, {176, 96});
        mouse(v, QEvent::MouseMove, {216, 100});
        CHECK(place(d, id) == QRect(8, 8, 80, 40));
        CHECK(v->referenceFrame() == QRect(16, 16, 200, 100));
        CHECK(w.statusBar()->currentMessage() == QStringLiteral("Resize Layer: w 100, h 50 | x 8, y 8"));
        mouse(v, QEvent::MouseButtonRelease, {216, 100});
        CHECK(place(d, id) == QRect(8, 8, 100, 50));
        CHECK(d->undoStack()->count() == steps + 1);
        CHECK(d->undoStack()->undoText() == QStringLiteral("Resize Layer"));

        // the top-left corner 10 left: the bottom-right one stays put
        drag(v, {16, 16}, {-4, 16});
        CHECK(place(d, id) == QRect(-2, 3, 110, 55));

        d->undoStack()->undo();
        CHECK(place(d, id) == QRect(8, 8, 100, 50));
        d->undoStack()->undo();
        CHECK(place(d, id) == QRect(8, 8, 80, 40));
        d->undoStack()->redo();
        CHECK(place(d, id) == QRect(8, 8, 100, 50));
    }
    SECTION("free with Shift") {
        drag(v, {176, 96}, {216, 100}, Qt::ShiftModifier);
        CHECK(place(d, id) == QRect(8, 8, 100, 42));
        CHECK(d->undoStack()->undoText() == QStringLiteral("Resize Layer"));
        // the top-right corner up and left, past nothing
        drag(v, {216, 16}, {196, 6}, Qt::ShiftModifier);
        CHECK(place(d, id) == QRect(8, 3, 90, 47));
        // dragged past the anchor, it stays a pixel wide
        drag(v, {196, 6}, {0, 6}, Qt::ShiftModifier);
        CHECK(place(d, id) == QRect(8, 3, 1, 47));
    }
    SECTION("Shift pressed mid-resize frees the aspect at once, and the release lands what is shown") {
        mouse(v, QEvent::MouseButtonPress, {176, 96});
        mouse(v, QEvent::MouseMove, {216, 100});
        CHECK(v->referenceFrame() == QRect(16, 16, 200, 100));
        QKeyEvent shift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QApplication::sendEvent(v, &shift);
        CHECK(v->referenceFrame() == QRect(16, 16, 200, 84));
        CHECK(w.statusBar()->currentMessage() == QStringLiteral("Resize Layer: w 100, h 42 | x 8, y 8"));
        mouse(v, QEvent::MouseButtonRelease, {216, 100}, Qt::ShiftModifier);
        CHECK(place(d, id) == QRect(8, 8, 100, 42));
        CHECK(d->undoStack()->count() == steps + 1);
    }
    SECTION("Shift let go mid-resize keeps the aspect again at once") {
        mouse(v, QEvent::MouseButtonPress, {176, 96}, Qt::ShiftModifier);
        mouse(v, QEvent::MouseMove, {216, 100}, Qt::ShiftModifier);
        CHECK(v->referenceFrame() == QRect(16, 16, 200, 84));
        QKeyEvent shift(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QApplication::sendEvent(v, &shift);
        CHECK(v->referenceFrame() == QRect(16, 16, 200, 100));
        mouse(v, QEvent::MouseButtonRelease, {216, 100});
        CHECK(place(d, id) == QRect(8, 8, 100, 50));
    }
    SECTION("Esc drops a resize") {
        mouse(v, QEvent::MouseButtonPress, {176, 96});
        mouse(v, QEvent::MouseMove, {300, 300});
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(v, &esc);
        mouse(v, QEvent::MouseButtonRelease, {300, 300});
        CHECK(place(d, id) == QRect(8, 8, 80, 40));
        CHECK(d->undoStack()->count() == steps);
    }
    SECTION("only the Move/Transform tool has handles: Alt+drag on a corner moves") {
        w.tools()->setActive(QStringLiteral("line"));
        drag(v, {176, 96}, {216, 100}, Qt::AltModifier);
        CHECK(place(d, id) == QRect(28, 10, 80, 40));
    }
}

TEST_CASE("a stylus Alt+drag moves a reference layer", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    const LayerId id = addPicture(d);
    CHECK(tablet(v, QEvent::TabletPress, {40, 40}, Qt::AltModifier));
    CHECK(tablet(v, QEvent::TabletMove, {50, 46}, Qt::AltModifier));
    CHECK(v->referenceFrame() == QRect(26, 22, 160, 80));
    CHECK(tablet(v, QEvent::TabletRelease, {50, 46}, Qt::AltModifier));
    CHECK(place(d, id) == QRect(13, 11, 80, 40));
    CHECK(d->undoStack()->undoText() == QStringLiteral("Move Layer"));
}

TEST_CASE("Tool Options places and sizes a reference layer by numbers", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    auto* options = w.findChild<ToolOptions*>();
    REQUIRE(options);
    TransformOptions* t = options->transformOptions();
    REQUIRE(t);
    w.tools()->setActive(QStringLiteral("move"));
    CHECK(options->shown() == ToolOptions::Options(ToolOptions::Option::Transform));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("line")).testFlag(ToolOptions::Option::Transform));

    // an art layer: nothing to place by numbers
    CHECK_FALSE(t->widthBox()->isEnabled());
    CHECK_FALSE(t->scaleBox()->isEnabled());

    const LayerId id = addPicture(d);
    CHECK(t->widthBox()->isEnabled());
    CHECK(t->xBox()->value() == 8);
    CHECK(t->yBox()->value() == 8);
    CHECK(t->widthBox()->value() == 80);
    CHECK(t->heightBox()->value() == 40);
    CHECK(t->keepAspectBox()->isChecked());
    CHECK(t->scaleBox()->value() == 200.0);  // 80 px across a 40 px picture
    int steps = d->undoStack()->count();

    t->xBox()->setValue(-5);
    CHECK(place(d, id) == QRect(-5, 8, 80, 40));
    CHECK(d->undoStack()->count() == ++steps);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Move Layer"));

    // the aspect kept: the height follows
    t->widthBox()->setValue(120);
    CHECK(place(d, id) == QRect(-5, 8, 120, 60));
    CHECK(d->undoStack()->count() == ++steps);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Resize Layer"));
    CHECK(t->heightBox()->value() == 60);
    CHECK(t->scaleBox()->value() == 300.0);

    // free: only the side set changes
    t->keepAspectBox()->setChecked(false);
    t->heightBox()->setValue(10);
    CHECK(place(d, id) == QRect(-5, 8, 120, 10));
    CHECK(d->undoStack()->count() == ++steps);

    // a scale sizes both sides from the picture's own
    t->scaleBox()->setValue(50);
    CHECK(place(d, id) == QRect(-5, 8, 20, 10));
    CHECK(d->undoStack()->count() == ++steps);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Resize Layer"));

    // undo and redo, the fields following
    d->undoStack()->undo();
    CHECK(place(d, id) == QRect(-5, 8, 120, 10));
    CHECK(t->widthBox()->value() == 120);
    d->undoStack()->redo();
    CHECK(t->widthBox()->value() == 20);
    CHECK(t->scaleBox()->value() == 50.0);

    // a drag shows in the fields once it lands
    drag(w.canvasView(), {0, 20}, {10, 24});
    CHECK(t->xBox()->value() == 0);
    CHECK(t->yBox()->value() == 10);

    // locked: off, with a word on why
    LayerProperties props = d->layers().find(id)->props;
    props.locked = true;
    REQUIRE(d->setLayerProperties(id, props, QStringLiteral("Lock")));
    CHECK_FALSE(t->xBox()->isEnabled());
    CHECK(t->noteLabel()->text() == QStringLiteral("The reference layer is locked."));
}

TEST_CASE("Keep aspect holds one aspect across numeric edits, not each rounded size", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    const LayerId id = addPicture(d);  // 80x40, a 40x20 picture
    REQUIRE(t->keepAspectBox()->isChecked());

    SECTION("width there and back") {
        t->widthBox()->setValue(79);
        CHECK(place(d, id) == QRect(8, 8, 79, 40));
        t->widthBox()->setValue(80);
        CHECK(place(d, id) == QRect(8, 8, 80, 40));
    }
    SECTION("ten width steps") {
        for (int width = 81; width <= 90; ++width)
            t->widthBox()->setValue(width);
        CHECK(place(d, id) == QRect(8, 8, 90, 45));
    }
    SECTION("height there and back, and ten height steps, after a rounded width") {
        t->widthBox()->setValue(79);
        t->heightBox()->setValue(41);
        t->heightBox()->setValue(40);
        CHECK(place(d, id) == QRect(8, 8, 80, 40));
        t->widthBox()->setValue(79);
        for (int height = 41; height <= 50; ++height)
            t->heightBox()->setValue(height);
        CHECK(place(d, id) == QRect(8, 8, 100, 50));
    }
    SECTION("scale steps, then width there and back") {
        t->widthBox()->setValue(79);
        for (int percent = 201; percent <= 210; ++percent)
            t->scaleBox()->setValue(percent);
        CHECK(place(d, id) == QRect(8, 8, 84, 42));
        t->widthBox()->setValue(83);
        t->widthBox()->setValue(84);
        CHECK(place(d, id) == QRect(8, 8, 84, 42));
    }
    SECTION("a size stretched on purpose keeps its own aspect") {
        t->keepAspectBox()->setChecked(false);
        t->heightBox()->setValue(50);
        t->keepAspectBox()->setChecked(true);
        t->widthBox()->setValue(79);
        t->widthBox()->setValue(80);
        CHECK(place(d, id) == QRect(8, 8, 80, 50));
        for (int width = 81; width <= 96; ++width)
            t->widthBox()->setValue(width);
        CHECK(place(d, id) == QRect(8, 8, 96, 60));
    }
    SECTION("a stretch cut to the limit on both sides keeps its own aspect") {
        constexpr int top = ReferenceImage::kMaxPlace;
        t->keepAspectBox()->setChecked(false);
        t->widthBox()->setValue(top);
        t->heightBox()->setValue(top);
        REQUIRE(place(d, id) == QRect(8, 8, top, top));
        t->keepAspectBox()->setChecked(true);
        t->widthBox()->setValue(top - 1);
        CHECK(place(d, id) == QRect(8, 8, top - 1, top - 1));
    }
    SECTION("a free height after a kept width: the next kept edit takes the new shape") {
        t->widthBox()->setValue(100);
        REQUIRE(place(d, id) == QRect(8, 8, 100, 50));
        t->keepAspectBox()->setChecked(false);
        t->heightBox()->setValue(100);
        t->keepAspectBox()->setChecked(true);
        t->widthBox()->setValue(120);
        CHECK(place(d, id) == QRect(8, 8, 120, 120));
    }
    SECTION("a free width after a kept height: the next kept edit takes the new shape") {
        t->heightBox()->setValue(50);
        REQUIRE(place(d, id) == QRect(8, 8, 100, 50));
        t->keepAspectBox()->setChecked(false);
        t->widthBox()->setValue(50);
        t->keepAspectBox()->setChecked(true);
        t->heightBox()->setValue(60);
        CHECK(place(d, id) == QRect(8, 8, 60, 60));
    }
    SECTION("a scale that moves no pixel still takes the picture's own aspect back") {
        t->keepAspectBox()->setChecked(false);
        t->heightBox()->setValue(41);  // 80x41, stretched
        t->keepAspectBox()->setChecked(true);
        t->widthBox()->setValue(79);  // kept to 80/41
        REQUIRE(place(d, id) == QRect(8, 8, 79, 40));
        const int steps = d->undoStack()->count();
        REQUIRE(t->scaleBox()->value() == 198.0);
        t->scaleBox()->setValue(198.5);  // still 79x40
        CHECK(place(d, id) == QRect(8, 8, 79, 40));
        CHECK(d->undoStack()->count() == steps);
        t->widthBox()->setValue(160);
        CHECK(place(d, id) == QRect(8, 8, 160, 80));
    }
    SECTION("undo and redo: the next kept edit takes the size they leave") {
        t->keepAspectBox()->setChecked(false);
        t->heightBox()->setValue(41);  // 80x41, stretched
        t->keepAspectBox()->setChecked(true);
        t->widthBox()->setValue(81);  // kept to 80/41
        REQUIRE(place(d, id) == QRect(8, 8, 81, 42));
        d->undoStack()->undo();
        d->undoStack()->redo();
        REQUIRE(place(d, id) == QRect(8, 8, 81, 42));
        t->widthBox()->setValue(160);  // 81/42 now, not 80/41
        CHECK(place(d, id) == QRect(8, 8, 160, 83));
    }
    SECTION("another layer: its own shape, and the first one's again on the way back") {
        t->widthBox()->setValue(100);
        REQUIRE(place(d, id) == QRect(8, 8, 100, 50));
        const LayerId square =
            d->addReferenceLayer(ReferenceImage{encoded(filled(40, 20, Qt::blue)), 0, 0, 80, 80, 100});
        REQUIRE(d->activeLayer() == square);
        t->widthBox()->setValue(40);
        CHECK(place(d, square) == QRect(0, 0, 40, 40));
        REQUIRE(d->setActiveLayer(id));
        t->widthBox()->setValue(120);
        CHECK(place(d, id) == QRect(8, 8, 120, 60));
    }
}

TEST_CASE("Scale's arrow steps add up through sizes that round the same", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    const LayerId id = addPicture(d);  // 80x40, a 40x20 picture
    REQUIRE(t->scaleBox()->value() == 200.0);
    const int steps = d->undoStack()->count();

    SECTION("up") {
        t->scaleBox()->stepUp();  // 201% is still 80x40
        CHECK(place(d, id) == QRect(8, 8, 80, 40));
        CHECK(t->scaleBox()->value() == 201.0);
        CHECK(d->undoStack()->count() == steps);
        for (int i = 1; i < 10; ++i)
            t->scaleBox()->stepUp();
        CHECK(place(d, id) == QRect(8, 8, 84, 42));
        CHECK(t->scaleBox()->value() == 210.0);
    }
    SECTION("down") {
        for (int i = 0; i < 10; ++i)
            t->scaleBox()->stepDown();
        CHECK(place(d, id) == QRect(8, 8, 76, 38));
        CHECK(t->scaleBox()->value() == 190.0);
    }
    SECTION("a width or height edit shows its own scale") {
        t->scaleBox()->stepUp();
        REQUIRE(t->scaleBox()->value() == 201.0);
        t->widthBox()->setValue(100);
        CHECK(place(d, id) == QRect(8, 8, 100, 50));
        CHECK(t->scaleBox()->value() == 250.0);
        t->scaleBox()->stepUp();  // 251% is still 100x50
        CHECK(t->scaleBox()->value() == 251.0);
        t->heightBox()->setValue(50);  // no change: the scale asked for stays
        CHECK(t->scaleBox()->value() == 251.0);
        t->heightBox()->setValue(30);
        CHECK(place(d, id) == QRect(8, 8, 60, 30));
        CHECK(t->scaleBox()->value() == 150.0);
    }
    SECTION("undo, a move or another layer shows the size's scale") {
        t->scaleBox()->stepUp();
        t->scaleBox()->stepUp();  // 202%: 81x40
        REQUIRE(place(d, id) == QRect(8, 8, 81, 40));
        REQUIRE(t->scaleBox()->value() == 202.0);
        t->xBox()->setValue(9);
        CHECK(t->scaleBox()->value() == 202.0);  // 202.5% would be 81x41
        d->undoStack()->undo();
        d->undoStack()->undo();
        REQUIRE(place(d, id) == QRect(8, 8, 80, 40));
        CHECK(t->scaleBox()->value() == 200.0);
        t->scaleBox()->stepUp();
        REQUIRE(t->scaleBox()->value() == 201.0);
        const LayerId other =
            d->addReferenceLayer(ReferenceImage{encoded(filled(40, 20, Qt::blue)), 0, 0, 80, 40, 100});
        REQUIRE(d->activeLayer() == other);
        CHECK(t->scaleBox()->value() == 200.0);
    }
}

TEST_CASE("Scale's arrow steps keep going the way they point where a side is at its limit", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    constexpr int top = ReferenceImage::kMaxPlace;
    const auto size = [d] {
        const ReferenceImage& r = *d->active()->props.reference;
        return QSize(r.width, r.height);
    };

    SECTION("a side floored at 1 px") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(1, 100, Qt::red)), 0, 0, 1, 100, 100}));
        t->scaleBox()->setValue(50);
        REQUIRE(size() == QSize(1, 50));
        t->scaleBox()->stepDown();
        CHECK(size() == QSize(1, 49));
        CHECK(t->scaleBox()->value() == 49.0);
        t->scaleBox()->stepDown();
        CHECK(size() == QSize(1, 48));
        CHECK(t->scaleBox()->value() == 48.0);
    }
    SECTION("a side cut to the limit") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(1, 100, Qt::red)), 0, 0, 1, 100, 100}));
        t->scaleBox()->setValue(1e8);
        REQUIRE(size() == QSize(167772, top));
        CHECK(t->scaleBox()->value() == 100.0 * top / 100);  // the scale there is, not past it
        const int steps = d->undoStack()->count();
        t->scaleBox()->stepUp();
        CHECK(size() == QSize(167772, top));
        CHECK(d->undoStack()->count() == steps);
        CHECK(t->scaleBox()->value() == 100.0 * top / 100);
        t->scaleBox()->stepDown();
        CHECK(size() == QSize(167772, top - 1));
    }
    SECTION("a kept width or height cut where the other side meets the limit") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(100, 1, Qt::red)), 0, 0, 100, 1, 100}));
        REQUIRE(t->keepAspectBox()->isChecked());
        t->heightBox()->setValue(top - 1);
        CHECK(size() == QSize(top, 167772));
        CHECK(t->heightBox()->value() == 167772);
        t->scaleBox()->stepUp();
        CHECK(size() == QSize(top, 167772));
        t->widthBox()->setValue(200);
        CHECK(size() == QSize(200, 2));
    }
}

TEST_CASE("Scale's arrows step from the size after a move, undo or redo", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    const auto size = [d] {
        const ReferenceImage& r = *d->active()->props.reference;
        return QSize(r.width, r.height);
    };
    const auto add = [&](QSize own) {
        REQUIRE(d->addReferenceLayer(
            ReferenceImage{encoded(filled(own.width(), own.height(), Qt::red)), 0, 0, own.width(), own.height(), 100}));
        REQUIRE(t->keepAspectBox()->isChecked());
    };

    SECTION("3x7 at 80%, then a move: a step up shrinks no side") {
        add({3, 7});
        t->scaleBox()->setValue(80);
        REQUIRE(size() == QSize(2, 6));
        t->xBox()->setValue(1);
        CHECK(scaledSize({3, 7}, t->scaleBox()->value()) == QSize(2, 6));  // not 66.67%, which is 2x5
        CHECK(t->scaleBox()->value() == 81.0);
        CHECK(t->scaleBox()->prefix().isEmpty());
        t->scaleBox()->stepUp();
        CHECK(size().width() >= 2);
        CHECK(size().height() >= 6);
    }
    SECTION("7x3 at 80%, then a move: a step down grows no side") {
        add({7, 3});
        t->scaleBox()->setValue(80);
        REQUIRE(size() == QSize(6, 2));
        t->xBox()->setValue(1);
        CHECK(scaledSize({7, 3}, t->scaleBox()->value()) == QSize(6, 2));  // not 85.71%, which is 6x3
        t->scaleBox()->stepDown();
        CHECK(size().width() <= 6);
        CHECK(size().height() <= 2);
    }
    SECTION("undo and redo show a scale that gives the size") {
        add({3, 7});
        t->scaleBox()->setValue(80);
        t->scaleBox()->setValue(90);
        REQUIRE(size() == QSize(3, 6));
        d->undoStack()->undo();
        REQUIRE(size() == QSize(2, 6));
        CHECK(scaledSize({3, 7}, t->scaleBox()->value()) == QSize(2, 6));
        t->scaleBox()->stepUp();
        CHECK(size().width() >= 2);
        CHECK(size().height() >= 6);
        d->undoStack()->redo();
        REQUIRE(size() == QSize(3, 6));
        CHECK(scaledSize({3, 7}, t->scaleBox()->value()) == QSize(3, 6));
    }
}

TEST_CASE("Keep aspect takes any size a scale gives as the picture's own, a move between or not", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    constexpr int top = ReferenceImage::kMaxPlace;
    const auto size = [d] {
        const ReferenceImage& r = *d->active()->props.reference;
        return QSize(r.width, r.height);
    };
    const auto add = [&](QSize own, const std::shared_ptr<const std::string>& bytes) {
        const LayerId id = d->addReferenceLayer(ReferenceImage{bytes, 0, 0, own.width(), own.height(), 100});
        REQUIRE(id);
        REQUIRE(t->keepAspectBox()->isChecked());
        return id;
    };

    SECTION("3x7 at 80%, then a move: a kept width of 3 is 3x7") {
        add({3, 7}, encoded(filled(3, 7, Qt::red)));
        t->scaleBox()->setValue(80);
        REQUIRE(size() == QSize(2, 6));
        t->xBox()->setValue(1);
        t->widthBox()->setValue(3);
        CHECK(size() == QSize(3, 7));
    }
    SECTION("7x3 at 80%, then a move: a kept height of 3 is 7x3") {
        add({7, 3}, encoded(filled(7, 3, Qt::red)));
        t->scaleBox()->setValue(80);
        REQUIRE(size() == QSize(6, 2));
        t->xBox()->setValue(1);
        t->heightBox()->setValue(3);
        CHECK(size() == QSize(7, 3));
    }
    // Codex round 9: 5 * 0.7 is 3.5 but 45 * 0.7 is 31.499999999999996, so
    // 70% gives 4x31, which no exact scale rounds to
    SECTION("5x45 at 70%, then a move: a kept width of 5 is 5x45") {
        add({5, 45}, encoded(filled(5, 45, Qt::red)));
        t->scaleBox()->setValue(70);
        REQUIRE(size() == QSize(4, 31));
        t->xBox()->setValue(1);
        t->widthBox()->setValue(5);
        CHECK(size() == QSize(5, 45));
    }
    SECTION("5x45 at 70%, undone and redone: a kept width of 5 is 5x45") {
        add({5, 45}, encoded(filled(5, 45, Qt::red)));
        t->scaleBox()->setValue(70);
        REQUIRE(size() == QSize(4, 31));
        d->undoStack()->undo();
        REQUIRE(size() == QSize(5, 45));
        d->undoStack()->redo();
        REQUIRE(size() == QSize(4, 31));
        t->widthBox()->setValue(5);
        CHECK(size() == QSize(5, 45));
    }
    SECTION("45x5 at 70%, then a move: a kept height of 5 is 45x5") {
        add({45, 5}, encoded(filled(45, 5, Qt::red)));
        t->scaleBox()->setValue(70);
        REQUIRE(size() == QSize(31, 4));
        t->xBox()->setValue(1);
        t->heightBox()->setValue(5);
        CHECK(size() == QSize(45, 5));
    }
    SECTION("after a scale and a move, a kept edit back to the picture's own side gives its own size") {
        // Every size scaled()'s own arithmetic gives: the scales where a side
        // lands within a hair of a half pixel, and a spread between.
        std::vector<QSize> owns = {QSize(5, 45), QSize(45, 5), QSize(3, 7),    QSize(7, 3),
                                   QSize(1, 45), QSize(45, 1), QSize(99, 101), QSize(333, 200)};
        for (const int a : {2, 9, 35, 64, 199})
            for (const int b : {1, 13, 55, 100, 301})
                owns.insert(owns.end(), {QSize(a, b), QSize(b, a)});
        for (const QSize own : owns) {
            std::vector<double> scales;
            for (int k = 1; k <= 30000; ++k) {
                const double p = k / 100.0;
                bool half = k % 737 == 0;
                for (const int n : {own.width(), own.height()}) {
                    const double v = n * (p / 100);
                    half = half || std::abs(v - std::floor(v) - 0.5) < 1e-9;
                }
                if (half)
                    scales.push_back(p);
            }
            const LayerId id = add(own, encoded(filled(own.width(), own.height(), Qt::red)));
            QSize last;
            for (const double scale : scales) {
                const QSize to = scaledSize(own, scale);
                if (to == last || scale == 100.0)
                    continue;
                last = to;
                CAPTURE(own.width(), own.height(), scale, to.width(), to.height());
                t->scaleBox()->setValue(scale);
                REQUIRE(size() == to);
                t->xBox()->setValue(t->xBox()->value() ? 0 : 1);
                if (to.width() != own.width())
                    t->widthBox()->setValue(own.width());
                else
                    t->heightBox()->setValue(own.height());
                CHECK(size() == own);
                if (size() != own)
                    break;  // one failure per picture
            }
            REQUIRE(d->removeLayer(id));
        }
    }
    SECTION("after a scale, a kept edit gives the same size with a move between or without") {
        for (const QSize own : {QSize(1, 100), QSize(100, 1), QSize(1, 1), QSize(40, 20), QSize(3, 7), QSize(7, 3),
                                QSize(20000, 1), QSize(30000, 1), QSize(20, 20)}) {
            const auto bytes = encoded(filled(own.width(), own.height(), Qt::red));
            const double most = 100.0 * top / std::max(own.width(), own.height());
            for (const double scale : {1.0, 50.0, 80.0, 250.0, most - 3, 1e8}) {
                for (const bool wide : {true, false}) {
                    for (const int by : {-1, 1, 0}) {
                        // the kept edit's size, from the scale's, a move first or not
                        const auto edited = [&](bool moved) {
                            const LayerId id = add(own, bytes);
                            t->scaleBox()->setValue(scale);
                            const QSize from = size();
                            if (moved)
                                t->xBox()->setValue(t->xBox()->value() + 1);
                            const int to = by ? (wide ? from.width() : from.height()) + by : 37;
                            wide ? t->widthBox()->setValue(to) : t->heightBox()->setValue(to);
                            const QSize got = size();
                            REQUIRE(d->removeLayer(id));
                            return got;
                        };
                        CAPTURE(own.width(), own.height(), scale, wide, by);
                        const QSize still = edited(false), moved = edited(true);
                        CAPTURE(still.width(), still.height(), moved.width(), moved.height());
                        CHECK(moved == still);
                    }
                }
            }
        }
    }
}

TEST_CASE("Scale marks a size no scale to two decimals gives, and its arrows step from the size", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    constexpr int top = ReferenceImage::kMaxPlace;
    const auto size = [d] {
        const ReferenceImage& r = *d->active()->props.reference;
        return QSize(r.width, r.height);
    };
    const QString approx = QStringLiteral("≈ ");

    SECTION("20000x1 kept to 1 px wide: below the box's least") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(20000, 1, Qt::red)), 0, 0, 20000, 1, 100}));
        t->widthBox()->setValue(1);
        REQUIRE(size() == QSize(1, 1));
        CHECK(t->scaleBox()->value() == 0.01);  // which gives 2x1
        CHECK(t->scaleBox()->prefix() == approx);
        const int steps = d->undoStack()->count();
        t->scaleBox()->stepDown();  // both sides at 1 px
        CHECK(size() == QSize(1, 1));
        CHECK(d->undoStack()->count() == steps);
        t->scaleBox()->stepUp();  // a step on from the 0.01% shown
        CHECK(size() == QSize(202, 1));
        CHECK(t->scaleBox()->value() == 1.01);
        CHECK(t->scaleBox()->prefix().isEmpty());
    }
    SECTION("30000x1 kept to 4 px wide: the arrow down stays on at the box's least") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(30000, 1, Qt::red)), 0, 0, 30000, 1, 100}));
        t->widthBox()->setValue(4);
        REQUIRE(size() == QSize(4, 1));
        CHECK(t->scaleBox()->value() == 0.01);  // which gives 3x1
        CHECK(t->scaleBox()->prefix() == approx);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(t->scaleBox(), &down);
        CHECK(size() == QSize(3, 1));
        CHECK(t->scaleBox()->prefix().isEmpty());
        const int steps = d->undoStack()->count();
        QApplication::sendEvent(t->scaleBox(), &down);  // the box's least: nothing
        CHECK(size() == QSize(3, 1));
        CHECK(d->undoStack()->count() == steps);
    }
    SECTION("30000x1 at the limit: the scale a hundredth past the long side's gives it") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(30000, 1, Qt::red)), 0, 0, 30000, 1, 100}));
        t->scaleBox()->setValue(1e8);
        REQUIRE(size() == QSize(top, 559));
        t->xBox()->setValue(1);
        CHECK(t->scaleBox()->value() == 55924.06);  // cut to the limit; 55924.05% is 16777215x559
        CHECK(scaledSize({30000, 1}, t->scaleBox()->value()) == QSize(top, 559));
        CHECK(t->scaleBox()->prefix().isEmpty());
        const int steps = d->undoStack()->count();
        t->scaleBox()->stepUp();
        CHECK(size() == QSize(top, 559));
        CHECK(d->undoStack()->count() == steps);
        t->scaleBox()->stepDown();
        CHECK(size() == QSize(16776918, 559));
        CHECK(t->scaleBox()->value() == 55923.06);
        CHECK(t->scaleBox()->prefix().isEmpty());
    }
}

TEST_CASE("Scale's arrows take a typed scale first, whatever the size there is", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    constexpr int top = ReferenceImage::kMaxPlace;
    const auto size = [d] {
        const ReferenceImage& r = *d->active()->props.reference;
        return QSize(r.width, r.height);
    };
    QDoubleSpinBox* box = t->scaleBox();

    SECTION("100x100 at 1x1: type 100, then the arrow down the size there is has none of") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(100, 100, Qt::red)), 0, 0, 1, 1, 100}));
        REQUIRE(box->value() == 1.0);
        type(box, QStringLiteral("100"));
        REQUIRE(size() == QSize(1, 1));  // not committed yet
        press(box, Qt::Key_Down);
        CHECK(size() == QSize(99, 99));
        CHECK(box->value() == 99.0);
        CHECK(box->prefix().isEmpty());
    }
    SECTION("1x100 at the limit: type 50, then the arrow up the size there is has none of") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(1, 100, Qt::red)), 0, 0, 1, 100, 100}));
        box->setValue(1e8);
        REQUIRE(size() == QSize(167772, top));
        type(box, QStringLiteral("50"));
        REQUIRE(size() == QSize(167772, top));
        press(box, Qt::Key_Up);
        CHECK(size() == QSize(1, 51));
        CHECK(box->value() == 51.0);
    }
    SECTION("committed, an arrow the size has no step for does nothing again") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(100, 100, Qt::red)), 0, 0, 1, 1, 100}));
        type(box, QStringLiteral("0.5"));
        press(box, Qt::Key_Return);
        REQUIRE(size() == QSize(1, 1));
        const int steps = d->undoStack()->count();
        press(box, Qt::Key_Down);
        CHECK(size() == QSize(1, 1));
        CHECK(d->undoStack()->count() == steps);
        CHECK(box->value() == 0.5);
    }
    SECTION("100x100 at 200x200: an emptied number, then the arrow down, steps nowhere") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(100, 100, Qt::red)), 0, 0, 200, 200, 100}));
        REQUIRE(box->value() == 200.0);
        const int steps = d->undoStack()->count();
        box->selectAll();
        press(box, Qt::Key_Backspace);
        REQUIRE(box->findChild<QLineEdit*>()->text() == QStringLiteral("%"));
        press(box, Qt::Key_Down);
        CHECK(size() == QSize(200, 200));
        CHECK(d->undoStack()->count() == steps);
        CHECK(box->value() == 200.0);
        CHECK(box->findChild<QLineEdit*>()->text() == QStringLiteral("200.00%"));
    }
    SECTION("100x100 at 200x200: a typed 0, then the arrow down, steps nowhere") {
        REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(100, 100, Qt::red)), 0, 0, 200, 200, 100}));
        const int steps = d->undoStack()->count();
        type(box, QStringLiteral("0"));
        REQUIRE(box->findChild<QLineEdit*>()->text() == QStringLiteral("0%"));
        press(box, Qt::Key_Down);
        CHECK(size() == QSize(200, 200));
        CHECK(d->undoStack()->count() == steps);
        CHECK(box->value() == 200.0);
    }
}

TEST_CASE("Scale shows a scale cut to the limit as exact when it gives the size", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    constexpr int top = ReferenceImage::kMaxPlace;
    const auto size = [d] {
        const ReferenceImage& r = *d->active()->props.reference;
        return QSize(r.width, r.height);
    };
    REQUIRE(d->addReferenceLayer(ReferenceImage{encoded(filled(100000, 1, Qt::red)), 0, 0, 100000, 1, 100}));
    t->scaleBox()->setValue(1e8);
    REQUIRE(size() == QSize(top, 168));
    REQUIRE(t->scaleBox()->value() == 16777.22);
    REQUIRE(t->scaleBox()->prefix().isEmpty());
    t->xBox()->setValue(1);  // the scale shown is worked out again from the size
    CHECK(t->scaleBox()->prefix().isEmpty());
    CHECK_FALSE(t->scaleBox()->toolTip().contains(QStringLiteral("≈")));
    CHECK(scaledSize({100000, 1}, t->scaleBox()->value()) == QSize(top, 168));
    CHECK(t->scaleBox()->value() == 16777.22);
}

TEST_CASE("Scale's arrow steps down never grow a side, and up never shrink one", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    TransformOptions* t = w.findChild<ToolOptions*>()->transformOptions();
    w.tools()->setActive(QStringLiteral("move"));
    constexpr int top = ReferenceImage::kMaxPlace;
    const auto size = [d] {
        const ReferenceImage& r = *d->active()->props.reference;
        return QSize(r.width, r.height);
    };
    const auto covers = [](QSize a, QSize b) { return a.width() >= b.width() && a.height() >= b.height(); };

    for (const QSize own : {QSize(1, 100), QSize(100, 1), QSize(1, 1), QSize(40, 20), QSize(3, 7), QSize(7, 3),
                            QSize(20000, 1), QSize(30000, 1), QSize(20, 20)}) {
        // the scale that puts the long side at the limit, or the box's own
        const double most = 100.0 * top / std::max(own.width(), own.height());
        // a start set by a scale, or by a kept width or height (the scale
        // the size shows then is the size's own)
        struct Start {
            double scale = 0;
            int width = 0, height = 0;
        };
        const Start starts[] = {{.scale = 1},        {.scale = 50},     {.scale = 80},       {.scale = 250},
                                {.scale = most - 3}, {.scale = 1e8},    {.width = 37},       {.height = 37},
                                {.width = 2},        {.height = 2},     {.width = top - 1},  {.height = top - 1}};
        // the sizes the box's two ends give: no step goes past them
        const QSize least = scaledSize(own, t->scaleBox()->minimum());
        const QSize largest = scaledSize(own, most);
        for (const Start& start : starts) {
            for (const bool up : {false, true}) {
                CAPTURE(own.width(), own.height(), start.scale, start.width, start.height, up);
                const LayerId id = d->addReferenceLayer(
                    ReferenceImage{encoded(filled(own.width(), own.height(), Qt::red)), 0, 0, own.width(),
                                   own.height(), 100});
                REQUIRE(id);
                REQUIRE(t->keepAspectBox()->isChecked());
                if (start.scale > 0)
                    t->scaleBox()->setValue(start.scale);
                else if (start.width > 0)
                    t->widthBox()->setValue(start.width);
                else
                    t->heightBox()->setValue(start.height);
                for (int i = 0; i < 30; ++i) {
                    // a move, or undo and redo, between steps: the scale
                    // shown is worked out again from the size
                    if (i % 3 == 1)
                        t->xBox()->setValue(t->xBox()->value() + 1);
                    else if (i % 3 == 2) {
                        d->undoStack()->undo();
                        d->undoStack()->redo();
                    }
                    const QSize last = size();
                    const double was = t->scaleBox()->value();
                    up ? t->scaleBox()->stepUp() : t->scaleBox()->stepDown();
                    const QSize now = size();
                    const double shown = t->scaleBox()->value();
                    CAPTURE(i, last.width(), last.height(), now.width(), now.height(), was, shown);
                    CHECK((up ? covers(now, last) : covers(last, now)));
                    // the scale shown gives the size, or says it doesn't
                    CHECK(t->scaleBox()->prefix().isEmpty() == (scaledSize(own, shown) == now));
                    // short of the box's end that way, a step moves the size,
                    // or the scale shown that way while it still gives the
                    // size (steps that round the same add up)
                    const QSize end = up ? largest : least;
                    const bool atEnd = !(up ? covers(end, last) : covers(last, end)) || end == last;
                    if (!atEnd)
                        CHECK((now != last || ((up ? shown > was : shown < was) && scaledSize(own, shown) == now)));
                }
                REQUIRE(d->removeLayer(id));
            }
        }
    }
}

TEST_CASE("a moved and resized reference layer saves and opens where it was put", "[reference]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("placed.bwp"));
    {
        MainWindow w;
        Document* d = w.document();
        CanvasView* v = w.canvasView();
        const LayerId id = addPicture(d);
        w.tools()->setActive(QStringLiteral("move"));
        drag(v, {40, 40}, {20, 30});                        // to (-2, 3)
        drag(v, {156, 86}, {196, 86}, Qt::ShiftModifier);  // the bottom-right corner 20 right
        REQUIRE(place(d, id) == QRect(-2, 3, 100, 40));
        QString error;
        REQUIRE(d->saveProject(path, FileForm::ProjectZip, &error));
    }
    const project::Project read = project::read(path.toStdString());
    const Layer* layer = nullptr;
    for (std::size_t i = 0; i < read.layers.size(); ++i)
        if (read.layers[i].props.kind == LayerKind::Reference)
            layer = &read.layers[i];
    REQUIRE(layer);
    REQUIRE(layer->props.reference);
    CHECK(layer->props.reference->x == -2);
    CHECK(layer->props.reference->y == 3);
    CHECK(layer->props.reference->width == 100);
    CHECK(layer->props.reference->height == 40);
    CHECK(layer->props.reference->opacity == 100);

    // opened again, the view draws it there
    MainWindow w;
    w.document()->replace(read, path, FileForm::ProjectZip);
    REQUIRE(w.document()->setActiveLayer(layer->props.id));
    CHECK(w.canvasView()->referenceFrame() == QRect(-4, 6, 200, 80));
}
