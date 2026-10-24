// The IRC row meter beside the canvas, View > IRC and File > Export IRC
// Text... (PLAN §8).

#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "irc_meter.h"
#include "main_window.h"
#include "select_tool.h"
#include "tool.h"

#include "blockwright/irc.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QScrollBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <filesystem>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

// The golden cocktober (120x120) with every other solid cell a half block
// of its colour over its other shade: over 407 bytes on many rows.
AnsiDocument noisyPiece() {
    AnsiDocument d = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "cocktober.ans"));
    Canvas& c = d.canvas;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 1; x < c.width(); x += 2) {
            Cell& k = c.at(x, y);
            if ((k.ch != U' ' && k.ch != U'█') || !k.fg.isIndexed() || !k.bg.isIndexed())
                continue;
            const int colour = k.ch == U' ' ? k.bg.index : k.fg.index;
            k = cell(U'▀', std::uint8_t(colour), std::uint8_t((colour ^ 8) & 15));
        }
    return d;
}

// The document's canvas as Save writes it and the CLI reads it back.
Canvas asSaved(const Document& doc) { return parseAnsi(doc.encode().bytes).canvas; }

// A row of `n` half blocks in changing colours: 40 are 293 bytes as an IRC
// line.
CellBlock halfBlockRow(int n = 40) {
    CellBlock b(n, 1);
    for (int x = 0; x < n; ++x)
        b.at(x, 0) = cell(U'▀', std::uint8_t(x % 15 + 1), std::uint8_t((x * 7) % 15 + 1));
    return b;
}

AnsiDocument blankDoc(int w, int h) {
    AnsiDocument d;
    d.canvas = Canvas(w, h);
    return d;
}

QByteArray readAll(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

std::filesystem::path fpath(const QString& p) { return std::filesystem::path(QFile::encodeName(p).toStdString()); }

// An 80x25 project at `path` whose budget is `bytes`: row 3 is 293 bytes as
// an IRC line and row 4 between 301 and 407.
void writeBudgetProject(const QString& path, std::size_t bytes) {
    AnsiDocument d = blankDoc(80, 25);
    const CellBlock row3 = halfBlockRow(40), row4 = halfBlockRow(50);
    for (int x = 0; x < row3.width; ++x)
        d.canvas.at(x, 3) = row3.at(x, 0);
    for (int x = 0; x < row4.width; ++x)
        d.canvas.at(x, 4) = row4.at(x, 0);
    project::Project p = project::fromAnsi(d);
    p.ircBudget = {std::string(irc::kCustomProfile), bytes};
    project::writeZip(p, fpath(path));
}

// What `blockwright-cli export --irc [--fit]` does with the project at
// `path` (cli/main.cpp, exportIrc): the lines it writes and the budget it
// holds them to, the project's own.
std::vector<std::string> cliExport(const QString& path, bool fit, std::size_t* budget) {
    const project::Project p = project::read(fpath(path));
    *budget = p.ircBudget.bytes;
    irc::EncodeOptions o;
    o.fit = fit ? p.ircBudget.bytes : 0;
    std::vector<std::string> out;
    for (irc::Line& l : irc::exportLines(project::toAnsi(p).canvas, o))
        out.push_back(std::move(l.text));
    return out;
}

// Picks `bytes` in View > IRC > Budget... and accepts it.
void pickBudget(MainWindow& w, int bytes) {
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* dialog = qobject_cast<IrcBudgetDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialog);
        dialog->bytesBox()->setValue(bytes);
        dialog->accept();
        ran = true;
    });
    auto* a = w.findChild<QAction*>(QStringLiteral("ircBudget"));
    REQUIRE(a);
    a->trigger();
    REQUIRE(ran);
}

} // namespace

