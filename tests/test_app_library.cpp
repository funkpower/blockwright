// The Library dock (PLAN §12) in the main window: thumbnails, categories and
// search; placing a stamp by double-click or drop as a floating selection in
// transparent paste mode; and Selection > Save to Library.... main() is in
// test_app.cpp, which points the libraries at empty scratch directories.

#include "fs_quirks.h"
#include "message_box.h"
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
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPointingDevice>
#include <QProgressDialog>
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
#include <functional>
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

// PLAN §13 Phase 2c's done-when: the next window finds the stamp where the
// first saved it, as a restart does, and places it with its blank cell
// transparent.
TEST_CASE("a stamp saved to the Library survives closing the window and places with its blank cells transparent") {
    // The user library both windows read at start, as a run's would be.
    QTemporaryDir user;
    REQUIRE(user.isValid());
    const QByteArray env = qgetenv("BLOCKWRIGHT_USER_LIBRARY");
    REQUIRE(!env.isEmpty());  // test_app.cpp's main points it at scratch
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(user.path()));
    struct Restore {
        QByteArray env;
        ~Restore() { qputenv("BLOCKWRIGHT_USER_LIBRARY", env); }
    } restore{env};

    {
        MainWindow w;
        w.show();
        REQUIRE(w.libraryWidget()->userDir() == fspath(user.path()));
        // A 3x1 part with a blank cell in the middle, saved by Selection >
        // Save to Library.
        write(w, 0, 0, QStringLiteral("x x"));
        w.tools()->setActive(w.selectTool());
        w.selectTool()->select({0, 0, 3, 1});
        bool ran = false;
        QTimer::singleShot(0, &w, [&] {
            auto* d = qobject_cast<StampDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            ran = true;
            d->nameEdit()->setText(QStringLiteral("Two Dots"));
            d->categoryBox()->setCurrentText(QStringLiteral("test"));
            QPushButton* ok = d->buttons()->button(QDialogButtonBox::Ok);
            REQUIRE(ok->isEnabled());
            ok->click();
        });
        act(w, "saveToLibrary")->trigger();
        REQUIRE(ran);
        REQUIRE(w.libraryWidget()->shownNames().contains(QStringLiteral("Two Dots")));
        w.document()->undoStack()->setClean();
        REQUIRE(w.close());
    }

    MainWindow w;
    LibraryDock* dock = w.libraryWidget();
    CHECK(dock->userDir() == fspath(user.path()));
    REQUIRE(dock->shownNames().contains(QStringLiteral("Two Dots")));
    CHECK(dock->categories().contains(QStringLiteral("test")));
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    CHECK(at(w, 0, 0) == QLatin1Char(' '));  // a new window, a blank canvas
    write(w, 9, 5, QStringLiteral("ooooo"));
    const int edits = undo->count();

    v->setCellCursor({10, 5});
    emit dock->grid()->itemActivated(item(dock, QStringLiteral("Two Dots")));
    SelectTool* s = w.selectTool();
    REQUIRE(s->floating());
    CHECK(s->floatingBlock().width == 3);
    CHECK(s->effectivePasteMode() == PasteMode::Transparent);
    press(v, Qt::Key_Return);
    CHECK_FALSE(s->floating());
    CHECK(undo->count() == edits + 1);
    CHECK(at(w, 9, 5) == QLatin1Char('o'));
    CHECK(at(w, 10, 5) == QLatin1Char('x'));
    CHECK(at(w, 11, 5) == QLatin1Char('o'));  // the blank cell left it
    CHECK(at(w, 12, 5) == QLatin1Char('x'));
    CHECK(at(w, 13, 5) == QLatin1Char('o'));
    undo->undo();
    CHECK(at(w, 10, 5) == QLatin1Char('o'));
    CHECK(at(w, 12, 5) == QLatin1Char('o'));
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
            text = boxText(box);
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
            text = boxText(box);
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

TEST_CASE("the undo or redo of a hide keeps a float, and its landing is checked (#433)", "[library][layers]") {
    // PLAN §4 Selections: the undo or redo of a layer step that leaves the
    // active layer and its cells as they were (a hide among them) keeps a
    // floating selection, even where the composite changes.
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId below = doc->activeLayer();
    write(f.w, 5, 3, QStringLiteral("x"));
    const LayerId above = doc->addLayer(LayerKind::Cells);
    REQUIRE(above);
    write(f.w, 20, 10, QStringLiteral("###"));  // content of its own, away from the float
    REQUIRE(doc->setActiveLayer(below));
    int changes = 0;
    QObject::connect(doc, &Document::cellsChanged, doc, [&] { ++changes; });
    f.w.statusBar()->clearMessage();

    SECTION("another layer hidden, undone and redone") {
        REQUIRE(doc->setLayerVisible(above, false));
        f.w.tools()->setActive(s);
        s->select({5, 3, 1, 1});
        press(v, Qt::Key_Right);  // lift, one cell right
        REQUIRE(s->floating());
        changes = 0;
        doc->undoStack()->undo();  // shown again: the composite changes
        REQUIRE(doc->layers().find(above)->props.visible);
        REQUIRE(changes > 0);
        CHECK(s->floating());
        doc->undoStack()->redo();  // hidden again
        REQUIRE_FALSE(doc->layers().find(above)->props.visible);
        CHECK(s->floating());
        CHECK(f.w.statusBar()->currentMessage().isEmpty());
        press(v, Qt::Key_Return, QStringLiteral("\r"));
        CHECK_FALSE(s->floating());
        CHECK(doc->layerCanvas().at(6, 3).ch.codePoint().value() == U'x');
        CHECK(doc->layerCanvas().at(5, 3).ch.codePoint().value() != U'x');
    }
    SECTION("a paste over a layer hidden, then the hide undone") {
        REQUIRE(doc->setLayerVisible(above, false));
        f.w.tools()->setActive(s);
        s->select({5, 3, 1, 1});
        act(f.w, "copy")->trigger();
        v->setCellCursor({30, 12});
        act(f.w, "paste")->trigger();
        REQUIRE(s->floating());
        changes = 0;
        doc->undoStack()->undo();
        REQUIRE(doc->layers().find(above)->props.visible);
        REQUIRE(changes > 0);
        CHECK(s->floating());
        CHECK(f.w.statusBar()->currentMessage().isEmpty());
        press(v, Qt::Key_Return, QStringLiteral("\r"));
        CHECK_FALSE(s->floating());
        CHECK(doc->layerCanvas().at(30, 12).ch.codePoint().value() == U'x');
    }
    SECTION("the active layer hidden by the undo: the float stays, and its landing is refused") {
        REQUIRE(doc->setLayerVisible(below, false));
        REQUIRE(doc->setLayerVisible(below, true));
        f.w.tools()->setActive(s);
        s->select({5, 3, 1, 1});
        press(v, Qt::Key_Right);
        REQUIRE(s->floating());
        changes = 0;
        doc->undoStack()->undo();  // the active layer is hidden again, its x gone from the composite
        REQUIRE_FALSE(doc->active()->props.visible);
        REQUIRE(changes > 0);
        CHECK(s->floating());
        const Canvas layer = doc->layerCanvas();
        const int steps = doc->undoStack()->count();
        press(v, Qt::Key_Return, QStringLiteral("\r"));
        CHECK(s->floating());
        CHECK(f.w.statusBar()->currentMessage().contains(QStringLiteral("the layer is hidden")));
        CHECK(doc->layerCanvas() == layer);
        CHECK(doc->undoStack()->count() == steps);
        doc->undoStack()->redo();  // shown: it lands now
        REQUIRE(doc->active()->props.visible);
        CHECK(s->floating());
        press(v, Qt::Key_Return, QStringLiteral("\r"));
        CHECK_FALSE(s->floating());
        CHECK(doc->layerCanvas().at(6, 3).ch.codePoint().value() == U'x');
    }
}

TEST_CASE("the undo or redo of another layer's move, add or delete keeps a float (#433)", "[library][layers]") {
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId below = doc->activeLayer();
    write(f.w, 5, 3, QStringLiteral("x"));
    const LayerId above = doc->addLayer(LayerKind::Cells);
    REQUIRE(above);
    write(f.w, 20, 10, QStringLiteral("###"));
    SECTION("moved one cell right") {
        LayerProperties p = doc->active()->props;
        p.x += 1;
        REQUIRE(doc->setLayerProperties(above, p, QStringLiteral("Move Layer")));
    }
    SECTION("lowered under the active layer") {
        REQUIRE(doc->moveLayer(above, 0));
    }
    SECTION("deleted while another is active") {
        REQUIRE(doc->setActiveLayer(below));
        REQUIRE(doc->removeLayer(above));
    }
    REQUIRE(doc->setActiveLayer(below));
    int changes = 0;
    QObject::connect(doc, &Document::cellsChanged, doc, [&] { ++changes; });
    f.w.statusBar()->clearMessage();
    f.w.tools()->setActive(s);
    s->select({5, 3, 1, 1});
    press(v, Qt::Key_Right);
    REQUIRE(s->floating());
    doc->undoStack()->undo();
    REQUIRE(changes > 0);
    REQUIRE(doc->activeLayer() == below);
    CHECK(s->floating());
    doc->undoStack()->redo();
    REQUIRE(doc->activeLayer() == below);
    CHECK(s->floating());
    CHECK(f.w.statusBar()->currentMessage().isEmpty());
    press(v, Qt::Key_Return, QStringLiteral("\r"));
    CHECK_FALSE(s->floating());
    CHECK(doc->layerCanvas().at(6, 3).ch.codePoint().value() == U'x');
}

