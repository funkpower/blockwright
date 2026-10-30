// Projects in the app (PLAN §6, #90): opening a .bwp zip or a project
// directory with its layers, iCE, SAUCE fields, IRC budget, notes,
// snapshots and kept files; saving it back in its form; a flat .ans save
// through project::toAnsi; snapshots from the GUI.

#include "document.h"
#include "main_window.h"

#include "blockwright/composite.h"
#include "blockwright/project.h"
#include "blockwright/sauce.h"
#include "blockwright/text_style.h"

#include "scratch_settings.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <filesystem>

#include <sys/stat.h>
#include <unistd.h>

using namespace bw;
namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& p) { return QString::fromStdString(p.string()); }
fs::path fpath(const QString& p) { return fs::path(QFile::encodeName(p).toStdString()); }

QAction* action(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

// tux with two layers over it (one hidden, one moved), its SAUCE title, a
// custom IRC budget, notes, a first snapshot and a reference kept as bytes.
project::Project sample() {
    project::Project p = project::fromAnsi(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans")));
    Layer top = makeLayer(LayerKind::Cells, 6, 2, "Top", 3, 1);
    top.cells.at(0, 0) = Cell{U'#'};
    top.cells.at(5, 1) = Cell{U'@'};
    p.layers.add(std::move(top));
    Layer hidden = makeLayer(LayerKind::Cells, 4, 4, "Hidden");
    hidden.cells.at(1, 1) = Cell{U'X'};
    hidden.props.visible = false;
    p.layers.add(std::move(hidden));
    p.layers.reserveIds(9);
    if (!p.sauce)
        p.sauce = Sauce::fresh(p.layers.width(), p.layers.height());
    p.sauce->title = "Project Tux";
    p.ircBudget = {"custom", 400};
    p.notes = "brief: tux, two layers\n";
    p.snapshots.push_back({1, writeAnsi(project::toAnsi(p)).bytes});
    p.files["refs/sketch.txt"] = "pencil";
    return p;
}

// The sample written as a zip at `path`, and what reading it gives.
project::Project writeSampleZip(const QString& path) {
    project::writeZip(sample(), fpath(path));
    return project::read(fpath(path));
}

project::Project writeSampleDirectory(const QString& path) {
    project::writeDirectory(sample(), fpath(path));
    return project::read(fpath(path));
}

void writeBytes(const QString& path, const std::string& bytes) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
}

void drawAt(MainWindow& w, int x, int y, char32_t ch) {
    CellEdit e;
    e.changes.push_back({x, y, w.document()->layerCanvas().at(x, y), Cell{ch}});
    REQUIRE(w.document()->edit(e));
}

// tux.ans copied to `path`, so a save over it leaves the golden file alone.
QString tuxAt(const QString& path) {
    writeBytes(path, bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"));
    return path;
}

// Answers the next message box with `button`; `asked` says one came and
// `text`, when given, takes what it said.
void answer(QWidget* context, QMessageBox::StandardButton button, bool* asked, QString* text = nullptr) {
    *asked = false;
    QTimer::singleShot(0, context, [=] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box)
            return;
        *asked = true;
        if (text)
            *text = box->text();
        box->button(button)->click();
    });
}

} // namespace

TEST_CASE("a .bwp project opens with everything it holds and saves back as a zip") {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("piece.bwp"));
    const project::Project p = writeSampleZip(path);

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    Document& d = *w.document();
    CHECK(d.path() == path);
    CHECK(d.form() == FileForm::ProjectZip);
    CHECK(d.project() == p);
    REQUIRE(d.layers().size() == 3);
    CHECK(d.layers()[1].props.name == "Top");
    CHECK_FALSE(d.layers()[2].props.visible);
    CHECK(d.layers().nextId() == 9);
    CHECK(d.activeLayer() == d.layers()[0].props.id);
    CHECK(d.extras().ircBudget.bytes == 400);
    CHECK(d.extras().notes == p.notes);
    CHECK(d.extras().snapshots.size() == 1);
    CHECK(d.extras().files.at("refs/sketch.txt") == "pencil");
    CHECK(d.ansi().sauce->title == "Project Tux");
    CHECK(d.canvas() == composite(p.layers, {p.ice, Cell{}}));
    CHECK_FALSE(d.undoStack()->canUndo());
    CHECK(w.windowTitle() == QStringLiteral("piece.bwp[*] - Blockwright"));
    CHECK_FALSE(w.isWindowModified());

    // an edit marks it modified; Save writes the zip again and clears it
    drawAt(w, 0, 0, U'Z');
    CHECK(w.isWindowModified());
    REQUIRE(w.save());
    CHECK_FALSE(w.isWindowModified());
    CHECK(d.path() == path);
    CHECK(d.form() == FileForm::ProjectZip);
    CHECK(project::isProject(fpath(path)));
    const project::Project saved = project::read(fpath(path));
    CHECK(saved == d.project());
    CHECK(saved.layers.size() == 3);
    CHECK(saved.layers[0].cells.at(0, 0).ch == U'Z');
    CHECK(saved.notes == p.notes);
    CHECK(saved.snapshots == p.snapshots);
    CHECK(saved.files == p.files);
    CHECK(saved.ircBudget == p.ircBudget);

    // undoing back to the saved cells is clean again
    d.undoStack()->undo();
    CHECK(w.isWindowModified());
    d.undoStack()->redo();
    CHECK_FALSE(w.isWindowModified());
}