TEST_CASE("the IRC meter gives every row's size as the encoder writes it") {
    MainWindow w;
    w.document()->replace(noisyPiece(), QString());
    IrcMeter* m = w.ircMeter();
    REQUIRE(m);
    CHECK(m->budget() == project::IrcBudget{});
    const Canvas saved = asSaved(*w.document());

    const auto rows = irc::encodeCanvas(saved);
    int over = 0;
    for (int y = 0; y < saved.height(); ++y) {
        INFO("row " << y);
        CHECK(m->bytes(y) == rows[std::size_t(y)].size());
        CHECK(m->merged(y) == 0);
        const bool isOver = rows[std::size_t(y)].size() > 407;
        CHECK((m->state(y) == IrcMeter::RowState::Over) == isOver);
        CHECK((m->state(y) == IrcMeter::RowState::Fits) == !isOver);
        over += isOver;
    }
    REQUIRE(over >= 10);

    // Fit: the rows that were over are merged to fit, the rest left alone.
    act(w, "ircFit")->trigger();
    CHECK(m->fit());
    CHECK(m->options().fit == 407);
    for (int y = 0; y < saved.height(); ++y) {
        INFO("row " << y);
        const irc::Line l = irc::encodeLine(saved, y, m->options());
        CHECK(m->bytes(y) == l.text.size());
        CHECK(m->merged(y) == l.merged);
        CHECK(m->bytes(y) <= 407);
        const bool wasOver = rows[std::size_t(y)].size() > 407;
        CHECK((m->state(y) == IrcMeter::RowState::Merged) == wasOver);
    }
    CHECK(m->describe(0).startsWith(QStringLiteral("Row 0: ")));
    act(w, "ircFit")->trigger();
    CHECK_FALSE(m->fit());
    for (int y = 0; y < saved.height(); ++y)
        if (rows[std::size_t(y)].size() > 407) {
            CHECK(m->describe(y).contains(QStringLiteral("over by %1").arg(rows[std::size_t(y)].size() - 407)));
            break;
        }
}

TEST_CASE("an edit re-measures the rows it changed") {
    MainWindow w;
    IrcMeter* m = w.ircMeter();
    Document* doc = w.document();
    REQUIRE(m->bytes(3) == 0);
    CellEdit e;
    for (int x = 0; x < doc->canvas().width(); ++x)
        e.changes.push_back({x, 3, doc->canvas().at(x, 3), cell(U'x', std::uint8_t(x % 8), std::uint8_t((x + 3) % 8))});
    REQUIRE(doc->edit(e));
    const auto rows = irc::encodeCanvas(asSaved(*doc));
    CHECK(m->bytes(3) == rows[3].size());
    CHECK(m->bytes(3) > 407);
    CHECK(m->state(3) == IrcMeter::RowState::Over);
    CHECK(m->state(2) == IrcMeter::RowState::Fits);
    CHECK(m->state(4) == IrcMeter::RowState::Fits);
    doc->undoStack()->undo();
    CHECK(m->bytes(3) == 0);
    CHECK(m->state(3) == IrcMeter::RowState::Fits);
    doc->undoStack()->redo();
    CHECK(m->state(3) == IrcMeter::RowState::Over);
    // a new canvas measures afresh
    doc->clear();
    CHECK(m->bytes(3) == 0);
}

TEST_CASE("the IRC meter measures the cells as saved, with iCE off") {
    MainWindow w;
    AnsiDocument d;
    d.canvas = Canvas(4, 1);
    d.ice = false;
    // a bright blue background: with iCE off it is written as blink on dark
    // blue, which IRC shows as dark blue (mIRC 2, not 12)
    d.canvas.at(0, 0) = cell(U'x', 15, 9);
    w.document()->replace(d, QString());
    const auto rows = irc::encodeCanvas(asSaved(*w.document()));
    CHECK(rows[0] == "\x03" "0,2x");
    CHECK(w.ircMeter()->bytes(0) == rows[0].size());
}

