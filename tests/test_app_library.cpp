// The Library dock (PLAN §12) in the main window: thumbnails, categories and
// search; placing a stamp by double-click or drop as a floating selection in
// transparent paste mode; and Selection > Save to Library.... main() is in
// test_app.cpp, which points the libraries at empty scratch directories.

#include "fs_quirks.h"
#include "system_input.h"
#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "library_dock.h"
#include "main_window.h"
#include "select_tool.h"
#include "tool.h"

#include "blockwright/frame.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/project.h"
#include "blockwright/render.h"
#include "blockwright/stamp_slice.h"
#include "blockwright/text.h"
#include "blockwright/text_style.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QScrollArea>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPointingDevice>
#include <QPushButton>
#include <QStatusBar>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTabletEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using namespace bw;
namespace fs = std::filesystem;

namespace {

fs::path fspath(const QString& s) { return fs::path(QFile::encodeName(s).toStdString()); }

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

void press(QWidget* w, int k, const QString& text = {}) {
    QKeyEvent down(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &up);
}

// Types `text` from (x, y) in keyboard mode.
void write(MainWindow& w, int x, int y, const QString& text) {
    w.tools()->setActive(QStringLiteral("keyboard"));
    w.canvasView()->setCellCursor({x, y});
    for (const QChar c : text)
        press(w.canvasView(), c.toUpper().unicode(), QString(c));
}

QChar at(MainWindow& w, int x, int y) { return QChar(char16_t(w.document()->canvas().at(x, y).ch.codePoint().value())); }

// A window whose libraries are the test data's (bundled) and a scratch
// directory (user).
struct Fixture {
    QTemporaryDir user;
    MainWindow w;
    LibraryDock* dock;
    Fixture() : dock(w.libraryWidget()) {
        REQUIRE(user.isValid());
        dock->setDirectories(bwtest::dataDir() / "library", fspath(user.path()));
    }
};

QListWidgetItem* item(LibraryDock* dock, const QString& name) {
    for (int i = 0; i < dock->grid()->count(); ++i)
        if (dock->grid()->item(i)->text() == name)
            return dock->grid()->item(i);
    FAIL("no stamp named " << name.toStdString() << " in the grid");
    return nullptr;
}

// What the grid would carry if `name` were dragged out of it.
std::unique_ptr<QMimeData> dragged(LibraryDock* dock, const QString& name) {
    QListWidgetItem* it = item(dock, name);
    const QModelIndex index = dock->grid()->model()->index(dock->grid()->row(it), 0);
    return std::unique_ptr<QMimeData>(dock->grid()->model()->mimeData({index}));
}

// Drags `mime` over the canvas and drops it on `cell`; whether the view
// accepted the drop.
bool drop(CanvasView* v, const QMimeData* mime, QPoint cell) {
    // Qt 6.12 takes the position as a QPointF and deprecates the QPoint one.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    const QPointF pos = v->toWidget(v->cellRect(cell)).center();
#else
    const QPoint pos = v->toWidget(v->cellRect(cell)).center();
#endif
    QDragEnterEvent enter(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v, &enter);
    if (!enter.isAccepted())
        return false;
    QDragMoveEvent move(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v, &move);
    // Offered as a move too: the stamp is still only copied, so the grid
    // keeps its item.
    QDropEvent d(QPointF(pos), Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(v, &d);
    return d.isAccepted() && d.dropAction() == Qt::CopyAction;
}

} // namespace

TEST_CASE("the Library dock shows the stamps as 1x thumbnails with categories and search") {
    Fixture f;
    QDockWidget* dock = f.w.libraryDock();
    REQUIRE(dock);
    CHECK(dock->objectName() == QStringLiteral("libraryDock"));
    CHECK(f.w.dockWidgetArea(dock) == Qt::RightDockWidgetArea);
    auto* scroll = qobject_cast<QScrollArea*>(dock->widget());
    REQUIRE(scroll);
    CHECK(scroll->widget() == f.dock);

    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Tux"), QStringLiteral("Small star")});
    CHECK(f.dock->categories() == QStringList{QStringLiteral("figures"), QStringLiteral("sky")});
    REQUIRE(f.dock->categoryList()->count() == 3);
    CHECK(f.dock->categoryList()->item(0)->text() == QStringLiteral("All"));
    CHECK(f.dock->problems().empty());
    CHECK(f.dock->problemLabel()->text().isEmpty());

    // The small star fits the grid's box, so its icon is the 1x render.
    const library::Stamp* star = f.dock->find(bwtest::dataDir() / "library" / "sky" / "small-star.ans");
    REQUIRE(star);
    const QImage want = renderCanvas(star->art.canvas, glyphWidth(star->art));
    REQUIRE(want.size() == QSize(40, 48));
    const QImage icon = item(f.dock, QStringLiteral("Small star"))
                            ->icon()
                            .pixmap(want.size())
                            .toImage()
                            .convertToFormat(want.format());
    CHECK(icon == want);
    // Tux (240 x 272 at 1x) is scaled down to fit.
    const QList<QSize> sizes = item(f.dock, QStringLiteral("Tux"))->icon().availableSizes();
    REQUIRE(sizes.size() == 1);
    CHECK(sizes.first().height() == 64);

    // Thumbnails are cached: a rescan renders nothing again.
    CHECK(f.dock->thumbnailRenders() == 2);
    f.dock->rescan();
    CHECK(f.dock->thumbnailRenders() == 2);

    f.dock->categoryList()->setCurrentRow(2);  // sky
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Small star")});
    f.dock->rescan();  // keeps the category
    CHECK(f.dock->categoryList()->currentItem()->text() == QStringLiteral("sky"));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Small star")});
    f.dock->categoryList()->setCurrentRow(0);
    f.dock->searchEdit()->setText(QStringLiteral("PENGUIN"));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Tux")});
    f.dock->searchEdit()->setText(QStringLiteral("star night"));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Small star")});
    f.dock->searchEdit()->setText(QStringLiteral("nothing"));
    CHECK(f.dock->shownNames().isEmpty());
}

// Every item of the grid fits its cell whole: its thumbnail and every line
// of its wrapped name, as the style lays them out (#430), and the cell is no
// bigger than its largest item. The option is the one QListView paints an
// icon-mode item with.
void checkNamesFit(StampGrid* grid) {
    QStyleOptionViewItem opt;
    opt.initFrom(grid);
    opt.widget = grid;
    opt.font = grid->font();
    opt.decorationSize = grid->iconSize();
    opt.decorationPosition = QStyleOptionViewItem::Top;
    opt.decorationAlignment = Qt::AlignCenter;
    opt.displayAlignment = Qt::AlignCenter;
    opt.textElideMode = grid->textElideMode();
    opt.features = grid->wordWrap() ? QStyleOptionViewItem::WrapText : QStyleOptionViewItem::None;
    opt.rect = QRect(QPoint(), grid->gridSize());
    const QSize cell = grid->gridSize();
    const QStyledItemDelegate natural;  // the item as the style lays it out
    QSize most(0, 0);
    REQUIRE(grid->count() > 0);
    for (int i = 0; i < grid->count(); ++i) {
        const QModelIndex index = grid->model()->index(i, 0);
        const QSize need = natural.sizeHint(opt, index);
        const QRect shown = grid->visualRect(index);
        INFO(grid->item(i)->text().toStdString() << ": needs " << need.width() << " x " << need.height()
                                                 << ", shown in " << shown.width() << " x " << shown.height()
                                                 << ", cell " << cell.width() << " x " << cell.height());
        CHECK(need.height() <= shown.height());
        CHECK(need.width() <= shown.width());
        CHECK(shown.height() <= cell.height());
        CHECK(shown.width() <= cell.width());
        most = most.expandedTo(need);
    }
    // No bigger than that: as tall as the tallest item, and wider than the
    // icon box and a gap only for a word too wide to wrap.
    CHECK(cell.height() == most.height());
    CHECK(cell.width() == std::max(grid->iconSize().width() + 8, most.width()));
}

TEST_CASE("every name in the Library dock shows whole under its thumbnail, however tall, at any font size") {
    QTemporaryDir user;
    REQUIRE(user.isValid());
    MainWindow w;
    LibraryDock* dock = w.libraryWidget();
    dock->setDirectories(BW_BUNDLED_LIBRARY, fspath(user.path()));
    w.resize(2000, 2400);
    w.show();
    QApplication::processEvents();
    StampGrid* grid = dock->grid();
    // Barn's thumbnail fills the box's height; Bevelled block frame's name
    // wraps to two lines under a thumbnail as tall.
    REQUIRE(item(dock, QStringLiteral("Barn"))->icon().actualSize(grid->iconSize()).height() == grid->iconSize().height());
    REQUIRE(item(dock, QStringLiteral("Bevelled block frame")));
    checkNamesFit(grid);

    QFont big = dock->font();
    big.setPointSizeF(big.pointSizeF() * 2);
    dock->setFont(big);
    QApplication::processEvents();
    checkNamesFit(grid);

    // A search shows fewer stamps; they still fit, and a cell of only short
    // thumbnails is only as tall as they are.
    dock->searchEdit()->setText(QStringLiteral("frame"));
    QApplication::processEvents();
    checkNamesFit(grid);
    dock->searchEdit()->setText(QStringLiteral("water"));
    QApplication::processEvents();
    REQUIRE(grid->count() > 0);
    REQUIRE(grid->gridSize().height() < grid->iconSize().height() + grid->fontMetrics().lineSpacing());
    checkNamesFit(grid);
}

TEST_CASE("the user library is GenericDataLocation/Blockwright/library unless overridden") {
    const QByteArray env = qgetenv("BLOCKWRIGHT_USER_LIBRARY");
    REQUIRE(!env.isEmpty());  // test_app.cpp's main points it at scratch
    // The window reads the overridden one.
    MainWindow w;
    CHECK(w.libraryWidget()->userDir() == fspath(QFile::decodeName(env)));

    // Without the override. Only compared, never read or written.
    qunsetenv("BLOCKWRIGHT_USER_LIBRARY");
    const fs::path got = library::userLibraryDir();
    qputenv("BLOCKWRIGHT_USER_LIBRARY", env);
    const QString generic = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    CHECK(got == fspath(generic) / "Blockwright" / "library");
#ifndef Q_OS_WIN
    // On Linux and macOS that is the GUI's AppDataLocation + /library (on
    // Windows AppDataLocation is the roaming folder, and it isn't).
    const QString saved = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName(QStringLiteral("Blockwright"));
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setApplicationName(saved);
    CHECK(got == fspath(appData) / "library");
#endif
}

TEST_CASE("double-clicking a stamp floats it at the cursor in transparent mode until Enter") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    QUndoStack* undo = f.w.document()->undoStack();
    write(f.w, 0, 1, QStringLiteral("abcdefgh"));
    const int edits = undo->count();
    CHECK(f.w.selectTool()->pasteMode() == PasteMode::Normal);
    CHECK(act(f.w, "pasteNormal")->isChecked());

    v->setCellCursor({1, 0});
    emit f.dock->grid()->itemActivated(item(f.dock, QStringLiteral("Small star")));
    SelectTool* s = f.w.selectTool();
    CHECK(f.w.tools()->active() == s);
    REQUIRE(s->floating());
    CHECK(s->floatingPos() == QPoint(1, 0));
    CHECK(s->floatingBlock().width == 5);
    CHECK(s->floatingBlock().height == 3);
    CHECK(s->effectivePasteMode() == PasteMode::Transparent);
    CHECK(s->pasteMode() == PasteMode::Normal);  // the setting is unchanged
    CHECK(act(f.w, "pasteTransparent")->isChecked());
    CHECK(undo->count() == edits);  // nothing written yet
    CHECK(at(f.w, 3, 1) == QLatin1Char('d'));

    press(v, Qt::Key_Return);
    CHECK_FALSE(s->floating());
    CHECK(undo->count() == edits + 1);
    CHECK(s->effectivePasteMode() == PasteMode::Normal);
    CHECK(act(f.w, "pasteNormal")->isChecked());
    // Row 1 of the star is " ─☼─ " from column 1: its blank ends keep b and f.
    CHECK(at(f.w, 1, 1) == QLatin1Char('b'));
    CHECK(at(f.w, 2, 1) == QChar(u'─'));
    CHECK(at(f.w, 3, 1) == QChar(u'☼'));
    CHECK(at(f.w, 4, 1) == QChar(u'─'));
    CHECK(at(f.w, 5, 1) == QLatin1Char('f'));
    CHECK(at(f.w, 3, 0) == QChar(u'·'));
    undo->undo();
    CHECK(at(f.w, 3, 1) == QLatin1Char('d'));
}

TEST_CASE("a stamp saved from a selection appears in the dock and drops onto the canvas as a movable float") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    QUndoStack* undo = f.w.document()->undoStack();
    CHECK_FALSE(act(f.w, "saveToLibrary")->isEnabled());

    // A 3x1 part with a blank cell in the middle.
    write(f.w, 0, 0, QStringLiteral("x x"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 3, 1});
    CHECK(act(f.w, "saveToLibrary")->isEnabled());
    QString error;
    const QString path = f.w.saveSelectionToLibrary(QStringLiteral("Two Dots"), QStringLiteral("test"),
                                                    {QStringLiteral("dots")}, &error);
    INFO(error.toStdString());
    REQUIRE(!path.isEmpty());
    CHECK(QDir::cleanPath(path) == QDir(f.user.path()).filePath(QStringLiteral("two-dots.ans")));
    CHECK(QFileInfo::exists(QDir(f.user.path()).filePath(QStringLiteral("two-dots.json"))));
    const library::Stamp read = library::readStamp(fspath(path));
    CHECK(read.name == "Two Dots");
    CHECK(read.meta.category == "test");
    CHECK(read.meta.tags == std::vector<std::string>{"dots"});
    CHECK(f.dock->shownNames().contains(QStringLiteral("Two Dots")));
    CHECK(f.dock->categories().contains(QStringLiteral("test")));

    // Saving again under the same name is refused unless replacing.
    CHECK(f.w.saveSelectionToLibrary(QStringLiteral("Two Dots"), QStringLiteral("test"), {}, &error).isEmpty());
    CHECK(error.contains(QStringLiteral("already exists")));

    // Onto a fresh canvas, by drop at (10, 5).
    f.w.document()->clear();
    write(f.w, 9, 5, QStringLiteral("ooooo"));
    const int edits = undo->count();
    const std::unique_ptr<QMimeData> mime = dragged(f.dock, QStringLiteral("Two Dots"));
    REQUIRE(mime);
    CHECK(QString::fromUtf8(mime->data(QString::fromLatin1(kStampMimeType))) == path);
    CHECK(drop(v, mime.get(), {10, 5}));
    SelectTool* s = f.w.selectTool();
    REQUIRE(s->floating());
    CHECK(s->effectivePasteMode() == PasteMode::Transparent);
    CHECK(s->floatingPos() == QPoint(10, 5));
    CHECK(v->cellCursor() == Cursor{10, 5});
    CHECK(undo->count() == edits);

    // Movable before it is committed; Esc drops it with no edit.
    press(v, Qt::Key_Right);
    press(v, Qt::Key_Down);
    CHECK(s->floatingPos() == QPoint(11, 6));
    press(v, Qt::Key_Escape);
    CHECK_FALSE(s->floating());
    CHECK(undo->count() == edits);
    CHECK(at(f.w, 10, 5) == QLatin1Char('o'));

    // Dropped again and saved: the float is stamped before the save writes.
    CHECK(drop(v, mime.get(), {10, 5}));
    REQUIRE(s->floating());
    press(v, Qt::Key_Left);
    QTemporaryDir out;
    const QString file = out.filePath(QStringLiteral("scene.ans"));
    REQUIRE(f.w.saveFile(file, &error));
    CHECK_FALSE(s->floating());
    CHECK(undo->count() == edits + 1);
    CHECK(at(f.w, 9, 5) == QLatin1Char('x'));
    CHECK(at(f.w, 10, 5) == QLatin1Char('o'));  // the blank cell left it
    CHECK(at(f.w, 11, 5) == QLatin1Char('x'));
    CHECK_FALSE(f.w.document()->isModified());

    // Other drags are not taken.
    QMimeData text;
    text.setText(QStringLiteral("hello"));
    CHECK_FALSE(drop(v, &text, {0, 0}));
}

