// The sample projects in the app (#207, PLAN §12): Help > Samples and the
// start screen's Samples list the bundled ones and open an untitled copy.

#include "util.h"

#include "blockwright/composite.h"
#include "blockwright/project.h"
#include "blockwright/samples.h"

#include "document.h"
#include "main_window.h"
#include "recent_files.h"
#include "settings.h"
#include "start_screen.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace bw;
namespace fs = std::filesystem;

namespace {

const fs::path kSource = BW_SAMPLES_SOURCE;
const QString kShowcase = QStringLiteral("01-layers-showcase");
const QString kKing = QStringLiteral("02-ddos-king");
const QString kStillLife = QStringLiteral("03-still-life");
const QString kRidges = QStringLiteral("04-generator-ridges");
const QString kCharacter = QStringLiteral("05-character");
const QString kBrandLogo = QStringLiteral("06-blockwright-logo");

QString qpath(const fs::path& p) { return QFile::decodeName(QByteArray::fromStdString(p.string())); }

// BLOCKWRIGHT_SAMPLES for as long as this lives.
struct SamplesAt {
    explicit SamplesAt(const QString& dir) { qputenv("BLOCKWRIGHT_SAMPLES", QFile::encodeName(dir)); }
    ~SamplesAt() { qunsetenv("BLOCKWRIGHT_SAMPLES"); }
};

void clearRecent() { appSettings()->remove(QStringLiteral("recent")); }

// A samples folder in `tmp`: the showcase as `names`, and a broken one.
QString scratchSamples(const QTemporaryDir& tmp, const QStringList& names, bool broken) {
    const fs::path dir = fs::path(QFile::encodeName(tmp.filePath(QStringLiteral("samples"))).toStdString());
    fs::create_directories(dir);
    for (const QString& n : names)
        fs::copy(kSource / kShowcase.toStdString(), dir / n.toStdString(), fs::copy_options::recursive);
    if (broken) {
        fs::create_directories(dir / "03-broken");
        QFile f(qpath(dir / "03-broken" / "manifest.json"));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("{ not json");
    }
    return qpath(dir);
}

// Replaces the notes of the sample `name` in the samples folder `dir`.
void writeNotes(const QString& dir, const QString& name, const QByteArray& notes) {
    QFile f(dir + QLatin1Char('/') + name + QStringLiteral("/notes.md"));
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(notes);
}

// Notes whose Variants table has one column: once read past its rows' end.
const QByteArray kShortTable = "# Broken notes\n\nA sample whose variants table has one column.\n\n"
                               "## Variants\n| Variant |\n|---|\n| Day |\n";

// Every file under `dir`, by its path there: its bytes and when it was
// last written.
std::map<std::string, std::pair<std::string, fs::file_time_type>> tree(const fs::path& dir) {
    std::map<std::string, std::pair<std::string, fs::file_time_type>> out;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) {
            std::ifstream f(e.path(), std::ios::binary);
            std::ostringstream bytes;
            bytes << f.rdbuf();
            out[fs::relative(e.path(), dir).generic_string()] = {bytes.str(), e.last_write_time()};
        }
    return out;
}

QList<QAction*> sampleActions(MainWindow& w) {
    QMenu* menu = w.samplesMenu();
    REQUIRE(menu);
    QMetaObject::invokeMethod(menu, "aboutToShow");
    return menu->actions();
}

// Answers the next message box with `button`; `asked` says one came.
void answer(QWidget* context, QMessageBox::StandardButton button, bool* asked) {
    *asked = false;
    QTimer::singleShot(0, context, [=] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box)
            return;
        *asked = true;
        box->button(button)->click();
    });
}

} // namespace