TEST_CASE("the IRC meter's rows line up with the canvas rows") {
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.document()->replace(noisyPiece(), QString());
    IrcMeter* m = w.ircMeter();
    CanvasView* view = w.canvasView();
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    REQUIRE(m->isVisible());
    // down the right of the viewport, as tall as it
    const QRect port = scroll->viewport()->geometry();
    CHECK(m->geometry().left() == port.right() + 1);
    CHECK(m->geometry().top() == port.top());
    CHECK(m->geometry().height() == port.height());
    CHECK(m->width() == m->sizeHint().width());

    // The view row under the middle of each meter row is that row, however
    // the canvas is zoomed and scrolled.
    const auto check = [&] {
        const QPoint viewX = view->mapToGlobal(QPoint(view->width() / 2, 0));
        int seen = 0;
        for (int y = 0; y < w.document()->canvas().height(); ++y) {
            const QRect r = m->rowRect(y);
            if (r.bottom() < 0 || r.top() >= m->height())
                continue;
            INFO("row " << y);
            const QPoint g = m->mapToGlobal(r.center());
            const QPoint in = view->mapFromGlobal(QPoint(viewX.x(), g.y()));
            CHECK(view->cellAt(view->toView(QPointF(in))).y() == y);
            CHECK(r.height() == view->toWidget(view->cellRect(QPoint(0, y))).height());
            ++seen;
        }
        CHECK(seen > 0);
    };
    check();
    QScrollBar* v = scroll->verticalScrollBar();
    REQUIRE(v->maximum() > 0);
    v->setValue(v->maximum() / 2);
    check();
    view->setZoom(2);
    QApplication::processEvents();
    v->setValue(v->maximum() / 3);
    check();

    // Hidden, it gives its room back to the canvas.
    act(w, "ircMeterShown")->trigger();
    CHECK_FALSE(m->isVisible());
    CHECK(scroll->viewport()->geometry().width() == port.width() + m->sizeHint().width());
    act(w, "ircMeterShown")->trigger();
    CHECK(m->isVisible());
    CHECK(scroll->viewport()->geometry().width() == port.width());
}

TEST_CASE("Export IRC Text writes the lines the meter measures") {
    QTemporaryDir dir;
    MainWindow w;
    w.document()->replace(noisyPiece(), QString());
    IrcMeter* m = w.ircMeter();
    const Canvas saved = asSaved(*w.document());
    const QString path = dir.filePath(QStringLiteral("piece.txt"));
    QString error;

    CHECK_FALSE(w.exportIrcText(path, &error));
    CHECK(error.contains(QStringLiteral("over the 407-byte budget")));
    CHECK_FALSE(QFile::exists(path));

    const auto texts = [](const std::vector<irc::Line>& lines) {
        std::vector<std::string> out;
        for (const auto& l : lines)
            out.push_back(l.text);
        return out;
    };
    REQUIRE(w.exportIrcText(path, &error, true));
    CHECK(readAll(path).toStdString() == irc::joinLines(texts(irc::exportLines(saved))));

    act(w, "ircFit")->trigger();
    act(w, "ircPad")->trigger();
    REQUIRE(w.exportIrcText(path, &error));
    const auto lines = irc::exportLines(saved, m->options());
    CHECK(readAll(path).toStdString() == irc::joinLines(texts(lines)));
    CHECK(irc::overBudget(texts(lines), 407).empty());
    // padded, no row is dropped: line y is row y, as the meter shows it
    REQUIRE(int(lines.size()) == saved.height());
    for (int y = 0; y < saved.height(); ++y) {
        INFO("row " << y);
        CHECK(m->bytes(y) == lines[std::size_t(y)].text.size());
    }
}

TEST_CASE("pad and swap shades change what the meter measures") {
    MainWindow w;
    IrcMeter* m = w.ircMeter();
    REQUIRE(m->bytes(0) == 0);
    act(w, "ircPad")->trigger();
    CHECK(m->pad());
    CHECK(m->options().pad);
    // a blank row padded: black spaces ended on a black block
    CHECK(m->bytes(0) == irc::encodeCanvas(asSaved(*w.document()), m->options())[0].size());
    CHECK(m->bytes(0) > 80);

    AnsiDocument d;
    d.canvas = Canvas(40, 1);
    for (int x = 0; x < 40; ++x)
        d.canvas.at(x, 0) = x % 2 ? cell(U'░', 1, 4) : cell(U'▓', 4, 1);
    w.document()->replace(d, QString());
    const std::size_t plain = m->bytes(0);
    act(w, "ircSwapShades")->trigger();
    CHECK(m->swapShades());
    CHECK(m->bytes(0) < plain);
    CHECK(m->bytes(0) == irc::encodeCanvas(asSaved(*w.document()), m->options())[0].size());
}