TEST_CASE("placing a stamp first stamps what already floats, in its own paste mode") {
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("a b"));
    f.w.tools()->setActive(f.w.selectTool());
    SelectTool* s = f.w.selectTool();
    s->select({0, 0, 3, 1});
    s->copy();
    write(f.w, 0, 2, QStringLiteral("zzz"));
    f.w.tools()->setActive(s);
    f.w.canvasView()->setCellCursor({0, 2});
    s->paste();
    REQUIRE(s->floating());
    REQUIRE(s->pasteMode() == PasteMode::Normal);
    QString error;
    REQUIRE(f.w.placeStamp(QString::fromStdString((bwtest::dataDir() / "library" / "sky" / "small-star.ans").string()),
                           {20, 10}, &error));
    // The paste landed in Normal mode: its blank middle overwrote the z.
    CHECK(at(f.w, 1, 2) == QLatin1Char(' '));
    CHECK(s->floating());
    CHECK(s->floatingPos() == QPoint(20, 10));
    CHECK(s->effectivePasteMode() == PasteMode::Transparent);
    CHECK(s->pasteMode() == PasteMode::Normal);
}

TEST_CASE("placing a stamp never changes the Paste Mode setting; a pick during the float does") {
    Fixture f;
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const QString star =
        QString::fromStdString((bwtest::dataDir() / "library" / "sky" / "small-star.ans").string());
    act(f.w, "pasteUnder")->trigger();
    REQUIRE(s->pasteMode() == PasteMode::Under);
    QString error;

    SECTION("Under, place, Esc: a paste is still Under") {
        REQUIRE(f.w.placeStamp(star, {0, 0}, &error));
        CHECK(s->effectivePasteMode() == PasteMode::Transparent);
        CHECK(act(f.w, "pasteTransparent")->isChecked());
        press(v, Qt::Key_Escape);
        CHECK_FALSE(s->floating());
        CHECK(s->pasteMode() == PasteMode::Under);
        CHECK(act(f.w, "pasteUnder")->isChecked());
        // A block "ab" pasted over "xy": Under writes only into blank cells.
        write(f.w, 0, 3, QStringLiteral("ab"));
        f.w.tools()->setActive(s);
        s->select({0, 3, 2, 1});
        s->copy();
        write(f.w, 4, 3, QStringLiteral("x"));
        f.w.tools()->setActive(s);
        v->setCellCursor({4, 3});
        s->paste();
        CHECK(s->effectivePasteMode() == PasteMode::Under);
        press(v, Qt::Key_Return);
        CHECK(at(f.w, 4, 3) == QLatin1Char('x'));
        CHECK(at(f.w, 5, 3) == QLatin1Char('b'));
    }
    SECTION("Under, place, Enter: the setting is still Under") {
        REQUIRE(f.w.placeStamp(star, {0, 0}, &error));
        press(v, Qt::Key_Return);
        CHECK(s->pasteMode() == PasteMode::Under);
        CHECK(act(f.w, "pasteUnder")->isChecked());
    }
    SECTION("a menu pick during the float applies to it and stays") {
        write(f.w, 0, 1, QStringLiteral("abcdef"));
        REQUIRE(f.w.placeStamp(star, {1, 0}, &error));
        act(f.w, "pasteNormal")->trigger();
        CHECK(s->effectivePasteMode() == PasteMode::Normal);
        CHECK(s->pasteMode() == PasteMode::Normal);
        press(v, Qt::Key_Return);
        // Normal: the star's blank cells overwrite b and f.
        CHECK(at(f.w, 1, 1) == QLatin1Char(' '));
        CHECK(at(f.w, 5, 1) == QLatin1Char(' '));
        CHECK(s->pasteMode() == PasteMode::Normal);
        CHECK(act(f.w, "pasteNormal")->isChecked());
    }
    SECTION("picking Transparent itself during the float makes it the setting") {
        REQUIRE(f.w.placeStamp(star, {0, 0}, &error));
        act(f.w, "pasteTransparent")->trigger();
        press(v, Qt::Key_Return);
        CHECK(s->pasteMode() == PasteMode::Transparent);
    }
}

// #433: a layer that can't take a stamp is refused when the stamp is placed,
// with the reason, as the Library's text style is: nothing floats, and the
// canvas and the undo history stay as they were.
TEST_CASE("placing a stamp on a layer that can't take it says why and changes nothing", "[library][layers]") {
    project::Project p;
    p.layers = LayerStack(80, 25);
    const LayerId bottom = p.layers.add(makeLayer(LayerKind::Cells, 80, 25));
    p.layers.find(bottom)->cells.at(1, 1).ch = U'a';
    TextSource source;
    source.text = U"x";
    source.wrapWidth = TextSource::kMaxWrapWidth;
    const LayerId text = p.layers.add(makeTextLayer(source));
    const LayerId generator = p.layers.add(makeLayer(LayerKind::Generator, 80, 25));
    const LayerId reference = p.layers.add(makeLayer(LayerKind::Reference, 80, 25));
    const LayerId pixels = p.layers.add(makeLayer(LayerKind::Pixels, 80, 25));

    Fixture f;
    Document* doc = f.w.document();
    doc->replace(p, QStringLiteral("stamp.bwp"), FileForm::ProjectZip);
    const fs::path star = bwtest::dataDir() / "library" / "sky" / "small-star.ans";

    QString expected;
    SECTION("locked") {
        REQUIRE(doc->setActiveLayer(bottom));
        LayerProperties props = doc->active()->props;
        props.locked = true;
        REQUIRE(doc->setLayerProperties(bottom, props, QStringLiteral("Lock")));
        expected = QStringLiteral("the layer is locked");
    }
    SECTION("hidden") {
        REQUIRE(doc->setActiveLayer(bottom));
        LayerProperties props = doc->active()->props;
        props.visible = false;
        REQUIRE(doc->setLayerProperties(bottom, props, QStringLiteral("Hide")));
        expected = QStringLiteral("the layer is hidden");
    }
    SECTION("live text") {
        REQUIRE(doc->setActiveLayer(text));
        expected = QStringLiteral("a live Text layer is drawn from its text");
    }
    SECTION("generator") {
        REQUIRE(doc->setActiveLayer(generator));
        expected = QStringLiteral("a Generator layer is drawn by its generator");
    }
    SECTION("reference") {
        REQUIRE(doc->setActiveLayer(reference));
        expected = QStringLiteral("a Reference layer holds a picture, not cells");
    }
    SECTION("pixels, a stamp with glyphs other than half blocks") {
        REQUIRE(doc->setActiveLayer(pixels));
        // the star's · ─ ☼, each once
        expected = QStringLiteral("a Pixels layer holds no ·─☼");
    }
    const Canvas canvas = doc->canvas();
    const LayerStack layers = doc->layers();
    const int steps = doc->undoStack()->count();
    const int index = doc->undoStack()->index();
    const auto unchanged = [&] {
        CHECK_FALSE(f.w.selectTool()->floating());
        CHECK(doc->canvas() == canvas);
        CHECK(doc->layers().layers() == layers.layers());
        CHECK(doc->undoStack()->count() == steps);
        CHECK(doc->undoStack()->index() == index);
    };

    QString error;
    CHECK_FALSE(f.w.placeStamp(star, {2, 2}, &error));
    CHECK(error == expected);
    unchanged();

    // dropped from the dock, it says so in the status bar
    const std::unique_ptr<QMimeData> mime = dragged(f.dock, QStringLiteral("Small star"));
    CHECK(drop(f.w.canvasView(), mime.get(), {2, 2}));
    CHECK(f.w.statusBar()->currentMessage() ==
          QStringLiteral("Could not place %1: %2").arg(QString::fromStdString(star.string()), expected));
    // Enter lands nothing, for nothing floats
    press(f.w.canvasView(), Qt::Key_Return);
    unchanged();
}

TEST_CASE("a stamp still floats and lands on a layer that can take it", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    QString error;

    SECTION("a Pixels layer and a stamp of half blocks and blanks") {
        CellBlock block(3, 1);
        block.at(0, 0).ch = U'█';
        block.at(1, 0).ch = U' ';
        block.at(2, 0).ch = U'▀';
        block.at(2, 0).fg = Color::indexed(Red);
        block.at(2, 0).bg = Color::indexed(Blue);
        const fs::path path = fspath(QDir(f.user.path()).filePath(QStringLiteral("halves.ans")));
        library::writeStamp(path, library::encodeStamp("Halves", "test", {}, block, false, std::nullopt));
        f.dock->rescan();
        REQUIRE(doc->addLayer(LayerKind::Pixels));
        const int steps = doc->undoStack()->count();
        REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
        REQUIRE(s->floating());
        press(f.w.canvasView(), Qt::Key_Return);
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(halfBlocks(doc->canvas().at(6, 4)) == HalfPair{Red, Blue});
    }
    SECTION("a rasterised Text layer") {
        project::Project p;
        p.layers = LayerStack(80, 25);
        p.layers.add(makeLayer(LayerKind::Cells, 80, 25));
        const LayerId id = p.layers.add(makeLayer(LayerKind::Text, 80, 25));
        doc->replace(p, QStringLiteral("stamp.bwp"), FileForm::ProjectZip);
        REQUIRE(doc->setActiveLayer(id));
        REQUIRE(doc->editable());
        const int steps = doc->undoStack()->count();
        REQUIRE(f.w.placeStamp(bwtest::dataDir() / "library" / "sky" / "small-star.ans", {10, 5}, &error));
        REQUIRE(s->floating());
        press(f.w.canvasView(), Qt::Key_Return);
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(doc->canvas().at(12, 5).ch == Glyph(U'·'));
    }
}

// #433: a stamp whose only glyphs a Pixels layer can't hold are ▌ ▐ floats
// there, as a quarter turn of the float makes them ▀ ▄; it lands only once
// none is left. ▌ ▐ in red on blue, so a turn shows in the colours.
// As text, so a failed check shows it.
std::string str(const QString& s) { return s.toStdString(); }

CellBlock sides() {
    CellBlock block(3, 1);
    block.at(0, 0).ch = U'▌';
    block.at(1, 0).ch = U'█';
    block.at(2, 0).ch = U'▐';
    for (Cell& c : block.cells) {
        c.fg = Color::indexed(Red);
        c.bg = Color::indexed(Blue);
    }
    return block;
}

fs::path writeSides(Fixture& f, const CellBlock& block = sides()) {
    const fs::path path = fspath(QDir(f.user.path()).filePath(QStringLiteral("sides.ans")));
    library::writeStamp(path, library::encodeStamp("Sides", "test", {}, block, false, std::nullopt));
    f.dock->rescan();
    return path;
}

QString refused(const fs::path& path, const QString& glyphs, bool turnable = true) {
    return QStringLiteral("Could not place %1: a Pixels layer holds no %2; %3")
        .arg(QString::fromStdString(path.string()), glyphs,
             turnable ? QStringLiteral("rotate it (Selection > Rotate) or press Esc to drop it")
                      : QStringLiteral("press Esc to drop it"));
}

// A left click on `cell`: a press and a release there.
void click(CanvasView* v, QPoint cell) {
    const QPointF at = v->toWidget(v->cellRect(cell)).center();
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, at, v->mapToGlobal(at), Qt::LeftButton,
                      type == QEvent::MouseButtonPress ? Qt::MouseButtons(Qt::LeftButton) : Qt::NoButton,
                      Qt::NoModifier);
        QApplication::sendEvent(v, &e);
    }
}

