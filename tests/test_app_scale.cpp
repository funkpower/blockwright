// The canvas view and the 1x preview under fractional display scaling
// (PLAN §4, integer zoom): every canvas pixel is a whole block of device
// pixels at every zoom and in the preview, and a partial repaint leaves the
// same pixels a full one does. Its own binary: the scale factor has to be
// set before the QApplication exists.

#include "blockwright/cp437.h"
#include "blockwright/render.h"

#include "canvas_view.h"
#include "device_grid.h"
#include "document.h"
#include "main_window.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QColor>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <string>

using namespace bw;

namespace {

constexpr double kScale = 1.5;

// A canvas whose every cell differs from its neighbours in glyph and
// colours, so a pixel drawn in the wrong place shows.
AnsiDocument busyCanvas(int w, int h) {
    AnsiDocument d;
    d.canvas = Canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Cell& c = d.canvas.at(x, y);
            c.ch = cp437ToUnicode(std::uint8_t(0x21 + (x * 7 + y * 13) % 0xDE));
            c.fg = Color::indexed(std::uint8_t((x + 2 * y) % 15 + 1));
            c.bg = Color::indexed(std::uint8_t((3 * x + y) % 8));
        }
    return d;
}

// Device pixels per canvas pixel at `zoom`: the zoom times the device pixel
// ratio rounded down.
int blockSize(int zoom) { return zoom * std::max(1, int(std::floor(kScale))); }

// Empty if `shot` (device pixels) shows `expected` with each of its pixels
// a b x b block, the first one's top-left at `at`; otherwise the first pixel
// that doesn't. The pixels of `expected` in `skip` (the cell cursor's) are
// not compared.
std::string compareBlocks(const QImage& shot, const QImage& expected, int b, QPoint at, QRect skip = {}) {
    const QImage s = shot.convertToFormat(QImage::Format_RGB32);
    const QImage e = expected.convertToFormat(QImage::Format_RGB32);
    if (s.width() < at.x() + e.width() * b || s.height() < at.y() + e.height() * b)
        return "shot " + std::to_string(s.width()) + "x" + std::to_string(s.height()) + " too small for " +
               std::to_string(e.width()) + "x" + std::to_string(e.height()) + " at " + std::to_string(b);
    for (int y = 0; y < e.height() * b; ++y) {
        const auto* sl = reinterpret_cast<const QRgb*>(s.constScanLine(at.y() + y));
        const auto* el = reinterpret_cast<const QRgb*>(e.constScanLine(y / b));
        for (int x = 0; x < e.width() * b; ++x)
            if (sl[at.x() + x] != el[x / b] && !skip.contains(x / b, y / b))
                return "device pixel " + std::to_string(at.x() + x) + "," + std::to_string(at.y() + y);
    }
    return {};
}

QImage deviceImage(const QWidget* w) {
    QImage img((QSizeF(w->size()) * w->devicePixelRatioF()).toSize(), QImage::Format_RGB32);
    img.setDevicePixelRatio(w->devicePixelRatioF());
    img.fill(Qt::magenta);
    return img;
}

// The region every widget it watches paints, in its parent's coordinates.
class PaintRecorder : public QObject {
public:
    QRegion region;

    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::Paint)
            region |= static_cast<QPaintEvent*>(e)->region().translated(static_cast<QWidget*>(o)->pos());
        return false;
    }
};

void key(QWidget* w, int k, const QString& text) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &release);
}

} // namespace

TEST_CASE("the test runs at a fractional device pixel ratio") {
    MainWindow w;
    REQUIRE(w.canvasView()->devicePixelRatioF() == kScale);
}

TEST_CASE("the 1x preview shows each canvas pixel as a whole block of device pixels") {
    MainWindow w;
    w.document()->replace(busyCanvas(16, 6), {});
    w.show();
    QApplication::processEvents();
    const QImage expected = renderCanvas(w.document()->canvas(), 8, 1);
    const QImage shot = w.preview()->grab().toImage();
    CHECK(compareBlocks(shot, expected, blockSize(1), {0, 0}) == "");
}