TEST_CASE("Help > Samples lists the bundled samples by title, a broken one disabled", "[samples]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    REQUIRE(w.samplesMenu()->objectName() == QStringLiteral("samplesMenu"));
    // under Help
    bool underHelp = false;
    for (QAction* a : w.menuBar()->actions())
        if (a->menu() && a->menu()->actions().contains(w.samplesMenu()->menuAction()))
            underHelp = a->text() == QStringLiteral("&Help");
    CHECK(underHelp);

    SECTION("the source samples") {
        SamplesAt env(qpath(kSource));
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 6);
        CHECK(actions[0]->text() == QStringLiteral("Harbour at night"));
        CHECK(actions[0]->isEnabled());
        CHECK(actions[0]->toolTip().startsWith(QStringLiteral("A city on the water")));
        CHECK(actions[0]->property("path").toString() == qpath(kSource / kShowcase.toStdString()));
        CHECK(actions[1]->text() == QStringLiteral("DDOS KING"));
        CHECK(actions[1]->isEnabled());
        CHECK(actions[1]->toolTip().startsWith(QStringLiteral("A king on a throne of server racks")));
        CHECK(actions[1]->property("path").toString() == qpath(kSource / kKing.toStdString()));
        CHECK(actions[2]->text() == QStringLiteral("Still life"));
        CHECK(actions[2]->isEnabled());
        CHECK(actions[2]->toolTip().startsWith(QStringLiteral("A cylinder, a box and a ball on a table")));
        CHECK(actions[2]->property("path").toString() == qpath(kSource / kStillLife.toStdString()));
        CHECK(actions[3]->text() == QStringLiteral("Ridges at dusk"));
        CHECK(actions[3]->isEnabled());
        CHECK(actions[3]->toolTip().startsWith(QStringLiteral("Mountain ridges and stars made by two generator layers")));
        CHECK(actions[3]->property("path").toString() == qpath(kSource / kRidges.toStdString()));
        CHECK(actions[4]->text() == QStringLiteral("Traveller"));
        CHECK(actions[4]->isEnabled());
        CHECK(actions[4]->toolTip().startsWith(QStringLiteral("A traveller with a staff and a pack")));
        CHECK(actions[4]->property("path").toString() == qpath(kSource / kCharacter.toStdString()));
        CHECK(actions[5]->text() == QStringLiteral("Blockwright logo"));
        CHECK(actions[5]->isEnabled());
        CHECK(actions[5]->toolTip().startsWith(QStringLiteral("The program's own logo taken apart")));
        CHECK(actions[5]->property("path").toString() == qpath(kSource / kBrandLogo.toStdString()));
    }
    SECTION("in file-name order, a broken one disabled with its problem") {
        SamplesAt env(scratchSamples(tmp, {QStringLiteral("02-b"), QStringLiteral("01-a")}, true));
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 3);
        CHECK(actions[0]->property("path").toString().endsWith(QStringLiteral("/01-a")));
        CHECK(actions[1]->property("path").toString().endsWith(QStringLiteral("/02-b")));
        CHECK(actions[2]->text() == QStringLiteral("03-broken"));
        CHECK_FALSE(actions[2]->isEnabled());
        CHECK(actions[2]->toolTip().startsWith(QStringLiteral("Can't be read: ")));
    }
    SECTION("an '&' in a title is shown, not taken as a mnemonic") {
        const QString dir = scratchSamples(tmp, {QStringLiteral("01-a")}, false);
        const QString notes = dir + QStringLiteral("/01-a/notes.md");
        QFile f(notes);
        REQUIRE(f.open(QIODevice::ReadOnly));
        QByteArray text = f.readAll();
        f.close();
        text.replace("# Harbour at night", "# Sea & sky");
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
        f.close();
        SamplesAt env(dir);
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 1);
        CHECK(actions[0]->text() == QStringLiteral("Sea && sky"));
    }
    SECTION("one whose notes' table is too narrow to read is disabled with its problem") {
        const QString dir = scratchSamples(tmp, {QStringLiteral("01-a")}, false);
        writeNotes(dir, QStringLiteral("01-a"), kShortTable);
        SamplesAt env(dir);
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 1);
        CHECK(actions[0]->text() == QStringLiteral("01-a"));
        CHECK_FALSE(actions[0]->isEnabled());
        CHECK(actions[0]->toolTip() == QStringLiteral("Can't be read: the Variants table has 1 column, not 2"));
    }
    SECTION("none installed") {
        SamplesAt env(tmp.filePath(QStringLiteral("nowhere")));
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 1);
        CHECK_FALSE(actions[0]->isEnabled());
    }
}