TEST_CASE("a project directory opens by its name and saves back as a directory") {
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(QStringLiteral("piece"));
    const project::Project p = writeSampleDirectory(dir);

    MainWindow w;
    QString error;
    // given with a trailing separator, it is still titled by its name
    REQUIRE(w.openFile(dir + QLatin1Char('/'), &error));
    Document& d = *w.document();
    CHECK(d.path() == dir);
    CHECK(d.form() == FileForm::ProjectDirectory);
    CHECK(d.project() == p);
    CHECK(w.windowTitle() == QStringLiteral("piece[*] - Blockwright"));
    CHECK(action(w, "saveAsOtherForm")->text().contains(QStringLiteral("File")));

    drawAt(w, 1, 0, U'Q');
    CHECK(w.isWindowModified());
    REQUIRE(w.save());
    CHECK_FALSE(w.isWindowModified());
    CHECK(d.form() == FileForm::ProjectDirectory);
    CHECK(QFileInfo(dir).isDir());
    CHECK(QFile::exists(dir + QStringLiteral("/manifest.json")));
    CHECK(QFile::exists(dir + QStringLiteral("/refs/sketch.txt")));
    CHECK(QFile::exists(dir + QStringLiteral("/snapshots/r1.ans")));
    const project::Project saved = project::read(fpath(dir));
    CHECK(saved == d.project());
    CHECK(saved.layers[0].cells.at(1, 0).ch == U'Q');
}

TEST_CASE("Save As moves a project between .bwp and a directory and exports it as .ans") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    const project::Project p = writeSampleZip(zip);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    CHECK(action(w, "snapshot")->isEnabled());
    CHECK(action(w, "saveAsOtherForm")->text().contains(QStringLiteral("Directory")));

    // a flat .ans is the visible composite through project::toAnsi: the
    // hidden layer adds nothing, SAUCE and iCE are the project's
    const QString flat = tmp.filePath(QStringLiteral("flat.ans"));
    REQUIRE(w.saveFile(flat, &error));
    const std::string bytes = bwtest::readFile(fpath(flat));
    CHECK(bytes == writeAnsi(project::toAnsi(p)).bytes);
    CHECK(parseAnsi(bytes).sauce->title == "Project Tux");
    // the .ans holds none of the layers, so it is an export: the project
    // stays the document's file, as saved as it was
    CHECK(d.path() == zip);
    CHECK(d.form() == FileForm::ProjectZip);
    CHECK(w.windowTitle() == QStringLiteral("piece.bwp[*] - Blockwright"));
    CHECK_FALSE(w.isWindowModified());
    CHECK(action(w, "snapshot")->isEnabled());
    CHECK(d.layers().size() == 3);

    const QString again = tmp.filePath(QStringLiteral("again.BWP"));
    REQUIRE(w.saveFile(again, &error));
    CHECK(d.form() == FileForm::ProjectZip);
    CHECK(project::read(fpath(again)) == p);
    CHECK(action(w, "snapshot")->isEnabled());

    // an existing empty directory takes a project directory
    const QString dir = tmp.filePath(QStringLiteral("dir"));
    REQUIRE(QDir().mkpath(dir));
    REQUIRE(w.saveFile(dir, &error));
    CHECK(d.form() == FileForm::ProjectDirectory);
    CHECK(d.path() == dir);
    CHECK(project::read(fpath(dir)) == p);
    CHECK(action(w, "saveAsOtherForm")->text().contains(QStringLiteral("File")));

    // a directory holding anything else is refused, and nothing changes
    const QString other = tmp.filePath(QStringLiteral("other"));
    REQUIRE(QDir().mkpath(other));
    writeBytes(other + QStringLiteral("/keep.txt"), "mine");
    CHECK_FALSE(w.saveFile(other, &error));
    CHECK_FALSE(error.isEmpty());
    CHECK(bwtest::readFile(fpath(other + QStringLiteral("/keep.txt"))) == "mine");
    CHECK(d.path() == dir);
    CHECK(d.form() == FileForm::ProjectDirectory);
}