TEST_CASE("the canvas view at zoom 1-8 shows whole device-pixel blocks after a partial repaint") {
    MainWindow w;
    w.document()->replace(busyCanvas(16, 6), {});
    w.show();
    QApplication::processEvents();
    CanvasView* v = w.canvasView();
    for (int zoom = CanvasView::kMinZoom; zoom <= CanvasView::kMaxZoom; ++zoom) {
        CAPTURE(zoom);
        v->setZoom(zoom);
        v->setCellCursor({7, 2});
        QApplication::processEvents();
        QImage shot = deviceImage(v);
        v->render(&shot);
        // Type over cells in the middle, then repaint only a rect around
        // them whose edges fall inside device pixels, over the old pixels.
        key(v, Qt::Key_Z, QStringLiteral("z"));
        key(v, Qt::Key_Q, QStringLiteral("q"));
        v->setCellCursor({0, 0});
        const QRect part(v->width() / 4 + 1, v->height() / 5 + 1, v->width() / 2 - 1, v->height() * 3 / 5 - 1);
        v->render(&shot, part.topLeft(), QRegion(part));
        const QImage expected = renderCanvas(w.document()->canvas(), 8, 1);
        CHECK(compareBlocks(shot, expected, blockSize(zoom), {0, 0}) == "");
        w.document()->undoStack()->undo();
        w.document()->undoStack()->undo();
    }
}

TEST_CASE("a device grid puts view pixels on whole device pixels") {
    const qreal ratio = GENERATE(1.0, 2.0, 1.25, 1.5, 1.75, 2.5);
    const double ox = GENERATE(0.0, 1.5, 3.75, 10.0 / 3);
    CAPTURE(ratio, ox);
    // widgets sit on whole logical pixels, so only off a whole ratio does
    // their origin fall inside a device pixel
    if (ratio == std::floor(ratio) && ox != std::floor(ox))
        return;
    const QPointF origin(ox, 2 * ox);
    const DeviceGrid g(ratio, origin);
    CHECK(g.scale() == std::max(1, int(std::floor(ratio))));
    const QTransform device = QTransform::fromScale(ratio, ratio) * QTransform::fromTranslate(origin.x(), origin.y());
    const QTransform m = g.viewTransform(device) * device;
    // view pixel (0, 0) starts on the first whole device pixel at or after
    // the widget's origin, and each is scale() device pixels
    const QPointF start = m.map(QPointF(0, 0));
    CHECK(start == QPointF(std::ceil(origin.x() - 1e-6), std::ceil(origin.y() - 1e-6)));
    CHECK(std::abs(m.map(QPointF(7, 5)).x() - (start.x() + 7 * g.scale())) < 1e-9);
    CHECK(std::abs(m.map(QPointF(7, 5)).y() - (start.y() + 5 * g.scale())) < 1e-9);

    // the widget holds the view wherever its origin falls
    const QSize view(129, 97);
    const QSize w = g.widgetSize(view);
    CHECK(start.x() + view.width() * g.scale() <= origin.x() + w.width() * ratio + 1e-9);
    CHECK(start.y() + view.height() * g.scale() <= origin.y() + w.height() * ratio + 1e-9);
    // and no more than a logical pixel over
    CHECK((w.width() - 1) * ratio < view.width() * g.scale() + 1);

    // rects round outward both ways, and are unchanged at a whole ratio
    const QRect r(5, 3, 17, 9);
    const QRect wr = g.toWidget(r);
    CHECK(g.toView(wr).contains(r));
    CHECK(origin.x() + wr.left() * ratio <= start.x() + r.left() * g.scale() + 1e-9);
    CHECK(origin.x() + (wr.right() + 1) * ratio >= start.x() + (r.right() + 1) * g.scale() - 1e-9);
    if (ratio == std::floor(ratio) && ox == std::floor(ox)) {
        CHECK(wr == QRect(r.topLeft() * g.scale() / ratio, r.size() * g.scale() / ratio));
        CHECK(w == view * g.scale() / ratio);
    }
    // a widget point maps to the view pixel drawn under it
    const QPointF p(13.4, 7.9);
    const QPointF v = g.toView(p);
    const QPointF back = m.map(v);
    CHECK(std::abs(back.x() - device.map(p).x()) < 1e-9);
    CHECK(std::abs(back.y() - device.map(p).y()) < 1e-9);
}

