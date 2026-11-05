// Format > Target, the 99-swatch palette and what an IRC document saves
// (#350).

#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "irc_meter.h"
#include "main_window.h"
#include "palette_widget.h"
#include "preferences.h"
#include "settings.h"

#include "blockwright/irc_view.h"
#include "blockwright/target.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QUndoStack>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

void click(QWidget* w, QPoint at, Qt::MouseButton button) {
    const QPointF global = w->mapToGlobal(QPointF(at));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), global, button, button, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), global, button, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

// Restores the default target setting when a test is done with it.
struct DefaultTarget {
    Target was = defaultTargetSetting();
    ~DefaultTarget() { setDefaultTargetSetting(was); }
};

} // namespace

TEST_CASE("Format > Target sets the target as one undo step", "[app][target]") {
    MainWindow w;
    Document* doc = w.document();
    REQUIRE(doc->target() == Target::Ansi);
    REQUIRE(doc->flatHoldsAll());
    QAction* ansi = act(w, "targetAnsi");
    QAction* irc = act(w, "targetIrc");
    CHECK(ansi->isChecked());
    CHECK_FALSE(irc->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("ANSI"));
    CHECK(w.paletteWidget()->target() == Target::Ansi);
    const Canvas before = doc->canvas();

    irc->trigger();
    CHECK(doc->target() == Target::Irc);
    CHECK(doc->isModified());
    CHECK(doc->undoStack()->undoText() == QStringLiteral("Target"));
    CHECK(irc->isChecked());
    CHECK_FALSE(ansi->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("IRC"));
    CHECK(w.paletteWidget()->target() == Target::Irc);
    // an .ans keeps no target, so a flat save doesn't hold it all
    CHECK_FALSE(doc->flatHoldsAll());
    // the art is untouched
    CHECK(doc->canvas() == before);
    // the same target again is no step
    const int steps = doc->undoStack()->count();
    CHECK_FALSE(doc->setTarget(Target::Irc));
    CHECK(doc->undoStack()->count() == steps);

    doc->undoStack()->undo();
    CHECK(doc->target() == Target::Ansi);
    CHECK(ansi->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("ANSI"));
    CHECK(w.paletteWidget()->target() == Target::Ansi);
    CHECK(doc->flatHoldsAll());
    doc->undoStack()->redo();
    CHECK(doc->target() == Target::Irc);
    CHECK(w.targetLabel()->text() == QStringLiteral("IRC"));
}

TEST_CASE("a new IRC document writes its .ans as UTF-8", "[app][target]") {
    Document doc;
    doc.clear(10, 4, true, Target::Irc);
    CHECK(doc.target() == Target::Irc);
    CHECK(doc.textEncoding() == TextEncoding::Utf8);
    CHECK_FALSE(doc.isModified());
    doc.clear(10, 4, true);
    CHECK(doc.target() == Target::Ansi);
    CHECK(doc.textEncoding() == TextEncoding::Cp437);
}

TEST_CASE("File > New starts on the target Preferences chose", "[app][target]") {
    DefaultTarget restore;
    setDefaultTargetSetting(Target::Ansi);
    {
        PreferencesDialog d(fontsFolderSetting());
        CHECK(d.defaultTargetBox()->objectName() == QStringLiteral("defaultTarget"));
        CHECK(d.defaultTarget() == Target::Ansi);
        d.defaultTargetBox()->setCurrentIndex(d.defaultTargetBox()->findData(int(Target::Irc)));
        CHECK(d.defaultTarget() == Target::Irc);
    }
    setDefaultTargetSetting(Target::Irc);
    CHECK(defaultTargetSetting() == Target::Irc);
    {
        PreferencesDialog d(fontsFolderSetting());
        CHECK(d.defaultTarget() == Target::Irc);
    }
    // a window opens on a new document of that target
    MainWindow w;
    CHECK(w.document()->target() == Target::Irc);
    CHECK(w.targetLabel()->text() == QStringLiteral("IRC"));
    CHECK_FALSE(w.document()->isModified());
    QString error;
    REQUIRE(w.newCanvas(QSize(20, 5), true, &error));
    CHECK(w.document()->target() == Target::Irc);
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
    CHECK(w.paletteWidget()->target() == Target::Irc);

    setDefaultTargetSetting(Target::Ansi);
    REQUIRE(w.newCanvas(QSize(20, 5), true, &error));
    CHECK(w.document()->target() == Target::Ansi);
    CHECK(w.paletteWidget()->target() == Target::Ansi);
}

