// Local font collection defaults, background publication and a large picker.
#include "document.h"
#include "face_picker.h"
#include "font_scanner.h"
#include "main_window.h"
#include "preferences.h"
#include "settings.h"
#include "text_options.h"
#include "tool_options.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QAction>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QTimer>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QScrollBar>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>

using namespace bw;

namespace {
void await(const std::function<bool()>& done, qint64 budgetMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < budgetMs) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    INFO("event-pumping wait: " << timer.elapsed() << " ms elapsed; deadline " << budgetMs << " ms");
    REQUIRE(done());
}
void writeFont(const QString& path, const std::string& bytes) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
}
struct ResetFolder {
    ~ResetFolder() { setFontsFolderSetting({}); }
};
struct ScratchHome {
    QByteArray old = qgetenv("HOME");
    bool had = qEnvironmentVariableIsSet("HOME");
    explicit ScratchHome(const QString& home) {
#ifdef _WIN32
        SKIP("Windows takes the home folder from the user's profile, not HOME");
#endif
        qputenv("HOME", QFile::encodeName(home));
    }
    ~ScratchHome() { if (had) qputenv("HOME", old); else qunsetenv("HOME"); }
};
int facesMade(const FontCatalogue& c) {
    int made = 0;
    for (const auto& [hash, file] : c.store.files())
        made += file->facesMade() ? int(file->names.size()) : 0;
    return made;
}
}

TEST_CASE("an unset fonts folder uses only an existing default and Clear stays explicit", "[fonts][collection]") {
    ResetFolder reset;
    QTemporaryDir home;
    REQUIRE(home.isValid());
    ScratchHome scratch(home.path());
    appSettings()->remove(QLatin1String(kFontsFolderKey));
    CHECK(fontsFolderSetting(home.path()).isEmpty());
    CHECK(fontsFolderSetting().isEmpty());
    const QString folder = home.filePath(QStringLiteral("tdfiglet/fonts"));
    REQUIRE(QDir().mkpath(folder));
    CHECK(fontsFolderSetting(home.path()) == folder);
    CHECK(fontsFolderSetting() == folder);
    CHECK(fontsFolderIsDefault());
    PreferencesDialog d(fontsFolderSetting());
    CHECK(d.fontsFolder() == folder);
    auto* label = d.findChild<QLabel*>(QStringLiteral("fontsDefault"));
    REQUIRE(label);
    CHECK_FALSE(label->isHidden());
    d.clearButton()->click();
    setFontsFolderSetting(d.fontsFolder());
    CHECK(appSettings()->contains(QLatin1String(kFontsFolderKey)));
    CHECK(fontsFolderSetting(home.path()).isEmpty());
    CHECK_FALSE(fontsFolderIsDefault());
    PreferencesDialog cleared(fontsFolderSetting());
    CHECK(cleared.fontsFolder().isEmpty());
    CHECK(cleared.findChild<QLabel*>(QStringLiteral("fontsDefault"))->isHidden());
}

TEST_CASE("a font scan runs on a worker and publishes on the GUI thread", "[fonts][collection]") {
    QTemporaryDir tmp;
    auto entered = std::make_shared<QSemaphore>();
    auto release = std::make_shared<QSemaphore>();
    auto worker = std::make_shared<std::atomic<bool>>(false);
    QThread* gui = QThread::currentThread();
    FontScanner scanner(nullptr, [=](const QString&, const FontScanner::Cancelled&) {
        *worker = QThread::currentThread() != gui;
        entered->release();
        release->tryAcquire(1, 3000);
        return std::make_shared<const FontCatalogue>();
    });
    bool publishedOnGui = false;
    QObject::connect(&scanner, &FontScanner::changed, [&] {
        if (!scanner.scanning() && scanner.catalogue())
            publishedOnGui = QThread::currentThread() == gui;
    });
    scanner.start(tmp.path());
    CHECK(scanner.scanning());
    await([&] { return entered->available() > 0; });
    CHECK(worker->load());
    CHECK_FALSE(scanner.catalogue());
    bool tick = false;
    QMetaObject::invokeMethod(&scanner, [&] { tick = true; }, Qt::QueuedConnection);
    QCoreApplication::processEvents();
    CHECK(tick);
    release->release();
    await([&] { return !scanner.scanning(); });
    CHECK(publishedOnGui);
}