TEST_CASE("an export to .ans leaves a layered project and its pending snapshot unsaved") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    writeSampleZip(zip);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    drawAt(w, 0, 0, U'Z');
    REQUIRE(w.snapshot(&error) == 2);

    const QString flat = tmp.filePath(QStringLiteral("flat.ans"));
    REQUIRE(w.saveFile(flat, &error));
    CHECK(bwtest::readFile(fpath(flat)) == writeAnsi(project::toAnsi(d.project())).bytes);
    // the .ans holds neither the layers nor r2: the project is still the
    // file, and the edit and r2 are still unsaved
    CHECK(d.path() == zip);
    CHECK(d.form() == FileForm::ProjectZip);
    CHECK(d.isModified());
    CHECK(d.extras().snapshots.size() == 2);
    const project::Project disk = project::read(fpath(zip));
    CHECK(disk.layers[0].cells.at(0, 0).ch != U'Z');
    CHECK(disk.snapshots.size() == 1);
    MainWindow flatWindow;
    REQUIRE(flatWindow.openFile(flat, &error));
    CHECK(flatWindow.document()->layers().size() == 1);

    // Save then writes the project, r2 with it
    REQUIRE(w.save());
    CHECK_FALSE(d.isModified());
    CHECK(project::read(fpath(zip)) == d.project());
    CHECK(project::read(fpath(zip)).snapshots.size() == 2);

    // a project directory exports the same way
    const QString dir = tmp.filePath(QStringLiteral("dir"));
    REQUIRE(QDir().mkpath(dir));
    REQUIRE(w.saveFile(dir, &error));
    drawAt(w, 1, 0, U'Y');
    REQUIRE(w.saveFile(flat, &error));
    CHECK(d.path() == dir);
    CHECK(d.form() == FileForm::ProjectDirectory);
    CHECK(d.isModified());
}

TEST_CASE("Save As .ans makes a project the flat file when the .ans holds all of it") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("one.bwp"));
    project::writeZip(project::fromAnsi(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"))),
                      fpath(zip));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    drawAt(w, 0, 0, U'Z');
    const QString flat = tmp.filePath(QStringLiteral("one.ans"));
    REQUIRE(w.saveFile(flat, &error));
    CHECK(d.path() == flat);
    CHECK(d.form() == FileForm::Ansi);
    CHECK_FALSE(d.isModified());
    CHECK_FALSE(action(w, "snapshot")->isEnabled());

    // a project's notes or kept files are more than an .ans holds
    project::Project p = project::fromAnsi(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans")));
    p.notes = "brief\n";
    project::writeZip(p, fpath(zip));
    REQUIRE(w.openFile(zip, &error));
    REQUIRE(w.saveFile(flat, &error));
    CHECK(d.path() == zip);
    CHECK(d.form() == FileForm::ProjectZip);
}

// Codex round 1: an .ans keeps a styled layer's look but not its cells or
// its style, so saving one is an export.
TEST_CASE("Save As .ans keeps a project with a styled layer its file") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("styled.bwp"));
    project::Project p = project::fromAnsi(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans")));
    REQUIRE(p.layers.size() == 1);
    Layer& l = *p.layers.find(p.layers[0].props.id);
    REQUIRE(l.bounds() == p.layers.canvasRect());
    TextStyle style;
    style.effects.push_back(Effect{true, FillEffect{FillMode::Flat, Yellow, "fire"}});
    l.props.style = style;
    project::writeZip(p, fpath(zip));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    REQUIRE(d.layers()[0].props.style);
    const QString flat = tmp.filePath(QStringLiteral("styled.ans"));
    REQUIRE(w.saveFile(flat, &error));
    CHECK(bwtest::readFile(fpath(flat)) == writeAnsi(project::toAnsi(d.project())).bytes);
    CHECK(d.path() == zip);
    CHECK(d.form() == FileForm::ProjectZip);
}

TEST_CASE("a flat save of a layered .ans leaves it unsaved until a project holds the layers") {
    QTemporaryDir tmp;
    const QString ans = tuxAt(tmp.filePath(QStringLiteral("piece.ans")));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(ans, &error));
    Document& d = *w.document();
    d.addLayer(LayerKind::Cells);
    drawAt(w, 0, 0, U'#');
    REQUIRE(w.saveFile(ans, &error));
    CHECK(bwtest::readFile(fpath(ans)) == writeAnsi(project::toAnsi(d.project())).bytes);
    CHECK(d.path() == ans);
    CHECK(d.form() == FileForm::Ansi);
    CHECK(d.isModified());
    // undoing back to how the file opened is no longer how the file is
    while (d.undoStack()->canUndo())
        d.undoStack()->undo();
    CHECK(d.isModified());
    // one layer again: Save writes all of it
    REQUIRE(w.save());
    CHECK_FALSE(d.isModified());

    // Save As another .ans takes the new name and stays unsaved
    d.undoStack()->redo();
    d.undoStack()->redo();
    REQUIRE(d.layers().size() == 2);
    const QString other = tmp.filePath(QStringLiteral("other.ans"));
    REQUIRE(w.saveFile(other, &error));
    CHECK(d.path() == other);
    CHECK(d.isModified());
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    REQUIRE(w.saveFile(zip, &error));
    CHECK_FALSE(d.isModified());
    CHECK(project::read(fpath(zip)).layers.size() == 2);
}

