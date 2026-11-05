// User frame styles in the window (#345 slice 3): Selection > Save as Frame
// Style... writes a .frame.json to the user library, and the Frame tool and
// Tool Options list it after the bundled styles. main() is in test_app.cpp,
// which points the libraries at empty scratch directories.

#include "canvas_view.h"
#include "document.h"
#include "frame_style_dialog.h"
#include "frame_tool.h"
#include "library_dock.h"
#include "main_window.h"
#include "select_tool.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/frame.h"
#include "blockwright/library.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QFile>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <filesystem>
#include <fstream>

using namespace bw;
namespace fs = std::filesystem;

namespace {

fs::path fspath(const QString& s) { return fs::path(QFile::encodeName(s).toStdString()); }

// The id the Frame tool knows a saved style by: its path's bytes
// percent-encoded but for '/' (frameStyleId), never the path as it reads.
QString libraryId(const QString& path) { return frameStyleId(fspath(path)); }

void mouse(CanvasView* v, QEvent::Type type, QPoint cell) {
    const QPointF at = v->cellRect(cell).center();
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

void drag(CanvasView* v, QPoint from, QPoint to) {
    mouse(v, QEvent::MouseButtonPress, from);
    mouse(v, QEvent::MouseMove, (from + to) / 2);
    mouse(v, QEvent::MouseMove, to);
    mouse(v, QEvent::MouseButtonRelease, to);
}

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

FrameTool* frameTool(MainWindow& w) {
    act(w, "frameTool")->trigger();
    auto* t = qobject_cast<FrameTool*>(w.tools()->active());
    REQUIRE(t);
    return t;
}

Canvas laid(Canvas base, const Canvas& frame, int x, int y) {
    for (int fy = 0; fy < frame.height(); ++fy)
        for (int fx = 0; fx < frame.width(); ++fx)
            if (!frame.at(fx, fy).transparent && base.contains(x + fx, y + fy))
                base.at(x + fx, y + fy) = frame.at(fx, fy);
    return base;
}

// A window whose user library is a scratch directory (and no bundled one),
// with a 3x3 box of + corners and - | edges at the top left, selected.
struct Fixture {
    QTemporaryDir user;
    MainWindow w;
    LibraryDock* dock;
    Fixture() : dock(w.libraryWidget()) {
        REQUIRE(user.isValid());
        dock->setDirectories({}, fspath(user.path()));
        Document* doc = w.document();
        const char32_t rows[3][4] = {U"+-+", U"| |", U"+-+"};
        CellEdit e;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                if (rows[y][x] != U' ')
                    e.changes.push_back({x, y, doc->layerCanvas().at(x, y),
                                         Cell{rows[y][x], Color::indexed(Yellow), Color::indexed(Blue)}});
        REQUIRE(doc->edit(e));
        select({0, 0, 3, 3});
    }
    void select(const CellRect& r) {
        w.tools()->setActive(w.selectTool());
        w.selectTool()->select(r);
    }
    ~Fixture() {
        // The style is a setting; leave it as the other tests expect.
        frameTool(w)->setStyle(QStringLiteral("single"));
    }
    QString file(const char* name) const { return QDir(user.path()).filePath(QString::fromLatin1(name)); }
};

} // namespace

TEST_CASE("a selection saved as a frame style is picked in the Frame tool and draws as one undo step") {
    Fixture f;
    CHECK(act(f.w, "saveFrameStyle")->isEnabled());
    QString error;
    const QString path = f.w.saveSelectionAsFrameStyle(QStringLiteral("Plus Box"), 1, 1, 1, 1, false, false, &error);
    INFO(error.toStdString());
    REQUIRE(!path.isEmpty());
    CHECK(QDir::cleanPath(path) == f.file("plus-box.frame.json"));
    const FrameStyle style = library::readFrameStyleEntry(fspath(path)).style;
    CHECK(style.name == "Plus Box");
    CHECK(style.source.at(1, 1).transparent);  // the open middle

    const QString id = libraryId(path);
    REQUIRE(f.dock->frameStyles().size() == 1);
    FrameTool* t = frameTool(f.w);
    CHECK(t->styleId() == id);
    QComboBox* box = f.w.toolOptions()->frameStyleBox();
    CHECK(box->count() == int(bundledFrameStyles().size()) + 1);
    CHECK(box->currentData().toString() == id);
    CHECK(box->itemText(box->count() - 1) == QStringLiteral("Plus Box"));
    CHECK_FALSE(box->itemIcon(box->count() - 1).isNull());

    // A drag draws it to fit, the edges repeated, as one undo step (with
    // no selection to keep it in).
    f.select({});
    t = frameTool(f.w);
    t->setPenColours(false);
    Document* doc = f.w.document();
    QUndoStack* undo = doc->undoStack();
    const Canvas before = doc->layerCanvas();
    const int edits = undo->count();
    drag(f.w.canvasView(), {10, 5}, {16, 8});
    const auto drawn = drawFrame(style, 7, 4);
    REQUIRE(drawn);
    CHECK(doc->layerCanvas() == laid(before, *drawn, 10, 5));
    CHECK(undo->count() == edits + 1);
    undo->undo();
    CHECK(doc->layerCanvas() == before);

    // Saving the same name again takes the next file, never the first.
    f.select({0, 0, 3, 3});
    const QString again = f.w.saveSelectionAsFrameStyle(QStringLiteral("Plus Box"), 1, 1, 1, 1, true, true, &error);
    CHECK(QDir::cleanPath(again) == f.file("plus-box-2.frame.json"));
    CHECK(f.dock->frameStyles().size() == 2);
    CHECK(t->styleId() == libraryId(again));

    // Refused: no name, and corners that leave no edge.
    CHECK(f.w.saveSelectionAsFrameStyle(QStringLiteral(" "), 1, 1, 1, 1, false, false, &error).isEmpty());
    CHECK_FALSE(error.isEmpty());
    CHECK(f.w.saveSelectionAsFrameStyle(QStringLiteral("Wide"), 2, 1, 1, 1, false, false, &error).isEmpty());
    CHECK(error.contains(QStringLiteral("no edge to repeat")));
    CHECK_FALSE(QFile::exists(f.file("wide.frame.json")));
}

TEST_CASE("a library frame style's id percent-encodes its path but for the slashes") {
#ifdef _WIN32
    // the drive's colon and the native backslashes are encoded too
    CHECK(frameStyleId(fs::path(L"C:\\Users\\a b\\50%.frame.json")) ==
          QStringLiteral("library:C%3A%5CUsers%5Ca%20b%5C50%25.frame.json"));
    CHECK(frameStyleId(fs::path(L"C:/x/y.frame.json")) == QStringLiteral("library:C%3A/x/y.frame.json"));
#else
    CHECK(frameStyleId(fs::path("/tmp/x/plain.frame.json")) == QStringLiteral("library:/tmp/x/plain.frame.json"));
    CHECK(frameStyleId(fs::path("/tmp/a b/c.frame.json")) == QStringLiteral("library:/tmp/a%20b/c.frame.json"));
    CHECK(frameStyleId(fs::path("/tmp/50%/c.frame.json")) == QStringLiteral("library:/tmp/50%25/c.frame.json"));
    CHECK(frameStyleId(fs::path("/tmp/C:/c.frame.json")) == QStringLiteral("library:/tmp/C%3A/c.frame.json"));
    // a backslash is a plain byte of a name here, not a separator
    CHECK(frameStyleId(fs::path("/tmp/a\\b.frame.json")) == QStringLiteral("library:/tmp/a%5Cb.frame.json"));
    // two names that aren't UTF-8 stay apart
    CHECK(frameStyleId(fs::path("/tmp/\xff.frame.json")) == QStringLiteral("library:/tmp/%FF.frame.json"));
#endif
}

TEST_CASE("a library frame style that goes away falls back to a bundled one, and comes back when it returns") {
    Fixture f;
    QString error;
    const QString path = f.w.saveSelectionAsFrameStyle(QStringLiteral("Plus Box"), 1, 1, 1, 1, false, false, &error);
    REQUIRE(!path.isEmpty());
    FrameTool* t = frameTool(f.w);
    const QString id = libraryId(path);
    REQUIRE(t->styleId() == id);

    const QString moved = f.file("elsewhere.txt");
    REQUIRE(QFile::rename(path, moved));
    f.dock->rescan();
    CHECK(t->styleId() == QString::fromStdString(bundledFrameStyles().front().id));
    CHECK(f.w.toolOptions()->frameStyleBox()->count() == int(bundledFrameStyles().size()));

    REQUIRE(QFile::rename(moved, path));
    f.dock->rescan();
    CHECK(t->styleId() == id);
    CHECK(f.w.toolOptions()->frameStyleBox()->currentData().toString() == id);

    // Picking another style by hand is what is remembered after that.
    t->setStyle(QStringLiteral("double"));
    f.dock->rescan();
    CHECK(t->styleId() == QStringLiteral("double"));
}

TEST_CASE("Selection > Save as Frame Style asks for the name and the corners' size") {
    Fixture f;
    bool ran = false;
    QTimer::singleShot(0, &f.w, [&] {
        auto* d = qobject_cast<FrameStyleDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        ran = true;
        QPushButton* ok = d->buttons()->button(QDialogButtonBox::Ok);
        CHECK_FALSE(ok->isEnabled());  // no name yet
        CHECK(d->leftSpin()->value() == 1);
        CHECK(d->leftSpin()->maximum() == 2);
        d->nameEdit()->setText(QStringLiteral("  Dash Box "));
        CHECK(ok->isEnabled());
        d->leftSpin()->setValue(2);
        CHECK_FALSE(ok->isEnabled());  // no edge left to repeat
        d->leftSpin()->setValue(1);
        d->keepBackgroundBox()->setChecked(true);
        REQUIRE(ok->isEnabled());
        ok->click();
    });
    act(f.w, "saveFrameStyle")->trigger();
    CHECK(ran);
    REQUIRE(QFile::exists(f.file("dash-box.frame.json")));
    const FrameStyle s = library::readFrameStyleEntry(fspath(f.file("dash-box.frame.json"))).style;
    CHECK(s.name == "Dash Box");
    CHECK(s.keepsBackground(0, 0));
    CHECK(frameTool(f.w)->styleId() == libraryId(f.file("dash-box.frame.json")));

    // Without a selection there is nothing to save.
    f.select({});
    CHECK_FALSE(act(f.w, "saveFrameStyle")->isEnabled());
}

TEST_CASE("a frame style's declared minimum doesn't size its thumbnail or the dialog's preview") {
    Fixture f;
    QFile huge(f.file("huge.frame.json"));
    REQUIRE(huge.open(QIODevice::WriteOnly));
    huge.write(R"({"id":"huge","insets":[0,0,0,0],"fill":true,"min":[400,5000],"rows":["X"],"fg":["7"],"bg":["0"]})");
    huge.close();
    f.dock->rescan();
    REQUIRE(f.dock->frameStyles().size() == 1);
    QComboBox* box = f.w.toolOptions()->frameStyleBox();
    REQUIRE(box->count() == int(bundledFrameStyles().size()) + 1);
    for (int i = 0; i < box->count(); ++i) {
        INFO(box->itemText(i).toStdString());
        const QList<QSize> sizes = box->itemIcon(i).availableSizes();
        REQUIRE_FALSE(sizes.isEmpty());
        for (const QSize& s : sizes)
            CHECK((s.width() <= box->iconSize().width() && s.height() <= box->iconSize().height()));
    }

    // The dialog's preview of a 100 x 100 selection with corners 49 deep.
    Canvas cells(100, 100);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x)
            cells.at(x, y) = Cell{U'X', Color::indexed(Yellow), Color::indexed(Blue)};
    FrameStyleDialog d(cells);
    for (QSpinBox* s : {d.leftSpin(), d.rightSpin(), d.topSpin(), d.bottomSpin()})
        s->setValue(49);
    REQUIRE(d.style());
    const QSize shown = d.preview()->pixmap().size();
    CHECK(!shown.isEmpty());
    CHECK(shown.width() <= 320);
    CHECK(shown.height() <= 256);
}