TEST_CASE("a rescan supersedes an older scan even when the older worker finishes last", "[fonts][collection]") {
    auto entered = std::make_shared<QSemaphore>();
    auto release = std::make_shared<QSemaphore>();
    auto ended = std::make_shared<std::atomic<bool>>(false);
    FontScanner scanner(nullptr, [=](const QString& folder, const FontScanner::Cancelled&) {
        if (folder == QStringLiteral("old")) {
            entered->release();
            release->tryAcquire(1, 3000);
            *ended = true;
        }
        auto c = std::make_shared<FontCatalogue>();
        c->folder = folder.toStdString();
        return c;
    });
    scanner.start(QStringLiteral("old"));
    await([&] { return entered->available() > 0; });
    scanner.start(QStringLiteral("new"));
    await([&] { return !scanner.scanning(); });
    REQUIRE(scanner.catalogue());
    CHECK(scanner.catalogue()->folder == "new");
    release->release();
    await([&] { return ended->load(); });
    // Drain the old watcher's completion as well as the worker's return.
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    CHECK(scanner.catalogue()->folder == "new");
    scanner.start({});
    CHECK_FALSE(scanner.scanning());
    CHECK_FALSE(scanner.catalogue());
}

TEST_CASE("the startup picker shows scanning then lists every file variant", "[fonts][collection]") {
    ResetFolder reset;
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    auto bytes = bwtest::readFile(bwtest::dataDir() / "tdf/bw-all.tdf");
    bytes.replace(bytes.find("BW Outline"), 10, "Line FaceX");
    bytes.replace(bytes.find("BW Block"), 8, "MonoFace");
    bytes.replace(bytes.find("BW Colour"), 9, "Tone Face");
    writeFont(tmp.filePath(QStringLiteral("variants.tdf")), bytes);
    setFontsFolderSetting(tmp.path());
    MainWindow window;
    FacePicker* picker = window.toolOptions()->textOptions()->facePicker();
    CHECK(window.document()->fontsScanning());
    CHECK(picker->scanning());
    CHECK(picker->listed().size() == 3);
    await([&] { return !window.document()->fontsScanning(); });
    CHECK_FALSE(picker->scanning());
    CHECK(picker->listed().size() == 6);
    CHECK(picker->listed().contains(QStringLiteral("Line FaceX (variants.tdf)")));
    CHECK(picker->listed().contains(QStringLiteral("MonoFace (variants.tdf)")));
    CHECK(picker->listed().contains(QStringLiteral("Tone Face (variants.tdf)")));
    CHECK(facesMade(*window.document()->fontsFolder()) == 0);
    picker->searchField()->setText(QStringLiteral("VARIANTS.TDF"));
    CHECK(picker->listed().size() == 3);
    picker->searchField()->setText(QStringLiteral("colour"));
    CHECK(picker->listed() == QStringList{QStringLiteral("Tone Face (variants.tdf)")});
    picker->searchField()->clear();
    picker->typeFilter()->setCurrentIndex(picker->typeFilter()->findData(QStringLiteral("outline")));
    CHECK(picker->listed() == QStringList{QStringLiteral("Line FaceX (variants.tdf)")});
}