TEST_CASE("an iCE switch that restyles no layer keeps a float, as does its undo (#433)", "[library][layers]") {
    // PLAN §4 Selections: only an iCE switch that restyles a layer drops a
    // float that can't land; a plain one keeps it, though a Pixels layer
    // makes the composite change with it.
    Fixture f;
    Document* doc = f.w.document();
    SelectTool* s = f.w.selectTool();
    CanvasView* v = f.w.canvasView();
    const LayerId below = doc->activeLayer();
    write(f.w, 5, 3, QStringLiteral("x"));
    REQUIRE(doc->addLayer(LayerKind::Pixels));
    CellEdit e;
    e.changes.push_back(
        {20, 10, doc->layerCanvas().at(20, 10), pixelLayerCell(LayerPixels{std::uint8_t(15), std::uint8_t(12)})});
    REQUIRE(doc->edit(e));
    REQUIRE(doc->setActiveLayer(below));
    f.w.tools()->setActive(s);
    s->select({5, 3, 1, 1});
    press(v, Qt::Key_Right);
    REQUIRE(s->floating());
    int changes = 0;
    QObject::connect(doc, &Document::cellsChanged, doc, [&] { ++changes; });
    f.w.statusBar()->clearMessage();
    const bool ice = doc->format().ice;
    act(f.w, "iceColours")->trigger();
    REQUIRE(doc->format().ice != ice);
    REQUIRE(changes > 0);
    CHECK(s->floating());
    CHECK(f.w.statusBar()->currentMessage().isEmpty());
    changes = 0;
    doc->undoStack()->undo();
    REQUIRE(doc->format().ice == ice);
    REQUIRE(changes > 0);
    CHECK(s->floating());
    press(v, Qt::Key_Return, QStringLiteral("\r"));
    CHECK_FALSE(s->floating());
    CHECK(doc->layerCanvas().at(6, 3).ch.codePoint().value() == U'x');
}

// Collections (#464 slice 2, PLAN §12 Collections): the Library dock's
// commands on the user's own stamps and collections.

namespace {

// A window as Fixture's whose trash is a scratch folder
// (library::testing::moveToTrash), never the system's: each path trashed
// is moved into it and listed in `trashed`. With `trashRefuses` the trash
// refuses, as where there is none.
struct CollectionFixture : Fixture {
    QTemporaryDir trash;
    bool trashRefuses = false;
    std::vector<fs::path> trashed;
    CollectionFixture() {
        REQUIRE(trash.isValid());
        library::testing::moveToTrash = [this](const fs::path& p) {
            if (trashRefuses)
                return false;
            std::error_code ec;
            fs::rename(p, fspath(trash.path()) / std::to_string(trashed.size()), ec);
            if (ec)
                return false;
            trashed.push_back(p);
            return true;
        };
    }
    ~CollectionFixture() { library::testing::moveToTrash = nullptr; }
    CollectionFixture(const CollectionFixture&) = delete;
    CollectionFixture& operator=(const CollectionFixture&) = delete;
    fs::path userPath() const { return fspath(user.path()); }
    bool trashEmpty() const { return fs::is_empty(fspath(trash.path())); }
};

// Answers the next QInputDialog with `text` (OK) or cancels it; `ran`
// says whether one opened.
void answerInput(MainWindow& w, const QString& text, bool ok, bool* ran) {
    *ran = false;
    QTimer::singleShot(0, &w, [&w, text, ok, ran] {
        auto* d = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        if (!d)
            return;
        *ran = true;
        d->setTextValue(text);
        ok ? d->accept() : d->reject();
        (void)w;
    });
}

// Answers the next QMessageBox with `button`, keeping its text and default.
struct Box {
    bool ran = false;
    QString text;
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton;
};
void answerBox(MainWindow& w, QMessageBox::StandardButton button, Box* seen) {
    *seen = {};
    QTimer::singleShot(0, &w, [button, seen] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box)
            return;
        seen->ran = true;
        seen->text = bw::boxText(box);
        seen->defaultButton = box->standardButton(box->defaultButton());
        box->button(button)->click();
    });
}

// Runs `trigger`, answering each QMessageBox that opens meanwhile, in
// turn, with the next of `buttons`, and keeping each one's text and
// default in *seen. Answers left over when `trigger` returns (fewer boxes
// opened) are dropped with the timer, so they can't answer a later box.
void answerBoxes(MainWindow& w, std::vector<QMessageBox::StandardButton> buttons, std::vector<Box>* seen,
                 const std::function<void()>& trigger) {
    seen->clear();
    // stopped and deleted however `trigger` ends (deleteLater alone waits
    // for an event loop to return to)
    const std::unique_ptr<QTimer> timer(new QTimer(&w));
    timer->setInterval(5);
    std::size_t answered = 0;
    QObject::connect(timer.get(), &QTimer::timeout, timer.get(), [&buttons, seen, &answered] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box || !box->isVisible() || answered >= buttons.size())
            return;
        Box b;
        b.ran = true;
        b.text = bw::boxText(box);
        b.defaultButton = box->standardButton(box->defaultButton());
        seen->push_back(b);
        box->button(buttons[answered++])->click();
    });
    timer->start();
    trigger();
}

// Runs `trigger` where no dialog should open: any that does is cancelled,
// and noted.
bool opensNoDialog(MainWindow& w, const std::function<void()>& trigger) {
    bool opened = false;
    QTimer::singleShot(0, &w, [&opened] {
        if (QWidget* m = QApplication::activeModalWidget()) {
            opened = true;
            if (auto* d = qobject_cast<QDialog*>(m))
                d->reject();
        }
    });
    trigger();
    QCoreApplication::processEvents();
    return !opened;
}

void pickCategory(LibraryDock* dock, const QString& name) {
    const auto found = dock->categoryList()->findItems(name, Qt::MatchExactly);
    REQUIRE(found.size() == 1);
    dock->categoryList()->setCurrentItem(found.first());
}

void pickStamp(LibraryDock* dock, const QString& name) { dock->grid()->setCurrentItem(item(dock, name)); }

QStringList listed(LibraryDock* dock) {
    QStringList out;
    for (int i = 0; i < dock->categoryList()->count(); ++i)
        out << dock->categoryList()->item(i)->text();
    return out;
}

QString status(MainWindow& w) { return w.statusBar()->currentMessage(); }

// A collection made through the dock, without asking.
void makeCollection(LibraryDock* dock, const QString& name) {
    QString error;
    REQUIRE(dock->createCollection(name, &error));
}

