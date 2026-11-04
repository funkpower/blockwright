// The GUI, headless (QT_QPA_PLATFORM=offscreen): the document, open/save
// round trips, zoom, the preview dock, undo/redo, F-key glyph sets, the
// palette, and an open-type-save smoke test.

#include "util.h"

#include "blockwright/cp437.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/xbin.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "palette_widget.h"
#include "scratch_settings.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

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
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#ifdef Q_OS_MACOS
#include <QFont>
#include <QWindow>
#include <qpa/qwindowsysteminterface_p.h>
#endif

#include <algorithm>
#include <map>
#include <optional>
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
    REQUIRE(w.document()->edit(e));
    QString error;
    const QString out = tmp.filePath(QStringLiteral("lossy.ans"));
    CHECK_FALSE(w.saveFile(out, &error));
    CHECK(error.contains(QStringLiteral("'?'")));
    CHECK_FALSE(QFile::exists(out));
    CHECK(w.saveFile(out, &error, true));
    CHECK(QFile::exists(out));
}

TEST_CASE("XBIN and BIN open and save in the format their names give") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const fs::path xb = bwtest::dataDir() / "xbin";
    const std::string tux = bwtest::readFile(goldenDir() / "tux.ans");
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(qpath(xb / "tux.xb"), &error));
    CHECK(w.document()->canvas() == parseXbin(bwtest::readFile(xb / "tux.xb")).canvas);
    CHECK(w.document()->ansi().ice);
    const QString asXb = tmp.filePath(QStringLiteral("tux.xb")), asBin = tmp.filePath(QStringLiteral("tux.bin")),
                  asAns = tmp.filePath(QStringLiteral("tux.ans"));
    REQUIRE(w.saveFile(asXb, &error));
    CHECK(bwtest::readFile(asXb.toStdString()) == bwtest::readFile(xb / "tux.xb"));
    // XBIN's SAUCE names no font, so this BIN names none either
    REQUIRE(w.saveFile(asBin, &error));
    const std::string bin = bwtest::readFile(asBin.toStdString());
    CHECK(bin == writeBin(parseXbin(bwtest::readFile(xb / "tux.xb"))).bytes);
    CHECK(parseBin(bin).canvas == parseAnsi(tux).canvas);

    // a BIN made from the piece, read back and saved as .ans, is the piece
    REQUIRE(w.openFile(qpath(xb / "tux.bin"), &error));
    CHECK(w.document()->canvas() == parseAnsi(tux).canvas);
    REQUIRE(w.saveFile(asAns, &error));
    CHECK(bwtest::readFile(asAns.toStdString()) == tux);

    // an XBIN under another name is read by its header
    const QString disguised = tmp.filePath(QStringLiteral("xbin.ans"));
    REQUIRE(QFile::copy(qpath(xb / "tux.xb"), disguised));
    REQUIRE(w.openFile(disguised, &error));
    CHECK(w.document()->canvas() == parseAnsi(tux).canvas);

    // a bad XBIN is refused and the document stays
    const QString bad = tmp.filePath(QStringLiteral("bad.xb"));
    QFile f(bad);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("XBIN\x1a\x00\x00\x01\x00\x10\x00", 11);
    f.close();
    CHECK_FALSE(w.openFile(bad, &error));
    CHECK(error.contains(QStringLiteral("width 0")));
    CHECK(w.document()->path() == disguised);
}

