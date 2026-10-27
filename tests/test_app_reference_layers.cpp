// Reference layers in the main window (#178): File > Open Reference Image
// adds one, the view draws each visible one at its own place, size and
// opacity, Layer > Reference Opacity sets it, and tools leave it alone.

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "reference_scale.h"
#include "tool.h"

#include "blockwright/project.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QMenu>
#include <QMouseEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

std::shared_ptr<const std::string> encoded(const QImage& image, const char* format = "PNG") {
    QByteArray bytes;
    QBuffer out(&bytes);
    REQUIRE(out.open(QIODevice::WriteOnly));
    REQUIRE(image.save(&out, format));
    return std::make_shared<const std::string>(bytes.toStdString());
}

QByteArray pngIcon(const QByteArray& png) {
    QByteArray ico;
    QDataStream directory(&ico, QIODevice::WriteOnly);
    directory.setByteOrder(QDataStream::LittleEndian);
    directory << quint16(0) << quint16(1) << quint16(1);
    directory << quint8(16) << quint8(16) << quint8(0) << quint8(0);
    directory << quint16(1) << quint16(32) << quint32(png.size()) << quint32(22);
    ico.append(png);
    return ico;
}

QImage filled(int w, int h, QColor c) {
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(c);
    return image;
}

// An XBM whose header claims `w` x `h` px, followed by one byte of data.
// Qt's XBM detector decodes the whole picture to recognise it.
QByteArray xbmClaiming(int w, int h) {
    return QStringLiteral("#define p_width %1\n#define p_height %2\nstatic char p_bits[] = {0x00};\n")
        .arg(w)
        .arg(h)
        .toLatin1();
}

// The process's peak resident memory in KiB since resetPeakMemory(), or -1
// where Linux's /proc doesn't report it.
qint64 peakMemoryKiB() {
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray& line : status.readAll().split('\n'))
        if (line.startsWith("VmHWM:"))
            return line.mid(6).simplified().split(' ').value(0).toLongLong();
    return -1;
}

bool resetPeakMemory() {
    std::FILE* f = std::fopen("/proc/self/clear_refs", "w");
    if (!f)
        return false;
    const bool written = std::fputs("5", f) >= 0;
    return std::fclose(f) == 0 && written && peakMemoryKiB() >= 0;
}

bool near(QColor a, QColor b, int tol = 3) {
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
           std::abs(a.blue() - b.blue()) <= tol;
}

void mouse(CanvasView* v, QEvent::Type type, QPoint cell, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const QPointF at = v->toWidget(v->cellRect(cell)).center();
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
    QApplication::sendEvent(v, &e);
}

} // namespace

TEST_CASE("Open Reference Image adds a reference layer across the canvas, one undo step", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const LayerId art = d->activeLayer();
    const int steps = d->undoStack()->count();
    const QString png = dir.filePath(QStringLiteral("wide.png"));
    REQUIRE(filled(200, 50, Qt::green).save(png));
    QString error;
    REQUIRE(w.loadReferenceImage(png, &error));

    // on top, active, the canvas's width at the picture's aspect, half opacity
    const Layer* l = d->active();
    REQUIRE(l);
    CHECK(l->props.kind == LayerKind::Reference);
    CHECK(d->layers()[d->layers().size() - 1].props.id == l->props.id);
    CHECK(l->props.name == "Reference 1");
    REQUIRE(l->props.reference);
    const ReferenceImage& r = *l->props.reference;
    CHECK(r.x == 0);
    CHECK(r.y == 0);
    CHECK(r.width == w.canvasView()->canvasPixels().width());
    CHECK(r.height == r.width / 4);
    CHECK(r.opacity == 50);
    CHECK(referenceImageType(*r.bytes) == "png");
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("New Reference Layer"));

    d->undoStack()->undo();
    CHECK(d->layers().size() == 1);
    CHECK(d->activeLayer() == art);
    d->undoStack()->redo();
    REQUIRE(d->active()->props.reference);

    // a picture in another format is kept as a PNG of it
    const QString bmp = dir.filePath(QStringLiteral("pic.bmp"));
    REQUIRE(filled(10, 10, Qt::blue).save(bmp));
    REQUIRE(w.loadReferenceImage(bmp, &error));
    CHECK(referenceImageType(*d->active()->props.reference->bytes) == "png");
    CHECK(d->active()->props.name == "Reference 2");
    // one that won't decode is refused, and adds nothing
    const QString junk = dir.filePath(QStringLiteral("junk.png"));
    {
        QFile f(junk);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a picture");
    }
    CHECK_FALSE(w.loadReferenceImage(junk, &error));
    CHECK_FALSE(error.isEmpty());
    CHECK(d->layers().size() == 3);

    // saved with the project and opened again
    const QString bwp = dir.filePath(QStringLiteral("refs.bwp"));
    REQUIRE(w.saveFile(bwp, &error));
    MainWindow again;
    REQUIRE(again.openFile(bwp, &error));
    REQUIRE(again.document()->layers().size() == 3);
    CHECK(again.document()->project().layers == d->project().layers);
}

