// Format > Target, the 99-swatch palette and what an IRC document saves
// (#350).

#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "glyph_bar.h"
#include "irc_meter.h"
#include "main_window.h"
#include "palette_widget.h"
#include "preferences.h"
#include "settings.h"

#include "blockwright/glyph_sets.h"
#include "blockwright/irc.h"
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
#include <QStatusBar>
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

// A key, or with `text` and no key the key that types it.
void press(QWidget* w, int k, const QString& text = {}) {
    if (!k)
        k = int(text.toUcs4().front());
    QKeyEvent down(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &up);
}

QStringList visibleGlyphSets(MainWindow& w) {
    QStringList sets;
    for (QAction* a : w.findChildren<QAction*>())
        if (a->objectName().startsWith(QStringLiteral("glyphSet")) && a->isVisible())
            sets << a->objectName();
    return sets;
}

// Restores the stored glyph set when a test is done with it.
struct StoredGlyphSet {
    QVariant was = appSettings()->value(QStringLiteral("glyphSet/index"));
    ~StoredGlyphSet() { appSettings()->setValue(QStringLiteral("glyphSet/index"), was); }
};

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

namespace {

// A 1x2 IRC canvas of A in mIRC 0 on 1, row 1 with `attrs` (#371).
AnsiDocument formattedPair(std::uint8_t attrs) {
    AnsiDocument d;
    d.canvas = Canvas(1, 2);
    for (int y = 0; y < 2; ++y) {
        Cell& c = d.canvas.at(0, y);
        c.ch = U'A';
        c.fg = Color::indexed(White);
        c.bg = Color::indexed(Black);
    }
    d.canvas.at(0, 1).attrs = attrs;
    return d;
}

constexpr std::uint8_t kBiu = attr::Bold | attr::Italic | attr::Underline;

} // namespace

TEST_CASE("the IRC meter measures rows that differ only in formatting apart", "[app][target][attrs]") {
    // #371: the meter's line cache was keyed without the cells' formatting
    MainWindow w;
    w.document()->replace(formattedPair(kBiu), QString());
    REQUIRE(w.document()->setTarget(Target::Irc));
    IrcMeter* m = w.ircMeter();
    CHECK(m->bytes(0) == 5);  // ^C0,1A
    CHECK(m->bytes(1) == 8);  // ^C0,1^B^]^_A
}

TEST_CASE("the IRC meter re-measures a row whose formatting changes after it was read", "[app][target][attrs]") {
    // #371
    MainWindow w;
    Document* doc = w.document();
    doc->replace(formattedPair(0), QString());
    REQUIRE(doc->setTarget(Target::Irc));
    IrcMeter* m = w.ircMeter();
    REQUIRE(m->bytes(0) == 5);
    REQUIRE(m->bytes(1) == 5);
    Cell after = doc->canvas().at(0, 1);
    after.attrs = kBiu;
    CellEdit e;
    e.changes.push_back({0, 1, doc->canvas().at(0, 1), after});
    REQUIRE(doc->edit(e));
    CHECK(m->bytes(1) == 8);
    CHECK(m->bytes(0) == 5);
    doc->undoStack()->undo();
    CHECK(m->bytes(1) == 5);
}

TEST_CASE("Export IRC Text writes a formatted row's toggles after a plain twin", "[app][target][attrs]") {
    // #371: Export writes the meter's lines, so the cache's key reached the .txt
    QTemporaryDir dir;
    MainWindow w;
    w.document()->replace(formattedPair(kBiu), QString());
    REQUIRE(w.document()->setTarget(Target::Irc));
    const QString path = dir.filePath(QStringLiteral("pair.txt"));
    QString error;
    REQUIRE(w.exportIrcText(path, &error));
    CHECK(bwtest::readFile(path.toStdString()) == "\x03" "0,1A\n"
                                                   "\x03" "0,1\x02\x1d\x1f" "A\n");
}

