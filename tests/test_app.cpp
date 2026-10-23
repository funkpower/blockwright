// The GUI, headless (QT_QPA_PLATFORM=offscreen): the document, open/save
// round trips, zoom, the preview dock, undo/redo, F-key glyph sets, the
// palette, and an open-type-save smoke test.

#include "util.h"

#include "blockwright/cp437.h"
#include "blockwright/glyph_sets.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "palette_widget.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QUndoStack>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>

#include <algorithm>
#include <map>
#include <sstream>

using namespace bw;
namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& p) { return QString::fromStdString(p.string()); }

fs::path goldenDir() { return bwtest::dataDir() / "golden"; }

std::vector<fs::path> pieces() {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(goldenDir()))
        if (e.path().extension() == ".ans")
            out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::map<std::string, std::string> nonCanonical() {
    std::map<std::string, std::string> out;
    std::istringstream in(bwtest::readFile(goldenDir() / "NONCANONICAL"));
    std::string line;
    while (std::getline(in, line))
        if (!line.empty() && line[0] != '#')
            out[line.substr(0, line.find(' '))] = line;
    return out;
}

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

QAction* action(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

} // namespace

TEST_CASE("a new document is a blank 80x40 canvas") {
    Document d;
    CHECK(d.canvas().width() == 80);
    CHECK(d.canvas().height() == 40);
    CHECK(d.path().isEmpty());
    CHECK_FALSE(d.isModified());
}

TEST_CASE("a document reports exactly the cells an edit changes") {
    Document d;
    std::vector<QRegion> seen;
    QObject::connect(&d, &Document::cellsChanged, [&](const QRegion& r) { seen.push_back(r); });

    CellEdit e;
    e.changes.push_back({4, 2, Cell{}, Cell{U'#'}});
    e.changes.push_back({5, 2, Cell{}, Cell{}});      // no change
    e.changes.push_back({9, 7, Cell{}, Cell{U'x'}});
    e.changes.push_back({200, 2, Cell{}, Cell{U'x'}});  // off the canvas
    CHECK(Document::changedCells(e) == (QRegion(4, 2, 1, 1) | QRegion(9, 7, 1, 1) | QRegion(200, 2, 1, 1)));
    CHECK(d.edit(e));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == (QRegion(4, 2, 1, 1) | QRegion(9, 7, 1, 1)));
    CHECK(d.canvas().at(4, 2).ch == U'#');
    CHECK(d.isModified());
    CHECK(d.undoStack()->count() == 1);

    d.undoStack()->undo();
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == seen[0]);
    CHECK(d.canvas() == Canvas(80, 40));
    CHECK_FALSE(d.isModified());

    // an edit that changes nothing reports nothing and is no undo step
    CellEdit none;
    none.changes.push_back({1, 1, Cell{}, Cell{}});
    none.changes.push_back({300, 1, Cell{}, Cell{U'x'}});
    CHECK_FALSE(d.edit(none));
    CHECK(seen.size() == 2);
    CHECK(d.undoStack()->count() == 1);
    CHECK_FALSE(d.isModified());
}

TEST_CASE("open then save of an unmodified golden piece") {
    const auto nc = nonCanonical();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    for (const auto& path : pieces()) {
        const std::string name = path.filename().string();
        DYNAMIC_SECTION(name) {
            Document d;
            QString error;
            REQUIRE(d.load(qpath(path), &error));
            CHECK(d.path() == qpath(path));
            const QString out = tmp.filePath(QString::fromStdString(name));
            const AnsiOutput enc = d.encode();
            CHECK_FALSE(enc.lossy());
            REQUIRE(d.save(out, enc, &error));
            CHECK(d.path() == out);
            const std::string src = bwtest::readFile(path), saved = bwtest::readFile(out.toStdString());
            if (!nc.count(name)) {
                CHECK(saved == src);  // byte for byte, SAUCE included
            } else {
                // the writer's canonical form: a second open/save gives it back
                Document again;
                REQUIRE(again.load(out, &error));
                CHECK(again.canvas() == d.canvas());
                const QString out2 = out + QStringLiteral(".2");
                REQUIRE(again.save(out2, again.encode(), &error));
                CHECK(bwtest::readFile(out2.toStdString()) == saved);
            }
            // SAUCE survives either way
            const AnsiDocument reread = parseAnsi(saved);
            const AnsiDocument orig = parseAnsi(src);
            REQUIRE(reread.sauce.has_value() == orig.sauce.has_value());
            if (orig.sauce) {
                CHECK(reread.sauce->title == orig.sauce->title);
                CHECK(reread.sauce->author == orig.sauce->author);
                CHECK(reread.sauce->group == orig.sauce->group);
                CHECK(reread.sauce->date == orig.sauce->date);
                CHECK(reread.sauce->flags == orig.sauce->flags);
                CHECK(reread.sauce->comments == orig.sauce->comments);
            }
        }
    }
}

TEST_CASE("opening a bad file leaves the document alone") {
    QTemporaryDir tmp;
    Document d;
    QString error;
    CHECK_FALSE(d.load(tmp.filePath(QStringLiteral("missing.ans")), &error));
    CHECK_FALSE(error.isEmpty());
    CHECK_FALSE(d.load(tmp.path(), &error));
    QFile empty(tmp.filePath(QStringLiteral("empty.ans")));
    REQUIRE(empty.open(QIODevice::WriteOnly));
    empty.close();
    CHECK_FALSE(d.load(empty.fileName(), &error));
    CHECK(d.path().isEmpty());
    CHECK(d.canvas() == Canvas(80, 40));
}

