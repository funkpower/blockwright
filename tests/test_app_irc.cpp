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
#include "blockwright/irc_view.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHelpEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolTip>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

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
// line, 336 as the ascii script sends it at a 300-byte budget.
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

// An 80x25 project at `path` whose budget is `bytes`. As the ascii script
// sends them, row 3 is 285 bytes (249 written) and row 4 369 at a 300-byte
// budget and 378 at 400 (322 written): over 300, within 400.
void writeBudgetProject(const QString& path, std::size_t bytes) {
    AnsiDocument d = blankDoc(80, 25);
    const CellBlock row3 = halfBlockRow(34), row4 = halfBlockRow(44);
    for (int x = 0; x < row3.width; ++x)
        d.canvas.at(x, 3) = row3.at(x, 0);
    for (int x = 0; x < row4.width; ++x)
        d.canvas.at(x, 4) = row4.at(x, 0);
    project::Project p = project::fromAnsi(d);
    p.ircBudget = {std::string(irc::kCustomProfile), bytes};
    project::writeZip(p, fpath(path));
}

struct CliRun {
    int status = -1;
    QString out;  // what it printed
    QString err;  // what it warned
    std::optional<std::string> written;  // the file, if it wrote one
};

// Runs `blockwright-cli export --irc [--fit] [--allow-over-budget]` on the
// project at `path`, writing beside it.
CliRun runCliExport(const QString& path, bool fit, bool allowOver) {
    const QString txt = path + QStringLiteral(".cli.txt");
    QFile::remove(txt);
    QStringList args{QStringLiteral("export"), QStringLiteral("--irc")};
    if (fit)
        args << QStringLiteral("--fit");
    if (allowOver)
        args << QStringLiteral("--allow-over-budget");
    args << path << txt;
    QProcess p;
    p.start(QStringLiteral(BW_CLI), args);
    REQUIRE(p.waitForFinished(60000));
    REQUIRE(p.exitStatus() == QProcess::NormalExit);
    CliRun r;
    r.status = p.exitCode();
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    if (QFile::exists(txt))
        r.written = readAll(txt).toStdString();
    return r;
}

// The lines `blockwright-cli export --irc [--fit]` writes for the project
// at `path`, over the budget or not, and the budget it holds them to, which
// it prints: the project's own.
std::vector<std::string> cliExport(const QString& path, bool fit, std::size_t* budget) {
    const CliRun r = runCliExport(path, fit, true);
    INFO(r.out.toStdString());
    REQUIRE(r.status == 0);
    REQUIRE(r.written);
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(" sent, budget (\\d+): ")).match(r.out);
    REQUIRE(m.hasMatch());
    *budget = m.captured(1).toULongLong();
    std::vector<std::string> out;
    std::size_t from = 0;
    for (std::size_t nl; (nl = r.written->find('\n', from)) != std::string::npos; from = nl + 1)
        out.push_back(r.written->substr(from, nl - from));
    REQUIRE(from == r.written->size());
    return out;
}

// Answers the next message box with `button`; `asked` says one came and
// `text`, when given, takes what it said.
void answer(QWidget* context, QMessageBox::StandardButton button, bool* asked, QString* text = nullptr) {
    *asked = false;
    QTimer::singleShot(0, context, [=] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box)
            return;
        *asked = true;
        if (text)
            *text = box->text();
        box->button(button)->click();
    });
}

// A copy of the golden tux.ans at `path`.
QString tuxAt(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    const std::string bytes = bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans");
    REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
    return path;
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

// Picks the default preset (EFnet / WeeChat, 407) in View > IRC >
// Budget... and accepts it.
void pickDefaultBudget(MainWindow& w) {
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* dialog = qobject_cast<IrcBudgetDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialog);
        dialog->presetBox()->setCurrentIndex(0);
        emit dialog->presetBox()->activated(0);
        dialog->accept();
        ran = true;
    });
    act(w, "ircBudget")->trigger();
    REQUIRE(ran);
}


// An 80x50 piece with no blank row: even rows half blocks in changing
// colours, over the default budget; odd rows plain letters, within it.
AnsiDocument stripedPiece() {
    AnsiDocument d = blankDoc(80, 50);
    const CellBlock over = halfBlockRow(80);
    for (int y = 0; y < d.canvas.height(); ++y)
        for (int x = 0; x < d.canvas.width(); ++x)
            d.canvas.at(x, y) = y % 2 == 0 ? over.at(x, 0) : cell(U'x', 7, 0);
    return d;
}

// The canvas row under meter pixel row `y` as the canvas draws it (PLAN §4):
// view pixel row v is device rows start + scale * v on, start the first whole
// device pixel in the view and scale the ratio rounded down (at least 1), so
// the row is the one whose device rows hold the pixel's top edge.
int canvasRowUnder(const QWidget& w, const IrcMeter* m, const CanvasView* view, int y) {
    const double dpr = view->devicePixelRatioF();
    const int scale = std::max(1, int(std::floor(dpr + 1e-9)));
    const int start = int(std::ceil(view->mapTo(&w, QPoint(0, 0)).y() * dpr - 1e-9));
    const double edge = m->mapTo(&w, QPoint(0, y)).y() * dpr;
    const int rowDevice = view->cellRect(QPoint(0, 0)).height() * scale;
    return int(std::floor((edge - start) / rowDevice));
}