TEST_CASE("a 1500 file collection lists and filters without building faces or previews", "[fonts][collection]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const auto source = bwtest::readFile(bwtest::dataDir() / "tdf/bw-all.tdf");
    const auto at = source.find("BW Outline");
    REQUIRE(at != std::string::npos);
    for (int i = 0; i < 1500; ++i) {
        std::string bytes = source;
        const auto name = QStringLiteral("Font%1").arg(i, 4, 10, QLatin1Char('0')).toStdString();
        bytes.replace(at, name.size(), name);
        writeFont(tmp.filePath(QStringLiteral("synthetic-%1.tdf").arg(i)), bytes);
    }
    auto c = loadFontsFolder(tmp.path());
    REQUIRE(c);
    REQUIRE(c->fonts.size() == 4500);
    CHECK(c->rejected.empty());
    CHECK(facesMade(*c) == 0);
    FacePicker picker(c->entries());
    CHECK(picker.listed().size() == 4500);
    picker.setText(U"Abc é");
    CHECK(picker.previewsBuilt() == 0);
    CHECK(facesMade(*c) == 0);
    picker.searchField()->setText(QStringLiteral("synthetic-1499.tdf"));
    CHECK(picker.listed().size() == 3);
    picker.searchField()->setText(QStringLiteral("colour"));
    CHECK(picker.listed().size() == 1500);
    picker.searchField()->setText(QStringLiteral("outline"));
    CHECK(picker.listed().size() == 1500);
    picker.searchField()->clear();
    CHECK(picker.previewsBuilt() == 0);
    CHECK(facesMade(*c) == 0);
    picker.resize(400, 400);
    picker.show();
    QCoreApplication::processEvents();
    CHECK(picker.previewsBuilt() > 0);
    CHECK(picker.previewsBuilt() < 20);
    CHECK(facesMade(*c) < 20);
    picker.list()->scrollToBottom();
    QCoreApplication::processEvents();
    const int built = picker.previewsBuilt();
    CHECK(built < 40);
    picker.list()->viewport()->update();
    QCoreApplication::processEvents();
    CHECK(picker.previewsBuilt() == built);
    CHECK(facesMade(*c) < 40);
}

TEST_CASE("face list type-ahead and display queries build only painted faces", "[fonts][collection][round1]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const auto source = bwtest::readFile(bwtest::dataDir() / "tdf/bw-all.tdf");
    for (int i = 0; i < 300; ++i) {
        auto bytes = source;
        const auto name = QStringLiteral("Font%1").arg(i, 4, 10, QLatin1Char('0')).toStdString();
        bytes.replace(bytes.find("BW Outline"), name.size(), name);
        writeFont(tmp.filePath(QStringLiteral("synthetic-%1.tdf").arg(i)), bytes);
    }
    auto c = loadFontsFolder(tmp.path());
    REQUIRE(c->fonts.size() == 900);
    FacePicker picker(c->entries());
    picker.setText(U"Abc é");
    picker.resize(400, 400);
    picker.show();
    picker.list()->setFocus();
    QCoreApplication::processEvents();
    const int painted = facesMade(*c);
    REQUIRE(painted > 0);
    REQUIRE(painted < 20);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    QApplication::sendEvent(picker.list(), &key);
    QCoreApplication::processEvents();
    CHECK(facesMade(*c) == painted);
    for (int row = 0; row < picker.list()->model()->rowCount(); ++row) {
        const auto index = picker.list()->model()->index(row, 0);
        CHECK_FALSE(index.data(Qt::DisplayRole).toString().isEmpty());
    }
    CHECK(facesMade(*c) == painted);
}