TEST_CASE("a ▌ ▐ stamp floats on a Pixels layer and lands only once a turn makes them ▀ ▄ (#433)",
          "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const fs::path path = writeSides(f);
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    const Canvas canvas = doc->canvas();
    const int steps = doc->undoStack()->count();
    const QImage before = v->grab().toImage();

    QString error;
    REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
    CHECK(error.isEmpty());
    REQUIRE(s->floating());
    // it shows as it is, though the layer can't take it as it is
    const QImage floated = v->grab().toImage();
    const QRect left = v->toWidget(v->cellRect(QPoint(4, 4)));
    const QPoint inLeft(left.left() + left.width() / 4, left.center().y());
    CHECK(floated.pixel(inLeft) != before.pixel(inLeft));

    // Enter: refused, said why, nothing written, still floating
    press(v, Qt::Key_Return);
    CHECK(str(f.w.statusBar()->currentMessage()) == str(refused(path, QStringLiteral("▌▐"))));
    CHECK(str(s->unlanded()) == str(QStringLiteral("a Pixels layer holds no ▌▐")));
    REQUIRE(s->floating());
    CHECK(s->floatingPos() == QPoint(4, 4));
    CHECK(s->floatingBlock().width == 3);
    CHECK(doc->canvas() == canvas);
    CHECK(doc->undoStack()->count() == steps);
    CHECK(v->grab().toImage().pixel(inLeft) == floated.pixel(inLeft));

    SECTION("turned clockwise, ▌ █ ▐ is ▀ █ ▄ down a column, and lands") {
        act(f.w, "rotateClockwise")->trigger();
        CHECK(s->unlanded().isEmpty());
        press(v, Qt::Key_Return);
        CHECK_FALSE(s->floating());
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(halfBlocks(doc->canvas().at(4, 4)) == HalfPair{Red, Blue});
        CHECK(halfBlocks(doc->canvas().at(4, 5)) == HalfPair{Red, Red});
        CHECK(halfBlocks(doc->canvas().at(4, 6)) == HalfPair{Blue, Red});
        CHECK(doc->canvas().at(5, 4) == canvas.at(5, 4));
    }
    SECTION("turned counter-clockwise, ▐ comes up as ▀ and ▌ goes down as ▄") {
        act(f.w, "rotateCounterClockwise")->trigger();
        press(v, Qt::Key_Return);
        CHECK_FALSE(s->floating());
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(halfBlocks(doc->canvas().at(4, 4)) == HalfPair{Red, Blue});
        CHECK(halfBlocks(doc->canvas().at(4, 5)) == HalfPair{Red, Red});
        CHECK(halfBlocks(doc->canvas().at(4, 6)) == HalfPair{Blue, Red});
    }
    SECTION("a flip keeps them on their sides, and a half turn too") {
        act(f.w, "flipHorizontal")->trigger();
        press(v, Qt::Key_Return);
        CHECK(s->floating());
        act(f.w, "flipVertical")->trigger();
        press(v, Qt::Key_Return);
        CHECK(s->floating());
        act(f.w, "rotateClockwise")->trigger();
        act(f.w, "rotateClockwise")->trigger();
        press(v, Qt::Key_Return);
        CHECK(s->floating());
        CHECK(str(f.w.statusBar()->currentMessage()) == str(refused(path, QStringLiteral("▌▐"))));
        CHECK(doc->canvas() == canvas);
        CHECK(doc->undoStack()->count() == steps);
    }
    SECTION("moved, it is checked again where it lands") {
        press(v, Qt::Key_Right);
        CHECK(s->unlanded().isEmpty());
        press(v, Qt::Key_Return);
        CHECK(s->floating());
        CHECK(s->floatingPos() == QPoint(5, 4));
        CHECK(str(s->unlanded()) == str(QStringLiteral("a Pixels layer holds no ▌▐")));
    }
    SECTION("Esc drops it, as before") {
        press(v, Qt::Key_Escape);
        CHECK_FALSE(s->floating());
        CHECK(s->unlanded().isEmpty());
        CHECK(doc->canvas() == canvas);
        CHECK(doc->undoStack()->count() == steps);
    }
}

TEST_CASE("a stamp with a glyph a Pixels layer can't hold but ▌ ▐ is still refused before it floats (#433)",
          "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    CellBlock block = sides();
    QString expected;
    SECTION("▌ with a shade") {
        block.at(1, 0).ch = U'░';
        expected = QStringLiteral("a Pixels layer holds no ▌░▐");
    }
    SECTION("a shade alone") {
        block = CellBlock(1, 1);
        block.at(0, 0).ch = U'▒';
        expected = QStringLiteral("a Pixels layer holds no ▒");
    }
    const fs::path path = writeSides(f, block);
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    const Canvas canvas = doc->canvas();
    const int steps = doc->undoStack()->count();
    QString error;
    CHECK_FALSE(f.w.placeStamp(path, {4, 4}, &error));
    CHECK(str(error) == str(expected));
    CHECK_FALSE(f.w.selectTool()->floating());
    CHECK(doc->canvas() == canvas);
    CHECK(doc->undoStack()->count() == steps);
}

TEST_CASE("a stamp of ▀ and ▌ floats, but no turn lands it (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CellBlock block = sides();
    block.at(1, 0).ch = U'▀';
    const fs::path path = writeSides(f, block);
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    const Canvas canvas = doc->canvas();
    QString error;
    REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
    press(f.w.canvasView(), Qt::Key_Return);
    CHECK(str(f.w.statusBar()->currentMessage()) == str(refused(path, QStringLiteral("▌▐"), false)));
    act(f.w, "rotateClockwise")->trigger();
    press(f.w.canvasView(), Qt::Key_Return);
    // the ▀ is on its side now
    CHECK(str(f.w.statusBar()->currentMessage()) == str(refused(path, QStringLiteral("▐"), false)));
    CHECK(s->floating());
    CHECK(doc->canvas() == canvas);
}

TEST_CASE("every way a float that can't land would land refuses it, or drops it (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId background = doc->activeLayer();
    const fs::path path = writeSides(f);
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    QString error;
    REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
    REQUIRE(s->floating());
    const Canvas canvas = doc->canvas();
    const LayerStack layers = doc->layers();
    const int steps = doc->undoStack()->count();
    const QString why = QStringLiteral("a Pixels layer holds no ▌▐");
    const QString cantLand = QStringLiteral("the floating selection can't land: ") + why;
    const auto kept = [&] {
        CHECK(s->floating());
        CHECK(s->floatingPos() == QPoint(4, 4));
        CHECK(str(s->unlanded()) == str(why));
        CHECK(doc->canvas() == canvas);
        CHECK(doc->layers().layers() == layers.layers());
        CHECK(doc->undoStack()->count() == steps);
    };
    const auto dropped = [&] {
        CHECK_FALSE(s->floating());
        CHECK(doc->canvas() == canvas);
    };

    SECTION("a click on it") {
        click(v, {5, 4});
        CHECK(str(f.w.statusBar()->currentMessage()) == str(refused(path, QStringLiteral("▌▐"))));
        kept();
    }
    SECTION("a click outside it keeps the selection too") {
        click(v, {20, 10});
        kept();
        CHECK(v->selection() == Selection({4, 4, 3, 1}));
    }
    SECTION("Select All and Deselect") {
        act(f.w, "selectAll")->trigger();
        kept();
        act(f.w, "deselect")->trigger();
        kept();
        CHECK(v->selection() == Selection({4, 4, 3, 1}));
    }
    SECTION("another tool: the switch is refused") {
        f.w.tools()->setActive(QStringLiteral("text"));
        CHECK(f.w.tools()->active() == s);
        CHECK(v->tool() == s);
        CHECK(act(f.w, "selectTool")->isChecked());
        kept();
    }
    SECTION("typing") {
        f.w.tools()->setActive(s);
        press(v, Qt::Key_A, QStringLiteral("a"));
        press(v, Qt::Key_Backspace, QStringLiteral("\b"));
        press(v, Qt::Key_F1);
        kept();
        // Delete is the select tool's, and drops a float, as before
        press(v, Qt::Key_Delete);
        dropped();
        CHECK(doc->undoStack()->count() == steps);
    }
    SECTION("another stamp, or a paste, over it") {
        CHECK_FALSE(f.w.placeStamp(path, {10, 5}, &error));
        CHECK(str(error) == str(cantLand));
        kept();
        CHECK(s->floatingBlock().width == 3);
        s->copy();
        s->paste();
        kept();
    }
    SECTION("saving, a snapshot, a canvas size, an IRC export") {
        const QString file = QDir(f.user.path()).filePath(QStringLiteral("out.ans"));
        CHECK_FALSE(f.w.saveFile(file, &error));
        CHECK(str(error) == str(cantLand));
        CHECK_FALSE(QFile::exists(file));
        error.clear();
        CHECK(f.w.snapshot(&error) == 0);
        CHECK(str(error) == str(cantLand));
        error.clear();
        CHECK_FALSE(f.w.resizeCanvas({100, 30}, Anchor::TopLeft, &error));
        CHECK(str(error) == str(cantLand));
        CHECK(doc->canvas().width() == 80);
        error.clear();
        const QString txt = QDir(f.user.path()).filePath(QStringLiteral("out.txt"));
        CHECK_FALSE(f.w.exportIrcText(txt, &error));
        CHECK(str(error) == str(cantLand));
        CHECK_FALSE(QFile::exists(txt));
        kept();
    }
    SECTION("another active layer drops it, and says so") {
        REQUIRE(doc->setActiveLayer(background));
        CHECK(str(f.w.statusBar()->currentMessage()) ==
              str(QStringLiteral("Dropped the floating selection, which can't land: ") + why));
        dropped();
        CHECK(doc->undoStack()->count() == steps);
    }
    SECTION("undo drops it, and undoes the step before") {
        doc->undoStack()->undo();
        CHECK_FALSE(s->floating());
        CHECK(doc->canvas() == canvas);
    }
    SECTION("Esc drops it") {
        press(v, Qt::Key_Escape);
        dropped();
        CHECK(doc->undoStack()->count() == steps);
    }
}

TEST_CASE("closing with a float that can't land asks first, though nothing else changed (#433)",
          "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    const fs::path path = writeSides(f);
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    doc->undoStack()->setClean();
    QString error;
    REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
    REQUIRE_FALSE(doc->isModified());
    QString text;
    QTimer::singleShot(0, &f.w, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            text = box->text();
            box->button(QMessageBox::Cancel)->click();
        }
    });
    f.w.show();
    CHECK_FALSE(f.w.close());
    INFO(str(text));
    CHECK(text.startsWith(QStringLiteral("The floating selection can't land (a Pixels layer holds no ▌▐): discarding "
                                         "drops it.")));
    CHECK(f.w.selectTool()->floating());
}

TEST_CASE("closing a changed document with a float that can't land names both (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    write(f.w, 10, 10, QStringLiteral("x"));
    const fs::path path = writeSides(f);
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    QString error;
    REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
    REQUIRE(doc->isModified());
    QString text;
    QTimer::singleShot(0, &f.w, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            text = box->text();
            box->button(QMessageBox::Cancel)->click();
        }
    });
    f.w.show();
    CHECK_FALSE(f.w.close());
    INFO(str(text));
    CHECK(text.startsWith(QStringLiteral("The canvas has unsaved changes, and the floating selection can't land (a "
                                         "Pixels layer holds no ▌▐): it can't be saved, and discarding drops it "
                                         "with them.")));
    CHECK(f.w.selectTool()->floating());
}

TEST_CASE("a half pixel lifted and turned on its side is refused, not landed as an erase (#433)",
          "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    // one pixel, as the half-block brush draws it on a clear pixel layer
    CellEdit e;
    e.changes.push_back({3, 3, doc->layerCanvas().at(3, 3), pixelLayerCell(LayerPixels{std::uint8_t(Red), {}})});
    REQUIRE(doc->edit(e));
    const Canvas layer = doc->layerCanvas();
    const int steps = doc->undoStack()->count();
    f.w.tools()->setActive(s);
    s->select({3, 3, 1, 1});
    press(v, Qt::Key_Right);
    REQUIRE(s->floating());
    act(f.w, "rotateClockwise")->trigger();  // ▀ → ▐: a pixel on its side
    press(v, Qt::Key_Return);
    CHECK(str(f.w.statusBar()->currentMessage()) ==
          str(QStringLiteral("Could not put the floating selection down: a Pixels layer holds no ▐; rotate it "
                             "(Selection > Rotate) or press Esc to drop it")));
    CHECK(s->floating());
    CHECK(doc->layerCanvas() == layer);
    CHECK(doc->undoStack()->count() == steps);
    SECTION("another quarter turn lands it as a bottom pixel") {
        act(f.w, "rotateClockwise")->trigger();  // ▐ → ▄
        press(v, Qt::Key_Return);
        CHECK_FALSE(s->floating());
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(layerPixels(doc->layerCanvas().at(4, 3)) == LayerPixels{{}, std::uint8_t(Red)});
        CHECK(layerPixels(doc->layerCanvas().at(3, 3)) == LayerPixels{});
    }
    SECTION("Esc puts it back") {
        press(v, Qt::Key_Escape);
        CHECK_FALSE(s->floating());
        CHECK(doc->layerCanvas() == layer);
    }
}

TEST_CASE("a Layers panel step drops a float that can't land, and says so (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId background = doc->activeLayer();
    write(f.w, 4, 4, QStringLiteral("xxx"));
    const fs::path path = writeSides(f);
    const LayerId pixels = doc->addLayer(LayerKind::Pixels);
    REQUIRE(pixels);
    QString error;
    REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
    REQUIRE(s->floating());
    const Canvas layer = doc->layerCanvas();
    SECTION("another layer hidden") {
        REQUIRE(doc->setLayerVisible(background, false));
    }
    SECTION("the layer under it deleted") {
        REQUIRE(doc->removeLayer(background));
    }
    SECTION("its layer moved below another") {
        REQUIRE(doc->moveLayer(pixels, 0));
    }
    SECTION("its layer locked; a turn then lands nothing") {
        LayerProperties p = doc->active()->props;
        p.locked = true;
        REQUIRE(doc->setLayerProperties(pixels, p, QStringLiteral("Lock Layer")));
        act(f.w, "rotateClockwise")->trigger();
        press(v, Qt::Key_Return);
    }
    CHECK(str(f.w.statusBar()->currentMessage()) ==
          str(QStringLiteral("Dropped the floating selection, which can't land: a Pixels layer holds no ▌▐")));
    CHECK_FALSE(s->floating());
    REQUIRE(doc->activeLayer() == pixels);
    CHECK(doc->layerCanvas() == layer);
}

TEST_CASE("a float that undo leaves on a locked or hidden layer is refused there, not lost (#433)",
          "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const fs::path path = writeSides(f);
    const LayerId pixels = doc->addLayer(LayerKind::Pixels);
    REQUIRE(pixels);
    QString why;
    SECTION("locked, then unlocked") {
        LayerProperties p = doc->active()->props;
        p.locked = true;
        REQUIRE(doc->setLayerProperties(pixels, p, QStringLiteral("Lock Layer")));
        p.locked = false;
        REQUIRE(doc->setLayerProperties(pixels, p, QStringLiteral("Unlock Layer")));
        why = QStringLiteral("the layer is locked");
    }
    SECTION("hidden, then shown") {
        REQUIRE(doc->setLayerVisible(pixels, false));
        REQUIRE(doc->setLayerVisible(pixels, true));
        why = QStringLiteral("the layer is hidden");
    }
    QString error;
    REQUIRE(f.w.placeStamp(path, {4, 4}, &error));
    act(f.w, "undo")->trigger();  // the lock or the hide is back, under the float
    REQUIRE(s->floating());
    const Canvas layer = doc->layerCanvas();
    const int steps = doc->undoStack()->count();
    act(f.w, "rotateClockwise")->trigger();  // the glyphs would land now
    press(v, Qt::Key_Return);
    CHECK(str(f.w.statusBar()->currentMessage()) ==
          str(QStringLiteral("Could not place %1: %2; press Esc to drop it")
                  .arg(QString::fromStdString(path.string()), why)));
    CHECK(s->floating());
    CHECK(doc->layerCanvas() == layer);
    CHECK(doc->undoStack()->count() == steps);
    press(v, Qt::Key_Escape);
    CHECK_FALSE(s->floating());
    CHECK(doc->layerCanvas() == layer);
}