TEST_CASE("smoke: open a golden piece, type AB, save and re-parse") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    w.show();
    QString error;
    const fs::path src = goldenDir() / "tux.ans";
    REQUIRE(w.openFile(qpath(src), &error));
    CanvasView* v = w.canvasView();
    CHECK(v->cellCursor() == Cursor{0, 0});
    CHECK_FALSE(w.document()->isModified());

    key(v, Qt::Key_Down);
    key(v, Qt::Key_Right);
    key(v, Qt::Key_Right);
    CHECK(v->cellCursor() == Cursor{2, 1});
    type(v, QStringLiteral("AB"));
    CHECK(v->cellCursor() == Cursor{4, 1});
    CHECK(w.document()->isModified());
    CHECK(w.isWindowModified());

    // the preview's renderer (shared with the canvas view) is up to date
    QCoreApplication::processEvents();
    const_cast<CanvasRenderer&>(w.renderer()).flush();
    const auto& atlas = dynamic_cast<const AtlasRenderer&>(w.renderer());
    CHECK(atlas.cache() == renderCanvas(w.document()->canvas(), 8).convertToFormat(atlas.cache().format()));

    const QString out = tmp.filePath(QStringLiteral("typed.ans"));
    REQUIRE(w.saveFile(out, &error));
    CHECK_FALSE(w.document()->isModified());
    CHECK(w.document()->path() == out);

    const AnsiDocument orig = parseAnsi(bwtest::readFile(src));
    const AnsiDocument saved = parseAnsi(bwtest::readFile(out.toStdString()));
    const Cell a{U'A', Color::indexed(Grey), Color::indexed(Black), 0, false};
    const Cell b{U'B', Color::indexed(Grey), Color::indexed(Black), 0, false};
    CHECK(saved.canvas.at(2, 1) == a);
    CHECK(saved.canvas.at(3, 1) == b);
    // everything else is as it was, SAUCE included
    Canvas expect = orig.canvas;
    expect.at(2, 1) = a;
    expect.at(3, 1) = b;
    CHECK(saved.canvas == expect);
    REQUIRE(saved.sauce);
    CHECK(saved.sauce->title == orig.sauce->title);
    CHECK(saved.sauce->author == orig.sauce->author);
}

TEST_CASE("Backspace, Delete, Home, End and Enter in the view") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    type(v, QStringLiteral("xyz"));
    CHECK(v->cellCursor() == Cursor{3, 0});
    key(v, Qt::Key_Backspace);
    CHECK(v->cellCursor() == Cursor{2, 0});
    CHECK(w.document()->canvas().at(2, 0) == Cell{});
    key(v, Qt::Key_Home);
    CHECK(v->cellCursor() == Cursor{0, 0});
    key(v, Qt::Key_Delete);
    CHECK(w.document()->canvas().at(0, 0) == Cell{});
    CHECK(w.document()->canvas().at(1, 0).ch == U'y');
    key(v, Qt::Key_End);
    CHECK(v->cellCursor() == Cursor{79, 0});
    key(v, Qt::Key_Return);
    CHECK(v->cellCursor() == Cursor{0, 1});
    // a glyph CP437 can't hold is not typed
    key(v, 0, QStringLiteral("★"));
    CHECK(v->cellCursor() == Cursor{0, 1});
    CHECK(w.document()->canvas().at(0, 1) == Cell{});
    // nor is a shortcut
    QKeyEvent ctrl(QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier, QStringLiteral("q"));
    QApplication::sendEvent(v, &ctrl);
    CHECK(w.document()->canvas().at(0, 1) == Cell{});
    QKeyEvent ctrlControl(QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier, QStringLiteral("\x11"));
    QApplication::sendEvent(v, &ctrlControl);
    CHECK(w.document()->canvas().at(0, 1) == Cell{});
    CHECK(v->cellCursor() == Cursor{0, 1});
}