TEST_CASE("New and closing ask before dropping what a flat save didn't keep") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    writeSampleZip(zip);
    QString error;
    bool asked = false;
    {
        // an exported project with a pending snapshot
        MainWindow w;
        REQUIRE(w.openFile(zip, &error));
        REQUIRE(w.snapshot(&error) == 2);
        REQUIRE(w.saveFile(tmp.filePath(QStringLiteral("flat.ans")), &error));
        answer(&w, QMessageBox::Cancel, &asked);
        w.newFile();
        CHECK(asked);
        CHECK(w.document()->path() == zip);
        CHECK(w.document()->extras().snapshots.size() == 2);
        answer(&w, QMessageBox::Cancel, &asked);
        CHECK_FALSE(w.close());
        CHECK(asked);
        answer(&w, QMessageBox::Discard, &asked);
        CHECK(w.close());
        CHECK(asked);
    }
    {
        // a layered .ans: the prompt's Save writes the flat file, which
        // still doesn't hold the layers, so nothing is dropped
        const QString ans = tuxAt(tmp.filePath(QStringLiteral("piece.ans")));
        MainWindow w;
        REQUIRE(w.openFile(ans, &error));
        Document& d = *w.document();
        d.addLayer(LayerKind::Cells);
        drawAt(w, 0, 0, U'#');
        answer(&w, QMessageBox::Save, &asked);
        w.newFile();
        CHECK(asked);
        CHECK(bwtest::readFile(fpath(ans)) == writeAnsi(project::toAnsi(d.project())).bytes);
        CHECK(d.path() == ans);
        CHECK(d.layers().size() == 2);
        CHECK(d.isModified());
        answer(&w, QMessageBox::Discard, &asked);
        w.newFile();
        CHECK(asked);
        CHECK(d.path().isEmpty());
        CHECK(d.layers().size() == 1);
    }
}

TEST_CASE("an untitled document a flat file can't hold says Save writes it flattened") {
    MainWindow w;
    Document& d = *w.document();
    bool asked = false;
    QString text;
    // one cells layer: an .ans holds it, so the prompt is the plain one
    drawAt(w, 0, 0, U'#');
    answer(&w, QMessageBox::Cancel, &asked, &text);
    w.newFile();
    REQUIRE(asked);
    CHECK(text == QStringLiteral("The canvas has unsaved changes."));
    // a second layer: Save to an .ans would write it flattened
    d.addLayer(LayerKind::Cells);
    drawAt(w, 1, 0, U'@');
    REQUIRE(d.path().isEmpty());
    answer(&w, QMessageBox::Cancel, &asked, &text);
    w.newFile();
    REQUIRE(asked);
    CHECK(text == QStringLiteral("The canvas has unsaved changes.\nSaved as an .ans it is written flattened and the "
                                 "document stays open; only a project saves its layers and IRC budget."));
    CHECK(d.layers().size() == 2);
    CHECK(d.isModified());
}

TEST_CASE("a snapshot redone after a flat save is dropped, not added to the .ans") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("one.bwp"));
    project::writeZip(project::fromAnsi(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"))),
                      fpath(zip));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    REQUIRE(w.snapshot(&error) == 1);
    drawAt(w, 0, 0, U'Z');
    d.undoStack()->undo();
    d.undoStack()->undo();
    REQUIRE(d.extras().snapshots.empty());
    // the .ans holds all of it, so it becomes the document's file
    const QString flat = tmp.filePath(QStringLiteral("one.ans"));
    REQUIRE(w.saveFile(flat, &error));
    REQUIRE(d.form() == FileForm::Ansi);
    REQUIRE_FALSE(d.isModified());
    // a flat file keeps no snapshots: the redo drops it and leaves the
    // document saved, and Edit > Redo offers the edit after it
    QAction* redo = action(w, "redo");
    REQUIRE(redo->text() == QStringLiteral("&Redo Snapshot r1"));
    redo->trigger();
    CHECK(d.extras().snapshots.empty());
    CHECK_FALSE(d.isModified());
    CHECK(d.flatHoldsAll());
    CHECK(redo->isEnabled());
    CHECK(redo->text() == QStringLiteral("&Redo Edit"));
    // the edit after it still redoes, and Save makes it clean again
    REQUIRE(d.undoStack()->canRedo());
    redo->trigger();
    CHECK(d.canvas().at(0, 0).ch == U'Z');
    CHECK(d.extras().snapshots.empty());
    CHECK(d.isModified());
    REQUIRE(w.save());
    CHECK_FALSE(d.isModified());
    CHECK(bwtest::readFile(fpath(flat)) == writeAnsi(project::toAnsi(d.project())).bytes);
    CHECK_FALSE(d.undoStack()->canRedo());
    // undo goes back past the edit to the save, not to a snapshot
    d.undoStack()->undo();
    CHECK_FALSE(d.undoStack()->canUndo());
    CHECK(d.extras().snapshots.empty());
}

