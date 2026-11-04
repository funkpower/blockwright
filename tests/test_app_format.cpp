// The GUI's document format, headless: File > SAUCE..., the iCE colours and
// 9 px letter-spacing toggles, their undo, and their round trip through
// save and open. main() is in test_app.cpp.

#include "util.h"

#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "plural.h"
#include "sauce_dialog.h"
#include "select_tool.h"
#include "shortcuts_dialog.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QUndoStack>

#include <functional>
#include <map>
#include <memory>
#include <vector>

using namespace bw;

namespace {

QAction* action(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

// The menu (by its title without '&') that holds `a`.
QString menuOf(MainWindow& w, QAction* a) {
    for (QAction* m : w.menuBar()->actions())
        if (m->menu() && m->menu()->actions().contains(a))
            return m->text().remove(QLatin1Char('&'));
    return {};
}

void typeInto(QWidget* w, const QString& text) {
    for (const QChar c : text) {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
        QApplication::sendEvent(w, &press);
    }
}

QRgb q(Rgb c) { return qRgb(c.r, c.g, c.b); }

// Cell (x, y)'s pixel (px, py) as the 1x preview shows it.
QRgb previewPixel(MainWindow& w, int x, int y, int px = 3, int py = 8) {
    const QSize cs = w.renderer().cellSize();
    const QImage img = w.preview()->grab(QRect(QPoint(x * cs.width(), y * cs.height()), cs)).toImage();
    return img.pixel(px, py);
}

void put(Document* d, int x, int y, Cell c) {
    CellEdit e;
    e.changes.push_back({x, y, d->canvas().at(x, y), c});
    REQUIRE(d->edit(e));
}

// Writes `body` with a SAUCE record whose iCE flag is off; returns its path.
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

// Answers the next message box with `button`; `asked` says one came.
void answer(QWidget* context, QMessageBox::StandardButton button, bool* asked) {
    *asked = false;
    QTimer::singleShot(0, context, [=] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box)
            return;
        *asked = true;
        box->button(button)->click();
    });
}

QString saveTo(MainWindow& w, const QTemporaryDir& tmp, const char* name) {
    const QString path = tmp.filePath(QString::fromLatin1(name));
    QString error;
    REQUIRE(w.saveFile(path, &error));
    return path;
}

// Fills in the SAUCE dialog `fill` gets once File > SAUCE... opens it, then
// presses OK.
void runSauceDialog(MainWindow& w, const std::function<void(SauceDialog*)>& fill) {
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* d = qobject_cast<SauceDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        fill(d);
        ran = true;
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    action(w, "sauce")->trigger();
    CHECK(ran);
}

} // namespace

TEST_CASE("the SAUCE dialog holds fields to their SAUCE limits and CP437") {
    Sauce s = Sauce::fresh(80, 25);
    s.title = "Title";
    s.author = "funkpower";
    s.comments = {"one", "\xb0\xb1\xb2"};
    SauceDialog d(s);
    QPushButton* ok = d.buttons()->button(QDialogButtonBox::Ok);
    CHECK(d.titleEdit()->text() == QStringLiteral("Title"));
    CHECK(d.commentsEdit()->toPlainText() == QStringLiteral("one\n░▒▓"));
    CHECK(d.problem().isEmpty());
    CHECK(ok->isEnabled());

    // title 35, author 20, group 20: typing or pasting past the limit stops there
    CHECK(d.titleEdit()->maxLength() == 35);
    CHECK(d.authorEdit()->maxLength() == 20);
    CHECK(d.groupEdit()->maxLength() == 20);
    d.titleEdit()->clear();
    typeInto(d.titleEdit(), QString(40, QLatin1Char('x')));
    CHECK(d.titleEdit()->text() == QString(35, QLatin1Char('x')));
    d.authorEdit()->clear();
    d.authorEdit()->insert(QString(25, QLatin1Char('a')));
    CHECK(d.authorEdit()->text() == QString(20, QLatin1Char('a')));
    d.groupEdit()->setText(QString(30, QLatin1Char('g')));
    CHECK(d.groupEdit()->text().size() == 20);

    // only text CP437 can store: ★ and “ ” are refused, ░ and é are taken
    d.groupEdit()->setText(QStringLiteral("grp"));
    typeInto(d.groupEdit(), QStringLiteral("★"));
    d.groupEdit()->insert(QStringLiteral("“x”"));
    CHECK(d.groupEdit()->text() == QStringLiteral("grp"));
    typeInto(d.groupEdit(), QStringLiteral("░é"));
    CHECK(d.groupEdit()->text() == QStringLiteral("grp░é"));
    CHECK(d.fields().group == "grp\xb0\x82");
    // a decomposed é is one CP437 byte
    CHECK(toCp437(QStringLiteral("é")) == std::string("\x82"));
    CHECK_FALSE(toCp437(QStringLiteral("a★")));
    CHECK_FALSE(toCp437(QString(QChar(0))));
    CHECK(ok->isEnabled());

    // comments: 64 per line, CP437 only, up to 255 lines; OK waits for a fix
    d.commentsEdit()->setPlainText(QString(64, QLatin1Char('c')) + QStringLiteral("\nok"));
    CHECK(d.problem().isEmpty());
    CHECK(ok->isEnabled());
    d.commentsEdit()->setPlainText(QStringLiteral("ok\n") + QString(65, QLatin1Char('c')));
    CHECK(d.problem().contains(QStringLiteral("line 2")));
    CHECK_FALSE(ok->isEnabled());
    d.commentsEdit()->setPlainText(QStringLiteral("a ★ star"));
    CHECK(d.problem().contains(QStringLiteral("CP437")));
    CHECK_FALSE(ok->isEnabled());
    QStringList many;
    for (int i = 0; i < 256; ++i)
        many << QStringLiteral("x");
    d.commentsEdit()->setPlainText(many.join(QLatin1Char('\n')));
    CHECK_FALSE(d.problem().isEmpty());
    many.removeLast();
    d.commentsEdit()->setPlainText(many.join(QLatin1Char('\n')) + QStringLiteral("\n\n  \n"));
    CHECK(d.problem().isEmpty());
    CHECK(d.fields().comments.size() == 255);  // trailing empty lines dropped
    // trailing spaces go, as SAUCE pads with them; blank lines inside stay
    d.commentsEdit()->setPlainText(QStringLiteral("a  \n\nb"));
    CHECK(d.fields().comments == std::vector<std::string>{"a", "", "b"});

    // the date: empty or a real CCYYMMDD, digits only
    d.dateEdit()->clear();
    typeInto(d.dateEdit(), QStringLiteral("2026-10x22"));
    CHECK(d.dateEdit()->text() == QStringLiteral("20261022"));
    CHECK(d.problem().isEmpty());
    d.dateEdit()->setText(QStringLiteral("20261340"));
    CHECK_FALSE(d.problem().isEmpty());
    CHECK_FALSE(ok->isEnabled());
    d.dateEdit()->setText(QStringLiteral("2026"));
    CHECK_FALSE(d.problem().isEmpty());
    d.dateEdit()->clear();
    CHECK(d.problem().isEmpty());
    CHECK(ok->isEnabled());
}

TEST_CASE("the SAUCE dialog keeps CP437 0xFF in comments and fields") {
    // 0xFF is U+00A0, which QPlainTextEdit::toPlainText makes a space
    Sauce s;
    s.title = "x\xff";
    s.comments = {"a\xff" "b"};
    SauceDialog d(s);
    CHECK(d.fields().title == s.title);
    CHECK(d.fields().comments == s.comments);
    // and when the comments are edited
    d.commentsEdit()->moveCursor(QTextCursor::End);
    d.commentsEdit()->insertPlainText(QStringLiteral("\nc"));
    CHECK(d.fields().comments == std::vector<std::string>{"a\xff" "b", "c"});
}

TEST_CASE("the SAUCE dialog accepted unchanged keeps trailing blank comment lines") {
    for (const std::vector<std::string>& comments :
         {std::vector<std::string>{"hello", ""}, std::vector<std::string>{""}}) {
        CAPTURE(comments);
        Sauce s = Sauce::fresh(80, 1);
        s.comments = comments;
        CHECK(SauceDialog(s).fields().comments == comments);

        // a file with them: OK on the dialog as shown changes nothing
        QTemporaryDir tmp;
        AnsiDocument doc = bwtest::parse("A");
        doc.sauce = s;
        const QString path = tmp.filePath(QStringLiteral("comments.ans"));
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        const std::string bytes = writeAnsi(doc).bytes;
        f.write(bytes.data(), qint64(bytes.size()));
        f.close();
        MainWindow w;
        QString error;
        REQUIRE(w.openFile(path, &error));
        REQUIRE(w.document()->ansi().sauce->comments == comments);
        runSauceDialog(w, [](SauceDialog*) {});
        CHECK(w.document()->undoStack()->count() == 0);
        CHECK_FALSE(w.document()->isModified());
        CHECK(w.document()->ansi().sauce->comments == comments);
        // cleared, they go ({""} shows as an empty box already)
        if (comments.size() > 1) {
            runSauceDialog(w, [](SauceDialog* d) { d->commentsEdit()->clear(); });
            CHECK(w.document()->ansi().sauce->comments.empty());
        }
    }
}

TEST_CASE("a malformed SAUCE date from the file doesn't block the dialog and is kept") {
    for (const char* date : {"00000000", "1996"}) {
        CAPTURE(date);
        Sauce s = Sauce::fresh(80, 1);
        s.date = date;
        SauceDialog d(s);
        QPushButton* ok = d.buttons()->button(QDialogButtonBox::Ok);
        CHECK(d.problem().isEmpty());
        d.titleEdit()->setText(QStringLiteral("new title"));
        CHECK(ok->isEnabled());
        CHECK(d.fields().title == "new title");
        CHECK(d.fields().date == date);
        // a date typed in still has to be a real one
        d.dateEdit()->setText(QStringLiteral("20261340"));
        CHECK_FALSE(ok->isEnabled());
        d.dateEdit()->setText(QString::fromLatin1(date));
        CHECK(ok->isEnabled());
    }
}