TEST_CASE("AltGr and Option characters are typed") {
    // #28 review P3-3: any Ctrl, Alt or Meta made a key a shortcut
    MainWindow w;
    CanvasView* v = w.canvasView();
    const auto press = [&](int k, Qt::KeyboardModifiers m, const QString& text) {
        QKeyEvent e(QEvent::KeyPress, k, m, text);
        QApplication::sendEvent(v, &e);
    };
    // AltGr+Q on Windows: Ctrl+Alt with the character it gives
    press(Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("@"));
    CHECK(w.document()->canvas().at(0, 0).ch == U'@');
    CHECK(v->cellCursor() == Cursor{1, 0});
    // Option+u then a on macOS gives ä with Alt held
    press(Qt::Key_A, Qt::AltModifier, QStringLiteral("ä"));
    CHECK(w.document()->canvas().at(1, 0).ch == U'ä');
    CHECK(v->cellCursor() == Cursor{2, 0});
    // Ctrl+Alt with no text, or a control character, is still a shortcut
    press(Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier, QString());
    press(Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("\x11"));
    // and Cmd (Ctrl in Qt on macOS) or Meta without Alt, even with text
    press(Qt::Key_Q, Qt::MetaModifier, QStringLiteral("q"));
    CHECK(w.document()->canvas().at(2, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{2, 0});
}

TEST_CASE("Backspace after typing on the last column clears that column") {
    // #28 review P3-1, through the view
    MainWindow w;
    CanvasView* v = w.canvasView();
    key(v, Qt::Key_End);
    key(v, Qt::Key_Left);
    type(v, QStringLiteral("ab"));
    CHECK(v->cellCursor() == Cursor{79, 0, true});
    key(v, Qt::Key_Backspace);
    CHECK(w.document()->canvas().at(79, 0) == Cell{});
    CHECK(w.document()->canvas().at(78, 0).ch == U'a');
    CHECK(v->cellCursor() == Cursor{79, 0});
    // undo puts the b back and the cursor past it again
    action(w, "undo")->trigger();
    CHECK(w.document()->canvas().at(79, 0).ch == U'b');
    CHECK(v->cellCursor() == Cursor{79, 0, true});
    // a move leaves past-the-end behind
    key(v, Qt::Key_Left);
    key(v, Qt::Key_Right);
    CHECK(v->cellCursor() == Cursor{79, 0});
    key(v, Qt::Key_Backspace);
    CHECK(w.document()->canvas().at(78, 0) == Cell{});
    CHECK(w.document()->canvas().at(79, 0).ch == U'b');
}

TEST_CASE("the view scrolls only to keep the cursor in sight") {
    MainWindow w;
    w.resize(700, 500);
    w.show();
    QString error;
    REQUIRE(w.openFile(qpath(goldenDir() / "tux.ans"), &error));
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    QScrollBar* h = scroll->horizontalScrollBar();
    REQUIRE(h->maximum() > 0);  // the canvas is wider than the view
    CHECK(h->value() == 0);
    type(w.canvasView(), QStringLiteral("AB"));
    CHECK(h->value() == 0);
    key(w.canvasView(), Qt::Key_End);
    CHECK(h->value() == h->maximum());
    key(w.canvasView(), Qt::Key_Home);
    CHECK(h->value() == 0);
}

TEST_CASE("zoom is an integer from 1 to 8") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    const QSize base = w.renderer().imageSize();
    CHECK(base == QSize(80 * 8, 40 * 16));
    action(w, "zoomReset")->trigger();
    CHECK(v->zoom() == 1);
    CHECK(v->size() == base);
    action(w, "zoomIn")->trigger();
    CHECK(v->zoom() == 2);
    CHECK(v->size() == base * 2);
    for (int i = 0; i < 10; ++i)
        action(w, "zoomIn")->trigger();
    CHECK(v->zoom() == 8);
    CHECK(v->size() == base * 8);
    action(w, "zoomOut")->trigger();
    CHECK(v->zoom() == 7);
    for (int i = 0; i < 10; ++i)
        action(w, "zoomOut")->trigger();
    CHECK(v->zoom() == 1);

    CHECK(action(w, "zoomIn")->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_Equal)));
    CHECK(action(w, "zoomOut")->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Minus));
    CHECK(action(w, "zoomReset")->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_0));
}

TEST_CASE("the zoomed view shows the canvas pixels, scaled") {
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(qpath(goldenDir() / "tux.ans"), &error));
    CanvasView* v = w.canvasView();
    v->setZoom(3);
    key(v, Qt::Key_Down);  // the cursor off the sampled cells
    key(v, Qt::Key_Down);
    const QImage shot = v->grab(QRect(0, 0, 24 * 8 * 3, 1 * 16 * 3)).toImage().convertToFormat(QImage::Format_RGB32);
    const QImage want = renderCanvas(w.document()->canvas(), 8, 3).copy(0, 0, 24 * 8 * 3, 16 * 3);
    CHECK(shot == want);
}

TEST_CASE("the 1x preview dock is always there and follows the canvas") {
    MainWindow w;
    w.show();
    QDockWidget* dock = w.previewDock();
    CHECK(dock->isVisible());
    CHECK_FALSE(dock->features().testFlag(QDockWidget::DockWidgetClosable));
    CHECK(w.preview()->size() == w.renderer().imageSize());

    QString error;
    REQUIRE(w.openFile(qpath(goldenDir() / "tux.ans"), &error));
    CHECK(w.preview()->size() == w.renderer().imageSize());
    CHECK(w.renderer().imageSize() == QSize(w.document()->canvas().width() * 8, w.document()->canvas().height() * 16));

    type(w.canvasView(), QStringLiteral("Q"));
    const QImage shot = w.preview()->grab(QRect(0, 0, 8, 16)).toImage().convertToFormat(QImage::Format_RGB32);
    Canvas one(1, 1);
    one.at(0, 0) = Cell{U'Q', Color::indexed(Grey), Color::indexed(Black), 0, false};
    CHECK(shot == renderCanvas(one, 8));
}

TEST_CASE("the cell width follows SAUCE letter spacing") {
    QTemporaryDir tmp;
    AnsiDocument doc;
    doc.canvas = Canvas(10, 2);
    doc.canvas.at(0, 0).ch = U'─';
    doc.sauce = Sauce::fresh(10, 2);
    doc.sauce->setLetterSpacing(2);
    const QString path = tmp.filePath(QStringLiteral("nine.ans"));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    const std::string bytes = writeAnsi(doc).bytes;
    f.write(bytes.data(), qint64(bytes.size()));
    f.close();

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    CHECK(w.renderer().cellSize() == QSize(9, 16));
    CHECK(w.preview()->size() == QSize(90, 32));
}

