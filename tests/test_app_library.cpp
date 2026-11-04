// The Library dock (PLAN §12) in the main window: thumbnails, categories and
// search; placing a stamp by double-click or drop as a floating selection in
// transparent paste mode; and Selection > Save to Library.... main() is in
// test_app.cpp, which points the libraries at empty scratch directories.

#include "fs_quirks.h"
#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "library_dock.h"
#include "main_window.h"
#include "select_tool.h"
#include "tool.h"

#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/render.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QScrollArea>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <filesystem>
#include <memory>

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
    const fs::path tree = fs::path(BW_BUNDLED_LIBRARY) / "trees" / "golden-tree.ans";
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