// Saves "ab" from the canvas as `name` in `category` through Selection >
// Save to Library.
void saveStamp(MainWindow& w, const QString& name, const QString& category, const QString& tags = {},
               QStringList* offered = nullptr) {
    write(w, 0, 0, QStringLiteral("ab"));
    w.tools()->setActive(w.selectTool());
    w.selectTool()->select({0, 0, 2, 1});
    bool ran = false;
    QTimer::singleShot(0, &w, [&] {
        auto* d = qobject_cast<StampDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        ran = true;
        if (offered)
            for (int i = 0; i < d->categoryBox()->count(); ++i)
                *offered << d->categoryBox()->itemText(i);
        d->nameEdit()->setText(name);
        d->categoryBox()->setCurrentText(category);
        d->tagsEdit()->setText(tags);
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    act(w, "saveToLibrary")->trigger();
    REQUIRE(ran);
}

} // namespace

TEST_CASE("New Collection makes an empty collection, listed at once and kept on a restart", "[library][collections]") {
    CollectionFixture f;
    // the search and the picked category stay through the rescan
    pickCategory(f.dock, QStringLiteral("figures"));
    f.dock->searchEdit()->setText(QStringLiteral("tu"));
    bool ran = false;

    answerInput(f.w, QStringLiteral("My Trees"), false, &ran);
    act(f.w, "newLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(f.dock->collections().empty());
    CHECK(listed(f.dock) == QStringList{QStringLiteral("All"), QStringLiteral("figures"), QStringLiteral("sky")});

    answerInput(f.w, QStringLiteral("My Trees"), true, &ran);
    act(f.w, "newLibraryCollection")->trigger();
    CHECK(ran);
    REQUIRE(f.dock->collections().size() == 1);
    CHECK(f.dock->collections()[0].stamps == 0);
    CHECK(fs::is_directory(f.userPath() / "my-trees"));
    CHECK(listed(f.dock) == QStringList{QStringLiteral("All"), QStringLiteral("My Trees"), QStringLiteral("figures"),
                                        QStringLiteral("sky")});
    CHECK(status(f.w) == QStringLiteral("Made the collection \"My Trees\""));
    CHECK(f.dock->pickedCategory() == QStringLiteral("figures"));
    CHECK(f.dock->searchEdit()->text() == QStringLiteral("tu"));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Tux")});

    // a name in use is refused, with the reason
    answerInput(f.w, QStringLiteral("My Trees"), true, &ran);
    act(f.w, "newLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(f.dock->collections().size() == 1);
    CHECK(status(f.w).startsWith(QStringLiteral("No collection made: a collection named \"My Trees\" already exists")));

    // a second dock on the same user library, as after a restart
    LibraryDock again;
    again.setDirectories(bwtest::dataDir() / "library", f.userPath());
    REQUIRE(again.collections().size() == 1);
    CHECK(again.collections()[0].name == "My Trees");
    CHECK(again.categories() == f.dock->categories());
}

TEST_CASE("a new MainWindow on the same user library shows the same collections and stamps", "[library][collections]") {
    QTemporaryDir user;
    REQUIRE(user.isValid());
    const QByteArray env = qgetenv("BLOCKWRIGHT_USER_LIBRARY");
    REQUIRE(!env.isEmpty());
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(user.path()));
    struct Restore {
        QByteArray env;
        ~Restore() { qputenv("BLOCKWRIGHT_USER_LIBRARY", env); }
    } restore{env};
    {
        MainWindow w;
        makeCollection(w.libraryWidget(), QStringLiteral("Empty One"));
        makeCollection(w.libraryWidget(), QStringLiteral("Rocks"));
        saveStamp(w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
        w.document()->undoStack()->setClean();
        REQUIRE(w.close());
    }
    MainWindow w;
    LibraryDock* dock = w.libraryWidget();
    REQUIRE(dock->collections().size() == 2);
    CHECK(dock->collections()[0].name == "Empty One");
    CHECK(dock->collections()[0].stamps == 0);
    CHECK(dock->collections()[1].name == "Rocks");
    CHECK(dock->collections()[1].stamps == 1);
    CHECK(dock->categories().contains(QStringLiteral("Empty One")));
    pickCategory(dock, QStringLiteral("Rocks"));
    CHECK(dock->shownNames() == QStringList{QStringLiteral("Pebble")});
}

TEST_CASE("Save to Library offers an empty collection and saves into its folder", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    QStringList offered;
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"), QStringLiteral("small, grey"), &offered);
    CHECK(offered.contains(QStringLiteral("Rocks")));
    const fs::path pebble = f.userPath() / "rocks" / "pebble.ans";
    REQUIRE(fs::exists(pebble));
    CHECK(fs::exists(f.userPath() / "rocks" / "pebble.json"));
    CHECK_FALSE(fs::exists(f.userPath() / "pebble.ans"));
    const library::Stamp* s = f.dock->find(pebble);
    REQUIRE(s);
    CHECK(s->meta.category == "Rocks");
    CHECK(s->meta.tags == std::vector<std::string>{"small", "grey"});
    CHECK(f.dock->findCollection(QStringLiteral("Rocks"))->stamps == 1);

    // the same name again in the collection asks to replace it, there
    bool exists = false;
    CHECK(fspath(f.w.libraryPathFor(QStringLiteral("Pebble"), &exists, nullptr, QStringLiteral("Rocks"))) == pebble);
    CHECK(exists);
    CHECK(fspath(f.w.libraryPathFor(QStringLiteral("Pebble"), &exists)) == f.userPath() / "pebble.ans");
    CHECK_FALSE(exists);

    // a category that is no collection saves at the top, as before
    saveStamp(f.w, QStringLiteral("Loose"), QStringLiteral("odds"));
    CHECK(fs::exists(f.userPath() / "loose.ans"));
    CHECK(f.dock->collections().size() == 1);
}

TEST_CASE("Delete Collection asks, naming the stamp count, and leaves a bundled category", "[library][collections]") {
    CollectionFixture f;
    // a collection named like the bundled category "sky", holding a copy of
    // the bundled star and a stamp of its own
    makeCollection(f.dock, QStringLiteral("sky"));
    const fs::path star = bwtest::dataDir() / "library" / "sky" / "small-star.ans";
    QString error;
    REQUIRE(f.dock->addToCollection(star, QStringLiteral("sky"), &error));
    saveStamp(f.w, QStringLiteral("Cloud"), QStringLiteral("sky"));
    REQUIRE(f.dock->findCollection(QStringLiteral("sky"))->stamps == 2);
    const auto starBytes = [&] {
        QFile file(QString::fromStdString(star.string()));
        REQUIRE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    const QByteArray before = starBytes();

    pickCategory(f.dock, QStringLiteral("sky"));
    f.dock->searchEdit()->setText(QStringLiteral("s"));
    Box box;
    answerBox(f.w, QMessageBox::No, &box);
    act(f.w, "deleteLibraryCollection")->trigger();
    REQUIRE(box.ran);
    CHECK(box.defaultButton == QMessageBox::No);
    CHECK(box.text.contains(QStringLiteral("Delete the collection \"sky\" and the 2 stamps in it?")));
    CHECK(box.text.contains(QStringLiteral("They go to the trash.")));
    CHECK(box.text.contains(QStringLiteral("The bundled stamps in \"sky\" stay.")));
    CHECK(fs::is_directory(f.userPath() / "sky"));
    CHECK(f.dock->collections().size() == 1);

    answerBox(f.w, QMessageBox::Yes, &box);
    act(f.w, "deleteLibraryCollection")->trigger();
    REQUIRE(box.ran);
    CHECK_FALSE(fs::exists(f.userPath() / "sky"));
    CHECK(f.trashed == std::vector<fs::path>{f.userPath() / "sky"});
    CHECK(f.dock->collections().empty());
    CHECK(status(f.w) == QStringLiteral("Moved the collection \"sky\" and its 2 stamps to the trash"));
    // the bundled category stays, picked, with the search, its stamp untouched
    CHECK(listed(f.dock) == QStringList{QStringLiteral("All"), QStringLiteral("figures"), QStringLiteral("sky")});
    CHECK(f.dock->pickedCategory() == QStringLiteral("sky"));
    CHECK(f.dock->searchEdit()->text() == QStringLiteral("s"));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Small star")});
    CHECK(starBytes() == before);

    // a bundled category, and All, are refused with a reason and no dialog
    CHECK(opensNoDialog(f.w, [&] { act(f.w, "deleteLibraryCollection")->trigger(); }));
    CHECK(status(f.w) == QStringLiteral("\"sky\" is a bundled category, not one of your collections; nothing deleted"));
    pickCategory(f.dock, QStringLiteral("All"));
    CHECK(opensNoDialog(f.w, [&] { act(f.w, "renameLibraryCollection")->trigger(); }));
    CHECK(status(f.w) == QStringLiteral("Pick one of your collections in the category list first; nothing renamed"));
    CHECK(starBytes() == before);
}

TEST_CASE("Rename Collection renames its folder and stamps and keeps it picked", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    makeCollection(f.dock, QStringLiteral("Gems"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    pickCategory(f.dock, QStringLiteral("Rocks"));
    f.dock->searchEdit()->setText(QStringLiteral("peb"));
    bool ran = false;

    answerInput(f.w, QStringLiteral("Stones"), false, &ran);
    act(f.w, "renameLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(fs::is_directory(f.userPath() / "rocks"));
    CHECK(f.dock->pickedCategory() == QStringLiteral("Rocks"));

    // a name in use is refused
    answerInput(f.w, QStringLiteral("Gems"), true, &ran);
    act(f.w, "renameLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(status(f.w).startsWith(QStringLiteral("Collection not renamed: a collection named \"Gems\" already exists")));
    CHECK(fs::is_directory(f.userPath() / "rocks"));

    answerInput(f.w, QStringLiteral("Stones"), true, &ran);
    act(f.w, "renameLibraryCollection")->trigger();
    CHECK(ran);
    CHECK_FALSE(fs::exists(f.userPath() / "rocks"));
    const fs::path pebble = f.userPath() / "stones" / "pebble.ans";
    REQUIRE(f.dock->find(pebble));
    CHECK(f.dock->find(pebble)->meta.category == "Stones");
    CHECK(listed(f.dock) == QStringList{QStringLiteral("All"), QStringLiteral("Gems"), QStringLiteral("Stones"),
                                        QStringLiteral("figures"), QStringLiteral("sky")});
    CHECK(f.dock->pickedCategory() == QStringLiteral("Stones"));
    CHECK(f.dock->searchEdit()->text() == QStringLiteral("peb"));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Pebble")});
    CHECK(status(f.w) == QStringLiteral("Renamed the collection \"Rocks\" to \"Stones\""));

    // a bundled category can't be renamed
    pickCategory(f.dock, QStringLiteral("figures"));
    CHECK(opensNoDialog(f.w, [&] { act(f.w, "renameLibraryCollection")->trigger(); }));
    CHECK(status(f.w) == QStringLiteral("\"figures\" is a bundled category, not one of your collections; nothing renamed"));
}

TEST_CASE("Add to Collection copies a bundled stamp; Edit and Remove refuse one", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Mine"));
    pickStamp(f.dock, QStringLiteral("Tux"));
    REQUIRE(f.dock->currentEntry());
    CHECK(f.dock->currentEntry()->bundled);
    const fs::path tux = bwtest::dataDir() / "library" / "figures" / "tux.ans";

    CHECK(opensNoDialog(f.w, [&] { act(f.w, "editLibraryStamp")->trigger(); }));
    CHECK(status(f.w).startsWith(QStringLiteral("Tux is a bundled stamp, which can't be changed; nothing changed")));
    CHECK(opensNoDialog(f.w, [&] { act(f.w, "removeLibraryStamp")->trigger(); }));
    CHECK(status(f.w).startsWith(QStringLiteral("Tux is a bundled stamp, which can't be changed; nothing removed")));
    CHECK(opensNoDialog(f.w, [&] { press(f.dock->grid(), Qt::Key_Delete); }));
    CHECK(fs::exists(tux));

    bool ran = false;
    answerInput(f.w, QStringLiteral("Mine"), false, &ran);
    act(f.w, "addStampToLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(f.dock->findCollection(QStringLiteral("Mine"))->stamps == 0);

    answerInput(f.w, QStringLiteral("Mine"), true, &ran);
    act(f.w, "addStampToLibraryCollection")->trigger();
    CHECK(ran);
    const fs::path copy = f.userPath() / "mine" / "tux.ans";
    REQUIRE(f.dock->find(copy));
    CHECK(f.dock->find(copy)->meta.category == "Mine");
    CHECK(f.dock->find(tux)->meta.category == "figures");
    CHECK(status(f.w) == QStringLiteral("Added Tux to the collection \"Mine\""));
    CHECK(listed(f.dock) == QStringList{QStringLiteral("All"), QStringLiteral("Mine"), QStringLiteral("figures"),
                                        QStringLiteral("sky")});

    // a new name makes the collection first
    pickStamp(f.dock, QStringLiteral("Small star"));
    answerInput(f.w, QStringLiteral("Night"), true, &ran);
    act(f.w, "addStampToLibraryCollection")->trigger();
    CHECK(ran);
    REQUIRE(f.dock->findCollection(QStringLiteral("Night")));
    CHECK(f.dock->findCollection(QStringLiteral("Night"))->stamps == 1);
    CHECK(fs::exists(f.userPath() / "night" / "small-star.ans"));

    // the same stamp again is refused
    pickStamp(f.dock, QStringLiteral("Small star"));
    answerInput(f.w, QStringLiteral("Night"), true, &ran);
    act(f.w, "addStampToLibraryCollection")->trigger();
    CHECK(status(f.w).startsWith(QStringLiteral("Stamp not added: the collection \"Night\" already holds a stamp named")));

    // nothing picked
    f.dock->grid()->clearSelection();
    f.dock->grid()->setCurrentItem(nullptr);
    CHECK(opensNoDialog(f.w, [&] { act(f.w, "addStampToLibraryCollection")->trigger(); }));
    CHECK(status(f.w) == QStringLiteral("Pick a stamp in the Library first; nothing added"));
}

TEST_CASE("Remove Stamp asks, default No, from the action and the Delete key", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Loose"), QStringLiteral("odds"));
    const fs::path pebble = f.userPath() / "rocks" / "pebble.ans";
    pickCategory(f.dock, QStringLiteral("Rocks"));
    pickStamp(f.dock, QStringLiteral("Pebble"));

    Box box;
    answerBox(f.w, QMessageBox::No, &box);
    act(f.w, "removeLibraryStamp")->trigger();
    REQUIRE(box.ran);
    CHECK(box.defaultButton == QMessageBox::No);
    CHECK(box.text.startsWith(QStringLiteral("Remove the stamp Pebble (")));
    CHECK(box.text.contains(QStringLiteral("It goes to the trash.")));
    CHECK(fs::exists(pebble));

    pickStamp(f.dock, QStringLiteral("Pebble"));
    answerBox(f.w, QMessageBox::Yes, &box);
    press(f.dock->grid(), Qt::Key_Delete);
    REQUIRE(box.ran);
    CHECK_FALSE(fs::exists(pebble));
    CHECK_FALSE(fs::exists(f.userPath() / "rocks" / "pebble.json"));
    // the sidecar goes first (removeStamp puts it back if the art can't follow)
    CHECK(f.trashed == std::vector<fs::path>{f.userPath() / "rocks" / "pebble.json", pebble});
    CHECK(status(f.w).startsWith(QStringLiteral("Moved ")));
    CHECK(status(f.w).endsWith(QStringLiteral(" to the trash")));
    // the collection stays, empty, and picked
    CHECK(f.dock->findCollection(QStringLiteral("Rocks"))->stamps == 0);
    CHECK(f.dock->pickedCategory() == QStringLiteral("Rocks"));
    CHECK(f.dock->shownNames().isEmpty());

    // a user stamp outside any collection can go too
    pickCategory(f.dock, QStringLiteral("odds"));
    pickStamp(f.dock, QStringLiteral("Loose"));
    answerBox(f.w, QMessageBox::Yes, &box);
    act(f.w, "removeLibraryStamp")->trigger();
    REQUIRE(box.ran);
    CHECK_FALSE(fs::exists(f.userPath() / "loose.ans"));
    CHECK_FALSE(listed(f.dock).contains(QStringLiteral("odds")));
}

TEST_CASE("Edit Stamp is prefilled from the stamp and renames, re-files and re-tags it", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    makeCollection(f.dock, QStringLiteral("Gems"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"), QStringLiteral("small"));
    const fs::path pebble = f.userPath() / "rocks" / "pebble.ans";
    pickStamp(f.dock, QStringLiteral("Pebble"));

    const auto edit = [&](bool ok, const QString& name, const QString& category, const QString& tags) {
        bool ran = false;
        QTimer::singleShot(0, &f.w, [&] {
            auto* d = qobject_cast<StampDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            ran = true;
            CHECK(d->windowTitle() == QStringLiteral("Edit Stamp"));
            CHECK_FALSE(d->resizeBox());
            CHECK(d->nameEdit()->text() == QStringLiteral("Pebble"));
            CHECK(d->categoryBox()->currentText() == QStringLiteral("Rocks"));
            CHECK(d->tagsEdit()->text() == QStringLiteral("small"));
            d->nameEdit()->setText(name);
            d->categoryBox()->setCurrentText(category);
            d->tagsEdit()->setText(tags);
            ok ? d->buttons()->button(QDialogButtonBox::Ok)->click() : d->reject();
        });
        act(f.w, "editLibraryStamp")->trigger();
        CHECK(ran);
    };
    edit(false, QStringLiteral("Ruby"), QStringLiteral("Gems"), QStringLiteral("red"));
    REQUIRE(f.dock->find(pebble));
    CHECK(f.dock->find(pebble)->meta.tags == std::vector<std::string>{"small"});

    edit(true, QStringLiteral("Ruby"), QStringLiteral("Gems"), QStringLiteral("red, shiny"));
    CHECK_FALSE(fs::exists(pebble));
    const fs::path ruby = f.userPath() / "gems" / "ruby.ans";
    const library::Stamp* s = f.dock->find(ruby);
    REQUIRE(s);
    CHECK(s->name == "Ruby");
    CHECK(s->meta.category == "Gems");
    CHECK(s->meta.tags == std::vector<std::string>{"red", "shiny"});
    CHECK(f.dock->findCollection(QStringLiteral("Rocks"))->stamps == 0);
    CHECK(f.dock->findCollection(QStringLiteral("Gems"))->stamps == 1);
    CHECK(status(f.w) == QStringLiteral("Changed %1").arg(QString::fromStdString(ruby.string())));
}

TEST_CASE("Add Files to Collection adds .ans files and folders and names the ones refused", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    const auto put = [&](const QString& name, const QByteArray& bytes) {
        const QString path = QDir(src.path()).filePath(name);
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        return path;
    };
    const QString good = put(QStringLiteral("good one.ans"), "\x1b[31mXY\r\n Z");
    const QString blank = put(QStringLiteral("blank.ans"), "   \r\n   ");
    const QString missing = QDir(src.path()).filePath(QStringLiteral("missing.ans"));
    put(QStringLiteral("folder/a.ans"), "aa");
    put(QStringLiteral("folder/deeper/b.ans"), "bb");
    put(QStringLiteral("folder/notes.txt"), "not art");
    const QString folder = QDir(src.path()).filePath(QStringLiteral("folder"));

    QStringList failed;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("Imports"), {good, blank, missing, folder}, &failed) == 2);
    REQUIRE(failed.size() == 2);
    CHECK(failed[0].startsWith(blank + QStringLiteral(": ")));
    CHECK(failed[1].startsWith(missing + QStringLiteral(": ")));
    CHECK(status(f.w) == QStringLiteral("Added 2 stamps to the collection \"Imports\"; 2 files refused"));
    CHECK(f.dock->findCollection(QStringLiteral("Imports"))->stamps == 2);
    pickCategory(f.dock, QStringLiteral("Imports"));
    QStringList names = f.dock->shownNames();
    names.sort();
    CHECK(names == QStringList{QStringLiteral("a"), QStringLiteral("good one")});  // not folder/deeper/b.ans

    // not a collection: refused as a whole, no file listed
    failed.clear();
    QString refusal;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("figures"), {good}, &failed, &refusal) == 0);
    CHECK(failed.isEmpty());
    CHECK(refusal == QStringLiteral("\"figures\" is not one of your collections"));
    CHECK(status(f.w) == QStringLiteral("Nothing added: \"figures\" is not one of your collections; 1 file not added"));
}

namespace {

// Writes `bytes` to `name` under `root`, making its folders.
QString putFile(const QString& root, const QString& name, const QByteArray& bytes) {
    const QString path = QDir(root).filePath(name);
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    return path;
}

// Drives the next file dialog: picks `pick` in it, accepts it, and answers
// the collection prompt that follows with `collection`. `fileDialog` says
// whether a QFileDialog opened and `prompt` whether the prompt did.
struct PickedDialog {
    bool fileDialog = false;
    bool prompt = false;
};
void pickInFileDialog(MainWindow& w, const QString& pick, const QString& collection, PickedDialog* seen) {
    *seen = {};
    QTimer::singleShot(0, &w, [&w, pick, collection, seen] {
        auto* d = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!d) {
            // some other modal (a swapped-in dialog): close it so the
            // trigger returns and the checks fail, instead of hanging
            if (QWidget* other = QApplication::activeModalWidget())
                other->close();
            return;
        }
        seen->fileDialog = true;
        QTimer::singleShot(0, &w, [collection, seen] {
            auto* in = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (!in) {
                if (QWidget* other = QApplication::activeModalWidget())
                    other->close();
                return;
            }
            seen->prompt = true;
            in->setTextValue(collection);
            in->accept();
        });
        d->selectFile(pick);
        static_cast<QDialog*>(d)->accept();
    });
}

} // namespace

TEST_CASE("Add Files to Collection and Add Folder to Collection reach the collection prompt from their file dialogs",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    putFile(src.path(), QStringLiteral("folder/a.ans"), "aa");
    const QString file = QDir(src.path()).filePath(QStringLiteral("folder/a.ans"));

    // a file still goes through the dialog to the collection prompt
    PickedDialog seen;
    pickInFileDialog(f.w, file, QStringLiteral("Imports"), &seen);
    f.dock->addFilesAction()->trigger();
    CHECK(seen.fileDialog);
    CHECK(seen.prompt);
    CHECK(f.dock->findCollection(QStringLiteral("Imports"))->stamps == 1);

    // Add Folder to Collection takes the folder from a directory dialog
    REQUIRE(f.dock->addFolderAction());
    putFile(src.path(), QStringLiteral("more/c.ans"), "cc");
    putFile(src.path(), QStringLiteral("more/deeper/d.ans"), "dd");
    pickInFileDialog(f.w, QDir(src.path()).filePath(QStringLiteral("more")), QStringLiteral("Imports"), &seen);
    f.dock->addFolderAction()->trigger();
    CHECK(seen.fileDialog);
    CHECK(seen.prompt);
    CHECK(f.dock->findCollection(QStringLiteral("Imports"))->stamps == 2);
    CHECK(status(f.w) == QStringLiteral("Added 1 stamp to the collection \"Imports\""));
}

TEST_CASE("Add Folder to Collection takes a folder's own .ans files, not hidden ones or AppleDouble files, as the CLI does",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    putFile(src.path(), QStringLiteral("folder/a.ans"), "aa");
    putFile(src.path(), QStringLiteral("folder/.hidden.ans"), "hh");
    putFile(src.path(), QStringLiteral("folder/._a.ans"), "AppleDouble");
    putFile(src.path(), QStringLiteral("folder/Upper.ANS"), "uu");
    putFile(src.path(), QStringLiteral("folder/deeper/b.ans"), "bb");
    putFile(src.path(), QStringLiteral("folder/notes.txt"), "not art");
    putFile(src.path(), QStringLiteral("empty/notes.txt"), "not art");
    const QString folder = QDir(src.path()).filePath(QStringLiteral("folder"));
    const QString empty = QDir(src.path()).filePath(QStringLiteral("empty"));

    // a folder with no .ans file is one refusal, and the rest are still added
    QStringList failed;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("Imports"), {empty, folder}, &failed) == 2);
    REQUIRE(failed.size() == 1);
    CHECK(failed[0] == empty + QStringLiteral(": no .ans files in the folder"));
    CHECK(f.dock->findCollection(QStringLiteral("Imports"))->stamps == 2);
    pickCategory(f.dock, QStringLiteral("Imports"));
    CHECK_FALSE(f.dock->shownNames().contains(QStringLiteral("b")));
    CHECK_FALSE(f.dock->shownNames().contains(QStringLiteral("hidden")));
    CHECK_FALSE(f.dock->shownNames().contains(QStringLiteral("._a")));
    CHECK_FALSE(fs::exists(f.userPath() / "imports" / "a-2.ans"));
}

TEST_CASE("Add Folder to Collection adds a file whose name is not ASCII", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    putFile(src.path(), QStringLiteral("folder/café.ans"), "aa");
    const QString folder = QDir(src.path()).filePath(QStringLiteral("folder"));

    QStringList failed;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("Imports"), {folder}, &failed) == 1);
    CHECK(failed.isEmpty());
    CHECK(f.dock->findCollection(QStringLiteral("Imports"))->stamps == 1);
    pickCategory(f.dock, QStringLiteral("Imports"));
    CHECK(f.dock->shownNames().contains(QStringLiteral("café")));
}