TEST_CASE("a lossy save is refused unless allowed") {
    QTemporaryDir tmp;
    MainWindow w;
    CellEdit e;
    e.changes.push_back({0, 0, Cell{}, Cell{U'★'}});
    w.document()->apply(e);
    QString error;
    const QString out = tmp.filePath(QStringLiteral("lossy.ans"));
    CHECK_FALSE(w.saveFile(out, &error));
    CHECK(error.contains(QStringLiteral("'?'")));
    CHECK_FALSE(QFile::exists(out));
    CHECK(w.saveFile(out, &error, true));
    CHECK(QFile::exists(out));
}

TEST_CASE("a canvas too large to display is refused and the open document stays") {
    QTemporaryDir tmp;
    // 400x5000 cells at 9 px is 3600x80000 = 288,000,000 px, over the
    // renderer's 2^28 limit; at 8 px it would fit.
    AnsiDocument big;
    big.canvas = Canvas(kMaxColumns, kMaxRows);
    big.canvas.at(kMaxColumns - 1, kMaxRows - 1).ch = U'#';
    big.sauce = Sauce::fresh(kMaxColumns, kMaxRows);
    big.sauce->setLetterSpacing(2);
    REQUIRE(glyphWidth(big) == 9);
    const QString bigPath = tmp.filePath(QStringLiteral("big.ans"));
    QFile f(bigPath);
    REQUIRE(f.open(QIODevice::WriteOnly));
    const std::string bytes = writeAnsi(big).bytes;
    f.write(bytes.data(), qint64(bytes.size()));
    f.close();

    MainWindow w;
    QString error;
    const QString tux = qpath(goldenDir() / "tux.ans");
    REQUIRE(w.openFile(tux, &error));
    const Canvas before = w.document()->canvas();
    const QSize imageBefore = w.renderer().imageSize();
    key(w.canvasView(), Qt::Key_Down);
    key(w.canvasView(), Qt::Key_Right);
    const Cursor cursor = w.canvasView()->cellCursor();
    REQUIRE(cursor == Cursor{1, 1});

    CHECK_FALSE(w.openFile(bigPath, &error));
    CHECK(error.contains(QStringLiteral("too large")));
    CHECK(w.document()->path() == tux);
    CHECK(w.document()->canvas() == before);
    CHECK_FALSE(w.document()->isModified());
    CHECK(w.canvasView()->cellCursor() == cursor);
    CHECK(w.renderer().imageSize() == imageBefore);
    CHECK(w.canvasView()->size() == imageBefore * w.canvasView()->zoom());

    // and so does a file that can't be read or parsed
    QFile junk(tmp.filePath(QStringLiteral("junk.ans")));
    REQUIRE(junk.open(QIODevice::WriteOnly));
    junk.write("\x1b[5001B#");
    junk.close();
    for (const QString& bad : {tmp.filePath(QStringLiteral("missing.ans")), tmp.path(), junk.fileName()}) {
        CHECK_FALSE(w.openFile(bad, &error));
        CHECK(w.document()->path() == tux);
        CHECK(w.document()->canvas() == before);
        CHECK(w.canvasView()->cellCursor() == cursor);
    }

    // the window still works on the kept document
    type(w.canvasView(), QStringLiteral("Z"));
    CHECK(w.document()->canvas().at(1, 1).ch == U'Z');
}

TEST_CASE("input-method commits type like keys") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    CHECK(v->testAttribute(Qt::WA_InputMethodEnabled));

    QInputMethodEvent e;
    e.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(v, &e);
    CHECK(w.document()->canvas().at(0, 0).ch == U'é');
    CHECK(unicodeToCp437(w.document()->canvas().at(0, 0).ch) == 0x82);
    CHECK(v->cellCursor() == Cursor{1, 0});
    CHECK(w.document()->isModified());

    // a glyph CP437 can't hold is rejected, as a typed one is
    QStringList rejected;
    QObject::connect(v, &CanvasView::glyphRejected, [&](const QString& t) { rejected << t; });
    QInputMethodEvent star;
    star.setCommitString(QStringLiteral("★"));
    QApplication::sendEvent(v, &star);
    CHECK(rejected == QStringList{QStringLiteral("★")});
    CHECK(v->cellCursor() == Cursor{1, 0});
    CHECK(w.document()->canvas().at(1, 0) == Cell{});

    // the input method is told where the cursor is
    CHECK(v->inputMethodQuery(Qt::ImCursorRectangle).toRect() == v->cellRect(v->cellCursor()));
}

TEST_CASE("an input-method commit replaces the cells it names") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    type(v, QStringLiteral("eX"));
    v->setCellCursor({1, 0});
    QInputMethodEvent e;
    e.setCommitString(QStringLiteral("é"), -1, 1);
    QApplication::sendEvent(v, &e);
    CHECK(w.document()->canvas().at(0, 0).ch == U'é');
    CHECK(w.document()->canvas().at(1, 0).ch == U'X');
    CHECK(v->cellCursor() == Cursor{1, 0});
}

