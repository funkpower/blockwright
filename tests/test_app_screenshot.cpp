// blockwright --screenshot (#295, PLAN §9): a steps file replayed in the
// window, each step's "screenshot" grabbed from it. What is tested is the
// structure, which no font or theme changes: the pictures made and their
// names, each the size of what it shows, a crop's geometry, and the canvas
// in a picture drawn exactly as the replay's canvas renders. main() is in
// test_app.cpp.

#include "canvas_view.h"
#include "draw_tools.h"
#include "irc_meter.h"
#include "lint_dock.h"
#include "main_window.h"
#include "palette_widget.h"
#include "replay.h"
#include "read_file.h"
#include "screenshot.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/brushes/clone.h"
#include "blockwright/canvas_size.h"
#include "blockwright/composite.h"
#include "blockwright/irc_render.h"
#include "blockwright/ramp.h"
#include "blockwright/render.h"
#include "blockwright/steps.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QRawFont>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>

#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#include <csignal>
#endif

#include <filesystem>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <optional>
#include <vector>

using namespace bw;
namespace fs = std::filesystem;

namespace {

const fs::path kTutorial = fs::path(BW_TEST_DATA) / "screenshot" / "tutorial.json";

steps::Script tutorial() { return steps::parse(bwcli::readFile(kTutorial.string())); }

project::Project startOf(const steps::Script& s) { return bwcli::replay::start(s, kTutorial.parent_path(), {}); }

const screenshot::Shot& shotNamed(const std::vector<screenshot::Shot>& shots, const std::string& stem) {
    for (const screenshot::Shot& s : shots)
        if (s.stem == stem)
            return s;
    FAIL("no shot " << stem);
    return shots.front();
}

// The canvas the CLI's replay leaves after the capture `stem`, rendered as
// the window's renderer draws it, at `zoom`.
QImage replayed(const steps::Script& s, const std::string& stem, int zoom) {
    const bwcli::replay::Outcome out = bwcli::replay::run(s, startOf(s));
    for (const bwcli::replay::Capture& c : out.captures)
        if (c.stem == stem)
            return renderCanvas(composite(c.project.layers, {c.project.ice, Cell{}}), 8, zoom)
                .convertToFormat(QImage::Format_RGB32);
    FAIL("no capture " << stem);
    return {};
}

// The pixels of cells `cells` in a picture of `of`, as the window shows them.
QRect cellsIn(MainWindow& w, QWidget* of, const QRect& cells) {
    CanvasView* v = w.canvasView();
    const QRect r = v->toWidget(v->cellRect(cells.topLeft()).united(v->cellRect(cells.bottomRight())));
    return QRect(v->mapTo(of, r.topLeft()), r.size());
}

// A script whose one step draws a stroke lint finds fault with (a glyph no
// block art uses, on a bright background) and takes `of`.
steps::Script strokeShowing(const char* of) {
    return steps::parse(std::string(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
        {"name": "stroke", "action": "glyph", "glyph": "#", "fg": 14, "bg": 9,
         "points": [[2, 2], [3, 2], [4, 2]], "screenshot": {"of": ")") + of + R"("}}]})");
}

std::string failureOf(const steps::Script& s) {
    MainWindow w;
    try {
        screenshot::take(w, s, startOf(s));
    } catch (const bwcli::Failure& f) {
        return f.message;
    }
    return "no failure";
}

// The pictures of a stroke, a half-block click and a select drag at
// `scale`, with "pointer": `pointer` on each, or no "pointer" key at all
// when `pointer` is empty.
std::vector<screenshot::Shot> pointerShots(int scale, const char* pointer) {
    const std::string extra = std::string(pointer ? std::string(", \"pointer\": ") + pointer : "") +
                              ", \"scale\": " + std::to_string(scale);
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
        {"name": "stroke", "action": "glyph", "glyph": "#", "fg": 14, "bg": 1, "points": [[2, 2], [3, 2], [6, 4]],
         "screenshot": {"when": "both")" + extra + R"(}},
        {"name": "dot", "action": "halfBlock", "fg": 12, "points": [[8, 3]],
         "screenshot": {"crop": {"cells": [5, 0, 6, 4]})" + extra + R"(}},
        {"name": "area", "action": "select", "rect": [1, 1, 4, 3],
         "screenshot": {"of": "canvasView")" + extra + R"(}}]})");
    MainWindow w;
    return screenshot::take(w, s, startOf(s));
}

// With "pointer": true each picture differs from the one without it, and
// only in the arrow's box around its tip; the tip is where pointerAt says,
// in the picture's logical pixels.
void checkPointer(int scale) {
    const std::vector<screenshot::Shot> on = pointerShots(scale, "true");
    const std::vector<screenshot::Shot> off = pointerShots(scale, "false");
    const std::vector<screenshot::Shot> plain = pointerShots(scale, nullptr);
    REQUIRE(on.size() == 4);
    REQUIRE(off.size() == 4);
    REQUIRE(plain.size() == 4);
    for (std::size_t i = 0; i < on.size(); ++i) {
        INFO(on[i].stem);
        // without it, the same bytes as a file that doesn't name it
        CHECK_FALSE(off[i].pointer);
        CHECK(off[i].image == plain[i].image);
        REQUIRE(on[i].pointer);
        const QImage a = on[i].image.convertToFormat(QImage::Format_RGB32);
        const QImage b = off[i].image.convertToFormat(QImage::Format_RGB32);
        REQUIRE(a.size() == b.size());
        CHECK(a.devicePixelRatio() == scale);
        const QPointF tip = *on[i].pointer;
        const QRect box(QPoint(int(std::floor(tip.x())), int(std::floor(tip.y()))) * scale +
                            screenshot::pointerBounds().topLeft() * scale,
                        screenshot::pointerBounds().size() * scale);
        QRect changed;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                if (a.pixel(x, y) != b.pixel(x, y))
                    changed |= QRect(x, y, 1, 1);
        CAPTURE(tip.x(), tip.y(), changed.x(), changed.y(), changed.width(), changed.height());
        CHECK_FALSE(changed.isEmpty());
        CHECK(box.contains(changed));
        // the arrow's tip pixel is drawn, black or its white edge
        const QRgb at = a.pixel(QPoint(int(tip.x()), int(tip.y())) * scale + QPoint(scale / 2, scale / 2));
        CHECK(at != b.pixel(QPoint(int(tip.x()), int(tip.y())) * scale + QPoint(scale / 2, scale / 2)));
    }
}

} // namespace

TEST_CASE("a pointer is drawn at the step's point, and only around it (#295)", "[screenshot]") {
    checkPointer(1);
    // where: the stroke's first cell before it and its last after it, the
    // middle of the lower half of the half-block pixel's cell, the select
    // rect's bottom-right corner
    const std::vector<screenshot::Shot> on = pointerShots(1, "true");
    MainWindow w;
    const int zoom = w.canvasView()->zoom();
    const QSize cell(8 * zoom, 16 * zoom);
    CHECK(*on[2].pointer == QPointF((8 - 5) * cell.width() + cell.width() / 2.0, 1 * cell.height() + cell.height() * 3 / 4.0));
    CHECK(*on[3].pointer == QPointF(4 * cell.width() + cell.width() / 2.0, 3 * cell.height() + cell.height() / 2.0));
    // the stroke's two pictures: the tip moved from its first cell to its last
    const QPointF moved = *on[1].pointer - *on[0].pointer;
    CHECK(moved == QPointF(4 * cell.width(), 2 * cell.height()));
}