TEST_CASE("each reference layer draws at its own place, size, opacity and visibility, in stack order",
          "[reference]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    Document* d = w.document();
    const LayerId red = d->addReferenceLayer(ReferenceImage{encoded(filled(4, 4, Qt::red)), 16, 16, 64, 32, 100});
    const LayerId blue = d->addReferenceLayer(ReferenceImage{encoded(filled(4, 4, Qt::blue)), 48, 32, 64, 32, 50});
    REQUIRE(red);
    REQUIRE(blue);
    QImage shot = v->grab().toImage();
    CHECK(shot.pixelColor(4, 100) == QColor(Qt::black));  // outside both
    CHECK(near(shot.pixelColor(20, 20), Qt::red));  // the red one, opaque
    CHECK(near(shot.pixelColor(100, 60), QColor(0, 0, 128)));  // the blue one, at half, on black
    CHECK(near(shot.pixelColor(60, 40), QColor(128, 0, 128)));  // the blue one over the red: on top
    CHECK(shot.pixelColor(130, 60) == QColor(Qt::black));  // past the blue one's right edge

    // the red one hidden: the blue one's opacity is its own
    LayerProperties props = d->layers().find(red)->props;
    props.visible = false;
    REQUIRE(d->setLayerProperties(red, props, QStringLiteral("Hide")));
    shot = v->grab().toImage();
    CHECK(shot.pixelColor(20, 20) == QColor(Qt::black));
    CHECK(near(shot.pixelColor(60, 40), QColor(0, 0, 128)));
    // and at 2x, a canvas pixel two view pixels
    v->setZoom(2);
    shot = v->grab().toImage();
    CHECK(near(shot.pixelColor(97, 65), QColor(0, 0, 128)));
    CHECK(shot.pixelColor(95, 63) == QColor(Qt::black));
}

TEST_CASE("Layer > Reference Opacity sets the active reference layer's, as an undo step", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    auto* menu = w.findChild<QMenu*>(QStringLiteral("referenceOpacity"));
    REQUIRE(menu);
    CHECK_FALSE(menu->isEnabled());  // a Cells layer is active
    const LayerId id = d->addReferenceLayer(ReferenceImage{encoded(filled(2, 2, Qt::red)), 0, 0, 8, 8, 50});
    REQUIRE(id);
    CHECK(menu->isEnabled());
    CHECK(act(w, "referenceOpacity50")->isChecked());
    act(w, "referenceOpacity25")->trigger();
    CHECK(d->layers().find(id)->props.reference->opacity == 25);
    CHECK(act(w, "referenceOpacity25")->isChecked());
    CHECK(d->undoStack()->undoText() == QStringLiteral("Reference Opacity"));
    d->undoStack()->undo();
    CHECK(d->layers().find(id)->props.reference->opacity == 50);
    CHECK(act(w, "referenceOpacity50")->isChecked());
}

TEST_CASE("tools and Alt+drag leave a reference layer alone", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    const LayerId id = d->addReferenceLayer(ReferenceImage{encoded(filled(2, 2, Qt::red)), 8, 8, 80, 40, 50});
    REQUIRE(id);
    const auto before = d->project().layers;
    const int steps = d->undoStack()->count();

    w.tools()->setActive(QStringLiteral("line"));
    mouse(v, QEvent::MouseButtonPress, {1, 1});
    mouse(v, QEvent::MouseMove, {6, 1});
    mouse(v, QEvent::MouseButtonRelease, {6, 1});
    CHECK(d->project().layers == before);
    CHECK(d->undoStack()->count() == steps);

    mouse(v, QEvent::MouseButtonPress, {1, 1}, Qt::AltModifier);
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("Cannot move a reference layer yet"));
    mouse(v, QEvent::MouseMove, {6, 3}, Qt::AltModifier);
    mouse(v, QEvent::MouseButtonRelease, {6, 3}, Qt::AltModifier);
    CHECK(w.statusBar()->currentMessage().isEmpty());
    CHECK(d->project().layers == before);
    CHECK(d->undoStack()->count() == steps);
}