TEST_CASE("a view whose origin falls inside a device pixel starts on the next one") {
    Document doc;
    doc.replace(busyCanvas(12, 4), {});
    AtlasRenderer renderer;
    renderer.setCanvas(&doc.canvas(), 8);
    QWidget top;
    top.resize(800, 400);
    auto* v = new CanvasView(&doc, &renderer, &top);
    auto* preview = new PreviewWidget(&renderer, &top);
    QObject::connect(&doc, &Document::cellsChanged, &top, [&](const QRegion& cells) {
        renderer.markDirty(cells);
        v->cellsChanged(cells);
        preview->cellsChanged(cells);
    });
    v->move(1, 1);  // device pixel 1.5
    preview->move(1, 251);  // 1.5, 376.5
    top.show();
    QApplication::processEvents();
    const QPoint viewStart(2, 2), previewStart(2, 377);
    const auto cursorPixels = [&] {
        const QSize cs = renderer.cellSize();
        return QRect(QPoint(v->cellCursor().x * cs.width(), v->cellCursor().y * cs.height()), cs);
    };
    for (int zoom : {1, 2, 3, 5}) {
        CAPTURE(zoom);
        v->setZoom(zoom);
        QApplication::processEvents();
        QImage shot = deviceImage(&top);
        top.render(&shot);
        CHECK(compareBlocks(shot, renderCanvas(doc.canvas(), 8, 1), blockSize(zoom), viewStart, cursorPixels()) == "");
        CHECK(compareBlocks(shot, renderCanvas(doc.canvas(), 8, 1), blockSize(1), previewStart) == "");
        // the device pixel the view shares with the window is the view's
        // background, not left unpainted
        CHECK(shot.pixelColor(1, 10) != QColor(Qt::magenta));

        // a cell under the pointer: the centre of its device pixels
        const QPoint cell(5, 2);
        const QPointF device =
            QPointF(viewStart) + QPointF(v->cellRect(cell).center()) * blockSize(zoom) / zoom + QPointF(0.5, 0.5);
        const QPointF local = device / kScale - QPointF(v->pos());
        QMouseEvent press(QEvent::MouseButtonPress, local, v->mapToGlobal(local), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(v, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, v->mapToGlobal(local), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(v, &release);
        CHECK(v->cellCursor() == Cursor{cell.x(), cell.y()});

        // typing repaints only the cells it changed and the cursor's; those
        // paints, made again over the frame from before, land on its grid
        QApplication::processEvents();
        shot = deviceImage(&top);
        top.render(&shot);
        PaintRecorder painted;
        v->installEventFilter(&painted);
        preview->installEventFilter(&painted);
        key(v, Qt::Key_X, QStringLiteral("x"));
        QApplication::processEvents();
        v->removeEventFilter(&painted);
        preview->removeEventFilter(&painted);
        REQUIRE_FALSE(painted.region.isEmpty());
        CHECK((painted.region & v->geometry()).boundingRect().width() < v->width() / 2);
        top.render(&shot, painted.region.boundingRect().topLeft(), painted.region);
        CHECK(compareBlocks(shot, renderCanvas(doc.canvas(), 8, 1), blockSize(zoom), viewStart, cursorPixels()) == "");
        CHECK(compareBlocks(shot, renderCanvas(doc.canvas(), 8, 1), blockSize(1), previewStart) == "");
        const QRect cursorWidget = v->toWidget(v->cellRect(v->cellCursor()));
        CHECK(QRect(QPoint(0, 0), v->size()).contains(cursorWidget));
        doc.undoStack()->undo();
    }
}

TEST_CASE("a stylus paints the cell under it at a fractional device pixel ratio") {
    MainWindow w;
    w.show();
    QApplication::processEvents();
    CanvasView* v = w.canvasView();
    auto* glyph = w.findChild<QAction*>(QStringLiteral("glyphTool"));
    REQUIRE(glyph);
    glyph->trigger();
    static const QPointingDevice pen(QStringLiteral("test stylus"), 4242, QInputDevice::DeviceType::Stylus,
                                     QPointingDevice::PointerType::Pen,
                                     QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const QPoint cell(9, 3);
    for (int zoom : {1, 2, 3, 5}) {
        CAPTURE(zoom);
        v->setZoom(zoom);
        QApplication::processEvents();
        const QPointF at = QRectF(v->toWidget(v->cellRect(cell))).center();
        for (QEvent::Type type : {QEvent::TabletPress, QEvent::TabletRelease}) {
            const bool down = type == QEvent::TabletPress;
            QTabletEvent e(type, &pen, at, v->mapToGlobal(at), down ? 1.0 : 0.0, 0, 0, 0, 0, 0, Qt::NoModifier,
                           Qt::LeftButton, down ? Qt::LeftButton : Qt::NoButton);
            QApplication::sendEvent(v, &e);
        }
        const Canvas& c = w.document()->canvas();
        CHECK_FALSE(c.at(cell.x(), cell.y()) == Cell{});
        int painted = 0;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                painted += !(c.at(x, y) == Cell{});
        CHECK(painted == 1);
        w.document()->undoStack()->undo();
    }
}

int main(int argc, char** argv) {
    qputenv("QT_SCALE_FACTOR", QByteArray::number(kScale));
    qputenv("QT_SCALE_FACTOR_ROUNDING_POLICY", "PassThrough");
    // The Library reads empty scratch directories, never the user's.
    QTemporaryDir libraries;
    qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("bundled"))));
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("user"))));
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