TEST_CASE("a pointer is drawn at twice the pixels at scale 2 (#295)", "[.][screenshot-scale]") {
    checkPointer(2);
    // the same place in the picture's logical pixels as at scale 1
    const std::vector<screenshot::Shot> one = pointerShots(1, "true"), two = pointerShots(2, "true");
    for (std::size_t i = 0; i < one.size(); ++i)
        CHECK(*one[i].pointer == *two[i].pointer);
    // drawn at the ratio: not the scale 1 arrow enlarged
    const QImage a = one[2].image.convertToFormat(QImage::Format_RGB32);
    CHECK(two[2].image.convertToFormat(QImage::Format_RGB32) !=
          a.scaled(a.size() * 2, Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

TEST_CASE("a pointer off the picture or out of view names the step (#295)", "[screenshot]") {
    const auto failing = [](const char* step) {
        return failureOf(steps::parse(std::string(R"({"version": 1, "start": {"blank": [200, 100]}, "window": [1024, 720], "steps": [)") +
                                      step + "]}"));
    };
    CHECK(failing(R"({"name": "dock", "action": "glyph", "points": [[1, 1]], "screenshot": {"of": "layers", "pointer": true}})") ==
          "step 1 (dock): a pointer needs \"of\" to hold the canvas, and \"layers\" doesn't");
    CHECK(failing(R"({"name": "far", "action": "glyph", "points": [[190, 90]], "screenshot": {"pointer": true}})") ==
          "step 1 (far): the pointer's cell [190, 90] is not in view in the 1024 x 720 window");
    CHECK(failing(R"({"name": "cropped", "action": "glyph", "points": [[1, 1], [9, 1]],
                    "screenshot": {"crop": {"cells": [0, 0, 4, 4]}, "pointer": true}})") ==
          "step 1 (cropped): the pointer's cell [9, 1] is outside the picture");
    // a point off the canvas, before the replay refuses it
    CHECK(failing(R"({"name": "off", "action": "glyph", "points": [[250, 1]], "screenshot": {"pointer": true, "when": "before"}})") ==
          "step 1 (off): the pointer's cell [250, 1] is off the 200 x 100 canvas");
    // a pixel's canvas is given in pixels, as the replay gives it
    CHECK(failing(R"({"name": "off", "action": "halfBlock", "points": [[5, 217]], "screenshot": {"pointer": true, "when": "before"}})") ==
          "step 1 (off): the pointer's pixel [5, 217] is off the 200 x 200 pixel canvas");
}

TEST_CASE("a pointer on the picture's right or bottom edge is outside it (#295)", "[screenshot]") {
    // at zoom 3, scrolled to cell [3, 1], pixel [7, 5]'s hotspot is the
    // canvas view's (180, 132): a crop 180 wide or 132 high ends on it
    const auto shot = [](const char* crop) {
        return failureOf(steps::parse(std::string(R"({"version": 1, "start": {"blank": [60, 30]}, "window": [1024, 720], "steps": [
            {"name": "look", "action": "view", "zoom": 3, "scrollTo": [3, 1]},
            {"name": "dot", "action": "halfBlock", "fg": 12, "points": [[7, 5]],
             "screenshot": {"of": "canvasView", "pointer": true, "crop": {"pixels": )") + crop + "}}}]}"));
    };
    CHECK(shot("[0, 0, 180, 200]") == "step 2 (dot): the pointer's pixel [7, 5] is outside the picture");
    CHECK(shot("[100, 0, 200, 132]") == "step 2 (dot): the pointer's pixel [7, 5] is outside the picture");
    CHECK(shot("[0, 0, 181, 200]") == "no failure");
    CHECK(shot("[100, 0, 200, 133]") == "no failure");
}

TEST_CASE("the UI font of blockwright --screenshot is the bundled Noto Sans, whatever the machine has (#295)", "[screenshot][ui-font]") {
    const QFont saved = QApplication::font();
    // the app doesn't load it unless --screenshot asks
    CHECK_FALSE(QFontDatabase::families().contains(QString::fromLatin1(screenshot::kUiFontFamily)));
    CHECK(QApplication::font().family() != QLatin1String(screenshot::kUiFontFamily));
    REQUIRE(screenshot::useBundledFont().empty());
    const QFont f = QApplication::font();
    CHECK(QFontInfo(f).family() == QLatin1String(screenshot::kUiFontFamily));
    CHECK(f.pointSize() == screenshot::kUiFontPointSize);
    CHECK(f.hintingPreference() == QFont::PreferVerticalHinting);
    // each style drawn from the bundled file, not a Noto Sans on the machine
    for (const char* file : {":/ui-font/NotoSans-Regular.ttf", ":/ui-font/NotoSans-Bold.ttf"}) {
        INFO(file);
        QFont styled = f;
        styled.setBold(QByteArray(file).contains("Bold"));
        const QByteArray want = QRawFont(QString::fromLatin1(file), 10).fontTable("head");
        REQUIRE_FALSE(want.isEmpty());
        CHECK(QRawFont::fromFont(styled).fontTable("head") == want);
        CHECK(QRawFont::fromFont(styled).familyName() == QLatin1String(screenshot::kUiFontFamily));
    }
    // a widget made now takes it
    MainWindow w;
    CHECK(QFontInfo(w.toolOptionsDock()->font()).family() == QLatin1String(screenshot::kUiFontFamily));
    QApplication::setFont(saved);
}

TEST_CASE("the font renamer keeps a font's length and refuses a name of another length (#295)", "[ui-font]") {
    QFile file(QStringLiteral(":/ui-font/NotoSans-Regular.ttf"));
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray font = file.readAll();
    CHECK(screenshot::renamedFont(font, "Bw Screen").size() == font.size());
    const QByteArray renamed = screenshot::renamedFont(font, "Bw Screen");
    CHECK(renamed.contains(QByteArray("\0B\0w\0 \0S\0c\0r\0e\0e\0n", 18)));
    CHECK_FALSE(renamed.contains(QByteArray("\0N\0o\0t\0o\0 \0S\0a\0n\0s", 18)));
    CHECK(screenshot::renamedFont(font, "Short").isEmpty());
    CHECK(screenshot::renamedFont(QByteArray("not a font"), "Bw Screen").isEmpty());
}

TEST_CASE("a screenshot replay takes the window, a dock and a crop at the steps that ask", "[screenshot]") {
    const steps::Script s = tutorial();
    MainWindow w;
    const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s));

    std::vector<std::string> stems;
    for (const screenshot::Shot& shot : shots)
        stems.push_back(shot.stem);
    CHECK(stems == std::vector<std::string>{"01-pick-ramp", "02-stroke-before", "02-stroke-after", "03-ink",
                                            "04-box", "06-corner"});
    CHECK(w.size() == QSize(1024, 720));
    CHECK_FALSE(w.canvasView()->cursorShown());

    // each the size of what it shows, at the window's device pixel ratio
    const qreal dpr = w.devicePixelRatioF();
    const auto logical = [&](const QImage& i) { return (QSizeF(i.size()) / dpr).toSize(); };
    CHECK(logical(shotNamed(shots, "01-pick-ramp").image) == w.toolOptionsDock()->size());
    CHECK(logical(shotNamed(shots, "02-stroke-after").image) == QSize(1024, 720));
    CHECK(logical(shotNamed(shots, "03-ink").image) == w.layersDock()->size());
    CHECK(logical(shotNamed(shots, "06-corner").image) == QSize(120, 40));

    // a crop in cells is those cells at the view's zoom, drawn as the
    // replay's canvas renders
    const int zoom = w.canvasView()->zoom();
    const QImage box = shotNamed(shots, "04-box").image.convertToFormat(QImage::Format_RGB32);
    REQUIRE(logical(box) == QSize(6 * 8 * zoom, 3 * 16 * zoom));
    if (dpr == 1.0)
        CHECK(box == replayed(s, "04-box", zoom).copy(QRect(8 * zoom, 16 * zoom, 6 * 8 * zoom, 3 * 16 * zoom)));

    // the window's picture shows the stroke's cells as the replay left them,
    // and not before it
    if (dpr == 1.0) {
        const QRect cells(2, 2, 3, 1);
        const QRect in = cellsIn(w, &w, cells);
        const QImage want = replayed(s, "02-stroke", zoom).copy(QRect(2 * 8 * zoom, 2 * 16 * zoom, 3 * 8 * zoom, 16 * zoom));
        CHECK(shotNamed(shots, "02-stroke-after").image.convertToFormat(QImage::Format_RGB32).copy(in) == want);
        CHECK(shotNamed(shots, "02-stroke-before").image.convertToFormat(QImage::Format_RGB32).copy(in) != want);
    }

    // the window is left as the last step left it: the rectangle tool
    // chosen, its toolbox button pressed, on the new layer
    REQUIRE(w.tools()->active());
    CHECK(w.tools()->active()->id() == QStringLiteral("rectangle"));
    auto* rect = w.findChild<QAction*>(QStringLiteral("rectangleTool"));
    REQUIRE(rect);
    CHECK(rect->isChecked());
}

TEST_CASE("the same steps file takes the same pictures every run", "[screenshot]") {
    const steps::Script s = tutorial();
    std::vector<screenshot::Shot> first, second;
    {
        MainWindow w;
        first = screenshot::take(w, s, startOf(s));
    }
    {
        MainWindow w;
        second = screenshot::take(w, s, startOf(s));
    }
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        INFO(first[i].stem);
        CHECK(first[i].stem == second[i].stem);
        CHECK(first[i].image == second[i].image);
    }
}