TEST_CASE("an opened .ans is an ANSI document whatever new ones are", "[app][target]") {
    DefaultTarget restore;
    setDefaultTargetSetting(Target::Irc);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(QString::fromStdString((bwtest::dataDir() / "golden" / "tux.ans").string()), &error));
    CHECK(w.document()->target() == Target::Ansi);
    CHECK(w.targetLabel()->text() == QStringLiteral("ANSI"));
}

TEST_CASE("under IRC the palette has 99 swatches, and a click picks mIRC's RGB", "[app][target]") {
    MainWindow w;
    w.show();
    PaletteWidget* p = w.paletteWidget();
    CHECK(p->swatchRect(15).isValid());
    CHECK_FALSE(p->swatchRect(16).isValid());
    CHECK(p->indexAt(p->swatchRect(15).center()) == 15);
    const QSize ansiHint = p->sizeHint();

    w.document()->setTarget(Target::Irc);
    REQUIRE(p->target() == Target::Irc);
    CHECK(p->sizeHint().height() > ansiHint.height());
    for (int i : {0, 15, 16, 52, 77, 98}) {
        INFO("swatch " << i);
        const QRect r = p->swatchRect(i);
        REQUIRE(r.isValid());
        CHECK(p->indexAt(r.center()) == i);
    }
    CHECK_FALSE(p->swatchRect(99).isValid());
    p->resize(p->sizeHint());

    click(p, p->swatchRect(77).center(), Qt::LeftButton);
    CHECK(w.canvasView()->pen().fg == swatchColor(77));
    CHECK_FALSE(w.canvasView()->pen().fg.isIndexed());
    click(p, p->swatchRect(20).center(), Qt::RightButton);
    CHECK(w.canvasView()->pen().bg == swatchColor(20));
    click(p, p->swatchRect(3).center(), Qt::LeftButton);
    CHECK(w.canvasView()->pen().fg == Color::indexed(3));
}

TEST_CASE("Ctrl+arrows step through 99 colours under IRC and 16 under ANSI", "[app][target]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QAction* next = act(w, "nextForeground");
    QAction* prevBg = act(w, "previousBackground");

    v->setForeground(15);
    next->trigger();
    CHECK(v->pen().fg == Color::indexed(0));  // ANSI wraps at 16

    w.document()->setTarget(Target::Irc);
    v->setForeground(15);
    next->trigger();
    CHECK(v->pen().fg == swatchColor(16));
    v->setForeground(98);
    next->trigger();
    CHECK(v->pen().fg == Color::indexed(0));  // IRC wraps at 99
    v->setBackground(0);
    prevBg->trigger();
    CHECK(v->pen().bg == swatchColor(98));
    prevBg->trigger();
    CHECK(v->pen().bg == swatchColor(97));

    // back under ANSI an RGB pen steps from the palette entry nearest it
    w.document()->setTarget(Target::Ansi);
    v->setForeground(77);
    const int from = swatchOf(v->pen().fg, Target::Ansi);
    next->trigger();
    CHECK(v->pen().fg == Color::indexed(std::uint8_t((from + 1) % 16)));
}

TEST_CASE("Save names the cells whose colour an .ans can't hold", "[app][target]") {
    AnsiOutput out;
    out.coloursQuantised = 1;
    CHECK(out.lossy());
    CHECK(lossDescription(out) ==
          QStringLiteral("1 cell is in a colour the file can't hold, saved as the nearest of the 16"));
    out.coloursQuantised = 3;
    CHECK(lossDescription(out) ==
          QStringLiteral("3 cells are in colours the file can't hold, saved as the nearest of the 16"));

    // a document with an mIRC colour isn't saved as .ans unless told to
    MainWindow w;
    AnsiDocument d;
    d.canvas = Canvas(4, 2);
    d.canvas.at(0, 0).ch = U'X';
    d.canvas.at(0, 0).fg = swatchColor(77);
    w.document()->replace(d, QString());
    w.document()->setTarget(Target::Irc);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("rgb.ans"));
    QString error;
    CHECK_FALSE(w.saveFile(path, &error));
    CHECK(error.contains(QStringLiteral("nearest of the 16")));
    CHECK(w.saveFile(path, &error, true));
}

namespace {

void key(QWidget* w, int k, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &release);
}

void type(QWidget* w, const QString& text) {
    for (const QChar c : text)
        key(w, c.toUpper().unicode(), QString(c));
}

} // namespace