#ifndef _WIN32
TEST_CASE("Add Folder to Collection adds a file whose name is not UTF-8", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    const fs::path folder = fs::path(QFile::encodeName(src.path()).toStdString()) / "folder";
    fs::create_directories(folder);
    {
        std::ofstream out(folder / "a\xfe.ans", std::ios::binary);
        out << "aa";
    }
    // a filesystem may refuse a name that is not valid UTF-8
    if (!fs::exists(folder / "a\xfe.ans"))
        SKIP("the filesystem refuses a file name that is not UTF-8");
    QStringList failed;
    const int added = f.dock->addFilesToCollection(QStringLiteral("Imports"), {QFile::decodeName(folder.c_str())},
                                                   &failed);
    INFO(failed.join(QStringLiteral(" | ")).toStdString());
    CHECK(added == 1);
    CHECK(failed.isEmpty());
}
#endif

#ifndef _WIN32
TEST_CASE("Add Folder to Collection names a folder it can't list once", "[library][collections]") {
    if (::geteuid() == 0)
        SKIP("root lists every folder");
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    const QString locked = QDir(src.path()).filePath(QStringLiteral("locked"));
    REQUIRE(QDir().mkpath(locked));
    fs::permissions(fspath(locked), fs::perms::none);
    QStringList failed;
    f.dock->addFilesToCollection(QStringLiteral("Imports"), {locked}, &failed);
    fs::permissions(fspath(locked), fs::perms::owner_all);
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].startsWith(locked + QStringLiteral(": can't be listed: ")));
    CHECK(failed[0].count(locked) == 1);
}
#endif