#ifndef _WIN32
TEST_CASE("frame styles whose file names differ only in bytes that aren't UTF-8 are told apart") {
    Fixture f;
    const fs::path dir = fspath(f.user.path());
    const std::string names[] = {"a\xfe.frame.json", "a\xff.frame.json"};
    for (int i = 0; i < 2; ++i) {
        std::ofstream out(dir / names[i], std::ios::binary);
        out << R"({"id":"a","name":"Style )" << i << R"(","insets":[1,1,1,1],"rows":["+-+","| |","+-+"],)"
            << R"("fg":["777","777","777"],"bg":["000","000","000"]})";
        if (!out.good() || !fs::exists(dir / names[i]))
            SKIP("this file system won't hold a name that isn't UTF-8");
    }
    f.dock->rescan();
    const std::vector<FrameStyle> styles = f.dock->frameStyles();
    REQUIRE(styles.size() == 2);
    CHECK(styles[0].id != styles[1].id);
    FrameTool* t = frameTool(f.w);
    for (const FrameStyle& s : styles) {
        const FrameStyle* found = t->findStyle(QString::fromStdString(s.id));
        REQUIRE(found);
        CHECK(found->name == s.name);
        t->setStyle(QString::fromStdString(s.id));
        CHECK(t->style().name == s.name);
    }
    CHECK(styles[0].name != styles[1].name);
}
#endif