TEST_CASE("Preferences OK preserves an untouched implicit fonts folder", "[fonts][collection][round1]") {
    ResetFolder reset;
    QTemporaryDir home;
    REQUIRE(home.isValid());
    ScratchHome scratch(home.path());
    appSettings()->remove(QLatin1String(kFontsFolderKey));
    const QString folder = home.filePath(QStringLiteral("tdfiglet/fonts"));
    bool clear = false;
    SECTION("a collection created later is discovered") {}
    SECTION("the existing default stays implicit") { REQUIRE(QDir().mkpath(folder)); }
    SECTION("Clear with no collection is an explicit choice") { clear = true; }
    MainWindow window;
    auto* action = window.findChild<QAction*>(QStringLiteral("preferences"));
    REQUIRE(action);
    bool shown = false;
    QTimer::singleShot(0, &window, [&] {
        auto* d = qobject_cast<PreferencesDialog*>(QApplication::activeModalWidget());
        if (!d)
            return;
        shown = true;
        if (clear)
            d->clearButton()->click();
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    action->trigger();
    REQUIRE(shown);
    CHECK(appSettings()->contains(QLatin1String(kFontsFolderKey)) == clear);
    REQUIRE(QDir().mkpath(folder));
    if (clear) {
        CHECK(fontsFolderSetting().isEmpty());
        CHECK_FALSE(fontsFolderIsDefault());
    } else {
        CHECK(fontsFolderSetting() == folder);
        CHECK(fontsFolderIsDefault());
    }
    await([&] { return !window.document()->fontsScanning(); });
}

TEST_CASE("a background scan of 3501 synthetic faces meets the picker budget", "[fonts][collection][budget]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    // These are our synthetic outline/block/colour fonts, not collection data.
    const auto source = bwtest::readFile(bwtest::dataDir() / "tdf/bw-all.tdf");
    const auto at = source.find("BW Outline");
    REQUIRE(at != std::string::npos);
    constexpr int files = 1167, faces = files * 3;
    static_assert(faces >= 3500);
    for (int i = 0; i < files; ++i) {
        auto bytes = source;
        const auto name = QStringLiteral("Font%1").arg(i, 6, 10, QLatin1Char('0')).toStdString();
        bytes.replace(at, name.size(), name); // Distinct hashes keep every file in the store.
        writeFont(tmp.filePath(QStringLiteral("synthetic-%1.tdf").arg(i)), bytes);
    }

    // Disk IO and publication get a hang deadline, not a throughput budget:
    // concurrent scans may take seconds without blocking the GUI. The picker
    // open budget below is the user-facing latency required by #177(a).
    FontScanner scanner;
    QElapsedTimer timer;
    timer.start();
    scanner.start(tmp.path());
    CHECK(scanner.scanning());
    await([&] { return !scanner.scanning(); }, 30000);
    const qint64 scanMs = timer.elapsed();
    INFO("3501-face background scan and publication: " << scanMs << " ms");
    const auto c = scanner.catalogue();
    REQUIRE(c);
    REQUIRE(c->fonts.size() == faces);
    CHECK(c->store.files().size() == files);
    CHECK(c->rejected.empty());
    CHECK(facesMade(*c) == 0);

    // Opening includes entries, list layout and the first visible previews.
    timer.restart();
    FacePicker picker(c->entries());
    picker.setText(U"Abc é");
    picker.resize(400, 400);
    picker.show();
    QCoreApplication::processEvents();
    const qint64 openMs = timer.elapsed();
    INFO("3501-face picker open and first paint: " << openMs << " ms");
    CHECK(openMs < 300);
    CHECK(picker.listed().size() == faces);
    CHECK(picker.previewsBuilt() > 0);
    CHECK(picker.previewsBuilt() < 20);
    CHECK(facesMade(*c) < 20);

    timer.restart();
    picker.searchField()->setText(QStringLiteral("SYNTHETIC-1166.TDF"));
    CHECK(picker.listed().size() == 3);
    picker.searchField()->setText(QStringLiteral("Font001166"));
    CHECK(picker.listed() == QStringList{QStringLiteral("Font001166 (synthetic-1166.tdf)")});
    picker.searchField()->setText(QStringLiteral("colour"));
    CHECK(picker.listed().size() == files);
    picker.searchField()->clear();
    picker.typeFilter()->setCurrentIndex(picker.typeFilter()->findData(QStringLiteral("outline")));
    CHECK(picker.listed().size() == files);
    picker.searchField()->setText(QStringLiteral("no such font"));
    CHECK(picker.listed().isEmpty());
    picker.searchField()->clear();
    picker.typeFilter()->setCurrentIndex(0);
    CHECK(picker.listed().size() == faces);
    const qint64 filterMs = timer.elapsed();
    INFO("3501-face filter sequence: " << filterMs << " ms");
    // Normally 2–5 ms, but a concurrent full suite reached 58 ms. Allow
    // scheduling headroom while catching a user-visible 100 ms filter delay.
    CHECK(filterMs < 100);
    CHECK(picker.previewsBuilt() < 20);
}