TEST_CASE("an XBIN's palette and font are kept by an XBIN save and lost after a warning by any other") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const fs::path pf = bwtest::dataDir() / "xbin" / "tux-palette-font.xb";
    const std::string fixture = bwtest::readFile(pf);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(qpath(pf), &error));
    REQUIRE(w.document()->ansi().palette);
    REQUIRE(w.document()->ansi().font);
    CHECK(w.document()->ansi().font->height == 8);

    // XBIN keeps both, and a save as XBIN is the file it was
    CHECK_FALSE(w.document()->encode(ArtFormat::Xbin).lossy());
    const QString again = tmp.filePath(QStringLiteral("again.xb"));
    REQUIRE(w.saveFile(again, &error));
    CHECK(bwtest::readFile(again.toStdString()) == fixture);

    // the others lose them: refused unless allowed, and the warning says so
    for (const ArtFormat f : {ArtFormat::Ansi, ArtFormat::Bin}) {
        const AnsiOutput out = w.document()->encode(f);
        CHECK(out.paletteDropped);
        CHECK(out.fontDropped);
    }
    const AnsiOutput lost = w.document()->encode(ArtFormat::Bin);
    CHECK(saveWarning(lost, false, ArtFormat::Bin).startsWith(QStringLiteral("Before saving as BIN:")));
    CHECK(saveWarning(lost, false, ArtFormat::Xbin).startsWith(QStringLiteral("Before saving as XBIN:")));
    CHECK(lossDescription(lost).contains(QStringLiteral("palette is left out")));
    CHECK(lossDescription(lost).contains(QStringLiteral("font is left out")));
    const QString ans = tmp.filePath(QStringLiteral("pf.ans")), bwp = tmp.filePath(QStringLiteral("pf.bwp"));
    CHECK_FALSE(w.saveFile(ans, &error));
    CHECK(error.contains(QStringLiteral("palette")));
    CHECK_FALSE(QFile::exists(ans));
    CHECK_FALSE(w.saveFile(bwp, &error));
    CHECK(error.contains(QStringLiteral("font")));
    CHECK_FALSE(QFile::exists(bwp));

    // saved as .ans anyway, the document is that file, which holds neither
    REQUIRE(w.saveFile(ans, &error, true));
    CHECK_FALSE(w.document()->ansi().palette);
    CHECK_FALSE(w.document()->ansi().font);
    CHECK_FALSE(w.document()->encode(ArtFormat::Bin).lossy());
    const QString plain = tmp.filePath(QStringLiteral("plain.xb"));
    REQUIRE(w.saveFile(plain, &error));
    CHECK(bwtest::readFile(plain.toStdString()) == bwtest::readFile(bwtest::dataDir() / "xbin" / "tux.xb"));

    // and a project save drops them too
    REQUIRE(w.openFile(qpath(pf), &error));
    REQUIRE(w.saveFile(bwp, &error, true));
    CHECK_FALSE(w.document()->ansi().palette);
    CHECK_FALSE(w.document()->ansi().font);
}

// #299: XBIN has no place for SAUCE's 9 px letter spacing, aspect ratio or
// font name; the editor's save warns of each, as of any other loss.
TEST_CASE("an XBIN save warns that SAUCE's letter spacing, aspect and font name are left out") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    AnsiDocument d = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"));
    REQUIRE(d.sauce);
    d.sauce->flags = 0x15;  // iCE, 9 px, square
    d.sauce->font = "Amiga Topaz 1";
    const QString in = tmp.filePath(QStringLiteral("flags.ans"));
    {
        const std::string bytes = writeAnsi(d).bytes;
        QFile f(in);
        REQUIRE(f.open(QIODevice::WriteOnly));
        REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
    }
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(in, &error));
    const AnsiOutput out = w.document()->encode(ArtFormat::Xbin);
    CHECK(out.lossy());
    CHECK(lossDescription(out) == QStringLiteral("the 9 px letter spacing is left out (XBIN has none and draws 8 px);\n"
                                                 "the square aspect ratio is left out (XBIN has none);\n"
                                                 "the SAUCE font name Amiga Topaz 1 is left out (XBIN names no font)"));
    CHECK(saveWarning(out, false, ArtFormat::Xbin).startsWith(QStringLiteral("Before saving as XBIN:")));
    // .ans and BIN keep them
    CHECK_FALSE(w.document()->encode(ArtFormat::Ansi).lossy());
    CHECK_FALSE(w.document()->encode(ArtFormat::Bin).lossy());

    const QString xb = tmp.filePath(QStringLiteral("flags.xb"));
    CHECK_FALSE(w.saveFile(xb, &error));
    CHECK(error.contains(QStringLiteral("letter spacing")));
    CHECK_FALSE(QFile::exists(xb));
    REQUIRE(w.saveFile(xb, &error, true));
    const AnsiDocument back = parseXbin(bwtest::readFile(xb.toStdString()));
    REQUIRE(back.sauce);
    CHECK(back.sauce->letterSpacing() == 0);
    CHECK(back.sauce->font.empty());
    // the document is now that XBIN, which has nothing more to lose
    CHECK(w.document()->path() == xb);
    REQUIRE(w.document()->ansi().sauce);
    CHECK(w.document()->ansi().sauce->letterSpacing() == 0);
    CHECK(w.document()->ansi().sauce->aspectRatio() == 0);
    CHECK(w.document()->ansi().sauce->font.empty());
    CHECK(w.document()->ansi().ice);
    CHECK_FALSE(w.document()->encode(ArtFormat::Xbin).lossy());
    CHECK_FALSE(w.document()->isModified());
}

// #319: the SAUCE fields an XBIN save drops are format state the undo
// stack holds, so the drop is a step of its own with the clean index after
// it. Undo brings them back as a change; redo drops them again, clean.
namespace {
int letterSpacing(MainWindow& w) {
    const auto& s = w.document()->ansi().sauce;
    return s ? s->letterSpacing() : 0;
}

// After a save that dropped the 9 px as one step: clean, 8 px; undo is 9 px
// and modified; redo is 8 px and clean again.
void checkDropStep(MainWindow& w, int stepsBefore) {
    QUndoStack* undo = w.document()->undoStack();
    CHECK(undo->count() == stepsBefore + 1);
    CHECK(undo->index() == undo->count());
    CHECK(undo->undoText() == QStringLiteral("Save as XBIN"));
    CHECK(letterSpacing(w) == 0);
    CHECK_FALSE(w.document()->isModified());
    undo->undo();
    CHECK(letterSpacing(w) == 2);
    CHECK(w.document()->isModified());
    undo->redo();
    CHECK(letterSpacing(w) == 0);
    CHECK_FALSE(w.document()->isModified());
}
}  // namespace

