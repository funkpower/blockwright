// Format > Target > IRC in 16 Colours (#496): the setting, the palette, the
// colour stepping, what is sent and the lint note.

#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "irc_meter.h"
#include "lint_dock.h"
#include "main_window.h"
#include "palette_widget.h"

#include "blockwright/irc.h"
#include "blockwright/irc_render.h"
#include "blockwright/irc_view.h"
#include "blockwright/target.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLabel>
#include <QTemporaryDir>
#include <QUndoStack>

#include <string>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

template <typename F>
bool waitFor(F done, int ms = 5000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

Color mircRgb(int i) {
    const std::uint32_t v = irc::kMircRgb[std::size_t(i)];
    return Color::fromRgb({std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)});
}

// A row of mIRC 77 on 40 and a palette cell, and a blank row.
AnsiDocument extended() {
    AnsiDocument d;
    d.ice = true;
    d.canvas = Canvas(6, 2);
    for (int x = 0; x < 5; ++x) {
        d.canvas.at(x, 0).ch = U'▒';
        d.canvas.at(x, 0).fg = mircRgb(77);
        d.canvas.at(x, 0).bg = mircRgb(40);
    }
    d.canvas.at(5, 0).ch = U'A';
    d.canvas.at(5, 0).fg = Color::indexed(14);
    return d;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t at = 0, end; (end = text.find('\n', at)) != std::string::npos; at = end + 1)
        out.push_back(text.substr(at, end - at));
    return out;
}

} // namespace

TEST_CASE("Format > Target > IRC in 16 Colours is one undo step, under IRC only", "[app][irc-colours]") {
    MainWindow w;
    Document* doc = w.document();
    QAction* sixteen = act(w, "ircColours16");
    REQUIRE(doc->target() == Target::Ansi);
    CHECK_FALSE(sixteen->isEnabled());
    CHECK_FALSE(sixteen->isChecked());
    CHECK(doc->ircColours() == IrcColours::Mirc99);

    REQUIRE(doc->setTarget(Target::Irc));
    CHECK(sixteen->isEnabled());
    CHECK(w.targetLabel()->text() == QStringLiteral("IRC"));
    const Canvas before = doc->canvas();
    sixteen->trigger();
    CHECK(doc->ircColours() == IrcColours::Mirc16);
    CHECK(doc->sentIrcColours() == IrcColours::Mirc16);
    CHECK(sixteen->isChecked());
    CHECK(doc->undoStack()->undoText() == QStringLiteral("IRC Colours"));
    CHECK(w.targetLabel()->text() == QStringLiteral("IRC 16"));
    CHECK(w.paletteWidget()->ircColours() == IrcColours::Mirc16);
    CHECK(doc->canvas() == before);
    CHECK_FALSE(doc->setIrcColours(IrcColours::Mirc16));

    // under ANSI it is kept and does nothing
    REQUIRE(doc->setTarget(Target::Ansi));
    CHECK(doc->ircColours() == IrcColours::Mirc16);
    CHECK(doc->sentIrcColours() == IrcColours::Mirc99);
    CHECK_FALSE(sixteen->isEnabled());
    CHECK(sixteen->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("ANSI"));
    // an .ans can't hold it
    CHECK_FALSE(doc->flatHoldsAll());

    doc->undoStack()->undo();  // the target
    doc->undoStack()->undo();  // the colours
    CHECK(doc->ircColours() == IrcColours::Mirc99);
    CHECK_FALSE(sixteen->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("IRC"));
    doc->undoStack()->redo();
    CHECK(doc->ircColours() == IrcColours::Mirc16);
    CHECK(sixteen->isChecked());
    sixteen->trigger();
    CHECK(doc->ircColours() == IrcColours::Mirc99);
}

