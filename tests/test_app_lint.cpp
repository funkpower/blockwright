// The Lint and IRC View docks (lint_dock.h): they follow the document a
// moment after each change while shown, agree with lintCanvas and ircView,
// and a finding's row takes the cell cursor to its cell.

#include "util.h"

#include "canvas_view.h"
#include "device_grid.h"
#include "document.h"
#include "irc_meter.h"
#include "lint_dock.h"
#include "main_window.h"

#include "blockwright/ansi.h"
#include "blockwright/irc.h"
#include "blockwright/irc_render.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/render.h"
#include "blockwright/target.h"

#include <catch2/catch_test_macros.hpp>

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QGestureEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPanGesture>
#include <QProcess>
#include <QScrollArea>
#include <QScrollBar>
#include <QSemaphore>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace bw;

namespace {

// Runs the event loop until `done` or `ms` pass.
template <typename F>
bool waitFor(F done, int ms = 2000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

AnsiDocument piece() {
    return parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "cafe-mug.ans"));
}

// An 'A' in its own background's colour: a merge.
CellEdit merge(const Document& doc, int x, int y) {
    Cell after;
    after.ch = U'A';
    after.fg = Color::indexed(Blue);
    after.bg = Color::indexed(Blue);
    return CellEdit{{CellChange{x, y, doc.canvas().at(x, y), after}}};
}

// As the dock lints: every named ramp and the craft pairs, or one ramp.
lint::LintReport lintAll(const Document& doc, std::optional<Ramp> ramp = std::nullopt) {
    lint::LintOptions o;
    o.ramps = ramp ? std::vector<Ramp>{*ramp} : namedRamps();
    o.craftPairs = !ramp;
    o.ice = doc.ansi().ice;
    return lint::lintCanvas(doc.canvas(), o);
}

// A canvas the size of a long scroll, every cell a shade of fire's: the
// lint takes about a second on it.
AnsiDocument big() {
    AnsiDocument d;
    d.canvas = Canvas(400, 2000);
    const char32_t shades[] = {kShadeLight, kShadeMedium, kShadeDark, kFullBlock};
    const std::uint8_t fire[] = {Black, Red, LightRed, Yellow, White};
    unsigned seed = 1;
    for (int y = 0; y < d.canvas.height(); ++y)
        for (int x = 0; x < d.canvas.width(); ++x) {
            seed = seed * 1103515245u + 12345u;
            const unsigned r = seed >> 16;
            Cell& c = d.canvas.at(x, y);
            c.ch = shades[r % 4];
            c.bg = Color::indexed(fire[(r / 4) % 4]);
            c.fg = Color::indexed(fire[(r / 4) % 4 + 1]);
        }
    return d;
}

QTreeWidgetItem* rowAt(QTreeWidget* list, const QString& cell, const QString& kind) {
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        QTreeWidgetItem* it = list->topLevelItem(i);
        if (it->text(0) == cell && it->text(1) == kind)
            return it;
    }
    return nullptr;
}

// Holds the first refresh `dock` starts from now on its worker until
// release(), and counts the refreshes whose work began.
class Hold {
public:
    explicit Hold(FollowsDocument* dock) : dock_(dock) {
        dock_->setBeforeWork([s = s_] {
            if (s->runs++ == 0)
                s->gate.acquire();
        });
    }
    ~Hold() {
        release();
        dock_->setBeforeWork({});
    }
    // Waits until the held refresh's work has begun.
    bool started() const {
        QElapsedTimer t;
        t.start();
        while (s_->runs == 0 && t.elapsed() < 2000)
            QThread::msleep(1);
        return s_->runs > 0;
    }
    void release() { s_->gate.release(); }

private:
    struct State {
        QSemaphore gate;
        std::atomic<int> runs{0};
    };
    std::shared_ptr<State> s_ = std::make_shared<State>();
    FollowsDocument* dock_;
};

// Delivers posted events (a finished refresh's result) and fires no timer,
// until `done` or `ms` pass.
template <typename F>
bool postedUntil(F done, int ms = 2000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::sendPostedEvents();
        QThread::msleep(1);
    }
    return done();
}

// What the IRC View dock should show of the document now.
QImage ircNow(const Document& doc) {
    return renderIrcView(irc::ircView(doc.canvas(), doc.extras().ircBudget.bytes), glyphWidth(doc.ansi()));
}

// An 80x5 piece in which each of View > IRC's encoder options and the iCE
// setting changes what Export sends. Rows 0 and 4 are blank: an export drops
// them unless padded. Row 1 is #297's: 420 bytes, over the 407-byte budget,
// and 407 once fit. Row 2 is shades that swap shorter, then black cells that
// pad keeps. Row 3 starts with four spaces on bright blue (9), which go as
// dark blue (mIRC 2) with iCE off (#301). iCE is off.
AnsiDocument sendPiece() {
    AnsiDocument d;
    d.canvas = Canvas(80, 5);
    d.ice = false;
    const auto set = [&](int x, int y, char32_t ch, int fg, int bg) {
        Cell& c = d.canvas.at(x, y);
        c.ch = ch;
        c.fg = Color::indexed(std::uint8_t(fg));
        c.bg = Color::indexed(std::uint8_t(bg));
    };
    // #297's row is in ANSI's colour order: its palette index
    constexpr int ansi[] = {Black, Red, Green, Brown, Blue, Magenta, Cyan, Grey};
    for (int x = 0; x < 80; ++x) {
        if (x % 2)
            set(x, 1, kFullBlock, ansi[x % 7 + 1], ansi[x * 3 % 8]);
        else
            set(x, 1, U'▀', ansi[x % 6], ansi[(x + 1) % 8]);
    }
    for (int x = 0; x < 40; ++x) {
        if (x % 2)
            set(x, 2, kShadeLight, Blue, Red);
        else
            set(x, 2, kShadeDark, Red, Blue);
    }
    for (int x = 0; x < 4; ++x)
        set(x, 3, U' ', White, 9);
    return d;
}

std::uint32_t rgbOf(const Color& c) {
    REQUIRE_FALSE(c.isIndexed());
    return std::uint32_t(c.rgb.r) << 16 | std::uint32_t(c.rgb.g) << 8 | c.rgb.b;
}

std::uint32_t mircRgbOf(std::uint8_t mirc, std::uint32_t client) {
    return mirc == irc::kClientColour ? client : irc::kMircRgb[mirc];
}

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::size_t from = 0;
    for (std::size_t nl; (nl = text.find('\n', from)) != std::string::npos; from = nl + 1)
        out.push_back(text.substr(from, nl - from));
    REQUIRE(from == text.size());
    return out;
}

// Checks that the view `v` shows `exported`, the lines an export wrote:
// past the blank rows above and below the art, which the export drops and
// the view marks trimmed, each row is the next line, as long as it, with its
// cells shown as a client reads the line (irc::decodeLine) in the mIRC
// palette up to where the budget cuts it. Returns how many rows are cut.
int checkShowsExport(const irc::View& v, const std::vector<std::string>& exported) {
    std::size_t next = 0;
    int cut = 0;
    for (int y = 0; y < int(v.rows.size()); ++y) {
        INFO("row " << y);
        const irc::ViewRow& row = v.rows[std::size_t(y)];
        if (row.trimmed) {
            CHECK(row.bytes == 0);
            continue;
        }
        REQUIRE(next < exported.size());
        const std::string& line = exported[next++];
        CHECK(row.bytes == line.size());
        const std::vector<irc::ShownCell> cells = irc::decodeLine(line);
        CHECK(row.kept == int(cells.size()));
        REQUIRE(row.shown <= int(cells.size()));
        for (int x = 0; x < row.shown; ++x) {
            INFO("cell " << x);
            const Cell& c = v.canvas.at(x, y);
            const irc::MircCell& m = cells[std::size_t(x)].cell;
            CHECK(c.ch == m.ch);
            CHECK(rgbOf(c.fg) == mircRgbOf(m.fg, irc::kClientForeground));
            CHECK(rgbOf(c.bg) == mircRgbOf(m.bg, irc::kClientBackground));
        }
        cut += row.cut();
    }
    CHECK(next == exported.size());
    return cut;
}

QAction* ircAction(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

struct CliRun {
    int status = -1;
    QString out;
};

CliRun runCli(const QStringList& args) {
    QProcess p;
    p.start(QStringLiteral(BW_CLI), args);
    REQUIRE(p.waitForFinished(60000));
    REQUIRE(p.exitStatus() == QProcess::NormalExit);
    return {p.exitCode(), QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError())};
}

QByteArray fileBytes(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

} // namespace