TEST_CASE("a snapshot that was the only redo leaves Redo disabled once dropped") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("one.bwp"));
    project::writeZip(project::fromAnsi(parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"))),
                      fpath(zip));
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    REQUIRE(w.snapshot(&error) == 1);
    action(w, "undo")->trigger();
    REQUIRE(w.saveFile(tmp.filePath(QStringLiteral("one.ans")), &error));
    REQUIRE(d.form() == FileForm::Ansi);
    QAction* redo = action(w, "redo");
    REQUIRE(redo->isEnabled());
    REQUIRE(redo->text() == QStringLiteral("&Redo Snapshot r1"));
    redo->trigger();
    CHECK(d.extras().snapshots.empty());
    CHECK_FALSE(d.isModified());
    CHECK_FALSE(d.undoStack()->canRedo());
    CHECK_FALSE(redo->isEnabled());
    CHECK(redo->text() == QStringLiteral("&Redo"));
    // a second trigger has nothing to redo
    redo->trigger();
    CHECK_FALSE(d.isModified());
    CHECK(d.extras().snapshots.empty());
}

TEST_CASE("the flattening warning follows what a flat file can't hold, and only for a flat document") {
    QString error;
    bool asked = false;
    QString text;
    const QString warning = QStringLiteral(
        "The canvas has unsaved changes.\nSaved as an .ans it is written flattened and the document stays open; "
        "only a project saves its layers and IRC budget.");
    {
        // one layer, but an IRC budget an .ans can't carry
        MainWindow w;
        Document& d = *w.document();
        project::IrcBudget budget;
        budget.bytes = 400;
        REQUIRE(d.setIrcBudget(budget));
        REQUIRE(d.layers().size() == 1);
        REQUIRE(d.path().isEmpty());
        REQUIRE_FALSE(d.flatHoldsAll());
        answer(&w, QMessageBox::Cancel, &asked, &text);
        w.newFile();
        REQUIRE(asked);
        CHECK(text == warning);
    }
    {
        // a layered project: Save writes the .bwp, which keeps them
        QTemporaryDir tmp;
        const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
        writeSampleZip(zip);
        MainWindow w;
        REQUIRE(w.openFile(zip, &error));
        Document& d = *w.document();
        REQUIRE(d.layers().size() > 1);
        REQUIRE(d.form() != FileForm::Ansi);
        drawAt(w, 0, 0, U'#');
        REQUIRE(d.isModified());
        answer(&w, QMessageBox::Cancel, &asked, &text);
        w.newFile();
        REQUIRE(asked);
        CHECK(text == QStringLiteral("The canvas has unsaved changes."));
    }
}

TEST_CASE("a project saved and opened in the app keeps every layer property and format field") {
    QTemporaryDir tmp;
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(tuxAt(tmp.filePath(QStringLiteral("tux.ans"))), &error));
    Document& d = *w.document();
    const LayerId px = d.addLayer(LayerKind::Pixels);
    REQUIRE(d.removeLayer(d.addLayer(LayerKind::Cells)));
    REQUIRE(d.setLayerAlphaLocked(px, true));
    REQUIRE(d.setLayerVisible(d.layers()[0].props.id, false));
    LayerProperties lp = d.layers().find(px)->props;
    lp.locked = true;
    lp.mode = LayerMode::ColoursOnly;
    lp.x = -2;
    lp.y = 3;
    REQUIRE(d.setLayerProperties(px, lp, QStringLiteral("props")));
    REQUIRE(d.setIce(!d.ansi().ice));
    Sauce s = d.ansi().sauce ? *d.ansi().sauce : Sauce::fresh(80, 25);
    s.title = "RT";
    s.author = "fp";
    REQUIRE(d.setSauceFields(s));
    REQUIRE(d.setLetterSpacing(9));
    const project::Project before = d.project();

    const QString zip = tmp.filePath(QStringLiteral("rt.bwp"));
    REQUIRE(w.saveFile(zip, &error, true));
    MainWindow w2;
    REQUIRE(w2.openFile(zip, &error));
    const project::Project after = w2.document()->project();
    CHECK(after.layers == before.layers);
    CHECK(after.layers.nextId() == before.layers.nextId());
    CHECK(after.ice == before.ice);
    CHECK(after.sauce->title == "RT");
    CHECK(after.sauce->author == "fp");
    CHECK(w2.document()->ansi().sauce->letterSpacing() == d.ansi().sauce->letterSpacing());
    CHECK(w2.document()->canvas() == d.canvas());

    // zip, then directory, then open gives the same project
    const QString dir = tmp.filePath(QStringLiteral("rtdir"));
    REQUIRE(QDir().mkpath(dir));
    REQUIRE(w2.saveFile(dir, &error, true));
    MainWindow w3;
    REQUIRE(w3.openFile(dir, &error));
    CHECK(w3.document()->project() == after);
}