TEST_CASE("a screenshot of what the window doesn't have, or off it, names the step", "[screenshot]") {
    const auto with = [](const char* shot) {
        return steps::parse(std::string(R"({"version": 1, "start": {"blank": [10, 4]}, "steps": [
            {"name": "first", "action": "selectAll"},
            {"name": "look", "action": "deselect", "screenshot": )") + shot + "}]}");
    };
    const std::string none = failureOf(with(R"({"of": "nope"})"));
    CHECK(none.rfind("step 2 (look): the window has no dock or widget \"nope\"", 0) == 0);
    CHECK(none.find("layersDock") != std::string::npos);

    CHECK(failureOf(with(R"({"crop": {"cells": [8, 0, 3, 1]}})")) ==
          "step 2 (look): the crop's cells [8, 0, 3, 1] are off the 10 x 4 canvas");
    CHECK(failureOf(with(R"({"of": "layers", "crop": {"cells": [0, 0, 1, 1]}})")) ==
          "step 2 (look): a crop in cells needs \"of\" to hold the canvas, and \"layers\" doesn't");
    CHECK(failureOf(with(R"({"crop": {"pixels": [1200, 0, 200, 10]}})")) ==
          "step 2 (look): the crop's pixels [1200, 0, 200, 10] are off the 1280 x 800 \"window\"");
}

TEST_CASE("a core error a step raises in the window fails at the step", "[screenshot]") {
    // #383: a start whose layer ids are exhausted, so New Cells Layer throws
    // the core's LayerError; the replay names the step with its message, and
    // the pictures before it were taken but none after it
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [10, 4]}, "steps": [
        {"name": "first", "action": "selectAll", "screenshot": {}},
        {"name": "add", "action": "newCellsLayer", "screenshot": {"when": "both"}}]})");
    project::Project start = startOf(s);
    start.layers.reserveIds(0);
    MainWindow w;
    std::vector<std::string> taken;
    std::string failure = "no failure";
    try {
        screenshot::take(w, s, std::move(start), [&](const screenshot::Shot& shot) { taken.push_back(shot.stem); });
    } catch (const bwcli::Failure& f) {
        failure = f.message;
    }
    CHECK(failure == "step 2 (add): layer ids exhausted");
    CHECK(taken == std::vector<std::string>{"01-first", "02-add-before"});
}

TEST_CASE("screenshots are written as PNGs, all of them or none", "[screenshot]") {
    const steps::Script s = tutorial();
    std::vector<screenshot::Shot> shots;
    {
        MainWindow w;
        shots = screenshot::take(w, s, startOf(s));
    }
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const fs::path out = fs::path(QFile::encodeName(dir.path()).toStdString()) / "shots";
    screenshot::write(shots, out);
    for (const screenshot::Shot& shot : shots) {
        INFO(shot.stem);
        QImage read((out / (shot.stem + ".png")).string().c_str());
        CHECK(read.size() == shot.image.size());
    }

    // a directory where a picture goes stops the write before any is moved
    // into place
    const fs::path blocked = fs::path(QFile::encodeName(dir.path()).toStdString()) / "blocked";
    fs::create_directories(blocked / "04-box.png");
    CHECK_THROWS_AS(screenshot::write(shots, blocked), bwcli::Failure);
    CHECK_FALSE(fs::exists(blocked / "01-pick-ramp.png"));
}

TEST_CASE("a dock's picture waits for its result of the step's document", "[screenshot]") {
    // each refresh's work held a while on its worker, so a picture taken
    // before the result is in shows an older one
    const auto slow = [] { QThread::msleep(300); };

    SECTION("Lint") {
        const steps::Script s = strokeShowing("lintDock");
        MainWindow w;
        LintDock* lint = w.lintWidget();
        lint->setBeforeWork(slow);
        const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s));
        REQUIRE(shots.size() == 1);
        const lint::LintReport want = dockLint(w.document()->canvas(), w.document()->ansi().ice, 0, w.document()->target());
        REQUIRE_FALSE(want.findings.empty());
        CHECK_FALSE(lint->stale());
        CHECK(lint->report().findings == want.findings);
        CHECK(shots[0].image == w.lintDock()->grab().toImage());
    }
    SECTION("IRC View, behind the Lint tab until asked for") {
        const steps::Script s = strokeShowing("ircView");
        MainWindow w;
        IrcViewDock* irc = w.ircViewWidget();
        irc->setBeforeWork(slow);
        const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s));
        REQUIRE(shots.size() == 1);
        CHECK(irc->shown());
        CHECK_FALSE(irc->stale());
        const Document& doc = *w.document();
        CHECK(irc->image() ==
              renderIrcView(irc::ircView(doc.canvas(), doc.extras().ircBudget.bytes), glyphWidth(doc.ansi())));
        CHECK(shots[0].image == w.ircViewDock()->grab().toImage());
    }
    SECTION("the whole window") {
        const steps::Script s = strokeShowing("window");
        MainWindow w;
        w.lintWidget()->setBeforeWork(slow);
        screenshot::take(w, s, startOf(s));
        CHECK_FALSE(w.lintWidget()->stale());
        CHECK(w.lintWidget()->report().findings ==
              dockLint(w.document()->canvas(), w.document()->ansi().ice, 0, w.document()->target()).findings);
    }
}

namespace {

// Two pictures: a dot, and a square of noise PNG can't shrink, each under
// QSaveFile's write buffer.
std::vector<screenshot::Shot> twoShots() {
    QImage dot(1, 1, QImage::Format_RGB32);
    dot.fill(Qt::black);
    QImage noise(40, 40, QImage::Format_RGB32);
    QRandomGenerator gen(295);
    for (int y = 0; y < noise.height(); ++y)
        for (int x = 0; x < noise.width(); ++x)
            noise.setPixel(x, y, gen.generate() | 0xff000000u);
    return {{"01-dot", dot}, {"02-noise", noise}};
}

} // namespace

TEST_CASE("a screenshot write that fails part way leaves no new picture", "[screenshot]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const fs::path out = fs::path(QFile::encodeName(dir.path()).toStdString());
    const std::vector<screenshot::Shot> shots = twoShots();

    SECTION("the second picture can't be moved into place") {
        // both written; a directory then takes the second's name
        CHECK_THROWS_AS(screenshot::write(shots, out, [&] { fs::create_directory(out / "02-noise.png"); }),
                        bwcli::Failure);
        CHECK_FALSE(fs::exists(out / "01-dot.png"));
    }
    SECTION("a picture already there is kept") {
        { QFile f(QString::fromStdString((out / "01-dot.png").string())); REQUIRE(f.open(QIODevice::WriteOnly)); }
        CHECK_THROWS_AS(screenshot::write(shots, out, [&] { fs::create_directory(out / "02-noise.png"); }),
                        bwcli::Failure);
        CHECK(fs::exists(out / "01-dot.png"));
    }
#if __has_include(<sys/resource.h>)
    SECTION("the second picture's buffered bytes can't be flushed") {
        // a file-size limit the dot fits under and the noise doesn't: the
        // noise's write fails only when its buffer is flushed
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        REQUIRE(shots[1].image.save(&buf, "PNG"));
        REQUIRE(png.size() > 2048);
        REQUIRE(png.size() < 16384);
        rlimit had{};
        REQUIRE(getrlimit(RLIMIT_FSIZE, &had) == 0);
        rlimit low = had;
        low.rlim_cur = 2048;
        const auto oldHandler = std::signal(SIGXFSZ, SIG_IGN);
        REQUIRE(setrlimit(RLIMIT_FSIZE, &low) == 0);
        bool threw = false;
        try {
            screenshot::write(shots, out);
        } catch (const bwcli::Failure&) {
            threw = true;
        }
        setrlimit(RLIMIT_FSIZE, &had);
        std::signal(SIGXFSZ, oldHandler);
        CHECK(threw);
        CHECK_FALSE(fs::exists(out / "01-dot.png"));
        CHECK_FALSE(fs::exists(out / "02-noise.png"));
    }
#endif
}

// What Tool Options and the palette show while a picture is taken.
struct Shown {
    int size = 0, strength = 0;
    QVariant tip, pressure;
    QString ramp;
    Color fg, bg;
    QImage picture;
};