TEST_CASE("the Lint dock lists the document's lint and follows its edits") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    LintDock* dock = w.lintWidget();
    REQUIRE(dock);
    REQUIRE(waitFor([&] { return dock->shown(); }));
    REQUIRE(waitFor([&] { return !dock->stale(); }));

    // the same lint as blockwright-cli lint: every named ramp
    CHECK(dock->report().findings == lintAll(*doc).findings);
    CHECK(dock->list()->topLevelItemCount() == int(dock->report().findings.size()));
    CHECK(dock->summaryLabel()->text().toStdString() == lint::summary(dock->report()));
    CHECK(dock->rampBox()->count() == int(namedRamps().size()) + 1);
    REQUIRE_FALSE(rowAt(dock->list(), QStringLiteral("3,2"), QStringLiteral("merge")));

    // an edit shows a moment later, not at once
    REQUIRE(doc->edit(merge(*doc, 3, 2)));
    CHECK(dock->stale());
    CHECK_FALSE(rowAt(dock->list(), QStringLiteral("3,2"), QStringLiteral("merge")));
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QTreeWidgetItem* row = rowAt(dock->list(), QStringLiteral("3,2"), QStringLiteral("merge"));
    REQUIRE(row);
    CHECK(row->text(2) == QStringLiteral("-"));
    CHECK(dock->report().findings == lintAll(*doc).findings);

    // undo takes it away again
    doc->undoStack()->undo();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    CHECK_FALSE(rowAt(dock->list(), QStringLiteral("3,2"), QStringLiteral("merge")));
}

TEST_CASE("a finding's row takes the cell cursor to its cell") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    REQUIRE(doc->edit(merge(*doc, 30, 20)));
    LintDock* dock = w.lintWidget();
    dock->refresh();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QTreeWidgetItem* row = rowAt(dock->list(), QStringLiteral("30,20"), QStringLiteral("merge"));
    REQUIRE(row);
    w.canvasView()->setCellCursor(Cursor{0, 0});
    emit dock->list()->itemClicked(row, 0);
    CHECK(w.canvasView()->cellCursor() == Cursor{30, 20});
    w.canvasView()->setCellCursor(Cursor{0, 0});
    emit dock->list()->itemActivated(row, 0);
    CHECK(w.canvasView()->cellCursor() == Cursor{30, 20});
}

TEST_CASE("the Lint dock's ramp box lints under one ramp") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    LintDock* dock = w.lintWidget();
    const int steel = dock->rampBox()->findText(QStringLiteral("steel"));
    REQUIRE(steel > 0);
    dock->rampBox()->setCurrentIndex(steel);
    CHECK(dock->stale());
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    CHECK(dock->report().findings == lintAll(*doc, findRamp("steel")).findings);
    for (const lint::Finding& f : dock->report().findings)
        if (lint::ladderKind(f.kind))
            CHECK(f.ramp == "steel");
    dock->rampBox()->setCurrentIndex(0);
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    CHECK(dock->report().findings == lintAll(*doc).findings);
}

TEST_CASE("the IRC View dock shows the canvas as IRC does, and catches up when shown") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    IrcViewDock* irc = w.ircViewWidget();
    REQUIRE(irc);
    // a tab behind Lint: it waits until it is shown
    QApplication::processEvents();
    REQUIRE_FALSE(irc->shown());
    CHECK(w.lintWidget()->shown());
    CHECK(irc->stale());

    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    CHECK(irc->budget() == irc::kEfnetWeechatBudget);
    const irc::View want = irc::ircView(doc->canvas());
    CHECK(irc->view().cutRows() == want.cutRows());
    CHECK(irc->image() == renderIrcView(want, glyphWidth(doc->ansi())));
    CHECK(irc->picture()->pixmap().size() == irc->image().size());
    CHECK(irc->statusLabel()->text() == QStringLiteral("Every line fits 407 bytes"));

    // a tight budget cuts rows and says how many
    REQUIRE(doc->setIrcBudget(project::IrcBudget{std::string(irc::kCustomProfile), 60}));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    const int cut = irc::ircView(doc->canvas(), 60).cutRows();
    REQUIRE(cut > 1);
    CHECK(irc->view().cutRows() == cut);
    CHECK(irc->statusLabel()->text() == QStringLiteral("%1 rows cut at 60 bytes").arg(cut));
    CHECK(irc->image() != renderIrcView(want, glyphWidth(doc->ansi())));

    // an edit shows a moment later
    REQUIRE(doc->edit(merge(*doc, 0, 0)));
    CHECK(irc->stale());
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    CHECK(irc->image() == renderIrcView(irc::ircView(doc->canvas(), 60), glyphWidth(doc->ansi())));

    // hidden, it lets edits wait
    w.lintDock()->raise();
    REQUIRE(waitFor([&] { return !irc->shown(); }));
    REQUIRE(doc->edit(merge(*doc, 1, 0)));
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 2 * kRefreshDelayMs)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(irc->stale());
    CHECK(irc->image() != renderIrcView(irc::ircView(doc->canvas(), 60), glyphWidth(doc->ansi())));
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    CHECK(irc->image() == renderIrcView(irc::ircView(doc->canvas(), 60), glyphWidth(doc->ansi())));
}


TEST_CASE("the IRC View dock cuts at the document's budget, the row meter's") {
    QTemporaryDir dir;
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    project::Project p = project::fromAnsi(piece());
    p.ircBudget = {std::string(irc::kCustomProfile), 150};
    project::writeZip(p, std::filesystem::path(QFile::encodeName(bwp).toStdString()));

    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    IrcViewDock* irc = w.ircViewWidget();
    IrcMeter* meter = w.ircMeter();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));

    // The view, caught up, cuts at `bytes`, the document's and the meter's
    // budget, and cuts the rows the meter calls over, row for row.
    const auto cutsAt = [&](std::size_t bytes) {
        INFO(bytes << " bytes");
        REQUIRE(waitFor([&] { return !irc->stale(); }));
        CHECK(doc->extras().ircBudget.bytes == bytes);
        CHECK(meter->budget().bytes == bytes);
        CHECK(irc->budget() == bytes);
        REQUIRE(irc->view().rows.size() == std::size_t(doc->canvas().height()));
        int over = 0;
        for (int y = 0; y < doc->canvas().height(); ++y) {
            INFO("row " << y);
            const bool cut = irc->view().rows[std::size_t(y)].cut();
            CHECK(cut == (meter->state(y) == IrcMeter::RowState::Over));
            over += cut;
        }
        CHECK(irc->view().cutRows() == over);
        CHECK(over == irc::ircView(doc->canvas(), bytes).cutRows());
        CHECK(irc->statusLabel()->text().contains(QString::number(bytes)));
        return over;
    };
    CHECK(cutsAt(irc::kEfnetWeechatBudget) == 0);

    // Document::setIrcBudget
    REQUIRE(doc->setIrcBudget(project::IrcBudget{std::string(irc::kCustomProfile), 60}));
    CHECK(irc->stale());
    const int at60 = cutsAt(60);
    CHECK(at60 > 1);

    // View > IRC > Budget...
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* dialog = qobject_cast<IrcBudgetDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialog);
        dialog->bytesBox()->setValue(200);
        dialog->accept();
        ran = true;
    });
    w.findChild<QAction*>(QStringLiteral("ircBudget"))->trigger();
    REQUIRE(ran);
    CHECK(irc->stale());
    const int at200 = cutsAt(200);
    CHECK(at200 < at60);

    // undo and redo
    doc->undoStack()->undo();
    CHECK(irc->stale());
    CHECK(cutsAt(60) == at60);
    doc->undoStack()->undo();
    CHECK(cutsAt(irc::kEfnetWeechatBudget) == 0);
    doc->undoStack()->redo();
    CHECK(cutsAt(60) == at60);
    doc->undoStack()->redo();
    CHECK(cutsAt(200) == at200);

    // another document brings its own: a project, a flat file, a new canvas
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    const int at150 = cutsAt(150);
    CHECK(at150 >= at200);
    CHECK(at150 <= at60);
    doc->replace(piece(), QString());
    CHECK(cutsAt(irc::kEfnetWeechatBudget) == 0);
    REQUIRE(w.openFile(bwp, &error));
    CHECK(cutsAt(150) == at150);
    w.newFile();
    CHECK(cutsAt(irc::kEfnetWeechatBudget) == 0);

    // the dock's toggle sits in View > IRC by the row meter's
    const auto* ircMenu = w.findChild<QMenu*>(QStringLiteral("ircMenu"));
    REQUIRE(ircMenu);
    const QList<QAction*> items = ircMenu->actions();
    const auto meterAt = items.indexOf(w.findChild<QAction*>(QStringLiteral("ircMeterShown")));
    REQUIRE(meterAt >= 0);
    CHECK(items.value(meterAt + 1) == w.ircViewDock()->toggleViewAction());
}