TEST_CASE("File > SAUCE... is one undo step that survives save and open") {
    QTemporaryDir tmp;
    MainWindow w;
    QAction* sauce = action(w, "sauce");
    CHECK(menuOf(w, sauce) == QStringLiteral("File"));
    Document* doc = w.document();
    REQUIRE_FALSE(doc->ansi().sauce);

    // accepted as shown: nothing changes, not even a record added
    runSauceDialog(w, [](SauceDialog*) {});
    CHECK_FALSE(doc->isModified());
    CHECK(doc->undoStack()->count() == 0);
    CHECK_FALSE(doc->ansi().sauce);

    runSauceDialog(w, [](SauceDialog* d) {
        d->titleEdit()->setText(QStringLiteral("Night ░ Shift"));
        d->authorEdit()->setText(QStringLiteral("funkpower"));
        d->groupEdit()->setText(QStringLiteral("FPW"));
        d->dateEdit()->setText(QStringLiteral("19970704"));
        d->commentsEdit()->setPlainText(QStringLiteral("line one\nline two"));
    });
    REQUIRE(doc->ansi().sauce);
    CHECK(doc->ansi().sauce->title == "Night \xb0 Shift");
    CHECK(doc->ansi().sauce->author == "funkpower");
    CHECK(doc->ansi().sauce->group == "FPW");
    CHECK(doc->ansi().sauce->date == "19970704");
    CHECK(doc->ansi().sauce->comments == std::vector<std::string>{"line one", "line two"});
    CHECK(doc->isModified());
    CHECK(doc->undoStack()->count() == 1);

    // one undo step back to no record at all, and redo
    doc->undoStack()->undo();
    CHECK_FALSE(doc->ansi().sauce);
    CHECK_FALSE(doc->isModified());
    doc->undoStack()->redo();
    CHECK(doc->ansi().sauce->author == "funkpower");
    CHECK(doc->isModified());

    const QString path = saveTo(w, tmp, "sauce.ans");
    CHECK_FALSE(doc->isModified());
    MainWindow again;
    QString error;
    REQUIRE(again.openFile(path, &error));
    const Sauce& back = *again.document()->ansi().sauce;
    CHECK(back.title == "Night \xb0 Shift");
    CHECK(back.author == "funkpower");
    CHECK(back.group == "FPW");
    CHECK(back.date == "19970704");
    CHECK(back.comments == std::vector<std::string>{"line one", "line two"});

    // the dialog shows what was read, and an edit keeps the rest of the record
    runSauceDialog(again, [](SauceDialog* d) {
        CHECK(d->titleEdit()->text() == QStringLiteral("Night ░ Shift"));
        CHECK(d->commentsEdit()->toPlainText() == QStringLiteral("line one\nline two"));
        d->commentsEdit()->clear();
    });
    CHECK(again.document()->ansi().sauce->comments.empty());
    CHECK(again.document()->ansi().sauce->title == "Night \xb0 Shift");
    CHECK(again.document()->undoStack()->count() == 1);
}

TEST_CASE("View > iCE Colours is undoable, changes the rendering and is saved and read back") {
    QTemporaryDir tmp;
    MainWindow w;
    QAction* ice = action(w, "iceColours");
    CHECK(menuOf(w, ice) == QStringLiteral("View"));
    CHECK(ice->isCheckable());
    CHECK(ice->isChecked());  // a new canvas has iCE colours
    Document* doc = w.document();
    put(doc, 0, 0, Cell{U' ', Color::indexed(Grey), Color::indexed(LightRed), 0, false});
    put(doc, 1, 0, Cell{U'█', Color::indexed(Yellow), Color::indexed(Black), 0, false});
    doc->undoStack()->setClean();
    CHECK(previewPixel(w, 0, 0) == q(kPalette[LightRed]));

    ice->trigger();
    CHECK_FALSE(ice->isChecked());
    CHECK_FALSE(doc->ansi().ice);
    CHECK(doc->isModified());
    CHECK(previewPixel(w, 0, 0) == q(kPalette[Red]));  // blinks on dark red
    CHECK(previewPixel(w, 1, 0) == q(kPalette[Yellow]));
    CHECK(doc->canvas().at(0, 0).bg == Color::indexed(LightRed));  // as drawn

    doc->undoStack()->undo();
    CHECK(ice->isChecked());
    CHECK(doc->ansi().ice);
    CHECK_FALSE(doc->isModified());
    CHECK(previewPixel(w, 0, 0) == q(kPalette[LightRed]));
    doc->undoStack()->redo();
    CHECK_FALSE(ice->isChecked());
    CHECK(previewPixel(w, 0, 0) == q(kPalette[Red]));

    // saved with the iCE flag off, the bright background written as blink
    const QString off = saveTo(w, tmp, "ice-off.ans");
    const AnsiDocument read = parseAnsi(bwtest::readFile(off.toStdString()));
    REQUIRE(read.sauce);
    CHECK_FALSE(read.sauce->ice());
    CHECK(read.canvas.at(0, 0) == Cell{U' ', Color::indexed(Grey), Color::indexed(Red), attr::Blink, false});

    // and read back on open
    MainWindow again;
    QString error;
    REQUIRE(again.openFile(off, &error));
    CHECK_FALSE(action(again, "iceColours")->isChecked());
    CHECK(previewPixel(again, 0, 0) == q(kPalette[Red]));
    // turned on there, the blink shows and saves as the bright background
    action(again, "iceColours")->trigger();
    CHECK(previewPixel(again, 0, 0) == q(kPalette[LightRed]));
    const QString on = saveTo(again, tmp, "ice-on.ans");
    const AnsiDocument onRead = parseAnsi(bwtest::readFile(on.toStdString()));
    CHECK(onRead.sauce->ice());
    CHECK(onRead.canvas.at(0, 0) == Cell{U' ', Color::indexed(Grey), Color::indexed(LightRed), 0, false});
    MainWindow third;
    REQUIRE(third.openFile(on, &error));
    CHECK(action(third, "iceColours")->isChecked());
}

TEST_CASE("a trailing DarkGrey space survives an iCE-off save and shows again with iCE on") {
    QTemporaryDir tmp;
    MainWindow w;
    Document* doc = w.document();
    put(doc, 0, 0, Cell{U'A', Color::indexed(Grey), Color::indexed(Black), 0, false});
    put(doc, 1, 0, Cell{U' ', Color::indexed(Grey), Color::indexed(DarkGrey), 0, false});
    action(w, "iceColours")->trigger();
    REQUIRE_FALSE(doc->ansi().ice);
    const QString off = saveTo(w, tmp, "grey-off.ans");
    MainWindow again;
    QString error;
    REQUIRE(again.openFile(off, &error));
    CHECK_FALSE(again.document()->ansi().ice);
    action(again, "iceColours")->trigger();
    CHECK(previewPixel(again, 1, 0) == q(kPalette[DarkGrey]));
}

TEST_CASE("an iCE-off file with bright backgrounds opens with iCE on, as it is rendered and converted") {
    // SGR 101 is a bright background whatever the iCE flag; writeAnsi keeps
    // it by writing iCE on, so the editor shows and saves it that way
    QTemporaryDir tmp;
    const QString path = writeIceOff(tmp, "bright.ans", "\x1b[101m ");
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    CHECK(w.document()->ansi().ice);
    CHECK(action(w, "iceColours")->isChecked());
    CHECK(previewPixel(w, 0, 0) == q(kPalette[LightRed]));
    CHECK_FALSE(w.document()->isModified());
}

TEST_CASE("an iCE-off file opened with iCE on shows and saves its blinking cells as cli render and convert do") {
    // writeAnsi keeps a blinking cell's dark background and drops the blink
    // once a bright background turns iCE on: the editor shows the same, saves
    // the same bytes and warns that the blink is lost
    struct Case {
        const char* body;
        int x;  // a blinking cell
        Rgb bg;  // its background, as renderCanvas draws it
        int blinks;  // blinking cells in the file
    };
    const Case cases[] = {
        {"\x1b[0;5;31;44mA\x1b[0;37;104mB", 0, kPalette[Blue], 1},
        {"\x1b[0;5;37;104mC", 0, kPalette[LightBlue], 1},
        {"\x1b[101mA\x1b[0;5;44mB", 1, kPalette[Blue], 1},
        // #36: trailing blinking black spaces, trimmed by both; their blink
        // never showed, so only A's counts (#42)
        {"\x1b[0;5;31;44mA\x1b[0;37;104mB\x1b[0;5;30;40m  ", 0, kPalette[Blue], 1},
    };
    for (const Case& c : cases) {
        CAPTURE(c.body);
        QTemporaryDir tmp;
        const QString path = writeIceOff(tmp, "blink.ans", c.body);
        const AnsiDocument parsed = parseAnsi(bwtest::readFile(path.toStdString()));
        REQUIRE_FALSE(parsed.ice);
        const AnsiOutput cli = writeAnsi(parsed);
        REQUIRE(cli.blinkDropped == c.blinks);

        MainWindow w;
        QString error;
        REQUIRE(w.openFile(path, &error));
        CHECK(w.document()->ansi().ice);
        CHECK_FALSE(w.document()->isModified());
        CHECK(previewPixel(w, c.x, 0, 0, 0) == q(c.bg));
        const AnsiOutput out = w.document()->encode();
        CHECK(out.bytes == cli.bytes);
        CHECK(out.blinkDropped == cli.blinkDropped);

        // the save stops on the loss, and names it
        const QString saved = tmp.filePath(QStringLiteral("saved.ans"));
        CHECK_FALSE(w.saveFile(saved, &error));
        CHECK(error.contains(QStringLiteral("blink")));
        REQUIRE(w.saveFile(saved, &error, true));
        CHECK(bwtest::readFile(saved.toStdString()) == cli.bytes);
        // the file now written has no blink left to lose
        CHECK(w.document()->encode().blinkDropped == 0);
        CHECK(w.saveFile(saved, &error));
    }
}

TEST_CASE("the save warning for blink lost on open stays true once iCE is off, and ends with the cell erased") {
    // #36: turning iCE off doesn't give the blink back. #82: erasing the cell
    // is the artist's own edit, so the cell no longer claims a blink
    const QString lost = QStringLiteral(
        "1 cell lost its blink when the file was opened with iCE colours, and this save doesn't put it back");
    QTemporaryDir tmp;
    const QString path = writeIceOff(tmp, "blink.ans", "\x1b[0;5;31;44mA\x1b[0;37;104mB");
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    REQUIRE(w.document()->ansi().ice);
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));

    SECTION("iCE off") {
        REQUIRE(w.document()->setIce(false));
        CHECK(w.document()->encode().blinkDropped == 1);
        CHECK_FALSE(w.saveFile(saved, &error));
        CHECK(error == lost);
        CHECK(saveWarning(w.document()->encode()).contains(lost));
        // #42: a save to a new path leaves the original's blink alone, so
        // the warning doesn't say the original lost it
        CHECK_FALSE(error.contains(QStringLiteral("original")));
    }
    SECTION("the blinking cell erased") {
        const Cell a = w.document()->canvas().at(0, 0);
        REQUIRE(w.document()->edit(CellEdit{{{0, 0, a, Cell{}}}}));
        CHECK(w.document()->encode().blinkDropped == 0);
        w.document()->undoStack()->undo();
        CHECK(w.document()->encode().blinkDropped == 1);
        w.document()->undoStack()->redo();
        REQUIRE(w.saveFile(saved, &error));
        CHECK(parseAnsi(bwtest::readFile(saved.toStdString())).canvas.at(0, 0).ch == U' ');
    }
}