TEST_CASE("picking the fallback style by hand replaces a library style that went away") {
    Fixture f;
    QString error;
    const QString path = f.w.saveSelectionAsFrameStyle(QStringLiteral("Plus Box"), 1, 1, 1, 1, false, false, &error);
    REQUIRE(!path.isEmpty());
    FrameTool* t = frameTool(f.w);
    const QString id = t->styleId();
    REQUIRE(id != QStringLiteral("single"));
    QComboBox* box = f.w.toolOptions()->frameStyleBox();

    const QString moved = f.file("elsewhere.txt");
    REQUIRE(QFile::rename(path, moved));
    f.dock->rescan();
    REQUIRE(t->styleId() == QStringLiteral("single"));
    REQUIRE(box->currentData().toString() == QStringLiteral("single"));
    // chosen in the picker, where it is already shown
    emit box->activated(box->currentIndex());
    REQUIRE(QFile::rename(moved, path));
    f.dock->rescan();
    CHECK(t->styleId() == QStringLiteral("single"));
    CHECK(box->currentData().toString() == QStringLiteral("single"));

    // and the same through the tool
    t->setStyle(id);
    REQUIRE(t->styleId() == id);
    REQUIRE(QFile::rename(path, moved));
    f.dock->rescan();
    REQUIRE(t->styleId() == QStringLiteral("single"));
    t->setStyle(QStringLiteral("single"));
    REQUIRE(QFile::rename(moved, path));
    f.dock->rescan();
    CHECK(t->styleId() == QStringLiteral("single"));
}
