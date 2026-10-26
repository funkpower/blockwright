// TheDraw fonts in the app (PLAN §15, #18): the fonts folder kept in the
// settings and set in Edit > Preferences, a folder font copied into the
// project when a text layer takes it, and the CLI reading a project's copies.

#include "document.h"
#include "main_window.h"
#include "preferences.h"

#include "blockwright/ansi.h"
#include "blockwright/font_store.h"
#include "blockwright/project.h"
#include "blockwright/text.h"

#include "scratch_settings.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <QUndoStack>

#include <filesystem>

using namespace bw;
namespace fs = std::filesystem;

namespace {

void awaitFonts(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    REQUIRE(done());
}

fs::path fpath(const QString& p) { return fs::path(QFile::encodeName(p).toStdString()); }

std::string tdfData(const char* name) { return bwtest::readFile(bwtest::dataDir() / "tdf" / name); }

void writeBytes(const QString& path, const std::string& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
}

// A fonts folder in `tmp`: bw-all.tdf, bw-block.tdf in a subfolder and a
// broken.tdf.
QString fontsFolder(const QTemporaryDir& tmp) {
    const QString dir = tmp.filePath(QStringLiteral("fonts"));
    writeBytes(dir + QStringLiteral("/bw-all.tdf"), tdfData("bw-all.tdf"));
    writeBytes(dir + QStringLiteral("/more/bw-block.tdf"), tdfData("bw-block.tdf"));
    writeBytes(dir + QStringLiteral("/broken.tdf"), "not a font");
    return dir;
}

// The folder's font named `name`.
const FontCatalogue::Font& fontNamed(const FontCatalogue& c, const std::string& name) {
    for (const auto& f : c.fonts)
        if (f.ref.name == name)
            return f;
    FAIL("no font " << name);
    return c.fonts.front();
}

struct SettingCleared {
    ~SettingCleared() { setFontsFolderSetting({}); }
};

struct CliRun {
    int status = -1;
    QString err;
};

CliRun runCli(const QStringList& args) {
    QProcess p;
    p.start(QStringLiteral(BW_CLI), args);
    REQUIRE(p.waitForFinished(60000));
    REQUIRE(p.exitStatus() == QProcess::NormalExit);
    return {p.exitCode(), QString::fromUtf8(p.readAllStandardError() + p.readAllStandardOutput())};
}

} // namespace

TEST_CASE("the fonts folder setting keeps an explicitly cleared value", "[fonts][preferences]") {
    SettingCleared cleared;
    setFontsFolderSetting({});
    CHECK(fontsFolderSetting().isEmpty());
    CHECK(appSettings()->contains(QLatin1String(kFontsFolderKey)));
    CHECK_FALSE(loadFontsFolder({}));

    setFontsFolderSetting(QStringLiteral("/some/fonts"));
    CHECK(fontsFolderSetting() == QStringLiteral("/some/fonts"));
    CHECK(appSettings()->value(QLatin1String(kFontsFolderKey)).toString() == QStringLiteral("/some/fonts"));
    setFontsFolderSetting({});
    CHECK(appSettings()->contains(QLatin1String(kFontsFolderKey)));
}

TEST_CASE("Preferences lists the fonts folder's fonts and the files it couldn't read", "[fonts][preferences]") {
    QTemporaryDir tmp;
    const QString dir = fontsFolder(tmp);
    PreferencesDialog d(dir);
    awaitFonts([&] { return !d.scanning(); });
    REQUIRE(d.catalogue());
    CHECK(d.catalogue()->fonts.size() == 4);
    CHECK(d.fontsSummary()->text() == QStringLiteral("4 TheDraw fonts; 1 file not read"));
    const QString list = d.fontsList()->toPlainText();
    CHECK(list.contains(QStringLiteral("BW Colour\tbw-all.tdf")));
    CHECK(list.contains(QStringLiteral("BW Block\tmore/bw-block.tdf")));
    CHECK(list.contains(QStringLiteral("Not read: broken.tdf: ")));

    d.clearButton()->click();
    CHECK(d.fontsFolder().isEmpty());
    CHECK_FALSE(d.catalogue());
    CHECK(d.fontsList()->toPlainText().isEmpty());

    // a folder typed and OK pressed without leaving the field is scanned
    d.fontsFolderEdit()->setText(dir + QStringLiteral("/more"));
    d.buttons()->button(QDialogButtonBox::Ok)->click();
    CHECK(d.result() == QDialog::Accepted);
    awaitFonts([&] { return !d.scanning(); });
    REQUIRE(d.catalogue());
    CHECK(d.catalogue()->fonts.size() == 1);
    CHECK(d.fontsSummary()->text() == QStringLiteral("1 TheDraw font"));
}

