// The Lint and IRC View docks (lint_dock.h): they follow the document a
// moment after each change while shown, agree with lintCanvas and ircView,
// and a finding's row takes the cell cursor to its cell.

#include "util.h"

#include "canvas_view.h"
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

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QSemaphore>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>

#include <atomic>
#include <memory>

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