TEST_CASE("a sample opens as an untitled, unmodified copy, off the recent list", "[samples]") {
    clearRecent();
    SamplesAt env(qpath(kSource));
    MainWindow w;
    const QString path = qpath(kSource / kShowcase.toStdString());
    const project::Project saved = project::read(kSource / kShowcase.toStdString());

    SECTION("openSampleFile") {
        QString error;
        REQUIRE(w.openSampleFile(path, &error));
        const Document& doc = *w.document();
        CHECK(doc.path().isEmpty());
        CHECK_FALSE(doc.isModified());
        CHECK(doc.form() == FileForm::ProjectZip);
        CHECK(doc.layers().size() == saved.layers.size());
        CHECK(doc.ansi().canvas == composite(saved.layers, {saved.ice, Cell{}}));
        REQUIRE(doc.ansi().sauce);
        CHECK(doc.ansi().sauce->author == "funkpower");
        CHECK(recentFiles().isEmpty());
        // fog.py doesn't run until it is trusted
        CHECK_FALSE(w.generatorsTrusted());
    }
    SECTION("from Help > Samples") {
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 6);
        actions[0]->trigger();
        CHECK(w.document()->layers().size() == saved.layers.size());
        CHECK(w.document()->path().isEmpty());
        CHECK(recentFiles().isEmpty());
    }
    SECTION("DDOS KING, its layers, its lock and its hidden variant layers intact") {
        const project::Project king = project::read(kSource / kKing.toStdString());
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 6);
        actions[1]->trigger();
        const Document& doc = *w.document();
        CHECK(doc.path().isEmpty());
        CHECK_FALSE(doc.isModified());
        CHECK(doc.ansi().canvas == composite(king.layers, {king.ice, Cell{}}));
        REQUIRE(doc.layers().size() == king.layers.size());
        CHECK(doc.layers() == king.layers);
    }
    SECTION("the still life, its locks intact") {
        const project::Project life = project::read(kSource / kStillLife.toStdString());
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 6);
        actions[2]->trigger();
        const Document& doc = *w.document();
        CHECK(doc.path().isEmpty());
        CHECK_FALSE(doc.isModified());
        CHECK(doc.ansi().canvas == composite(life.layers, {life.ice, Cell{}}));
        REQUIRE(doc.layers().size() == life.layers.size());
        CHECK(doc.layers() == life.layers);
    }
    SECTION("the generator piece, its generators and their files intact, not run") {
        const project::Project ridges = project::read(kSource / kRidges.toStdString());
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 6);
        actions[3]->trigger();
        const Document& doc = *w.document();
        CHECK(doc.path().isEmpty());
        CHECK_FALSE(doc.isModified());
        CHECK(doc.ansi().canvas == composite(ridges.layers, {ridges.ice, Cell{}}));
        CHECK(doc.layers() == ridges.layers);
        CHECK_FALSE(w.generatorsTrusted());
    }
    SECTION("the character, its figure's layers in place and the merged copy hidden") {
        const project::Project traveller = project::read(kSource / kCharacter.toStdString());
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 6);
        actions[4]->trigger();
        const Document& doc = *w.document();
        CHECK(doc.path().isEmpty());
        CHECK_FALSE(doc.isModified());
        CHECK(doc.ansi().canvas == composite(traveller.layers, {traveller.ice, Cell{}}));
        CHECK(doc.layers() == traveller.layers);
    }
    SECTION("the logo, its mark and wordmark layers in place and the letters alpha-locked") {
        const project::Project brand = project::read(kSource / kBrandLogo.toStdString());
        const QList<QAction*> actions = sampleActions(w);
        REQUIRE(actions.size() == 6);
        actions[5]->trigger();
        const Document& doc = *w.document();
        CHECK(doc.path().isEmpty());
        CHECK_FALSE(doc.isModified());
        CHECK(doc.ansi().canvas == composite(brand.layers, {brand.ice, Cell{}}));
        CHECK(doc.layers() == brand.layers);
    }
    SECTION("a save goes where it is asked to, the whole sample, and the sample is left as it was") {
        // a copy of the sample, so a write to it would show
        QTemporaryDir samplesTmp;
        REQUIRE(samplesTmp.isValid());
        const QString dir = scratchSamples(samplesTmp, {kShowcase}, false);
        const fs::path copy = fs::path(QFile::encodeName(dir).toStdString()) / kShowcase.toStdString();
        const auto before = tree(copy);
        QString error;
        REQUIRE(w.openSampleFile(qpath(copy), &error));
        QTemporaryDir tmp;
        REQUIRE(tmp.isValid());
        const QString mine = tmp.filePath(QStringLiteral("mine.bwp"));
        REQUIRE(w.saveFile(mine, &error));
        CHECK(w.document()->path() == mine);
        // layers, cells, scripts, refs, snapshots, notes, SAUCE and all
        CHECK(project::read(fs::path(QFile::encodeName(mine).toStdString())) == saved);
        // not a byte of the sample written, nor any file touched
        CHECK(tree(copy) == before);
    }
    SECTION("one that can't be read leaves the document as it was") {
        QString error;
        CHECK_FALSE(w.openSampleFile(qpath(kSource / "no-such-sample"), &error));
        CHECK_FALSE(error.isEmpty());
        CHECK(w.document()->layers().size() == 1);
    }
    SECTION("unsaved edits are asked about first; Cancel keeps them") {
        w.document()->markUnsaved();
        bool asked = false;
        answer(&w, QMessageBox::Cancel, &asked);
        w.openSample(path);
        CHECK(asked);
        CHECK(w.document()->isModified());
        CHECK(w.document()->layers().size() == 1);
    }
}