TEST_CASE("an input-method replacement is one edit clamped to the row") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    const Canvas& cv = w.document()->canvas();
    REQUIRE(cv.width() == 80);
    int edits = 0;
    QObject::connect(w.document(), &Document::cellsChanged, [&] { ++edits; });

    // a range starting before column 0 and longer than the commit: the cells
    // on the row are replaced, the leftovers cleared, in one edit
    type(v, QStringLiteral("abcd"));
    edits = 0;
    v->setCellCursor({2, 0});
    QInputMethodEvent e;
    e.setCommitString(QStringLiteral("é"), -5, 6);
    QApplication::sendEvent(v, &e);
    CHECK(edits == 1);
    CHECK(cv.at(0, 0).ch == U'é');
    CHECK(cv.at(1, 0) == Cell{});
    CHECK(cv.at(2, 0) == Cell{});
    CHECK(cv.at(3, 0).ch == U'd');
    CHECK(v->cellCursor() == Cursor{1, 0});

    // a range running off the last column
    v->setCellCursor({78, 1});
    type(v, QStringLiteral("pq"));
    v->setCellCursor({78, 1});
    QInputMethodEvent end;
    end.setCommitString(QStringLiteral("x"), 0, 40);
    QApplication::sendEvent(v, &end);
    CHECK(cv.at(78, 1).ch == U'x');
    CHECK(cv.at(79, 1) == Cell{});
    CHECK(cv.at(0, 2) == Cell{});
    CHECK(v->cellCursor() == Cursor{79, 1});
}

TEST_CASE("a commit with a glyph CP437 can't hold writes none of it") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QStringList rejected;
    QObject::connect(v, &CanvasView::glyphRejected, [&](const QString& t) { rejected << t; });
    QInputMethodEvent e;
    e.setCommitString(QStringLiteral("A★B"));
    QApplication::sendEvent(v, &e);
    CHECK(rejected == QStringList{QStringLiteral("★")});
    CHECK(w.document()->canvas() == Canvas(80, 40));
    CHECK(v->cellCursor() == Cursor{0, 0});
    CHECK_FALSE(w.document()->isModified());

    // as with a key whose text has one
    rejected.clear();
    key(v, Qt::Key_A, QStringLiteral("a★"));
    CHECK(rejected == QStringList{QStringLiteral("★")});
    CHECK(w.document()->canvas() == Canvas(80, 40));
    CHECK(v->cellCursor() == Cursor{0, 0});
}

TEST_CASE("a decomposed input-method commit types its composed glyph") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QStringList rejected;
    QObject::connect(v, &CanvasView::glyphRejected, [&](const QString& t) { rejected << t; });
    QInputMethodEvent e;
    e.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(v, &e);
    CHECK(rejected.isEmpty());
    CHECK(w.document()->canvas().at(0, 0).ch == U'é');
    CHECK(w.document()->canvas().at(1, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{1, 0});
    CHECK(w.document()->isModified());
}

TEST_CASE("a decomposed input-method commit replaces the cells it names") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    type(v, QStringLiteral("eX"));
    v->setCellCursor({1, 0});
    QInputMethodEvent e;
    e.setCommitString(QStringLiteral("é"), -1, 1);
    QApplication::sendEvent(v, &e);
    CHECK(w.document()->canvas().at(0, 0).ch == U'é');
    CHECK(w.document()->canvas().at(1, 0).ch == U'X');
    CHECK(w.document()->canvas().at(2, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{1, 0});
}

TEST_CASE("a key with decomposed text types its composed glyph") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    key(v, Qt::Key_E, QStringLiteral("é"));
    CHECK(w.document()->canvas().at(0, 0).ch == U'é');
    CHECK(w.document()->canvas().at(1, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{1, 0});
    CHECK(w.document()->isModified());
}

TEST_CASE("a combining mark with no CP437 composed form writes none of the commit") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QStringList rejected;
    QObject::connect(v, &CanvasView::glyphRejected, [&](const QString& t) { rejected << t; });
    // q + U+0301 has no precomposed form, so NFC leaves the mark on its own
    QInputMethodEvent e;
    e.setCommitString(QStringLiteral("Aq́B"));
    QApplication::sendEvent(v, &e);
    CHECK(rejected == QStringList{QStringLiteral("́")});
    CHECK(w.document()->canvas() == Canvas(80, 40));
    CHECK(v->cellCursor() == Cursor{0, 0});
    CHECK_FALSE(w.document()->isModified());

    rejected.clear();
    key(v, Qt::Key_Q, QStringLiteral("q́"));
    CHECK(rejected == QStringList{QStringLiteral("́")});
    CHECK(w.document()->canvas() == Canvas(80, 40));
    CHECK(v->cellCursor() == Cursor{0, 0});
}