TEST_CASE("a lift turned on its side is refused in every paste mode, opaque or a half pixel (#433)",
          "[library][layers]") {
    const PasteMode mode = GENERATE(PasteMode::Normal, PasteMode::Transparent, PasteMode::Under);
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    CellEdit e;
    SECTION("an opaque ▀") {
        e.changes.push_back(
            {3, 3, doc->layerCanvas().at(3, 3), pixelLayerCell(LayerPixels{std::uint8_t(Red), std::uint8_t(Blue)})});
    }
    SECTION("a half pixel") {
        e.changes.push_back({3, 3, doc->layerCanvas().at(3, 3), pixelLayerCell(LayerPixels{std::uint8_t(Red), {}})});
    }
    // a pixel where the float goes down, which Under mode doesn't write over
    e.changes.push_back(
        {4, 3, doc->layerCanvas().at(4, 3), pixelLayerCell(LayerPixels{std::uint8_t(Blue), std::uint8_t(Blue)})});
    REQUIRE(doc->edit(e));
    const Canvas layer = doc->layerCanvas();
    const int steps = doc->undoStack()->count();
    f.w.tools()->setActive(s);
    s->setPasteMode(mode);
    s->select({3, 3, 1, 1});
    press(v, Qt::Key_Right);
    REQUIRE(s->floating());
    act(f.w, "rotateClockwise")->trigger();  // ▀ → ▐
    press(v, Qt::Key_Return);
    INFO("mode " << int(mode) << ": " << str(f.w.statusBar()->currentMessage()));
    CHECK(s->floating());
    CHECK(doc->layerCanvas() == layer);
    CHECK(doc->undoStack()->count() == steps);
}

TEST_CASE("a paste or a lift on a Pixels layer gets the same landing check (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId background = doc->activeLayer();
    const LayerId pixels = doc->addLayer(LayerKind::Pixels);
    REQUIRE(pixels);
    const QString unnamed = QStringLiteral(
        "Could not put the floating selection down: a Pixels layer holds no %1; rotate it (Selection > Rotate) or "
        "press Esc to drop it");

    SECTION("a ▌ copied from a cells layer") {
        REQUIRE(doc->setActiveLayer(background));
        write(f.w, 0, 0, QStringLiteral("▌"));
        f.w.tools()->setActive(s);
        s->select({0, 0, 1, 1});
        s->copy();
        s->deselect();
        REQUIRE(doc->setActiveLayer(pixels));
        const Canvas canvas = doc->canvas();
        const int steps = doc->undoStack()->count();
        v->setCellCursor({6, 2});
        s->paste();
        REQUIRE(s->floating());
        press(v, Qt::Key_Return);
        CHECK(str(f.w.statusBar()->currentMessage()) == str(unnamed.arg(QStringLiteral("▌"))));
        CHECK(s->floating());
        CHECK(doc->canvas() == canvas);
        CHECK(doc->undoStack()->count() == steps);
        act(f.w, "rotateClockwise")->trigger();
        press(v, Qt::Key_Return);
        CHECK_FALSE(s->floating());
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(doc->canvas().at(6, 2).ch == Glyph(U'▀'));
    }
    SECTION("a ▀ lifted and turned on its side") {
        CellEdit e;
        Cell c;
        c.ch = U'▀';
        c.fg = Color::indexed(Red);
        c.bg = Color::indexed(Blue);
        e.changes.push_back({3, 3, doc->layerCanvas().at(3, 3), c});
        REQUIRE(doc->edit(e));
        const Canvas canvas = doc->canvas();
        const int steps = doc->undoStack()->count();
        f.w.tools()->setActive(s);
        s->select({3, 3, 1, 1});
        press(v, Qt::Key_Right);
        REQUIRE(s->floating());
        act(f.w, "rotateClockwise")->trigger();
        press(v, Qt::Key_Return);
        CHECK(str(f.w.statusBar()->currentMessage()) == str(unnamed.arg(QStringLiteral("▐"))));
        CHECK(s->floating());
        CHECK(doc->canvas() == canvas);
        CHECK(doc->undoStack()->count() == steps);
        // Esc puts it back where it was lifted from, as it was
        press(v, Qt::Key_Escape);
        CHECK_FALSE(s->floating());
        CHECK(halfBlocks(doc->canvas().at(3, 3)) == HalfPair{Red, Blue});
    }
}

TEST_CASE("a refused stamp leaves a block that already floats floating", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    write(f.w, 0, 0, QStringLiteral("ab"));
    SelectTool* s = f.w.selectTool();
    f.w.tools()->setActive(s);
    s->select({0, 0, 2, 1});
    s->copy();
    f.w.canvasView()->setCellCursor({5, 3});
    s->paste();
    REQUIRE(s->floating());
    LayerProperties props = doc->active()->props;
    props.locked = true;
    REQUIRE(doc->setLayerProperties(doc->activeLayer(), props, QStringLiteral("Lock")));
    // the lock is a document change: it cancels the paste, so float it again
    f.w.tools()->setActive(s);
    s->paste();
    REQUIRE(s->floating());
    const int steps = doc->undoStack()->count();
    QString error;
    CHECK_FALSE(f.w.placeStamp(bwtest::dataDir() / "library" / "sky" / "small-star.ans", {10, 5}, &error));
    CHECK(error == QStringLiteral("the layer is locked"));
    CHECK(s->floating());
    CHECK(s->floatingPos() == QPoint(5, 3));
    CHECK(doc->undoStack()->count() == steps);
}

// Fallback review round 1 on #435: the check comes after the tool before
// has landed what it holds, so the Text tool's emptied new layer is gone
// and the stamp goes on the layer under it.
TEST_CASE("a stamp placed while the Text tool holds the emptied text layer it made lands on the layer under it",
          "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    const LayerId background = doc->activeLayer();
    const size_t layers = doc->layers().size();
    f.w.tools()->setActive(QStringLiteral("text"));
    {
        CanvasView* v = f.w.canvasView();
        const QPointF at = v->cellRect(QPoint(2, 1)).center();
        for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent e(type, at, v->mapToGlobal(at), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::MouseButtons(Qt::LeftButton) : Qt::NoButton,
                          Qt::NoModifier);
            QApplication::sendEvent(v, &e);
        }
    }
    press(f.w.canvasView(), Qt::Key_A, QStringLiteral("A"));
    press(f.w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
    REQUIRE(doc->layers().size() == layers + 1);
    REQUIRE(doc->active()->props.text);
    REQUIRE(doc->active()->props.text->text.empty());

    QString error;
    CHECK(f.w.placeStamp(bwtest::dataDir() / "library" / "sky" / "small-star.ans", {10, 5}, &error));
    INFO("error: " << error.toStdString());
    CHECK(error.isEmpty());
    CHECK(doc->activeLayer() == background);
    CHECK(doc->layers().size() == layers);
    REQUIRE(f.w.selectTool()->floating());
    press(f.w.canvasView(), Qt::Key_Return);
    CHECK(doc->canvas().at(12, 5).ch == Glyph(U'·'));
}

TEST_CASE("a stamp that can't be read is reported, not placed") {
    Fixture f;
    QString error;
    CHECK_FALSE(f.w.placeStamp(QStringLiteral("/no/such/stamp.ans"), {0, 0}, &error));
    CHECK_FALSE(error.isEmpty());
    CHECK_FALSE(f.w.selectTool()->floating());
}

TEST_CASE("a stamp edited on disk renders a new thumbnail after a rescan") {
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("ab"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    QString error;
    REQUIRE(!f.w.saveSelectionToLibrary(QStringLiteral("Edit Me"), QStringLiteral("test"), {}, &error).isEmpty());
    const int renders = f.dock->thumbnailRenders();
    write(f.w, 0, 0, QStringLiteral("cd"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    REQUIRE(!f.w.saveSelectionToLibrary(QStringLiteral("Edit Me"), QStringLiteral("test"), {}, &error, true)
                 .isEmpty());
    CHECK(f.dock->thumbnailRenders() == renders + 1);
    const library::Stamp* s = f.dock->find(fspath(f.w.libraryPathFor(QStringLiteral("Edit Me"))));
    REQUIRE(s);
    CHECK(s->art.canvas.at(0, 0).ch == U'c');
    // Only icons are cached, one per stamp: the old version's is gone.
    CHECK(f.dock->thumbnailCache().size() == qsizetype(f.dock->entries().size()));
    for (const QImage& img : f.dock->thumbnailCache()) {
        CHECK(img.width() <= 96);
        CHECK(img.height() <= 64);
    }
}

TEST_CASE("the thumbnail cache holds icons, not the full 1x render") {
    Fixture f;
    // Tux is 240 x 272 at 1x.
    REQUIRE(f.dock->thumbnailCache().size() == 2);
    for (const QImage& img : f.dock->thumbnailCache()) {
        CHECK(img.width() <= 96);
        CHECK(img.height() <= 64);
    }
    // A library that no longer has a stamp forgets its icon.
    f.dock->setDirectories({}, fspath(f.user.path()));
    CHECK(f.dock->thumbnailCache().isEmpty());
}

TEST_CASE("names that share a slug don't replace each other") {
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("ab"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    QString error;
    const QString heart = f.w.saveSelectionToLibrary(QStringLiteral("♥"), QStringLiteral("test"), {}, &error);
    const QString smile = f.w.saveSelectionToLibrary(QStringLiteral("☺"), QStringLiteral("test"), {}, &error);
    CHECK(QDir::cleanPath(heart) == QDir(f.user.path()).filePath(QStringLiteral("u2665.ans")));
    CHECK(QDir::cleanPath(smile) == QDir(f.user.path()).filePath(QStringLiteral("u263a.ans")));
    CHECK(f.dock->shownNames().contains(QStringLiteral("♥")));
    CHECK(f.dock->shownNames().contains(QStringLiteral("☺")));
    const QString oak = f.w.saveSelectionToLibrary(QStringLiteral("Oak Tree"), QStringLiteral("test"), {}, &error);
    const QString oak2 = f.w.saveSelectionToLibrary(QStringLiteral("oak-tree"), QStringLiteral("test"), {}, &error);
    CHECK(QDir::cleanPath(oak) == QDir(f.user.path()).filePath(QStringLiteral("oak-tree.ans")));
    CHECK(QDir::cleanPath(oak2) == QDir(f.user.path()).filePath(QStringLiteral("oak-tree-2.ans")));
    // The same name again is the one that asks to be replaced.
    bool exists = false;
    CHECK(f.w.libraryPathFor(QStringLiteral("♥"), &exists) == heart);
    CHECK(exists);
    CHECK(f.w.saveSelectionToLibrary(QStringLiteral("♥"), QStringLiteral("test"), {}, &error).isEmpty());
    CHECK(error.contains(QStringLiteral("already exists")));
    CHECK(f.w.saveSelectionToLibrary(QStringLiteral("♥"), QStringLiteral("test"), {}, &error, true) == heart);
}

TEST_CASE("an all-blank selection is not saved as a stamp") {
    Fixture f;
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 4, 2});
    QString error;
    CHECK(f.w.saveSelectionToLibrary(QStringLiteral("Nothing"), QStringLiteral("test"), {}, &error).isEmpty());
    CHECK(error.contains(QStringLiteral("blank")));
    CHECK_FALSE(QFileInfo::exists(QDir(f.user.path()).filePath(QStringLiteral("nothing.ans"))));
}

TEST_CASE("the dock reports a stamp whose byte order mark its sidecar doesn't back") {
    // #336: a mark says UTF-8, so a sidecar with no "encoding" key is a
    // problem, not a stamp placed misread with ∩╗┐ on its first row
    Fixture f;
    const fs::path user = fspath(f.user.path());
    CellBlock b(2, 1);
    b.at(0, 0).ch = U'#';
    const library::StampFiles plain = library::encodeStamp("Hash", "test", {}, b, true);
    library::writeStamp(user / "hash.ans", {std::string("\xEF\xBB\xBF") + plain.ans, plain.json});
    f.dock->rescan();
    CHECK(f.dock->problems().size() == 1);
    CHECK(f.dock->problemLabel()->text() == QStringLiteral("1 file skipped"));
    CHECK(f.dock->problemLabel()->toolTip().contains(QStringLiteral("starts with a byte order mark")));
    for (int i = 0; i < f.dock->grid()->count(); ++i)
        CHECK(f.dock->grid()->item(i)->text() != QStringLiteral("Hash"));
}

TEST_CASE("the dock reports links it skipped") {
    // std::filesystem's links: QFile::link makes .lnk shortcuts on Windows.
    if (!bwtest::symlinksWork())
        SKIP("the symbolic-link tests don't run here: no right to make links (fs_quirks.h)");
    Fixture f;
    QTemporaryDir shared;
    const auto in = [&](const char* name) { return QDir(f.user.path()).filePath(QLatin1String(name)).toStdWString(); };
    std::filesystem::create_directory_symlink(shared.path().toStdWString(), in("linked"));
    std::filesystem::create_symlink(in("gone.ans"), in("broken.ans"));
    f.dock->rescan();
    CHECK(f.dock->problems().size() == 2);
    CHECK(f.dock->problemLabel()->text() == QStringLiteral("2 files skipped"));
    CHECK(f.dock->problemLabel()->toolTip().contains(QStringLiteral("symbolic link to a folder, not followed")));
    CHECK(f.dock->problemLabel()->toolTip().contains(QStringLiteral("broken symbolic link, skipped")));
}

TEST_CASE("Selection > Save to Library asks for the name, category and tags") {
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("tree"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 4, 1});
    const int edits = f.w.document()->undoStack()->count();
    bool ran = false;
    QTimer::singleShot(0, &f.w, [&] {
        auto* d = qobject_cast<StampDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        ran = true;
        QPushButton* ok = d->buttons()->button(QDialogButtonBox::Ok);
        CHECK_FALSE(ok->isEnabled());  // no name yet
        // The existing categories are offered.
        CHECK(d->categoryBox()->findText(QStringLiteral("figures")) >= 0);
        d->nameEdit()->setText(QStringLiteral("Big ★"));
        CHECK_FALSE(ok->isEnabled());  // no CP437 glyph
        d->nameEdit()->setText(QStringLiteral("  Oak Tree "));
        CHECK_FALSE(ok->isEnabled());  // no category
        d->categoryBox()->setCurrentText(QStringLiteral("trees"));
        d->tagsEdit()->setText(QStringLiteral("oak, , tree,oak"));
        CHECK(d->tags() == QStringList{QStringLiteral("oak"), QStringLiteral("tree")});
        REQUIRE(ok->isEnabled());
        ok->click();
    });
    act(f.w, "saveToLibrary")->trigger();
    CHECK(ran);
    const library::Stamp* s = f.dock->find(fspath(QDir(f.user.path()).filePath(QStringLiteral("oak-tree.ans"))));
    REQUIRE(s);
    CHECK(s->name == "Oak Tree");
    CHECK(s->meta.category == "trees");
    CHECK(s->meta.tags == std::vector<std::string>{"oak", "tree"});
    CHECK(s->art.canvas.width() == 4);
    // Saving doesn't touch the canvas or the selection.
    CHECK(f.w.document()->undoStack()->count() == edits);
    CHECK(f.w.canvasView()->selection().bounds() == CellRect{0, 0, 4, 1});
}

TEST_CASE("Save to Library on a float turned past the width limit reports it") {
    // The Codex repro: a 1x401 document, Select All, copy, paste, rotate
    // clockwise: the float is 401x1, wider than .ans allows.
    Fixture f;
    QTemporaryDir dir;
    AnsiDocument doc;
    doc.canvas = Canvas(1, 401);
    for (int y = 0; y < 401; ++y)
        doc.canvas.at(0, y).ch = U'#';
    doc.sauce = Sauce::fresh(1, 401);
    const QString file = dir.filePath(QStringLiteral("tall.ans"));
    {
        QFile out(file);
        REQUIRE(out.open(QIODevice::WriteOnly));
        out.write(QByteArray::fromStdString(writeAnsi(doc).bytes));
    }
    QString error;
    REQUIRE(f.w.openFile(file, &error));
    REQUIRE(f.w.document()->canvas().height() == 401);
    SelectTool* s = f.w.selectTool();
    f.w.tools()->setActive(s);
    s->selectAll();
    s->copy();
    f.w.canvasView()->setCellCursor({0, 0});
    s->paste();
    s->transform(Transform::RotateClockwise);
    REQUIRE(s->floating());
    REQUIRE(s->floatingBlock().width == 401);
    QString saved;
    REQUIRE_NOTHROW(saved = f.w.saveSelectionToLibrary(QStringLiteral("Tall"), QStringLiteral("test"), {}, &error));
    CHECK(saved.isEmpty());
    CHECK(error == QStringLiteral("the stamp is 401 x 1 cells; a stamp is at most 400 x 5000"));
    CHECK(s->floating());  // nothing else happened
}

TEST_CASE("a stamp's icon is its 1x render, or that render averaged down, whatever the band size") {
    const AnsiDocument star =
        parseAnsi(bwtest::readFile(bwtest::dataDir() / "library" / "sky" / "small-star.ans"));
    const AnsiDocument tux = parseAnsi(bwtest::readFile(bwtest::dataDir() / "library" / "figures" / "tux.ans"));
    // Fits: the 1x render itself.
    CHECK(stampIcon(star.canvas, 8, kIconBox) == renderCanvas(star.canvas, 8));
    CHECK(stampIcon(star.canvas, 8, kIconBox, 1) == renderCanvas(star.canvas, 8));

    // Doesn't fit: the same icon from one band or from a row at a time.
    const QImage whole = stampIcon(tux.canvas, 8, kIconBox, std::int64_t(1) << 40);
    REQUIRE(whole.size() == QSize(240, 272).scaled(kIconBox, Qt::KeepAspectRatio));
    for (const std::int64_t band : {std::int64_t(1), std::int64_t(240 * 16 * 3), std::int64_t(240 * 16 * 5)}) {
        INFO("band " << band);
        CHECK(stampIcon(tux.canvas, 8, kIconBox, band) == whole);
    }
    // And it is the full 1x render averaged into each icon pixel.
    const QImage full = renderCanvas(tux.canvas, 8).convertToFormat(QImage::Format_RGB32);
    const int iw = whole.width(), ih = whole.height();
    for (const QPoint at : {QPoint(0, 0), QPoint(iw / 2, ih / 2), QPoint(iw - 1, ih - 1), QPoint(iw / 3, ih - 5)}) {
        INFO(at.x() << "," << at.y());
        std::uint64_t sum[3] = {0, 0, 0}, n = 0;
        for (int y = 0; y < full.height(); ++y)
            for (int x = 0; x < full.width(); ++x)
                if (x * iw / full.width() == at.x() && y * ih / full.height() == at.y()) {
                    const QRgb c = full.pixel(x, y);
                    sum[0] += qRed(c);
                    sum[1] += qGreen(c);
                    sum[2] += qBlue(c);
                    ++n;
                }
        REQUIRE(n > 0);
        const QRgb got = whole.pixel(at);
        CHECK(qRed(got) == int((sum[0] + n / 2) / n));
        CHECK(qGreen(got) == int((sum[1] + n / 2) / n));
        CHECK(qBlue(got) == int((sum[2] + n / 2) / n));
    }
}

TEST_CASE("a drop naming something that isn't a regular file is refused at once") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    Fixture f;
    QTemporaryDir dir;
    const QString fifo = dir.filePath(QStringLiteral("pipe.ans"));
    REQUIRE(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
    QString error;
    CHECK_FALSE(f.w.placeStamp(fifo, {0, 0}, &error));
    CHECK(error == QStringLiteral(".ans is not a regular file"));
    QMimeData mime;
    mime.setData(QString::fromLatin1(kStampMimeType), fifo.toUtf8());
    CHECK(drop(f.w.canvasView(), &mime, {2, 2}));
    CHECK_FALSE(f.w.selectTool()->floating());
#endif
}

TEST_CASE("the dock reports a folder it can't read") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root reads every folder");
    Fixture f;
    const QString locked = QDir(f.user.path()).filePath(QStringLiteral("locked"));
    REQUIRE(QDir().mkpath(locked));
    REQUIRE(QFile::setPermissions(locked, {}));
    f.dock->rescan();
    QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    CHECK(f.dock->problemLabel()->text() == QStringLiteral("1 file skipped"));
    CHECK(f.dock->problemLabel()->toolTip().contains(QStringLiteral("folder can't be read")));
#endif
}