TEST_CASE("a screenshot shows the Tool Options and pen the step is applied with", "[screenshot]") {
    // two strokes of the same tool with different options, the second's
    // picture also taken before it, then a step that sets nothing
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720],
      "steps": [
        {"name": "defaults", "action": "rampShade", "screenshot": {"of": "toolOptions"}},
        {"name": "soft", "action": "rampShade", "size": 3, "tip": "round", "strength": 0.4, "pressure": "size",
         "ramp": "steel",
         "fg": 12, "bg": 1, "points": [[5, 3]], "screenshot": {"of": "brushSize"}},
        {"name": "hard", "action": "rampShade", "size": 7, "strength": 0.9, "pressure": "strength", "ramp": "fire",
         "fg": 14, "bg": 4,
         "points": [[10, 3]], "screenshot": {"of": "brushSize", "when": "both"}},
        {"name": "held", "action": "rampShade", "screenshot": {"of": "paletteDock"}}
      ]})");
    MainWindow w;
    std::map<std::string, Shown> shown;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& shot) {
        ToolOptions* o = w.toolOptions();
        shown[shot.stem] = {o->sizeBox()->value(),         o->strengthBox()->value(),
                            o->tipBox()->currentData(),    o->pressureBox()->currentData(),
                            o->rampBox()->currentText(),   w.paletteWidget()->pen().fg,
                            w.paletteWidget()->pen().bg,   shot.image};
    });
    REQUIRE(shown.size() == 5);

    // a step that sets nothing shows what replay paints with at the start
    // (steps.h), not the editor's own defaults
    const Shown& d = shown["01-defaults"];
    CHECK(d.size == 1);
    CHECK(d.strength == 100);
    CHECK(d.tip == QVariant(int(Tip::Square)));
    CHECK(d.pressure == QVariant(int(PressureTo::Nothing)));
    CHECK(d.ramp == QStringLiteral("fire"));
    CHECK(d.fg == Color::indexed(Grey));
    CHECK(d.bg == Color::indexed(Black));

    const Shown& soft = shown["02-soft"];
    CHECK(soft.size == 3);
    CHECK(soft.strength == 40);
    CHECK(soft.tip == QVariant(int(Tip::Round)));
    CHECK(soft.pressure == QVariant(int(PressureTo::Size)));
    CHECK(soft.ramp == QStringLiteral("steel"));
    CHECK(soft.fg == Color::indexed(12));
    CHECK(soft.bg == Color::indexed(1));

    // before the step as after it: the options it is applied with, the tip
    // the step before left
    for (const char* stem : {"03-hard-before", "03-hard-after"}) {
        INFO(stem);
        const Shown& h = shown[stem];
        CHECK(h.size == 7);
        CHECK(h.strength == 90);
        CHECK(h.tip == QVariant(int(Tip::Round)));
        CHECK(h.pressure == QVariant(int(PressureTo::Strength)));
        CHECK(h.ramp == QStringLiteral("fire"));
        CHECK(h.fg == Color::indexed(14));
        CHECK(h.bg == Color::indexed(4));
    }

    // what a step sets holds for the steps after it
    const Shown& held = shown["04-held"];
    CHECK(held.size == 7);
    CHECK(held.strength == 90);
    CHECK(held.pressure == QVariant(int(PressureTo::Strength)));
    CHECK(held.fg == Color::indexed(14));
    CHECK(held.bg == Color::indexed(4));

    // and the pictures are of those settings: the size box of the same tool
    // is drawn with 3, then 7
    CHECK(soft.picture.size() == shown["03-hard-after"].picture.size());
    CHECK(soft.picture != shown["03-hard-after"].picture);
    CHECK(shown["03-hard-before"].picture == shown["03-hard-after"].picture);
}

namespace {

// An icon's picture at its own size, to compare swatches.
QImage iconImage(const QIcon& icon) {
    const QList<QSize> sizes = icon.availableSizes();
    return sizes.isEmpty() ? QImage() : icon.pixmap(sizes.front()).toImage();
}

Ramp customRamp(std::vector<std::uint8_t> colours) { return Ramp{"custom", std::move(colours)}; }

// What the Ramp field shows: the selected entry's text and swatches, and
// how many entries it lists.
struct RampShown {
    int index = -1, count = 0;
    QString text;
    QImage swatches;
};

RampShown rampShown(MainWindow& w) {
    QComboBox* box = w.toolOptions()->rampBox();
    return {box->currentIndex(), box->count(), box->currentText(),
            box->currentIndex() < 0 ? QImage() : iconImage(box->itemIcon(box->currentIndex()))};
}

} // namespace

TEST_CASE("a screenshot shows a ramp of palette indices in the Ramp field", "[screenshot]") {
    // Codex's repro: a stroke taken before and after, and the same step
    // with no points
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "steps": [
        {"name": "custom", "action": "rampShade", "ramp": [0, 2, 10, 15], "points": [[5, 3]],
         "screenshot": {"of": "toolOptions", "when": "both"}},
        {"name": "options", "action": "rampShade", "ramp": [0, 2, 10, 15], "screenshot": {"of": "toolOptions"}}
      ]})");
    MainWindow w;
    const int named = int(namedRamps().size());
    std::map<std::string, RampShown> shown;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& shot) { shown[shot.stem] = rampShown(w); });
    REQUIRE(shown.size() == 3);
    const QImage want = iconImage(rampIcon(customRamp({0, 2, 10, 15})));
    REQUIRE_FALSE(want.isNull());
    for (const char* stem : {"01-custom-before", "01-custom-after", "02-options"}) {
        INFO(stem);
        const RampShown& r = shown[stem];
        CHECK(r.index >= 0);
        CHECK(r.text == QStringLiteral("Custom"));
        CHECK(r.swatches == want);
        CHECK(r.count == named + 1);
    }
}

TEST_CASE("the Ramp field follows a custom ramp to a named one and back", "[screenshot]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "steps": [
        {"name": "a", "action": "rampShade", "ramp": [0, 2, 10, 15], "screenshot": {"of": "toolOptions"}},
        {"name": "b", "action": "rampShade", "ramp": "steel", "screenshot": {"of": "toolOptions"}},
        {"name": "c", "action": "rampShade", "ramp": [1, 4, 12], "screenshot": {"of": "toolOptions"}},
        {"name": "d", "action": "rampShade", "ramp": "ice", "screenshot": {"of": "toolOptions"}}
      ]})");
    MainWindow w;
    const int named = int(namedRamps().size());
    std::map<std::string, RampShown> shown;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& shot) { shown[shot.stem] = rampShown(w); });
    REQUIRE(shown.size() == 4);

    const auto custom = [&](const RampShown& r, std::vector<std::uint8_t> colours) {
        CHECK(r.index == named);
        CHECK(r.count == named + 1);
        CHECK(r.text == QStringLiteral("Custom"));
        CHECK(r.swatches == iconImage(rampIcon(customRamp(std::move(colours)))));
    };
    const auto isNamed = [&](const RampShown& r, const char* name) {
        CHECK(r.count == named);
        CHECK(r.text == QString::fromLatin1(name));
        CHECK(r.swatches == iconImage(rampIcon(*findRamp(name))));
    };
    custom(shown["01-a"], {0, 2, 10, 15});
    isNamed(shown["02-b"], "steel");
    custom(shown["03-c"], {1, 4, 12});
    isNamed(shown["04-d"], "ice");
}

TEST_CASE("Tool Options a step doesn't set carry over to another tool", "[screenshot]") {
    // a Ramp Shade step sets the size and a custom ramp; a Form step after it
    // sets only the light
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "steps": [
        {"name": "shade", "action": "rampShade", "size": 3, "ramp": [0, 8, 7, 15], "points": [[5, 3]]},
        {"name": "form", "action": "form", "light": "SE", "points": [[10, 3]],
         "screenshot": {"of": "toolOptions", "when": "both"}}
      ]})");
    MainWindow w;
    std::map<std::string, std::pair<int, RampShown>> shown;
    std::map<std::string, QVariant> light;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& shot) {
        shown[shot.stem] = {w.toolOptions()->sizeBox()->value(), rampShown(w)};
        light[shot.stem] = w.toolOptions()->lightBox()->currentData();
    });
    REQUIRE(shown.size() == 2);
    REQUIRE(w.tools()->active());
    CHECK(w.tools()->active()->id() == QStringLiteral("form"));
    for (const char* stem : {"02-form-before", "02-form-after"}) {
        INFO(stem);
        CHECK(shown[stem].first == 3);
        CHECK(shown[stem].second.text == QStringLiteral("Custom"));
        CHECK(shown[stem].second.swatches == iconImage(rampIcon(customRamp({0, 8, 7, 15}))));
        CHECK(light[stem] == QVariant(int(Compass::SE)));
    }
}

namespace {

// The IRC View the window's document gives with `opts`, as the dock draws it
// (IrcViewDock::snapshot).
QImage ircPicture(MainWindow& w, const irc::EncodeOptions& opts) {
    const Document& doc = *w.document();
    return renderIrcView(irc::ircView(irc::sentCanvas(doc.canvas(), doc.ansi().ice, doc.target()),
                                      doc.extras().ircBudget.bytes, opts),
                         glyphWidth(doc.ansi()));
}

bool checked(MainWindow& w, const char* action) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(action));
    REQUIRE(a);
    return a->isChecked();
}

// The canvas cell at the top left of the canvas's viewport.
QPoint topLeftCell(MainWindow& w) {
    CanvasView* v = w.canvasView();
    const QPoint top = v->mapFrom(w.canvasScroll()->viewport(), QPoint(0, 0));
    return v->cellAt(v->toView(QPointF(top)));
}

} // namespace