TEST_CASE("reference pictures are decoded once and scaled within one shared cap", "[reference]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* d = w.document();
    const auto shared = encoded(filled(300, 200, Qt::red));
    // three layers, two pictures, each shown far past the cap at 8x
    const int big = 2048;
    const LayerId a = d->addReferenceLayer(ReferenceImage{shared, 0, 0, big, big, 50});
    const LayerId b = d->addReferenceLayer(ReferenceImage{shared, 8, 8, big, big, 50});
    const LayerId c = d->addReferenceLayer(ReferenceImage{encoded(filled(30, 20, Qt::blue)), 0, 0, big, big, 50});
    v->setZoom(8);
    v->grab();
    CHECK(v->referencePictureCount() == 2);
    CHECK(v->referencePictureSize(a) == QSize(300, 200));
    qint64 total = 0;
    for (const LayerId id : {a, b, c}) {
        const QSize s = v->referenceCacheSize(id);
        CHECK_FALSE(s.isEmpty());
        total += qint64(s.width()) * s.height();
    }
    CHECK(total <= CanvasView::kMaxReferencePixels);
    CHECK(total > CanvasView::kMaxReferencePixels * 9 / 10);

    // a hidden layer's copy goes, and its picture once no visible layer shows it
    for (const LayerId id : {c}) {
        LayerProperties props = d->layers().find(id)->props;
        props.visible = false;
        REQUIRE(d->setLayerProperties(id, props, QStringLiteral("Hide")));
    }
    v->grab();
    CHECK(v->referencePictureCount() == 1);
    CHECK(v->referenceCacheSize(c).isEmpty());
    // the two left share the cap
    CHECK(qint64(v->referenceCacheSize(a).width()) * v->referenceCacheSize(a).height() <=
          CanvasView::kMaxReferencePixels / 2);
}

TEST_CASE("a reduced reference picture is averaged, not sampled", "[reference]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    // one-pixel white and black columns, shown at a quarter of their width
    QImage stripes(256, 4, QImage::Format_RGB32);
    for (int x = 0; x < stripes.width(); ++x)
        for (int y = 0; y < stripes.height(); ++y)
            stripes.setPixelColor(x, y, x % 2 ? Qt::black : Qt::white);
    REQUIRE(w.document()->addReferenceLayer(ReferenceImage{encoded(stripes), 0, 0, 64, 16, 100}));
    const QImage shot = v->grab().toImage();
    for (const QPoint at : {QPoint(3, 3), QPoint(30, 8), QPoint(60, 12)}) {
        INFO(at.x() << "," << at.y() << ": " << shot.pixelColor(at).name().toStdString());
        CHECK(near(shot.pixelColor(at), QColor(128, 128, 128), 4));
    }
}

TEST_CASE("thin reference layers respect the shared cache cap", "[reference]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    Document* d = w.document();
    const auto bytes = encoded(filled(1, 1, Qt::red));

    const LayerId a = d->addReferenceLayer(ReferenceImage{bytes, 0, 0, ReferenceImage::kMaxPlace, 1, 100});
    const LayerId b = d->addReferenceLayer(ReferenceImage{bytes, 0, 0, ReferenceImage::kMaxPlace, 1, 100});
    REQUIRE(a);
    REQUIRE(b);

    v->grab(QRect(0, 0, 16, 16));

    qint64 total = 0;
    for (const LayerId id : {a, b}) {
        const QSize s = v->referenceCacheSize(id);
        REQUIRE_FALSE(s.isEmpty());
        total += qint64(s.width()) * s.height();
    }
    CHECK(total <= CanvasView::kMaxReferencePixels);
}

TEST_CASE("fitPixels stays within its budget after rounding and holding a side at 1", "[reference]") {
    const qint64 cap = CanvasView::kMaxReferencePixels;
    struct Case {
        double w, h;
        qint64 budget;
    };
    // the cache of one of two thin layers; a picture past the decoded limit,
    // wide and tall (PNG and JPEG can't be that thin, but the rule is the
    // same); a square one; one under the budget, kept as it is
    for (const Case c : {Case{double(ReferenceImage::kMaxPlace), 1, cap / 2}, Case{double(cap + cap / 4), 1, cap},
                         Case{1, double(cap + cap / 4), cap}, Case{4100, 4100, cap}, Case{6000, 3000, cap},
                         Case{300, 200, cap}}) {
        const QSize s = fitPixels(c.w, c.h, c.budget);
        INFO(c.w << " x " << c.h << " in " << c.budget << ": " << s.width() << " x " << s.height());
        CHECK(s.width() >= 1);
        CHECK(s.height() >= 1);
        CHECK(s.width() <= c.w);
        CHECK(s.height() <= c.h);
        CHECK(qint64(s.width()) * s.height() <= c.budget);
        // nothing given up but what the budget takes
        CHECK(double(s.width()) * s.height() >= std::min<double>(c.budget, c.w * c.h) * 0.99);
    }
    CHECK(fitPixels(300, 200, cap) == QSize(300, 200));
    CHECK(fitPixels(double(cap + cap / 4), 1, cap) == QSize(int(cap), 1));
}