TEST_CASE("undo then redo after an XBIN Save As that drops 9 px leaves it dropped and clean") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(qpath(bwtest::dataDir() / "golden" / "tux.ans"), &error));
    REQUIRE(w.document()->setLetterSpacing(9));
    const QString xb = tmp.filePath(QStringLiteral("p.xb"));
    REQUIRE(w.saveFile(xb, &error, true));
    REQUIRE(w.document()->path() == xb);
    checkDropStep(w, 1);
    // undoing the drop and the 9 px is the file as opened, not as saved
    QUndoStack* undo = w.document()->undoStack();
    undo->undo();
    undo->undo();
    CHECK(undo->index() == 0);
    CHECK(letterSpacing(w) == 0);
    CHECK(w.document()->isModified());
    // and a second save to the same XBIN has nothing left to drop
    undo->redo();
    undo->redo();
    REQUIRE(w.saveFile(xb, &error));
    CHECK(undo->count() == 2);
    CHECK_FALSE(w.document()->isModified());
}

TEST_CASE("undo then redo after an XBIN Save that drops 9 px leaves it dropped and clean") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString xb = tmp.filePath(QStringLiteral("tux.xb"));
    REQUIRE(QFile::copy(qpath(bwtest::dataDir() / "xbin" / "tux.xb"), xb));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(xb, &error));
    REQUIRE(w.document()->setLetterSpacing(9));
    CHECK_FALSE(w.saveFile(xb, &error));  // refused unless allowed
    CHECK(w.document()->undoStack()->count() == 1);
    CHECK(letterSpacing(w) == 2);
    REQUIRE(w.saveFile(xb, &error, true));
    checkDropStep(w, 1);
}

TEST_CASE("an XBIN save that drops 9 px, aspect and font name drops them in one undo step") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    AnsiDocument d = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"));
    REQUIRE(d.sauce);
    d.sauce->flags = 0x15;  // iCE, 9 px, square
    d.sauce->font = "Amiga Topaz 1";
    const QString in = tmp.filePath(QStringLiteral("flags.ans"));
    {
        const std::string bytes = writeAnsi(d).bytes;
        QFile f(in);
        REQUIRE(f.open(QIODevice::WriteOnly));
        REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
    }
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(in, &error));
    REQUIRE(w.saveFile(tmp.filePath(QStringLiteral("flags.xb")), &error, true));
    // opened with them, so the drop is the only step
    checkDropStep(w, 0);
    w.document()->undoStack()->undo();
    const auto& s = w.document()->ansi().sauce;
    REQUIRE(s);
    CHECK(s->aspectRatio() != 0);
    CHECK(s->font == "Amiga Topaz 1");
    CHECK(w.document()->ansi().ice);
    w.document()->undoStack()->redo();
    CHECK(s->aspectRatio() == 0);
    CHECK(s->font.empty());
    CHECK(w.document()->ansi().ice);
}

TEST_CASE("a save that drops no SAUCE field adds no undo step") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(qpath(bwtest::dataDir() / "golden" / "tux.ans"), &error));
    QUndoStack* undo = w.document()->undoStack();

    SECTION("an 8 px XBIN Save As") {
        REQUIRE(w.document()->setIce(!w.document()->ansi().ice));
        const QString xb = tmp.filePath(QStringLiteral("plain.xb"));
        REQUIRE(w.saveFile(xb, &error));
        CHECK(undo->count() == 1);
        CHECK_FALSE(w.document()->isModified());
        undo->undo();
        CHECK(w.document()->isModified());
        undo->redo();
        CHECK_FALSE(w.document()->isModified());
    }
    SECTION("9 px saved to formats that keep it, Save As then Save") {
        REQUIRE(w.document()->setLetterSpacing(9));
        for (const char* name : {"keep.ans", "keep.bin"}) {
            const QString path = tmp.filePath(QString::fromLatin1(name));
            REQUIRE(w.saveFile(path, &error));
            CHECK(undo->count() == 1);
            CHECK(letterSpacing(w) == 2);
            CHECK_FALSE(w.document()->isModified());
            REQUIRE(w.saveFile(path, &error));  // Save, to the document's own file
            CHECK(undo->count() == 1);
            CHECK_FALSE(w.document()->isModified());
            undo->undo();
            CHECK(letterSpacing(w) == 0);
            CHECK(w.document()->isModified());
            undo->redo();
            CHECK(letterSpacing(w) == 2);
            CHECK_FALSE(w.document()->isModified());
        }
    }
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
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("★ has no CP437 code; not typed"));

    // #306: so is one CP437 has but .ans can't hold, and the status bar says why
    QInputMethodEvent circle;
    circle.setCommitString(QStringLiteral("○"));
    QApplication::sendEvent(v, &circle);
    CHECK(rejected.last() == QStringLiteral("○"));
    CHECK(w.document()->canvas().at(1, 0) == Cell{});
    CHECK(w.statusBar()->currentMessage() == QStringLiteral("○ is CP437 0x09, which .ans reads as TAB; not typed"));

    // the input method is told where the cursor is
    CHECK(v->inputMethodQuery(Qt::ImCursorRectangle).toRect() == v->cellRect(v->cellCursor()));
}

