// blockwright --screenshot (#295, PLAN §9): a steps file replayed in the
// window, each step's "screenshot" grabbed from it. What is tested is the
// structure, which no font or theme changes: the pictures made and their
// names, each the size of what it shows, a crop's geometry, and the canvas
// in a picture drawn exactly as the replay's canvas renders. main() is in
// test_app.cpp.

#include "canvas_view.h"
#include "lint_dock.h"
#include "main_window.h"
#include "replay.h"
#include "read_file.h"
#include "screenshot.h"
#include "tool.h"

#include "blockwright/composite.h"
#include "blockwright/irc_render.h"
#include "blockwright/render.h"
#include "blockwright/steps.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QBuffer>
#include <QDockWidget>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QThread>

#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#include <csignal>
#endif

#include <filesystem>
#include <string>

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

} // namespace

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