TEST_CASE("a failed save over a project directory leaves it readable as it was (#114)") {
    if (::geteuid() == 0)
        SKIP("root writes into a read-only directory");
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(QStringLiteral("piece"));
    const project::Project before = writeSampleDirectory(dir);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(dir, &error));
    Document& d = *w.document();
    drawAt(w, 0, 0, U'Z');         // a change to a layer the manifest on disk lists
    d.addLayer(LayerKind::Cells);  // and a layer with an id it doesn't

    const QByteArray refs = QFile::encodeName(dir + QStringLiteral("/refs"));
    REQUIRE(::chmod(refs.constData(), 0555) == 0);
    const bool saved = w.saveFile(dir, &error);
    ::chmod(refs.constData(), 0755);
    CHECK_FALSE(saved);
    CHECK_FALSE(error.isEmpty());
    CHECK(d.isModified());
    CHECK(project::read(fpath(dir)) == before);

    REQUIRE(w.save());
    CHECK(project::read(fpath(dir)) == d.project());
}

TEST_CASE("a failed project save keeps the snapshot pending and the project unsaved") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    writeSampleZip(zip);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    REQUIRE(w.snapshot(&error) == 2);
    const QString ro = tmp.filePath(QStringLiteral("ro"));
    REQUIRE(QDir().mkpath(ro));
    REQUIRE(::chmod(QFile::encodeName(ro).constData(), 0555) == 0);
    const bool saved = w.saveFile(ro + QStringLiteral("/x.bwp"), &error);
    ::chmod(QFile::encodeName(ro).constData(), 0755);
    CHECK_FALSE(saved);
    CHECK_FALSE(error.isEmpty());
    CHECK(d.path() == zip);
    CHECK(d.isModified());
    CHECK(d.extras().snapshots.size() == 2);
    CHECK_FALSE(QFile::exists(ro + QStringLiteral("/x.bwp")));

    // saved, saved again unchanged, then r2 undone and saved off the disk
    REQUIRE(w.save());
    CHECK_FALSE(d.isModified());
    REQUIRE(w.save());
    CHECK(project::read(fpath(zip)).snapshots.size() == 2);
    d.undoStack()->undo();
    CHECK(d.isModified());
    REQUIRE(w.save());
    CHECK(project::read(fpath(zip)).snapshots.size() == 1);
    d.undoStack()->redo();
    CHECK(d.isModified());
    CHECK(d.extras().snapshots.size() == 2);
}

TEST_CASE("the app refuses a newer major project and keeps a newer minor's files") {
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(QStringLiteral("p"));
    writeSampleDirectory(dir);
    const QString manifest = dir + QStringLiteral("/manifest.json");
    const std::string m = bwtest::readFile(fpath(manifest));
    const auto at = m.find("\"1.3\"");
    REQUIRE(at != std::string::npos);
    const auto withVersion = [&](const char* v) { return std::string(m).replace(at, 5, v); };

    MainWindow w;
    QString error;
    const QString ans = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    REQUIRE(w.openFile(ans, &error));
    writeBytes(manifest, withVersion("\"3.0\""));
    CHECK_FALSE(w.openFile(dir, &error));
    CHECK_FALSE(error.isEmpty());
    CHECK(w.document()->path() == ans);

    writeBytes(manifest, withVersion("\"1.7\""));
    REQUIRE(QDir().mkpath(dir + QStringLiteral("/future")));
    writeBytes(dir + QStringLiteral("/future/x.bin"), "abc");
    REQUIRE(w.openFile(dir, &error));
    drawAt(w, 0, 0, U'Q');
    REQUIRE(w.save());
    CHECK(bwtest::readFile(fpath(dir + QStringLiteral("/future/x.bin"))) == "abc");
}

TEST_CASE("a flat .ans is not written over a project") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    writeSampleZip(zip);
    const std::string before = bwtest::readFile(fpath(zip));
    const QString dir = tmp.filePath(QStringLiteral("piece"));
    writeSampleDirectory(dir);

    Document d;
    QString error;
    const AnsiOutput out = d.encode();
    CHECK_FALSE(d.save(zip, out, &error));
    CHECK(error.contains(QStringLiteral("project")));
    CHECK(bwtest::readFile(fpath(zip)) == before);
    CHECK_FALSE(d.save(dir, out, &error));
    CHECK(project::isProject(fpath(dir)));
    CHECK(d.path().isEmpty());

    // a zip named .ans is still a project
    const QString disguised = tmp.filePath(QStringLiteral("disguised.ans"));
    REQUIRE(QFile::copy(zip, disguised));
    CHECK_FALSE(d.save(disguised, out, &error));
    CHECK(bwtest::readFile(fpath(disguised)) == before);
}