TEST_CASE("Save As adds .ans and asks before replacing a file the dialog never saw") {
    QTemporaryDir tmp;
    const QString existing = tmp.filePath(QStringLiteral("piece.ans"));
    QFile f(existing);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    // "piece" passed the dialog's overwrite check, but "piece.ans" is what
    // gets written, and it exists
    SavePath p = resolveSavePath(tmp.filePath(QStringLiteral("piece")));
    CHECK(p.path == existing);
    CHECK(p.confirmOverwrite);
    // the dialog already asked about a name it returned whole
    p = resolveSavePath(existing);
    CHECK(p.path == existing);
    CHECK_FALSE(p.confirmOverwrite);
    // nothing there: nothing to ask
    p = resolveSavePath(tmp.filePath(QStringLiteral("fresh")));
    CHECK(p.path == tmp.filePath(QStringLiteral("fresh.ans")));
    CHECK_FALSE(p.confirmOverwrite);
    // another suffix is kept
    p = resolveSavePath(tmp.filePath(QStringLiteral("piece.txt")));
    CHECK(p.path == tmp.filePath(QStringLiteral("piece.txt")));
    CHECK_FALSE(p.confirmOverwrite);

    // the dialog adds the suffix itself, so its own check sees the final name
    QFileDialog d;
    configureSaveDialog(d, existing);
    CHECK(d.defaultSuffix() == QStringLiteral("ans"));
    CHECK(d.acceptMode() == QFileDialog::AcceptSave);
    CHECK_FALSE(d.testOption(QFileDialog::DontConfirmOverwrite));
    CHECK(d.nameFilters().contains(QStringLiteral("All files (*)")));
    // Qt's own dialog: "piece" under "All files (*)" comes back as piece.ans
    QFileDialog own;
    own.setOption(QFileDialog::DontUseNativeDialog);
    configureSaveDialog(own, existing);
    own.selectNameFilter(QStringLiteral("All files (*)"));
    own.selectFile(QStringLiteral("piece"));
    CHECK(own.selectedFiles() == QStringList{existing});
}

namespace {

// A click on the palette at swatch `index`.
void click(PaletteWidget* p, int index, Qt::MouseButton button) {
    const QPointF at = p->swatchRect(index).center();
    QMouseEvent e(QEvent::MouseButtonPress, at, p->mapToGlobal(at), button, button, Qt::NoModifier);
    QApplication::sendEvent(p, &e);
}

// The cells of the canvas that differ from `before`.
QRegion diff(const Canvas& a, const Canvas& b) {
    QRegion r;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.at(x, y) != b.at(x, y))
                r |= QRect(x, y, 1, 1);
    return r;
}

} // namespace

TEST_CASE("undo and redo restore the cells, the cursor and the modified flag") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    QAction* undo = action(w, "undo");
    QAction* redo = action(w, "redo");
    CHECK(undo->shortcut() == QKeySequence(QKeySequence::Undo));
    CHECK(redo->shortcut() == QKeySequence(QKeySequence::Redo));
    CHECK_FALSE(undo->isEnabled());

    std::vector<QRegion> repainted;
    QObject::connect(d, &Document::cellsChanged, [&](const QRegion& r) { repainted.push_back(r); });

    key(v, Qt::Key_Down);
    type(v, QStringLiteral("ab"));
    key(v, Qt::Key_Backspace);
    const Canvas typed = d->canvas();
    CHECK(typed.at(0, 1).ch == U'a');
    CHECK(typed.at(1, 1) == Cell{});
    CHECK(d->undoStack()->count() == 3);  // one command per key
    CHECK(undo->isEnabled());

    undo->trigger();  // the Backspace
    CHECK(d->canvas().at(1, 1).ch == U'b');
    CHECK(v->cellCursor() == Cursor{2, 1});
    CHECK(d->isModified());
    undo->trigger();  // b
    CHECK(d->canvas().at(1, 1) == Cell{});
    CHECK(v->cellCursor() == Cursor{1, 1});
    undo->trigger();  // a
    CHECK(d->canvas() == Canvas(80, 40));
    CHECK(v->cellCursor() == Cursor{0, 1});
    CHECK_FALSE(d->isModified());
    CHECK_FALSE(w.isWindowModified());
    CHECK_FALSE(undo->isEnabled());

    redo->trigger();
    redo->trigger();
    redo->trigger();
    CHECK(d->canvas() == typed);
    CHECK(v->cellCursor() == Cursor{1, 1});
    CHECK(d->isModified());
    CHECK(w.isWindowModified());

    // every change, undo and redo repainted just the one cell it touched
    REQUIRE(repainted.size() == 9);
    for (const QRegion& r : repainted)
        CHECK(r.rectCount() == 1);
    CHECK(repainted[3] == QRegion(1, 1, 1, 1));
    CHECK(repainted[5] == QRegion(0, 1, 1, 1));

    // the renderer's cache followed
    QCoreApplication::processEvents();
    const_cast<CanvasRenderer&>(w.renderer()).flush();
    const auto& atlas = dynamic_cast<const AtlasRenderer&>(w.renderer());
    CHECK(atlas.cache() == renderCanvas(d->canvas(), 8).convertToFormat(atlas.cache().format()));

    // a new edit after an undo drops the redo
    undo->trigger();  // the Backspace again: b is back, the cursor after it
    type(v, QStringLiteral("z"));
    CHECK_FALSE(redo->isEnabled());
    CHECK(d->canvas().at(1, 1).ch == U'b');
    CHECK(d->canvas().at(2, 1).ch == U'z');
}

