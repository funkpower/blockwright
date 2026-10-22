// The GUI's atlas renderer: it must draw exactly what renderCanvas (the
// CLI's PNG renderer) draws, at any zoom, and repaint only dirty cells.

#include "util.h"

#include "blockwright/canvas_renderer.h"

#include <catch2/catch_test_macros.hpp>

#include <QPainter>

#include <algorithm>

using namespace bw;
namespace fs = std::filesystem;

namespace {

// What the renderer paints for the whole canvas at `zoom`, into an RGB32
// image like renderCanvas's.
QImage paintAll(CanvasRenderer& r, int zoom) {
    QImage img(r.imageSize() * zoom, QImage::Format_RGB32);
    img.fill(Qt::magenta);
    QPainter p(&img);
    r.paint(p, QRect(QPoint(0, 0), r.imageSize()), QPoint(0, 0), zoom);
    return img;
}

// The first pixel where two images differ, for a readable failure.
std::string firstDiff(const QImage& a, const QImage& b) {
    if (a.size() != b.size())
        return "sizes differ";
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y))
                return "first difference at " + std::to_string(x) + "," + std::to_string(y);
    return "same";
}

bool same(const QImage& a, const QImage& b) {
    return a.convertToFormat(QImage::Format_RGB32) == b.convertToFormat(QImage::Format_RGB32);
}

} // namespace

TEST_CASE("the atlas renderer draws a known cell like renderCanvas, at zoom 1 and 3") {
    Canvas cv(3, 2);
    cv.at(0, 0) = Cell{U'A', Color::indexed(Yellow), Color::indexed(Blue), 0, false};
    cv.at(1, 0) = Cell{U'▓', Color::indexed(LightCyan), Color::indexed(Red), 0, false};
    cv.at(2, 1) = Cell{U'─', Color::indexed(White), Color::indexed(Magenta), 0, false};  // 0xC4: 9th column repeats
    cv.at(1, 1) = Cell{U'@', Color::fromRgb({0x12, 0x34, 0x56}), Color::fromRgb({0xfe, 0xdc, 0xba}), 0, false};
    for (int gw : {8, 9}) {
        AtlasRenderer r;
        r.setCanvas(&cv, gw);
        CHECK(r.cellSize() == QSize(gw, 16));
        CHECK(r.imageSize() == QSize(3 * gw, 32));
        for (int zoom : {1, 3}) {
            INFO("width " << gw << " zoom " << zoom);
            const QImage want = renderCanvas(cv, gw, zoom);
            const QImage got = paintAll(r, zoom);
            INFO(firstDiff(got, want));
            CHECK(same(got, want));
        }
    }
}

TEST_CASE("the known cell's pixels") {
    // 'A' in the VGA font: row 2 (0-based) is 0x10, one pixel at x = 3.
    Canvas cv(1, 1);
    cv.at(0, 0) = Cell{U'A', Color::indexed(Yellow), Color::indexed(Blue), 0, false};
    AtlasRenderer r;
    r.setCanvas(&cv, 8);
    const QImage img = paintAll(r, 3);
    const QRgb fg = qRgb(0xff, 0xff, 0x55), bg = qRgb(0x00, 0x00, 0xaa);
    CHECK(img.pixel(0, 0) == bg);
    CHECK(img.pixel(3 * 3, 2 * 3) == fg);
    CHECK(img.pixel(3 * 3 + 2, 2 * 3 + 2) == fg);
    CHECK(img.pixel(3 * 2 + 2, 2 * 3) == bg);
}

TEST_CASE("the atlas renderer agrees with renderCanvas on the golden pieces") {
    std::vector<fs::path> pieces;
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden"))
        if (e.path().extension() == ".ans")
            pieces.push_back(e.path());
    std::sort(pieces.begin(), pieces.end());
    REQUIRE(pieces.size() >= 10);
    for (const auto& path : pieces) {
        DYNAMIC_SECTION(path.filename().string()) {
            const AnsiDocument doc = parseAnsi(bwtest::readFile(path));
            for (int gw : {8, 9}) {
                AtlasRenderer r;
                r.setCanvas(&doc.canvas, gw);
                for (int zoom : {1, 2}) {
                    INFO("width " << gw << " zoom " << zoom);
                    CHECK(same(paintAll(r, zoom), renderCanvas(doc.canvas, gw, zoom)));
                }
            }
        }
    }
}