TEST_CASE("the IRC budget dialog takes a preset, a number or a mask and channel") {
    IrcBudgetDialog d(project::IrcBudget{});
    CHECK(d.budget() == project::IrcBudget{});
    CHECK(d.bytesBox()->value() == 407);

    d.maskEdit()->setText(QStringLiteral("fp!~fp@example.org"));
    emit d.maskEdit()->textEdited(d.maskEdit()->text());
    CHECK(d.budget() == project::IrcBudget{});  // no channel yet
    d.targetEdit()->setText(QStringLiteral("#art"));
    emit d.targetEdit()->textEdited(d.targetEdit()->text());
    CHECK(d.budget() == project::IrcBudget{"custom", 476});

    d.presetBox()->setCurrentIndex(0);
    emit d.presetBox()->activated(0);
    CHECK(d.budget() == project::IrcBudget{});

    d.bytesBox()->setValue(300);
    CHECK(d.budget() == project::IrcBudget{"custom", 300});

    // a custom budget opens as custom
    IrcBudgetDialog again(project::IrcBudget{"custom", 300});
    CHECK(again.budget() == project::IrcBudget{"custom", 300});
}

TEST_CASE("View > IRC > Budget... sets the budget the meter measures against") {
    MainWindow w;
    Document* doc = w.document();
    CellEdit e;
    for (int x = 0; x < 30; ++x)
        e.changes.push_back({x, 0, doc->canvas().at(x, 0), cell(U'x', std::uint8_t(x % 8), std::uint8_t((x + 3) % 8))});
    REQUIRE(doc->edit(e));
    IrcMeter* m = w.ircMeter();
    REQUIRE(m->bytes(0) > 100);
    REQUIRE(m->state(0) == IrcMeter::RowState::Fits);
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* dialog = qobject_cast<IrcBudgetDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialog);
        dialog->bytesBox()->setValue(100);
        dialog->accept();
        ran = true;
    });
    act(w, "ircBudget")->trigger();
    CHECK(ran);
    CHECK(m->budget() == project::IrcBudget{"custom", 100});
    CHECK(m->state(0) == IrcMeter::RowState::Over);
}