TEST_CASE("Save to Library refuses a library folder it can't list") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root lists every folder");
    // The Codex P1 repro: Oak Tree saved, the folder set to 0300, then a
    // different stamp named oak-tree.
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("ab"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    QString error;
    REQUIRE(!f.w.saveSelectionToLibrary(QStringLiteral("Oak Tree"), QStringLiteral("test"), {}, &error).isEmpty());
    const fs::path user = fspath(f.user.path());
    fs::permissions(user, fs::perms::owner_write | fs::perms::owner_exec);
    QString saved, shown, shownError;
    REQUIRE_NOTHROW(saved = f.w.saveSelectionToLibrary(QStringLiteral("oak-tree"), QStringLiteral("test"), {},
                                                        &error));
    REQUIRE_NOTHROW(shown = f.w.libraryPathFor(QStringLiteral("oak-tree"), nullptr, &shownError));
    fs::permissions(user, fs::perms::owner_all);
    CHECK(saved.isEmpty());
    CHECK(error.contains(QStringLiteral("can't be listed")));
    CHECK(shown.isEmpty());
    CHECK(shownError.contains(QStringLiteral("can't be listed")));
    CHECK(library::readStamp(user / "oak-tree.ans").name == "Oak Tree");
#endif
}

TEST_CASE("stamps whose file names aren't UTF-8 are told apart") {
#ifndef __linux__
    SKIP("byte file names are a Linux test");
#else
    // The Codex repro: a\xfe.ans and a\xff.ans decode to the same QString.
    Fixture f;
    const fs::path user = fspath(f.user.path());
    CellBlock two(2, 1), three(3, 1);
    two.at(0, 0).ch = U'#';
    three.at(0, 0).ch = U'#';
    library::writeStamp(user / "a\xfe.ans", library::encodeStamp("Two", "test", {}, two, true));
    library::writeStamp(user / "a\xff.ans", library::encodeStamp("Three", "test", {}, three, true));
    f.dock->rescan();
    REQUIRE(f.dock->problems().empty());
    SelectTool* s = f.w.selectTool();
    for (const auto& [name, width] : {std::pair{QStringLiteral("Two"), 2}, std::pair{QStringLiteral("Three"), 3}}) {
        INFO(name.toStdString());
        emit f.dock->grid()->itemActivated(item(f.dock, name));
        REQUIRE(s->floating());
        CHECK(s->floatingBlock().width == width);
        press(f.w.canvasView(), Qt::Key_Escape);
        // And by drag and drop.
        const std::unique_ptr<QMimeData> mime = dragged(f.dock, name);
        CHECK(drop(f.w.canvasView(), mime.get(), {5, 5}));
        REQUIRE(s->floating());
        CHECK(s->floatingBlock().width == width);
        press(f.w.canvasView(), Qt::Key_Escape);
    }
    CHECK(f.dock->find(user / "a\xfe.ans")->name == "Two");
    CHECK(f.dock->find(user / "a\xff.ans")->name == "Three");
    CHECK(stampKeyPath(stampKey(user / "a\xff.ans")) == user / "a\xff.ans");
#endif
}

TEST_CASE("a drop whose payload is no stamp in the dock places nothing") {
    // Codex round 3 P1: the MIME type can come from any application.
    Fixture f;
    QByteArray noise(1024, '\0');
    std::uint32_t x = 12345;
    for (char& c : noise) {
        x = x * 1103515245u + 12345u;
        c = char(x >> 24);
    }
    const QString starPath = QString::fromStdString((bwtest::dataDir() / "library" / "sky" / "small-star.ans").string());
    QTemporaryDir dir;
    const QString elsewhere = dir.filePath(QStringLiteral("loose.ans"));
    REQUIRE(QFile::copy(starPath, elsewhere));
    REQUIRE(QFile::copy(QString(starPath).replace(QStringLiteral(".ans"), QStringLiteral(".json")),
                        dir.filePath(QStringLiteral("loose.json"))));
    for (const QByteArray& payload : {QByteArray(1, char(0xff)), noise, QByteArray(), elsewhere.toUtf8()}) {
        INFO(payload.size() << " bytes");
        QMimeData mime;
        mime.setData(QString::fromLatin1(kStampMimeType), payload);
        bool accepted = false;
        REQUIRE_NOTHROW(accepted = drop(f.w.canvasView(), &mime, {1, 1}));
        CHECK(accepted);
        CHECK_FALSE(f.w.selectTool()->floating());
        CHECK(f.w.statusBar()->currentMessage() == QStringLiteral("Not a stamp in the Library; nothing placed"));
    }
    // A stamp from the dock still drops.
    const std::unique_ptr<QMimeData> mime = dragged(f.dock, QStringLiteral("Small star"));
    CHECK(drop(f.w.canvasView(), mime.get(), {1, 1}));
    CHECK(f.w.selectTool()->floating());
}

TEST_CASE("a stamp replaced under an upper-case extension stays in the dock") {
    // The Codex round 4 repro through Save to Library.
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("ab"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    QString error;
    REQUIRE(!f.w.saveSelectionToLibrary(QStringLiteral("Oak Tree"), QStringLiteral("test"), {}, &error).isEmpty());
    const fs::path user = fspath(f.user.path());
    fs::rename(user / "oak-tree.ans", user / "oak-tree.ANS");
    if (!bwtest::caseRenamed(user / "oak-tree.ANS"))
        SKIP("this file system keeps a file's old case through a rename");
    f.dock->rescan();
    REQUIRE(f.dock->shownNames().contains(QStringLiteral("Oak Tree")));
    write(f.w, 0, 0, QStringLiteral("cde"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 3, 1});
    const QString saved =
        f.w.saveSelectionToLibrary(QStringLiteral("Oak Tree"), QStringLiteral("test"), {}, &error, true);
    INFO(error.toStdString());
    CHECK(fspath(saved) == user / "oak-tree.ANS");
    CHECK(f.dock->problems().empty());
    const library::Stamp* s = f.dock->find(user / "oak-tree.ANS");
    REQUIRE(s);
    CHECK(s->art.canvas.width() == 3);
    CHECK(f.dock->shownNames().count(QStringLiteral("Oak Tree")) == 1);
}

// Phase 2c's "done when" (PLAN §13): a tree from the bundled Library dropped
// onto a blank canvas floats, moves, and lands where it was moved to, with
// its transparent cells leaving the canvas as it was. The tree is one that
// shows on the default black canvas, and the test checks the render changed.
TEST_CASE("a bundled tree is placed on a blank canvas, moved, then committed") {
    Fixture f;
    const fs::path tree = fs::path(BW_BUNDLED_LIBRARY) / "trees" / "oak-tree.ans";
    const library::Stamp stamp = library::readStamp(tree);
    const Canvas& art = stamp.art.canvas;
    const QImage before = renderCanvas(f.w.document()->canvas(), 8);
    QString error;
    REQUIRE(f.w.placeStamp(QString::fromStdString(tree.string()), {2, 1}, &error));
    SelectTool* s = f.w.selectTool();
    REQUIRE(s->floating());
    CHECK(s->effectivePasteMode() == PasteMode::Transparent);
    REQUIRE(f.w.document()->canvas().height() >= 3 + art.height());
    REQUIRE(f.w.document()->canvas().width() >= 5 + art.width());
    s->moveBy(3, 2);
    CHECK(s->floatingPos() == QPoint(5, 3));
    CHECK(f.w.document()->canvas().at(5, 3) == Cell{});  // nothing lands until it is committed
    s->stamp();
    CHECK_FALSE(s->floating());
    int drawn = 0;
    for (int y = 0; y < art.height(); ++y)
        for (int x = 0; x < art.width(); ++x) {
            const Cell& got = f.w.document()->canvas().at(5 + x, 3 + y);
            if (isBlank(art.at(x, y))) {
                CHECK(got == Cell{});
            } else {
                CHECK(got == iceCell(art.at(x, y), stamp.art.ice));
                ++drawn;
            }
        }
    CHECK(drawn > 100);
    // The tree shows: the render changed inside the placed rectangle, and
    // only there.
    const QImage after = renderCanvas(f.w.document()->canvas(), 8);
    REQUIRE(after.size() == before.size());
    const QRect placed(5 * 8, 3 * 16, art.width() * 8, art.height() * 16);
    int changed = 0, outside = 0;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (after.pixel(x, y) != before.pixel(x, y))
                ++(placed.contains(x, y) ? changed : outside);
    CHECK(changed > art.width() * art.height() * 8 * 16 / 4);
    CHECK(outside == 0);
}

TEST_CASE("Save to Library on a float lifted off a pixel layer writes its pixels as they show and without blink") {
    Fixture f;
    Document* doc = f.w.document();
    REQUIRE(doc->setIce(false));
    doc->addLayer(LayerKind::Pixels);
    // two bright pixels in one cell (stored ▀ fg 12 bg 14), and a red top pixel
    const Cell both = pixelLayerCell(LayerPixels{LightRed, Yellow});
    const Cell top = pixelLayerCell(LayerPixels{Red, std::nullopt});
    REQUIRE(doc->edit(CellEdit{{{5, 0, Cell{}, both}, {6, 0, Cell{}, top}}}));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({5, 0, 2, 1});
    f.w.selectTool()->moveBy(0, 0);
    REQUIRE(f.w.selectTool()->floating());
    QString error;
    const QString path = f.w.saveSelectionToLibrary(QStringLiteral("Pixels"), QStringLiteral("test"), {}, &error);
    INFO(error.toStdString());
    REQUIRE(!path.isEmpty());
    const library::Stamp read = library::readStamp(fspath(path));
    REQUIRE_FALSE(read.art.ice);
    const Cell a = read.art.canvas.at(0, 0), b = read.art.canvas.at(1, 0);
    INFO("a: ch " << a.ch << " fg " << int(a.fg.paletteIndex()) << " bg " << int(a.bg.paletteIndex())
                  << " attrs " << int(a.attrs));
    // as the pixel layer showed it with iCE off: the bottom goes dark, nothing blinks
    CHECK_FALSE(a.attrs & attr::Blink);
    CHECK(halfBlocks(a, false) == HalfPair{LightRed, Brown});
    // the half pixel is kept, over black
    INFO("b: ch " << b.ch << " fg " << int(b.fg.paletteIndex()) << " bg " << int(b.bg.paletteIndex()));
    CHECK(halfBlocks(b, false) == HalfPair{Red, Black});
}

// #434: with nothing floating, Save to Library takes what the visible layers
// show under the selection (a stamp is what you see), while Copy takes the
// active layer's cells (it edits the layer being worked on). With a float
// both take the float, which was lifted off the active layer.
namespace {

Cell letter(char ch, PaletteIndex fg) {
    Cell c;
    c.ch = char32_t(ch);
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(Black);
    return c;
}

// A bottom cells layer with "ABCD" across (0, 0)-(3, 0), and a cells layer
// above it, active, with X at (1, 0) and Y at (3, 0) and clear cells at (0, 0)
// and (2, 0). The selection is (0, 0, 4, 1), over both. Returns the bottom
// layer's id.
LayerId twoLayers(Fixture& f) {
    Document* doc = f.w.document();
    const LayerId bottom = doc->activeLayer();
    REQUIRE(doc->edit(CellEdit{{{0, 0, Cell{}, letter('A', Green)},
                                {1, 0, Cell{}, letter('B', Green)},
                                {2, 0, Cell{}, letter('C', Green)},
                                {3, 0, Cell{}, letter('D', Green)}}}));
    doc->addLayer(LayerKind::Cells);
    REQUIRE(doc->activeLayer() != bottom);
    REQUIRE(doc->edit(CellEdit{{{1, 0, transparentCell(), letter('X', LightRed)},
                                {3, 0, transparentCell(), letter('Y', LightRed)}}}));
    REQUIRE(doc->canvas().at(0, 0) == letter('A', Green));
    REQUIRE(doc->canvas().at(1, 0) == letter('X', LightRed));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 4, 1});
    return bottom;
}