TEST_CASE("Open Reference Image decodes a PNG past 2^24 px scaled, at its own aspect", "[reference]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* d = w.document();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QString error;

    // kept as it is: 18 Mpx, decoded at most 2^24 on import, and that copy
    // is the one the view draws from
    QImage wide(6000, 3000, QImage::Format_Grayscale8);
    wide.fill(200);
    const QString png = dir.filePath(QStringLiteral("wide.png"));
    REQUIRE(wide.save(png));
    REQUIRE(w.loadReferenceImage(png, &error));
    const Layer* l = d->active();
    REQUIRE(l);
    REQUIRE(l->props.reference);
    CHECK(l->props.reference->height == (l->props.reference->width + 1) / 2);
    const LayerId id = l->props.id;
    CHECK(v->referencePictureCount() == 1);
    const QSize decoded = v->referencePictureSize(id);
    REQUIRE_FALSE(decoded.isEmpty());
    CHECK(qint64(decoded.width()) * decoded.height() <= CanvasView::kMaxReferencePixels);
    CHECK(std::abs(decoded.width() - 2 * decoded.height()) <= 2);
    v->grab(QRect(0, 0, 16, 16));
    CHECK(v->referencePictureCount() == 1);
    CHECK(v->referencePictureSize(id) == decoded);
}

TEST_CASE("Open Reference Image keeps a BMP past 2^24 px as a PNG of at most 2^24 px", "[reference]") {
    MainWindow w;
    Document* d = w.document();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QString error;
    QImage square(4100, 4100, QImage::Format_Grayscale8);
    square.fill(100);
    const QString bmp = dir.filePath(QStringLiteral("square.bmp"));
    REQUIRE(square.save(bmp, "BMP"));
    REQUIRE(w.loadReferenceImage(bmp, &error));
    const Layer* m = d->active();
    REQUIRE(m);
    REQUIRE(m->props.reference);
    CHECK(m->props.reference->height == m->props.reference->width);
    QByteArray kept = QByteArray::fromStdString(*m->props.reference->bytes);
    QBuffer in(&kept);
    QImageReader reader(&in);
    CHECK(reader.format() == "png");
    const QSize stored = reader.size();
    REQUIRE(stored.isValid());
    CHECK(qint64(stored.width()) * stored.height() <= CanvasView::kMaxReferencePixels);
    CHECK(stored.width() == stored.height());
}

TEST_CASE("reference decoding refuses PNG and BMP above the full decode ceiling", "[reference]") {
    // Let Qt accept these headers so its allocation guard cannot hide a missing
    // application guard. The source is only 64 MiB plus one column, grayscale.
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImageReader::setAllocationLimit(512);
    QImage source(8193, 8192, QImage::Format_Grayscale8);
    source.fill(200);
    for (const char* format : {"PNG", "BMP"}) {
        INFO(format);
        const auto bytes = encoded(source, format);
        const QByteArray data = QByteArray::fromStdString(*bytes);
        QString error;
        CHECK(readReferencePicture(data, CanvasView::kMaxReferencePixels, nullptr, &error).isNull());
        CHECK(error.contains(QStringLiteral("too large to decode")));
    }
}

TEST_CASE("reference decoding scales JPEG above the full decode ceiling", "[reference]") {
    QImage source(8193, 8192, QImage::Format_Grayscale8);
    source.fill(200);
    const auto bytes = encoded(source, "JPEG");
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    // A full decode needs over 256 MiB under Qt's allocation accounting;
    // scaled JPEG decoding must work within 128 MiB instead (native JPEG
    // scaling rounds up before Qt resizes to the requested dimensions).
    QImageReader::setAllocationLimit(128);
    QSize original;
    QString error;
    const QImage image = readReferencePicture(QByteArray::fromStdString(*bytes),
                                             CanvasView::kMaxReferencePixels, &original, &error);
    INFO(error.toStdString());
    REQUIRE_FALSE(image.isNull());
    CHECK(original == source.size());
    CHECK(qint64(image.width()) * image.height() <= CanvasView::kMaxReferencePixels);
    CHECK(near(image.pixelColor(0, 0), QColor(200, 200, 200)));
}

TEST_CASE("reference decoding refuses an oversized PNG inside an ICO", "[reference]") {
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImage source(8193, 8192, QImage::Format_Grayscale8);
    REQUIRE_FALSE(source.isNull());
    source.fill(200);
    const QByteArray ico = pngIcon(QByteArray::fromStdString(*encoded(source)));
    QBuffer input;
    input.setData(ico);
    REQUIRE(input.open(QIODevice::ReadOnly));
    QImageReader probe(&input);
    REQUIRE(probe.format() == QByteArray("ico"));
    REQUIRE(probe.size() == QSize(16, 16));

    // Neither a raised nor a disabled caller limit may bypass the ceiling.
    for (const int allocation : {512, 0}) {
        INFO(allocation);
        QImageReader::setAllocationLimit(allocation);
        QSize original;
        QString error;
        CHECK(readReferencePicture(ico, CanvasView::kMaxReferencePixels, &original, &error).isNull());
        CHECK_FALSE(error.isEmpty());
        CHECK(QImageReader::allocationLimit() == allocation);
    }
}