TEST_CASE("an IRC document with a CP437 .ans types and draws ▚ and ★ (#350 slice 2)", "[app][target]") {
    StoredGlyphSet restore;
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->target() == Target::Ansi);
    REQUIRE(doc->textEncoding() == TextEncoding::Cp437);
    CHECK(v->typingPolicy() == TypingPolicy::Cp437);
    CHECK(visibleGlyphSets(w).size() == kCp437GlyphSetCount);
    CHECK(w.glyphBar()->setBox()->count() == kCp437GlyphSetCount);

    // Format > Target > IRC keeps the CP437 .ans, and offers the Unicode sets
    act(w, "targetIrc")->trigger();
    REQUIRE(doc->target() == Target::Irc);
    REQUIRE(doc->textEncoding() == TextEncoding::Cp437);
    CHECK(v->typingPolicy() == TypingPolicy::Unicode);
    CHECK(visibleGlyphSets(w).size() == kGlyphSetCount);
    CHECK(w.glyphBar()->setBox()->count() == kGlyphSetCount);

    // the quadrant set from the Glyphs menu; F5 is ▚
    act(w, "glyphSet17")->trigger();
    CHECK(v->glyphSet() == 16);
    CHECK(w.glyphSetLabel()->text().startsWith(QStringLiteral("F-keys set 17: ▖")));
    CHECK(w.glyphBar()->setBox()->currentData().toInt() == 16);
    press(v, Qt::Key_F5);
    CHECK(doc->canvas().at(0, 0).ch == U'▚');
    // the shapes set by stepping on from 17; F9 is ★
    act(w, "nextGlyphSet")->trigger();
    act(w, "nextGlyphSet")->trigger();
    act(w, "nextGlyphSet")->trigger();
    CHECK(v->glyphSet() == 19);
    press(v, Qt::Key_F9);
    CHECK(doc->canvas().at(1, 0).ch == U'★');
    // and a typed ★, as a key's text
    press(v, 0, QStringLiteral("★"));
    CHECK(doc->canvas().at(2, 0).ch == U'★');
    CHECK(v->cellCursor() == Cursor{3, 0});
    // stepping wraps over all 20
    act(w, "nextGlyphSet")->trigger();
    CHECK(v->glyphSet() == 0);
    act(w, "previousGlyphSet")->trigger();
    CHECK(v->glyphSet() == kGlyphSetCount - 1);

    // export writes their UTF-8 bytes
    const std::vector<std::string> lines = irc::exportCanvas(irc::sentCanvas(doc->canvas(), true, Target::Irc));
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front().find("\xE2\x96\x9A\xE2\x98\x85\xE2\x98\x85") != std::string::npos);

    // a CP437 .ans lists them as glyphs with no CP437 code, as before
    const AnsiOutput out = doc->encode(ArtFormat::Ansi, TextEncoding::Cp437);
    CHECK(lossDescription(out) ==
          QStringLiteral("3 cells hold a glyph with no CP437 code and will be written as '?': "
                         "▚ at 0,0; ★ in 2 cells, first at 1,0"));

    // back to ANSI: set 20 reads as the default, and comes back under IRC
    doc->undoStack()->undo();  // the typed ★
    doc->undoStack()->undo();  // F9
    doc->undoStack()->undo();  // F5
    doc->undoStack()->undo();  // the target
    REQUIRE(doc->target() == Target::Ansi);
    CHECK(v->typingPolicy() == TypingPolicy::Cp437);
    CHECK(v->glyphSet() == kDefaultGlyphSet);
    CHECK(act(w, QStringLiteral("glyphSet%1").arg(kDefaultGlyphSet + 1).toLatin1().constData())->isChecked());
    CHECK(visibleGlyphSets(w).size() == kCp437GlyphSetCount);
    CHECK(w.glyphBar()->setBox()->count() == kCp437GlyphSetCount);
    CHECK(w.glyphBar()->setBox()->currentData().toInt() == kDefaultGlyphSet);
    CHECK(appSettings()->value(QStringLiteral("glyphSet/index")).toInt() == kGlyphSetCount - 1);
    // stepping stays in Moebius's 16
    act(w, "previousGlyphSet")->trigger();
    CHECK(v->glyphSet() == kDefaultGlyphSet - 1);
    act(w, "glyphSet1")->trigger();
    act(w, "previousGlyphSet")->trigger();
    CHECK(v->glyphSet() == kCp437GlyphSetCount - 1);
    doc->undoStack()->redo();
    CHECK(v->glyphSet() == kCp437GlyphSetCount - 1);
}

TEST_CASE("an ANSI document with a CP437 .ans refuses ▚ and ★, naming the target (#350 slice 2)", "[app][target]") {
    StoredGlyphSet restore;
    appSettings()->setValue(QStringLiteral("glyphSet/index"), 16);
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->target() == Target::Ansi);
    // a stored Unicode set reads as set 6; F5 types its ▀, not ▚
    CHECK(v->glyphSet() == kDefaultGlyphSet);
    press(v, Qt::Key_F5);
    CHECK(doc->canvas().at(0, 0).ch == glyphSetGlyph(kDefaultGlyphSet, 4));
    QStringList rejected;
    QObject::connect(v, &CanvasView::glyphRejected, [&](const QString& t) { rejected << t; });
    press(v, 0, QStringLiteral("★"));
    CHECK(rejected == QStringList{QStringLiteral("★")});
    CHECK(doc->canvas().at(1, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{1, 0});
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("★ has no CP437 code; not typed (target: ANSI, CP437)"));
    press(v, 0, QStringLiteral("▚"));
    CHECK(doc->canvas().at(1, 0) == Cell{});
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("▚ has no CP437 code; not typed (target: ANSI, CP437)"));

    // a UTF-8 .ans takes them under ANSI too
    Document utf8;
    utf8.clear(10, 4, true, Target::Irc);
    REQUIRE(utf8.textEncoding() == TextEncoding::Utf8);
    CHECK(typingPolicy(Target::Ansi, utf8.textEncoding()) == TypingPolicy::Unicode);
}