TEST_CASE("undoing back to the saved state clears the modified flag") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    QAction* undo = action(w, "undo");
    QAction* redo = action(w, "redo");
    QString error;
    const fs::path src = goldenDir() / "tux.ans";
    REQUIRE(w.openFile(qpath(src), &error));
    const Canvas orig = d->canvas();

    type(v, QStringLiteral("A"));
    const QString out = tmp.filePath(QStringLiteral("saved.ans"));
    REQUIRE(w.saveFile(out, &error));
    CHECK_FALSE(d->isModified());
    const Canvas saved = d->canvas();

    // edit after the save, then undo back to it
    type(v, QStringLiteral("B"));
    CHECK(d->isModified());
    undo->trigger();
    CHECK_FALSE(d->isModified());
    CHECK_FALSE(w.isWindowModified());
    CHECK(d->canvas() == saved);
    CHECK(diff(d->canvas(), orig) == QRegion(0, 0, 1, 1));

    // past the save is modified again, and redo comes back to it
    undo->trigger();
    CHECK(d->isModified());
    CHECK(d->canvas() == orig);
    redo->trigger();
    CHECK_FALSE(d->isModified());
    CHECK(d->canvas() == saved);

    // undo past the save and edit: the saved state can't come back
    undo->trigger();
    type(v, QStringLiteral("C"));
    CHECK(d->isModified());
    undo->trigger();
    CHECK(d->isModified());  // the canvas is the original, not the saved one
    CHECK(d->canvas() == orig);

    // opening a file starts a fresh stack, clean
    REQUIRE(w.openFile(out, &error));
    CHECK_FALSE(d->isModified());
    CHECK_FALSE(undo->isEnabled());
    CHECK_FALSE(redo->isEnabled());
    CHECK(d->undoStack()->isClean());
    // and New does too
    type(v, QStringLiteral("x"));
    CHECK(undo->isEnabled());
    d->clear();
    CHECK_FALSE(undo->isEnabled());
    CHECK_FALSE(d->isModified());
}

TEST_CASE("F1-F10 type the glyphs of the current set") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    CHECK(v->glyphSet() == kDefaultGlyphSet);
    CHECK(w.glyphSetLabel()->text() == QStringLiteral("F-keys set 6: ░ ▒ ▓ █ ▀ ▄ ▌ ▐ ■ ·"));
    CHECK(action(w, "glyphSet6")->isChecked());

    for (int k = 0; k < kGlyphSetKeys; ++k)
        key(v, Qt::Key_F1 + k);
    for (int k = 0; k < kGlyphSetKeys; ++k)
        CHECK(d->canvas().at(k, 0).ch == glyphSetGlyph(kDefaultGlyphSet, k));
    CHECK(v->cellCursor() == Cursor{10, 0});
    CHECK(d->undoStack()->count() == 10);  // one command per key
    // F11 and F-keys with Ctrl, Alt or Meta type nothing
    key(v, Qt::Key_F11);
    QKeyEvent alt(QEvent::KeyPress, Qt::Key_F1, Qt::AltModifier);
    QApplication::sendEvent(v, &alt);
    CHECK(d->canvas().at(10, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{10, 0});

    // next and previous wrap round
    QAction* next = action(w, "nextGlyphSet");
    QAction* prev = action(w, "previousGlyphSet");
    CHECK(next->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Period));
    CHECK(prev->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Comma));
    next->trigger();
    CHECK(v->glyphSet() == 6);
    CHECK(w.glyphSetLabel()->text().startsWith(QStringLiteral("F-keys set 7: ☺")));
    CHECK(action(w, "glyphSet7")->isChecked());
    key(v, Qt::Key_F1);
    CHECK(d->canvas().at(10, 0).ch == U'☺');

    // Ctrl+Shift+F-keys pick sets 1-10; 11-16 have no direct chord
    QAction* first = action(w, "glyphSet1");
    for (int i = 0; i < kGlyphSetKeys; ++i)
        CHECK(action(w, QStringLiteral("glyphSet%1").arg(i + 1).toLatin1().constData())->shortcut() ==
              QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key(Qt::Key_F1 + i)));
    for (int i = kGlyphSetKeys; i < kGlyphSetCount; ++i)
        CHECK(action(w, QStringLiteral("glyphSet%1").arg(i + 1).toLatin1().constData())->shortcut().isEmpty());
    first->trigger();
    CHECK(v->glyphSet() == 0);
    prev->trigger();
    CHECK(v->glyphSet() == kGlyphSetCount - 1);
    CHECK(w.glyphSetLabel()->text().startsWith(QStringLiteral("F-keys set 16: /")));
    next->trigger();
    CHECK(v->glyphSet() == 0);
    key(v, Qt::Key_F5);
    CHECK(d->canvas().at(11, 0).ch == U'─');

    action(w, "defaultGlyphSet")->trigger();
    CHECK(v->glyphSet() == kDefaultGlyphSet);

    // F-keys type in the pen's colours and land on the last column like keys
    v->setPen({Color::indexed(LightRed), Color::indexed(Blue)});
    key(v, Qt::Key_End);
    key(v, Qt::Key_F4);
    CHECK(d->canvas().at(79, 0) == Cell{U'█', Color::indexed(LightRed), Color::indexed(Blue), 0, false});
    CHECK(v->cellCursor() == Cursor{79, 0, true});
}

TEST_CASE("no shortcut uses a desktop's F-key chords") {
    // #33 review P2: the window manager takes Alt+F-keys (Alt+F4 closes the
    // window), KDE and Xfce take Ctrl+F-keys for desktops and Linux takes
    // Ctrl+Alt+F-keys for VTs, so the editor never sees them
    MainWindow w;
    const QList<QAction*> actions = w.findChildren<QAction*>();
    REQUIRE(actions.size() > kGlyphSetCount);
    int checked = 0;
    for (QAction* a : actions) {
        for (const QKeySequence& seq : a->shortcuts()) {
            for (int k = 0; k < seq.count(); ++k) {
                const QKeyCombination c = seq[k];
                if (c.key() < Qt::Key_F1 || c.key() > Qt::Key_F35)
                    continue;
                ++checked;
                INFO(a->objectName().toStdString() << " " << seq.toString().toStdString());
                CHECK_FALSE(c.keyboardModifiers() & Qt::AltModifier);
                CHECK(c.keyboardModifiers() != Qt::ControlModifier);
            }
        }
    }
    CHECK(checked == kGlyphSetKeys);
}