TEST_CASE("the IRC View dock shows the lines Export writes, with Fit, Pad, Swap Shades and iCE off") {
    QTemporaryDir dir;
    const QString txt = dir.filePath(QStringLiteral("sent.txt"));
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(sendPiece(), QString());
    REQUIRE_FALSE(doc->ansi().ice);
    IrcViewDock* irc = w.ircViewWidget();
    IrcMeter* meter = w.ircMeter();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));

    // Exports the piece and checks the dock, caught up, shows what was
    // written; returns how many rows it cuts.
    const auto showsExport = [&]() {
        REQUIRE(waitFor([&] { return !irc->stale(); }));
        QString error;
        REQUIRE(w.exportIrcText(txt, &error, true));
        const std::vector<std::string> sent = linesOf(fileBytes(txt).toStdString());
        const int cut = checkShowsExport(irc->view(), sent);
        CHECK(irc->view().cutRows() == cut);
        CHECK(irc->image() == renderIrcView(irc->view(), glyphWidth(doc->ansi())));
        return cut;
    };

    // nothing on: row 1 is over the budget and cut; the blank rows are dropped
    CHECK(showsExport() == 1);
    CHECK(irc->view().rows[1].bytes == 420);
    CHECK(irc->view().rows[1].cut());
    CHECK(irc->view().rows[0].trimmed);
    CHECK(irc->view().rows[4].trimmed);
    // iCE off: row 3's bright blue goes as dark blue, mIRC 2 (#301)
    CHECK(rgbOf(irc->view().canvas.at(0, 3).bg) == irc::kMircRgb[2]);
    CHECK(fileBytes(txt).split('\n').value(2).startsWith("\x03" "2,2    \x0f"));

    // Fit: row 1 fit to 407 bytes as the ascii script sends it and not cut,
    // as the meter says (#297, #364)
    ircAction(w, "ircFit")->trigger();
    REQUIRE(meter->fit());
    CHECK(irc->stale());
    CHECK(showsExport() == 0);
    CHECK(irc->view().rows[1].bytes == 346);
    CHECK(irc->view().rows[1].bytes == meter->bytes(1));
    // the blank row 0 is dropped, so row 1 is the file's first line
    CHECK(meter->sentBytes(1) == irc::txtSentBytes(linesOf(fileBytes(txt).toStdString())[0]));
    CHECK(meter->sentBytes(1) <= 407);
    CHECK(meter->state(1) == IrcMeter::RowState::Merged);
    CHECK(irc->statusLabel()->text() == QStringLiteral("Every line fits 407 bytes"));

    // Pad: the blank rows are kept and row 2's black cells with them
    const std::size_t unpadded = irc->view().rows[2].bytes;
    ircAction(w, "ircPad")->trigger();
    REQUIRE(meter->pad());
    CHECK(irc->stale());
    CHECK(showsExport() == 0);
    CHECK_FALSE(irc->view().rows[0].trimmed);
    CHECK_FALSE(irc->view().rows[4].trimmed);
    CHECK(irc->view().rows[2].kept == 80);
    CHECK(irc->view().rows[2].bytes > unpadded);

    // Swap Shades: row 2 goes shorter
    const std::size_t unswapped = irc->view().rows[2].bytes;
    ircAction(w, "ircSwapShades")->trigger();
    REQUIRE(meter->swapShades());
    CHECK(irc->stale());
    CHECK(showsExport() == 0);
    CHECK(irc->view().rows[2].bytes < unswapped);
    for (int y = 0; y < doc->canvas().height(); ++y) {
        INFO("row " << y);
        CHECK(irc->view().rows[std::size_t(y)].bytes == meter->bytes(y));
    }

    // iCE on: row 3 goes bright blue, mIRC 12, in the dock as in the export
    REQUIRE(doc->setIce(true));
    CHECK(irc->stale());
    CHECK(showsExport() == 0);
    CHECK(rgbOf(irc->view().canvas.at(0, 3).bg) == irc::kMircRgb[12]);

    // and with each turned off again
    for (const char* name : {"ircFit", "ircPad", "ircSwapShades"}) {
        INFO(name);
        ircAction(w, name)->trigger();
        CHECK(irc->stale());
        showsExport();
    }
    CHECK(irc->view().cutRows() == 1);
}

TEST_CASE("an encoder option turned on while the IRC View dock is hidden shows when it is next shown") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(sendPiece(), QString());
    IrcViewDock* irc = w.ircViewWidget();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    CHECK(irc->view().cutRows() == 1);

    w.lintDock()->raise();
    REQUIRE(waitFor([&] { return !irc->shown(); }));
    ircAction(w, "ircFit")->trigger();
    CHECK(irc->stale());
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    CHECK(irc->view().cutRows() == 0);
}

// #394: the IRC View dock follows the canvas's scroll by cells. A zoomed
// canvas of an 80-column, 50-row piece scrolled sideways and down puts the
// dock's left column and top row on the canvas viewport's, as far as the
// dock's bars go; a scroll of the dock by hand holds until the canvas moves.
TEST_CASE("the IRC View dock scrolls to the canvas viewport's left column and top row (#394)") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "darkmage.ans")), QString());
    REQUIRE(doc->canvas().width() == 80);
    REQUIRE(doc->canvas().height() == 50);
    CanvasView* view = w.canvasView();
    auto* canvasScroll = qobject_cast<QScrollArea*>(view->parentWidget()->parentWidget());
    REQUIRE(canvasScroll);
    QScrollBar* ch = canvasScroll->horizontalScrollBar();
    QScrollBar* cv = canvasScroll->verticalScrollBar();
    view->setZoom(4);
    IrcViewDock* irc = w.ircViewWidget();
    auto* dockScroll = irc->findChild<QScrollArea*>();
    REQUIRE(dockScroll);
    QScrollBar* dh = dockScroll->horizontalScrollBar();
    QScrollBar* dv = dockScroll->verticalScrollBar();
    // floating, at a size of its own: the window's right column is too
    // cramped offscreen for the dock to show more rows than the canvas
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    w.ircViewDock()->setFloating(true);
    w.ircViewDock()->resize(320, 360);
    w.ircViewDock()->show();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    QApplication::processEvents();

    // The canvas's cells are zoomed, the dock's at 1x.
    const QSize cell = view->cellRect(QPoint(0, 0)).size();
    const int gw = glyphWidth(doc->ansi()), gh = vgaFont().height;
    REQUIRE(cell == QSize(4 * gw, 4 * gh));
    REQUIRE(irc->image().size() == QSize(80 * gw, 50 * gh));
    // The canvas viewport's top-left cell, from where the view sits in it.
    const auto origin = [&] {
        const QPoint top = view->mapFrom(canvasScroll->viewport(), QPoint(0, 0));
        return QPoint(std::max(0, top.x()) / cell.width(), std::max(0, top.y()) / cell.height());
    };
    const auto canvasTo = [&](int x, int y) {
        ch->setValue(x);
        cv->setValue(y);
        QApplication::processEvents();
    };
    REQUIRE(ch->maximum() > 0);
    REQUIRE(cv->maximum() > 0);
    REQUIRE(dh->maximum() > 0);
    REQUIRE(dv->maximum() > 0);

    // sideways and down, a few pixels into a cell: the dock is at that
    // cell's left column and top row, by cells
    canvasTo(9 * cell.width() + 5, 3 * cell.height() + 7);
    CHECK(origin() == QPoint(9, 3));
    REQUIRE(9 * gw < dh->maximum());
    REQUIRE(3 * gh < dv->maximum());
    CHECK(dh->value() == 9 * gw);
    CHECK(dv->value() == 3 * gh);

    // scrolled on: it moves with the canvas, a cell at a time
    canvasTo(10 * cell.width(), 4 * cell.height() + cell.height() - 1);
    CHECK(origin() == QPoint(10, 4));
    CHECK(dh->value() == 10 * gw);
    CHECK(dv->value() == 4 * gh);

    // at the far ends the canvas's cell is past the dock's range: clamped
    canvasTo(ch->maximum(), cv->maximum());
    const QPoint end = origin();
    REQUIRE(end.x() * gw > dh->maximum());
    REQUIRE(end.y() * gh > dv->maximum());
    CHECK(dh->value() == dh->maximum());
    CHECK(dv->value() == dv->maximum());
    // and back at the start, at its own
    canvasTo(0, 0);
    CHECK(dh->value() == 0);
    CHECK(dv->value() == 0);

    // a scroll of the dock by hand holds: through an edit, a hide and a show
    canvasTo(6 * cell.width(), 2 * cell.height());
    CHECK(dh->value() == 6 * gw);
    dh->triggerAction(QAbstractSlider::SliderPageStepAdd);
    dv->triggerAction(QAbstractSlider::SliderSingleStepAdd);
    const int handX = dh->value(), handY = dv->value();
    REQUIRE(handX != 6 * gw);
    REQUIRE(handY != 2 * gh);
    REQUIRE(doc->edit(merge(*doc, 0, 0)));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    QApplication::processEvents();
    CHECK(dh->value() == handX);
    CHECK(dv->value() == handY);
    w.ircViewDock()->close();
    REQUIRE(waitFor([&] { return !irc->shown(); }));
    w.ircViewDock()->show();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    QApplication::processEvents();
    CHECK(dh->value() == handX);
    CHECK(dv->value() == handY);
    // until the canvas scrolls again
    canvasTo(7 * cell.width(), 2 * cell.height());
    CHECK(dh->value() == 7 * gw);
    CHECK(dv->value() == 2 * gh);
    // the wheel is a scroll by hand too: a resize of the dock, which changes
    // its range, leaves it where the wheel put it
    QWheelEvent wheel(QPointF(5, 5), dockScroll->viewport()->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(dockScroll->viewport(), &wheel);
    const int wheelY = dv->value();
    REQUIRE(wheelY > 2 * gh);
    const int rangeBefore = dv->maximum();
    w.ircViewDock()->resize(320, 340);
    QApplication::processEvents();
    REQUIRE(dv->maximum() != rangeBefore);
    REQUIRE(wheelY <= dv->maximum());
    CHECK(dv->value() == wheelY);
    CHECK(dh->value() == 7 * gw);
    canvasTo(7 * cell.width(), 3 * cell.height());
    CHECK(dv->value() == 3 * gh);

    // a zoom keeps the rule: the new zoom's cell at the viewport's corner
    canvasTo(20 * cell.width(), 8 * cell.height());
    view->setZoom(2);
    QApplication::processEvents();
    const QSize cell2 = view->cellRect(QPoint(0, 0)).size();
    REQUIRE(cell2 == QSize(2 * gw, 2 * gh));
    {
        const QPoint top = view->mapFrom(canvasScroll->viewport(), QPoint(0, 0));
        const QPoint o(std::max(0, top.x()) / cell2.width(), std::max(0, top.y()) / cell2.height());
        CHECK(dh->value() == std::min(o.x() * gw, dh->maximum()));
        CHECK(dv->value() == std::min(o.y() * gh, dv->maximum()));
    }

    // closed while the canvas scrolls, it lines up when it is opened again
    view->setZoom(4);
    QApplication::processEvents();
    w.ircViewDock()->close();
    REQUIRE(waitFor([&] { return !irc->shown(); }));
    canvasTo(8 * cell.width(), 5 * cell.height());
    w.ircViewDock()->show();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    QApplication::processEvents();
    CHECK(dh->value() == 8 * gw);
    CHECK(dv->value() == 5 * gh);
    // and docked again, in a tab behind Lint while the canvas scrolls: lined
    // up once it is brought to the front, in the tab's smaller range
    w.ircViewDock()->setFloating(false);
    w.tabifyDockWidget(w.lintDock(), w.ircViewDock());
    w.lintDock()->raise();
    REQUIRE(waitFor([&] { return !irc->shown(); }));
    canvasTo(3 * cell.width(), 2 * cell.height());
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    QApplication::processEvents();
    REQUIRE(3 * gw <= dh->maximum());
    REQUIRE(2 * gh <= dv->maximum());
    CHECK(dh->value() == 3 * gw);
    CHECK(dv->value() == 2 * gh);
}