namespace {

// A folder of `n` one-line .ans files, <prefix>000.ans up.
QString stampFolder(const QTemporaryDir& dir, int n, const QString& prefix = QStringLiteral("f")) {
    for (int i = 0; i < n; ++i) {
        QFile file(QDir(dir.path()).filePath(prefix + QStringLiteral("%1.ans").arg(i, 3, 10, QLatin1Char('0'))));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("\x1b[31mX" + QByteArray::number(i) + "\r\n");
    }
    return dir.path();
}

} // namespace

TEST_CASE("Add Files to Collection on a collection the core refuses says so once, not as a refused file",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    REQUIRE(f.dock->findCollection(QStringLiteral("Rocks")));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    const QString folder = stampFolder(src, 3);
    // gone from the disk since the dock's last scan
    fs::remove_all(f.userPath() / "rocks");

    QStringList failed;
    QString refusal;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("Rocks"), {folder}, &failed, &refusal) == 0);
    // no file is listed as refused: the collection was
    CHECK(failed.isEmpty());
    CHECK(refusal.startsWith(QStringLiteral("no collection named \"Rocks\"")));
    CHECK(status(f.w) == QStringLiteral("Nothing added: %1; 3 files not added").arg(refusal));
    CHECK_FALSE(status(f.w).contains(QStringLiteral("refused")));
    // rescanned: the collection is gone from the dock too
    CHECK_FALSE(f.dock->findCollection(QStringLiteral("Rocks")));
    CHECK_FALSE(listed(f.dock).contains(QStringLiteral("Rocks")));
    CHECK_FALSE(fs::exists(f.userPath() / "rocks"));
}