TEST_CASE("Open Reference Image imports a small ICO as PNG", "[reference]") {
    const QImage source = filled(16, 16, Qt::green);
    QByteArray ico;
    SECTION("PNG payload") { ico = pngIcon(QByteArray::fromStdString(*encoded(source))); }
    SECTION("bitmap payload") { ico = QByteArray::fromStdString(*encoded(source, "ICO")); }
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("small.ico"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(ico) == ico.size());
    file.close();

    MainWindow w;
    QString error;
    REQUIRE(w.loadReferenceImage(path, &error));
    const Layer* layer = w.document()->active();
    REQUIRE(layer->props.reference);
    CHECK(referenceImageType(*layer->props.reference->bytes) == "png");
    CHECK(w.canvasView()->referencePictureSize(layer->props.id) == source.size());
    const QImage kept = QImage::fromData(QByteArray::fromStdString(*layer->props.reference->bytes));
    REQUIRE_FALSE(kept.isNull());
    CHECK(kept.pixelColor(8, 8) == source.pixelColor(8, 8));
}

TEST_CASE("reference decoding respects and restores caller allocation limits", "[reference]") {
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    const QByteArray ico = pngIcon(QByteArray::fromStdString(*encoded(filled(1024, 512, Qt::blue))));
    for (const int allocation : {0, 1, 128, 512}) {
        INFO(allocation);
        QImageReader::setAllocationLimit(allocation);
        const QImage image = readReferencePicture(ico, CanvasView::kMaxReferencePixels);
        CHECK(image.isNull() == (allocation == 1));
        CHECK(QImageReader::allocationLimit() == allocation);
        CHECK(readReferencePicture(ico.left(22), CanvasView::kMaxReferencePixels).isNull());
        CHECK(QImageReader::allocationLimit() == allocation);
    }
}

TEST_CASE("concurrent reference decoding restores the process allocation limit", "[reference]") {
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImageReader::setAllocationLimit(512);
    const QByteArray png = QByteArray::fromStdString(*encoded(filled(16, 16, Qt::red)));
    const QByteArray ico = pngIcon(png);
    std::vector<std::future<bool>> jobs;
    for (int i = 0; i < 8; ++i) {
        jobs.push_back(std::async(std::launch::async, [png, ico] {
            for (int j = 0; j < 32; ++j) {
                if (readReferencePicture(j % 2 ? png : ico, CanvasView::kMaxReferencePixels).size() != QSize(16, 16))
                    return false;
                if (!readReferencePicture(ico.left(22), CanvasView::kMaxReferencePixels).isNull())
                    return false;
            }
            return true;
        }));
    }
    for (auto& job : jobs)
        CHECK(job.get());
    CHECK(QImageReader::allocationLimit() == 512);
}

TEST_CASE("reference decoding refuses an unenforceable environment allocation limit", "[reference]") {
    // Qt caches QT_IMAGEIO_MAXALLOC at first use, so exercise each override
    // in a fresh process rather than changing the current test's environment.
    if (qEnvironmentVariableIsSet("BW_TEST_REFERENCE_ALLOCATION_CHILD")) {
        const int allocation = qEnvironmentVariableIntValue("QT_IMAGEIO_MAXALLOC");
        const QByteArray png = QByteArray::fromStdString(*encoded(filled(16, 16, Qt::green)));
        QString error;
        const QImage image = readReferencePicture(pngIcon(png), CanvasView::kMaxReferencePixels, nullptr, &error);
        CHECK(image.isNull() == (allocation == 0 || allocation > 256));
        if (image.isNull())
            CHECK(error.contains(QStringLiteral("allocation limit")));
        CHECK(QImageReader::allocationLimit() == allocation);
        // A format whose own header bounds decoding remains usable.
        CHECK_FALSE(readReferencePicture(png, CanvasView::kMaxReferencePixels).isNull());
        // Bytes Qt's XBM detector would decode in full, 128 MiB here, are
        // refused before detection when the cap can't be enforced.
        const bool measured = resetPeakMemory();
        const qint64 before = peakMemoryKiB();
        error.clear();
        CHECK(readReferencePicture(xbmClaiming(32767, 32767), CanvasView::kMaxReferencePixels, nullptr, &error)
                  .isNull());
        if (allocation == 0 || allocation > 256)
            CHECK(error.contains(QStringLiteral("allocation limit")));
        if (measured)
            CHECK(peakMemoryKiB() - before < 32 * 1024);
        return;
    }
    for (const char* allocation : {"0", "512", "1"}) {
        INFO(allocation);
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_IMAGEIO_MAXALLOC"), QString::fromLatin1(allocation));
        environment.insert(QStringLiteral("BW_TEST_REFERENCE_ALLOCATION_CHILD"), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("reference decoding refuses an unenforceable environment allocation limit")});
        REQUIRE(child.waitForFinished(30000));
        INFO(child.readAllStandardOutput().toStdString());
        INFO(child.readAllStandardError().toStdString());
        CHECK(child.exitStatus() == QProcess::NormalExit);
        CHECK(child.exitCode() == 0);
    }
}