TEST_CASE("a project that can't be opened leaves the open document alone") {
    QTemporaryDir tmp;
    MainWindow w;
    QString error;
    const QString tux = qpath(bwtest::dataDir() / "golden" / "tux.ans");
    REQUIRE(w.openFile(tux, &error));
    drawAt(w, 0, 0, U'Z');
    Document& d = *w.document();
    const Canvas canvas = d.canvas();
    const LayerStack layers = d.layers();

    const QString truncated = tmp.filePath(QStringLiteral("truncated.bwp"));
    writeSampleZip(truncated);
    const std::string whole = bwtest::readFile(fpath(truncated));
    writeBytes(truncated, whole.substr(0, whole.size() / 2));
    const QString text = tmp.filePath(QStringLiteral("text.bwp"));
    writeBytes(text, "not a zip\n");
    const QString magic = tmp.filePath(QStringLiteral("magic.ans"));
    writeBytes(magic, std::string("PK\x03\x04", 4) + "junk");
    const QString noManifest = tmp.filePath(QStringLiteral("loose"));
    REQUIRE(QDir().mkpath(noManifest));
    writeBytes(noManifest + QStringLiteral("/notes.md"), "notes\n");
    const QString empty = tmp.filePath(QStringLiteral("empty"));
    REQUIRE(QDir().mkpath(empty));

    for (const QString& bad : {truncated, text, magic, noManifest, empty, tmp.filePath(QStringLiteral("missing.bwp"))}) {
        CAPTURE(bad.toStdString());
        error.clear();
        CHECK_FALSE(w.openFile(bad, &error));
        CHECK_FALSE(error.isEmpty());
        CHECK(d.path() == tux);
        CHECK(d.form() == FileForm::Ansi);
        CHECK(d.canvas() == canvas);
        CHECK(d.layers() == layers);
        CHECK(d.isModified());
        CHECK(d.undoStack()->canUndo());
    }
}

TEST_CASE("a project too large to display is refused and the open document stays") {
    QTemporaryDir tmp;
    // as the flat-file case: 9 px cells over the renderer's limit
    AnsiDocument big;
    big.canvas = Canvas(kMaxColumns, kMaxRows);
    big.sauce = Sauce::fresh(kMaxColumns, kMaxRows);
    big.sauce->setLetterSpacing(2);
    REQUIRE(glyphWidth(big) == 9);
    const QString path = tmp.filePath(QStringLiteral("big.bwp"));
    project::writeZip(project::fromAnsi(big), fpath(path));

    MainWindow w;
    QString error;
    const QString tux = qpath(bwtest::dataDir() / "golden" / "tux.ans");
    REQUIRE(w.openFile(tux, &error));
    const Canvas before = w.document()->canvas();
    const QSize imageBefore = w.renderer().imageSize();
    CHECK_FALSE(w.openFile(path, &error));
    CHECK(error.contains(QStringLiteral("too large")));
    CHECK(w.document()->path() == tux);
    CHECK(w.document()->canvas() == before);
    CHECK(w.renderer().imageSize() == imageBefore);
}

TEST_CASE("opening a flat file after a project drops the project's extras") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    writeSampleZip(zip);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    REQUIRE_FALSE(w.document()->extras() == ProjectExtras{});
    REQUIRE(w.openFile(qpath(bwtest::dataDir() / "golden" / "tux.ans"), &error));
    CHECK(w.document()->form() == FileForm::Ansi);
    CHECK(w.document()->extras() == ProjectExtras{});
    CHECK(w.document()->layers().size() == 1);
    CHECK_FALSE(action(w, "snapshot")->isEnabled());

    REQUIRE(w.openFile(zip, &error));
    w.document()->clear();
    CHECK(w.document()->form() == FileForm::Ansi);
    CHECK(w.document()->extras() == ProjectExtras{});
    CHECK_FALSE(action(w, "snapshot")->isEnabled());
}

TEST_CASE("a snapshot is the next round, an undo step, and saved with the project") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    writeSampleZip(zip);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    drawAt(w, 2, 0, U'S');
    const AnsiOutput expected = writeAnsi(project::toAnsi(d.project()));

    REQUIRE(w.snapshot(&error) == 2);
    REQUIRE(d.extras().snapshots.size() == 2);
    CHECK(d.extras().snapshots[1].round == 2);
    CHECK(d.extras().snapshots[1].ans == expected.bytes);
    CHECK(d.undoStack()->undoText() == QStringLiteral("Snapshot r2"));
    d.undoStack()->undo();
    CHECK(d.extras().snapshots.size() == 1);
    d.undoStack()->redo();
    CHECK(d.extras().snapshots.size() == 2);

    REQUIRE(w.save());
    const project::Project saved = project::read(fpath(zip));
    REQUIRE(saved.snapshots.size() == 2);
    CHECK(saved.snapshots[1] == d.extras().snapshots[1]);
    // the snapshot alone marks the project modified
    REQUIRE(w.snapshot(&error) == 3);
    CHECK(w.isWindowModified());
    d.undoStack()->undo();
    CHECK_FALSE(w.isWindowModified());
}

