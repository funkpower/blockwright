// `blockwright-cli shade` against the editor itself (issue #44): a file is
// opened in the main window and shaded with the ramp shade brush through
// the view, and the same file read and shaded as the CLI does must give the
// same cells and the same saved bytes.

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "tool.h"

#include "shade.h"
#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/ramp.h"
#include "blockwright/sauce.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMouseEvent>
#include <QTemporaryDir>

#include <string>

using namespace bw;

namespace {

Ramp fire() { return *findRamp("fire"); }  // Black Red LightRed Yellow White

// An 80-column file with SAUCE and its iCE flag off.
QString writeIceOff(const QTemporaryDir& tmp, const char* name, const std::string& body) {
    Sauce s = Sauce::fresh(80, 1);
    s.setIce(false);
    const QString path = tmp.filePath(QString::fromLatin1(name));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    const std::string bytes = body + encodeSauce(s, std::uint32_t(body.size()));
    REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
    return path;
}

// Opens `path` in `w` with the ramp shade brush on fire at the dock's
// default quarter strength (a rung a pass).
void openForShading(MainWindow& w, const QString& path) {
    QString error;
    REQUIRE(w.openFile(path, &error));
    auto* a = w.findChild<QAction*>(QStringLiteral("rampShadeTool"));
    REQUIRE(a);
    a->trigger();
    REQUIRE(w.tools()->active()->id() == QStringLiteral("rampShade"));
    w.canvasView()->setRamp(fire());
    REQUIRE(w.canvasView()->brushSettings().strength == 0.25);
}

// A click of the ramp shade brush on cell (x, y): left lightens, right
// darkens.
void click(CanvasView* v, int x, int y, Qt::MouseButton button) {
    const QPointF at = QRectF(v->cellRect(QPoint(x, y))).center();
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
        QMouseEvent e(type, at, v->mapToGlobal(at), button, held, Qt::NoModifier);
        QApplication::sendEvent(v, &e);
    }
}

// `cli shade --ramp fire [--darken] --box x,y,x,y` on the file `bytes`, as
// the CLI reads it.
AnsiDocument cliShade(const std::string& bytes, ShadeDirection dir, int x, int y) {
    AnsiDocument doc = parseAnsi(bytes);
    bwcli::ShadeOptions o;
    o.ramp = fire();
    o.direction = dir;
    o.box = lint::Box{x, y, x, y};
    bwcli::shadeDocument(doc, o);
    return doc;
}

std::string readFile(const QString& path) { return bwtest::readFile(path.toStdString()); }

} // namespace

TEST_CASE("cli shade shades a blinking cell of an iCE-off file with bright backgrounds as the editor does",
          "[cli][shade]") {
    // A blinking red block, then a bright background: the editor opens the
    // file with iCE on and the block without its blink, so it is on the ramp.
    // The shade writes the block afresh as a rung, which ends its claim to
    // the blink (#82), in the editor and the CLI alike.
    QTemporaryDir tmp;
    const QString path = writeIceOff(tmp, "blink.ans", "\x1b[0;5;31;40m\xdb\x1b[0;104mA");

    MainWindow w;
    openForShading(w, path);
    REQUIRE(rungOf(fire(), w.document()->canvas().at(0, 0)) == 4);
    click(w.canvasView(), 0, 0, Qt::LeftButton);
    const Canvas& gui = w.document()->canvas();
    CHECK(rungOf(fire(), gui.at(0, 0)) == 5);
    const AnsiOutput saved = w.document()->encode();
    CHECK(saved.blinkDropped == 0);

    const AnsiDocument cli = cliShade(readFile(path), ShadeDirection::Lighten, 0, 0);
    CHECK(rungOf(fire(), cli.canvas.at(0, 0)) == 5);
    CHECK(cli.canvas == gui);
    CHECK(writeAnsi(cli).bytes == saved.bytes);
}

TEST_CASE("cli shade keeps the iCE colours the editor opens a file with once its last bright background is gone",
          "[cli][shade]") {
    // One LightRed space, rung 8 of fire: darkened to rung 7 it has no
    // bright background left, and the editor still saves it with iCE on
    QTemporaryDir tmp;
    const QString path = writeIceOff(tmp, "bright.ans", "\x1b[101m ");
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));

    MainWindow w;
    openForShading(w, path);
    REQUIRE(rungOf(fire(), w.document()->canvas().at(0, 0)) == 8);
    click(w.canvasView(), 0, 0, Qt::RightButton);
    REQUIRE(rungOf(fire(), w.document()->canvas().at(0, 0)) == 7);
    QString error;
    REQUIRE(w.saveFile(saved, &error));
    const std::string guiBytes = readFile(saved);
    REQUIRE(parseAnsi(guiBytes).ice);

    AnsiDocument cli = cliShade(readFile(path), ShadeDirection::Darken, 0, 0);
    CHECK(cli.canvas == w.document()->canvas());
    const std::string cliBytes = writeAnsi(cli).bytes;
    CHECK(parseAnsi(cliBytes).ice);
    CHECK(cliBytes == guiBytes);

    SECTION("so two passes lightening the saved file reach rung 9 in both") {
        MainWindow again;
        openForShading(again, saved);
        click(again.canvasView(), 0, 0, Qt::LeftButton);
        click(again.canvasView(), 0, 0, Qt::LeftButton);
        CHECK(rungOf(fire(), again.document()->canvas().at(0, 0)) == 9);

        cli = cliShade(cliBytes, ShadeDirection::Lighten, 0, 0);
        cli = cliShade(writeAnsi(cli).bytes, ShadeDirection::Lighten, 0, 0);
        CHECK(rungOf(fire(), cli.canvas.at(0, 0)) == 9);
        CHECK(cli.canvas == again.document()->canvas());
        CHECK(writeAnsi(cli).bytes == again.document()->encode().bytes);
    }
}