TEST_CASE("oversized reference import reports refusal and project rendering skips the picture", "[reference]") {
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImageReader::setAllocationLimit(512);
    QImage source(8193, 8192, QImage::Format_Grayscale8);
    source.fill(200);
    const auto bytes = encoded(source);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("large.png"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes->data(), qint64(bytes->size())) == qint64(bytes->size()));
    file.close();

    MainWindow w;
    Document* d = w.document();
    const auto before = d->project().layers;
    const int steps = d->undoStack()->count();
    QString error;
    CHECK_FALSE(w.loadReferenceImage(path, &error));
    CHECK(error.contains(QStringLiteral("too large to decode")));
    CHECK_FALSE(w.statusBar()->currentMessage().isEmpty());
    CHECK(w.statusBar()->currentMessage().contains(error));
    CHECK(d->project().layers == before);
    CHECK(d->undoStack()->count() == steps);

    // A project may still hold the original bytes. The view must skip them,
    // remember the failure, and leave the art visible without repeated reads.
    const LayerId id = d->addReferenceLayer(ReferenceImage{bytes, 0, 0, 32, 32, 100});
    REQUIRE(id);
    CanvasView* v = w.canvasView();
    v->setZoom(1);
    int failures = 0;
    QObject::connect(v, &CanvasView::referenceFailed, &w, [&] { ++failures; });
    const QImage shot = v->grab(QRect(0, 0, 16, 16)).toImage();
    CHECK(v->referencePictureSize(id).isEmpty());
    CHECK(v->referenceCacheSize(id).isEmpty());
    CHECK(v->referencePictureCount() == 1);
    CHECK(shot.pixelColor(8, 8) == QColor(Qt::black));
    CHECK(failures == 1);
    v->grab(QRect(0, 0, 16, 16));
    CHECK(failures == 1);
}

namespace {

// A one-page PDF, 1024 x 1024 points: the Qt PDF image plugin renders it
// past any allocation limit.
const QByteArray kPdfPage =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n"
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 1024 1024] >>\n"
    "endobj\n"
    "xref\n0 4\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "trailer\n<< /Size 4 /Root 1 0 R >>\n"
    "startxref\n188\n%%EOF\n";

// A 16 x 16 SVG whose only content is kPdfPage, as an <image> data URL.
// The SVG handler decodes that through QImage::fromData, whatever its format.
QByteArray svgEmbeddingPdf() {
    return QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" "
                      "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
                      "width=\"16\" height=\"16\" viewBox=\"0 0 16 16\">"
                      "<image width=\"16\" height=\"16\" "
                      "xlink:href=\"data:application/pdf;base64,") +
           kPdfPage.toBase64() + "\"/></svg>";
}

// `data` as a gzip member (one stored deflate block), as an .svgz holds it.
QByteArray gzipped(const QByteArray& data) {
    REQUIRE(data.size() <= 0xffff);
    quint32 crc = 0xffffffff;
    for (const char c : data) {
        crc ^= quint8(c);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
    }
    crc ^= 0xffffffff;
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s << quint8(0x1f) << quint8(0x8b) << quint8(8) << quint8(0) << quint32(0) << quint8(0) << quint8(255);
    s << quint8(1) << quint16(data.size()) << quint16(~quint16(data.size()));
    s.writeRawData(data.constData(), int(data.size()));
    s << crc << quint32(data.size());
    return out;
}

}  // namespace

TEST_CASE("reference decoding refuses an SVG or SVGZ that embeds a PDF", "[reference]") {
    const auto formats = QImageReader::supportedImageFormats();
    if (!formats.contains(QByteArray("svg")) || !formats.contains(QByteArray("pdf")))
        SKIP("Qt SVG or PDF image plugin is unavailable");
    QByteArray bytes = svgEmbeddingPdf();
    QByteArray format = "svg";
    SECTION("SVG") {}
    SECTION("SVGZ") {
        bytes = gzipped(bytes);
        format = "svgz";
    }
    QBuffer input(&bytes);
    REQUIRE(input.open(QIODevice::ReadOnly));
    REQUIRE(QImageReader(&input).format() == format);

    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    // The embedded 1024 x 1024 page needs 4 MiB; the outer SVG only 1 KiB.
    QImageReader::setAllocationLimit(1);
    QString error;
    const QImage image = readReferencePicture(bytes, CanvasView::kMaxReferencePixels, nullptr, &error);
    CHECK(image.isNull());
    CHECK(error == QStringLiteral("%1 pictures cannot be decoded safely as a reference image.")
                       .arg(QString::fromLatin1(format.toUpper())));
    CHECK(QImageReader::allocationLimit() == 1);
}