// The glyphs of a saved stamp's first row.
std::string savedRow(Fixture& f, const QString& name) {
    QString error;
    const QString path = f.w.saveSelectionToLibrary(name, QStringLiteral("test"), {}, &error);
    INFO(error.toStdString());
    REQUIRE(!path.isEmpty());
    const library::Stamp read = library::readStamp(fspath(path));
    std::string row;
    for (int x = 0; x < read.art.canvas.width(); ++x)
        row += char(read.art.canvas.at(x, 0).ch.codePoint().value());
    return row;
}

} // namespace

TEST_CASE("Save to Library takes the visible layers' composite; a hidden layer is left out (#434)") {
    Fixture f;
    const LayerId bottom = twoLayers(f);
    Document* doc = f.w.document();
    REQUIRE_FALSE(f.w.selectTool()->floating());

    // the bottom layer shows through the top layer's clear cells
    CHECK(savedRow(f, QStringLiteral("Both")) == "AXCY");

    // hidden, the bottom layer is left out: the clear cells save as blanks
    REQUIRE(doc->setLayerVisible(bottom, false));
    REQUIRE_FALSE(f.w.selectTool()->floating());
    CHECK(savedRow(f, QStringLiteral("Top Only")) == " X Y");

    // a hidden active layer is left out too: what shows is the bottom layer
    REQUIRE(doc->setLayerVisible(bottom, true));
    REQUIRE(doc->setLayerVisible(doc->activeLayer(), false));
    CHECK(savedRow(f, QStringLiteral("Bottom Only")) == "ABCD");
}

TEST_CASE("Copy takes the active layer's cells only, whatever the other layers show (#434)") {
    Fixture f;
    const LayerId bottom = twoLayers(f);
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();

    const auto checkTop = [&] {
        const CellBlock& c = s->clipboard();
        REQUIRE(c.width == 4);
        REQUIRE(c.height == 1);
        CHECK(c.at(0, 0) == transparentCell());
        CHECK(c.at(1, 0) == letter('X', LightRed));
        CHECK(c.at(2, 0) == transparentCell());
        CHECK(c.at(3, 0) == letter('Y', LightRed));
    };
    s->copy();
    checkTop();

    // the bottom layer hidden changes nothing
    REQUIRE(doc->setLayerVisible(bottom, false));
    s->copy();
    checkTop();

    // with the bottom layer active, Copy takes its cells, not what shows
    REQUIRE(doc->setLayerVisible(bottom, true));
    REQUIRE(doc->setActiveLayer(bottom));
    s->select({0, 0, 4, 1});
    s->copy();
    const CellBlock& c = s->clipboard();
    REQUIRE(c.width == 4);
    CHECK(c.at(0, 0) == letter('A', Green));
    CHECK(c.at(1, 0) == letter('B', Green));
    CHECK(c.at(2, 0) == letter('C', Green));
    CHECK(c.at(3, 0) == letter('D', Green));
}

TEST_CASE("with a float, Save to Library and Copy both take the float, whatever lies under it (#434)") {
    Fixture f;
    const LayerId bottom = twoLayers(f);
    Document* doc = f.w.document();
    const LayerId top = doc->activeLayer();
    SelectTool* s = f.w.selectTool();

    SECTION("a float lifted off the active layer") {
        // lifted and moved one cell right, off the cells it came from: the
        // active layer under the selection is now X, clear, Y, clear and the
        // composite X C Y and a blank, neither of them the float
        s->moveBy(1, 0);
        REQUIRE(s->floating());
        REQUIRE(f.w.canvasView()->selection().bounds() == CellRect{1, 0, 4, 1});
        CHECK(savedRow(f, QStringLiteral("Float")) == " X Y");
        REQUIRE(s->floating());  // saving doesn't stamp it
        s->copy();
        const CellBlock& c = s->clipboard();
        REQUIRE(c.width == 4);
        CHECK(c.at(0, 0) == transparentCell());
        CHECK(c.at(1, 0) == letter('X', LightRed));
        CHECK(c.at(2, 0) == transparentCell());
        CHECK(c.at(3, 0) == letter('Y', LightRed));
    }

    SECTION("a paste of another layer's cells") {
        REQUIRE(doc->setActiveLayer(bottom));
        s->select({0, 0, 4, 1});
        s->copy();
        REQUIRE(doc->setActiveLayer(top));
        f.w.canvasView()->setCellCursor({0, 0});
        s->paste();
        REQUIRE(s->floating());
        // the bottom layer's cells, though the top layer is active
        CHECK(savedRow(f, QStringLiteral("Pasted")) == "ABCD");
        REQUIRE(s->floating());
        s->copy();
        const CellBlock& c = s->clipboard();
        REQUIRE(c.width == 4);
        CHECK(c.at(1, 0) == letter('B', Green));
        CHECK(c.at(3, 0) == letter('D', Green));
    }
}

// Resizable stamps (#345 slice 2): a stamp with slice insets floats with
// handles, and a drag on one re-cuts it to the size it reaches.
namespace {

Cell ascii(char ch) {
    Cell c;
    c.ch = char32_t(ch);
    c.fg = Color::indexed(Grey);
    c.bg = Color::indexed(Black);
    return c;
}

CellBlock asciiBlock(std::initializer_list<const char*> rows) {
    const int h = int(rows.size()), w = int(std::strlen(*rows.begin()));
    CellBlock b(w, h);
    int y = 0;
    for (const char* r : rows) {
        for (int x = 0; x < w; ++x)
            b.at(x, y) = ascii(r[x]);
        ++y;
    }
    return b;
}

// Writes `block` to the user library as a stamp with `slice` and places it
// at `cell`.
void placeResizable(Fixture& f, const CellBlock& block, const std::optional<StampSlice>& slice, QPoint cell) {
    const fs::path p = fspath(QDir(f.user.path()).filePath(QStringLiteral("piece.ans")));
    library::writeStamp(p, library::encodeStamp("Piece", "frames", {}, block, false, slice));
    f.dock->rescan();
    QString error;
    REQUIRE(f.w.placeStamp(p, cell, &error));
}

// A mouse event at `px`, in view pixels, with the left button.
void mouseAt(CanvasView* v, QEvent::Type type, QPoint px) {
    const QPointF at = v->toWidget(QRect(px, QSize(1, 1))).topLeft();
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

// The view pixel at the top-left corner of `cell`: the edge a handle goes to.
QPoint edgeOf(CanvasView* v, QPoint cell) { return v->cellRect(cell).topLeft(); }

void dragPx(CanvasView* v, QPoint from, QPoint to) {
    mouseAt(v, QEvent::MouseButtonPress, from);
    mouseAt(v, QEvent::MouseMove, to);
    mouseAt(v, QEvent::MouseButtonRelease, to);
}

QString canvasRow(MainWindow& w, int x, int y, int n) {
    QString s;
    for (int i = 0; i < n; ++i)
        s += at(w, x + i, y);
    return s;
}

} // namespace

TEST_CASE("a resizable stamp floats with handles; a drag re-cuts it and Enter stamps it as one undo step") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), StampSlice{1, 1, 1, 1, true, true}, {2, 2});
    REQUIRE(t->resizable());
    CHECK(t->floatingSlice() == StampSlice{1, 1, 1, 1, true, true});
    // eight handles: the corners and the edges' middles
    const QPoint br = edgeOf(v, {5, 5});
    CHECK(t->handleAt(br) == 4);
    CHECK(t->handleAt(edgeOf(v, {2, 2})) == 0);
    CHECK(t->handleAt(br + QPoint(0, -v->cellRect(QPoint(0, 0)).height() * 3 / 2)) == 3);
    CHECK(t->handleAt(v->cellRect(QPoint(3, 3)).center()) == -1);
    const int edits = f.w.document()->undoStack()->count();

    dragPx(v, br, edgeOf(v, {9, 7}));
    CHECK(t->floating());
    CHECK(t->floatingPos() == QPoint(2, 2));
    REQUIRE(t->floatingBlock().width == 7);
    REQUIRE(t->floatingBlock().height == 5);
    CHECK(f.w.document()->undoStack()->count() == edits);  // nothing landed yet
    CHECK(at(f.w, 8, 6) == QLatin1Char(' '));

    press(v, Qt::Key_Return);
    CHECK_FALSE(t->floating());
    CHECK(f.w.document()->undoStack()->count() == edits + 1);
    CHECK(canvasRow(f.w, 2, 2, 7) == QStringLiteral("+-----+"));
    CHECK(canvasRow(f.w, 2, 4, 7) == QStringLiteral("|     |"));
    CHECK(canvasRow(f.w, 2, 6, 7) == QStringLiteral("+-----+"));
    CHECK(v->selection().bounds() == CellRect{2, 2, 7, 5});

    f.w.document()->undoStack()->undo();
    CHECK(canvasRow(f.w, 2, 2, 7) == QStringLiteral("       "));
    CHECK(canvasRow(f.w, 2, 6, 7) == QStringLiteral("       "));
}

TEST_CASE("a drag on a resizable stamp's top-left handle keeps its far corner, down to the minimum") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), StampSlice{1, 1, 1, 1, true, true}, {4, 4});
    // past the far corner: the minimum, the bottom-right staying put
    dragPx(v, edgeOf(v, {4, 4}), edgeOf(v, {12, 12}));
    CHECK(t->floatingBlock().width == 2);
    CHECK(t->floatingBlock().height == 2);
    CHECK(t->floatingPos() == QPoint(5, 5));
    // and out again: growing back loses nothing
    dragPx(v, edgeOf(v, {5, 5}), edgeOf(v, {1, 2}));
    CHECK(t->floatingPos() == QPoint(1, 2));
    CHECK(t->floatingBlock() == *resizeStamp(asciiBlock({"+-+", "| |", "+-+"}), {1, 1, 1, 1, true, true}, 6, 5));
    // a size past the limit is refused, and the float stays as it was
    CHECK_FALSE(t->resizeFloat(QRect(0, 0, kMaxColumns + 1, 3)));
    CHECK(t->floatingBlock().width == 6);
}

TEST_CASE("a drag on a resizable stamp that loses focus puts the size back") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), StampSlice{1, 1, 1, 1, true, true}, {2, 2});
    mouseAt(v, QEvent::MouseButtonPress, edgeOf(v, {5, 5}));
    mouseAt(v, QEvent::MouseMove, edgeOf(v, {10, 9}));
    REQUIRE(t->floatingBlock().width == 8);
    QFocusEvent lost(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(v, &lost);
    CHECK_FALSE(t->dragging());
    CHECK(t->floating());
    CHECK(t->floatingBlock().width == 3);
    CHECK(t->floatingBlock().height == 3);
    CHECK(t->floatingPos() == QPoint(2, 2));
}

TEST_CASE("a resize takes the size where the button is let go, past the last move") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), StampSlice{1, 1, 1, 1, true, true}, {2, 2});
    mouseAt(v, QEvent::MouseButtonPress, edgeOf(v, {5, 5}));
    mouseAt(v, QEvent::MouseMove, edgeOf(v, {8, 6}));
    REQUIRE(t->floatingBlock().width == 6);
    REQUIRE(t->floatingBlock().height == 4);
    mouseAt(v, QEvent::MouseButtonRelease, edgeOf(v, {9, 7}));
    CHECK_FALSE(t->dragging());
    CHECK(t->floatingPos() == QPoint(2, 2));
    CHECK(t->floatingBlock().width == 7);
    CHECK(t->floatingBlock().height == 5);
}