// The view row under meter pixel row `y` by the canvas's own pointer mapping.
int viewRowUnder(const QWidget& w, const IrcMeter* m, const CanvasView* view, int y) {
    const QPoint in = view->mapFrom(&w, m->mapTo(&w, QPoint(0, y)));
    return view->cellAt(view->toView(QPointF(in))).y();
}

// The tooltip the meter shows with the pointer at `pos`.
std::string tooltipAt(IrcMeter* m, QPoint pos) {
    QToolTip::hideText();
    QHelpEvent he(QEvent::ToolTip, pos, m->mapToGlobal(pos));
    QApplication::sendEvent(m, &he);
    return QToolTip::isVisible() ? QToolTip::text().toStdString() : std::string();
}

// A main window 1600x900 showing stripedPiece at `zoom`, with the meter.
struct StripedWindow {
    MainWindow w;
    IrcMeter* m = nullptr;
    CanvasView* view = nullptr;
    QScrollBar* v = nullptr;

    explicit StripedWindow(int zoom) {
        w.resize(1600, 900);
        w.show();
        w.document()->replace(stripedPiece(), QString());
        m = w.ircMeter();
        view = w.canvasView();
        auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
        REQUIRE(scroll);
        REQUIRE(m->isVisible());
        view->setZoom(zoom);
        QApplication::processEvents();
        v = scroll->verticalScrollBar();
        REQUIRE(v->maximum() > 0);
        const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR");
        if (!scale.isEmpty())
            REQUIRE(view->devicePixelRatioF() == scale.toDouble());
    }
    void scrollTo(int value) {
        v->setValue(value);
        QApplication::processEvents();
        REQUIRE(v->value() == value);
    }
    int canvasRow(int y) const { return canvasRowUnder(w, m, view, y); }
};
} // namespace

TEST_CASE("the IRC meter gives every row's size as the encoder writes it and as the script sends it") {
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
        CHECK(m->sentBytes(y) == irc::txtSentBytes(rows[std::size_t(y)], 407));
        CHECK(m->merged(y) == 0);
        const bool isOver = irc::txtSentBytes(rows[std::size_t(y)], 407) > 407;
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
        CHECK(m->sentBytes(y) <= 407);
        const bool wasOver = irc::txtSentBytes(rows[std::size_t(y)], 407) > 407;
        CHECK((m->state(y) == IrcMeter::RowState::Merged) == wasOver);
    }
    CHECK(m->describe(0).startsWith(QStringLiteral("Row 0: ")));
    act(w, "ircFit")->trigger();
    CHECK_FALSE(m->fit());
    for (int y = 0; y < saved.height(); ++y)
        if (const std::size_t sent = irc::txtSentBytes(rows[std::size_t(y)], 407); sent > 407) {
            CHECK(m->describe(y).contains(QStringLiteral("over by %1").arg(sent - 407)));
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

    // #393: it says Publish and Send never go past the library's 407
    auto* note = d.findChild<QLabel*>(QStringLiteral("ircPublishNote"));
    REQUIRE(note);
    CHECK(note->text() == QStringLiteral("Publish to Library and Send to WeeChat hold lines to this budget or to "
                                         "407 bytes, the ascii script's EFnet limit, whichever is less."));

    // the range the CLI's --budget and a project take (#303)
    CHECK(d.bytesBox()->minimum() == int(irc::kMinBudget));
    CHECK(d.bytesBox()->maximum() == int(irc::kMaxBudget));
    CHECK(d.bytesBox()->maximum() == 512);
}