TEST_CASE("a view step's Fit shows the fitted IRC View and the meter's fit, from where it stands (#398)",
          "[screenshot][view]") {
    // a row of blocks in six colours, one dab a step (a stroke joins its
    // points), far over the 407-byte budget
    std::string dabs;
    for (int x = 0; x < 120; ++x)
        dabs += "{\"name\": \"c" + std::to_string(x) + "\", \"action\": \"glyph\", \"fg\": " +
                std::to_string(2 + x % 6) + ", \"points\": [[" + std::to_string(x) + ", 0]]},\n";
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [120, 2]}, "window": [1024, 720],
        "steps": [)" + dabs + R"(
        {"name": "fit", "action": "view", "ircFit": true, "screenshot": {"of": "ircView", "when": "both"}},
        {"name": "later", "action": "deselect", "screenshot": {"of": "ircView"}}]})");
    MainWindow w;
    IrcViewDock* irc = w.ircViewWidget();
    std::map<std::string, bool> fitSeen;
    std::map<std::string, int> cutSeen;
    std::map<std::string, QImage> shot;
    const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& sh) {
        INFO(sh.stem);
        fitSeen[sh.stem] = w.ircMeter()->fit();
        CHECK(checked(w, "ircFit") == w.ircMeter()->fit());
        CHECK_FALSE(irc->stale());
        cutSeen[sh.stem] = irc->view().cutRows();
        irc::EncodeOptions opts;
        opts.fit = w.ircMeter()->fit() ? w.ircMeter()->budget().bytes : 0;
        CHECK(irc->image() == ircPicture(w, opts));
        CHECK(sh.image == w.ircViewDock()->grab().toImage());
        shot[sh.stem] = sh.image;
        // the view's toggle is no edit: nothing to undo, nothing to save
        CHECK(w.document()->undoStack()->count() == 0);
        CHECK_FALSE(w.document()->isModified());
    });
    REQUIRE(shots.size() == 3);
    // before the view step, Fit is off and the row is cut; at it and after
    // it, on, and the row fits
    CHECK_FALSE(fitSeen["121-fit-before"]);
    CHECK(cutSeen["121-fit-before"] == 1);
    CHECK(fitSeen["121-fit-after"]);
    CHECK(cutSeen["121-fit-after"] == 0);
    CHECK(fitSeen["122-later"]);
    CHECK(cutSeen["122-later"] == 0);
    CHECK(shot["121-fit-before"] != shot["121-fit-after"]);
    CHECK(irc->statusLabel()->text().startsWith(QStringLiteral("Every line fits")));
}

TEST_CASE("a view step's Pad and Swap Shades reach the row meter and the IRC View (#398)", "[screenshot][view]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [10, 4]}, "window": [1024, 720],
        "steps": [
        {"name": "dot", "action": "glyph", "glyph": "░", "fg": 12, "bg": 4, "points": [[1, 1]]},
        {"name": "opts", "action": "view", "ircPad": true, "ircSwapShades": true,
         "screenshot": {"of": "ircView", "when": "both"}},
        {"name": "swap-off", "action": "view", "ircSwapShades": false, "screenshot": {"of": "ircView"}}]})");
    MainWindow w;
    IrcViewDock* irc = w.ircViewWidget();
    std::map<std::string, irc::EncodeOptions> seen;
    std::map<std::string, int> kept;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& sh) {
        INFO(sh.stem);
        const irc::EncodeOptions o = w.ircMeter()->options();
        seen[sh.stem] = o;
        CHECK(checked(w, "ircPad") == o.pad);
        CHECK(checked(w, "ircSwapShades") == o.swapShades);
        CHECK(w.ircMeter()->pad() == o.pad);
        CHECK(w.ircMeter()->swapShades() == o.swapShades);
        CHECK(irc->image() == ircPicture(w, o));
        REQUIRE(irc->view().rows.size() == 4);
        kept[sh.stem] = irc->view().rows[1].kept;
    });
    REQUIRE(seen.size() == 3);
    CHECK_FALSE(seen["02-opts-before"].pad);
    CHECK_FALSE(seen["02-opts-before"].swapShades);
    CHECK(seen["02-opts-after"].pad);
    CHECK(seen["02-opts-after"].swapShades);
    // a later view step changes one and keeps the other
    CHECK(seen["03-swap-off"].pad);
    CHECK_FALSE(seen["03-swap-off"].swapShades);
    // Pad keeps the row's trailing black, which the view then carries
    CHECK(kept["02-opts-after"] > kept["02-opts-before"]);
}

TEST_CASE("a view step's zoom and scroll take effect, and a later one changes them again (#398)",
          "[screenshot][view]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [120, 60]}, "window": [1024, 720],
        "steps": [
        {"name": "dot", "action": "glyph", "fg": 14, "bg": 1, "points": [[30, 20], [31, 21]]},
        {"name": "near", "action": "view", "zoom": 4, "scrollTo": [30, 20],
         "screenshot": {"crop": {"cells": [30, 20, 2, 2]}}},
        {"name": "zoom-only", "action": "view", "zoom": 3, "screenshot": {}},
        {"name": "back", "action": "view", "zoom": 1, "scrollTo": [0, 0], "screenshot": {}}]})");
    MainWindow w;
    CanvasView* v = w.canvasView();
    QScrollArea* scroll = w.canvasScroll();
    std::map<std::string, int> zoom;
    std::map<std::string, QPoint> cell, bars;
    const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& sh) {
        zoom[sh.stem] = v->zoom();
        cell[sh.stem] = topLeftCell(w);
        bars[sh.stem] = QPoint(scroll->horizontalScrollBar()->value(), scroll->verticalScrollBar()->value());
        CHECK(w.document()->undoStack()->count() == 0);
        CHECK_FALSE(w.document()->isModified());
    });
    REQUIRE(shots.size() == 3);
    CHECK(zoom["02-near"] == 4);
    CHECK(cell["02-near"] == QPoint(30, 20));
    // the scroll cell kept at the zoom a later step sets
    CHECK(zoom["03-zoom-only"] == 3);
    CHECK(cell["03-zoom-only"] == QPoint(30, 20));
    CHECK(zoom["04-back"] == 1);
    CHECK(bars["04-back"] == QPoint(0, 0));

    // the crop is the two cells at zoom 4, drawn as the replay renders them
    const qreal dpr = w.devicePixelRatioF();
    const QImage near = shotNamed(shots, "02-near").image.convertToFormat(QImage::Format_RGB32);
    CHECK((QSizeF(near.size()) / dpr).toSize() == QSize(2 * 8 * 4, 2 * 16 * 4));
    if (dpr == 1.0)
        CHECK(near == replayed(s, "02-near", 4).copy(QRect(30 * 8 * 4, 20 * 16 * 4, 2 * 8 * 4, 2 * 16 * 4)));
}

