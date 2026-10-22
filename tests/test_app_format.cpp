// The GUI's document format, headless: File > SAUCE..., the iCE colours and
// 9 px letter-spacing toggles, their undo, and their round trip through
// save and open. main() is in test_app.cpp.

#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "sauce_dialog.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QUndoStack>

#include <functional>
#include <map>

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
        // #36: trailing blinking black spaces, trimmed by both
        {"\x1b[0;5;31;44mA\x1b[0;37;104mB\x1b[0;5;30;40m  ", 0, kPalette[Blue], 3},
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

TEST_CASE("the save warning for blink lost on open stays true once iCE is off or the cell is erased") {
    // #36: the count is fixed when the file opens; neither turning iCE off
    // nor erasing the cell gives the blink back, and the warning says what
    // happened rather than what the save will do
    const QString lost = QStringLiteral("1 blinking cell(s) of the original file lost their blink when it was "
                                        "opened with iCE colours");
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
    }
    SECTION("the blinking cell erased") {
        const Cell a = w.document()->canvas().at(0, 0);
        REQUIRE(w.document()->edit(CellEdit{{{0, 0, a, Cell{}}}}));
        CHECK(w.document()->encode().blinkDropped == 1);
    }
    CHECK_FALSE(w.saveFile(saved, &error));
    CHECK(error == lost);
    CHECK_FALSE(error.contains(QStringLiteral("will lose")));
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
    for (QAction* a : w.findChildren<QAction*>()) {
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