TEST_CASE("Export IRC Text says line for one line over the budget and lines for more (#304)") {
    QTemporaryDir dir;
    MainWindow w;
    AnsiDocument d = blankDoc(80, 25);
    const CellBlock row = halfBlockRow(40);  // 293 bytes
    for (int x = 0; x < row.width; ++x)
        d.canvas.at(x, 3) = row.at(x, 0);
    w.document()->replace(d, QString());
    w.document()->setIrcBudget({std::string(irc::kCustomProfile), 200});
    const QString path = dir.filePath(QStringLiteral("p.txt"));
    QString error;
    CHECK_FALSE(w.exportIrcText(path, &error));
    CHECK(error == QStringLiteral("1 line over the 200-byte budget as WeeChat's ascii script sends it"));

    for (int x = 0; x < row.width; ++x)
        d.canvas.at(x, 5) = row.at(x, 0);
    w.document()->replace(d, QString());
    w.document()->setIrcBudget({std::string(irc::kCustomProfile), 200});
    CHECK_FALSE(w.exportIrcText(path, &error));
    CHECK(error == QStringLiteral("2 lines over the 200-byte budget as WeeChat's ascii script sends them"));
    CHECK_FALSE(QFile::exists(path));
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
    const auto cliOver = irc::txtOverBudget(plain, cliBudget);
    REQUIRE(cliOver.size() == 1);
    REQUIRE(cliOver[0].line == 2);
    REQUIRE(cliOver[0].bytes == 369);

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    IrcMeter* m = w.ircMeter();
    CHECK(m->budget() == w.document()->extras().ircBudget);
    CHECK(m->budget().bytes == 300);
    CHECK(m->bytes(3) == 249);
    CHECK(m->sentBytes(3) == 285);
    CHECK(m->state(3) == IrcMeter::RowState::Fits);
    CHECK(m->bytes(4) == 322);
    CHECK(m->sentBytes(4) == cliOver[0].bytes);
    CHECK(m->state(4) == IrcMeter::RowState::Over);

    // Export refuses the line the CLI refuses, and writes what the CLI writes
    const QString txt = dir.filePath(QStringLiteral("budget.txt"));
    CHECK_FALSE(w.exportIrcText(txt, &error));
    CHECK(error.contains(QStringLiteral("over the 300-byte budget")));
    CHECK_FALSE(QFile::exists(txt));
    const CliRun refused = runCliExport(bwp, false, false);
    CHECK(refused.status == 3);
    CHECK_FALSE(refused.written);
    REQUIRE(w.exportIrcText(txt, &error, true));
    CHECK(readAll(txt).toStdString() == irc::joinLines(plain));

    // fit fits to the project's budget: no line over 300, as --fit writes
    act(w, "ircFit")->trigger();
    const std::vector<std::string> fitted = cliExport(bwp, true, &cliBudget);
    REQUIRE(irc::txtOverBudget(fitted, 300).empty());
    CHECK(m->state(4) == IrcMeter::RowState::Merged);
    CHECK(m->sentBytes(4) <= 300);
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
    CHECK(irc::txtOverBudget(lines, cliBudget).empty());

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

TEST_CASE("Save As takes a project's IRC budget to the new file, and an .ans export leaves the project its file") {
    QTemporaryDir dir;
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    writeBudgetProject(bwp, 300);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    Document* doc = w.document();
    IrcMeter* m = w.ircMeter();

    // an .ans can't hold the budget: it is an export, and the project stays
    // the document's file, unmodified, with its budget
    const QString flat = dir.filePath(QStringLiteral("budget.ans"));
    REQUIRE(w.saveFile(flat, &error));
    CHECK(doc->path() == bwp);
    CHECK(doc->form() == FileForm::ProjectZip);
    CHECK_FALSE(doc->isModified());
    CHECK(m->budget().bytes == 300);
    CHECK(m->state(4) == IrcMeter::RowState::Over);
    {
        MainWindow other;
        REQUIRE(other.openFile(flat, &error));
        CHECK(other.ircMeter()->budget() == project::IrcBudget{});
    }

    // Save As a project directory writes it there and makes that the file
    const QString pdir = dir.filePath(QStringLiteral("budget-dir"));
    REQUIRE(QDir().mkpath(pdir));
    REQUIRE(w.saveFile(pdir, &error));
    CHECK(doc->path() == pdir);
    CHECK(doc->form() == FileForm::ProjectDirectory);
    CHECK_FALSE(doc->isModified());
    CHECK(project::read(fpath(pdir)).ircBudget == project::IrcBudget{std::string(irc::kCustomProfile), 300});
    std::size_t cliBudget = 0;
    CHECK(cliExport(pdir, false, &cliBudget).size() == 2);
    CHECK(cliBudget == 300);

    // a budget picked after that goes to the next Save As, not the old file
    pickBudget(w, 400);
    const QString again = dir.filePath(QStringLiteral("again.bwp"));
    REQUIRE(w.saveFile(again, &error));
    CHECK(doc->path() == again);
    CHECK(doc->form() == FileForm::ProjectZip);
    CHECK_FALSE(doc->isModified());
    CHECK(project::read(fpath(again)).ircBudget == project::IrcBudget{std::string(irc::kCustomProfile), 400});
    CHECK(project::read(fpath(pdir)).ircBudget.bytes == 300);
    cliExport(again, false, &cliBudget);
    CHECK(cliBudget == 400);
    CHECK(m->budget().bytes == 400);
    CHECK(m->state(4) == IrcMeter::RowState::Fits);
}

TEST_CASE("a flat file's IRC budget keeps it unsaved until Save As a project, and the default lets an .ans hold it again") {
    QTemporaryDir dir;
    const QString ans = tuxAt(dir.filePath(QStringLiteral("piece.ans")));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(ans, &error));
    Document* doc = w.document();
    IrcMeter* m = w.ircMeter();
    pickBudget(w, 300);

    // Save writes the .ans, which has no room for the budget
    REQUIRE(w.save());
    CHECK(readAll(ans).toStdString() == writeAnsi(project::toAnsi(doc->project())).bytes);
    CHECK(doc->path() == ans);
    CHECK(doc->form() == FileForm::Ansi);
    CHECK(doc->isModified());
    CHECK(w.isWindowModified());
    CHECK(m->budget().bytes == 300);

    // Save As .bwp keeps it, and the project is the document's file now
    const QString bwp = dir.filePath(QStringLiteral("piece.bwp"));
    REQUIRE(w.saveFile(bwp, &error));
    CHECK(doc->path() == bwp);
    CHECK(doc->form() == FileForm::ProjectZip);
    CHECK_FALSE(doc->isModified());
    CHECK(project::read(fpath(bwp)).ircBudget == project::IrcBudget{std::string(irc::kCustomProfile), 300});
    CHECK(m->budget().bytes == 300);

    // the default picked again: the project holds nothing an .ans lacks,
    // so Save As .ans makes the .ans the document's file, saved
    pickDefaultBudget(w);
    CHECK(doc->extras().ircBudget == project::IrcBudget{});
    CHECK(doc->isModified());
    CHECK(doc->flatHoldsAll());
    const QString flat = dir.filePath(QStringLiteral("flat.ans"));
    REQUIRE(w.saveFile(flat, &error));
    CHECK(doc->path() == flat);
    CHECK(doc->form() == FileForm::Ansi);
    CHECK_FALSE(doc->isModified());
    CHECK(readAll(flat).toStdString() == writeAnsi(project::toAnsi(doc->project())).bytes);
    CHECK(m->budget() == project::IrcBudget{});
}