TEST_CASE("the Glyphs menu lists every set") {
    MainWindow w;
    QMenu* glyphs = nullptr;
    for (QAction* m : w.menuBar()->actions())
        if (m->text() == QStringLiteral("&Glyphs"))
            glyphs = m->menu();
    REQUIRE(glyphs);
    QStringList sets;
    for (QAction* a : glyphs->actions())
        if (a->objectName().startsWith(QStringLiteral("glyphSet")))
            sets << a->objectName();
    REQUIRE(sets.size() == kGlyphSetCount);
    for (int i = 0; i < kGlyphSetCount; ++i) {
        CHECK(sets[i] == QStringLiteral("glyphSet%1").arg(i + 1));
        QAction* a = action(w, sets[i].toLatin1().constData());
        CHECK(a->text().startsWith(QStringLiteral("Set %1: ").arg(i + 1)));
        a->trigger();
        CHECK(w.canvasView()->glyphSet() == i);
        CHECK(a->isChecked());
    }
    // the status bar's tooltip names the keys
    const QString tip = w.glyphSetLabel()->toolTip();
    CHECK(tip.contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F1).toString(QKeySequence::NativeText)));
    CHECK(tip.contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F10).toString(QKeySequence::NativeText)));
    CHECK_FALSE(tip.contains(QStringLiteral("Alt")));
}

TEST_CASE("palette clicks set the pen, and typing uses it") {
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    PaletteWidget* p = w.paletteWidget();
    REQUIRE(w.paletteDock()->isVisibleTo(&w));
    CHECK_FALSE(w.paletteDock()->features() & QDockWidget::DockWidgetClosable);
    CHECK(p->pen().fg == Color::indexed(Grey));
    CHECK(p->pen().bg == Color::indexed(Black));
    for (int i = 0; i < 16; ++i)
        CHECK(p->indexAt(p->swatchRect(i).center()) == i);
    CHECK(p->indexAt(p->penRect().center()) == -1);

    click(p, Yellow, Qt::LeftButton);
    CHECK(v->pen().fg == Color::indexed(Yellow));
    CHECK(v->pen().bg == Color::indexed(Black));
    click(p, Blue, Qt::RightButton);
    CHECK(v->pen().fg == Color::indexed(Yellow));
    CHECK(v->pen().bg == Color::indexed(Blue));
    // the palette shows the pen
    CHECK(p->pen().fg == Color::indexed(Yellow));
    CHECK(p->pen().bg == Color::indexed(Blue));
    // a middle click picks nothing
    click(p, Red, Qt::MiddleButton);
    CHECK(v->pen().fg == Color::indexed(Yellow));

    type(v, QStringLiteral("Q"));
    key(v, Qt::Key_F1);
    CHECK(d->canvas().at(0, 0) == Cell{U'Q', Color::indexed(Yellow), Color::indexed(Blue), 0, false});
    CHECK(d->canvas().at(1, 0) == Cell{U'░', Color::indexed(Yellow), Color::indexed(Blue), 0, false});

    // the swatches draw the palette's colours
    const QImage img = p->grab().toImage();
    const QPoint at = p->swatchRect(Red).bottomLeft() + QPoint(3, -3);
    CHECK(img.pixelColor(at) == QColor(0xaa, 0x00, 0x00));
}

TEST_CASE("Ctrl+arrows cycle the pen's colours and Ctrl+D resets them") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    PaletteWidget* p = w.paletteWidget();
    QAction* nextFg = action(w, "nextForeground");
    QAction* prevFg = action(w, "previousForeground");
    QAction* nextBg = action(w, "nextBackground");
    QAction* prevBg = action(w, "previousBackground");
    CHECK(prevFg->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Up));
    CHECK(nextFg->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Down));
    CHECK(prevBg->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Left));
    CHECK(nextBg->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_Right));

    nextFg->trigger();
    CHECK(v->pen().fg == Color::indexed(DarkGrey));
    prevFg->trigger();
    prevFg->trigger();
    CHECK(v->pen().fg == Color::indexed(Brown));
    prevBg->trigger();  // wraps from black to white
    CHECK(v->pen().bg == Color::indexed(White));
    nextBg->trigger();
    nextBg->trigger();
    CHECK(v->pen().bg == Color::indexed(Blue));
    CHECK(p->pen().fg == Color::indexed(Brown));
    CHECK(p->pen().bg == Color::indexed(Blue));
    v->setForeground(White);
    nextFg->trigger();
    CHECK(v->pen().fg == Color::indexed(Black));

    action(w, "defaultColours")->trigger();
    CHECK(v->pen().fg == Color::indexed(Grey));
    CHECK(v->pen().bg == Color::indexed(Black));
    CHECK(p->pen().fg == Color::indexed(Grey));
}

int main(int argc, char** argv) {
    // Every window here reads its Library from empty scratch directories,
    // never the user's real one.
    QTemporaryDir libraries;
    qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("bundled"))));
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("user"))));
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