// Run at 1.25, 1.5 and 1.75 too (tests/CMakeLists.txt): off a whole device pixel
// ratio a cell's rounded-out widget rect starts inside the cell before it,
// and the whole cell scrolled to has to be in view for a crop of it.
TEST_CASE("a view step's scrollTo brings the whole cell to the viewport's corner at any device pixel ratio (#398)",
          "[screenshot][view][view-dpr]") {
    const struct {
        int zoom, x, y;
    } at[] = {{3, 1, 1}, {1, 7, 3}, {2, 33, 17}, {5, 50, 40}, {7, 2, 9}, {3, 11, 5}, {4, 3, 2}, {6, 19, 23}};
    std::string views;
    for (const auto& a : at)
        views += std::string(views.empty() ? "" : ",\n") + "{\"name\": \"z" + std::to_string(a.zoom) + "-" +
                 std::to_string(a.x) + "-" + std::to_string(a.y) + "\", \"action\": \"view\", \"zoom\": " +
                 std::to_string(a.zoom) + ", \"scrollTo\": [" + std::to_string(a.x) + ", " + std::to_string(a.y) +
                 "], \"screenshot\": {\"crop\": {\"cells\": [" + std::to_string(a.x) + ", " + std::to_string(a.y) +
                 ", 1, 1]}}}";
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [120, 60]}, "window": [640, 480],
        "steps": [)" + views + "]}");
    MainWindow w;
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR");
    if (!scale.isEmpty())
        REQUIRE(w.canvasView()->devicePixelRatioF() == scale.toDouble());
    CanvasView* v = w.canvasView();
    const bool whole = w.devicePixelRatioF() == std::floor(w.devicePixelRatioF());
    std::map<std::string, QPoint> corner, gap, further;
    std::map<std::string, QSize> cellSize;
    // the cell a shot's step scrolled to, from its stem "NN-zZ-X-Y"
    const auto cellOf = [](const std::string& stem) {
        int n = 0, z = 0, x = 0, y = 0;
        std::sscanf(stem.c_str(), "%d-z%d-%d-%d", &n, &z, &x, &y);
        return QPoint(x, y);
    };
    // how far the cell's rect starts in from the viewport's corner
    const auto gapOf = [&](const QPoint& cell) {
        const QPoint top = v->mapFrom(w.canvasScroll()->viewport(), QPoint(0, 0));
        return v->toWidget(v->cellRect(cell)).topLeft() - top;
    };
    const std::vector<screenshot::Shot> shots =
        screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& sh) {
            const QPoint cell = cellOf(sh.stem);
            corner[sh.stem] = topLeftCell(w);
            gap[sh.stem] = gapOf(cell);
            cellSize[sh.stem] = v->toWidget(v->cellRect(cell)).size();
            // the gap one more step of each bar would leave (the next step
            // scrolls again, so the bars are put back only to be tidy)
            QPoint more(-1, -1);
            for (QScrollBar* bar : {w.canvasScroll()->horizontalScrollBar(), w.canvasScroll()->verticalScrollBar()}) {
                const int was = bar->value();
                if (was == bar->maximum())
                    continue;
                bar->setValue(was + 1);
                const QPoint g = gapOf(cell);
                (bar->orientation() == Qt::Horizontal ? more.rx() : more.ry()) =
                    bar->orientation() == Qt::Horizontal ? g.x() : g.y();
                bar->setValue(was);
            }
            further[sh.stem] = more;
        });
    REQUIRE(shots.size() == std::size(at));
    int n = 0;
    for (const auto& a : at) {
        char stem[32];
        std::snprintf(stem, sizeof stem, "%02d-z%d-%d-%d", ++n, a.zoom, a.x, a.y);
        INFO(stem);
        // the cell's whole rect is in view, and one more step of either bar
        // would cut it: its rect starts at the corner, or a widget pixel in
        // where a cell's edge falls inside one
        CHECK(gap[stem].x() >= 0);
        CHECK(gap[stem].y() >= 0);
        CHECK(gap[stem].x() <= (whole ? 0 : 1));
        CHECK(gap[stem].y() <= (whole ? 0 : 1));
        CHECK(further[stem].x() < 0);
        CHECK(further[stem].y() < 0);
        // so the corner is on the cell, or off a whole ratio on the cell
        // before, which then shows less than a widget pixel
        if (whole)
            CHECK(corner[stem] == QPoint(a.x, a.y));
        CHECK(a.x - corner[stem].x() >= 0);
        CHECK(a.x - corner[stem].x() <= 1);
        CHECK(a.y - corner[stem].y() >= 0);
        CHECK(a.y - corner[stem].y() <= 1);
        // and a crop of it is taken, the cell's own size
        const QSize crop = (QSizeF(shotNamed(shots, stem).image.size()) / w.devicePixelRatioF()).toSize();
        CHECK(crop == cellSize[stem]);
    }
}

// Run at the fractional ratios too: a crop of cells not all in view is
// refused at any ratio, however little of it is cut (#398 review r3).
TEST_CASE("a crop of cells a device pixel wider than the viewport is refused (#398)", "[screenshot][view][view-dpr]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [120, 60]}, "window": [2048, 720],
        "steps": [{"name": "size", "action": "deselect", "screenshot": {}},
                  {"name": "crop", "action": "deselect", "screenshot": {"crop": {"cells": [0, 0, 61, 1]}}}]})");
    MainWindow w;
    CHECK_THROWS_AS(screenshot::take(w, s, startOf(s),
                                     [&](const screenshot::Shot& shot) {
                                         if (shot.stem != "01-size")
                                             return;
                                         QScrollArea* scroll = w.canvasScroll();
                                         // 61 cells at zoom 2 are 976 device pixels wide;
                                         // the viewport shows at most 975 of them
                                         const qreal dpr = w.devicePixelRatioF();
                                         scroll->viewport()->setFixedSize(int(std::floor(975 / dpr)), 200);
                                         scroll->horizontalScrollBar()->setValue(0);
                                         scroll->verticalScrollBar()->setValue(0);
                                     }),
                    bwcli::Failure);
}

TEST_CASE("a steps file with no view step leaves the window's view as it opens (#398)", "[screenshot][view]") {
    MainWindow fresh;
    const int zoom = fresh.canvasView()->zoom();
    const bool fit = fresh.ircMeter()->fit(), pad = fresh.ircMeter()->pad(), swap = fresh.ircMeter()->swapShades();
    const steps::Script s = tutorial();
    MainWindow w;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot&) {
        CHECK(w.canvasView()->zoom() == zoom);
        CHECK(w.ircMeter()->fit() == fit);
        CHECK(w.ircMeter()->pad() == pad);
        CHECK(w.ircMeter()->swapShades() == swap);
    });
}

// "scale": 2 (#295) runs on FixedScreens' two screens: this case's ctest
// entry gives the app tests the same screens, from the same configuration.
TEST_CASE("the app tests' screens are the ones blockwright --screenshot fixes", "[.][screenshot-scale]") {
    const QJsonArray want =
        QJsonDocument::fromJson(screenshot::FixedScreens::configuration()).object()["screens"].toArray();
    const QList<QScreen*> have = QGuiApplication::screens();
    REQUIRE(have.size() == want.size());
    REQUIRE(want.size() == 2);
    CHECK(QGuiApplication::primaryScreen() == have[0]);
    for (int i = 0; i < have.size(); ++i) {
        const QJsonObject o = want[i].toObject();
        INFO(o["name"].toString().toStdString());
        CHECK(have[i]->name() == o["name"].toString());
        // the configuration is in device pixels; a screen's ratio is its
        // dpi over the base dpi, and its fonts are at 96 logical dpi
        const qreal ratio = o["logicalDpi"].toDouble() / o["logicalBaseDpi"].toDouble();
        CHECK(have[i]->devicePixelRatio() == ratio * o["dpr"].toDouble());
        CHECK(have[i]->devicePixelRatio() == i + 1);
        CHECK(have[i]->geometry() == QRect(o["x"].toInt(), o["y"].toInt(), int(o["width"].toInt() / ratio),
                                           int(o["height"].toInt() / ratio)));
        CHECK(have[i]->geometry().size() == QSize(4096, 4096));
        CHECK(have[i]->logicalDotsPerInch() == 96);
    }
}

TEST_CASE("a scale 2 picture is drawn at device pixel ratio 2, the window laid out as at 1", "[.][screenshot-scale]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
        {"name": "one", "action": "glyph", "glyph": "#", "fg": 14, "bg": 1, "points": [[2, 2], [3, 2], [4, 2]],
         "screenshot": {"of": "toolOptions"}},
        {"name": "two", "action": "deselect", "screenshot": {"of": "toolOptions", "scale": 2}},
        {"name": "cells", "action": "deselect", "screenshot": {"crop": {"cells": [2, 2, 3, 1]}, "scale": 2}},
        {"name": "back", "action": "deselect", "screenshot": {"of": "toolOptions", "when": "both"}}]})");
    MainWindow w;
    std::map<std::string, QSize> laidOut;
    std::map<std::string, qreal> ratio;
    const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& shot) {
        laidOut[shot.stem] = w.toolOptionsDock()->size();
        ratio[shot.stem] = w.devicePixelRatioF();
        CHECK(w.size() == QSize(1024, 720));
    });
    CHECK(ratio["01-one"] == 1.0);
    CHECK(ratio["02-two"] == 2.0);
    CHECK(ratio["03-cells"] == 2.0);
    // back on the first screen for a picture at scale 1, before the step as after it
    CHECK(ratio["04-back-before"] == 1.0);
    CHECK(ratio["04-back-after"] == 1.0);

    // twice the pixels each way, of a dock laid out at the same size
    const QImage one = shotNamed(shots, "01-one").image;
    const QImage two = shotNamed(shots, "02-two").image;
    CHECK(laidOut["02-two"] == laidOut["01-one"]);
    CHECK(one.size() == laidOut["01-one"]);
    CHECK(two.size() == laidOut["02-two"] * 2);
    CHECK(two.devicePixelRatio() == 2.0);
    // drawn at the ratio, not the picture at 1 enlarged: its text has detail
    // a doubled pixel can't hold
    CHECK(two.convertToFormat(QImage::Format_RGB32) !=
          one.scaled(one.size() * 2, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32));
    // and the same picture again at 1 when the window comes back
    CHECK(shotNamed(shots, "04-back-after").image.convertToFormat(QImage::Format_RGB32) ==
          one.convertToFormat(QImage::Format_RGB32));

    // a crop of cells is those cells at twice the view's pixels, the canvas
    // drawn as the replay's renders it at twice the zoom
    const int zoom = w.canvasView()->zoom();
    const QImage cells = shotNamed(shots, "03-cells").image.convertToFormat(QImage::Format_RGB32);
    REQUIRE(cells.size() == QSize(3 * 8 * zoom * 2, 16 * zoom * 2));
    CHECK(cells == replayed(s, "03-cells", zoom * 2)
                       .copy(QRect(2 * 8 * zoom * 2, 2 * 16 * zoom * 2, 3 * 8 * zoom * 2, 16 * zoom * 2)));
}