TEST_CASE("the save warning for blink lost on open follows the lost cell through edits") {
    // #79, #82: the count is the cells marked on open as having lost their
    // blink that the save still writes without one. The mark goes where the
    // cell goes, so a lost cell that only moved stays lost, and one made to
    // blink again, in place or moved, is back. Fixture from #75 round 2: A
    // blinks red on blue at (0,0), B is grey on bright blue at (1,0).
    QTemporaryDir tmp;
    const QString path = writeIceOff(tmp, "blink.ans", "\x1b[0;5;31;44mA\x1b[0;37;104mB");
    const Cell original = parseAnsi(bwtest::readFile(path.toStdString())).canvas.at(0, 0);
    REQUIRE((original.attrs & attr::Blink));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    Document* d = w.document();
    REQUIRE(d->encode().blinkDropped == 1);
    QUndoStack* undo = d->undoStack();
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));
    const auto blinksAt = [](const std::string& bytes, int x) {
        const Cell c = parseAnsi(bytes).canvas.at(x, 0);
        return c.ch == U'A' && (c.attrs & attr::Blink);
    };

    SECTION("iCE off and A put back in place: nothing lost, and undo and redo follow") {
        REQUIRE(d->setIce(false));
        put(d, 0, 0, original);
        const AnsiOutput out = d->encode();
        CHECK(blinksAt(out.bytes, 0));
        CHECK(out.blinkDropped == 0);
        undo->undo();
        CHECK(d->encode().blinkDropped == 1);
        undo->redo();
        CHECK(d->encode().blinkDropped == 0);
        REQUIRE(w.saveFile(saved, &error));
        CHECK(bwtest::readFile(saved.toStdString()) == out.bytes);
    }
    SECTION("iCE off and the cells flipped: A moves and B's bright background takes its place") {
        REQUIRE(d->setIce(false));
        action(w, "selectTool")->trigger();
        w.selectTool()->select({0, 0, 2, 1});
        action(w, "flipHorizontal")->trigger();
        REQUIRE(d->canvas().at(0, 0).ch == U'B');
        REQUIRE(d->canvas().at(1, 0).ch == U'A');
        CHECK(d->encode().blinkDropped == 1);
        undo->undo();
        CHECK(d->encode().blinkDropped == 1);
        undo->redo();
        CHECK(d->encode().blinkDropped == 1);
        CHECK_FALSE(w.saveFile(saved, &error));
        CHECK(error.contains(QStringLiteral("1 cell lost its blink")));
    }
    SECTION("iCE off, A put back and moved: the moved cell carries its blink") {
        REQUIRE(d->setIce(false));
        put(d, 0, 0, original);
        action(w, "selectTool")->trigger();
        w.selectTool()->select({0, 0, 1, 1});
        w.selectTool()->moveBy(2, 0);
        w.selectTool()->stamp();
        REQUIRE(d->canvas().at(0, 0) == Cell{});
        REQUIRE(d->canvas().at(2, 0) == original);
        const AnsiOutput out = d->encode();
        CHECK(blinksAt(out.bytes, 2));
        CHECK(out.blinkDropped == 0);
        undo->undo();  // the move
        CHECK(d->encode().blinkDropped == 0);
        undo->undo();  // the put
        CHECK(d->encode().blinkDropped == 1);
        undo->redo();
        undo->redo();
        CHECK(d->encode().blinkDropped == 0);
        REQUIRE(w.saveFile(saved, &error));
        CHECK(bwtest::readFile(saved.toStdString()) == out.bytes);
    }
    SECTION("iCE off, A erased: its claim goes with it, and undo brings it back") {
        REQUIRE(d->setIce(false));
        put(d, 0, 0, Cell{});
        CHECK(d->encode().blinkDropped == 0);
        undo->undo();
        CHECK(d->encode().blinkDropped == 1);
        put(d, 5, 0, original);
        CHECK(d->encode().blinkDropped == 1);
    }
    SECTION("A drawn blinking over itself with iCE on is the bright background iCE shows") {
        // the artist drew the cell anew, as it saves with iCE on
        put(d, 0, 0, original);
        CHECK(d->encode().blinkDropped == 0);
        REQUIRE(w.saveFile(saved, &error));
        const AnsiDocument back = parseAnsi(bwtest::readFile(saved.toStdString()));
        CHECK(back.ice);
        CHECK(back.canvas.at(0, 0).bg.paletteIndex() == LightBlue);
        REQUIRE(d->setIce(false));
        CHECK(d->encode().blinkDropped == 0);
    }
    SECTION("a blinking cell that isn't A doesn't bring it back, and drawn over A ends A's claim") {
        REQUIRE(d->setIce(false));
        Cell space = original;
        space.ch = U' ';
        put(d, 3, 0, space);
        CHECK(d->encode().blinkDropped == 1);
        Cell other = original;
        other.fg = Color::indexed(Green);
        put(d, 4, 0, other);
        CHECK(d->encode().blinkDropped == 1);
        put(d, 0, 0, other);
        CHECK(d->encode().blinkDropped == 0);
    }
    SECTION("a failed save keeps the record; a successful one clears it") {
        const QString nowhere = tmp.filePath(QStringLiteral("missing/saved.ans"));
        CHECK_FALSE(w.saveFile(nowhere, &error, true));
        CHECK(d->encode().blinkDropped == 1);
        REQUIRE(w.saveFile(saved, &error, true));
        CHECK(d->encode().blinkDropped == 0);
    }
    SECTION("reopening replaces the record and New clears it") {
        REQUIRE(d->setIce(false));
        put(d, 0, 0, original);
        REQUIRE(d->encode().blinkDropped == 0);
        REQUIRE(w.openFile(path, &error));
        CHECK(d->encode().blinkDropped == 1);
        const QString plain = writeIceOff(tmp, "plain.ans", "\x1b[0;37;104mB");
        REQUIRE(w.openFile(plain, &error));
        CHECK(d->encode().blinkDropped == 0);
        REQUIRE(w.openFile(path, &error));
        REQUIRE(d->encode().blinkDropped == 1);
        d->clear();
        CHECK(d->encode().blinkDropped == 0);
    }
}

TEST_CASE("a cell the file already wrote as the lost one doesn't bring it back") {
    // #79: with iCE off, A red on bright blue saves as A blinking red on
    // blue, which is how the lost A was written; it wasn't marked on open
    // (#82), so it doesn't stand in for the lost one
    QTemporaryDir tmp;
    const QString path = writeIceOff(tmp, "twin.ans", "\x1b[0;5;31;44mA\x1b[0;31;104mA");
    const Cell original = parseAnsi(bwtest::readFile(path.toStdString())).canvas.at(0, 0);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    Document* d = w.document();
    REQUIRE(d->encode().blinkDropped == 1);
    REQUIRE(d->setIce(false));
    CHECK(d->encode().blinkDropped == 1);
    put(d, 0, 0, original);
    CHECK(d->encode().blinkDropped == 0);
}

namespace {

// Opens `body` as an iCE-off file in `w` and turns iCE off, so the cells
// save as they were in the file; the document.
Document* openIceOff(MainWindow& w, const QTemporaryDir& tmp, const char* name, const std::string& body) {
    QString error;
    REQUIRE(w.openFile(writeIceOff(tmp, name, body), &error));
    REQUIRE(w.document()->ansi().ice);
    REQUIRE(w.document()->setIce(false));
    return w.document();
}

// Copies the cells in `from` and stamps them with their top-left at `to`.
void copyTo(MainWindow& w, const CellRect& from, QPoint to) {
    action(w, "selectTool")->trigger();
    w.selectTool()->select(from);
    w.selectTool()->copy();
    w.selectTool()->floatBlock(w.selectTool()->clipboard(), to);
    w.selectTool()->stamp();
}

// A cell from scratch, as a tool draws it: nothing of what was there before.
Cell drawn(char32_t ch, std::uint8_t fg, std::uint8_t bg, std::uint8_t attrs = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    c.attrs = attrs;
    return c;
}

bool blinksIn(const std::string& bytes, int x) { return parseAnsi(bytes).canvas.at(x, 0).attrs & attr::Blink; }

} // namespace

TEST_CASE("another cell made to blink doesn't stand in for a cell that lost its blink on open") {
    // #82 round 1 (P2): the claim is the lost cell's own and goes where edits
    // take it; a cell that never lost a blink earns nothing by blinking
    QTemporaryDir tmp;
    MainWindow w;
    QString error;
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));

    SECTION("the bright < flipped into a blinking > beside the lost >") {
        Document* d = openIceOff(w, tmp, "arrows.ans", "\x1b[0;5;31;44m>\x1b[0;31;104m<");
        action(w, "selectTool")->trigger();
        w.selectTool()->select({1, 0, 1, 1});
        action(w, "flipHorizontal")->trigger();
        REQUIRE(d->canvas().at(1, 0).ch == U'>');
        const AnsiOutput out = d->encode();
        CHECK(blinksIn(out.bytes, 1));
        CHECK_FALSE(blinksIn(out.bytes, 0));
        CHECK(out.blinkDropped == 1);
        CHECK_FALSE(w.saveFile(saved, &error));
        CHECK(error.contains(QStringLiteral("1 cell lost its blink")));
        d->undoStack()->undo();
        CHECK(d->encode().blinkDropped == 1);
        d->undoStack()->redo();
        CHECK(d->encode().blinkDropped == 1);
    }
    SECTION("twin.ans: the bright A copied, then the lost A put back") {
        Document* d = openIceOff(w, tmp, "twin.ans", "\x1b[0;5;31;44mA\x1b[0;31;104mA");
        const Cell original = drawn(U'A', Red, Blue, attr::Blink);
        copyTo(w, {1, 0, 1, 1}, {5, 0});
        REQUIRE(d->canvas().at(5, 0) == d->canvas().at(1, 0));
        CHECK(blinksIn(d->encode().bytes, 5));
        CHECK(d->encode().blinkDropped == 1);
        CHECK_FALSE(w.saveFile(saved, &error));
        put(d, 0, 0, original);
        CHECK(d->encode().blinkDropped == 0);
        REQUIRE(w.saveFile(saved, &error));
        CHECK(blinksIn(bwtest::readFile(saved.toStdString()), 0));
    }
    SECTION("the lost A drawn again elsewhere, blinking, while it stays as it is") {
        Document* d = openIceOff(w, tmp, "blink.ans", "\x1b[0;5;31;44mA\x1b[0;37;104mB");
        put(d, 5, 0, drawn(U'A', Red, Blue, attr::Blink));
        CHECK(d->encode().blinkDropped == 1);
    }
}