TEST_CASE("the start screen lists the samples and opens one", "[samples][start-screen]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    SECTION("none installed: no Samples section") {
        SamplesAt env(tmp.filePath(QStringLiteral("nowhere")));
        StartScreen s(&w);
        CHECK_FALSE(s.findChild<QWidget*>(QStringLiteral("startSamples")));
    }
    SECTION("each sample a button; a broken one disabled") {
        SamplesAt env(scratchSamples(tmp, {QStringLiteral("01-a")}, true));
        StartScreen s(&w);
        const QList<QPushButton*> entries = s.findChildren<QPushButton*>(QStringLiteral("startSampleEntry"));
        REQUIRE(entries.size() == 2);
        CHECK(entries[0]->text() == QStringLiteral("Harbour at night"));
        CHECK(entries[0]->isEnabled());
        CHECK(entries[0]->toolTip().startsWith(QStringLiteral("A city on the water")));
        CHECK_FALSE(entries[1]->isEnabled());
    }
    SECTION("one whose notes' table is too narrow to read: the screen still comes up, the sample disabled") {
        const QString dir = scratchSamples(tmp, {QStringLiteral("01-a"), QStringLiteral("02-b")}, false);
        writeNotes(dir, QStringLiteral("01-a"), kShortTable);
        SamplesAt env(dir);
        auto* s = new StartScreen(&w);
        s->setAttribute(Qt::WA_DeleteOnClose);
        s->open();
        QCoreApplication::processEvents();
        CHECK(s->isVisible());
        const QList<QPushButton*> entries = s->findChildren<QPushButton*>(QStringLiteral("startSampleEntry"));
        REQUIRE(entries.size() == 2);
        CHECK(entries[0]->text() == QStringLiteral("01-a"));
        CHECK_FALSE(entries[0]->isEnabled());
        CHECK(entries[0]->toolTip() == QStringLiteral("Can't be read: the Variants table has 1 column, not 2"));
        CHECK(entries[1]->text() == QStringLiteral("Harbour at night"));
        CHECK(entries[1]->isEnabled());
        s->reject();
    }
    SECTION("an '&' in a title is shown, not taken as a mnemonic") {
        const QString dir = scratchSamples(tmp, {QStringLiteral("01-a")}, false);
        writeNotes(dir, QStringLiteral("01-a"), "# Sea & sky\n\nThe sea and the sky.\n");
        SamplesAt env(dir);
        StartScreen s(&w);
        QPushButton* entry = s.findChild<QPushButton*>(QStringLiteral("startSampleEntry"));
        REQUIRE(entry);
        CHECK(entry->text() == QStringLiteral("Sea && sky"));
        CHECK(QKeySequence::mnemonic(entry->text()).isEmpty());
        CHECK(entry->toolTip() == QStringLiteral("The sea and the sky."));
    }
    SECTION("a click opens the sample and closes the screen") {
        SamplesAt env(qpath(kSource));
        auto* s = new StartScreen(&w);
        s->setAttribute(Qt::WA_DeleteOnClose);
        s->open();
        QPointer<StartScreen> guard(s);
        const QList<QPushButton*> entries = s->findChildren<QPushButton*>(QStringLiteral("startSampleEntry"));
        REQUIRE(entries.size() == 6);
        CHECK(entries[1]->text() == QStringLiteral("DDOS KING"));
        CHECK(entries[2]->text() == QStringLiteral("Still life"));
        CHECK(entries[3]->text() == QStringLiteral("Ridges at dusk"));
        CHECK(entries[4]->text() == QStringLiteral("Traveller"));
        CHECK(entries[5]->text() == QStringLiteral("Blockwright logo"));
        CHECK(entries[1]->toolTip().startsWith(QStringLiteral("A king on a throne of server racks")));
        QPushButton* entry = entries[0];
        entry->click();
        QCoreApplication::processEvents();
        CHECK((!guard || !guard->isVisible()));
        CHECK(w.document()->layers().size() == project::read(kSource / kShowcase.toStdString()).layers.size());
        CHECK(w.document()->path().isEmpty());
        CHECK_FALSE(w.document()->isModified());
    }
}