TEST_CASE("the IRC meter measures a pasted floating selection where it floats, as Export writes it") {
    QTemporaryDir dir;
    MainWindow w;
    w.document()->replace(blankDoc(80, 25), QString());
    IrcMeter* m = w.ircMeter();
    CHECK(m->bytes(3) == 0);
    act(w, "selectTool")->trigger();
    w.selectTool()->floatBlock(halfBlockRow(), QPoint(0, 3));
    REQUIRE(w.selectTool()->floating());
    const std::size_t floating = m->bytes(3);
    CHECK(floating == 293);
    CHECK(m->state(3) == IrcMeter::RowState::Fits);

    // Esc drops it: the row is blank again
    w.selectTool()->cancel();
    REQUIRE_FALSE(w.selectTool()->floating());
    CHECK(m->bytes(3) == 0);

    w.selectTool()->floatBlock(halfBlockRow(), QPoint(0, 3));
    CHECK(m->bytes(3) == floating);
    const QString path = dir.filePath(QStringLiteral("f.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error, true));
    CHECK(std::size_t(readAll(path).size()) == floating + 1);
    CHECK(m->bytes(3) == floating);
}

TEST_CASE("the IRC meter follows a lifted selection as it moves") {
    QTemporaryDir dir;
    MainWindow w;
    AnsiDocument d = blankDoc(80, 25);
    const CellBlock b = halfBlockRow();
    for (int x = 0; x < b.width; ++x)
        d.canvas.at(x, 2) = b.at(x, 0);
    w.document()->replace(d, QString());
    IrcMeter* m = w.ircMeter();
    REQUIRE(m->bytes(2) == 293);
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 2, 40, 1});
    w.selectTool()->moveBy(0, 4);
    REQUIRE(w.selectTool()->floating());
    CHECK(m->bytes(2) == 0);
    CHECK(m->bytes(6) == 293);
    w.selectTool()->moveBy(0, 1);
    CHECK(m->bytes(6) == 0);
    CHECK(m->bytes(7) == 293);
    // moved back where it came from, it measures as it was
    w.selectTool()->moveBy(0, -5);
    CHECK(m->bytes(2) == 293);
    CHECK(m->bytes(7) == 0);

    // fit measures the floating cells too, against the same budget Export uses
    w.selectTool()->moveBy(0, 4);
    w.document()->setIrcBudget({std::string(irc::kCustomProfile), 200});
    act(w, "ircFit")->trigger();
    const std::size_t fitted = m->bytes(6);
    CHECK(fitted <= 200);
    CHECK(m->merged(6) > 0);
    CHECK(m->state(2) == IrcMeter::RowState::Fits);
    const QString path = dir.filePath(QStringLiteral("f.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error));
    CHECK(std::size_t(readAll(path).size()) == fitted + 1);
}

TEST_CASE("the IRC meter measures a drag in progress, which Export lands") {
    QTemporaryDir dir;
    MainWindow w;
    w.document()->replace(blankDoc(80, 25), QString());
    IrcMeter* m = w.ircMeter();
    w.tools()->setActive(QStringLiteral("line"));
    Tool* t = w.tools()->active();
    REQUIRE(t);
    const auto event = [](int x, int py, Qt::MouseButton b, Qt::MouseButtons held) {
        return ToolEvent{QPoint(x, py >> 1), py, b, held, Qt::NoModifier, 1.0};
    };
    REQUIRE(t->mousePress(event(2, 8, Qt::LeftButton, Qt::LeftButton)));
    t->mouseMove(event(30, 8, Qt::NoButton, Qt::LeftButton));
    REQUIRE(t->dragging());
    const std::size_t drawn = m->bytes(4);
    CHECK(drawn > 0);
    t->mouseMove(event(30, 10, Qt::NoButton, Qt::LeftButton));
    CHECK(m->bytes(5) > 0);
    t->mouseMove(event(30, 8, Qt::NoButton, Qt::LeftButton));
    CHECK(m->bytes(5) == 0);
    CHECK(m->bytes(4) == drawn);
    const QString path = dir.filePath(QStringLiteral("f.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error));
    CHECK_FALSE(t->dragging());
    CHECK(std::size_t(readAll(path).size()) == drawn + 1);
}

TEST_CASE("the IRC meter measures a float under a layer above it as the canvas shows it, as Export writes it") {
    QTemporaryDir dir;
    MainWindow w;
    Document* doc = w.document();
    doc->replace(blankDoc(80, 25), QString());
    act(w, "ircPad")->trigger();
    IrcMeter* m = w.ircMeter();
    const LayerId bottom = doc->activeLayer();
    doc->addLayer(LayerKind::Cells);
    // cells 0-9 of row 3 on the upper layer: solid red
    CellEdit e;
    for (int x = 0; x < 10; ++x)
        e.changes.push_back({x, 3, doc->layerCanvas().at(x, 3), cell(U'█', 4, 0)});
    REQUIRE(doc->edit(e));
    REQUIRE(doc->setActiveLayer(bottom));
    act(w, "selectTool")->trigger();
    w.selectTool()->floatBlock(halfBlockRow(), QPoint(0, 3));
    REQUIRE(w.selectTool()->floating());
    // the view shows the upper layer's red over the float's first 10 cells
    const std::size_t floating = m->bytes(3);
    const QString path = dir.filePath(QStringLiteral("f.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error, true));
    REQUIRE_FALSE(w.selectTool()->floating());
    const QList<QByteArray> out = readAll(path).split('\n');
    REQUIRE(out.size() > 3);
    CHECK(std::size_t(out[3].size()) == floating);
    CHECK(m->bytes(3) == floating);
}

TEST_CASE("the IRC meter measures nothing for a float the active layer would refuse") {
    QTemporaryDir dir;
    MainWindow w;
    Document* doc = w.document();
    doc->replace(blankDoc(80, 25), QString());
    IrcMeter* m = w.ircMeter();
    act(w, "selectTool")->trigger();
    REQUIRE(doc->setLayerVisible(doc->activeLayer(), false));
    w.selectTool()->floatBlock(halfBlockRow(), QPoint(0, 3));
    // a hidden layer takes no edit: the float shows nothing and lands nothing
    CHECK(m->bytes(3) == 0);
    REQUIRE(doc->setLayerVisible(doc->activeLayer(), true));
    w.selectTool()->floatBlock(halfBlockRow(), QPoint(0, 3));
    CHECK(m->bytes(3) == 293);
}

TEST_CASE("the IRC meter follows the alpha lock under a float, as Export lands it") {
    QTemporaryDir dir;
    MainWindow w;
    Document* doc = w.document();
    doc->replace(blankDoc(80, 25), QString());
    IrcMeter* m = w.ircMeter();
    doc->addLayer(LayerKind::Cells);
    act(w, "selectTool")->trigger();
    w.selectTool()->floatBlock(halfBlockRow(), QPoint(0, 5));
    REQUIRE(w.selectTool()->floating());
    CHECK(m->bytes(5) == 293);
    // locked, the new layer's transparent cells take none of the float
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    REQUIRE(w.selectTool()->floating());
    const std::size_t locked = m->bytes(5);
    const QString path = dir.filePath(QStringLiteral("f.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error, true));
    CHECK(std::size_t(readAll(path).size()) == (locked ? locked + 1 : 0));
    CHECK(locked == 0);
}

TEST_CASE("an opened project's IRC budget is the one the meter, fit and Export hold lines to, as the CLI does") {
    QTemporaryDir dir;
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    writeBudgetProject(bwp, 300);
    std::size_t cliBudget = 0;
    const std::vector<std::string> plain = cliExport(bwp, false, &cliBudget);
    REQUIRE(cliBudget == 300);
    const auto cliOver = irc::overBudget(plain, cliBudget);
    REQUIRE(cliOver.size() == 1);
    REQUIRE(cliOver[0].bytes > 300);
    REQUIRE(cliOver[0].bytes <= 407);

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    IrcMeter* m = w.ircMeter();
    CHECK(m->budget() == w.document()->extras().ircBudget);
    CHECK(m->budget().bytes == 300);
    CHECK(m->bytes(3) == 293);
    CHECK(m->state(3) == IrcMeter::RowState::Fits);
    CHECK(m->bytes(4) == cliOver[0].bytes);
    CHECK(m->state(4) == IrcMeter::RowState::Over);

    // Export refuses the line the CLI refuses, and writes what the CLI writes
    const QString txt = dir.filePath(QStringLiteral("budget.txt"));
    CHECK_FALSE(w.exportIrcText(txt, &error));
    CHECK(error.contains(QStringLiteral("over the 300-byte budget")));
    CHECK_FALSE(QFile::exists(txt));
    REQUIRE(w.exportIrcText(txt, &error, true));
    CHECK(readAll(txt).toStdString() == irc::joinLines(plain));

    // fit fits to the project's budget: no line over 300, as --fit writes
    act(w, "ircFit")->trigger();
    const std::vector<std::string> fitted = cliExport(bwp, true, &cliBudget);
    REQUIRE(irc::overBudget(fitted, 300).empty());
    CHECK(m->state(4) == IrcMeter::RowState::Merged);
    CHECK(m->bytes(4) <= 300);
    REQUIRE(w.exportIrcText(txt, &error));
    CHECK(readAll(txt).toStdString() == irc::joinLines(fitted));

    // the dialog starts from the project's budget
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* dialog = qobject_cast<IrcBudgetDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialog);
        CHECK(dialog->bytesBox()->value() == 300);
        dialog->reject();
        ran = true;
    });
    act(w, "ircBudget")->trigger();
    CHECK(ran);
    CHECK(m->budget().bytes == 300);
    CHECK_FALSE(w.document()->isModified());
}

TEST_CASE("a budget picked in View > IRC > Budget... is an undo step saved with the project") {
    QTemporaryDir dir;
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    writeBudgetProject(bwp, 300);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    Document* doc = w.document();
    IrcMeter* m = w.ircMeter();
    REQUIRE(m->state(4) == IrcMeter::RowState::Over);

    pickBudget(w, 400);
    CHECK(doc->extras().ircBudget == project::IrcBudget{std::string(irc::kCustomProfile), 400});
    CHECK(m->budget() == doc->extras().ircBudget);
    CHECK(m->state(4) == IrcMeter::RowState::Fits);
    CHECK(doc->isModified());
    CHECK(w.isWindowModified());
    CHECK(doc->undoStack()->undoText() == QStringLiteral("IRC Budget"));

    // the meter follows undo and redo
    doc->undoStack()->undo();
    CHECK(m->budget().bytes == 300);
    CHECK(m->state(4) == IrcMeter::RowState::Over);
    CHECK_FALSE(doc->isModified());
    doc->undoStack()->redo();
    CHECK(m->budget().bytes == 400);
    CHECK(m->state(4) == IrcMeter::RowState::Fits);

    // the same budget again changes nothing
    pickBudget(w, 400);
    CHECK(doc->undoStack()->count() == 1);

    REQUIRE(w.save());
    CHECK_FALSE(doc->isModified());
    CHECK(project::read(fpath(bwp)).ircBudget == project::IrcBudget{std::string(irc::kCustomProfile), 400});
    std::size_t cliBudget = 0;
    const std::vector<std::string> lines = cliExport(bwp, false, &cliBudget);
    CHECK(cliBudget == 400);
    CHECK(irc::overBudget(lines, cliBudget).empty());

    MainWindow again;
    REQUIRE(again.openFile(bwp, &error));
    CHECK(again.ircMeter()->budget().bytes == 400);
    CHECK(again.ircMeter()->state(4) == IrcMeter::RowState::Fits);
}

TEST_CASE("a flat file or a new canvas after a project measures against the default budget") {
    QTemporaryDir dir;
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    writeBudgetProject(bwp, 300);
    MainWindow w;
    QString error;
    IrcMeter* m = w.ircMeter();
    REQUIRE(w.openFile(bwp, &error));
    REQUIRE(m->budget().bytes == 300);

    REQUIRE(w.openFile(QString::fromStdString((bwtest::dataDir() / "golden" / "tux.ans").string()), &error));
    CHECK(m->budget() == project::IrcBudget{});
    CHECK(w.document()->extras().ircBudget == project::IrcBudget{});

    REQUIRE(w.openFile(bwp, &error));
    REQUIRE(m->budget().bytes == 300);
    REQUIRE(m->state(4) == IrcMeter::RowState::Over);
    w.newFile();
    CHECK(m->budget() == project::IrcBudget{});
    CHECK(m->bytes(4) == 0);

    // a budget picked on a flat file is the document's too: an .ans can't
    // hold it, so only a project save keeps it
    REQUIRE(w.openFile(QString::fromStdString((bwtest::dataDir() / "golden" / "tux.ans").string()), &error));
    pickBudget(w, 200);
    CHECK(w.document()->extras().ircBudget.bytes == 200);
    CHECK(w.document()->isModified());
    CHECK_FALSE(w.document()->flatHoldsAll());
}