TEST_CASE("two cells that lost their blink on open come back one at a time") {
    // #82 round 1: duplicate lost cells, one restored; a copy of the restored
    // one doesn't restore the other
    QTemporaryDir tmp;
    MainWindow w;
    Document* d = openIceOff(w, tmp, "pair.ans", "\x1b[0;5;31;44mAA\x1b[0;37;104mB");
    QUndoStack* undo = d->undoStack();
    const Cell original = drawn(U'A', Red, Blue, attr::Blink);
    CHECK(d->encode().blinkDropped == 2);
    put(d, 0, 0, original);
    CHECK(d->encode().blinkDropped == 1);
    copyTo(w, {0, 0, 1, 1}, {5, 0});
    REQUIRE(d->canvas().at(5, 0) == original);
    CHECK(d->encode().blinkDropped == 1);
    put(d, 1, 0, original);
    CHECK(d->encode().blinkDropped == 0);
    undo->undo();
    CHECK(d->encode().blinkDropped == 1);
    undo->undo();  // the copy
    CHECK(d->encode().blinkDropped == 1);
    undo->undo();
    CHECK(d->encode().blinkDropped == 2);
}

TEST_CASE("attribute bits other than blink neither restore a lost cell nor end its claim") {
    // #82 round 1: writeAnsi writes only the blink bit, so a cell drawn over
    // the lost one that differs from it only in other bits saves as it did
    QTemporaryDir tmp;
    MainWindow w;
    Document* d = openIceOff(w, tmp, "blink.ans", "\x1b[0;5;31;44mA\x1b[0;37;104mB");
    put(d, 0, 0, drawn(U'A', Red, Blue, 0x80));
    CHECK(d->encode().blinkDropped == 1);
    put(d, 5, 0, drawn(U'A', Red, Blue, attr::Blink | 0x80));
    CHECK(d->encode().blinkDropped == 1);
    put(d, 0, 0, drawn(U'A', Red, Blue, attr::Blink | 0x80));
    CHECK(d->encode().blinkDropped == 0);
}

TEST_CASE("a lost cell drawn over with its CP437 look-alike is the same cell to the save") {
    // #82 round 1 (P3): ß and β (and µ and μ) are one CP437 byte, so the save
    // writes them alike
    QTemporaryDir tmp;
    MainWindow w;
    QString error;
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));

    SECTION("beta, blinking, in place of a lost sharp s") {
        Document* d = openIceOff(w, tmp, "eszett.ans", "\x1b[0;5;31;44m\xe1\x1b[0;37;104mB");
        REQUIRE(d->canvas().at(0, 0).ch == U'ß');
        put(d, 0, 0, drawn(U'β', Red, Blue, attr::Blink));
        CHECK(d->encode().blinkDropped == 0);
        REQUIRE(w.saveFile(saved, &error));
        const Cell back = parseAnsi(bwtest::readFile(saved.toStdString())).canvas.at(0, 0);
        CHECK(back.ch == U'ß');
        CHECK((back.attrs & attr::Blink));
    }
    SECTION("mu, blinking, in place of a lost micro sign") {
        Document* d = openIceOff(w, tmp, "micro.ans", "\x1b[0;5;31;44m\xe6\x1b[0;37;104mB");
        REQUIRE(d->canvas().at(0, 0).ch == U'µ');
        put(d, 0, 0, drawn(U'μ', Red, Blue, attr::Blink));
        CHECK(d->encode().blinkDropped == 0);
    }
    SECTION("beta without the blink keeps the sharp s's claim") {
        Document* d = openIceOff(w, tmp, "eszett.ans", "\x1b[0;5;31;44m\xe1\x1b[0;37;104mB");
        put(d, 0, 0, drawn(U'β', Red, Blue));
        CHECK(d->encode().blinkDropped == 1);
        put(d, 5, 0, drawn(U'ß', Red, Blue, attr::Blink));
        CHECK(d->encode().blinkDropped == 1);
        CHECK_FALSE(w.saveFile(saved, &error));
    }
}

TEST_CASE("a lost cell drawn over or moved onto a cell the save writes alike keeps its claim") {
    // #82 round 1: an edit that leaves the cell as the save writes it can't
    // be what takes its blink away, and a move can't leave the claim behind
    // where the cell it lands on looks the same
    QTemporaryDir tmp;
    MainWindow w;
    Document* d = openIceOff(w, tmp, "same.ans", "\x1b[0;5;31;44mA\x1b[0;31;44mA\x1b[0;37;104mB");
    REQUIRE(d->encode().blinkDropped == 1);

    SECTION("drawn over as it is") {
        CHECK_FALSE(d->edit(CellEdit{{{0, 0, d->canvas().at(0, 0), drawn(U'A', Red, Blue)}}}));
        CHECK(d->encode().blinkDropped == 1);
    }
    SECTION("moved onto the plain A beside it") {
        action(w, "selectTool")->trigger();
        w.selectTool()->select({0, 0, 1, 1});
        w.selectTool()->moveBy(1, 0);
        w.selectTool()->stamp();
        REQUIRE(d->canvas().at(0, 0) == Cell{});
        CHECK(d->encode().blinkDropped == 1);
        put(d, 1, 0, drawn(U'A', Red, Blue, attr::Blink));
        CHECK(d->encode().blinkDropped == 0);
    }
}

TEST_CASE("flipping a lost cell with its look-alike moves the claim and doesn't copy it") {
    // #82 round 2 (P2 1): the flip takes the lost A to (1,0) and puts the
    // plain A at (0,0); the plain A doesn't take over the claim it leaves
    QTemporaryDir tmp;
    MainWindow w;
    QString error;
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));
    Document* d = openIceOff(w, tmp, "same.ans", "\x1b[0;5;31;44mA\x1b[0;31;44mA\x1b[0;37;104mB");
    QUndoStack* undo = d->undoStack();
    REQUIRE(d->encode().blinkDropped == 1);
    action(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    action(w, "flipHorizontal")->trigger();
    CHECK_FALSE(d->canvas().at(0, 0).lostBlink);
    CHECK(d->canvas().at(1, 0).lostBlink);
    CHECK(d->encode().blinkDropped == 1);
    put(d, 1, 0, drawn(U'A', Red, Blue, attr::Blink));
    CHECK(d->encode().blinkDropped == 0);
    undo->undo();
    CHECK(d->encode().blinkDropped == 1);
    undo->undo();  // the flip
    CHECK(d->canvas().at(0, 0).lostBlink);
    CHECK_FALSE(d->canvas().at(1, 0).lostBlink);
    CHECK(d->encode().blinkDropped == 1);
    undo->redo();
    CHECK_FALSE(d->canvas().at(0, 0).lostBlink);
    CHECK(d->canvas().at(1, 0).lostBlink);
    CHECK(d->encode().blinkDropped == 1);
    undo->redo();
    CHECK(d->encode().blinkDropped == 0);
    REQUIRE(w.saveFile(saved, &error));
    const std::string bytes = bwtest::readFile(saved.toStdString());
    CHECK_FALSE(blinksIn(bytes, 0));
    CHECK(blinksIn(bytes, 1));
}