TEST_CASE("the typing refusal says why each glyph can't be typed") {
    // #306: the five CP437 glyphs .ans can't hold say which byte and control code
    CHECK(rejectedGlyphsMessage(QStringLiteral("○")) ==
          QStringLiteral("○ is CP437 0x09, which .ans reads as TAB; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("◙")) ==
          QStringLiteral("◙ is CP437 0x0A, which .ans reads as LF; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("♪")) ==
          QStringLiteral("♪ is CP437 0x0D, which .ans reads as CR; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("→")) ==
          QStringLiteral("→ is CP437 0x1A, which .ans reads as SUB; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("←")) ==
          QStringLiteral("← is CP437 0x1B, which .ans reads as ESC; not typed"));
    // a glyph with no CP437 code keeps its own reason, and a mix gives both
    CHECK(rejectedGlyphsMessage(QStringLiteral("★")) == QStringLiteral("★ has no CP437 code; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("★○…")) ==
          QStringLiteral("○ is CP437 0x09, which .ans reads as TAB; ★… has no CP437 code; not typed"));
    // one keypress or input-method commit carrying a glyph twice gives its reason once
    CHECK(rejectedGlyphsMessage(QStringLiteral("○○★★")) ==
          QStringLiteral("○ is CP437 0x09, which .ans reads as TAB; ★ has no CP437 code; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("★○←○★…")) ==
          QStringLiteral("○ is CP437 0x09, which .ans reads as TAB; ← is CP437 0x1B, which .ans reads as ESC; "
                         "★… has no CP437 code; not typed"));
    // review of #332, N3: a typed or committed right-to-left override, or an
    // invisible glyph, is named by its code point, never put in the status
    // bar raw, and set off from the glyphs beside it
    CHECK(rejectedGlyphsMessage(QString(QChar(0x202E))) == QStringLiteral("U+202E has no CP437 code; not typed"));
    CHECK(rejectedGlyphsMessage(QChar(0x202E) + QStringLiteral("★…")) ==
          QStringLiteral("U+202E ★… has no CP437 code; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("★") + QChar(0x202E) + QChar(0xFE0F) + QStringLiteral("…")) ==
          QStringLiteral("★ U+202E U+FE0F … has no CP437 code; not typed"));
    CHECK(rejectedGlyphsMessage(QStringLiteral("○") + QChar(0x0301)) ==
          QStringLiteral("○ is CP437 0x09, which .ans reads as TAB; U+0301 has no CP437 code; not typed"));
    for (const QChar ch : {QChar(0x202E), QChar(0xFE0F), QChar(0x0301)})
        CHECK_FALSE(rejectedGlyphsMessage(QStringLiteral("★") + ch).contains(ch));
}

TEST_CASE("the loss warning names the glyphs it loses, with a cell, and never shows a control raw") {
    // #331 N1: up to three glyphs, each with its cell (column,row from 0) or
    // its count and first cell, then how many more
    AnsiOutput out;
    out.encoding = TextEncoding::Cp437;
    out.unwritable = {{0, 0, U'★'}};
    CHECK(lossDescription(out) ==
          QStringLiteral("1 cell holds a glyph with no CP437 code and will be written as '?': ★ at 0,0"));
    out.unwritable = {{5, 0, U'★'}, {1, 2, U'☃'}, {7, 0, U'★'}, {0, 3, char32_t(0x202E)}};
    CHECK(lossDescription(out) ==
          QStringLiteral("4 cells hold a glyph with no CP437 code and will be written as '?': ★ in 2 cells, first "
                         "at 5,0; ☃ at 1,2; U+202E at 0,3"));
    out.unwritable.push_back({2, 3, U'✓'});
    CHECK(lossDescription(out).endsWith(QStringLiteral("U+202E at 0,3; and 1 more glyph")));
    out.unwritable.push_back({3, 3, char32_t(0x9B)});
    out.unwritable.push_back({4, 3, U'✓'});
    CHECK(lossDescription(out) ==
          QStringLiteral("7 cells hold a glyph with no CP437 code and will be written as '?': ★ in 2 cells, first "
                         "at 5,0; ☃ at 1,2; U+202E at 0,3; and 2 more glyphs"));
    // a control byte's glyph is counted with the controls, not named among these
    out.unwritable = {{0, 0, U'○'}, {1, 0, U'★'}};
    CHECK(lossDescription(out).startsWith(
        QStringLiteral("1 cell holds a glyph with no CP437 code and will be written as '?': ★ at 1,0;\n")));

    // a UTF-8 .ans names its control characters by their code points
    out.encoding = TextEncoding::Utf8;
    out.unwritable = {{0, 0, char32_t(0x9B)}, {1, 0, char32_t(0x1B)}, {2, 1, char32_t(0x9B)}};
    CHECK(lossDescription(out) == QStringLiteral("3 cells hold control characters a UTF-8 .ans can't hold and will "
                                                 "be written as '?': U+009B in 2 cells, first at 0,0; U+001B at 1,0"));

    // the editor's own save of a document holding them
    AnsiDocument d;
    d.canvas = Canvas(3, 1);
    d.canvas.at(0, 0).ch = char32_t(0x202E);
    d.canvas.at(1, 0).ch = char32_t(0x9B);
    d.canvas.at(2, 0).ch = U'★';
    const QString cp = saveWarning(writeAnsi(d, TextEncoding::Cp437));
    CHECK(cp.contains(QStringLiteral("'?': U+202E at 0,0; U+009B at 1,0; ★ at 2,0.")));
    const QString u8 = saveWarning(writeAnsi(d, TextEncoding::Utf8));
    CHECK(u8.contains(QStringLiteral("'?': U+009B at 1,0.")));
    for (const QString& text : {cp, u8}) {
        CHECK_FALSE(text.contains(QChar(0x202E)));
        CHECK_FALSE(text.contains(QChar(0x9B)));
    }
}

TEST_CASE("the loss warning names an invisible glyph by its code point, never as nothing") {
    // review of #332, N1: ❤️ in a UTF-8 document is U+2764 U+FE0F, two
    // cells; Publish and Send write CP437 and name the second as U+FE0F
    const AnsiDocument d = parseAnsi("\xe2\x9d\xa4\xef\xb8\x8f\r\n", {.encoding = TextEncoding::Utf8});
    REQUIRE(d.canvas.at(0, 0).ch == U'\u2764');
    REQUIRE(d.canvas.at(1, 0).ch == char32_t(0xFE0F));
    const AnsiOutput heart = writeAnsi(d, TextEncoding::Cp437);
    CHECK(lossDescription(heart) == QStringLiteral("2 cells hold a glyph with no CP437 code and will be written as "
                                                   "'?': ❤ at 0,0; U+FE0F at 1,0"));
    // a lone combining accent and a Hangul filler: a mark on the space
    // before it, and nothing
    AnsiOutput out;
    out.encoding = TextEncoding::Cp437;
    out.unwritable = {{3, 0, char32_t(0x0301)}, {4, 1, char32_t(0x3164)}, {5, 1, char32_t(0x034F)}};
    const QString lost = lossDescription(out);
    CHECK(lost == QStringLiteral("3 cells hold a glyph with no CP437 code and will be written as '?': U+0301 at 3,0; "
                                 "U+3164 at 4,1; U+034F at 5,1"));
    for (const QString& text : {lossDescription(heart), lost})
        for (const char32_t ch : {char32_t(0xFE0F), char32_t(0x0301), char32_t(0x3164), char32_t(0x034F)})
            CHECK_FALSE(text.contains(QString::fromUcs4(&ch, 1)));
}

TEST_CASE("the save warning tells glyphs with no CP437 code from control bytes") {
    // #300: the cells writeAnsi lists as '?' for either reason
    AnsiOutput out;
    out.unwritable = {{0, 0, U'○'}, {1, 0, U'←'}, {2, 0, U'★'}, {3, 0, U'◙'}, {4, 0, U'○'}};
    const QString lost = lossDescription(out);
    CHECK(lost.contains(QStringLiteral("1 cell holds a glyph with no CP437 code and will be written as '?'")));
    CHECK(lost.contains(QStringLiteral("4 cells hold a glyph whose CP437 byte .ans reads as a control code "
                                       "(TAB, ESC, LF) and will be written as '?' (XBIN and BIN keep them)")));
    out.unwritable = {{0, 0, U'○'}};
    CHECK(lossDescription(out) == QStringLiteral("1 cell holds a glyph whose CP437 byte .ans reads as a control code "
                                                 "(TAB) and will be written as '?' (XBIN and BIN keep it)"));
    out.unwritable = {{0, 0, U'★'}, {1, 0, U'★'}};
    CHECK(lossDescription(out) ==
          QStringLiteral("2 cells hold a glyph with no CP437 code and will be written as '?': ★ in 2 cells, first at "
                         "0,0"));

    // the editor's own save of #300's XBIN: .ans loses the bytes and says
    // which, XBIN keeps them
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    std::string xb("XBIN\x1a\x04\x00\x01\x00\x10\x00", 11);
    for (const char b : {'\x09', '\x1b', '\x0a', 'A'})
        xb += {b, '\x07'};
    const QString path = tmp.filePath(QStringLiteral("ctl.xb"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        REQUIRE(f.write(xb.data(), qint64(xb.size())) == qint64(xb.size()));
    }
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    CHECK_FALSE(w.document()->encode(ArtFormat::Xbin).lossy());
    CHECK_FALSE(w.document()->encode(ArtFormat::Bin).lossy());
    const AnsiOutput ans = w.document()->encode(ArtFormat::Ansi);
    CHECK(saveWarning(ans) == QStringLiteral("Before saving as .ans:\n3 cells hold a glyph whose CP437 byte .ans "
                                             "reads as a control code (TAB, ESC, LF) and will be written as '?' "
                                             "(XBIN and BIN keep them).\n\nA UTF-8 .ans keeps all 3 of those glyphs: "
                                             "Save As and choose \"ANSI art, UTF-8 (*.ans)\".\n\nSave anyway?"));
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
}

TEST_CASE("the Save As dialog adds .ans itself, so its own check sees the final name") {
    // A QFileDialog writes Qt's own settings when it is destroyed. Only where
    // they are in the scratch directory may this test build one.
    const QString scratch = QFileInfo(bwtest::scratchSettingsFile()).absolutePath();
    if (!bwtest::qtSettingsInScratch(scratch))
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    QTemporaryDir tmp;
    const QString existing = tmp.filePath(QStringLiteral("piece.ans"));
    QFile f(existing);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    {
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
    // the dialogs kept their state in the scratch directory
    const QSettings qt(QSettings::UserScope, QStringLiteral("QtProject"));
    CHECK(qt.contains(QStringLiteral("FileDialog/lastVisited")));
    CHECK(bwtest::inDir(qt.fileName(), scratch));
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
    // Ctrl+Alt+F-keys for VTs, so the editor never sees them. Every action is
    // checked; only the glyph sets' chords are counted, because the platform's
    // standard keys add their own (#48: on xcb, Undo is also F14, the Sun Undo
    // key). Those may not take a glyph set's chord either: Qt fires neither
    // action on an ambiguous key (#78 review P2; "no two actions share a
    // shortcut" checks every key)
    MainWindow w;
    const QList<QAction*> actions = w.findChildren<QAction*>();
    REQUIRE(actions.size() > kGlyphSetCount);
    const auto isGlyphSet = [](QAction* a) { return a->objectName().startsWith(QStringLiteral("glyphSet")); };
    QList<QKeySequence> glyphSetKeys;
    for (QAction* a : actions)
        if (isGlyphSet(a))
            glyphSetKeys << a->shortcuts();
    REQUIRE(glyphSetKeys.size() == kGlyphSetKeys);
    int glyphSetChords = 0;
    for (QAction* a : actions) {
        for (const QKeySequence& seq : a->shortcuts()) {
            if (!isGlyphSet(a)) {
                INFO(a->objectName().toStdString() << " " << seq.toString().toStdString());
                CHECK_FALSE(glyphSetKeys.contains(seq));
            }
            for (int k = 0; k < seq.count(); ++k) {
                const QKeyCombination c = seq[k];
                if (c.key() < Qt::Key_F1 || c.key() > Qt::Key_F35)
                    continue;
                if (isGlyphSet(a))
                    ++glyphSetChords;
                INFO(a->objectName().toStdString() << " " << seq.toString().toStdString());
                CHECK_FALSE(c.keyboardModifiers() & Qt::AltModifier);
                CHECK(c.keyboardModifiers() != Qt::ControlModifier);
            }
        }
    }
    CHECK(glyphSetChords == kGlyphSetKeys);
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

TEST_CASE("the pen and the glyph set are kept between runs", "[settings]") {
    REQUIRE(bwtest::settingsInScratch());
    const auto stored = [](const char* k) { return appSettings()->value(QString::fromLatin1(k)).toString(); };
    {
        MainWindow w;
        CHECK(w.canvasView()->pen().fg == Color::indexed(Grey));
        CHECK(w.canvasView()->pen().bg == Color::indexed(Black));
        CHECK(w.canvasView()->glyphSet() == kDefaultGlyphSet);
        click(w.paletteWidget(), Red, Qt::LeftButton);
        click(w.paletteWidget(), Blue, Qt::RightButton);
        action(w, "glyphSet3")->trigger();
    }
    CHECK(stored("pen/foreground") == QStringLiteral("4"));
    CHECK(stored("pen/background") == QStringLiteral("1"));
    CHECK(stored("glyphSet/index") == QStringLiteral("2"));
    {
        MainWindow w;
        CanvasView* v = w.canvasView();
        CHECK(v->pen().fg == Color::indexed(Red));
        CHECK(v->pen().bg == Color::indexed(Blue));
        CHECK(w.paletteWidget()->pen().fg == Color::indexed(Red));
        CHECK(w.paletteWidget()->pen().bg == Color::indexed(Blue));
        CHECK(v->glyphSet() == 2);
        CHECK(w.glyphSetLabel()->text().startsWith(QStringLiteral("F-keys set 3: ")));
        CHECK(action(w, "glyphSet3")->isChecked());
        key(v, Qt::Key_F1);
        CHECK(w.document()->canvas().at(0, 0) ==
              Cell{glyphSetGlyph(2, 0), Color::indexed(Red), Color::indexed(Blue), 0, false});
        // a 24-bit colour the eyedropper took is kept as it is, not as the
        // palette colour nearest it
        v->setPen({Color::fromRgb({0x12, 0xab, 0xef}), Color::indexed(LightRed)});
        action(w, "nextGlyphSet")->trigger();
    }
    CHECK(stored("pen/foreground") == QStringLiteral("#12abef"));
    {
        MainWindow w;
        CHECK(w.canvasView()->pen().fg == Color::fromRgb({0x12, 0xab, 0xef}));
        CHECK(w.canvasView()->pen().bg == Color::indexed(LightRed));
        CHECK(w.canvasView()->glyphSet() == 3);
        // the defaults, once chosen, are kept too
        action(w, "defaultColours")->trigger();
        action(w, "defaultGlyphSet")->trigger();
    }
    {
        MainWindow w;
        CHECK(w.canvasView()->pen().fg == Color::indexed(Grey));
        CHECK(w.canvasView()->pen().bg == Color::indexed(Black));
        CHECK(w.canvasView()->glyphSet() == kDefaultGlyphSet);
    }
}

TEST_CASE("a stored pen colour or glyph set out of range reads as the default", "[settings]") {
    REQUIRE(bwtest::settingsInScratch());
    // A colour is a palette index 0-15 or #rrggbb; a glyph set an index 0-15.
    // Anything else is not taken into range but dropped: the nearest colour
    // or set is not one anybody picked.
    const std::vector<std::pair<QVariant, std::optional<Color>>> colours = {
        {QStringLiteral("0"), Color::indexed(Black)},
        {QStringLiteral("15"), Color::indexed(White)},
        {QVariant(12), Color::indexed(LightRed)},
        {QStringLiteral("#A0b1C2"), Color::fromRgb({0xa0, 0xb1, 0xc2})},
        {QStringLiteral("16"), std::nullopt},
        {QStringLiteral("-1"), std::nullopt},
        {QVariant(255), std::nullopt},
        {QStringLiteral("12.5"), std::nullopt},
        {QStringLiteral("red"), std::nullopt},
        {QStringLiteral(""), std::nullopt},
        {QStringLiteral("#12345"), std::nullopt},
        {QStringLiteral("#1234567"), std::nullopt},
        {QStringLiteral("#gg0000"), std::nullopt},
        {QStringLiteral("#+12345"), std::nullopt},
        {QStringLiteral("0x0c"), std::nullopt},
        {QVariant(QStringList{QStringLiteral("1"), QStringLiteral("2")}), std::nullopt},
    };
    for (const auto& [value, want] : colours) {
        INFO(value.toString().toStdString());
        appSettings()->setValue(QStringLiteral("pen/foreground"), value);
        appSettings()->setValue(QStringLiteral("pen/background"), value);
        MainWindow w;
        CHECK(w.canvasView()->pen().fg == want.value_or(Color::indexed(Grey)));
        CHECK(w.canvasView()->pen().bg == want.value_or(Color::indexed(Black)));
        CHECK(w.paletteWidget()->pen().fg == w.canvasView()->pen().fg);
    }
    const std::vector<std::pair<QVariant, int>> sets = {
        {QStringLiteral("0"), 0},           {QStringLiteral("15"), 15},
        {QVariant(9), 9},                   {QStringLiteral("16"), kDefaultGlyphSet},
        {QStringLiteral("-1"), kDefaultGlyphSet}, {QVariant(100), kDefaultGlyphSet},
        {QStringLiteral("2.5"), kDefaultGlyphSet}, {QStringLiteral("set"), kDefaultGlyphSet},
        {QStringLiteral(""), kDefaultGlyphSet},
    };
    for (const auto& [value, want] : sets) {
        INFO(value.toString().toStdString());
        appSettings()->setValue(QStringLiteral("glyphSet/index"), value);
        MainWindow w;
        CHECK(w.canvasView()->glyphSet() == want);
        CHECK(w.glyphSetLabel()->text().startsWith(QStringLiteral("F-keys set %1: ").arg(want + 1)));
    }
}

TEST_CASE("a bright background pen kept from a run works in an iCE-off document as it did in that run",
          "[settings]") {
    REQUIRE(bwtest::settingsInScratch());
    // Typing with a bright background into an iCE-off document, in a window
    // whose pen was picked there and in one whose pen came from the settings:
    // the pen, the cell and the saved bytes are the same.
    const auto typeBright = [](MainWindow& w) {
        action(w, "iceColours")->trigger();
        REQUIRE_FALSE(w.document()->ansi().ice);
        CHECK(w.canvasView()->pen().bg == Color::indexed(LightRed));
        type(w.canvasView(), QStringLiteral("x"));
        return std::pair{w.document()->canvas().at(0, 0), w.document()->encode().bytes};
    };
    std::pair<Cell, std::string> live;
    {
        MainWindow w;
        click(w.paletteWidget(), Yellow, Qt::LeftButton);
        click(w.paletteWidget(), LightRed, Qt::RightButton);
        live = typeBright(w);
    }
    MainWindow w;
    CHECK(w.canvasView()->pen().fg == Color::indexed(Yellow));
    CHECK(w.document()->ansi().ice);
    const auto restored = typeBright(w);
    CHECK(restored.first == live.first);
    CHECK(restored.second == live.second);
    // the cell keeps the pen's bright background; with iCE off it shows and
    // saves as blink on the dark colour
    CHECK(live.first.bg == Color::indexed(LightRed));
}

#ifdef Q_OS_MACOS
// Cocoa never makes a popup the key window: a menu or a combo box's list opens
// over the window that stays active. The offscreen platform activates every
// window it shows, popups too, and Qt on macOS acts on an activation even
// while a popup is open, so closing the popup would deactivate the main
// window and end a stroke a real Mac keeps. These tests see the Mac's order:
// a popup never takes the focus window.
namespace {
struct PopupsStayInactive : QWindowSystemEventHandler {
    bool sendEvent(QWindowSystemInterfacePrivate::WindowSystemEvent* e) override {
        if (e->type == QWindowSystemInterfacePrivate::FocusWindow) {
            const auto* focus = static_cast<QWindowSystemInterfacePrivate::FocusWindowEvent*>(e);
            if (focus->focused && focus->focused->type() == Qt::Popup)
                return true;
        }
        return QWindowSystemEventHandler::sendEvent(e);
    }
};
}
#endif

// Each test case starts from the settings a fresh run has, as it does when
// ctest runs it in a process of its own, so a window an earlier case closed
// (saving its layout) or a setting it changed can't reach the next one (#249).
class FreshSettings : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(const Catch::TestCaseInfo&) override {
        if (!bwtest::resetScratchSettings()) {
            std::fprintf(stderr, "the app's settings are not in the scratch directory; not clearing them\n");
            std::exit(1);
        }
    }
};
CATCH_REGISTER_LISTENER(FreshSettings)

int main(int argc, char** argv) {
    // Every window here reads its Library from empty scratch directories,
    // never the user's real one, and keeps its settings, and Qt's own, in
    // the same scratch directory.
    QTemporaryDir libraries;
    qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("bundled"))));
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("user"))));
    bwtest::useScratchSettings(libraries);
#ifdef Q_OS_MACOS
    // A Mac's screens are 72 dpi to Qt, and its styles scale their metrics
    // from that base, but the offscreen platform's screen says 96, which
    // makes every scroll bar, frame and margin a third larger than a Mac
    // draws them. So the screen here is a Mac's, and the font Cocoa gives
    // an application, the 13 pt system font, is set below in place of the
    // offscreen platform's.
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") {
        QFile config(libraries.filePath(QStringLiteral("offscreen.json")));
        if (!config.open(QIODevice::WriteOnly) ||
            config.write(R"({"screens": [{"name": "Mac", "x": 0, "y": 0, "width": 800, "height": 800,)"
                         R"( "logicalDpi": 72, "logicalBaseDpi": 72, "dpr": 1}]})") < 0)
            return 1;
        config.close();
        qputenv("QT_QPA_PLATFORM", "offscreen:configfile=" + QFile::encodeName(config.fileName()));
    }
#endif
    QApplication app(argc, argv);
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
        QApplication::setFont(QFont(QStringLiteral(".AppleSystemUIFont"), 13));
    PopupsStayInactive popupsStayInactive;
    QWindowSystemInterfacePrivate::installWindowSystemEventHandler(&popupsStayInactive);
    const int failed = Catch::Session().run(argc, argv);
    QWindowSystemInterfacePrivate::removeWindowSystemEventhandler(&popupsStayInactive);
    return failed;
#else
    return Catch::Session().run(argc, argv);
#endif
}