TEST_CASE("the IRC View dock lines up again when the canvas changes size, and a narrow picture stays put (#394)") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "darkmage.ans")), QString());
    CanvasView* view = w.canvasView();
    auto* canvasScroll = qobject_cast<QScrollArea*>(view->parentWidget()->parentWidget());
    REQUIRE(canvasScroll);
    view->setZoom(4);
    IrcViewDock* irc = w.ircViewWidget();
    auto* dockScroll = irc->findChild<QScrollArea*>();
    REQUIRE(dockScroll);
    QScrollBar* dh = dockScroll->horizontalScrollBar();
    QScrollBar* dv = dockScroll->verticalScrollBar();
    // floating, wider than a 30-column picture
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    w.ircViewDock()->setFloating(true);
    w.ircViewDock()->resize(400, 360);
    w.ircViewDock()->show();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    QApplication::processEvents();
    const int gh = vgaFont().height;
    const auto expect = [&](QScrollBar* d, int canvasPixel, int canvasCell, int dockCell) {
        return std::min((std::max(0, canvasPixel) / canvasCell) * dockCell, d->maximum());
    };
    const auto lined = [&] {
        const QSize cell = view->cellRect(QPoint(0, 0)).size();
        const int gw = glyphWidth(doc->ansi());
        const QPoint top = view->mapFrom(canvasScroll->viewport(), QPoint(0, 0));
        return dh->value() == expect(dh, top.x(), cell.width(), gw) &&
               dv->value() == expect(dv, top.y(), cell.height(), gh);
    };

    // a wider and taller piece: the new canvas's corner cell, in the new picture
    canvasScroll->horizontalScrollBar()->setValue(3 * 4 * glyphWidth(doc->ansi()));
    canvasScroll->verticalScrollBar()->setValue(2 * 4 * gh);
    QApplication::processEvents();
    doc->replace(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "cocktober.ans")), QString());
    REQUIRE(doc->canvas().width() == 120);
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    QApplication::processEvents();
    REQUIRE(irc->image().width() == 120 * glyphWidth(doc->ansi()));
    canvasScroll->horizontalScrollBar()->setValue(40 * 4 * glyphWidth(doc->ansi()));
    canvasScroll->verticalScrollBar()->setValue(30 * 4 * gh);
    QApplication::processEvents();
    REQUIRE(dh->value() > 0);
    CHECK(lined());

    // a piece narrower than the dock: nothing to scroll sideways, though the
    // canvas is scrolled
    doc->replace(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans")), QString());
    REQUIRE(doc->canvas().width() == 30);
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    QApplication::processEvents();
    REQUIRE(irc->image().width() <= dockScroll->viewport()->width());
    canvasScroll->horizontalScrollBar()->setValue(canvasScroll->horizontalScrollBar()->maximum());
    QApplication::processEvents();
    REQUIRE(view->mapFrom(canvasScroll->viewport(), QPoint(0, 0)).x() > 0);
    CHECK(dh->maximum() == 0);
    CHECK(dh->value() == 0);
    CHECK(lined());
}

// A `w` by `h` piece of 'x's in colours, every row sent (no blank rows trimmed).
static AnsiDocument filledPiece(int w, int h) {
    AnsiDocument d;
    d.canvas = Canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Cell& c = d.canvas.at(x, y);
            c.ch = U'x';
            c.fg = Color::indexed(std::uint8_t((x + y) % 7 + 1));
            c.bg = Color::indexed(0);
        }
    return d;
}

// #394, round 1: a resize of the window's right column, the IRC View docked
// in it, moves the canvas only to centre it again in its narrower viewport.
// That isn't a scroll of the canvas, so a scroll of the dock by hand holds;
// the canvas scrolling after it lines the dock up again.
TEST_CASE("a docked IRC View scrolled by hand holds when its column is widened and the canvas centred again (#394)") {
    MainWindow w;
    w.resize(1600, 900);
    w.show();
    Document* doc = w.document();
    doc->replace(filledPiece(30, 120), QString());
    CanvasView* view = w.canvasView();
    view->setZoom(1);
    auto* canvasScroll = qobject_cast<QScrollArea*>(view->parentWidget()->parentWidget());
    REQUIRE(canvasScroll);
    QScrollBar* ch = canvasScroll->horizontalScrollBar();
    QScrollBar* cv = canvasScroll->verticalScrollBar();
    IrcViewDock* irc = w.ircViewWidget();
    QScrollBar* dv = irc->scrollArea()->verticalScrollBar();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    QApplication::processEvents();
    REQUIRE_FALSE(w.ircViewDock()->isFloating());
    REQUIRE(w.dockWidgetArea(w.ircViewDock()) == Qt::RightDockWidgetArea);
    const int gh = vgaFont().height;
    REQUIRE(view->cellRect(QPoint(0, 0)).height() == gh);

    // the canvas narrower than its viewport, centred; scrolled down 4 rows
    REQUIRE(ch->maximum() == 0);
    REQUIRE(view->x() > 0);
    cv->setValue(4 * gh);
    QApplication::processEvents();
    REQUIRE(irc->followingCanvas());
    REQUIRE(dv->maximum() > 6 * gh);
    CHECK(dv->value() == 4 * gh);
    // the dock scrolled by hand, past the canvas's row
    dv->triggerAction(QAbstractSlider::SliderSingleStepAdd);
    QApplication::processEvents();
    const int hand = dv->value();
    REQUIRE(hand > 4 * gh);
    REQUIRE_FALSE(irc->followingCanvas());

    // the right column 100 pixels wider: the canvas moves, centred again,
    // without a scroll
    const int canvasX = view->x();
    const int column = w.ircViewDock()->width();
    w.resizeDocks({w.ircViewDock()}, {column + 100}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.ircViewDock()->width() > column);
    REQUIRE(view->x() != canvasX);
    REQUIRE(ch->maximum() == 0);
    REQUIRE(cv->value() == 4 * gh);
    CHECK_FALSE(irc->followingCanvas());
    CHECK(dv->value() == hand);

    // the canvas scrolled, even by less than a cell: lined up again
    cv->setValue(4 * gh + 3);
    QApplication::processEvents();
    CHECK(irc->followingCanvas());
    CHECK(dv->value() == 4 * gh);
    cv->setValue(6 * gh);
    QApplication::processEvents();
    CHECK(dv->value() == 6 * gh);
}

namespace {

// #394: a window whose canvas, 30x120 at zoom 1, is scrolled four rows down,
// the IRC View dock shown, redrawn and lined up with it.
struct ScrolledDock {
    MainWindow w;
    Document* doc = nullptr;
    QScrollBar* cv = nullptr;  // the canvas's vertical bar
    IrcViewDock* irc = nullptr;
    QScrollBar* dv = nullptr;  // the dock's
    int gh = vgaFont().height;
    int edits = 0;