TEST_CASE("a turn during a resize drag, then the undo key, puts the float back whole") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    REQUIRE(bwtest::showExposed(&f.w));
    v->setFocus();
    placeResizable(f, asciiBlock({"[ab]"}), StampSlice{1, 1, 0, 0, true, false}, {2, 2});
    const int edits = f.w.document()->undoStack()->count();
    const int half = v->cellRect(QPoint(0, 0)).height() / 2;
    mouseAt(v, QEvent::MouseButtonPress, edgeOf(v, {6, 2}) + QPoint(0, half));
    mouseAt(v, QEvent::MouseMove, edgeOf(v, {9, 2}) + QPoint(0, half));
    REQUIRE(t->floatingBlock().width == 7);
    bwtest::systemKey(v, Qt::Key_R, Qt::ControlModifier);
    REQUIRE(t->floatingBlock().width == 1);
    REQUIRE(t->floatingBlock().height == 7);
    CHECK(t->floatingSlice() == StampSlice{0, 0, 1, 1, false, true});
    bwtest::systemKey(v, Qt::Key_Z, Qt::ControlModifier);
    mouseAt(v, QEvent::MouseButtonRelease, edgeOf(v, {9, 2}) + QPoint(0, half));
    // the size, the place and the slice as the drag found them
    CHECK_FALSE(t->dragging());
    REQUIRE(t->floating());
    CHECK(t->floatingBlock() == asciiBlock({"[ab]"}));
    CHECK(t->floatingPos() == QPoint(2, 2));
    REQUIRE(t->floatingSlice());
    CHECK(*t->floatingSlice() == StampSlice{1, 1, 0, 0, true, false});
    CHECK(sliceProblem(t->floatingBlock(), *t->floatingSlice()).empty());
    CHECK(f.w.document()->undoStack()->count() == edits);
    // and its handles still resize it
    dragPx(v, edgeOf(v, {6, 2}) + QPoint(0, half), edgeOf(v, {11, 2}) + QPoint(0, half));
    CHECK(t->floatingBlock().width == 9);
    CHECK(t->floatingBlock().height == 1);
}

TEST_CASE("an arrow key during a resize drag moves the side that stays with the float") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    REQUIRE(bwtest::showExposed(&f.w));
    v->setFocus();
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), StampSlice{1, 1, 1, 1, true, true}, {2, 2});
    mouseAt(v, QEvent::MouseButtonPress, edgeOf(v, {5, 5}));
    mouseAt(v, QEvent::MouseMove, edgeOf(v, {8, 6}));
    REQUIRE(t->floatingBlock().width == 6);
    bwtest::systemKey(v, Qt::Key_Right);
    REQUIRE(t->floatingPos() == QPoint(3, 2));
    mouseAt(v, QEvent::MouseMove, edgeOf(v, {9, 7}));
    CHECK(t->floatingPos() == QPoint(3, 2));
    CHECK(t->floatingBlock().width == 6);
    CHECK(t->floatingBlock().height == 5);
    // the undo key puts back the place the drag found it at, and the size
    bwtest::systemKey(v, Qt::Key_Z, Qt::ControlModifier);
    mouseAt(v, QEvent::MouseButtonRelease, edgeOf(v, {9, 7}));
    CHECK(t->floatingPos() == QPoint(2, 2));
    CHECK(t->floatingBlock() == asciiBlock({"+-+", "| |", "+-+"}));
}

TEST_CASE("a turned float with repeats of several cells re-cuts as the stamp re-cut, then turned") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    const CellBlock big = asciiBlock({"AB12CD", "EF34GH", "ij56kl", "mn78op", "QR90ST", "UVwxYZ"});
    const StampSlice cut{2, 1, 1, 2, true, true};
    const Transform all[] = {Transform::FlipHorizontal, Transform::FlipVertical, Transform::RotateClockwise,
                             Transform::RotateCounterClockwise};
    for (const Transform turn : all) {
        for (const auto& [w, h] : {std::pair{9, 8}, std::pair{10, 13}, std::pair{4, 4}, std::pair{11, 5}}) {
            INFO(int(turn) << " at " << w << "x" << h);
            placeResizable(f, big, cut, {2, 2});
            t->transform(turn);
            const bool side = turn == Transform::RotateClockwise || turn == Transform::RotateCounterClockwise;
            REQUIRE(t->resizeFloat(QRect(2, 2, side ? h : w, side ? w : h)));
            CHECK(t->floatingBlock() == transformed(*resizeStamp(big, cut, w, h), turn));
            t->cancel();
        }
    }
    // two turns, and a real handle drag after them
    placeResizable(f, big, cut, {2, 2});
    t->transform(Transform::RotateClockwise);
    t->transform(Transform::FlipHorizontal);
    REQUIRE(t->floatingBlock().width == 6);
    dragPx(v, edgeOf(v, {8, 8}), edgeOf(v, {15, 11}));
    REQUIRE(t->floatingBlock().width == 13);
    REQUIRE(t->floatingBlock().height == 9);
    CHECK(t->floatingBlock() ==
          transformed(transformed(*resizeStamp(big, cut, 9, 13), Transform::RotateClockwise), Transform::FlipHorizontal));
}

TEST_CASE("Save to Library keeps a placed frame's insets past the column limit on the rows") {
    Fixture f;
    // a frame 452 rows tall whose top inset is 450 rows
    std::vector<const char*> rows(452, "|.|");
    rows.front() = "+-+";
    rows.back() = "+-+";
    CellBlock tall(3, 452);
    for (int y = 0; y < 452; ++y)
        for (int x = 0; x < 3; ++x)
            tall.at(x, y) = ascii(rows[std::size_t(y)][x]);
    const StampSlice deep{1, 1, 450, 1, true, true};
    REQUIRE(sliceProblem(tall, deep).empty());
    placeResizable(f, tall, deep, {0, 0});
    REQUIRE(f.w.selectTool()->floatingSlice() == deep);
    bool ran = false;
    QTimer::singleShot(0, &f.w, [&] {
        auto* d = qobject_cast<StampDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        ran = true;
        CHECK(d->insetBox(2)->value() == 450);
        CHECK(d->insetBox(2)->maximum() == kMaxRows);
        CHECK(d->insetBox(3)->maximum() == kMaxRows);
        CHECK(d->insetBox(0)->maximum() == kMaxColumns);
        CHECK(d->slice() == deep);
        d->nameEdit()->setText(QStringLiteral("Deep"));
        d->categoryBox()->setCurrentText(QStringLiteral("frames"));
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    act(f.w, "saveToLibrary")->trigger();
    REQUIRE(ran);
    const library::Stamp* s = f.dock->find(fspath(QDir(f.user.path()).filePath(QStringLiteral("deep.ans"))));
    REQUIRE(s);
    CHECK(s->meta.slice == deep);
    // and it resizes, though its source is past a frame style's
    placeResizable(f, tall, deep, {0, 0});
    REQUIRE(f.w.selectTool()->resizeFloat(QRect(0, 0, 4, 453)));
    CHECK(f.w.selectTool()->floatingBlock() == *resizeStamp(tall, deep, 4, 453));
}

TEST_CASE("a band floats with handles on its ends only and resizes in width") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"[ab]"}), StampSlice{1, 1, 0, 0, true, false}, {2, 2});
    REQUIRE(t->resizable());
    const int half = v->cellRect(QPoint(0, 0)).height() / 2;
    CHECK(t->handleAt(edgeOf(v, {2, 2})) == -1);  // no corners
    CHECK(t->handleAt(edgeOf(v, {6, 2}) + QPoint(0, half)) == 3);
    CHECK(t->handleAt(edgeOf(v, {2, 2}) + QPoint(0, half)) == 7);
    dragPx(v, edgeOf(v, {6, 2}) + QPoint(0, half), edgeOf(v, {11, 6}));
    REQUIRE(t->floatingBlock().height == 1);
    CHECK(t->floatingBlock().width == 9);
    press(v, Qt::Key_Return);
    CHECK(canvasRow(f.w, 2, 2, 9) == QStringLiteral("[abababa]"));
}

TEST_CASE("a resizable float turned on its side resizes along its new length") {
    Fixture f;
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"[ab]"}), StampSlice{1, 1, 0, 0, true, false}, {2, 2});
    t->transform(Transform::RotateClockwise);
    REQUIRE(t->resizable());
    CHECK(t->floatingSlice() == StampSlice{0, 0, 1, 1, false, true});
    CHECK_FALSE(t->resizeFloat(QRect(2, 2, 2, 6)));
    REQUIRE(t->resizeFloat(QRect(2, 2, 1, 6)));
    CHECK(t->floatingBlock().height == 6);
    t->transform(Transform::FlipVertical);
    CHECK(t->floatingSlice() == StampSlice{0, 0, 1, 1, false, true});
    // a stamp's own float of a fixed size, or the cells lifted off the
    // canvas, has none
    t->stamp();
    CHECK_FALSE(t->resizable());
    t->select({2, 2, 1, 6});
    t->moveBy(1, 0);
    CHECK(t->floating());
    CHECK_FALSE(t->resizable());
    CHECK(t->handleAt(QPoint(0, 0)) == -1);
}

TEST_CASE("a stamp with a source past a frame style's limit still resizes") {
    // 120 x 3 and 3 x 120: stamps may be, frame style sources may not
    REQUIRE(120 > kMaxFrameSourceColumns);
    REQUIRE(120 > kMaxFrameSourceRows);
    Fixture f;
    SelectTool* t = f.w.selectTool();
    std::string top(120, '-'), mid(120, ' ');
    top.front() = top.back() = '+';
    mid.front() = mid.back() = '|';
    const CellBlock wide = asciiBlock({top.c_str(), mid.c_str(), top.c_str()});
    const StampSlice cut{1, 1, 1, 1, true, true};
    placeResizable(f, wide, cut, {0, 0});
    REQUIRE(t->resizable());
    REQUIRE(t->resizeFloat(QRect(0, 0, 130, 5)));
    CHECK(t->floatingBlock() == *resizeStamp(wide, cut, 130, 5));
    REQUIRE(t->resizeFloat(QRect(0, 0, 4, 3)));
    CHECK(t->floatingBlock() == asciiBlock({"+--+", "|  |", "+--+"}));
    t->cancel();
    std::vector<const char*> rows(120, "|.|");
    rows.front() = rows.back() = "+-+";
    CellBlock tall(3, 120);
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 3; ++x)
            tall.at(x, y) = ascii(rows[std::size_t(y)][x]);
    placeResizable(f, tall, cut, {0, 0});
    REQUIRE(t->resizeFloat(QRect(0, 0, 6, 200)));
    CHECK(t->floatingBlock() == *resizeStamp(tall, cut, 6, 200));
}

TEST_CASE("a band on its side resizes to the canvas's height and no wider than its width") {
    Fixture f;
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"[ab]"}), StampSlice{1, 1, 0, 0, true, false}, {2, 2});
    t->transform(Transform::RotateClockwise);
    // the displayed height is checked against the rows, not the columns
    REQUIRE(t->resizeFloat(QRect(2, 2, 1, 401)));
    CHECK(t->floatingBlock().height == 401);
    CHECK(t->floatingBlock().width == 1);
    CHECK(t->floatingBlock().at(0, 0).ch == Glyph(rotateGlyph(U'[', true)));
    CHECK(t->floatingBlock().at(0, 399).ch == Glyph(U'a'));
    CHECK(t->floatingBlock().at(0, 400).ch == Glyph(rotateGlyph(U']', true)));
    REQUIRE(t->resizeFloat(QRect(2, 2, 1, kMaxRows)));
    CHECK(t->floatingBlock().height == kMaxRows);
    CHECK_FALSE(t->resizeFloat(QRect(2, 2, 1, kMaxRows + 1)));
    t->cancel();
    // a frame turned: 401 across is past the columns however it was cut
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), StampSlice{1, 1, 1, 1, true, true}, {2, 2});
    t->transform(Transform::RotateCounterClockwise);
    CHECK_FALSE(t->resizeFloat(QRect(2, 2, kMaxColumns + 1, 3)));
    CHECK(t->floatingBlock().width == 3);
    REQUIRE(t->resizeFloat(QRect(2, 2, kMaxColumns, 401)));
    CHECK(t->floatingBlock().width == kMaxColumns);
    CHECK(t->floatingBlock().height == 401);
}

TEST_CASE("a tall band turned on its side rotates, but a drag can't resize it past the columns") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    // 3 x 401: on its side its fixed width is 401, past kMaxColumns
    std::vector<const char*> rows(kMaxColumns + 1, "[x]");
    CellBlock tall(3, kMaxColumns + 1);
    for (int y = 0; y < tall.height; ++y)
        for (int x = 0; x < 3; ++x)
            tall.at(x, y) = ascii(rows[std::size_t(y)][x]);
    placeResizable(f, tall, StampSlice{1, 1, 0, 0, true, false}, {0, 0});
    REQUIRE(t->resizable());
    // the turn itself is allowed, as a plain float's is
    t->transform(Transform::RotateClockwise);
    REQUIRE(t->floatingBlock().width == kMaxColumns + 1);
    REQUIRE(t->floatingBlock().height == 3);
    CHECK_FALSE(t->resizeFloat(QRect(0, 0, kMaxColumns + 1, 8)));
    // the bottom handle, dragged down five rows
    const int mx = (edgeOf(v, {0, 0}).x() + edgeOf(v, {kMaxColumns + 1, 0}).x()) / 2;
    const QPoint handle(mx, edgeOf(v, {0, 3}).y());
    REQUIRE(t->handleAt(handle) >= 0);
    dragPx(v, handle, QPoint(mx, edgeOf(v, {0, 8}).y()));
    CHECK(t->floatingBlock().width == kMaxColumns + 1);
    CHECK(t->floatingBlock().height == 3);
}

TEST_CASE("a turned frame whose minimum is past the columns ignores a drag on its width") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    // 3 x 402 with insets [1, 1, 400, 1]: on its side 402 x 3 with insets
    // [1, 400, 1, 1], so its least width (401) is past kMaxColumns (400)
    CellBlock tall(3, kMaxColumns + 2);
    for (int y = 0; y < tall.height; ++y)
        for (int x = 0; x < 3; ++x)
            tall.at(x, y) = ascii(x == 1 ? '.' : '|');
    placeResizable(f, tall, StampSlice{1, 1, kMaxColumns, 1, true, true}, {0, 0});
    REQUIRE(t->resizable());
    t->transform(Transform::RotateClockwise);
    const CellBlock turned = t->floatingBlock();
    REQUIRE(turned.width == kMaxColumns + 2);
    REQUIRE(turned.height == 3);
    // the left-middle handle, dragged one cell right
    const int my = (edgeOf(v, {0, 0}).y() + edgeOf(v, {0, 3}).y()) / 2;
    const QPoint handle(edgeOf(v, {0, 0}).x(), my);
    REQUIRE(t->handleAt(handle) == 7);
    dragPx(v, handle, QPoint(edgeOf(v, {1, 0}).x(), my));
    CHECK(t->floatingBlock() == turned);
    CHECK(t->floatingPos() == QPoint(0, 0));
    // and the right-middle handle, dragged one cell left
    const QPoint right(edgeOf(v, {kMaxColumns + 2, 0}).x(), my);
    REQUIRE(t->handleAt(right) == 3);
    dragPx(v, right, QPoint(edgeOf(v, {kMaxColumns + 1, 0}).x(), my));
    CHECK(t->floatingBlock() == turned);
    CHECK(t->floatingPos() == QPoint(0, 0));
}