TEST_CASE("Edit > Preferences sets the fonts folder the document and the next window use", "[fonts][preferences]") {
    SettingCleared cleared;
    setFontsFolderSetting({});
    QTemporaryDir tmp;
    const QString dir = fontsFolder(tmp);
    MainWindow w;
    CHECK_FALSE(w.document()->fontsFolder());
    auto* a = w.findChild<QAction*>(QStringLiteral("preferences"));
    REQUIRE(a);

    bool shown = false;
    QTimer::singleShot(0, &w, [&] {
        auto* d = qobject_cast<PreferencesDialog*>(QApplication::activeModalWidget());
        if (!d)
            return;
        shown = true;
        d->fontsFolderEdit()->setText(dir);
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    a->trigger();
    REQUIRE(shown);
    CHECK(fontsFolderSetting() == dir);
    awaitFonts([&] { return !w.document()->fontsScanning(); });
    REQUIRE(w.document()->fontsFolder());
    CHECK(w.document()->fontsFolder()->fonts.size() == 4);

    MainWindow next;
    awaitFonts([&] { return !next.document()->fontsScanning(); });
    REQUIRE(next.document()->fontsFolder());
    CHECK(next.document()->fontsFolder()->fonts.size() == 4);
}

TEST_CASE("Preferences' OK scans the folder again even when it is unchanged", "[fonts][preferences]") {
    // Codex round 1 (#168): a font added while the dialog was open wasn't
    // in the catalogue OK handed on.
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(QStringLiteral("fonts"));
    REQUIRE(QDir().mkpath(dir));
    PreferencesDialog d(dir);
    awaitFonts([&] { return !d.scanning(); });
    REQUIRE(d.catalogue());
    CHECK(d.catalogue()->fonts.empty());
    writeBytes(dir + QStringLiteral("/bw-block.tdf"), tdfData("bw-block.tdf"));
    d.buttons()->button(QDialogButtonBox::Ok)->click();
    CHECK(d.result() == QDialog::Accepted);
    awaitFonts([&] { return !d.scanning(); });
    REQUIRE(d.catalogue());
    REQUIRE(d.catalogue()->fonts.size() == 1);
    CHECK(d.catalogue()->fonts[0].ref.name == "BW Block");
    CHECK(d.fontsSummary()->text() == QStringLiteral("1 TheDraw font"));
}

TEST_CASE("a font copy no layer uses is no unsaved content: undone, a flat save holds it all", "[fonts][project]") {
    // Codex round 1 (#168): the copy kept for redo made flatHoldsAll false
    // after the step that took it was undone.
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(QStringLiteral("fonts"));
    writeBytes(dir + QStringLiteral("/bw-block.tdf"), tdfData("bw-block.tdf"));
    Document doc;
    doc.setFontsFolder(loadFontsFolder(dir));
    const FontCatalogue::Font block = fontNamed(*doc.fontsFolder(), "BW Block");
    REQUIRE(doc.flatHoldsAll());

    const LayerId id = doc.active()->props.id;
    TextSource s;
    s.text = U"Funk";
    s.face = block.ref;
    LayerProperties p = doc.active()->props;
    p.kind = LayerKind::Text;
    p.text = s;
    REQUIRE(doc.setLayerProperties(id, p, QStringLiteral("Text")));
    CHECK_FALSE(doc.flatHoldsAll());
    const Layer drawn = *doc.active();

    doc.undoStack()->undo();
    CHECK(doc.active()->props.kind == LayerKind::Cells);
    CHECK(doc.flatHoldsAll());
    QString error;
    const QString ans = tmp.filePath(QStringLiteral("piece.ans"));
    REQUIRE(doc.save(ans, doc.encode(), &error));
    CHECK_FALSE(doc.isModified());
    CHECK(doc.path() == ans);

    // redone with the folder gone, the copy kept for it draws the layer
    doc.setFontsFolder(nullptr);
    doc.undoStack()->redo();
    CHECK(*doc.active() == drawn);
    CHECK(doc.active()->cells == renderText(s, *block.resolvedFace()).cells);
    CHECK(doc.isModified());
    CHECK_FALSE(doc.flatHoldsAll());
}

TEST_CASE("a folder font a text layer takes is copied into the project and saved with it", "[fonts][project]") {
    QTemporaryDir tmp;
    const QString dir = fontsFolder(tmp);
    MainWindow w;
    Document& doc = *w.document();
    doc.setFontsFolder(loadFontsFolder(dir));
    // a copy: the catalogue goes when the folder is cleared
    const FontCatalogue::Font colour = fontNamed(*doc.fontsFolder(), "BW Colour");

    const LayerId id = doc.addLayer(LayerKind::Text);
    TextSource s;
    s.text = U"Funk";
    s.face = colour.ref;
    LayerProperties p = doc.active()->props;
    p.text = s;
    CHECK(doc.extras().fonts.empty());
    REQUIRE(doc.setLayerProperties(id, p, QStringLiteral("Text")));
    CHECK(doc.active()->cells == renderText(s, *colour.resolvedFace()).cells);
    REQUIRE(doc.extras().fonts.file(colour.ref.file));
    CHECK(doc.extras().fonts.files().size() == 1);

    // undone and redone with the folder gone: the copy draws it
    doc.setFontsFolder(nullptr);
    const Layer drawn = *doc.active();
    doc.undoStack()->undo();
    CHECK_FALSE(doc.active()->props.text);
    doc.undoStack()->redo();
    CHECK(*doc.active() == drawn);

    // a font neither the project nor the folder has is refused
    TextSource missing = s;
    missing.face.name = "BW Nothing";
    LayerProperties bad = drawn.props;
    bad.text = missing;
    CHECK_FALSE(doc.setLayerProperties(id, bad, QStringLiteral("Text")));

    // saved and opened with no fonts folder, the text is edited with the copy
    QString error;
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    REQUIRE(w.saveFile(zip, &error, true));
    REQUIRE(QDir(dir).removeRecursively());
    MainWindow w2;
    REQUIRE(w2.openFile(zip, &error));
    Document& d2 = *w2.document();
    CHECK(d2.fontsFolder() == nullptr);
    REQUIRE(d2.extras().fonts.file(colour.ref.file));
    const Layer* opened = d2.layers().find(id);
    REQUIRE(opened);
    CHECK(opened->cells == drawn.cells);
    TextSource next = *opened->props.text;
    next.text = U"Power";
    LayerProperties np = opened->props;
    np.text = next;
    REQUIRE(d2.setLayerProperties(id, np, QStringLiteral("Text")));
    CHECK(d2.layers().find(id)->cells == renderText(next, *d2.extras().fonts.face(colour.ref)).cells);
}

TEST_CASE("the CLI reads a TheDraw text layer from the project's copy of its font", "[fonts][cli]") {
    QTemporaryDir tmp;
    const FontFilePtr all = loadFontFile(tdfData("bw-all.tdf"));
    project::Project p;
    p.layers = LayerStack(80, 25);
    p.fonts.add(all);
    TextSource s;
    s.text = U"Funk";
    s.face.kind = FaceRef::Kind::Tdf;
    s.face.file = all->sha256;
    s.face.index = 2;
    s.face.name = "BW Colour";
    p.layers.add(makeTextLayer(s, "Title", 1, 1, {&p.fonts, nullptr}));
    const QString zip = tmp.filePath(QStringLiteral("piece.bwp"));
    project::writeZip(p, fpath(zip));

    // convert: the composite of the saved cells
    const QString ans = tmp.filePath(QStringLiteral("piece.ans"));
    CliRun r = runCli({QStringLiteral("convert"), zip, ans});
    INFO(r.err.toStdString());
    REQUIRE(r.status == 0);
    CHECK(bwtest::readFile(fpath(ans)) == writeAnsi(project::toAnsi(p)).bytes);

    // export --irc reads it too
    r = runCli({QStringLiteral("export"), QStringLiteral("--irc"), QStringLiteral("--allow-over-budget"), zip,
                tmp.filePath(QStringLiteral("piece.txt"))});
    CHECK(r.status == 0);

    // a command that writes the project back keeps the font's copy
    r = runCli({QStringLiteral("snapshot"), QStringLiteral("--allow-lossy"), zip});
    REQUIRE(r.status == 0);
    const project::Project back = project::read(fpath(zip));
    CHECK(back.layers == p.layers);
    CHECK(back.fonts == p.fonts);

    // a tampered copy is refused, nothing written
    const QString bad = tmp.filePath(QStringLiteral("bad"));
    project::writeDirectory(p, fpath(bad));
    const QString copy = bad + QStringLiteral("/fonts/") + QString::fromStdString(all->sha256) + QStringLiteral(".tdf");
    std::string bytes = all->bytes;
    bytes[200] ^= 1;
    writeBytes(copy, bytes);
    r = runCli({QStringLiteral("convert"), bad, tmp.filePath(QStringLiteral("bad.ans"))});
    CHECK(r.status == 1);
    CHECK(r.err.contains(QStringLiteral("hash to")));
    CHECK_FALSE(QFile::exists(tmp.filePath(QStringLiteral("bad.ans"))));
}