TEST_CASE("the unsaved-changes prompt keeps a flat file's IRC budget open when Save writes only the .ans") {
    QTemporaryDir dir;
    const QString ans = tuxAt(dir.filePath(QStringLiteral("piece.ans")));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(ans, &error));
    Document* doc = w.document();
    IrcMeter* m = w.ircMeter();
    pickBudget(w, 300);
    bool asked = false;
    QString text;

    answer(&w, QMessageBox::Cancel, &asked, &text);
    w.newFile();
    REQUIRE(asked);
    CHECK(text == QStringLiteral("The canvas has unsaved changes.\nSave writes piece.ans flattened and keeps the "
                                 "document open; only a project saves its layers and IRC budget."));
    CHECK(doc->path() == ans);
    CHECK(m->budget().bytes == 300);

    // a cell edited alongside the budget, which the file doesn't hold yet
    CellEdit e;
    e.changes.push_back({0, 0, doc->canvas().at(0, 0), cell(U'x', 15, 9)});
    REQUIRE(doc->edit(e));
    REQUIRE(parseAnsi(readAll(ans).toStdString()).canvas.at(0, 0).ch != U'x');
    REQUIRE(readAll(ans).toStdString() != writeAnsi(project::toAnsi(doc->project())).bytes);
    const int steps = doc->undoStack()->index();

    // Save writes the .ans and keeps the document, budget and all, unsaved
    answer(&w, QMessageBox::Save, &asked);
    w.newFile();
    REQUIRE(asked);
    CHECK(readAll(ans).toStdString() == writeAnsi(project::toAnsi(doc->project())).bytes);
    CHECK(parseAnsi(readAll(ans).toStdString()).canvas.at(0, 0).ch == U'x');
    CHECK(doc->path() == ans);
    CHECK(doc->form() == FileForm::Ansi);
    CHECK(doc->isModified());
    CHECK(w.isWindowModified());
    CHECK(doc->canvas().at(0, 0).ch == U'x');
    CHECK(doc->extras().ircBudget == project::IrcBudget{std::string(irc::kCustomProfile), 300});
    CHECK(m->budget().bytes == 300);
    CHECK(doc->undoStack()->index() == steps);
    CHECK(doc->undoStack()->text(steps - 2) == QStringLiteral("IRC Budget"));
    // closing too
    answer(&w, QMessageBox::Save, &asked);
    CHECK_FALSE(w.close());
    CHECK(asked);
    CHECK(m->budget().bytes == 300);

    // Discard drops it: a new canvas has the default
    answer(&w, QMessageBox::Discard, &asked);
    w.newFile();
    REQUIRE(asked);
    CHECK(doc->path().isEmpty());
    CHECK_FALSE(doc->isModified());
    CHECK(m->budget() == project::IrcBudget{});

    // a project holds its budget: the plain prompt, and its Save lets New go on
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    writeBudgetProject(bwp, 300);
    REQUIRE(w.openFile(bwp, &error));
    pickBudget(w, 350);
    answer(&w, QMessageBox::Save, &asked, &text);
    w.newFile();
    REQUIRE(asked);
    CHECK(text == QStringLiteral("The canvas has unsaved changes."));
    CHECK(project::read(fpath(bwp)).ircBudget == project::IrcBudget{std::string(irc::kCustomProfile), 350});
    CHECK(doc->path().isEmpty());
    CHECK(m->budget() == project::IrcBudget{});

    // a budget undone on a flat file leaves nothing to ask about
    REQUIRE(w.openFile(ans, &error));
    pickBudget(w, 300);
    doc->undoStack()->undo();
    REQUIRE_FALSE(doc->isModified());
    answer(&w, QMessageBox::Cancel, &asked);
    w.newFile();
    QApplication::processEvents();
    CHECK_FALSE(asked);
    CHECK(doc->path().isEmpty());
}