TEST_CASE("Add Files to Collection on a folder scans the library once, not once per file (#505)",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    makeCollection(f.dock, QStringLiteral("Other"));
    QTemporaryDir few, many;
    REQUIRE(few.isValid());
    REQUIRE(many.isValid());
    std::size_t scans = 0;
    library::testing::scanned = [&scans](const fs::path&) { ++scans; };
    struct Reset {
        ~Reset() { library::testing::scanned = nullptr; }
    } reset;
    QStringList failed;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("Imports"), {stampFolder(few, 3)}, &failed) == 3);
    const std::size_t forFew = scans;
    scans = 0;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("Imports"), {stampFolder(many, 60, QStringLiteral("g"))}, &failed) == 60);
    CHECK(failed.isEmpty());
    // the same scans for 60 files as for 3: the import's and the rescan's
    CHECK(scans == forFew);
    CHECK(f.dock->findCollection(QStringLiteral("Imports"))->stamps == 63);
    CHECK(status(f.w) == QStringLiteral("Added 60 stamps to the collection \"Imports\""));
}

TEST_CASE("Add Files to Collection shows its progress and stops between files on Cancel (#505)",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Imports"));
    QTemporaryDir src;
    REQUIRE(src.isValid());
    const QString folder = stampFolder(src, 40);
    // Cancel pressed while the fifth file is written
    int written = 0;
    int maximum = -1;
    library::testing::writeStampFault = [&](std::string_view step) {
        if (step != "art" || ++written != 5)
            return;
        auto* progress = f.dock->findChild<QProgressDialog*>();
        REQUIRE(progress);
        maximum = progress->maximum();
        CHECK(progress->labelText().contains(QStringLiteral("\"Imports\"")));
        progress->cancel();
    };
    struct Reset {
        ~Reset() { library::testing::writeStampFault = nullptr; }
    } reset;
    QStringList failed;
    CHECK(f.dock->addFilesToCollection(QStringLiteral("Imports"), {folder}, &failed) == 5);
    CHECK(maximum == 40);
    CHECK(failed.isEmpty());
    CHECK(f.dock->findCollection(QStringLiteral("Imports"))->stamps == 5);
    CHECK(status(f.w) == QStringLiteral("Added 5 stamps to the collection \"Imports\"; cancelled, 35 files not added"));
    CHECK(f.dock->findChild<QProgressDialog*>() == nullptr);
}

TEST_CASE("timing: Add Files to Collection on a folder of 150, 300 and 600 stamps (#505)", "[.perf]") {
    for (const int n : {150, 300, 600}) {
        CollectionFixture f;
        makeCollection(f.dock, QStringLiteral("Imports"));
        QTemporaryDir src;
        REQUIRE(src.isValid());
        const QString folder = stampFolder(src, n);
        QStringList failed;
        QElapsedTimer t;
        t.start();
        CHECK(f.dock->addFilesToCollection(QStringLiteral("Imports"), {folder}, &failed) == n);
        WARN(n << " files: " << t.elapsed() << " ms");
    }
}

TEST_CASE("Delete Collection where the trash refuses asks again before deleting for good", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    f.trashRefuses = true;
    pickCategory(f.dock, QStringLiteral("Rocks"));

    // No to the second question: nothing goes
    std::vector<Box> boxes;
    answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[1].defaultButton == QMessageBox::No);
    CHECK(boxes[1].text.contains(QStringLiteral("The trash can't take the collection \"Rocks\" (")));
    CHECK(boxes[1].text.contains(
        QStringLiteral("Delete the collection \"Rocks\" and the 1 stamp in it permanently? This can't be undone.")));
    CHECK(fs::exists(f.userPath() / "rocks" / "pebble.ans"));
    CHECK(fs::exists(f.userPath() / "rocks" / library::kCollectionMarker));
    CHECK(f.dock->collections().size() == 1);
    CHECK(status(f.w) == QStringLiteral("Collection not deleted: the trash refused it, and nothing was removed"));

    // No to the first: the second never opens
    answerBoxes(f.w, {QMessageBox::No}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    CHECK(boxes.size() == 1);
    CHECK(fs::exists(f.userPath() / "rocks"));

    // Yes to both: deleted for good, never trashed
    answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::Yes}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    REQUIRE(boxes.size() == 2);
    CHECK_FALSE(fs::exists(f.userPath() / "rocks"));
    CHECK(f.dock->collections().empty());
    CHECK(f.trashed.empty());
    CHECK(f.trashEmpty());
    CHECK(status(f.w) == QStringLiteral("Deleted the collection \"Rocks\" and its 1 stamp"));
}

TEST_CASE("Remove Stamp where the trash refuses asks again before deleting for good", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    const fs::path pebble = f.userPath() / "rocks" / "pebble.ans";
    f.trashRefuses = true;
    pickCategory(f.dock, QStringLiteral("Rocks"));
    pickStamp(f.dock, QStringLiteral("Pebble"));

    std::vector<Box> boxes;
    answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[1].defaultButton == QMessageBox::No);
    CHECK(boxes[1].text.contains(QStringLiteral("The stamp Pebble can't go to the trash: ")));
    CHECK(boxes[1].text.contains(QStringLiteral("Delete the stamp Pebble (%1) permanently? This can't be undone.")
                                     .arg(QString::fromStdString(pebble.string()))));
    CHECK(fs::exists(pebble));
    CHECK(fs::exists(f.userPath() / "rocks" / "pebble.json"));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Pebble")});
    CHECK(status(f.w).startsWith(QStringLiteral("Stamp not removed: ")));
    CHECK(status(f.w).contains(QStringLiteral("can't be moved to the trash")));
    CHECK(status(f.w).endsWith(QStringLiteral("nothing was removed")));

    pickStamp(f.dock, QStringLiteral("Pebble"));
    answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::Yes}, &boxes, [&] { press(f.dock->grid(), Qt::Key_Delete); });
    REQUIRE(boxes.size() == 2);
    CHECK_FALSE(fs::exists(pebble));
    CHECK_FALSE(fs::exists(f.userPath() / "rocks" / "pebble.json"));
    CHECK(f.trashed.empty());
    CHECK(f.trashEmpty());
    CHECK(status(f.w).startsWith(QStringLiteral("Removed ")));
    CHECK(f.dock->findCollection(QStringLiteral("Rocks"))->stamps == 0);
}