TEST_CASE("reference PDF decoding respects a stricter allocation limit", "[reference]") {
    if (!QImageReader::supportedImageFormats().contains(QByteArray("pdf")))
        SKIP("Qt PDF image plugin is unavailable");

    const QByteArray pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n"
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 1024 1024] >>\n"
        "endobj\n"
        "xref\n0 4\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000115 00000 n \n"
        "trailer\n<< /Size 4 /Root 1 0 R >>\n"
        "startxref\n188\n%%EOF\n";

    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] {
        QImageReader::setAllocationLimit(previous);
    });
    QImageReader::setAllocationLimit(1);
    REQUIRE(QImageReader::allocationLimit() == 1);

    QString error;
    const QImage image = readReferencePicture(
        pdf, CanvasView::kMaxReferencePixels, nullptr, &error);

    CHECK(image.isNull()); // Fails: returns 1024×1024, 4194304 bytes.
    CHECK(QImageReader::allocationLimit() == 1);
}

namespace {

// `image` encoded as `format`, with Qt's own writer.
QByteArray written(const QImage& image, const char* format) {
    QByteArray bytes;
    QBuffer out(&bytes);
    REQUIRE(out.open(QIODevice::WriteOnly));
    QImageWriter writer(&out, format);
    INFO(writer.errorString().toStdString());
    REQUIRE(writer.write(image));
    return bytes;
}

QByteArray detected(QByteArray bytes) {
    QBuffer input(&bytes);
    REQUIRE(input.open(QIODevice::ReadOnly));
    return QImageReader(&input).format();
}

}  // namespace

TEST_CASE("reference decoding reads each allowed format but not past the allocation limit", "[reference]") {
    const auto readable = QImageReader::supportedImageFormats();
    const auto writable = QImageWriter::supportedImageFormats();
    for (const char* format : {"png", "jpeg", "bmp", "gif", "webp", "tiff", "ico", "pbm", "pgm", "ppm", "xbm", "xpm"}) {
        // GIF has no writer in Qt; the others depend on the plugins installed.
        if (!readable.contains(QByteArray(format)) || !writable.contains(QByteArray(format)))
            continue;
        DYNAMIC_SECTION(format) {
            const int previous = QImageReader::allocationLimit();
            const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });

            const QByteArray small = written(filled(8, 8, Qt::white), format);
            REQUIRE(detected(small) == QByteArray(format));
            QSize original;
            QString error;
            const QImage image = readReferencePicture(small, CanvasView::kMaxReferencePixels, &original, &error);
            INFO(error.toStdString());
            CHECK(image.size() == QSize(8, 8));
            CHECK(original == QSize(8, 8));

            // 1024 x 1024 needs 4 MiB; Qt's writer keeps an ICO to 256 px,
            // so that one carries the big picture as a PNG inside.
            const QImage big = filled(1024, 1024, Qt::white);
            const QByteArray large = QByteArray(format) == "ico"
                                         ? pngIcon(QByteArray::fromStdString(*encoded(big)))
                                         : written(big, format);
            REQUIRE(detected(large) == QByteArray(format));
            CHECK(readReferencePicture(large, CanvasView::kMaxReferencePixels).size() == big.size());
            // The XBM handler stops recognising its bytes; the others fail
            // to allocate the picture.
            QImageReader::setAllocationLimit(1);
            error.clear();
            CHECK(readReferencePicture(large, CanvasView::kMaxReferencePixels, nullptr, &error).isNull());
            CHECK_FALSE(error.isEmpty());
            CHECK(QImageReader::allocationLimit() == 1);
        }
    }
}

namespace {

QStringList* imageWarnings = nullptr;

void collectImageWarnings(QtMsgType, const QMessageLogContext& context, const QString& message) {
    if (imageWarnings && QByteArray(context.category ? context.category : "") == "qt.gui.imageio")
        imageWarnings->append(message);
}

}  // namespace