TEST_CASE("undo and redo after a save move the IRC budget off and back onto what the file holds") {
    QTemporaryDir dir;
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    writeBudgetProject(bwp, 300);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    Document* doc = w.document();
    IrcMeter* m = w.ircMeter();
    pickBudget(w, 400);
    REQUIRE(w.save());
    REQUIRE_FALSE(doc->isModified());

    // undo: the budget the file doesn't hold, so modified; the file is as saved
    doc->undoStack()->undo();
    CHECK(m->budget().bytes == 300);
    CHECK(m->state(4) == IrcMeter::RowState::Over);
    CHECK(doc->isModified());
    CHECK(w.isWindowModified());
    CHECK(project::read(fpath(bwp)).ircBudget.bytes == 400);
    // redo: back to the saved budget, clean
    doc->undoStack()->redo();
    CHECK(m->budget().bytes == 400);
    CHECK(m->state(4) == IrcMeter::RowState::Fits);
    CHECK_FALSE(doc->isModified());
    CHECK_FALSE(w.isWindowModified());

    // saved after the undo, the file holds 300 and redo is the change
    doc->undoStack()->undo();
    REQUIRE(w.save());
    CHECK_FALSE(doc->isModified());
    CHECK(project::read(fpath(bwp)).ircBudget.bytes == 300);
    std::size_t cliBudget = 0;
    cliExport(bwp, false, &cliBudget);
    CHECK(cliBudget == 300);
    doc->undoStack()->redo();
    CHECK(m->budget().bytes == 400);
    CHECK(doc->isModified());
    doc->undoStack()->undo();
    CHECK_FALSE(doc->isModified());

    // A flat save doesn't hold the budget, so no undo step matches the
    // file afterwards: undone to the default it is still unsaved
    const QString ans = tuxAt(dir.filePath(QStringLiteral("piece.ans")));
    REQUIRE(w.openFile(ans, &error));
    pickBudget(w, 300);
    REQUIRE(w.save());
    REQUIRE(doc->isModified());
    doc->undoStack()->undo();
    CHECK(m->budget() == project::IrcBudget{});
    CHECK(doc->flatHoldsAll());
    CHECK(doc->isModified());
    REQUIRE(w.save());
    CHECK_FALSE(doc->isModified());
    doc->undoStack()->redo();
    CHECK(m->budget().bytes == 300);
    CHECK(doc->isModified());
    CHECK_FALSE(doc->flatHoldsAll());
}

TEST_CASE("with Fit on, a budget change and its undo and redo fit the rows to the budget they have") {
    QTemporaryDir dir;
    const QString bwp = dir.filePath(QStringLiteral("budget.bwp"));
    writeBudgetProject(bwp, 300);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(bwp, &error));
    Document* doc = w.document();
    IrcMeter* m = w.ircMeter();
    const Canvas saved = asSaved(*doc);
    const std::size_t plain = irc::encodeCanvas(saved)[4].size();
    REQUIRE(irc::txtSentBytes(irc::encodeCanvas(saved)[4], 300) > 300);
    REQUIRE(irc::txtSentBytes(irc::encodeCanvas(saved)[4], 400) <= 400);
    act(w, "ircFit")->trigger();
    REQUIRE(m->fit());

    // the line fitted to `budget`, as the meter must give it
    const auto fittedTo = [&](std::size_t budget) {
        irc::EncodeOptions o;
        o.fit = budget;
        return irc::encodeLine(saved, 4, o);
    };
    const irc::Line at300 = fittedTo(300);
    REQUIRE(at300.merged > 0);
    REQUIRE(irc::txtSentBytes(at300.text, 300) <= 300);
    const auto fitsAt300 = [&] {
        CHECK(m->options().fit == 300);
        CHECK(m->state(4) == IrcMeter::RowState::Merged);
        CHECK(m->bytes(4) == at300.text.size());
        CHECK(m->merged(4) == at300.merged);
    };
    const auto fitsAt400 = [&] {
        // 400 holds the row whole: nothing merged
        CHECK(m->options().fit == 400);
        CHECK(m->state(4) == IrcMeter::RowState::Fits);
        CHECK(m->bytes(4) == plain);
        CHECK(m->merged(4) == 0);
    };
    fitsAt300();
    pickBudget(w, 400);
    fitsAt400();
    doc->undoStack()->undo();
    fitsAt300();
    doc->undoStack()->redo();
    fitsAt400();
    doc->undoStack()->undo();
    fitsAt300();

    // Export after the undo writes the rows fitted to 300, as --fit does
    std::size_t cliBudget = 0;
    const std::vector<std::string> cli = cliExport(bwp, true, &cliBudget);
    REQUIRE(cliBudget == 300);
    const QString txt = dir.filePath(QStringLiteral("budget.txt"));
    REQUIRE(w.exportIrcText(txt, &error));
    CHECK(readAll(txt).toStdString() == irc::joinLines(cli));
}

