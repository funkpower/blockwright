// The GUI, headless (QT_QPA_PLATFORM=offscreen): the document, open/save
// round trips, zoom, the preview dock, and an open-type-save smoke test.

#include "util.h"

#include "blockwright/cp437.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputMethodEvent>
#include <QKeyEvent>
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
    d.apply(e);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == (QRegion(4, 2, 1, 1) | QRegion(9, 7, 1, 1)));
    CHECK(d.canvas().at(4, 2).ch == U'#');
    CHECK(d.isModified());

    d.apply(e, true);  // revert
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == seen[0]);
    CHECK(d.canvas() == Canvas(80, 40));

    // an edit that changes nothing reports nothing
    CellEdit none;
    none.changes.push_back({1, 1, Cell{}, Cell{}});
    d.apply(none);
    CHECK(seen.size() == 2);
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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