TEST_CASE("a look-alike pasted or moved onto a lost cell that stays put leaves it its claim") {
    // #82 round 2: as drawing it does; the lost cell isn't the one moved
    QTemporaryDir tmp;
    MainWindow w;
    Document* d = openIceOff(w, tmp, "same.ans", "\x1b[0;5;31;44mA\x1b[0;31;44mA\x1b[0;37;104mB");
    REQUIRE(d->encode().blinkDropped == 1);
    action(w, "selectTool")->trigger();

    SECTION("pasted") {
        copyTo(w, {1, 0, 1, 1}, {0, 0});
        CHECK(d->canvas().at(0, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 1);
    }
    SECTION("moved") {
        w.selectTool()->select({1, 0, 1, 1});
        w.selectTool()->moveBy(-1, 0);
        w.selectTool()->stamp();
        REQUIRE(d->canvas().at(1, 0) == Cell{});
        CHECK(d->canvas().at(0, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 1);
    }
}

TEST_CASE("colorizing a lost cell ends its claim as drawing the recoloured cell does") {
    // #82 round 2 (P2 2): the colorize brush starts from a copy of the cell,
    // mark and all; the save writes it green, so the edit is the artist's
    QTemporaryDir tmp;
    MainWindow w;
    Document* d = openIceOff(w, tmp, "same.ans", "\x1b[0;5;31;44mA\x1b[0;31;44mA\x1b[0;37;104mB");
    QUndoStack* undo = d->undoStack();
    REQUIRE(d->encode().blinkDropped == 1);

    SECTION("the colorize brush") {
        REQUIRE(d->edit(paintStroke(d->canvas(), ColorizeBrush(Color::indexed(Green), std::nullopt), {}, {{0, 0}})));
        REQUIRE(d->canvas().at(0, 0) == drawn(U'A', Green, Blue));
        CHECK_FALSE(d->canvas().at(0, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 0);
        undo->undo();
        CHECK(d->canvas().at(0, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 1);
        undo->redo();
        CHECK(d->encode().blinkDropped == 0);
    }
    SECTION("the glyph brush") {
        REQUIRE(d->edit(paintStroke(d->canvas(), GlyphBrush(drawn(U'A', Green, Blue)), {}, {{0, 0}})));
        CHECK(d->encode().blinkDropped == 0);
    }
    SECTION("colorized to the colour it has") {
        CHECK_FALSE(d->edit(paintStroke(d->canvas(), ColorizeBrush(Color::indexed(Red), std::nullopt), {}, {{0, 0}})));
        CHECK(d->encode().blinkDropped == 1);
    }
}

namespace {

// A 3x3 field of K on k (rung 4 of steel), blinking, with a lighter speck
// (rung 8, a space on w) in the middle that doesn't, and a bright B below so
// the file opens with iCE on: eight lost cells.
const std::string kField = [] {
    const std::string k = "\x1b[0;1;5;30;40m\xdb";
    const std::string speck = "\x1b[0;30;47m ";
    return k + k + k + "\r\n" + k + speck + k + "\r\n" + k + k + k + "\r\n\x1b[0;37;104mB";
}();

int marked(const Canvas& c) {
    int n = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            n += c.at(x, y).lostBlink;
    return n;
}

} // namespace

TEST_CASE("mirror mode keeps a symmetric pair of lost cells' claims through a paste, a move and undo") {
    // #98: in mirror X an edit's image at width - 1 - x is part of the edit
    // (mirrorEdit). A lost A at (0,0) and its image at (79,0), a plain
    // look-alike A at (1,0).
    QTemporaryDir tmp;
    MainWindow w;
    QString error;
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));
    const std::string lost = "\x1b[0;5;31;44mA";
    Document* d = openIceOff(w, tmp, "pair.ans",
                             lost + "\x1b[0;31;44mA\x1b[0;37;104mB\x1b[0m" + std::string(76, ' ') + lost);
    QUndoStack* undo = d->undoStack();
    REQUIRE(d->canvas().at(0, 0).lostBlink);
    REQUIRE(d->canvas().at(79, 0).lostBlink);
    REQUIRE(d->encode().blinkDropped == 2);
    action(w, "mirrorMode")->trigger();
    REQUIRE(w.canvasView()->mirror());

    SECTION("the look-alike stamped onto the lost cell: both keep their claim") {
        // bit 0x80, which the save doesn't write, makes the paste a change
        // that still writes alike
        put(d, 1, 0, drawn(U'A', Red, Blue, 0x80));
        REQUIRE(d->encode().blinkDropped == 2);
        copyTo(w, {1, 0, 1, 1}, {0, 0});
        CHECK((d->canvas().at(0, 0).attrs & 0x80));
        CHECK((d->canvas().at(79, 0).attrs & 0x80));
        CHECK(d->canvas().at(0, 0).lostBlink);
        CHECK(d->canvas().at(79, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 2);
        undo->undo();
        CHECK_FALSE((d->canvas().at(0, 0).attrs & 0x80));
        CHECK_FALSE((d->canvas().at(79, 0).attrs & 0x80));
        CHECK(d->canvas().at(0, 0).lostBlink);
        CHECK(d->canvas().at(79, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 2);
        undo->redo();
        CHECK((d->canvas().at(0, 0).attrs & 0x80));
        CHECK((d->canvas().at(79, 0).attrs & 0x80));
        CHECK(d->canvas().at(0, 0).lostBlink);
        CHECK(d->canvas().at(79, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 2);
    }
    SECTION("the look-alike stamped where only the image lands on a lost cell: that one keeps its claim") {
        put(d, 0, 0, drawn(U'B', White, Blue));  // not through the view: not mirrored
        REQUIRE(d->encode().blinkDropped == 1);
        copyTo(w, {1, 0, 1, 1}, {0, 0});
        CHECK(d->canvas().at(0, 0) == drawn(U'A', Red, Blue));
        CHECK_FALSE(d->canvas().at(0, 0).lostBlink);
        CHECK(d->canvas().at(79, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 1);
    }
    SECTION("the lost cell moved: its image moves with it") {
        action(w, "selectTool")->trigger();
        w.selectTool()->select({0, 0, 1, 1});
        w.selectTool()->moveBy(3, 0);
        w.selectTool()->stamp();
        CHECK(d->canvas().at(3, 0).lostBlink);
        CHECK(d->canvas().at(76, 0).lostBlink);
        CHECK(identical(d->canvas().at(0, 0), Cell{}));
        CHECK(identical(d->canvas().at(79, 0), Cell{}));
        CHECK(d->encode().blinkDropped == 2);
        undo->undo();
        CHECK(d->canvas().at(0, 0).lostBlink);
        CHECK(d->canvas().at(79, 0).lostBlink);
        CHECK_FALSE(d->canvas().at(3, 0).lostBlink);
        CHECK_FALSE(d->canvas().at(76, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 2);
        undo->redo();
        CHECK(d->encode().blinkDropped == 2);
    }
    SECTION("the lost cell drawn blinking: its image blinks too and the save loses nothing") {
        REQUIRE(w.canvasView()->applyEdit(CellEdit{{{0, 0, d->canvas().at(0, 0), drawn(U'A', Red, Blue, attr::Blink)}}}));
        CHECK_FALSE(d->canvas().at(0, 0).lostBlink);
        CHECK_FALSE(d->canvas().at(79, 0).lostBlink);
        CHECK(d->encode().blinkDropped == 0);
        REQUIRE(w.saveFile(saved, &error));
        const std::string bytes = bwtest::readFile(saved.toStdString());
        CHECK(blinksIn(bytes, 0));
        CHECK(blinksIn(bytes, 79));
    }
    SECTION("the lost cell drawn over as it is: nothing changes") {
        CHECK_FALSE(w.canvasView()->applyEdit(CellEdit{{{0, 0, d->canvas().at(0, 0), drawn(U'A', Red, Blue)}}}));
        CHECK(d->encode().blinkDropped == 2);
    }
}

TEST_CASE("mirror mode flips a lost cell with its look-alike, and undo restores the marks") {
    // #98: as without mirror mode, the claim moves with the lost A; undo puts
    // back every cell the flip and its image changed, marks included. The
    // image of the lost A at (78,0) is a copy with a mark of its own (PLAN
    // §4, the test below), so the flip counts two.
    QTemporaryDir tmp;
    MainWindow w;
    Document* d = openIceOff(w, tmp, "same.ans", "\x1b[0;5;31;44mA\x1b[0;31;44mA\x1b[0;37;104mB");
    QUndoStack* undo = d->undoStack();
    REQUIRE(d->encode().blinkDropped == 1);
    const Canvas before = d->canvas();
    action(w, "mirrorMode")->trigger();
    action(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    action(w, "flipHorizontal")->trigger();
    REQUIRE(d->canvas().at(1, 0).ch == U'A');
    CHECK_FALSE(d->canvas().at(0, 0).lostBlink);
    CHECK(d->canvas().at(1, 0).lostBlink);
    CHECK(d->canvas().at(78, 0) == drawn(U'A', Red, Blue));
    CHECK(d->canvas().at(78, 0).lostBlink);
    CHECK(d->canvas().at(79, 0) == drawn(U'A', Red, Blue));
    CHECK_FALSE(d->canvas().at(79, 0).lostBlink);
    CHECK(d->encode().blinkDropped == 2);
    const Canvas flipped = d->canvas();
    undo->undo();
    CHECK(d->canvas().at(0, 0).lostBlink);
    CHECK(marked(d->canvas()) == 1);
    for (int x = 0; x < before.width(); ++x)
        CHECK(identical(d->canvas().at(x, 0), before.at(x, 0)));
    CHECK(d->encode().blinkDropped == 1);
    undo->redo();
    for (int x = 0; x < flipped.width(); ++x)
        CHECK(identical(d->canvas().at(x, 0), flipped.at(x, 0)));
    CHECK(d->encode().blinkDropped == 2);
}

TEST_CASE("documented current behaviour: in mirror mode the image of a moved lost cell counts as a lost blink too") {
    // #98 P3 2, kept as PLAN §4 documents it: the image of a move is pasted
    // as a copy, mark and all, so the save warning over-counts. A fix that
    // gives the image the mark of the cell it moves from on its own side
    // changes the counts below; update this test with it.
    QTemporaryDir tmp;
    MainWindow w;
    QString error;
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));
    Document* d = openIceOff(w, tmp, "same.ans", "\x1b[0;5;31;44mA\x1b[0;31;44mA\x1b[0;37;104mB");
    REQUIRE(d->encode().blinkDropped == 1);
    REQUIRE(marked(d->canvas()) == 1);
    action(w, "mirrorMode")->trigger();
    REQUIRE(w.canvasView()->mirror());
    action(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    w.selectTool()->moveBy(4, 0);
    w.selectTool()->stamp();
    REQUIRE(d->canvas().at(4, 0).ch == U'A');
    CHECK(d->canvas().at(4, 0).lostBlink);
    // (75,0) was blank on open and lost nothing, yet it counts
    CHECK(d->canvas().at(75, 0).lostBlink);
    CHECK(marked(d->canvas()) == 2);
    CHECK(d->encode().blinkDropped == 2);

    // the moved A made to blink again: its image still counts, and the save
    // still stops on it
    put(d, 4, 0, drawn(U'A', Red, Blue, attr::Blink));
    CHECK_FALSE(d->canvas().at(4, 0).lostBlink);
    CHECK(d->canvas().at(75, 0).lostBlink);
    CHECK(d->encode().blinkDropped == 1);
    CHECK_FALSE(w.saveFile(saved, &error));
    CHECK(error.contains(QStringLiteral("blink")));
}

TEST_CASE("despeckling next to lost cells doesn't copy their claim into the speck") {
    // #82 round 2: the despeckle brush copies the neighbour's look into the
    // speck; the speck never lost a blink
    QTemporaryDir tmp;
    MainWindow w;
    Document* d = openIceOff(w, tmp, "field.ans", kField);
    const Ramp r = *findRamp("steel");
    REQUIRE(d->canvas().at(0, 0) == rungCell(r, 4));
    REQUIRE(d->canvas().at(1, 1) == rungCell(r, 8));
    REQUIRE(d->encode().blinkDropped == 8);
    StrokeOptions o;
    o.ice = false;
    REQUIRE(d->edit(paintStroke(d->canvas(), DespeckleBrush(r), {}, {{1, 1}}, o)));
    REQUIRE(d->canvas().at(1, 1) == rungCell(r, 4));
    CHECK_FALSE(d->canvas().at(1, 1).lostBlink);
    CHECK(marked(d->canvas()) == 8);
    CHECK(d->encode().blinkDropped == 8);
    d->undoStack()->undo();
    CHECK(d->encode().blinkDropped == 8);
}

TEST_CASE("every brush leaves a painted lost cell its claim only when the save writes it alike") {
    // #82 round 2: one rule for every brush, whatever it builds its cell from
    const Ramp r = *findRamp("steel");
    const Pen pen{Color::indexed(LightRed), Color::indexed(Blue)};
    struct Case {
        const char* name;
        std::function<std::unique_ptr<Brush>()> make;
    };
    const std::vector<Case> cases = {
        {"shade", [&] { return std::make_unique<ShadeBrush>(pen, false); }},
        {"glyph", [&] { return std::make_unique<GlyphBrush>(rungCell(r, 4)); }},
        {"colorize", [] { return std::make_unique<ColorizeBrush>(Color::indexed(Green), std::nullopt); }},
        {"ramp shade", [&] { return std::make_unique<RampShadeBrush>(r, ShadeDirection::Lighten); }},
        {"soft", [&] { return std::make_unique<SoftShadeBrush>(r, ShadeDirection::Darken); }},
        {"form", [&] { return std::make_unique<FormShadeBrush>(r); }},
        {"despeckle", [&] { return std::make_unique<DespeckleBrush>(r); }},
        {"outline", [&] { return std::make_unique<OutlineBrush>(r); }},
        {"rim light", [&] { return std::make_unique<RimLightBrush>(r, Compass::NW); }},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        QTemporaryDir tmp;
        MainWindow w;
        Document* d = openIceOff(w, tmp, "field.ans", kField);
        const Canvas before = d->canvas();
        BrushSettings s;
        s.size = 3;
        StrokeOptions o;
        o.ice = false;
        d->edit(paintStroke(before, *c.make(), s, {{1, 1}}, o));
        const Canvas& after = d->canvas();
        int keep = 0;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                INFO(x << "," << y);
                const bool alike = before.at(x, y).lostBlink && writesAlike(before.at(x, y), after.at(x, y));
                CHECK(after.at(x, y).lostBlink == alike);
                keep += alike;
            }
        CHECK(marked(after) == keep);
        CHECK(d->encode().blinkDropped == keep);
    }
}

TEST_CASE("an iCE-off file whose only blink is on blank glyphs opens and saves without a loss warning") {
    // #42: blink with iCE off hides and shows the foreground, so on a space it
    // never shows; turning iCE on for the bright background loses nothing
    QTemporaryDir tmp;
    const QString path =
        writeIceOff(tmp, "blank-blink.ans", "\x1b[0;5;40m   \x1b[0;37;104mB\r\n\x1b[0;5;40m    ");
    const AnsiDocument parsed = parseAnsi(bwtest::readFile(path.toStdString()));
    REQUIRE_FALSE(parsed.ice);
    const AnsiOutput cli = writeAnsi(parsed);
    REQUIRE(cli.blinkDropped == 0);

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    CHECK(w.document()->ansi().ice);
    CHECK(w.document()->canvas().at(0, 0).attrs == 0);
    CHECK(w.document()->canvas().at(3, 1).attrs == 0);
    const AnsiOutput out = w.document()->encode();
    CHECK(out.blinkDropped == 0);
    CHECK(out.bytes == cli.bytes);
    const QString saved = tmp.filePath(QStringLiteral("saved.ans"));
    REQUIRE(w.saveFile(saved, &error));
    CHECK(bwtest::readFile(saved.toStdString()) == cli.bytes);
}

TEST_CASE("View > 9 px Letter Spacing sizes the view and preview, is undoable and is saved and read back") {
    QTemporaryDir tmp;
    MainWindow w;
    QAction* nine = action(w, "ninePx");
    CHECK(menuOf(w, nine) == QStringLiteral("View"));
    CHECK_FALSE(nine->isChecked());
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    v->setZoom(2);
    put(doc, 0, 0, Cell{U'─', Color::indexed(White), Color::indexed(Blue), 0, false});
    put(doc, 1, 0, Cell{U'▓', Color::indexed(White), Color::indexed(Blue), 0, false});
    doc->undoStack()->setClean();
    CHECK(w.renderer().cellSize() == QSize(8, 16));

    nine->trigger();
    CHECK(nine->isChecked());
    CHECK(doc->isModified());
    CHECK(w.renderer().cellSize() == QSize(9, 16));
    CHECK(w.preview()->size() == QSize(80 * 9, 40 * 16));
    CHECK(v->size() == QSize(80 * 9 * 2, 40 * 16 * 2));
    REQUIRE(doc->ansi().sauce);
    CHECK(doc->ansi().sauce->letterSpacing() == 2);
    // the 9th column: ─ joins, ▓ doesn't
    const QRgb white = q(kPalette[White]), blue = q(kPalette[Blue]);
    int joined = 0;
    for (int y = 0; y < 16; ++y) {
        CHECK(previewPixel(w, 0, 0, 8, y) == previewPixel(w, 0, 0, 7, y));
        joined += previewPixel(w, 0, 0, 8, y) == white;
        CHECK(previewPixel(w, 1, 0, 8, y) == blue);
    }
    CHECK(joined > 0);
    // the zoomed view is the same pixels, scaled
    v->setCellCursor({5, 5});
    const QImage zoomed = v->grab(QRect(0, 0, 18 * 2, 16 * 2)).toImage().convertToFormat(QImage::Format_RGB32);
    Canvas two(2, 1);
    two.at(0, 0) = doc->canvas().at(0, 0);
    two.at(1, 0) = doc->canvas().at(1, 0);
    CHECK(zoomed == renderCanvas(two, 9, 2));

    doc->undoStack()->undo();
    CHECK_FALSE(nine->isChecked());
    CHECK_FALSE(doc->isModified());
    CHECK(w.renderer().cellSize() == QSize(8, 16));
    CHECK(w.preview()->size() == QSize(80 * 8, 40 * 16));
    CHECK(v->size() == QSize(80 * 8 * 2, 40 * 16 * 2));
    CHECK_FALSE(doc->ansi().sauce);  // back to no record at all
    doc->undoStack()->redo();
    CHECK(w.renderer().cellSize() == QSize(9, 16));

    const QString path = saveTo(w, tmp, "nine.ans");
    CHECK(parseAnsi(bwtest::readFile(path.toStdString())).sauce->letterSpacing() == 2);
    MainWindow again;
    QString error;
    REQUIRE(again.openFile(path, &error));
    CHECK(action(again, "ninePx")->isChecked());
    CHECK(again.renderer().cellSize() == QSize(9, 16));

    // off again: SAUCE says 8 px, explicitly
    action(again, "ninePx")->trigger();
    CHECK(again.renderer().cellSize() == QSize(8, 16));
    CHECK(again.preview()->size() == QSize(80 * 8, 40 * 16));
    const QString eight = saveTo(again, tmp, "eight.ans");
    CHECK(parseAnsi(bwtest::readFile(eight.toStdString())).sauce->letterSpacing() == 1);
}

TEST_CASE("the aspect-ratio flag and font name survive the toggles, the SAUCE dialog and a save") {
    QTemporaryDir tmp;
    AnsiDocument src = bwtest::parse("hi");
    src.sauce = Sauce::fresh(80, 1);
    src.sauce->setAspectRatio(1);
    src.sauce->font = "IBM VGA50";
    src.sauce->title = "aspect";
    const QString path = tmp.filePath(QStringLiteral("aspect.ans"));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    const std::string bytes = writeAnsi(src).bytes;
    f.write(bytes.data(), qint64(bytes.size()));
    f.close();

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    action(w, "iceColours")->trigger();
    action(w, "ninePx")->trigger();
    runSauceDialog(w, [](SauceDialog* d) { d->authorEdit()->setText(QStringLiteral("funkpower")); });
    CHECK(w.document()->undoStack()->count() == 3);
    const AnsiDocument back = parseAnsi(bwtest::readFile(saveTo(w, tmp, "aspect-out.ans").toStdString()));
    REQUIRE(back.sauce);
    CHECK(back.sauce->aspectRatio() == 1);
    CHECK(back.sauce->font == "IBM VGA50");
    CHECK(back.sauce->title == "aspect");
    CHECK(back.sauce->author == "funkpower");
    CHECK_FALSE(back.sauce->ice());
    CHECK(back.sauce->letterSpacing() == 2);

    // each undo takes back one change
    QUndoStack* st = w.document()->undoStack();
    st->undo();
    CHECK(w.document()->ansi().sauce->author.empty());
    CHECK(w.document()->ansi().sauce->letterSpacing() == 2);
    st->undo();
    CHECK(w.document()->ansi().sauce->letterSpacing() == 0);
    CHECK_FALSE(w.document()->ansi().ice);
    st->undo();
    CHECK(w.document()->ansi().ice);
    CHECK(w.document()->ansi().sauce->aspectRatio() == 1);
}

TEST_CASE("no two actions share a shortcut") {
    // #7: the format actions add none, and nothing else may clash either
    MainWindow w;
    std::map<QString, QString> seen;
    for (QAction* a : windowActions(&w)) {
        for (const QKeySequence& seq : a->shortcuts()) {
            const QString k = seq.toString(QKeySequence::PortableText);
            const auto it = seen.find(k);
            INFO(k.toStdString() << ": " << a->text().toStdString() << " and "
                                 << (it == seen.end() ? QString() : it->second).toStdString());
            CHECK(it == seen.end());
            seen[k] = a->text();
        }
    }
    CHECK(seen.size() > 20);
}

TEST_CASE("a count picks the English form, with no translator installed (#304)") {
    const QString one = QStringLiteral("%n line over the %1-byte budget"),
                  many = QStringLiteral("%n lines over the %1-byte budget");
    CHECK(plural(1, one, many).arg(407) == QStringLiteral("1 line over the 407-byte budget"));
    CHECK(plural(3, one, many).arg(407) == QStringLiteral("3 lines over the 407-byte budget"));
    CHECK(plural(0, one, many).arg(407) == QStringLiteral("0 lines over the 407-byte budget"));
    // what tr does with a count and no translator: the count, and line(s) as written
    CHECK(QCoreApplication::translate("MainWindow", "Exported %n IRC line(s) to %1", nullptr, 1) ==
          QStringLiteral("Exported 1 IRC line(s) to %1"));

    AnsiOutput out;
    out.unwritable = {{0, 0, U'\u263A'}};
    out.blinkDropped = 1;
    CHECK(lossDescription(out) ==
          QStringLiteral("1 cell holds a glyph with no CP437 code and will be written as '?': ☺ at 0,0;\n"
                         "1 cell lost its blink when the file was opened with iCE colours, and this save doesn't "
                         "put it back"));
    out.unwritable.push_back({1, 0, U'\u263B'});
    out.blinkDropped = 2;
    CHECK(lossDescription(out) ==
          QStringLiteral("2 cells hold a glyph with no CP437 code and will be written as '?': ☺ at 0,0; "
                         "☻ at 1,0;\n"
                         "2 cells lost their blink when the file was opened with iCE colours, and this save doesn't "
                         "put it back"));
}

TEST_CASE("the app opens a UTF-8 .ans as its cells and saves it as UTF-8", "[app][utf8]") {
    const std::string cp437Path = (bwtest::dataDir() / "golden/tux.ans").string();
    const std::string utf8Path = (bwtest::dataDir() / "utf8/tux-utf8.ans").string();
    const AnsiDocument cp437 = parseAnsi(bwtest::readFile(cp437Path));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(QString::fromStdString(utf8Path), &error));
    CHECK(w.document()->ansi().encoding == TextEncoding::Utf8);
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(w.document()->canvas() == cp437.canvas);
    REQUIRE(w.document()->ansi().sauce);
    CHECK(w.document()->ansi().sauce->author == "funkpower");

    QTemporaryDir tmp;
    auto read = [](const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll().toStdString() : std::string();
    };
    // Save keeps the encoding it was opened in: the UTF-8 file, byte for byte
    const QString out = tmp.filePath(QStringLiteral("tux.ans"));
    REQUIRE(w.saveFile(out, &error));
    CHECK(read(out) == bwtest::readFile(utf8Path));
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);

    // Save As CP437 writes the CP437 piece, and Save then keeps CP437
    const QString cp = tmp.filePath(QStringLiteral("tux-cp437.ans"));
    REQUIRE(w.saveFile(cp, &error, false, TextEncoding::Cp437));
    CHECK(read(cp) == writeAnsi(cp437).bytes);
    CHECK(w.document()->textEncoding() == TextEncoding::Cp437);
    REQUIRE(w.saveFile(cp, &error));
    CHECK(read(cp) == writeAnsi(cp437).bytes);
}

TEST_CASE("the app saves a CP437 .ans as CP437 unless told UTF-8", "[app][utf8]") {
    const std::string cp437Path = (bwtest::dataDir() / "golden/tux.ans").string();
    const std::string utf8Path = (bwtest::dataDir() / "utf8/tux-utf8.ans").string();
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(QString::fromStdString(cp437Path), &error));
    CHECK(w.document()->textEncoding() == TextEncoding::Cp437);
    QTemporaryDir tmp;
    auto read = [](const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll().toStdString() : std::string();
    };
    const QString out = tmp.filePath(QStringLiteral("tux.ans"));
    REQUIRE(w.saveFile(out, &error));
    CHECK(read(out) == bwtest::readFile(cp437Path));

    // Save As UTF-8, then Save keeps UTF-8
    const QString u8 = tmp.filePath(QStringLiteral("tux-utf8.ans"));
    REQUIRE(w.saveFile(u8, &error, false, TextEncoding::Utf8));
    CHECK(read(u8) == bwtest::readFile(utf8Path));
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
    REQUIRE(w.saveFile(u8, &error));
    CHECK(read(u8) == bwtest::readFile(utf8Path));

    // an XBIN is CP437, and the document saved as one is too
    const QString xb = tmp.filePath(QStringLiteral("tux.xb"));
    REQUIRE(w.saveFile(xb, &error, false, TextEncoding::Utf8));
    CHECK(read(xb).starts_with("XBIN\x1a"));
    CHECK(w.document()->textEncoding() == TextEncoding::Cp437);

    // a new document is CP437
    MainWindow fresh;
    CHECK(fresh.document()->textEncoding() == TextEncoding::Cp437);
}

TEST_CASE("UTF-8 saves glyphs a CP437 save loses, with no warning", "[app][utf8]") {
    MainWindow w;
    QString error;
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("star.ans"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("\xE2\x98\x85\xE2\x96\x88\r\n");  // ★█
    }
    REQUIRE(w.openFile(path, &error));
    REQUIRE(w.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(w.document()->canvas().at(0, 0).ch == U'\u2605');
    CHECK_FALSE(w.document()->encode().lossy());
    CHECK(w.document()->encode(ArtFormat::Ansi, TextEncoding::Cp437).unwritable.size() == 1);
    // saved without allowLossy: nothing is lost, so nothing to allow
    REQUIRE(w.saveFile(path, &error));
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK(f.readAll().contains("\xE2\x98\x85\xE2\x96\x88"));
}

TEST_CASE("a UTF-8 save asks before writing a lone wide cell or a lone mark as '?'", "[app][utf8]") {
    // review of #348, M1 and M2: the row keeps its columns, and the save says so
    MainWindow w;
    QString error;
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("lone.ans"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("\xe2\x96\x88" "b\x1b[D\x1b[C\xcc\x81" "c\r\n");  // █ b, CUB, CUF, U+0301, c
    }
    REQUIRE(w.openFile(path, &error));
    REQUIRE(w.document()->textEncoding() == TextEncoding::Utf8);
    REQUIRE(w.document()->canvas().at(2, 0).ch == U'\u0301');
    CellEdit e;
    e.changes.push_back({5, 0, w.document()->canvas().at(5, 0), Cell{U'中'}});  // lone wide, as typing makes it
    REQUIRE(w.document()->edit(e));
    const AnsiOutput out = w.document()->encode();
    REQUIRE(out.encoding == TextEncoding::Utf8);
    REQUIRE(out.unwritable.size() == 2);
    const QString warning = saveWarning(out);
    CHECK(warning.contains(QStringLiteral("U+0301 at 2,0")));
    CHECK(warning.contains(QStringLiteral("中 at 5,0")));
    CHECK(warning.contains(QStringLiteral("so the row keeps its columns")));
    CHECK_FALSE(w.saveFile(path, &error));
    CHECK(error.contains(QStringLiteral("'?'")));
    REQUIRE(w.saveFile(path, &error, true));
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const AnsiDocument back = parseAnsi(f.readAll().toStdString());
    CHECK(back.canvas.height() == w.document()->canvas().height());
    CHECK(back.canvas.at(2, 0).ch == U'?');
    CHECK(back.canvas.at(3, 0).ch == U'c');
    CHECK(back.canvas.at(5, 0).ch == U'?');
}

namespace {

std::string fileBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().toStdString() : std::string();
}

} // namespace

TEST_CASE("File > Open's filters pick the encoding an .ans is read as", "[app][utf8]") {
    const QStringList filters = openNameFilters();
    REQUIRE(filters.size() == 8);
    CHECK(filters[2] == QStringLiteral("ANSI art (*.ans)"));
    CHECK(filters[3] == QStringLiteral("ANSI art, CP437 (*.ans)"));
    CHECK(filters[4] == QStringLiteral("ANSI art, UTF-8 (*.ans)"));
    CHECK(openFilterEncoding(filters[3]) == TextEncoding::Cp437);
    CHECK(openFilterEncoding(filters[4]) == TextEncoding::Utf8);
    for (const int i : {0, 1, 2, 5, 6, 7}) {
        INFO(filters[i].toStdString());
        CHECK_FALSE(openFilterEncoding(filters[i]).has_value());
    }
    CHECK_FALSE(openFilterEncoding(QString()).has_value());

    const QString utf8Path = QString::fromStdString((bwtest::dataDir() / "utf8/tux-utf8.ans").string());
    const AnsiDocument cp437 = parseAnsi(bwtest::readFile((bwtest::dataDir() / "golden/tux.ans").string()));
    MainWindow w;
    QString error;
    // auto: the bytes say UTF-8
    REQUIRE(w.openFile(utf8Path, &error));
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(w.document()->canvas() == cp437.canvas);
    CHECK(w.encodingLabel()->text() == QStringLiteral("UTF-8"));
    // told CP437, each byte is a glyph: the canvas is the mojibake CP437 draws
    REQUIRE(w.openFile(utf8Path, &error, TextEncoding::Cp437));
    CHECK(w.document()->textEncoding() == TextEncoding::Cp437);
    CHECK(w.document()->canvas() == parseAnsi(bwtest::readFile(utf8Path.toStdString()), {.encoding = TextEncoding::Cp437}).canvas);
    CHECK_FALSE(w.document()->canvas() == cp437.canvas);
    CHECK(w.encodingLabel()->text() == QStringLiteral("CP437"));
    // and saved, it writes those bytes back, as CP437
    QTemporaryDir tmp;
    const QString out = tmp.filePath(QStringLiteral("mojibake.ans"));
    REQUIRE(w.saveFile(out, &error));
    CHECK(parseAnsi(fileBytes(out)).encoding == TextEncoding::Cp437);
}

TEST_CASE("File > Reopen with Encoding reads the document's .ans again", "[app][utf8]") {
    const QString utf8Path = QString::fromStdString((bwtest::dataDir() / "utf8/tux-utf8.ans").string());
    const AnsiDocument cp437 = parseAnsi(bwtest::readFile((bwtest::dataDir() / "golden/tux.ans").string()));
    MainWindow w;
    auto* menu = w.findChild<QMenu*>(QStringLiteral("reopenWithEncoding"));
    REQUIRE(menu);
    CHECK(menuOf(w, menu->menuAction()) == QStringLiteral("File"));
    CHECK_FALSE(menu->isEnabled());  // a new document has no file to read again

    QString error;
    REQUIRE(w.openFile(utf8Path, &error));
    CHECK(menu->isEnabled());
    action(w, "reopenCp437")->trigger();
    CHECK(w.document()->path() == utf8Path);
    CHECK(w.document()->textEncoding() == TextEncoding::Cp437);
    CHECK_FALSE(w.document()->canvas() == cp437.canvas);
    CHECK(w.encodingLabel()->text() == QStringLiteral("CP437"));
    CHECK_FALSE(w.isWindowModified());
    action(w, "reopenUtf8")->trigger();
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(w.document()->canvas() == cp437.canvas);
    action(w, "reopenCp437")->trigger();
    action(w, "reopenAuto")->trigger();
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(w.encodingLabel()->text() == QStringLiteral("UTF-8"));

    // an XBIN and a project have no text encoding to choose
    QTemporaryDir tmp;
    const QString xb = tmp.filePath(QStringLiteral("tux.xb"));
    REQUIRE(w.saveFile(xb, &error));
    CHECK(w.document()->path() == xb);
    CHECK_FALSE(menu->isEnabled());
    action(w, "reopenUtf8")->trigger();  // does nothing
    CHECK(w.document()->path() == xb);
    const QString bwp = tmp.filePath(QStringLiteral("tux.bwp"));
    REQUIRE(w.saveFile(bwp, &error));
    CHECK(w.document()->form() == FileForm::ProjectZip);
    CHECK_FALSE(menu->isEnabled());
    w.document()->clear();
    CHECK_FALSE(menu->isEnabled());
}

TEST_CASE("the status bar shows the encoding Save writes, through open, save, save-as and a project",
          "[app][utf8]") {
    const QString cp437Path = QString::fromStdString((bwtest::dataDir() / "golden/tux.ans").string());
    const std::string utf8Bytes = bwtest::readFile((bwtest::dataDir() / "utf8/tux-utf8.ans").string());
    MainWindow w;
    QLabel* label = w.encodingLabel();
    REQUIRE(label);
    CHECK(label->objectName() == QStringLiteral("encodingLabel"));
    CHECK(label->text() == QStringLiteral("CP437"));
    QString error;
    REQUIRE(w.openFile(cp437Path, &error));
    CHECK(label->text() == QStringLiteral("CP437"));

    QTemporaryDir tmp;
    const QString u8 = tmp.filePath(QStringLiteral("tux-utf8.ans"));
    REQUIRE(w.saveFile(u8, &error, false, TextEncoding::Utf8));  // Save As UTF-8
    CHECK(label->text() == QStringLiteral("UTF-8"));
    REQUIRE(w.saveFile(u8, &error));  // Save
    CHECK(label->text() == QStringLiteral("UTF-8"));

    // a project saved from it keeps UTF-8 for its next .ans, opened again too
    const QString bwp = tmp.filePath(QStringLiteral("tux.bwp"));
    REQUIRE(w.saveFile(bwp, &error));
    CHECK(project::read(std::filesystem::path(bwp.toStdString())).ansEncoding == TextEncoding::Utf8);
    CHECK(label->text() == QStringLiteral("UTF-8"));
    MainWindow again;
    REQUIRE(again.openFile(bwp, &error));
    CHECK(again.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(again.encodingLabel()->text() == QStringLiteral("UTF-8"));
    // an export to a CP437 .ans leaves the project's encoding as it was
    again.document()->addLayer(LayerKind::Cells);
    const QString cp = tmp.filePath(QStringLiteral("export.ans"));
    REQUIRE(again.saveFile(cp, &error, false, TextEncoding::Cp437));
    REQUIRE(again.document()->form() == FileForm::ProjectZip);
    CHECK(again.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(again.encodingLabel()->text() == QStringLiteral("UTF-8"));
    CHECK(parseAnsi(fileBytes(cp)).encoding == TextEncoding::Cp437);
    // and one in the document's own encoding is the UTF-8 piece
    const QString own = tmp.filePath(QStringLiteral("own.ans"));
    REQUIRE(again.saveFile(own, &error));
    CHECK(fileBytes(own) == utf8Bytes);

    // Save As CP437 from the flat document, then New
    REQUIRE(w.openFile(u8, &error));
    REQUIRE(w.saveFile(tmp.filePath(QStringLiteral("back.ans")), &error, false, TextEncoding::Cp437));
    CHECK(label->text() == QStringLiteral("CP437"));
    REQUIRE(w.openFile(u8, &error));
    CHECK(label->text() == QStringLiteral("UTF-8"));
    w.document()->clear();
    CHECK(label->text() == QStringLiteral("CP437"));
}

TEST_CASE("the loss warning words a UTF-8 save's '?' cells as controls, and a CP437 one points to UTF-8",
          "[app][utf8]") {
    AnsiDocument d;
    d.canvas = Canvas(4, 1);
    d.canvas.at(0, 0).ch = U'★';  // ★: no CP437 code, fine in UTF-8
    d.canvas.at(1, 0).ch = U'\u0001';  // a C0 control: neither keeps it
    d.canvas.at(2, 0).ch = U'\u0085';  // a C1 control: neither keeps it
    d.canvas.at(3, 0).ch = U'○';  // ○: CP437 09, a TAB in .ans; fine in UTF-8

    const AnsiOutput u8 = writeAnsi(d, TextEncoding::Utf8);
    REQUIRE(u8.unwritable.size() == 2);
    CHECK(lossDescription(u8) ==
          QStringLiteral("2 cells hold control characters a UTF-8 .ans can't hold and will be written as '?': "
                         "U+0001 at 1,0; U+0085 at 2,0"));
    CHECK_FALSE(lossDescription(u8).contains(QStringLiteral("CP437")));
    CHECK_FALSE(saveWarning(u8).contains(QStringLiteral("UTF-8 .ans keeps")));
    d.canvas.at(2, 0).ch = U' ';
    CHECK(lossDescription(writeAnsi(d, TextEncoding::Utf8)) ==
          QStringLiteral("1 cell holds a control character a UTF-8 .ans can't hold and will be written as '?': "
                         "U+0001 at 1,0"));
    d.canvas.at(2, 0).ch = U'\u0085';

    // CP437 loses all four; UTF-8 would keep ★ and ○
    const AnsiOutput cp = writeAnsi(d, TextEncoding::Cp437);
    REQUIRE(cp.unwritable.size() == 4);
    const QString warning = saveWarning(cp);
    CHECK(warning.contains(QStringLiteral("glyph with no CP437 code")));
    CHECK(warning.contains(QStringLiteral(
        "A UTF-8 .ans keeps the glyphs of 2 of those cells: Save As and choose \"ANSI art, UTF-8 (*.ans)\".")));
    CHECK(warning.endsWith(QStringLiteral("\n\nSave anyway?")));
    // no hint for a project, an XBIN or BIN, or what UTF-8 can't keep either
    CHECK_FALSE(saveWarning(cp, true).contains(QStringLiteral("UTF-8")));
    CHECK_FALSE(saveWarning(cp, false, ArtFormat::Xbin).contains(QStringLiteral("UTF-8")));
    CHECK_FALSE(saveWarning(cp, false, ArtFormat::Bin).contains(QStringLiteral("UTF-8")));
    AnsiDocument controls;
    controls.canvas = Canvas(1, 1);
    controls.canvas.at(0, 0).ch = U'\u0001';
    CHECK_FALSE(saveWarning(writeAnsi(controls, TextEncoding::Cp437)).contains(QStringLiteral("UTF-8")));

    AnsiDocument star;
    star.canvas = Canvas(2, 1);
    star.canvas.at(0, 0).ch = U'★';
    CHECK(saveWarning(writeAnsi(star)).contains(QStringLiteral("A UTF-8 .ans keeps that cell's glyph: Save As")));
    star.canvas.at(1, 0).ch = U'☆';
    CHECK(saveWarning(writeAnsi(star)).contains(QStringLiteral("A UTF-8 .ans keeps all 2 of those glyphs: Save As")));
}

TEST_CASE("a CP437 save lossy only by palette or blink gets no UTF-8 hint", "[app][utf8]") {
    AnsiOutput palette;
    palette.encoding = TextEncoding::Cp437;
    palette.paletteDropped = true;
    CHECK(saveWarning(palette) ==
          QStringLiteral("Before saving as .ans:\nthe file's own palette is left out (only XBIN keeps one).\n\nSave anyway?"));
    AnsiOutput blink;
    blink.encoding = TextEncoding::Cp437;
    blink.blinkDropped = 2;
    CHECK_FALSE(saveWarning(blink).contains(QStringLiteral("UTF-8")));
    CHECK(saveWarning(blink) ==
          QStringLiteral("Before saving as .ans:\n2 cells lost their blink when the file was opened with iCE colours, "
                         "and this save doesn't put it back.\n\nSave anyway?"));

    // the XBIN with its own palette and font, opened in the app and saved as a CP437 .ans
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(QString::fromStdString((bwtest::dataDir() / "xbin/tux-palette-font.xb").string()), &error));
    const AnsiOutput out = w.document()->encode(ArtFormat::Ansi);
    REQUIRE(out.encoding == TextEncoding::Cp437);
    REQUIRE(out.unwritable.empty());
    CHECK(saveWarning(out) == QStringLiteral("Before saving as .ans:\nthe file's own palette is left out (only XBIN "
                                             "keeps one);\nthe file's own font is left out (only XBIN keeps one)."
                                             "\n\nSave anyway?"));
}

TEST_CASE("the UTF-8 hint is its own paragraph, for glyphs all kept, some kept and beside other losses",
          "[app][utf8]") {
    const QString hintEnd = QStringLiteral(": Save As and choose \"ANSI art, UTF-8 (*.ans)\".\n\nSave anyway?");
    AnsiDocument d;
    d.canvas = Canvas(2, 1);
    d.canvas.at(0, 0).ch = U'★';  // no CP437 code, fine in UTF-8
    CHECK(saveWarning(writeAnsi(d)) ==
          QStringLiteral("Before saving as .ans:\n1 cell holds a glyph with no CP437 code and will be written as "
                         "'?': ★ at 0,0.\n\nA UTF-8 .ans keeps that cell's glyph") + hintEnd);
    d.canvas.at(1, 0).ch = U'☆';
    CHECK(saveWarning(writeAnsi(d)) ==
          QStringLiteral("Before saving as .ans:\n2 cells hold a glyph with no CP437 code and will be written as "
                         "'?': ★ at 0,0; ☆ at 1,0.\n\nA UTF-8 .ans keeps all 2 of those glyphs") + hintEnd);

    // a C1 control: no CP437 code, and no UTF-8 .ans holds it either
    d.canvas.at(1, 0).ch = U'\u0085';
    AnsiOutput some = writeAnsi(d);
    REQUIRE(some.unwritable.size() == 2);
    CHECK(saveWarning(some) ==
          QStringLiteral("Before saving as .ans:\n2 cells hold a glyph with no CP437 code and will be written as "
                         "'?': ★ at 0,0; U+0085 at 1,0.\n\nA UTF-8 .ans keeps the glyph of 1 of those cells") +
              hintEnd);
    // beside a loss UTF-8 doesn't help with, the list ends first
    some.paletteDropped = true;
    some.blinkDropped = 1;
    CHECK(saveWarning(some) ==
          QStringLiteral("Before saving as .ans:\n2 cells hold a glyph with no CP437 code and will be written as "
                         "'?': ★ at 0,0; U+0085 at 1,0;\n1 cell lost its blink when the file was opened with iCE colours, and this save "
                         "doesn't put it back;\nthe file's own palette is left out (only XBIN keeps one)."
                         "\n\nA UTF-8 .ans keeps the glyph of 1 of those cells") + hintEnd);
}

TEST_CASE("Reopen with Encoding with unsaved edits: Cancel keeps the document, its edits and its encoding",
          "[app][utf8]") {
    const QString utf8Path = QString::fromStdString((bwtest::dataDir() / "utf8/tux-utf8.ans").string());
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(utf8Path, &error));
    REQUIRE(w.document()->textEncoding() == TextEncoding::Utf8);
    Cell x = w.document()->canvas().at(0, 0);
    x.ch = U'★';
    put(w.document(), 0, 0, x);
    REQUIRE(w.document()->isModified());
    const Canvas edited = w.document()->canvas();
    const int undo = w.document()->undoStack()->count();

    bool asked = false;
    answer(&w, QMessageBox::Cancel, &asked);
    action(w, "reopenCp437")->trigger();
    CHECK(asked);
    CHECK(w.document()->path() == utf8Path);
    CHECK(w.document()->canvas() == edited);
    CHECK(w.document()->isModified());
    CHECK(w.document()->undoStack()->count() == undo);
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(w.encodingLabel()->text() == QStringLiteral("UTF-8"));

    // Discard drops the edit and reads the file again in the encoding picked
    answer(&w, QMessageBox::Discard, &asked);
    action(w, "reopenCp437")->trigger();
    CHECK(asked);
    CHECK(w.document()->textEncoding() == TextEncoding::Cp437);
    CHECK_FALSE(w.document()->isModified());
    CHECK(w.document()->canvas().at(0, 0).ch != U'★');
}