    ScrolledDock() {
        w.resize(1600, 900);
        w.show();
        doc = w.document();
        doc->replace(filledPiece(30, 120), QString());
        CanvasView* view = w.canvasView();
        view->setZoom(1);
        auto* canvasScroll = qobject_cast<QScrollArea*>(view->parentWidget()->parentWidget());
        REQUIRE(canvasScroll);
        cv = canvasScroll->verticalScrollBar();
        irc = w.ircViewWidget();
        dv = irc->scrollArea()->verticalScrollBar();
        w.ircViewDock()->raise();
        REQUIRE(waitFor([&] { return irc->shown(); }));
        REQUIRE(waitFor([&] { return !irc->stale(); }));
        QApplication::processEvents();
        cv->setValue(4 * gh);
        QApplication::processEvents();
        REQUIRE(irc->followingCanvas());
        REQUIRE(dv->value() == 4 * gh);
        REQUIRE(dv->isVisible());
        REQUIRE(dv->maximum() > 8 * gh);
    }

    // An edit (a new cell of the first row each time), and the dock redrawn
    // after it.
    void editAndRedraw() {
        int redrawn = 0;
        const auto c = QObject::connect(irc, &FollowsDocument::refreshed, [&] { ++redrawn; });
        REQUIRE(doc->edit(merge(*doc, edits++, 0)));
        REQUIRE(waitFor([&] { return redrawn > 0 && !irc->stale(); }));
        QObject::disconnect(c);
        QApplication::processEvents();
    }
};

// Right-clicks `bar` at `at` and picks the item `text` from the bar's own
// context menu from the keyboard, as a user would. Qt runs the menu in its
// own event loop (exec) or leaves it open (popup), depending on the version:
// either way the timer finds it. False if the menu never showed or had no
// such item.
bool pickFromBarMenu(QScrollBar* bar, QPoint at, const QString& text) {
    bool picked = false;
    QTimer pick;
    pick.setInterval(10);
    QObject::connect(&pick, &QTimer::timeout, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu)
            return;
        pick.stop();
        for (QAction* a : menu->actions())
            if (a->text() == text) {
                menu->setActiveAction(a);
                picked = true;
            }
        QKeyEvent press(QEvent::KeyPress, picked ? Qt::Key_Return : Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(menu, &press);
    });
    pick.start();
    // a fail-safe: a menu that never shows doesn't hold the test
    QTimer::singleShot(5000, &pick, [&] {
        pick.stop();
        if (QWidget* p = QApplication::activePopupWidget())
            p->close();
    });
    QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, at, bar->mapToGlobal(at));
    QApplication::sendEvent(bar, &menuEvent);
    return waitFor([&] { return picked && !QApplication::activePopupWidget(); }, 4000);
}

} // namespace

// #394, round 1: the scroll bar's own context menu, "Scroll here", sets the
// bar's value without an action; it is a scroll by hand all the same, and an
// edit's redraw leaves the dock where it put it.
TEST_CASE("the IRC View's scroll bar's Scroll here is a scroll by hand (#394)") {
    ScrolledDock s;
    // three quarters of the way down the bar
    REQUIRE(pickFromBarMenu(s.dv, QPoint(s.dv->width() / 2, s.dv->height() * 3 / 4), QScrollBar::tr("Scroll here")));
    const int here = s.dv->value();
    REQUIRE(here > 8 * s.gh);
    CHECK_FALSE(s.irc->followingCanvas());

    // an edit: the redraw leaves the dock where Scroll here put it
    s.editAndRedraw();
    CHECK(s.dv->value() == here);
    // until the canvas scrolls
    s.cv->setValue(5 * s.gh);
    QApplication::processEvents();
    CHECK(s.dv->value() == 5 * s.gh);
}

// #394, round 2: the menu's other items are slider actions; Top is one.
TEST_CASE("the IRC View's scroll bar's Top is a scroll by hand (#394)") {
    ScrolledDock s;
    REQUIRE(pickFromBarMenu(s.dv, QPoint(s.dv->width() / 2, s.dv->height() / 2), QScrollBar::tr("Top")));
    REQUIRE(s.dv->value() == 0);
    CHECK_FALSE(s.irc->followingCanvas());
    s.editAndRedraw();
    CHECK(s.dv->value() == 0);
    s.cv->setValue(5 * s.gh);
    QApplication::processEvents();
    CHECK(s.dv->value() == 5 * s.gh);
}

// #394, round 2: a pan gesture on the picture moves the bars by its delta
// (QAbstractScrollArea) without a slider action; it is a scroll by hand.
TEST_CASE("a pan gesture on the IRC View is a scroll by hand (#394)") {
    ScrolledDock s;
    QPanGesture pan;
    pan.setLastOffset(QPointF(0, 0));
    pan.setOffset(QPointF(0, -3.0 * s.gh));  // the finger up three rows: the picture down
    QGestureEvent gesture(QList<QGesture*>{&pan});
    QApplication::sendEvent(s.irc->scrollArea()->viewport(), &gesture);
    QApplication::processEvents();
    REQUIRE(s.dv->value() == 7 * s.gh);
    CHECK_FALSE(s.irc->followingCanvas());
    s.editAndRedraw();
    CHECK(s.dv->value() == 7 * s.gh);
}

// #394, round 2: only what the user does is a scroll by hand. A value set
// by code (Qt's own ensureVisible here, a plain setValue) moves the picture
// but leaves it following: the next redraw lines it up with the canvas.
TEST_CASE("the IRC View's bars set by code are not a scroll by hand (#394)") {
    ScrolledDock s;
    s.irc->scrollArea()->ensureVisible(0, 0, 0, 0);
    REQUIRE(s.dv->value() == 0);
    CHECK(s.irc->followingCanvas());
    s.editAndRedraw();
    CHECK(s.dv->value() == 4 * s.gh);

    s.dv->setValue(9 * s.gh);
    REQUIRE(s.dv->value() == 9 * s.gh);
    CHECK(s.irc->followingCanvas());
    s.editAndRedraw();
    CHECK(s.dv->value() == 4 * s.gh);
}

// #394, round 3: an assistive technology sets a bar's value through the
// bar's accessible interface (QAccessibleValueInterface::setCurrentValue);
// it is a scroll by hand, as a drag of the bar is.
TEST_CASE("an assistive technology's scroll of the IRC View is a scroll by hand (#394)") {
    ScrolledDock s; // canvas and dock at four rows down
    auto* iface = QAccessible::queryAccessibleInterface(s.dv);
    REQUIRE(iface);
    REQUIRE(iface->valueInterface());
    iface->valueInterface()->setCurrentValue(9 * s.gh);
    REQUIRE(s.dv->value() == 9 * s.gh);
    CHECK_FALSE(s.irc->followingCanvas());
    s.editAndRedraw();
    CHECK(s.dv->value() == 9 * s.gh);
    // until the canvas scrolls
    s.cv->setValue(5 * s.gh);
    QApplication::processEvents();
    CHECK(s.irc->followingCanvas());
    CHECK(s.dv->value() == 5 * s.gh);
    // a value past the end is clamped, as Qt's own interface clamps it
    iface->valueInterface()->setCurrentValue(s.dv->maximum() + 1000);
    CHECK(s.dv->value() == s.dv->maximum());
    CHECK_FALSE(s.irc->followingCanvas());
}