TEST_CASE("the start screen with many samples, a long title and a full recent list fits a 1280x720 screen",
          "[samples][start-screen]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    for (int i = 0; i < kRecentLimit; ++i)
        addRecentFile(tmp.filePath(QStringLiteral("a-long-name-for-a-recent-piece-%1.ans").arg(i)));
    const int count = GENERATE(4, 12, 20);
    QStringList names;
    for (int i = 1; i <= count; ++i)
        names << QStringLiteral("%1-s").arg(i, 2, 10, QLatin1Char('0'));
    const QString dir = scratchSamples(tmp, names, false);
    // one title far wider than any screen, an '&' in the part that shows
    const QString longTitle = (QStringLiteral("Sea & sky: ") + QStringLiteral("a harbour at night ").repeated(12)).trimmed();
    writeNotes(dir, names[1], "# " + longTitle.toUtf8() + "\n\nThe long one.\n");
    SamplesAt env(dir);
    MainWindow w;
    StartScreen::Options o;
    o.available = QSize(1280, 720);
    StartScreen s(&w, o);
    INFO(count << " samples");
    const QList<QPushButton*> entries = s.findChildren<QPushButton*>(QStringLiteral("startSampleEntry"));
    REQUIRE(entries.size() == count);
    CHECK(s.logoScale() == 1);
    CHECK(s.sizeHint().height() <= 660);
    CHECK(s.minimumSizeHint().height() <= 660);
    CHECK(s.sizeHint().width() <= o.available.width());
    CHECK(s.minimumSizeHint().width() <= o.available.width());

    // the long title cut short, the whole of it in the tooltip
    QPushButton* longEntry = entries[1];
    CHECK(longEntry->text().endsWith(QChar(0x2026)));
    CHECK(longEntry->text().startsWith(QStringLiteral("Sea && sky: a harbour")));
    QString shown = longEntry->text();
    CHECK(longEntry->fontMetrics().horizontalAdvance(shown.replace(QStringLiteral("&&"), QStringLiteral("&"))) <= 260);
    CHECK(QKeySequence::mnemonic(longEntry->text()).isEmpty());
    CHECK(longEntry->toolTip() == longTitle + QStringLiteral("\nThe long one."));
    // the others whole, their summary the tooltip
    CHECK(entries[0]->text() == QStringLiteral("Harbour at night"));
    CHECK(entries[0]->toolTip().startsWith(QStringLiteral("A city on the water")));

    // shown, at its size and squeezed to its smallest, the column's scrollbar
    // (when it has one) covers no entry, so the cut title's ellipsis is in view
    auto* column = s.findChild<QScrollArea*>(QStringLiteral("startSamples"));
    REQUIRE(column);
    s.show();
    for (const QSize size : {s.sizeHint(), s.minimumSizeHint()}) {
        s.resize(size);
        QCoreApplication::processEvents();
        INFO("screen " << size.width() << "x" << size.height() << ", viewport " << column->viewport()->width()
                       << ", list " << column->widget()->width());
        CHECK(column->verticalScrollBar()->isVisible() == (count > 4));
        CHECK(column->widget()->width() <= column->viewport()->width());
        for (QPushButton* entry : entries) {
            CHECK(entry->width() >= entry->sizeHint().width());
            CHECK(entry->mapTo(column->viewport(), QPoint(entry->width(), 0)).x() <= column->viewport()->width());
        }
    }
}