TEST_CASE("a word typed bold, italic or underlined exports with its codes, at the bytes the meter shows",
          "[app][target][attrs]") {
    // #350 slice 3b
    QTemporaryDir dir;
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    REQUIRE(doc->setTarget(Target::Irc));
    const struct {
        const char* action;
        std::uint8_t bit;
        QString word;
    } words[] = {{"formatBold", attr::Bold, QStringLiteral("bold")},
                 {"formatItalic", attr::Italic, QStringLiteral("italic")},
                 {"formatUnderline", attr::Underline, QStringLiteral("under")}};
    // one row each: "x ", the word in its formatting, " x"
    for (const auto& r : words) {
        type(v, QStringLiteral("x "));
        act(w, r.action)->trigger();
        REQUIRE(v->pen().attrs == r.bit);
        type(v, r.word);
        act(w, r.action)->trigger();
        REQUIRE(v->pen().attrs == 0);
        type(v, QStringLiteral(" x"));
        key(v, Qt::Key_Return);
    }
    for (int y = 0; y < 3; ++y) {
        INFO("row " << y);
        const int n = int(words[y].word.size());
        for (int x = 0; x < n + 4; ++x) {
            INFO("column " << x);
            const bool inWord = x >= 2 && x < 2 + n;
            CHECK(doc->canvas().at(x, y).attrs == (inWord ? words[y].bit : 0));
        }
    }

    const QString path = dir.filePath(QStringLiteral("words.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error));
    const std::string text = bwtest::readFile(path.toStdString());
    std::vector<std::string> lines;
    for (std::size_t at = 0, end; (end = text.find('\n', at)) != std::string::npos; at = end + 1)
        lines.push_back(text.substr(at, end - at));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "\x03" "15,1x \x02" "bold\x02 x");
    CHECK(lines[1] == "\x03" "15,1x \x1d" "italic\x1d x");
    CHECK(lines[2] == "\x03" "15,1x \x1f" "under\x1f x");
    for (int y = 0; y < 3; ++y) {
        INFO("row " << y);
        CHECK(w.ircMeter()->bytes(y) == lines[std::size_t(y)].size());
        // and the IRC view reads the formatting back
        const std::vector<irc::ShownCell> shown = irc::decodeLine(lines[std::size_t(y)]);
        for (std::size_t x = 0; x < shown.size(); ++x) {
            INFO("column " << x);
            CHECK(shown[x].cell.attrs == doc->canvas().at(int(x), y).attrs);
        }
    }
}

TEST_CASE("typing writes the pen's formatting under IRC on a cells layer only", "[app][target][attrs]") {
    // #350 slice 3b: as the glyph brush does (GlyphTool::makeBrush)
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    REQUIRE(doc->setTarget(Target::Irc));
    const std::uint8_t iu = attr::Italic | attr::Underline;
    v->setPen(Pen{Color::indexed(LightGreen), Color::indexed(Blue), iu});

    // a key, an F-key glyph and an input-method commit
    type(v, QStringLiteral("k"));
    key(v, Qt::Key_F1);
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(v, &commit);
    for (int x = 0; x < 3; ++x) {
        INFO("column " << x);
        CHECK(doc->canvas().at(x, 0).attrs == iu);
        CHECK(doc->canvas().at(x, 0).fg == Color::indexed(LightGreen));
    }
    CHECK(doc->canvas().at(0, 0).ch == U'k');
    CHECK(doc->canvas().at(2, 0).ch == U'é');
    // Backspace clears to a blank without formatting
    key(v, Qt::Key_Backspace);
    CHECK(doc->canvas().at(2, 0) == Cell{});
    // undone, the typing goes with its formatting
    doc->undoStack()->undo();
    CHECK(doc->canvas().at(2, 0).attrs == iu);
    doc->undoStack()->undo();
    CHECK(doc->canvas().at(2, 0) == Cell{});

    // a Pixels layer holds no formatting: a typed block is written without it
    const LayerId cells = doc->activeLayer();
    act(w, "newPixelLayer")->trigger();
    REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
    v->setCellCursor({6, 0});
    type(v, QStringLiteral("█"));
    CHECK(doc->active()->cells.at(6, 0).ch == U'█');
    CHECK(doc->active()->cells.at(6, 0).attrs == 0);
    REQUIRE(doc->setActiveLayer(cells));

    // under ANSI the pen keeps its formatting but typing writes none
    REQUIRE(doc->setTarget(Target::Ansi));
    CHECK(v->pen().attrs == iu);
    v->setCellCursor({0, 1});
    type(v, QStringLiteral("k"));
    key(v, Qt::Key_F1);
    QInputMethodEvent ansiCommit;
    ansiCommit.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(v, &ansiCommit);
    for (int x = 0; x < 3; ++x) {
        INFO("column " << x);
        CHECK(doc->canvas().at(x, 1).attrs == 0);
        CHECK(doc->canvas().at(x, 1).fg == Color::indexed(LightGreen));
    }
}