// #394, round 3: the dock's bars have their own accessible interface; it
// tells an assistive technology what Qt's own (QAccessibleScrollBar) tells
// of a bar with the same range, steps and value.
TEST_CASE("the IRC View's scroll bars tell assistive technology what Qt's do (#394)") {
    ScrolledDock s;
    QScrollArea plain;  // with Qt's own bars
    for (const Qt::Orientation o : {Qt::Horizontal, Qt::Vertical}) {
        CAPTURE(o);
        QScrollArea* area = s.irc->scrollArea();
        QScrollBar* ours = o == Qt::Horizontal ? area->horizontalScrollBar() : area->verticalScrollBar();
        QScrollBar* qts = o == Qt::Horizontal ? plain.horizontalScrollBar() : plain.verticalScrollBar();
        qts->setRange(ours->minimum(), ours->maximum());
        qts->setSingleStep(ours->singleStep());
        qts->setPageStep(ours->pageStep());
        qts->setValue(ours->value());
        QAccessibleInterface* a = QAccessible::queryAccessibleInterface(ours);
        QAccessibleInterface* b = QAccessible::queryAccessibleInterface(qts);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a->object() == ours);
        CHECK(a->role() == QAccessible::ScrollBar);
        CHECK(a->role() == b->role());
        CHECK(a->childCount() == b->childCount());
        for (const QAccessible::Text t : {QAccessible::Name, QAccessible::Description, QAccessible::Value,
                                          QAccessible::Help, QAccessible::Accelerator}) {
            CAPTURE(t);
            CHECK(a->text(t) == b->text(t));
        }
        CHECK(a->text(QAccessible::Value) == QString::number(ours->value()));
        REQUIRE(a->valueInterface());
        REQUIRE(b->valueInterface());
        CHECK(a->valueInterface()->currentValue() == QVariant(ours->value()));
        CHECK(a->valueInterface()->currentValue() == b->valueInterface()->currentValue());
        CHECK(a->valueInterface()->minimumValue() == b->valueInterface()->minimumValue());
        CHECK(a->valueInterface()->maximumValue() == b->valueInterface()->maximumValue());
        CHECK(a->valueInterface()->minimumStepSize() == b->valueInterface()->minimumStepSize());
        CHECK(a->valueInterface()->minimumValue() == QVariant(ours->minimum()));
        CHECK(a->valueInterface()->maximumValue() == QVariant(ours->maximum()));
        CHECK(a->valueInterface()->minimumStepSize() == QVariant(ours->singleStep()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        // the attributes (Qt 6.10 on: Locale; 6.11 on: Orientation too)
        REQUIRE((a->attributesInterface() == nullptr) == (b->attributesInterface() == nullptr));
        if (a->attributesInterface()) {
            const auto keys = a->attributesInterface()->attributeKeys();
            CHECK(keys == b->attributesInterface()->attributeKeys());
            for (const QAccessible::Attribute k : keys) {
                CAPTURE(k);
                CHECK(a->attributesInterface()->attributeValue(k) == b->attributesInterface()->attributeValue(k));
            }
        }
#endif
    }
}

// #394, round 1: the dock's origin is the canvas's cell under its viewport's
// top-left corner on the canvas's own device grid. Run at a device pixel
// ratio of 1.25 too (tests/CMakeLists.txt), where a canvas cell is a
// fractional number of logical pixels: at zoom 4 a cell is 64 device pixels
// tall, 51.2 logical, and a scroll of 103 is 128.75 device pixels down, in
// row 2.
TEST_CASE("the IRC View's origin is the canvas's cell under the viewport's corner, at any device pixel ratio (#394)",
          "[irc-view-dpr]") {
    MainWindow w;
    w.resize(1600, 900);
    w.show();
    Document* doc = w.document();
    doc->replace(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "darkmage.ans")), QString());
    CanvasView* view = w.canvasView();
    view->setZoom(4);
    auto* canvasScroll = qobject_cast<QScrollArea*>(view->parentWidget()->parentWidget());
    REQUIRE(canvasScroll);
    const qreal dpr = view->devicePixelRatioF();
    if (qEnvironmentVariable("QT_SCALE_FACTOR") == QLatin1String("1.25"))
        REQUIRE(dpr == 1.25);
    IrcViewDock* irc = w.ircViewWidget();
    QScrollBar* dh = irc->scrollArea()->horizontalScrollBar();
    QScrollBar* dv = irc->scrollArea()->verticalScrollBar();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    w.ircViewDock()->setFloating(true);
    w.ircViewDock()->resize(320, 360);
    w.ircViewDock()->show();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    QApplication::processEvents();
    const int gw = glyphWidth(doc->ansi()), gh = vgaFont().height;

    for (const QPoint scroll : {QPoint(0, 103), QPoint(77, 103), QPoint(205, 51), QPoint(333, 257)}) {
        CAPTURE(scroll.x(), scroll.y());
        canvasScroll->horizontalScrollBar()->setValue(scroll.x());
        canvasScroll->verticalScrollBar()->setValue(scroll.y());
        QApplication::processEvents();
        REQUIRE(canvasScroll->horizontalScrollBar()->value() == scroll.x());
        REQUIRE(canvasScroll->verticalScrollBar()->value() == scroll.y());
        const QPoint corner = view->mapFrom(canvasScroll->viewport(), QPoint(0, 0));
        REQUIRE(corner == scroll);
        const QPoint o = irc->canvasOrigin();
        // the corner's device pixel is in the origin cell's device pixels:
        // the cell's view rect, in view pixels from the grid's first (a
        // corner in the part of a device pixel before the first is cell 0's)
        const QPointF at = DeviceGrid::of(view).toView(QPointF(corner));
        const QPointF v(std::max(0.0, at.x()), std::max(0.0, at.y()));
        const QRect cellView = view->cellRect(o);
        CHECK(v.x() >= cellView.left());
        CHECK(v.x() < cellView.left() + cellView.width());
        CHECK(v.y() >= cellView.top());
        CHECK(v.y() < cellView.top() + cellView.height());
        CHECK(dh->value() == std::min(o.x() * gw, dh->maximum()));
        CHECK(dv->value() == std::min(o.y() * gh, dv->maximum()));
        if (dpr == 1.25 && scroll.y() == 103)
            CHECK(o.y() == 2);
        if (dpr == 1.0 && scroll.y() == 103)
            CHECK(o.y() == 1);
    }
}

TEST_CASE("render --irc with export's options draws the IRC View dock, and both show what export --irc writes") {
    QTemporaryDir dir;
    // a project with its own budget, saved with iCE turned off: its bright
    // blue goes as dark blue
    const QString bwp = dir.filePath(QStringLiteral("sent.bwp"));
    project::Project p = project::fromAnsi(sendPiece());
    REQUIRE(p.ice);  // a bright background opens with iCE on
    p.ice = false;
    p.ircBudget = {std::string(irc::kCustomProfile), 380};
    project::writeZip(p, std::filesystem::path(QFile::encodeName(bwp).toStdString()));

    MainWindow w;
    w.show();
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    Document* doc = w.document();
    REQUIRE_FALSE(doc->ansi().ice);
    IrcViewDock* irc = w.ircViewWidget();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));

    const QString png = dir.filePath(QStringLiteral("view.png"));
    const QString txt = dir.filePath(QStringLiteral("cli.txt"));
    const QString appTxt = dir.filePath(QStringLiteral("app.txt"));
    // the options turned on in the app one by one, the same flags to the CLI
    const std::pair<const char*, QString> options[] = {
        {nullptr, QString()},
        {"ircFit", QStringLiteral("--fit")},
        {"ircPad", QStringLiteral("--pad")},
        {"ircSwapShades", QStringLiteral("--swap-shades")},
    };
    QStringList flags;
    for (const auto& [action, flag] : options) {
        if (action) {
            ircAction(w, action)->trigger();
            flags << flag;
        }
        INFO("flags: " << flags.join(QLatin1Char(' ')).toStdString());
        REQUIRE(waitFor([&] { return !irc->stale(); }));

        const CliRun render =
            runCli(QStringList{QStringLiteral("render"), QStringLiteral("--irc")} + flags + QStringList{bwp, png});
        INFO(render.out.toStdString());
        REQUIRE(render.status == 0);
        const int cut = irc->view().cutRows();
        CHECK(render.out.contains(QStringLiteral("IRC view: %1 %2 cut at 380 bytes")
                                      .arg(cut)
                                      .arg(cut == 1 ? QStringLiteral("row") : QStringLiteral("rows"))));
        QImage drawn(png);
        REQUIRE_FALSE(drawn.isNull());
        CHECK(drawn.convertToFormat(irc->image().format()) == irc->image());

        const CliRun exported = runCli(QStringList{QStringLiteral("export"), QStringLiteral("--irc")} + flags +
                                       QStringList{QStringLiteral("--allow-over-budget"), bwp, txt});
        INFO(exported.out.toStdString());
        REQUIRE(exported.status == 0);
        REQUIRE(w.exportIrcText(appTxt, &error, true));
        CHECK(fileBytes(txt) == fileBytes(appTxt));
        CHECK(checkShowsExport(irc->view(), linesOf(fileBytes(txt).toStdString())) == cut);
        CHECK(rgbOf(irc->view().canvas.at(0, 3).bg) == irc::kMircRgb[2]);
    }
    // fit at the project's 380 bytes leaves nothing cut
    CHECK(irc->view().cutRows() == 0);
}

TEST_CASE("the Lint dock lints off the GUI thread: the window keeps going while it runs") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    LintDock* dock = w.lintWidget();
    REQUIRE(waitFor([&] { return dock->shown(); }));
    doc->replace(big(), QString());
    QElapsedTimer t;
    t.start();
    const lint::LintReport want = lintAll(*doc);
    const qint64 lintMs = t.elapsed();
    REQUIRE(lintMs > 200);

    // a timer due at once fires before the result is in
    bool ticked = false, readyAtTick = true;
    QTimer::singleShot(0, [&] {
        ticked = true;
        readyAtTick = dock->report().findings == want.findings;
    });
    t.restart();
    dock->refresh();
    const qint64 refreshMs = t.elapsed();
    CHECK(refreshMs * 4 < lintMs);
    REQUIRE(waitFor([&] { return ticked; }));
    CHECK_FALSE(readyAtTick);
    REQUIRE(waitFor([&] { return dock->report().findings == want.findings; }, 20000));
    // the list holds the first kMaxLintRows; the summary counts them all
    REQUIRE(want.findings.size() > std::size_t(kMaxLintRows));
    CHECK(dock->list()->topLevelItemCount() == kMaxLintRows);
    CHECK(dock->list()->topLevelItem(0)->text(0) ==
          QStringLiteral("%1,%2").arg(want.findings[0].x).arg(want.findings[0].y));
    CHECK(dock->summaryLabel()->text() == QString::fromStdString(lint::summary(want)) +
                                              QStringLiteral("\nThe first 1000 are listed."));
}