// --- pairs and clusters (PLAN §16, slice 4) ---------------------------------

TEST_CASE("IRC text's losses are described as a UTF-8 .ans's are") {
    Canvas c(6, 2);
    c.at(0, 0).ch = U'A';
    c.at(1, 0).ch = Glyph::continuation();
    c.at(2, 0).ch = char32_t(0x1B);
    c.at(3, 0).ch = Glyph::continuation();
    c.at(4, 1).ch = char32_t(0x85);
    const std::vector<Unwritable> lost = irc::unwritable(c);
    REQUIRE(lost.size() == 3);
    CHECK(ircLossDescription(lost) ==
          QStringLiteral("1 cell holds a control character IRC text can't hold and will be written as '?': "
                         "U+0085 at 4,1;\n"
                         "1 cell holds a control character IRC text can't hold before a continuation, and will be "
                         "written as '?' and a space: U+001B at 2,0;\n"
                         "1 cell holds a glyph that a terminal doesn't draw two columns wide before a "
                         "continuation, and will be written as '?' and a space: A at 0,0"));
    CHECK(ircLossDescription({}).isEmpty());
    // a UTF-8 .ans says the same of a control-holding lead
    AnsiDocument d;
    d.canvas = c;
    CHECK(lossDescription(writeAnsi(d, TextEncoding::Utf8))
              .contains(QStringLiteral("1 cell holds a control character a UTF-8 .ans can't hold before a "
                                       "continuation, and will be written as '?' and a space: U+001B at 2,0")));
}

TEST_CASE("Export IRC Text names the cells IRC text can't hold and asks first") {
    MainWindow w;
    AnsiDocument d = blankDoc(80, 25);
    d.canvas.at(5, 2).ch = char32_t(0x85);
    w.document()->replace(d, QString());
    bool asked = false;
    QString text;
    answer(&w, QMessageBox::Cancel, &asked, &text);
    act(w, "exportIrc")->trigger();
    REQUIRE(asked);
    CHECK(text == QStringLiteral("Before exporting IRC text:\n1 cell holds a control character IRC text can't hold "
                                 "and will be written as '?': U+0085 at 5,2.\n\nExport anyway?"));
}

TEST_CASE("the IRC meter counts a pair and clusters as the encoder writes them") {
    MainWindow w;
    AnsiDocument d = blankDoc(20, 6);
    const auto at = [&](int x, int y, Glyph g, std::uint8_t fg, std::uint8_t bg) {
        d.canvas.at(x, y) = cell(U' ', fg, bg);
        d.canvas.at(x, y).ch = g;
    };
    const std::vector<char32_t> two{U'e', 0x301}, three{U'e', 0x301, 0x302};
    const Glyph e2 = *clusterPool().intern(two, PoolCap::Capped);
    const Glyph e3 = *clusterPool().intern(three, PoolCap::Capped);
    // rows 0 and 1 differ only in how long their cluster is
    for (int y = 0; y < 2; ++y) {
        at(2, y, U'中', 4, 1);
        at(3, y, Glyph::continuation(), 4, 1);
        at(6, y, y == 0 ? e2 : e3, 9, 2);
    }
    at(0, 2, U'A', 4, 1);  // a lead '?' and a space
    at(1, 2, Glyph::continuation(), 4, 1);
    at(18, 3, U'中', 4, 1);  // a pair on the last two columns
    at(19, 3, Glyph::continuation(), 4, 1);
    at(5, 4, U'中', 4, 1);  // a lone wide cell
    at(5, 5, char32_t(0x9B), 4, 1);  // a control, sent as '?'
    w.document()->replace(d, QString());
    // a CP437 save would lose the pair, so the canvas as IRC text sends it
    const Canvas sent = irc::sentCanvas(w.document()->canvas(), w.document()->ansi().ice);
    IrcMeter* m = w.ircMeter();
    for (int y = 0; y < 6; ++y) {
        INFO("row " << y);
        CHECK(m->bytes(y) == irc::encodeLine(sent, y, m->options()).text.size());
    }
    CHECK(m->bytes(1) == m->bytes(0) + 2);
}