TEST_CASE("painting part of the canvas draws that part") {
    const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"));
    AtlasRenderer r;
    r.setCanvas(&doc.canvas, 8);
    const QImage full = renderCanvas(doc.canvas, 8, 3);
    const QRect src(13, 21, 40, 30);  // not on cell boundaries
    QImage img(src.size() * 3, QImage::Format_RGB32);
    QPainter p(&img);
    r.paint(p, src, QPoint(0, 0), 3);
    p.end();
    CHECK(same(img, full.copy(QRect(src.topLeft() * 3, src.size() * 3))));
}

TEST_CASE("dirty tracking repaints only the cells marked") {
    Canvas cv(10, 5);
    AtlasRenderer r;
    r.setCanvas(&cv, 8);
    CHECK(r.dirtyCells() == QRegion(0, 0, 10, 5));  // a new canvas is all dirty
    CHECK(r.flush() == QRegion(0, 0, 10, 5));
    CHECK(r.dirtyCells().isEmpty());
    CHECK(r.flush().isEmpty());

    // Change two cells but mark only one: only that one is repainted.
    cv.at(3, 2) = Cell{U'X', Color::indexed(White), Color::indexed(Red), 0, false};
    cv.at(7, 4) = Cell{U'Y', Color::indexed(White), Color::indexed(Green), 0, false};
    r.markDirty(QRegion(3, 2, 1, 1));
    CHECK(r.dirtyCells() == QRegion(3, 2, 1, 1));
    CHECK(r.flush() == QRegion(3, 2, 1, 1));
    const QImage want = renderCanvas(cv, 8);
    const QImage& cache = r.cache();
    CHECK(same(cache.copy(3 * 8, 2 * 16, 8, 16), want.copy(3 * 8, 2 * 16, 8, 16)));
    CHECK_FALSE(same(cache.copy(7 * 8, 4 * 16, 8, 16), want.copy(7 * 8, 4 * 16, 8, 16)));  // not marked: stale
    CHECK(same(cache.copy(7 * 8, 4 * 16, 8, 16), renderCanvas(Canvas(1, 1), 8)));

    r.markDirty(QRegion(7, 4, 1, 1));
    r.flush();
    CHECK(same(r.cache(), want));

    // Cells off the canvas are ignored.
    r.markDirty(QRegion(8, 3, 5, 5));
    CHECK(r.dirtyCells() == QRegion(8, 3, 2, 2));
}

TEST_CASE("the atlas renderer refuses a bad width or a canvas too large to cache") {
    Canvas cv(2, 2);
    AtlasRenderer r;
    CHECK_THROWS_AS(r.setCanvas(&cv, 7), std::invalid_argument);
    Canvas huge(kMaxColumns, kMaxRows);
    CHECK_THROWS_AS(r.setCanvas(&huge, 9), std::length_error);
    // a refused canvas leaves the renderer as it was
    r.setCanvas(&cv, 8);
    CHECK_THROWS(r.setCanvas(&huge, 9));
    CHECK(r.imageSize() == QSize(16, 32));
}

TEST_CASE("an equal-sized canvas reuses the cache and redraws all of it") {
    Canvas a(3, 2), b(3, 2);
    b.at(1, 1) = Cell{U'A', Color::indexed(Yellow), Color::indexed(Blue), 0, false};
    AtlasRenderer r;
    r.setCanvas(&a, 8);
    r.flush();
    const qint64 key = r.cache().cacheKey();
    r.setCanvas(&b, 8);
    CHECK(r.cache().cacheKey() == key);
    CHECK(r.dirtyCells() == QRegion(0, 0, 3, 2));
    r.flush();
    CHECK(r.cache() == renderCanvas(b, 8).convertToFormat(r.cache().format()));
}