TEST_CASE("the refusal names the target and encoding, under either policy (#350 slice 2)", "[app][target]") {
    CHECK(rejectedGlyphsMessage(QStringLiteral("★"), Target::Ansi, TextEncoding::Cp437) ==
          QStringLiteral("★ has no CP437 code; not typed (target: ANSI, CP437)"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("中"), Target::Ansi, TextEncoding::Cp437) ==
          QStringLiteral("中 has no CP437 code; not typed (target: ANSI, CP437)"));
    // under Unicode, #334 slice 8's reasons: a control, worded for IRC text
    // under IRC, and a cluster past 16 code points
    CHECK(rejectedGlyphsMessage(QString(QChar(0x2028)), Target::Irc, TextEncoding::Cp437) ==
          QStringLiteral("U+2028 can't be held in IRC text; not typed (target: IRC, CP437)"));
    CHECK(rejectedGlyphsMessage(QString(QChar(0x0085)) + QChar(0x2028), Target::Irc, TextEncoding::Utf8) ==
          QStringLiteral("U+0085 U+2028 can't be held in IRC text; not typed (target: IRC, UTF-8)"));
    CHECK(rejectedGlyphsMessage(QString(QChar(0x2028)), Target::Ansi, TextEncoding::Utf8) ==
          QStringLiteral("U+2028 can't be held in a UTF-8 .ans; not typed (target: ANSI, UTF-8)"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("q") + QString(16, QChar(0x0302)), Target::Irc, TextEncoding::Cp437)
              .endsWith(QStringLiteral(" holds more than 16 code points; not typed (target: IRC, CP437)")));
}

TEST_CASE("a wide glyph types as a pair under IRC, as in a UTF-8 document (#350 slice 2)", "[app][target]") {
    StoredGlyphSet restore;
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    // ANSI with a CP437 .ans refuses it, as #334 slice 8 does
    press(v, 0, QStringLiteral("中"));
    CHECK(doc->canvas().at(0, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{0, 0});
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("中 has no CP437 code; not typed (target: ANSI, CP437)"));

    // under IRC, still saved as a CP437 .ans, it is typed as a pair
    act(w, "targetIrc")->trigger();
    REQUIRE(doc->textEncoding() == TextEncoding::Cp437);
    press(v, 0, QStringLiteral("中"));
    CHECK(doc->canvas().at(0, 0).ch == U'中');
    CHECK(doc->canvas().at(1, 0).ch.isContinuation());
    CHECK(v->cellCursor() == Cursor{2, 0});
    // and an F-key Unicode set glyph after it: set 17's F1 is ▖
    act(w, "glyphSet17")->trigger();
    press(v, Qt::Key_F1);
    CHECK(doc->canvas().at(2, 0).ch == U'▖');
    CHECK(v->cellCursor() == Cursor{3, 0});
    // IRC text writes 中 whole, then ▖
    const std::vector<std::string> lines = irc::exportCanvas(irc::sentCanvas(doc->canvas(), true, Target::Irc));
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front().find("\xE4\xB8\xAD\xE2\x96\x96") != std::string::npos);
    // a control is refused, worded for IRC text
    QInputMethodEvent separator;
    separator.setCommitString(QString(QChar(0x2028)));
    QApplication::sendEvent(v, &separator);
    CHECK(doc->canvas().at(3, 0) == Cell{});
    CHECK(w.statusBar()->currentMessage() ==
          QStringLiteral("U+2028 can't be held in IRC text; not typed (target: IRC, CP437)"));
}

TEST_CASE("the IRC meter tells Unicode glyphs of one byte length apart (#350 slice 2)", "[app][target]") {
    // ▚ and ★ are both three bytes in IRC text: the line cache (#371) has
    // to key on the glyph, not its size, or one row takes the other's line
    AnsiDocument d = formattedPair(0);
    d.canvas.at(0, 0).ch = U'▚';
    d.canvas.at(0, 1).ch = U'★';
    MainWindow w;
    Document* doc = w.document();
    doc->replace(d, QString());
    REQUIRE(doc->setTarget(Target::Irc));
    IrcMeter* m = w.ircMeter();
    CHECK(m->bytes(0) == 7);  // ^C0,1 ▚
    CHECK(m->bytes(1) == 7);
    std::vector<irc::Line> lines = m->lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "\x03" "0,1\xE2\x96\x9A");
    CHECK(lines[1].text == "\x03" "0,1\xE2\x98\x85");
    // row 1 made ▚ takes row 0's line; made ★ again, its own
    Cell after = doc->canvas().at(0, 1);
    after.ch = U'▚';
    CellEdit e;
    e.changes.push_back({0, 1, doc->canvas().at(0, 1), after});
    REQUIRE(doc->edit(e));
    lines = m->lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].text == lines[0].text);
    doc->undoStack()->undo();
    lines = m->lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].text == "\x03" "0,1\xE2\x98\x85");
}