TEST_CASE("blockwright-cli export --irc warns of the cells IRC text can't hold") {
    QTemporaryDir dir;
    const QString in = dir.filePath(QStringLiteral("c1.ans"));
    {
        QFile f(in);
        REQUIRE(f.open(QIODevice::WriteOnly));
        REQUIRE(f.write("AB\xc2\x85" "CD") == 6);
    }
    const QString txt = dir.filePath(QStringLiteral("c1.txt"));
    QProcess p;
    p.start(QStringLiteral(BW_CLI), {QStringLiteral("export"), QStringLiteral("--irc"), QStringLiteral("--encoding"), QStringLiteral("utf-8"), in, txt});
    REQUIRE(p.waitForFinished(60000));
    const QString err = QString::fromUtf8(p.readAllStandardError());
    INFO(err.toStdString());
    CHECK(p.exitCode() == 0);
    CHECK(err.contains(QStringLiteral("blockwright-cli: warning: ") + in +
                       QStringLiteral(": 1 cell holds a control character IRC text can't hold and is written as "
                                      "'?' (first: U+0085 at column 3, row 1)")));
    CHECK(readAll(txt).contains("AB?CD"));
}

TEST_CASE("80 stars written in the budget are over it as the script sends them, and Export refuses them (#364)") {
    QTemporaryDir dir;
    MainWindow w;
    AnsiDocument d = blankDoc(80, 25);
    // ★ in bright red and red by turns on black: 402 bytes written, 483 sent;
    // 67 of them on the next row: 337 written, 405 sent
    for (int x = 0; x < 80; ++x)
        d.canvas.at(x, 3) = cell(U'★', x % 2 ? Red : LightRed, Black);
    w.document()->replace(d, QString());
    IrcMeter* m = w.ircMeter();
    REQUIRE(m);
    CHECK(m->bytes(3) == 402);
    CHECK(m->sentBytes(3) == 483);
    CHECK(m->state(3) == IrcMeter::RowState::Over);
    CHECK(m->describe(3).contains(QStringLiteral("483 of 407 bytes")));
    CHECK(m->describe(3).contains(QStringLiteral("402 as written")));
    CHECK(m->describe(3).contains(QStringLiteral("over by 76")));

    const QString path = dir.filePath(QStringLiteral("stars.txt"));
    QString error;
    CHECK_FALSE(w.exportIrcText(path, &error));
    CHECK(error == QStringLiteral("1 line over the 407-byte budget as WeeChat's ascii script sends it"));
    CHECK_FALSE(QFile::exists(path));

    for (int x = 0; x < 80; ++x)
        d.canvas.at(x, 3) = x < 67 ? cell(U'★', x % 2 ? Red : LightRed, Black) : Cell{};
    w.document()->replace(d, QString());
    CHECK(m->bytes(3) == 337);
    CHECK(m->sentBytes(3) == 405);
    CHECK(m->state(3) == IrcMeter::RowState::Fits);
    REQUIRE(w.exportIrcText(path, &error));
    CHECK(readAll(path).size() > 337);
}

// #404: the meter maps a pixel to a row as the canvas maps the pointer to a
// cell, on the canvas's device grid. Run at device pixel ratios 1.25 and 1.5
// too (tests/CMakeLists.txt), where a canvas row is a fractional number of
// logical pixels: at 1.25 and zoom 4 a row is 64 device pixels, 51.2 logical,
// and dividing by the 52 of a band rounded outward put the top slice of row n
// (about 0.8 n logical pixels deep) in row n - 1.
TEST_CASE("the IRC meter's row under a pixel is the canvas's cell under it, at any device pixel ratio (#404)",
          "[irc-meter-dpr]") {
    const int zoom = GENERATE(4, 3);
    CAPTURE(zoom);
    StripedWindow sw(zoom);
    IrcMeter* m = sw.m;
    CanvasView* view = sw.view;
    const qreal dpr = view->devicePixelRatioF();
    const int rows = sw.w.document()->canvas().height();

    // Codex's counterexample (round 2 of #403): scrolled 103 down at zoom 4,
    // the meter's top pixel is level with view y 103, 128.75 device pixels at
    // 1.25, in row 2 (from 128); floor(103 / 52) is 1. At 1 it is row 1.
    if (zoom == 4) {
        sw.scrollTo(103);
        REQUIRE(view->mapFrom(&sw.w, m->mapTo(&sw.w, QPoint(0, 0))).y() == 103);
        if (dpr == 1.25)
            CHECK(m->rowAt(0) == 2);
        if (dpr == 1.0)
            CHECK(m->rowAt(0) == 1);
    }

    for (const int s : {0, 103, 1000, sw.v->maximum()}) {
        CAPTURE(s);
        sw.scrollTo(std::min(s, sw.v->maximum()));
        // every meter pixel row: rowAt is the canvas row under it
        int wrong = 0, firstWrong = -1;
        for (int y = 0; y < m->height(); ++y) {
            const int want = sw.canvasRow(y);
            REQUIRE(viewRowUnder(sw.w, m, view, y) == want);
            if (m->rowAt(y) != want && wrong++ == 0)
                firstWrong = y;
        }
        CAPTURE(firstWrong);
        CHECK(wrong == 0);

        // every row on screen: its band is the pixels under it, the bands
        // tile the meter, and at a whole ratio a band is the canvas row's
        // logical rect, as before
        const int first = std::max(0, sw.canvasRow(0));
        const int last = std::min(rows - 1, sw.canvasRow(m->height() - 1));
        REQUIRE(first <= last);
        if (s == sw.v->maximum())
            CHECK(last == rows - 1);
        for (int n = first; n <= last; ++n) {
            CAPTURE(n);
            const QRect r = m->rowRect(n);
            CHECK(r.left() == 0);
            CHECK(r.width() == m->width());
            REQUIRE(r.height() > 0);
            CHECK(sw.canvasRow(r.top()) == n);
            CHECK(sw.canvasRow(r.top() - 1) == n - 1);
            CHECK(sw.canvasRow(r.bottom()) == n);
            CHECK(sw.canvasRow(r.bottom() + 1) == n + 1);
            CHECK(m->rowRect(n + 1).top() == r.bottom() + 1);
            if (dpr == 1.0)
                CHECK(r == QRect(0, m->mapFrom(&sw.w, view->mapTo(&sw.w, QPoint(0, 0))).y() +
                                        view->toWidget(view->cellRect(QPoint(0, n))).top(),
                                 m->width(), view->toWidget(view->cellRect(QPoint(0, n))).height()));
        }
    }
}