TEST_CASE("a scale 2 picture with no screen at device pixel ratio 2 names its step", "[screenshot]") {
    for (QScreen* screen : QGuiApplication::screens())
        if (screen->devicePixelRatio() == 2.0)
            return;  // the scale's own ctest entry has one
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [10, 4]}, "steps": [
        {"name": "first", "action": "selectAll", "screenshot": {}},
        {"name": "sharp", "action": "deselect", "screenshot": {"of": "layers", "scale": 2}}]})");
    CHECK(failureOf(s) == "step 2 (sharp): no screen draws at scale 2");
}

#ifdef Q_OS_UNIX
TEST_CASE("the screens' file is put where the platform's arguments can name it, and release() removes it (#295)",
          "[screenshot]") {
    // FixedScreens sets and clears these; the test gives them back.
    const char* const kept[] = {"TMPDIR", "QT_QPA_PLATFORM", "QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS",
                                "QT_AUTO_SCREEN_SCALE_FACTOR", "QT_ENABLE_HIGHDPI_SCALING",
                                "QT_SCALE_FACTOR_ROUNDING_POLICY", "QT_FONT_DPI", "QT_USE_PHYSICAL_DPI",
                                "QT_DPI_ADJUSTMENT_POLICY"};
    std::vector<std::optional<QByteArray>> was;
    for (const char* v : kept)
        was.push_back(qEnvironmentVariableIsSet(v) ? std::optional(qgetenv(v)) : std::nullopt);

    QTemporaryDir root;
    REQUIRE(root.isValid());
    // a folder that isn't there can't be written, so /tmp is
    for (const QString name : {QStringLiteral("semi;dir"), QStringLiteral("col:dir"), QStringLiteral("plain"),
                               QStringLiteral("not there")}) {
        CAPTURE(name.toStdString());
        const QString folder = root.path() + QLatin1Char('/') + name;
        const bool there = name != QLatin1String("not there");
        if (there)
            REQUIRE(QDir().mkpath(folder));
        qputenv("TMPDIR", QFile::encodeName(folder));
        QString path;
        {
            screenshot::FixedScreens screens;
            REQUIRE(screens.error().empty());
            path = screens.path();
            CHECK_FALSE(path.contains(QLatin1Char(';')));
            CHECK_FALSE(path.contains(QLatin1Char(':')));
            CHECK(qgetenv("QT_QPA_PLATFORM") == "offscreen:configfile=" + QFile::encodeName(path));
            if (name == QLatin1String("plain"))
                CHECK(QFileInfo(path).absolutePath() == QFileInfo(folder).absoluteFilePath());
            else
                CHECK(QFileInfo(path).absolutePath() == QStringLiteral("/tmp"));
            QFile f(path);
            REQUIRE(f.open(QIODevice::ReadOnly));
            CHECK(f.readAll() == screenshot::FixedScreens::configuration());
            f.close();
            screens.release();
            CHECK_FALSE(QFileInfo::exists(path));
        }
        if (there)
            CHECK(QDir(folder).entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).isEmpty());
        else
            CHECK_FALSE(QFileInfo::exists(folder));
    }

    for (std::size_t i = 0; i < was.size(); ++i) {
        if (was[i])
            qputenv(kept[i], *was[i]);
        else
            qunsetenv(kept[i]);
    }
}
#endif

TEST_CASE("a canvasSize step resizes the window's canvas, and the steps after it draw on the new one (#420)",
          "[screenshot][canvasSize]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [6, 4]}, "window": [1024, 720], "steps": [
        {"name": "dot", "action": "glyph", "glyph": "#", "fg": 14, "bg": 1, "points": [[0, 0], [5, 3]]},
        {"name": "grow", "action": "canvasSize", "size": [10, 7], "anchor": "centre",
         "screenshot": {"crop": {"cells": [0, 0, 10, 7]}}},
        {"name": "corner", "action": "glyph", "glyph": "#", "fg": 12, "points": [[9, 6]],
         "screenshot": {"of": "canvasView"}}]})");
    MainWindow w;
    std::map<std::string, QSize> size;
    std::map<std::string, Canvas> shown;
    int zoom = 0;
    const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& sh) {
        size[sh.stem] = QSize(w.document()->canvas().width(), w.document()->canvas().height());
        shown[sh.stem] = w.document()->canvas();
        zoom = w.canvasView()->zoom();
    });
    REQUIRE(shots.size() == 2);
    CHECK(size["02-grow"] == QSize(10, 7));
    CHECK(size["03-corner"] == QSize(10, 7));
    // what the window holds is what the CLI's replay leaves
    const bwcli::replay::Outcome out = bwcli::replay::run(s, startOf(s));
    REQUIRE(out.captures.size() == 3);
    CHECK(shown["02-grow"] == composite(out.captures[1].project.layers, {out.captures[1].project.ice, Cell{}}));
    CHECK(shown["03-corner"] == composite(out.captures[2].project.layers, {out.captures[2].project.ice, Cell{}}));
    // the old corners moved by (2, 1)
    CHECK(shown["02-grow"].at(2, 1).ch == U'#');
    CHECK(shown["02-grow"].at(7, 4).ch == U'#');
    CHECK(shown["03-corner"].at(9, 6).ch == U'#');
    // the picture is the whole new canvas, drawn as the replay renders it
    const qreal dpr = w.devicePixelRatioF();
    const QImage grow = shotNamed(shots, "02-grow").image.convertToFormat(QImage::Format_RGB32);
    CHECK((QSizeF(grow.size()) / dpr).toSize() == QSize(10 * 8 * zoom, 7 * 16 * zoom));
    if (dpr == 1.0)
        CHECK(grow == replayed(s, "02-grow", zoom));
}

TEST_CASE("a canvasSize step to a canvas too large to display is refused, named, as the dialog refuses it (#420)",
          "[screenshot][canvasSize]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [4, 2]}, "steps": [
        {"name": "dot", "action": "glyph", "points": [[1, 1]]},
        {"name": "huge", "action": "canvasSize", "size": [400, 5000], "screenshot": {"crop": {"cells": [0, 0, 2, 2]}}}]})");
    // at 9 px a cell the largest canvas is too wide for the renderer, as in the dialog
    project::Project nine = startOf(s);
    nine.sauce = Sauce{};
    nine.sauce->flags = 2 << 1;
    // whether or not the step asks for a picture
    const steps::Script unseen = steps::parse(R"({"version": 1, "start": {"blank": [4, 2]}, "steps": [
        {"name": "dot", "action": "glyph", "points": [[1, 1]]},
        {"name": "huge", "action": "canvasSize", "size": [400, 5000]}]})");
    for (const steps::Script* script : {&s, &unseen}) {
        MainWindow w;
        try {
            screenshot::take(w, *script, nine);
            FAIL("a canvas too large to display was taken");
        } catch (const bwcli::Failure& f) {
            CHECK(f.message == "step 2 (huge): the canvas is too large to display");
        }
    }
    // at 8 px it is shown
    MainWindow w;
    CHECK(screenshot::take(w, s, startOf(s)).size() == 1);
    CHECK(w.document()->canvas().width() == 400);
    CHECK(w.document()->canvas().height() == 5000);
}

TEST_CASE("a start canvas too large to display at its 9 px cells is refused at the start, not at a same-size canvasSize step (#420 review)",
          "[screenshot][canvasSize]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [4, 2]}, "steps": [
        {"name": "same", "action": "canvasSize", "size": [400, 5000]}]})");
    // a start no blank can give, as a project file can: the largest canvas
    project::Project big = startOf(s);
    big.layers = resizeCanvas(big.layers, 400, 5000, CellShift{}, 8);
    project::Project nine = big;
    nine.sauce = Sauce{};
    nine.sauce->flags = 2 << 1;
    MainWindow w;
    try {
        screenshot::take(w, s, nine);
        FAIL("a canvas too large to display was taken");
    } catch (const bwcli::Failure& f) {
        CHECK(f.message == "start: the canvas is too large to display");
    }
    // at 8 px the start is taken and the same-size step changes nothing
    MainWindow eight;
    CHECK(screenshot::take(eight, s, big).empty());
}