TEST_CASE("a collection name two folders go by is refused in the status bar, changing nothing",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    // a folder made by hand, no marker, whose stamp's category is "Rocks"
    fs::create_directories(f.userPath() / "zz");
    fs::copy_file(f.userPath() / "rocks" / "pebble.ans", f.userPath() / "zz" / "pebble.ans");
    fs::copy_file(f.userPath() / "rocks" / "pebble.json", f.userPath() / "zz" / "pebble.json");
    f.dock->rescan();
    REQUIRE(f.dock->collections().size() == 2);
    QString why;
    CHECK_FALSE(f.dock->uniqueCollection(QStringLiteral("Rocks"), &why));
    CHECK(why.startsWith(QStringLiteral("more than one folder goes by the collection name \"Rocks\"")));
    CHECK(why.contains(QString::fromStdString((f.userPath() / "zz").string())));

    const auto unchanged = [&] {
        CHECK(fs::exists(f.userPath() / "rocks" / "pebble.ans"));
        CHECK(fs::exists(f.userPath() / "zz" / "pebble.ans"));
        CHECK(f.dock->collections().size() == 2);
        CHECK(f.trashed.empty());
    };
    pickCategory(f.dock, QStringLiteral("Rocks"));
    CHECK(opensNoDialog(f.w, [&] { act(f.w, "deleteLibraryCollection")->trigger(); }));
    CHECK(status(f.w).startsWith(QStringLiteral("more than one folder goes by the collection name \"Rocks\"")));
    CHECK(status(f.w).endsWith(QStringLiteral("; nothing deleted")));
    unchanged();
    CHECK(opensNoDialog(f.w, [&] { act(f.w, "renameLibraryCollection")->trigger(); }));
    CHECK(status(f.w).endsWith(QStringLiteral("; nothing renamed")));
    unchanged();

    // Add to Collection, the name typed
    pickCategory(f.dock, QStringLiteral("All"));
    pickStamp(f.dock, QStringLiteral("Tux"));
    bool ran = false;
    answerInput(f.w, QStringLiteral("Rocks"), true, &ran);
    act(f.w, "addStampToLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(status(f.w).startsWith(QStringLiteral("No collection picked: more than one folder")));
    CHECK_FALSE(fs::exists(f.userPath() / "rocks" / "tux.ans"));
    CHECK_FALSE(fs::exists(f.userPath() / "zz" / "tux.ans"));
    unchanged();

    // Save to Library into it
    write(f.w, 0, 0, QStringLiteral("ab"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    QString error;
    CHECK(f.w.libraryPathFor(QStringLiteral("Flint"), nullptr, &error, QStringLiteral("Rocks")).isEmpty());
    CHECK(error.startsWith(QStringLiteral("more than one folder")));
    error.clear();
    CHECK(f.w.saveSelectionToLibrary(QStringLiteral("Flint"), QStringLiteral("Rocks"), {}, &error).isEmpty());
    CHECK(error.startsWith(QStringLiteral("more than one folder")));
    CHECK_FALSE(fs::exists(f.userPath() / "flint.ans"));
    unchanged();
}

TEST_CASE("New and Rename Collection refuse a name past kMaxCollectionName bytes", "[library][collections]") {
    CollectionFixture f;
    const QString longest(int(library::kMaxCollectionName), QLatin1Char('a'));
    const QString tooLong = longest + QLatin1Char('a');
    // two bytes each in UTF-8: 65 of them is 130 bytes
    const QString tooLongWide(int(library::kMaxCollectionName) / 2 + 1, QChar(0x00e9));
    bool ran = false;
    for (const QString& name : {tooLong, tooLongWide}) {
        answerInput(f.w, name, true, &ran);
        act(f.w, "newLibraryCollection")->trigger();
        CHECK(ran);
        CHECK(status(f.w).startsWith(QStringLiteral("No collection made: ")));
        CHECK(f.dock->collections().empty());
        CHECK(fs::is_empty(f.userPath()));
    }
    answerInput(f.w, longest, true, &ran);
    act(f.w, "newLibraryCollection")->trigger();
    REQUIRE(f.dock->collections().size() == 1);

    pickCategory(f.dock, longest);
    answerInput(f.w, tooLong, true, &ran);
    act(f.w, "renameLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(status(f.w).startsWith(QStringLiteral("Collection not renamed: ")));
    REQUIRE(f.dock->collections().size() == 1);
    CHECK(QString::fromStdString(f.dock->collections()[0].name) == longest);
}

TEST_CASE("the dock's commands on a user library that is the bundled one are refused, with the reason",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    const QByteArray env = qgetenv("BLOCKWRIGHT_BUNDLED_LIBRARY");
    struct Restore {
        QByteArray env;
        ~Restore() { qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", env); }
    } restore{env};
    qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", QFile::encodeName(f.user.path()));

    bool ran = false;
    answerInput(f.w, QStringLiteral("Gems"), true, &ran);
    act(f.w, "newLibraryCollection")->trigger();
    CHECK(ran);
    CHECK(status(f.w).startsWith(QStringLiteral("No collection made: ")));
    CHECK(status(f.w).contains(QStringLiteral("bundled stamps are never changed")));
    CHECK_FALSE(fs::exists(f.userPath() / "gems"));

    pickCategory(f.dock, QStringLiteral("Rocks"));
    std::vector<Box> boxes;
    answerBoxes(f.w, {QMessageBox::Yes}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    REQUIRE(boxes.size() == 1);
    CHECK(status(f.w).startsWith(QStringLiteral("Collection not deleted: ")));
    CHECK(status(f.w).contains(QStringLiteral("bundled stamps are never changed")));
    CHECK(fs::exists(f.userPath() / "rocks" / "pebble.ans"));
    CHECK(f.trashed.empty());

    pickStamp(f.dock, QStringLiteral("Pebble"));
    answerBoxes(f.w, {QMessageBox::Yes}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
    REQUIRE(boxes.size() == 1);
    CHECK(status(f.w).startsWith(QStringLiteral("Stamp not removed: ")));
    CHECK(status(f.w).contains(QStringLiteral("bundled stamps are never changed")));
    CHECK(fs::exists(f.userPath() / "rocks" / "pebble.ans"));
    CHECK(f.trashed.empty());
}

TEST_CASE("Save to Library replaces the stamp holding the title in a collection, one renamed in place included",
          "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    const fs::path pebble = f.userPath() / "rocks" / "pebble.ans";
    // renamed in place: pebble.ans now holds "Rock"
    QString error;
    REQUIRE(f.dock->editStamp(pebble, QStringLiteral("Rock"), QStringLiteral("Rocks"), {}, &error));
    REQUIRE(f.dock->find(pebble));
    REQUIRE(f.dock->find(pebble)->name == "Rock");

    bool exists = false;
    CHECK(fspath(f.w.libraryPathFor(QStringLiteral("Rock"), &exists, nullptr, QStringLiteral("Rocks"))) == pebble);
    CHECK(exists);
    // "Pebble" is free again: it would go to a new file, not over "Rock"
    CHECK(fspath(f.w.libraryPathFor(QStringLiteral("Pebble"), &exists, nullptr, QStringLiteral("Rocks"))) != pebble);
    CHECK_FALSE(exists);

    write(f.w, 0, 0, QStringLiteral("cd"));
    f.w.tools()->setActive(f.w.selectTool());
    f.w.selectTool()->select({0, 0, 2, 1});
    CHECK(f.w.saveSelectionToLibrary(QStringLiteral("Rock"), QStringLiteral("Rocks"), {}, &error).isEmpty());
    CHECK(error.contains(QString::fromStdString(pebble.string())));
    CHECK(fspath(f.w.saveSelectionToLibrary(QStringLiteral("Rock"), QStringLiteral("Rocks"), {}, &error, true)) ==
          pebble);
    CHECK_FALSE(fs::exists(f.userPath() / "rocks" / "rock.ans"));
    CHECK(f.dock->findCollection(QStringLiteral("Rocks"))->stamps == 1);
    REQUIRE(f.dock->find(pebble));
    CHECK(f.dock->find(pebble)->name == "Rock");
}

TEST_CASE("Remove Stamp says what went when the trash refuses half a stamp", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    const fs::path dir = f.userPath() / "rocks";
    const fs::path pebble = dir / "pebble.ans", side = dir / "pebble.json";
    const fs::path bin = fspath(f.trash.path());
    pickCategory(f.dock, QStringLiteral("Rocks"));
    pickStamp(f.dock, QStringLiteral("Pebble"));
    std::vector<Box> boxes;

    SECTION("the trash refuses the sidecar, which goes first: nothing goes") {
        library::testing::moveToTrash = [&](const fs::path& p) {
            if (p.extension() == ".json")
                return false;
            fs::rename(p, bin / p.filename());
            return true;
        };
        answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
        REQUIRE(boxes.size() == 2);
        CHECK(boxes[1].text.contains(QString::fromStdString(side.string())));
        CHECK(status(f.w) == QStringLiteral("Stamp not removed: %1 can't be moved to the trash (there is none, or it "
                                            "refused); nothing was removed")
                                 .arg(QString::fromStdString(side.string())));
        CHECK(fs::exists(pebble));
        CHECK(fs::exists(side));
        CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Pebble")});
        CHECK(f.trashEmpty());
    }
    SECTION("the trash refuses the art after the sidecar: the sidecar is put back") {
        library::testing::moveToTrash = [&](const fs::path& p) {
            if (p.extension() == ".ans")
                return false;
            fs::rename(p, bin / p.filename());
            return true;
        };
        answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
        REQUIRE(boxes.size() == 2);
        CHECK(boxes[1].text.contains(QStringLiteral("was put back")));
        // the trash keeps the sidecar's copy, and the status says so
        CHECK(status(f.w).startsWith(QStringLiteral("Stamp not removed: ")));
        CHECK(status(f.w).contains(QStringLiteral("nothing was removed")));
        CHECK(status(f.w).contains(QStringLiteral("was put back; the trash keeps a copy of it")));
        CHECK(fs::exists(pebble));
        CHECK(fs::exists(side));
        CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Pebble")});
        CHECK(f.dock->findCollection(QStringLiteral("Rocks"))->stamps == 1);

        // Yes to both: deleted for good
        pickStamp(f.dock, QStringLiteral("Pebble"));
        answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::Yes}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
        REQUIRE(boxes.size() == 2);
        CHECK_FALSE(fs::exists(pebble));
        CHECK_FALSE(fs::exists(side));
        CHECK(f.dock->shownNames().isEmpty());
    }
    SECTION("the sidecar can't be put back: the dock says the stamp is half removed and stops listing it") {
        if (::geteuid() == 0)
            SKIP("root writes every folder");
        struct Unlock {
            fs::path p;
            ~Unlock() { fs::permissions(p, fs::perms::owner_all); }
        } unlock{dir};
        // the sidecar goes, then the folder turns read-only: the art can't
        // follow and the sidecar can't come back
        library::testing::moveToTrash = [&](const fs::path& p) {
            fs::rename(p, bin / p.filename());
            fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
            return true;
        };
        answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
        CHECK(boxes.size() == 1);
        CHECK(status(f.w).startsWith(QStringLiteral("Stamp only partly removed: ")));
        CHECK(status(f.w).contains(QStringLiteral("couldn't be put back")));
        CHECK_FALSE(status(f.w).contains(QStringLiteral("nothing was removed")));
        CHECK(fs::exists(pebble));
        CHECK_FALSE(fs::exists(side));
        // art with no sidecar is no stamp: off the grid, named as a problem
        CHECK(f.dock->shownNames().isEmpty());
        CHECK(f.dock->problemLabel()->text() == QStringLiteral("1 file skipped"));
        CHECK(f.dock->problemLabel()->toolTip().contains(QStringLiteral("pebble.ans")));
    }
}

TEST_CASE("Remove Stamp refuses a stamp whose sidecar can't be read before the trash takes anything",
          "[library][collections]") {
    if (::geteuid() == 0)
        SKIP("root reads every file");
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    const fs::path dir = f.userPath() / "rocks";
    const fs::path pebble = dir / "pebble.ans", side = dir / "pebble.json";
    pickCategory(f.dock, QStringLiteral("Rocks"));
    pickStamp(f.dock, QStringLiteral("Pebble"));
    // unreadable since the dock's scan
    struct Unlock {
        fs::path p;
        ~Unlock() {
            std::error_code ec;
            fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, ec);
        }
    } unlock{side};
    fs::permissions(side, fs::perms::none);
    std::vector<Box> boxes;

    SECTION("No: nothing moved, and the status says why") {
        answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
        REQUIRE(boxes.size() == 2);
        CHECK(boxes[1].defaultButton == QMessageBox::No);
        // the trash wasn't tried, so the box doesn't say it refused
        CHECK_FALSE(boxes[1].text.contains(QStringLiteral("The trash can't take")));
        CHECK(boxes[1].text.contains(QString::fromStdString(side.string()) + QStringLiteral(" can't be read")));
        CHECK(boxes[1].text.contains(QStringLiteral("Delete the stamp Pebble (%1) permanently? This can't be undone.")
                                         .arg(QString::fromStdString(pebble.string()))));
        CHECK(fs::exists(pebble));
        CHECK(fs::exists(side));
        CHECK(f.trashed.empty());
        CHECK(f.trashEmpty());
        CHECK(status(f.w).startsWith(QStringLiteral("Stamp not removed: ")));
        CHECK(status(f.w).contains(QString::fromStdString(side.string()) + QStringLiteral(" can't be read")));
        CHECK(status(f.w).contains(QStringLiteral("nothing was removed")));
        CHECK_FALSE(status(f.w).contains(QStringLiteral("the trash refused")));
    }
    SECTION("Yes: deleted for good, art and sidecar, and the trash stays empty") {
        answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::Yes}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
        REQUIRE(boxes.size() == 2);
        CHECK_FALSE(fs::exists(pebble));
        CHECK_FALSE(fs::exists(fs::symlink_status(side)));
        CHECK(f.trashed.empty());
        CHECK(f.trashEmpty());
        CHECK(status(f.w).startsWith(QStringLiteral("Removed ")));
        CHECK(f.dock->findCollection(QStringLiteral("Rocks"))->stamps == 0);
    }
}

TEST_CASE("a refused command rescans, so what is gone from the disk goes from the dock", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    makeCollection(f.dock, QStringLiteral("Sand"));
    saveStamp(f.w, QStringLiteral("Grain"), QStringLiteral("Sand"));
    std::vector<Box> boxes;

    // a collection deleted on disk: Delete Collection is refused, and it goes
    fs::remove_all(f.userPath() / "rocks");
    pickCategory(f.dock, QStringLiteral("Rocks"));
    answerBoxes(f.w, {QMessageBox::Yes}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    CHECK(boxes.size() == 1);
    CHECK(status(f.w).startsWith(QStringLiteral("Collection not deleted: no collection named \"Rocks\"")));
    CHECK_FALSE(listed(f.dock).contains(QStringLiteral("Rocks")));
    CHECK_FALSE(f.dock->findCollection(QStringLiteral("Rocks")));

    // a stamp removed on disk, and another added: Remove Stamp is refused,
    // and the count is fresh
    pickCategory(f.dock, QStringLiteral("Sand"));
    pickStamp(f.dock, QStringLiteral("Grain"));
    const library::Collection* sand = f.dock->findCollection(QStringLiteral("Sand"));
    REQUIRE(sand);
    CellBlock dune(1, 1);
    dune.at(0, 0).ch = U'#';
    library::addStamp(f.userPath(), "Sand", "Dune", {}, dune, false);
    fs::remove(f.userPath() / "sand" / "grain.ans");
    fs::remove(f.userPath() / "sand" / "grain.json");
    answerBoxes(f.w, {QMessageBox::Yes}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
    CHECK(boxes.size() == 1);
    CHECK(status(f.w).startsWith(QStringLiteral("Stamp not removed: ")));
    CHECK(f.dock->shownNames() == QStringList{QStringLiteral("Dune")});
    CHECK(f.dock->findCollection(QStringLiteral("Sand"))->stamps == 1);

    // a collection made on disk: New Collection of its name is refused, and it shows
    library::createCollection(f.userPath(), "Late");
    QString error;
    CHECK_FALSE(f.dock->createCollection(QStringLiteral("Late"), &error));
    CHECK(error == QStringLiteral("a collection named \"Late\" already exists"));
    CHECK(listed(f.dock).contains(QStringLiteral("Late")));
}

TEST_CASE("Delete Collection on an empty collection says so, not \"the 0 stamps in it\"", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Empty"));
    pickCategory(f.dock, QStringLiteral("Empty"));
    std::vector<Box> boxes;

    answerBoxes(f.w, {QMessageBox::No}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].text == QStringLiteral("Delete the empty collection \"Empty\"?\nIt goes to the trash."));

    f.trashRefuses = true;
    answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[1].text.endsWith(
        QStringLiteral("Delete the empty collection \"Empty\" permanently? This can't be undone.")));
    CHECK_FALSE(boxes[1].text.contains(QStringLiteral("0 stamps")));

    f.trashRefuses = false;
    answerBoxes(f.w, {QMessageBox::Yes}, &boxes, [&] { act(f.w, "deleteLibraryCollection")->trigger(); });
    REQUIRE(boxes.size() == 1);
    CHECK(status(f.w) == QStringLiteral("Moved the empty collection \"Empty\" to the trash"));
    CHECK_FALSE(fs::exists(f.userPath() / "empty"));
}

TEST_CASE("answerBoxes stops answering when its call returns, though fewer boxes opened", "[library][collections]") {
    CollectionFixture f;
    makeCollection(f.dock, QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Pebble"), QStringLiteral("Rocks"));
    saveStamp(f.w, QStringLiteral("Flint"), QStringLiteral("Rocks"));
    pickCategory(f.dock, QStringLiteral("Rocks"));
    std::vector<Box> boxes;

    // two answers, one box: the No is left over
    pickStamp(f.dock, QStringLiteral("Pebble"));
    answerBoxes(f.w, {QMessageBox::Yes, QMessageBox::No}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
    CHECK(boxes.size() == 1);
    CHECK_FALSE(fs::exists(f.userPath() / "rocks" / "pebble.ans"));

    // and must not answer Flint's box
    pickStamp(f.dock, QStringLiteral("Flint"));
    answerBoxes(f.w, {QMessageBox::Yes}, &boxes, [&] { act(f.w, "removeLibraryStamp")->trigger(); });
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].text.contains(QStringLiteral("Flint")));
    CHECK_FALSE(fs::exists(f.userPath() / "rocks" / "flint.ans"));
}