TEST_CASE("reference decoding detects the format under its allocation cap", "[reference]") {
    // 16384 x 8192 px is 512 MiB at the 4 bytes a pixel Qt accounts: twice
    // the reference cap, but within a looser caller limit.
    const QByteArray claimed = xbmClaiming(16384, 8192);
    REQUIRE(claimed.size() == 75);
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });

    // The limit as the app leaves it.
    QString error;
    CHECK(readReferencePicture(claimed, CanvasView::kMaxReferencePixels, nullptr, &error).isNull());
    CHECK_FALSE(error.isEmpty());
    CHECK(QImageReader::allocationLimit() == previous);

    // A looser caller limit matters only if detection runs before the cap
    // is installed: Qt then allocates the raster without complaint. Under
    // the cap, Qt's XBM detector is refused the raster at 256 MiB.
    QImageReader::setAllocationLimit(512);
    QStringList warnings;
    imageWarnings = &warnings;
    const QtMessageHandler handler = qInstallMessageHandler(collectImageWarnings);
    error.clear();
    const QImage image = readReferencePicture(claimed, CanvasView::kMaxReferencePixels, nullptr, &error);
    qInstallMessageHandler(handler);
    imageWarnings = nullptr;
    CHECK(image.isNull());
    CHECK_FALSE(error.isEmpty());
    INFO(warnings.join(QLatin1Char('\n')).toStdString());
    CHECK(warnings.filter(QStringLiteral("allocation limit of 256 megabytes")).size() == 1);
    CHECK(QImageReader::allocationLimit() == 512);

    // With no caller limit at all, detection before the cap fills a 128 MiB
    // raster for a 32767 x 32767 claim. Measured as the peak resident set.
    QImageReader::setAllocationLimit(0);
    if (!resetPeakMemory())
        SKIP("the peak resident set can't be reset or read here");
    const qint64 before = peakMemoryKiB();
    CHECK(readReferencePicture(xbmClaiming(32767, 32767), CanvasView::kMaxReferencePixels).isNull());
    CHECK(peakMemoryKiB() - before < 32 * 1024);
    CHECK(QImageReader::allocationLimit() == 0);
}

TEST_CASE("reference decoding refuses SVG SVGZ ICNS PDF and unrecognised bytes before decoding", "[reference]") {
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImageReader::setAllocationLimit(512);
    const auto readable = QImageReader::supportedImageFormats();
    const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
                           "<rect width=\"8\" height=\"8\" fill=\"white\"/></svg>";
    std::vector<std::pair<QByteArray, QByteArray>> refused;  // format, bytes
    if (readable.contains(QByteArray("svg"))) {
        refused.emplace_back("svg", svg);
        refused.emplace_back("svgz", gzipped(svg));
    }
    if (readable.contains(QByteArray("icns")) && QImageWriter::supportedImageFormats().contains(QByteArray("icns")))
        refused.emplace_back("icns", written(filled(16, 16, Qt::white), "icns"));
    if (readable.contains(QByteArray("pdf")))
        refused.emplace_back("pdf", kPdfPage);
    for (const auto& [format, bytes] : refused) {
        INFO(format.toStdString());
        REQUIRE(detected(bytes) == format);
        QString error;
        CHECK(readReferencePicture(bytes, CanvasView::kMaxReferencePixels, nullptr, &error).isNull());
        CHECK(error == QStringLiteral("%1 pictures cannot be decoded safely as a reference image.")
                           .arg(QString::fromLatin1(format.toUpper())));
        CHECK(QImageReader::allocationLimit() == 512);
    }

    // Bytes no handler recognises are refused before any limit is touched.
    QString error;
    CHECK(readReferencePicture(QByteArray("not a picture at all"), CanvasView::kMaxReferencePixels, nullptr, &error)
              .isNull());
    CHECK(error.contains(QStringLiteral("not recognised")));
    CHECK(QImageReader::allocationLimit() == 512);
}

TEST_CASE("Open Reference Image refuses a PDF without adding a layer", "[reference]") {
    if (!QImageReader::supportedImageFormats().contains(QByteArray("pdf")))
        SKIP("Qt PDF image plugin is unavailable");
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImageReader::setAllocationLimit(1);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // The suffix claims PNG; the bytes decide.
    const QString path = dir.filePath(QStringLiteral("page.png"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(kPdfPage) == kPdfPage.size());
    file.close();

    MainWindow w;
    Document* d = w.document();
    const auto before = d->project().layers;
    const int steps = d->undoStack()->count();
    QString error;
    CHECK_FALSE(w.loadReferenceImage(path, &error));
    CHECK(error.contains(QStringLiteral("PDF pictures cannot be decoded safely")));
    CHECK(w.statusBar()->currentMessage().contains(error));
    CHECK(d->project().layers == before);
    CHECK(d->undoStack()->count() == steps);
    CHECK(QImageReader::allocationLimit() == 1);
}

TEST_CASE("Open Reference Image refuses an SVG without adding a layer", "[reference]") {
    if (!QImageReader::supportedImageFormats().contains(QByteArray("svg")))
        SKIP("Qt SVG image plugin is unavailable");
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImageReader::setAllocationLimit(512);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("picture.svg"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    const QByteArray svg = svgEmbeddingPdf();
    REQUIRE(file.write(svg) == svg.size());
    file.close();

    MainWindow w;
    Document* d = w.document();
    const auto before = d->project().layers;
    const int steps = d->undoStack()->count();
    QString error;
    CHECK_FALSE(w.loadReferenceImage(path, &error));
    CHECK(error.contains(QStringLiteral("SVG pictures cannot be decoded safely")));
    CHECK(w.statusBar()->currentMessage().contains(error));
    CHECK(d->project().layers == before);
    CHECK(d->undoStack()->count() == steps);
    CHECK(QImageReader::allocationLimit() == 512);
}