TEST_CASE("a clone step plays in the window: the Clone Stamp, its source, Aligned and the pointer at the Ctrl+click (#418)",
          "[screenshot][clone]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
        {"name": "paint", "action": "glyph", "glyph": "#", "fg": 14, "bg": 1, "points": [[1, 1], [2, 1], [3, 2]]},
        {"name": "pick", "action": "clone", "source": [2, 1], "aligned": false,
         "screenshot": {"of": "canvasView", "when": "both", "pointer": true}},
        {"name": "stamp", "action": "clone", "size": 3, "points": [[10, 4], [12, 5]],
         "screenshot": {"of": "canvasView", "when": "both", "pointer": true}},
        {"name": "again", "action": "clone", "source": [1, 1], "aligned": true, "points": [[16, 3]],
         "screenshot": {"of": "canvasView", "when": "both", "pointer": true}}]})");
    struct Shown {
        QString tool;
        CloneSource clone;
        std::optional<QPoint> mark;
        bool box = false;
        Canvas canvas;
        QString status;
    };
    MainWindow w;
    std::map<std::string, Shown> shown;
    const std::vector<screenshot::Shot> shots = screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& sh) {
        auto* clone = qobject_cast<CloneTool*>(w.tools()->active());
        REQUIRE(clone);
        shown[sh.stem] = {w.tools()->active()->id(), clone->source(), clone->mark(),
                          w.toolOptions()->cloneAlignedBox()->isChecked(), w.document()->canvas(),
                          w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"))->text()};
    });
    REQUIRE(shots.size() == 6);

    // what the CLI's replay holds after each step: the window shows the same
    std::map<std::string, CloneSource> replayed;
    std::map<std::string, Canvas> canvas;
    bwcli::replay::run(s, startOf(s), [&](const bwcli::replay::Moment& m) {
        const std::string at = m.step.name + (m.before ? "-before" : "-after");
        replayed[at] = m.settings.clone;
        canvas[at] = composite(m.project.layers, {m.project.ice, Cell{}});
    });
    for (const char* step : {"pick", "stamp", "again"})
        for (const char* when : {"-before", "-after"}) {
            const std::string at = std::string(step) + when;
            const std::string stem = std::string(step == std::string("pick") ? "02-" : step == std::string("stamp") ? "03-" : "04-") + at;
            INFO(stem);
            REQUIRE(shown.count(stem));
            const Shown& got = shown[stem];
            CHECK(got.tool == QStringLiteral("clone"));
            CHECK(got.clone.source == replayed[at].source);
            CHECK(got.clone.anchor == replayed[at].anchor);
            CHECK(got.clone.aligned == replayed[at].aligned);
            CHECK(got.box == replayed[at].aligned);
            CHECK(got.canvas == canvas[at]);
        }
    // the source is shown before the step that sets it; Aligned is the step's
    CHECK(shown["02-pick-after"].clone.source == std::pair{2, 1});
    CHECK(shown["02-pick-after"].mark == QPoint(2, 1));
    CHECK_FALSE(shown["02-pick-after"].box);
    CHECK(shown["03-stamp-after"].clone.anchor == std::pair{10, 4});
    CHECK(shown["04-again-before"].clone.source == std::pair{2, 1});
    CHECK(shown["04-again-after"].clone.source == std::pair{1, 1});
    CHECK(shown["04-again-after"].box);
    // and the status bar says so, as after a Ctrl+click
    CHECK(shown["02-pick-before"].status == QStringLiteral("Ctrl+click to set the source"));
    CHECK(shown["02-pick-after"].status == QStringLiteral("Source 2, 1"));
    CHECK(shown["04-again-before"].status == QStringLiteral("Source 2, 1, aligned"));
    CHECK(shown["04-again-after"].status == QStringLiteral("Source 1, 1, aligned"));
    // the stamp copied the painted cells
    CHECK(shown["03-stamp-after"].canvas.at(10, 4).ch == U'#');
    CHECK(shown["04-again-after"].canvas.at(16, 3).ch == U'#');

    // the pointer: at the source before a step that sets one, and after it
    // too when it has no stroke; a stroke's start before and its end after
    const int zoom = w.canvasView()->zoom();
    const QSizeF cell(8 * zoom, 16 * zoom);
    const auto centre = [&](int x, int y) {
        return QPointF(x * cell.width() + cell.width() / 2.0, y * cell.height() + cell.height() / 2.0);
    };
    CHECK(*shotNamed(shots, "02-pick-before").pointer == centre(2, 1));
    CHECK(*shotNamed(shots, "02-pick-after").pointer == centre(2, 1));
    CHECK(*shotNamed(shots, "03-stamp-before").pointer == centre(10, 4));
    CHECK(*shotNamed(shots, "03-stamp-after").pointer == centre(12, 5));
    CHECK(*shotNamed(shots, "04-again-before").pointer == centre(1, 1));
    CHECK(*shotNamed(shots, "04-again-after").pointer == centre(16, 3));
}

TEST_CASE("a clone step's source off the window's canvas fails at the step, as the CLI's replay does (#418)",
          "[screenshot][clone]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
        {"name": "pick", "action": "clone", "source": [20, 3]}]})");
    CHECK(failureOf(s) == "step 1 (pick): source cell (20, 3) is off the 20 x 8 canvas");
}

TEST_CASE("a screenshot shows the Sample merged a fill or eyedropper step sets, held for the steps after (#419)",
          "[screenshot]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720],
      "steps": [
        {"name": "off", "action": "fill", "screenshot": {"of": "sampleMerged"}},
        {"name": "on", "action": "fill", "sampleMerged": true, "at": [3, 3],
         "screenshot": {"of": "sampleMerged", "when": "both"}},
        {"name": "held", "action": "eyedropper", "at": [3, 3], "screenshot": {"of": "sampleMerged"}},
        {"name": "again", "action": "fill", "sampleMerged": false, "screenshot": {"of": "sampleMerged"}}
      ]})");
    MainWindow w;
    struct Seen {
        bool checked = false, visible = false, view = false;
        QImage picture;
    };
    std::map<std::string, Seen> seen;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& shot) {
        QCheckBox* box = w.toolOptions()->sampleMergedBox();
        seen[shot.stem] = {box->isChecked(), box->isVisibleTo(w.toolOptions()), w.canvasView()->sampleMerged(),
                           shot.image};
    });
    REQUIRE(seen.size() == 5);
    for (const auto& [stem, x] : seen) {
        INFO(stem);
        CHECK(x.visible);
        CHECK(x.view == x.checked);  // the box is the view's own setting
    }
    CHECK_FALSE(seen["01-off"].checked);
    // before the step as after it: the option it is applied with
    CHECK(seen["02-on-before"].checked);
    CHECK(seen["02-on-after"].checked);
    CHECK(seen["03-held"].checked);
    CHECK_FALSE(seen["04-again"].checked);
    // and the picture is of it: the same box drawn ticked and not
    CHECK(seen["01-off"].picture.size() == seen["02-on-after"].picture.size());
    CHECK(seen["01-off"].picture != seen["02-on-after"].picture);
    CHECK(seen["01-off"].picture == seen["04-again"].picture);
}

TEST_CASE("a clone step's window keeps the Sample merged a fill step left on, with the box hidden (#418, #419)",
          "[screenshot][clone][sample-merged]") {
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720],
      "steps": [
        {"name": "merged", "action": "fill", "sampleMerged": true, "screenshot": {"of": "toolOptions"}},
        {"name": "stamp", "action": "clone", "source": [2, 1], "points": [[10, 4]],
         "screenshot": {"of": "toolOptions", "when": "both"}},
        {"name": "pick", "action": "eyedropper", "at": [3, 3], "screenshot": {"of": "toolOptions"}}
      ]})");
    MainWindow w;
    struct Seen {
        QString tool;
        bool view = false, checked = false, visible = false;
    };
    std::map<std::string, Seen> seen;
    screenshot::take(w, s, startOf(s), [&](const screenshot::Shot& shot) {
        QCheckBox* box = w.toolOptions()->sampleMergedBox();
        seen[shot.stem] = {w.tools()->active()->id(), w.canvasView()->sampleMerged(), box->isChecked(),
                           box->isVisibleTo(w.toolOptions())};
    });
    REQUIRE(seen.size() == 4);
    for (const auto& [stem, x] : seen) {
        INFO(stem);
        CHECK(x.view);  // on from the first step to the last
        CHECK(x.checked == x.view);
    }
    CHECK(seen["02-stamp-before"].tool == "clone");
    CHECK(seen["02-stamp-after"].tool == "clone");
    // the Clone Stamp's Tool Options don't show it; fill's and the eyedropper's do
    CHECK(seen["01-merged"].visible);
    CHECK_FALSE(seen["02-stamp-before"].visible);
    CHECK_FALSE(seen["02-stamp-after"].visible);
    CHECK(seen["03-pick"].visible);
}