TEST_CASE("a lossy snapshot is refused unless allowed, and a flat file has none") {
    QTemporaryDir tmp;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    writeSampleZip(zip);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(zip, &error));
    Document& d = *w.document();
    drawAt(w, 0, 0, U'★');
    const auto count = d.undoStack()->count();
    CHECK(w.snapshot(&error) == 0);
    CHECK(error.contains(QStringLiteral("'?'")));
    CHECK(d.extras().snapshots.size() == 1);
    CHECK(d.undoStack()->count() == count);
    CHECK(w.snapshot(&error, true) == 2);
    CHECK(d.extras().snapshots.size() == 2);

    w.document()->clear();
    CHECK(w.snapshot(&error) == 0);
    CHECK(error.contains(QStringLiteral("project")));
    CHECK(d.extras().snapshots.empty());
}

TEST_CASE("an .ans that lost blink on open warns before it is saved as a project") {
    QTemporaryDir tmp;
    Sauce s = Sauce::fresh(80, 1);
    s.setIce(false);
    const std::string body = "\x1b[0;5;31;44mA\x1b[0;37;104mB";
    const QString ans = tmp.filePath(QStringLiteral("blink.ans"));
    writeBytes(ans, body + encodeSauce(s, std::uint32_t(body.size())));

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(ans, &error));
    REQUIRE(w.document()->lostBlinkOnOpen() == 1);
    const QString zip = tmp.filePath(QStringLiteral("blink.bwp"));
    CHECK_FALSE(w.saveFile(zip, &error));
    CHECK(error.contains(QStringLiteral("blink")));
    CHECK_FALSE(QFile::exists(zip));
    CHECK(saveWarning(w.document()->encode(), true).startsWith(QStringLiteral("Before saving the project:")));
    REQUIRE(w.saveFile(zip, &error, true));
    CHECK(w.document()->form() == FileForm::ProjectZip);
    CHECK(w.document()->lostBlinkOnOpen() == 0);
    CHECK(w.saveFile(zip, &error));
}

TEST_CASE("Save As offers .bwp, and adds the suffix of the filter chosen") {
    CHECK(fileFormFor(QStringLiteral("a.bwp")) == FileForm::ProjectZip);
    CHECK(fileFormFor(QStringLiteral("a.Bwp")) == FileForm::ProjectZip);
    CHECK(fileFormFor(QStringLiteral("a.ans")) == FileForm::Ansi);
    CHECK(fileFormFor(QStringLiteral("a")) == FileForm::Ansi);

    QTemporaryDir tmp;
    CHECK(resolveSavePath(tmp.filePath(QStringLiteral("piece")), QStringLiteral("bwp")).path ==
          tmp.filePath(QStringLiteral("piece.bwp")));
    writeBytes(tmp.filePath(QStringLiteral("piece.bwp")), "x");
    CHECK(resolveSavePath(tmp.filePath(QStringLiteral("piece")), QStringLiteral("bwp")).confirmOverwrite);

    // A QFileDialog writes Qt's own settings when it is destroyed. Only where
    // they are in the scratch directory may this test build one.
    const QString scratch = QFileInfo(bwtest::scratchSettingsFile()).absolutePath();
    if (!bwtest::qtSettingsInScratch(scratch))
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    QFileDialog dialog;
    configureSaveDialog(dialog, tmp.filePath(QStringLiteral("piece.bwp")), true);
    CHECK(dialog.defaultSuffix() == QStringLiteral("bwp"));
    CHECK(dialog.selectedNameFilter().contains(QStringLiteral("*.bwp")));
    const QStringList filters = dialog.nameFilters();
    REQUIRE(filters.size() == 5);
    emit dialog.filterSelected(filters[0]);
    CHECK(dialog.defaultSuffix() == QStringLiteral("ans"));
    emit dialog.filterSelected(filters[4]);  // All files keeps the last
    CHECK(dialog.defaultSuffix() == QStringLiteral("ans"));
    emit dialog.filterSelected(filters[1]);
    CHECK(dialog.defaultSuffix() == QStringLiteral("bwp"));
    emit dialog.filterSelected(filters[2]);
    CHECK(dialog.defaultSuffix() == QStringLiteral("xb"));
    emit dialog.filterSelected(filters[3]);
    CHECK(dialog.defaultSuffix() == QStringLiteral("bin"));

    // a flat file starts on its own format's filter
    QFileDialog xbin;
    configureSaveDialog(xbin, tmp.filePath(QStringLiteral("piece.xb")));
    CHECK(xbin.defaultSuffix() == QStringLiteral("xb"));
    CHECK(xbin.selectedNameFilter().contains(QStringLiteral("*.xb")));
    QFileDialog bin;
    configureSaveDialog(bin, tmp.filePath(QStringLiteral("PIECE.BIN")));
    CHECK(bin.defaultSuffix() == QStringLiteral("bin"));
    CHECK(bin.selectedNameFilter().contains(QStringLiteral("*.bin")));
}