TEST_CASE("the Lint dock shows only the newest result: one started before an edit is dropped") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    LintDock* dock = w.lintWidget();
    REQUIRE(waitFor([&] { return dock->shown(); }));
    const lint::LintReport first = lintAll(*doc);
    REQUIRE(waitFor([&] { return dock->report().findings == first.findings; }));

    int shownResults = 0;
    QObject::connect(dock->list()->model(), &QAbstractItemModel::rowsInserted, [&] { ++shownResults; });
    REQUIRE(doc->edit(merge(*doc, 3, 2)));
    dock->refresh();
    // refresh starts the lint and returns: the last result stays until the new one is in
    CHECK(dock->report().findings == first.findings);
    REQUIRE(doc->edit(merge(*doc, 30, 20)));
    dock->refresh();
    const lint::LintReport last = lintAll(*doc);
    REQUIRE(waitFor([&] { return dock->report().findings == last.findings; }));
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 2 * kRefreshDelayMs)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(shownResults == 1);
    CHECK(dock->report().findings == last.findings);
    CHECK(rowAt(dock->list(), QStringLiteral("3,2"), QStringLiteral("merge")));
    CHECK(rowAt(dock->list(), QStringLiteral("30,20"), QStringLiteral("merge")));
}

TEST_CASE("the IRC View dock renders off the GUI thread too") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    IrcViewDock* irc = w.ircViewWidget();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    const QImage whole = renderIrcView(irc::ircView(doc->canvas()), glyphWidth(doc->ansi()));
    REQUIRE(waitFor([&] { return irc->image() == whole; }));
    REQUIRE(doc->setIrcBudget(project::IrcBudget{std::string(irc::kCustomProfile), 60}));
    // the old view until the new one is in
    CHECK(irc->image() == whole);
    CHECK(irc->view().cutRows() == 0);
    const int cut = irc::ircView(doc->canvas(), 60).cutRows();
    REQUIRE(waitFor([&] { return irc->view().cutRows() == cut; }));
    CHECK(irc->image() == renderIrcView(irc::ircView(doc->canvas(), 60), glyphWidth(doc->ansi())));
    CHECK(irc->statusLabel()->text() == QStringLiteral("%1 rows cut at 60 bytes").arg(cut));
}

TEST_CASE("the IRC View dock marks a row cut only past its width (#364)") {
    // 80 lone wide 中, bright red and red by turns on black: every cell the
    // view shows is sent at 407 bytes, but the script cuts the line
    MainWindow w;
    w.show();
    Document* doc = w.document();
    AnsiDocument wide;
    wide.canvas = Canvas(80, 1);
    for (int x = 0; x < 80; ++x)
        wide.canvas.at(x, 0) = Cell{U'中', Color::indexed(x % 2 ? Red : LightRed), Color::indexed(Black), 0, false};
    doc->replace(wide, QString());
    REQUIRE(doc->setTarget(Target::Irc));
    IrcViewDock* irc = w.ircViewWidget();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown() && irc->view().cutRows() == 1; }));
    CHECK(irc->view().rows[0].shown == 80);
    CHECK(irc->statusLabel()->text() == QStringLiteral("1 row cut at 407 bytes"));
    const QImage img = irc->image();
    CHECK(img == ircNow(*doc));
    const int bar = glyphWidth(doc->ansi()) / 4;
    for (int py = 0; py < img.height(); ++py) {
        CHECK(img.pixel(img.width() - 1, py) == kIrcCutMark);
        CHECK(img.pixel(img.width() - bar, py) == kIrcCutMark);
    }
}

TEST_CASE("a budget change drops the IRC View's render in flight: it never shows the old budget") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    IrcViewDock* irc = w.ircViewWidget();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    REQUIRE(irc->statusLabel()->text() == QStringLiteral("Every line fits 407 bytes"));

    // each result shown, and whether it was the document's at the time
    QStringList shown;
    QList<bool> current;
    QObject::connect(irc, &FollowsDocument::refreshed, [&] {
        shown << irc->statusLabel()->text();
        current << (irc->image() == ircNow(*doc));
    });

    // a render at 60 bytes, held on its worker
    Hold hold(irc);
    REQUIRE(doc->setIrcBudget(project::IrcBudget{std::string(irc::kCustomProfile), 60}));
    irc->refresh();
    REQUIRE(hold.started());
    // the budget moves on to 200 while it runs
    REQUIRE(doc->setIrcBudget(project::IrcBudget{std::string(irc::kCustomProfile), 200}));
    CHECK(irc->stale());
    // its result comes in before any timer fires
    hold.release();
    REQUIRE(postedUntil([&] { return !irc->busy(); }));
    for (const QString& s : shown)
        CHECK_FALSE(s.contains(QStringLiteral(" 60 bytes")));
    CHECK_FALSE(current.contains(false));
    // a new budget is redrawn at once, with no timer: 200 is already in
    CHECK(shown.size() == 1);
    CHECK_FALSE(irc->stale());

    // and the view is caught up with 200
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    CHECK(irc->image() == ircNow(*doc));
    CHECK(irc->statusLabel()->text().endsWith(QStringLiteral(" 200 bytes")));
    CHECK_FALSE(current.contains(false));
}

TEST_CASE("an edit or another document or format drops the IRC View's render in flight") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    IrcViewDock* irc = w.ircViewWidget();
    w.ircViewDock()->raise();
    REQUIRE(waitFor([&] { return irc->shown(); }));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    REQUIRE(doc->edit(merge(*doc, 0, 0)));
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    REQUIRE(irc->image() == ircNow(*doc));

    int results = 0;
    QList<bool> current;
    QObject::connect(irc, &FollowsDocument::refreshed, [&] {
        ++results;
        current << (irc->image() == ircNow(*doc));
    });

    // a render of the document as it is, held on its worker
    Hold hold(irc);
    irc->refresh();
    REQUIRE(hold.started());
    const QImage before = ircNow(*doc);

    SECTION("an edit") {
        REQUIRE(doc->edit(merge(*doc, 5, 3)));
    }
    SECTION("replace") {
        doc->replace(piece(), QString());
    }
    SECTION("New") {
        doc->clear();
    }
    SECTION("open") {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("piece.ans"));
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        const std::string bytes = bwtest::readFile(bwtest::dataDir() / "golden" / "cafe-mug.ans");
        f.write(bytes.data(), qint64(bytes.size()));
        f.close();
        QString error;
        REQUIRE(w.openFile(path, &error));
    }
    SECTION("a format change") {
        REQUIRE(doc->setLetterSpacing(9));
    }
    REQUIRE(ircNow(*doc) != before);
    CHECK(irc->stale());

    // its result comes in before any timer fires: it is dropped
    hold.release();
    REQUIRE(postedUntil([&] { return !irc->busy(); }));
    CHECK(results == 0);
    CHECK(irc->image() == before);
    CHECK(irc->stale());

    // and the view catches up a moment later
    REQUIRE(waitFor([&] { return !irc->stale(); }));
    CHECK(irc->image() == ircNow(*doc));
    CHECK(results == 1);
    CHECK_FALSE(current.contains(false));
}

TEST_CASE("an edit drops the Lint dock's lint in flight") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    LintDock* dock = w.lintWidget();
    REQUIRE(waitFor([&] { return dock->shown(); }));
    REQUIRE(waitFor([&] { return !dock->stale(); }));

    QList<bool> current;
    QObject::connect(dock, &FollowsDocument::refreshed,
                     [&] { current << (dock->report().findings == lintAll(*doc).findings); });
    Hold hold(dock);
    dock->refresh();
    REQUIRE(hold.started());
    REQUIRE(doc->edit(merge(*doc, 3, 2)));
    hold.release();
    REQUIRE(postedUntil([&] { return !dock->busy(); }));
    CHECK(current.isEmpty());
    CHECK_FALSE(rowAt(dock->list(), QStringLiteral("3,2"), QStringLiteral("merge")));

    REQUIRE(waitFor([&] { return !dock->stale(); }));
    CHECK(rowAt(dock->list(), QStringLiteral("3,2"), QStringLiteral("merge")));
    CHECK(current == QList<bool>{true});
}

TEST_CASE("a dock behind the document for a while says so; one caught up at once doesn't") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    FollowsDocument* dock = nullptr;
    QString note;
    SECTION("the Lint dock") {
        dock = w.lintWidget();
        note = QStringLiteral("Linting…");
    }
    SECTION("the IRC View dock") {
        w.ircViewDock()->raise();
        dock = w.ircViewWidget();
        note = QStringLiteral("Updating…");
    }
    QLabel* label = dock->staleLabel();
    REQUIRE(waitFor([&] { return dock->shown(); }));
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    CHECK(label->isHidden());
    CHECK(label->text() == note);
    CHECK(dock->isAncestorOf(label));

    // an edit caught up within kStaleShowMs shows no note
    bool noted = false;
    QElapsedTimer t;
    t.start();
    REQUIRE(doc->edit(merge(*doc, 3, 2)));
    REQUIRE(waitFor([&] {
        noted = noted || !label->isHidden();
        return !dock->stale();
    }));
    if (t.elapsed() < kStaleShowMs)
        CHECK_FALSE(noted);
    CHECK(label->isHidden());

    // a refresh held on its worker while the document moves on
    Hold hold(dock);
    dock->refresh();
    REQUIRE(hold.started());
    REQUIRE(doc->edit(merge(*doc, 30, 20)));
    CHECK(dock->stale());
    // not at once: a moment behind is how every edit starts
    CHECK(label->isHidden());
    t.restart();
    while (t.elapsed() < kStaleShowMs + 200)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(dock->stale());
    CHECK_FALSE(label->isHidden());
    // caught up, the note goes
    hold.release();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    CHECK(label->isHidden());
}

