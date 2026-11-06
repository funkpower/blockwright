// blockwright --screenshot (#295, PLAN §9): a steps file replayed in the
// window, each step's "screenshot" grabbed from it. What is tested is the
// structure, which no font or theme changes: the pictures made and their
// names, each the size of what it shows, a crop's geometry, and the canvas
// in a picture drawn exactly as the replay's canvas renders. main() is in
// test_app.cpp.

#include "canvas_view.h"
#include "lint_dock.h"
#include "main_window.h"
#include "palette_widget.h"
#include "replay.h"
#include "read_file.h"
#include "screenshot.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/composite.h"
#include "blockwright/irc_render.h"
#include "blockwright/ramp.h"
#include "blockwright/render.h"
#include "blockwright/steps.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QBuffer>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QIcon>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>

#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#include <csignal>
#endif

#include <filesystem>
#include <map>
#include <string>
#include <utility>
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
        {"name": "soft", "action": "rampShade", "size": 3, "tip": "round", "strength": 0.4, "ramp": "steel",
         "fg": 12, "bg": 1, "points": [[5, 3]], "screenshot": {"of": "brushSize"}},
        {"name": "hard", "action": "rampShade", "size": 7, "strength": 0.9, "ramp": "fire", "fg": 14, "bg": 4,
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
        CHECK(h.ramp == QStringLiteral("fire"));
        CHECK(h.fg == Color::indexed(14));
        CHECK(h.bg == Color::indexed(4));
    }

    // what a step sets holds for the steps after it
    const Shown& held = shown["04-held"];
    CHECK(held.size == 7);
    CHECK(held.strength == 90);
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