TEST_CASE("a stylus release the view missed keeps the size the last move showed") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), StampSlice{1, 1, 1, 1, true, true}, {2, 2});
    static const QPointingDevice pen(QStringLiteral("test stylus"), 4343, QInputDevice::DeviceType::Stylus,
                                     QPointingDevice::PointerType::Pen,
                                     QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const auto tablet = [&](QEvent::Type type, QPoint px, Qt::MouseButton button, Qt::MouseButtons held) {
        const QPointF at = v->toWidget(QRect(px, QSize(1, 1))).topLeft();
        QTabletEvent e(type, &pen, at, v->mapToGlobal(at), held ? 1.0 : 0.0, 0, 0, 0, 0, 0, Qt::NoModifier, button,
                       held);
        QApplication::sendEvent(v, &e);
    };
    // the review's repro in 8 x 16 cells, (40, 80) to (71, 111), in the
    // view's own: a pixel short of the edge at (9, 7), which rounds to it,
    // while the centre of the cell it is in, (8, 6), rounds to that cell
    tablet(QEvent::TabletPress, edgeOf(v, {5, 5}), Qt::LeftButton, Qt::LeftButton);
    REQUIRE(t->dragging());
    tablet(QEvent::TabletMove, edgeOf(v, {9, 7}) - QPoint(1, 1), Qt::NoButton, Qt::LeftButton);
    REQUIRE(t->floatingBlock().width == 7);
    REQUIRE(t->floatingBlock().height == 5);
    // no release; the pen hovers back with no button held
    tablet(QEvent::TabletMove, edgeOf(v, {20, 15}), Qt::NoButton, Qt::NoButton);
    CHECK_FALSE(t->dragging());
    CHECK(t->floatingPos() == QPoint(2, 2));
    CHECK(t->floatingBlock().width == 7);
    CHECK(t->floatingBlock().height == 5);
}

TEST_CASE("a float's flips and turns that undo each other aren't replayed") {
    Fixture f;
    SelectTool* t = f.w.selectTool();
    const CellBlock big = asciiBlock({"AB12CD", "EF34GH", "ij56kl", "mn78op", "QR90ST", "UVwxYZ"});
    const StampSlice cut{2, 1, 1, 2, true, true};
    placeResizable(f, big, cut, {2, 2});
    for (int i = 0; i < 1000; ++i) {
        t->transform(Transform::FlipHorizontal);
        t->transform(Transform::FlipHorizontal);
    }
    CHECK(t->floatTurns() == 0);
    for (int i = 0; i < 10; ++i) {
        t->transform(Transform::RotateClockwise);
        t->transform(Transform::RotateCounterClockwise);
        t->transform(Transform::FlipVertical);
        t->transform(Transform::FlipVertical);
    }
    CHECK(t->floatTurns() == 0);
    for (int i = 0; i < 8; ++i)
        t->transform(Transform::RotateCounterClockwise);
    CHECK(t->floatTurns() == 0);
    t->transform(Transform::RotateClockwise);
    t->transform(Transform::FlipHorizontal);
    CHECK(t->floatTurns() == 2);
    REQUIRE(t->resizeFloat(QRect(2, 2, 13, 9)));
    CHECK(t->floatingBlock() ==
          transformed(transformed(*resizeStamp(big, cut, 9, 13), Transform::RotateClockwise), Transform::FlipHorizontal));
}

TEST_CASE("a resize drag's rollback is let go when the drag ends") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    SelectTool* t = f.w.selectTool();
    const CellBlock frame = asciiBlock({"+-+", "| |", "+-+"});
    const StampSlice cut{1, 1, 1, 1, true, true};
    const auto drag = [&](QPoint corner) {
        mouseAt(v, QEvent::MouseButtonPress, edgeOf(v, corner));
        mouseAt(v, QEvent::MouseMove, edgeOf(v, corner + QPoint(4, 3)));
        REQUIRE(t->dragging());
        CHECK(t->resizeRollbackBytes() > 0);
    };
    SECTION("released") {
        placeResizable(f, frame, cut, {2, 2});
        drag({5, 5});
        mouseAt(v, QEvent::MouseButtonRelease, edgeOf(v, {9, 8}));
        CHECK(t->resizeRollbackBytes() == 0);
        CHECK(t->floatingBlock().width == 7);
    }
    SECTION("focus lost") {
        placeResizable(f, frame, cut, {2, 2});
        drag({5, 5});
        QFocusEvent lost(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(v, &lost);
        CHECK(t->resizeRollbackBytes() == 0);
        CHECK(t->floatingBlock().width == 3);
    }
    SECTION("committed") {
        placeResizable(f, frame, cut, {2, 2});
        drag({5, 5});
        press(v, Qt::Key_Return);
        CHECK_FALSE(t->floating());
        CHECK(t->resizeRollbackBytes() == 0);
    }
    SECTION("Escape") {
        placeResizable(f, frame, cut, {2, 2});
        drag({5, 5});
        press(v, Qt::Key_Escape);
        CHECK_FALSE(t->floating());
        CHECK(t->resizeRollbackBytes() == 0);
    }
    SECTION("Delete") {
        placeResizable(f, frame, cut, {2, 2});
        drag({5, 5});
        press(v, Qt::Key_Delete);
        CHECK_FALSE(t->floating());
        CHECK(t->resizeRollbackBytes() == 0);
    }
}

TEST_CASE("a stamp of a fixed size floats without handles") {
    Fixture f;
    SelectTool* t = f.w.selectTool();
    placeResizable(f, asciiBlock({"+-+", "| |", "+-+"}), std::nullopt, {2, 2});
    CHECK(t->floating());
    CHECK_FALSE(t->resizable());
    CHECK(t->handleAt(f.w.canvasView()->cellRect(QPoint(5, 5)).topLeft()) == -1);
    CHECK_FALSE(t->resizeFloat(QRect(2, 2, 5, 5)));
}

TEST_CASE("Save to Library can mark the selection as a frame or a band") {
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("+-+"));
    write(f.w, 0, 1, QStringLiteral("| |"));
    write(f.w, 0, 2, QStringLiteral("+-+"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 3, 3});
    const auto save = [&](const QString& name, int resize) {
        bool ran = false;
        QTimer::singleShot(0, &f.w, [&] {
            auto* d = qobject_cast<StampDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            ran = true;
            REQUIRE(d->resizeBox());
            CHECK(d->resizeBox()->currentIndex() == 0);  // a fixed size unless asked
            CHECK_FALSE(d->insetBox(0)->isEnabled());
            d->nameEdit()->setText(name);
            d->categoryBox()->setCurrentText(QStringLiteral("frames"));
            d->resizeBox()->setCurrentIndex(resize);
            CHECK(d->insetBox(0)->isEnabled());
            CHECK(d->insetBox(1)->isEnabled());
            CHECK(d->insetBox(2)->isEnabled() == (resize == 2));
            CHECK(d->insetBox(3)->isEnabled() == (resize == 2));
            d->buttons()->button(QDialogButtonBox::Ok)->click();
        });
        act(f.w, "saveToLibrary")->trigger();
        CHECK(ran);
    };
    save(QStringLiteral("Box"), 2);
    const library::Stamp* box = f.dock->find(fspath(QDir(f.user.path()).filePath(QStringLiteral("box.ans"))));
    REQUIRE(box);
    CHECK(box->meta.slice == StampSlice{1, 1, 1, 1, true, true});
    save(QStringLiteral("Rail"), 1);
    const library::Stamp* rail = f.dock->find(fspath(QDir(f.user.path()).filePath(QStringLiteral("rail.ans"))));
    REQUIRE(rail);
    CHECK(rail->meta.slice == StampSlice{1, 1, 0, 0, true, false});

    // the saved frame places resizable, and saving the placed float offers
    // its slice again
    QString error;
    REQUIRE(f.w.placeStamp(box->path, {5, 5}, &error));
    REQUIRE(f.w.selectTool()->resizable());
    REQUIRE(f.w.selectTool()->resizeFloat(QRect(5, 5, 6, 4)));
    bool ran = false;
    QTimer::singleShot(0, &f.w, [&] {
        auto* d = qobject_cast<StampDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        ran = true;
        CHECK(d->resizeBox()->currentIndex() == 2);
        CHECK(d->slice() == StampSlice{1, 1, 1, 1, true, true});
        d->reject();
    });
    act(f.w, "saveToLibrary")->trigger();
    CHECK(ran);
}

TEST_CASE("Save to Library refuses insets the selection can't be cut by") {
    Fixture f;
    write(f.w, 0, 0, QStringLiteral("ab"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    QString error;
    CHECK(f.w.saveSelectionToLibrary(QStringLiteral("Bar"), QStringLiteral("frames"), {}, &error, false,
                                     StampSlice{1, 1, 0, 0, true, false})
              .isEmpty());
    CHECK(error.contains(QStringLiteral("can't be resizable")));
    CHECK_FALSE(QFileInfo::exists(QDir(f.user.path()).filePath(QStringLiteral("bar.ans"))));
}
TEST_CASE("a float lifted on a Reference layer is refused for the layer's reason, not lost (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer out(&bytes);
    REQUIRE(out.open(QIODevice::WriteOnly));
    REQUIRE(image.save(&out, "PNG"));
    write(f.w, 2, 2, QStringLiteral("ab"));
    const LayerId ref =
        doc->addReferenceLayer(ReferenceImage{std::make_shared<const std::string>(bytes.toStdString()), 0, 0, 80, 40, 100});
    REQUIRE(ref);
    REQUIRE(doc->activeLayer() == ref);
    f.w.tools()->setActive(s);
    s->select({2, 2, 2, 1});
    press(v, Qt::Key_Down);
    REQUIRE(s->floating());
    const Canvas canvas = doc->canvas();
    const int steps = doc->undoStack()->count();
    press(v, Qt::Key_Return, QStringLiteral("\r"));
    CHECK(str(f.w.statusBar()->currentMessage()) ==
          "Could not put the floating selection down: a Reference layer holds a picture, not cells; press Esc to drop it");
    CHECK(s->floating());
    CHECK(doc->canvas() == canvas);
    CHECK(doc->undoStack()->count() == steps);
    press(v, Qt::Key_Escape);
    CHECK_FALSE(s->floating());
    CHECK(doc->canvas() == canvas);
}

TEST_CASE("Delete on a lifted float that can't land drops it and clears the lift's source, one step (#433)",
          "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    CellEdit e;
    e.changes.push_back({3, 3, doc->layerCanvas().at(3, 3), pixelLayerCell(LayerPixels{std::uint8_t(Red), {}})});
    REQUIRE(doc->edit(e));
    const Cell blank = doc->layerCanvas().at(4, 4);
    const int steps = doc->undoStack()->count();
    f.w.tools()->setActive(s);
    s->select({3, 3, 1, 1});
    press(v, Qt::Key_Right);
    act(f.w, "rotateClockwise")->trigger();  // ▀ → ▐: can't land
    press(v, Qt::Key_Return);
    REQUIRE(s->floating());
    REQUIRE(str(s->unlanded()) == "a Pixels layer holds no ▐");
    press(v, Qt::Key_Delete);
    // as before the PR: the float goes, and the lift's source is cleared
    CHECK_FALSE(s->floating());
    CHECK(doc->layerCanvas().at(3, 3) == blank);
    CHECK(doc->layerCanvas().at(4, 3) == blank);
    CHECK(doc->undoStack()->count() == steps + 1);
}

TEST_CASE("the undo of an edit under a covering layer drops a lift, as the active layer's cells changed under it (#433)",
          "[library][layers]") {
    // PLAN §4 Tools: the view cancels the tool on "an undo or redo that
    // changes cells or the active layer's cells"; §4 Selections: an undo or
    // redo that changes cells drops a float.
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId below = doc->activeLayer();
    REQUIRE(doc->addLayer(LayerKind::Cells));
    for (int y = 2; y <= 4; ++y)
        write(f.w, 3, y, QStringLiteral("######"));  // the layer above covers (3..8, 2..4)
    REQUIRE(doc->setActiveLayer(below));
    const Canvas composite = doc->canvas();
    write(f.w, 5, 3, QStringLiteral("x"));  // under the cover: the composite is as it was
    REQUIRE(doc->layerCanvas().at(5, 3).ch.codePoint().value() == U'x');
    REQUIRE(doc->canvas() == composite);
    f.w.tools()->setActive(s);
    s->select({4, 2, 3, 3});
    press(v, Qt::Key_Right);  // lift, one cell right
    REQUIRE(s->floating());
    doc->undoStack()->undo();  // the x is undone
    REQUIRE(doc->layerCanvas().at(5, 3).ch.codePoint().value() != U'x');
    CHECK_FALSE(s->floating());
    if (s->floating()) {
        press(v, Qt::Key_Return, QStringLiteral("\r"));
        const Canvas& layer = doc->layerCanvas();
        bool resurrected = false;
        for (int y = 0; y < layer.height(); ++y)
            for (int x = 0; x < layer.width(); ++x)
                resurrected |= layer.at(x, y).ch.codePoint().value() == U'x';
        INFO("the undone x is in the layer again after the landing, at (6,3): "
             << (layer.at(6, 3).ch.codePoint().value() == U'x'));
        CHECK_FALSE(resurrected);
    }
}

TEST_CASE("Bake Layer Style and its undo keep a plain selection (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    write(f.w, 5, 3, QStringLiteral("x"));
    LayerProperties p = doc->active()->props;
    p.style = TextStyle{{Effect{true, GlowEffect{"fire", 12, 12}}}};
    REQUIRE(doc->setLayerProperties(doc->activeLayer(), p, QStringLiteral("Style")));
    f.w.tools()->setActive(s);
    s->select({4, 2, 3, 3});
    act(f.w, "bakeLayer")->trigger();
    REQUIRE_FALSE(doc->active()->props.style);
    CHECK(s->selection() == Selection(CellRect{4, 2, 3, 3}));
    doc->undoStack()->undo();
    REQUIRE(doc->active()->props.style);
    CHECK(s->selection() == Selection(CellRect{4, 2, 3, 3}));
}

TEST_CASE("the undo of a covered layer's move drops a lift, as the cells under it changed (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId below = doc->activeLayer();
    write(f.w, 5, 3, QStringLiteral("x"));
    REQUIRE(doc->addLayer(LayerKind::Cells));
    for (int y = 2; y <= 4; ++y)
        write(f.w, 3, y, QStringLiteral("######"));  // covers (3..8, 2..4)
    REQUIRE(doc->setActiveLayer(below));
    LayerProperties p = doc->active()->props;
    p.x += 1;
    REQUIRE(doc->setLayerProperties(below, p, QStringLiteral("Move Layer")));
    REQUIRE(doc->layerCanvas().at(6, 3).ch.codePoint().value() == U'x');
    f.w.tools()->setActive(s);
    s->select({5, 2, 3, 3});
    press(v, Qt::Key_Right);
    REQUIRE(s->floating());
    doc->undoStack()->undo();  // the layer moves back: x is at (5,3) again
    REQUIRE(doc->layerCanvas().at(5, 3).ch.codePoint().value() == U'x');
    CHECK_FALSE(s->floating());
}