TEST_CASE("the IRC meter's tooltip names the canvas row under the pointer, at any device pixel ratio (#404)",
          "[irc-meter-dpr]") {
    StripedWindow sw(4);
    IrcMeter* m = sw.m;
    const qreal dpr = sw.view->devicePixelRatioF();
    const int rows = sw.w.document()->canvas().height();
    const int x = m->width() / 2;

    // the issue's repro: scrolled to the bottom, the middle of the last
    // row's band names the last row
    sw.scrollTo(sw.v->maximum());
    const QRect lastBand = m->rowRect(rows - 1);
    const std::string tip = tooltipAt(m, QPoint(x, lastBand.center().y()));
    CHECK(tip == m->describe(rows - 1).toStdString());
    CHECK(tip.rfind("Row " + std::to_string(rows - 1) + ": ", 0) == 0);

    // Codex's counterexample: the top pixel, scrolled 103 down, is row 2 at 1.25
    sw.scrollTo(103);
    if (dpr == 1.25)
        CHECK(tooltipAt(m, QPoint(x, 0)) == m->describe(2).toStdString());
    CHECK(tooltipAt(m, QPoint(x, 0)) == m->describe(sw.canvasRow(0)).toStdString());

    // the top, middle and bottom pixel of every band on screen
    for (const int s : {0, sw.v->maximum()}) {
        CAPTURE(s);
        sw.scrollTo(s);
        const int first = std::max(0, sw.canvasRow(0));
        const int last = std::min(rows - 1, sw.canvasRow(m->height() - 1));
        for (int n = first; n <= last; ++n) {
            const QRect r = m->rowRect(n);
            for (const int y : {r.top(), r.center().y(), r.bottom()}) {
                if (y < 0 || y >= m->height())
                    continue;
                CAPTURE(n, y);
                CHECK(tooltipAt(m, QPoint(x, y)) == m->describe(sw.canvasRow(y)).toStdString());
            }
        }
    }
    QToolTip::hideText();
}

TEST_CASE("the IRC meter paints each row's bar in the canvas row's band, in a partial repaint too (#404)",
          "[irc-meter-dpr]") {
    StripedWindow sw(4);
    IrcMeter* m = sw.m;
    const qreal dpr = m->devicePixelRatioF();
    const int rows = sw.w.document()->canvas().height();
    const QColor over(0xd0, 0x30, 0x30), fits = m->palette().color(QPalette::Mid);
    REQUIRE(fits != m->palette().color(QPalette::Window));
    for (int n = 0; n < rows; ++n)
        REQUIRE(m->state(n) == (n % 2 == 0 ? IrcMeter::RowState::Over : IrcMeter::RowState::Fits));
    // a device pixel at the left of the bar, in logical pixel row y of an
    // image grabbed from the meter's pixel row `top`
    const auto at = [&](const QImage& img, int y, int top) {
        return img.pixelColor(int(std::floor(1.5 * dpr)), int(std::floor((y - top + 0.5) * dpr))).name().toStdString();
    };

    for (const int s : {0, 1000, sw.v->maximum()}) {
        CAPTURE(s);
        sw.scrollTo(s);
        const QImage full = m->grab().toImage();
        const int first = std::max(0, sw.canvasRow(0));
        const int last = std::min(rows - 1, sw.canvasRow(m->height() - 1));
        for (int n = first; n <= last; ++n) {
            CAPTURE(n);
            const QRect r = m->rowRect(n);
            if (r.top() < 0 || r.bottom() >= m->height())
                continue;  // a band cut by the meter's edge
            const std::string want = (n % 2 == 0 ? over : fits).name().toStdString();
            CHECK(at(full, r.center().y(), 0) == want);
            // a repaint of the top of the band alone paints the band's row
            const QRect part(0, r.top(), m->width(), std::min(8, r.height() - 1));
            const QImage img = m->grab(part).toImage();
            CHECK(at(img, r.top() + part.height() / 2, r.top()) == want);
        }
    }
}