TEST_CASE("in 16 colours the palette shows the 16 and Ctrl+arrows step through them", "[app][irc-colours]") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    PaletteWidget* p = w.paletteWidget();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->setTarget(Target::Irc));
    const QSize all = p->sizeHint();
    REQUIRE(p->swatchRect(98).isValid());

    REQUIRE(doc->setIrcColours(IrcColours::Mirc16));
    CHECK(p->swatchRect(15).isValid());
    CHECK_FALSE(p->swatchRect(16).isValid());
    CHECK_FALSE(p->swatchRect(77).isValid());
    CHECK(p->sizeHint().height() < all.height());

    QAction* next = act(w, "nextForeground");
    QAction* prevBg = act(w, "previousBackground");
    v->setForeground(15);
    next->trigger();
    CHECK(v->pen().fg == Color::indexed(0));  // wraps at 16
    v->setBackground(0);
    prevBg->trigger();
    CHECK(v->pen().bg == Color::indexed(15));

    // an extended pen steps on from the swatch it is sent as
    Pen pen = v->pen();
    pen.fg = mircRgb(77);
    v->setPen(pen);
    const int from = irc::fold16(mircRgb(77)).index;
    next->trigger();
    CHECK(v->pen().fg == Color::indexed(std::uint8_t((from + 1) % 16)));

    // back to 99 the extended swatches come back
    REQUIRE(doc->setIrcColours(IrcColours::Mirc99));
    CHECK(p->swatchRect(77).isValid());
}

TEST_CASE("in 16 colours Export IRC Text, the meter and the IRC view send mIRC 77 as one of 0-15",
          "[app][irc-colours]") {
    QTemporaryDir dir;
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(extended(), QString());
    REQUIRE(doc->setTarget(Target::Irc));
    const QString path = dir.filePath(QStringLiteral("a.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error));
    const std::vector<std::string> all = lines(bwtest::readFile(path.toStdString()));
    REQUIRE(all.size() == 1);
    CHECK(all[0].find("\x03" "77,40") != std::string::npos);

    REQUIRE(doc->setIrcColours(IrcColours::Mirc16));
    REQUIRE(w.exportIrcText(path, &error));
    const std::vector<std::string> sent = lines(bwtest::readFile(path.toStdString()));
    REQUIRE(sent.size() == 1);
    const Canvas want = irc::sentCanvas(doc->canvas(), true, Target::Irc, IrcColours::Mirc16);
    CHECK(sent == irc::exportCanvas(want));
    for (const irc::ShownCell& s : irc::decodeLine(sent[0])) {
        CHECK(s.cell.fg < 16);
        CHECK(s.cell.bg < 16);
    }
    CHECK(w.ircMeter()->bytes(0) == sent[0].size());

    IrcViewDock* view = w.ircViewWidget();
    REQUIRE(view);
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return view->shown() && !view->stale(); }));
    CHECK(view->view().canvas == irc::ircView(want, view->budget()).canvas);
    CHECK(view->view().canvas != irc::ircView(irc::sentCanvas(doc->canvas(), true, Target::Irc)).canvas);
}

TEST_CASE("the lint dock notes the cells 16 colours fold", "[app][irc-colours]") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(extended(), QString());
    REQUIRE(doc->setTarget(Target::Irc));
    LintDock* dock = w.lintWidget();
    w.lintDock()->raise();
    const auto noted = [&] {
        for (const lint::Finding& f : dock->report().findings)
            if (f.kind == lint::Finding::Kind::IrcFold)
                return true;
        return false;
    };
    REQUIRE(waitFor([&] { return dock->shown() && !dock->stale(); }));
    CHECK_FALSE(noted());
    REQUIRE(doc->setIrcColours(IrcColours::Mirc16));
    REQUIRE(waitFor([&] { return !dock->stale() && noted(); }));
    for (const lint::Finding& f : dock->report().findings)
        if (f.kind == lint::Finding::Kind::IrcFold)
            CHECK(f.message.rfind("5 cells are in mIRC colours past 15", 0) == 0);
}

TEST_CASE("a project saves and reopens in 16 colours", "[app][irc-colours]") {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("p.bwp"));
    {
        MainWindow w;
        Document* doc = w.document();
        REQUIRE(doc->setTarget(Target::Irc));
        REQUIRE(doc->setIrcColours(IrcColours::Mirc16));
        QString error;
        REQUIRE(doc->saveProject(path, FileForm::ProjectZip, &error));
    }
    MainWindow w;
    Document* doc = w.document();
    QString error;
    REQUIRE(w.openFile(path, &error));
    CHECK(doc->target() == Target::Irc);
    CHECK(doc->ircColours() == IrcColours::Mirc16);
    CHECK(act(w, "ircColours16")->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("IRC 16"));
    CHECK(w.paletteWidget()->ircColours() == IrcColours::Mirc16);
}