TEST_CASE("a Lint dock that can't keep up with the edits says its result is behind") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    LintDock* dock = w.lintWidget();
    REQUIRE(waitFor([&] { return dock->shown(); }));
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QLabel* label = dock->staleLabel();
    CHECK(label->isHidden());
    int results = 0;
    QObject::connect(dock, &FollowsDocument::refreshed, [&] { ++results; });

    // the review's repro, an edit every 2 * kRefreshDelayMs, with the lint
    // held on its worker as long as the edits go on: each edit drops the
    // lint in flight and its refresh waits behind it, so the result shown
    // stays behind the document; what the dock shows has to say so, and
    // keep saying so
    Hold hold(dock);
    REQUIRE(doc->edit(merge(*doc, 10, 2)));
    dock->refresh();
    REQUIRE(hold.started());
    bool caughtUp = false, noted = false, unnoted = false;
    auto edit = [&](int x) {
        REQUIRE(doc->edit(merge(*doc, x, 2)));
        QElapsedTimer gap;
        gap.start();
        while (gap.elapsed() < 2 * kRefreshDelayMs) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            caughtUp = caughtUp || !dock->stale();
            if (!label->isHidden())
                noted = true;
            else if (noted)
                unnoted = true;
        }
    };
    for (int i = 1; i <= 4; ++i)
        edit(10 + i);
    // behind for longer than kStaleShowMs, the note is up
    REQUIRE(waitFor([&] { return !label->isHidden(); }));
    noted = true;
    for (int i = 5; i <= 8; ++i)
        edit(10 + i);
    CHECK_FALSE(caughtUp);
    CHECK_FALSE(unnoted);
    CHECK_FALSE(label->isHidden());
    CHECK(results == 0);

    // once the edits stop it catches up, and the note goes
    hold.release();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    CHECK(label->isHidden());
    CHECK(results == 1);
    CHECK(dock->report().findings == lintAll(*doc).findings);
}

TEST_CASE("a refresh asked for while one runs takes the note down once its result is current") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    doc->replace(piece(), QString());
    LintDock* dock = w.lintWidget();
    REQUIRE(waitFor([&] { return dock->shown(); }));
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QLabel* label = dock->staleLabel();
    QList<bool> current;
    QObject::connect(dock, &FollowsDocument::refreshed,
                     [&] { current << (dock->report().findings == lintAll(*doc).findings); });

    // the review's repro: an edit's refresh held until the note is up
    Hold hold(dock);
    REQUIRE(doc->edit(merge(*doc, 3, 2)));
    dock->refresh();
    REQUIRE(hold.started());
    REQUIRE(waitFor([&] { return !label->isHidden(); }));
    // another edit, and a refresh that waits for the held one
    REQUIRE(doc->edit(merge(*doc, 30, 20)));
    dock->refresh();
    // and an edit after that refresh was asked for
    REQUIRE(doc->edit(merge(*doc, 10, 2)));
    // the held lint is dropped; the refresh waiting for it reads the
    // document as it is now, so its result is current. No timer fires.
    hold.release();
    REQUIRE(postedUntil([&] { return !dock->busy(); }));
    CHECK(current == QList<bool>{true});
    CHECK(rowAt(dock->list(), QStringLiteral("30,20"), QStringLiteral("merge")));
    CHECK_FALSE(dock->stale());
    CHECK(label->isHidden());
}

TEST_CASE("the Lint dock shows ans-loss as a note, in italics, and the summary clean") {
    MainWindow w;
    w.show();
    Document* doc = w.document();
    AnsiDocument small;
    small.canvas = Canvas(4, 1);
    doc->replace(small, QString());
    REQUIRE(doc->setTarget(Target::Irc));
    Cell x;
    x.ch = U'X';
    x.fg = swatchColor(77);
    REQUIRE(doc->edit(CellEdit{{CellChange{0, 0, doc->canvas().at(0, 0), x}}}));
    LintDock* dock = w.lintWidget();
    dock->refresh();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QTreeWidgetItem* note = rowAt(dock->list(), QStringLiteral("0,0"), QStringLiteral("ans-loss"));
    REQUIRE(note);
    CHECK(note->text(3).startsWith(QStringLiteral("note: 1 cell is in a colour an .ans can't hold")));
    CHECK(note->font(1).italic());
    CHECK(note->font(3).italic());
    CHECK(dock->summaryLabel()->text().toStdString().starts_with("clean, 1 note (1 ans-loss), noise "));

    // a warning beside it is a finding, in the usual face
    REQUIRE(doc->edit(merge(*doc, 3, 0)));
    dock->refresh();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QTreeWidgetItem* row = rowAt(dock->list(), QStringLiteral("3,0"), QStringLiteral("merge"));
    REQUIRE(row);
    CHECK_FALSE(row->text(3).startsWith(QStringLiteral("note: ")));
    CHECK_FALSE(row->font(3).italic());
    REQUIRE(rowAt(dock->list(), QStringLiteral("0,0"), QStringLiteral("ans-loss")));
    CHECK(dock->summaryLabel()->text().toStdString().starts_with("1 finding (1 merge), 1 note (1 ans-loss), noise "));
}

TEST_CASE("the Lint dock names a control or bidi override from a file by its code point") {
    // #327 finding 3: a UTF-8 .ans with █, U+009B (a C1 CSI), "2J", U+202E
    // (right-to-left override) and "!"
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("controls.ans"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("\xe2\x96\x88\xc2\x9b" "2J\xe2\x80\xae!\r\n");
    }
    MainWindow w;
    w.show();
    QString error;
    REQUIRE(w.openFile(path, &error));
    LintDock* dock = w.lintWidget();
    dock->refresh();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QTreeWidgetItem* csi = rowAt(dock->list(), QStringLiteral("1,0"), QStringLiteral("no-cp437"));
    // U+202E is zero-width, so it joins the J before it (PLAN §16, Reading)
    QTreeWidgetItem* rlo = rowAt(dock->list(), QStringLiteral("3,0"), QStringLiteral("bidi-control"));
    REQUIRE(csi);
    REQUIRE(rlo);
    CHECK(csi->text(3) == QStringLiteral("U+009B has no CP437 code: an .ans saves it as '?'"));
    CHECK(rlo->text(3) == QStringLiteral("U+004A U+202E J U+202E holds the bidi control U+202E: a UTF-8 .ans or IRC "
                                         "text writes it as it is, and a client that applies bidi (HexChat, mIRC) "
                                         "reorders the rest of the line"));
    for (int i = 0; i < dock->list()->topLevelItemCount(); ++i) {
        const QString text = dock->list()->topLevelItem(i)->text(3);
        CHECK_FALSE(text.contains(QChar(0x9B)));
        CHECK_FALSE(text.contains(QChar(0x202E)));
    }
}

TEST_CASE("the Lint dock names an invisible glyph or a noncharacter by its code point") {
    // review of #332, N1 and N2: a UTF-8 .ans with ❤️ (U+2764 U+FE0F, one
    // cluster cell) and U+FFFE
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("invisible.ans"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("\xe2\x9d\xa4\xef\xb8\x8f\xef\xbf\xbe\r\n");
    }
    MainWindow w;
    w.show();
    QString error;
    REQUIRE(w.openFile(path, &error));
    LintDock* dock = w.lintWidget();
    dock->refresh();
    REQUIRE(waitFor([&] { return !dock->stale(); }));
    QTreeWidgetItem* heart = rowAt(dock->list(), QStringLiteral("0,0"), QStringLiteral("no-cp437"));
    QTreeWidgetItem* fffe = rowAt(dock->list(), QStringLiteral("1,0"), QStringLiteral("no-cp437"));
    REQUIRE(heart);
    REQUIRE(fffe);
    CHECK(heart->text(3) == QStringLiteral("U+2764 U+FE0F ❤ U+FE0F has no CP437 code: an .ans saves it as '?'"));
    CHECK(fffe->text(3) == QStringLiteral("U+FFFE has no CP437 code: an .ans saves it as '?'"));
    for (int i = 0; i < dock->list()->topLevelItemCount(); ++i) {
        const QString text = dock->list()->topLevelItem(i)->text(3);
        CHECK_FALSE(text.contains(QChar(0xFE0F)));
        CHECK_FALSE(text.contains(QChar(0xFFFE)));
    }
}
