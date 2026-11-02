// The start screen (#206, PLAN §11) and the recent list behind it and File >
// Open Recent: when it shows, the logo and its fallback, New, Open, Recent,
// Esc and "Show at startup".

#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/render.h"
#include "blockwright/sauce.h"
#include "blockwright/version.h"

#include "canvas_view.h"
#include "document.h"
#include "file_open.h"
#include "main_window.h"
#include "recent_files.h"
#include "scratch_settings.h"
#include "settings.h"
#include "start_screen.h"
#include "branding.h"
#include "system_input.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileOpenEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>

#include <utility>

using namespace bw;

namespace {

void writeBytes(const QString& path, const std::string& bytes) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
}

QString tuxAt(const QString& path) {
    writeBytes(path, bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"));
    return path;
}

// A logo `w` x `h` cells of coloured full blocks, as an .ans with SAUCE.
AnsiDocument logoDocument(int w, int h) {
    AnsiDocument d;
    d.canvas = Canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Cell& c = d.canvas.at(x, y);
            c.ch = U'█';
            c.fg = Color::indexed(std::uint8_t(1 + (x + y) % 15));
        }
    d.sauce = Sauce::fresh(w, h);
    return d;
}

QString logoAt(const QString& path, int w, int h) {
    writeBytes(path, writeAnsi(logoDocument(w, h)).bytes);
    return path;
}

// A project directory at `path`, saved by a window of its own.
QString projectDirectoryAt(const QString& path) {
    REQUIRE(QDir().mkpath(path));
    MainWindow other;
    QString error;
    REQUIRE(other.saveFile(path, &error));
    REQUIRE(other.document()->form() == FileForm::ProjectDirectory);
    return path;
}

QString clean(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

StartScreen* startScreenOf(MainWindow& w) { return w.findChild<StartScreen*>(); }

// A start screen that closed: hidden, or deleted already (it deletes itself
// when it closes).
bool closed(const QPointer<StartScreen>& s) { return !s || !s->isVisible(); }

// The list as written to the settings, not as recentFiles() reads it back.
QStringList stored() { return appSettings()->value(QStringLiteral("recent/files")).toStringList(); }

// Sections run the test case again from the top, in the same settings.
void clearRecent() { appSettings()->remove(QStringLiteral("recent")); }

template <class T>
T* child(QWidget* w, const char* name) {
    T* c = w->findChild<T*>(QString::fromLatin1(name));
    REQUIRE(c);
    return c;
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

// Answers Qt's own file dialog, when one opens, with `path`, or cancels it
// when `path` is empty.
struct FileAnswer {
    QString path;
    bool picked = false;
    QTimer timer;
    explicit FileAnswer(QString p) : path(std::move(p)) {
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!dialog || picked)
                return;
            picked = true;
            if (path.isEmpty()) {
                QMetaObject::invokeMethod(dialog, "reject");
                return;
            }
            auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
            REQUIRE(name);
            name->setText(path);
            QMetaObject::invokeMethod(dialog, "accept");
        });
        timer.start();
    }
};

// Qt's own file dialogs for as long as this lives, which a test can answer.
struct QtDialogs {
    QtDialogs() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true); }
    ~QtDialogs() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false); }
};

bool fileDialogsAllowed() {
    // A QFileDialog writes Qt's own settings when it is destroyed.
    return bwtest::qtSettingsInScratch(QFileInfo(bwtest::scratchSettingsFile()).absolutePath());
}

} // namespace

TEST_CASE("the recent list keeps the last ten, newest first, each once", "[start-screen][recent]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    CHECK(recentFiles().isEmpty());
    QStringList paths;
    for (int i = 0; i < 12; ++i)
        paths << tmp.filePath(QStringLiteral("f%1.ans").arg(i));
    addRecentFile(paths[0]);
    addRecentFile(paths[1]);
    CHECK(recentFiles() == QStringList{paths[1], paths[0]});

    SECTION("an entry added again moves to the front, once") {
        addRecentFile(paths[0]);
        CHECK(recentFiles() == QStringList{paths[0], paths[1]});
        CHECK(stored() == QStringList{paths[0], paths[1]});
        // the same file by another spelling is the same entry
        addRecentFile(tmp.path() + QStringLiteral("/./sub/../f1.ans"));
        CHECK(recentFiles() == QStringList{paths[1], paths[0]});
    }
    SECTION("the eleventh drops the oldest") {
        for (int i = 2; i < 12; ++i)
            addRecentFile(paths[i]);
        const QStringList list = recentFiles();
        REQUIRE(list.size() == kRecentLimit);
        CHECK(list.front() == paths[11]);
        CHECK(list.back() == paths[2]);
        CHECK_FALSE(list.contains(paths[1]));
        CHECK(stored() == list);
    }
    SECTION("remove takes one out") {
        removeRecentFile(paths[1]);
        CHECK(recentFiles() == QStringList{paths[0]});
    }
    SECTION("it is kept as recent/files in the settings") {
        CHECK(stored() == QStringList{paths[1], paths[0]});
    }
    SECTION("a hand-edited list is read de-duplicated and cut at ten") {
        QStringList many;
        for (int i = 0; i < 12; ++i)
            many << paths[i] << paths[i];
        appSettings()->setValue(QStringLiteral("recent/files"), many);
        const QStringList list = recentFiles();
        CHECK(list == paths.mid(0, kRecentLimit));
    }
}

TEST_CASE("opening and saving put the file first in the recent list", "[start-screen][recent]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    QString error;
    REQUIRE(w.openFile(tux, &error));
    CHECK(recentFiles() == QStringList{clean(tux)});

    // a failed open adds nothing
    CHECK_FALSE(w.openFile(tmp.filePath(QStringLiteral("nothing.ans")), &error));
    CHECK(recentFiles() == QStringList{clean(tux)});

    const QString copy = tmp.filePath(QStringLiteral("copy.ans"));
    REQUIRE(w.saveFile(copy, &error));
    CHECK(recentFiles() == QStringList{clean(copy), clean(tux)});

    const QString project = tmp.filePath(QStringLiteral("piece.bwp"));
    REQUIRE(w.saveFile(project, &error));
    CHECK(recentFiles() == QStringList{clean(project), clean(copy), clean(tux)});

    // saving the open file again brings it back to the front
    REQUIRE(w.openFile(tux, &error));
    REQUIRE(w.openFile(project, &error));
    CHECK(recentFiles() == QStringList{clean(project), clean(tux), clean(copy)});
    REQUIRE(w.saveFile(copy, &error));
    CHECK(recentFiles().front() == clean(copy));
}

TEST_CASE("File > Open Recent lists the recent files and opens one", "[start-screen][recent]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    const QString other = tuxAt(tmp.filePath(QStringLiteral("other & more.ans")));
    const QString gone = tmp.filePath(QStringLiteral("gone.ans"));
    addRecentFile(gone);
    addRecentFile(tux);
    addRecentFile(other);
    MainWindow w;
    QMenu* menu = w.openRecentMenu();
    REQUIRE(menu);
    CHECK(menu->objectName() == QStringLiteral("openRecent"));
    QAction* file = w.findChild<QAction*>(QStringLiteral("open"));
    REQUIRE(file);
    emit menu->aboutToShow();
    const QList<QAction*> entries = menu->actions();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0]->text() == QDir::toNativeSeparators(clean(other)).replace(QStringLiteral("&"), QStringLiteral("&&")));
    CHECK(entries[1]->text() == QDir::toNativeSeparators(clean(tux)));
    CHECK(entries[2]->text() == QDir::toNativeSeparators(clean(gone)));
    CHECK(entries[0]->isEnabled());
    CHECK(entries[1]->isEnabled());
    CHECK_FALSE(entries[2]->isEnabled());

    entries[1]->trigger();
    CHECK(w.document()->path() == clean(tux));
    // and it went to the front, which the menu shows the next time it opens
    emit menu->aboutToShow();
    REQUIRE(menu->actions().size() == 3);
    CHECK(menu->actions()[0]->text() == QDir::toNativeSeparators(clean(tux)));

    SECTION("asking first about unsaved edits") {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QApplication::sendEvent(w.canvasView(), &press);
        REQUIRE(w.document()->isModified());
        bool asked = false;
        answer(&w, QMessageBox::Cancel, &asked);
        menu->actions()[1]->trigger();
        CHECK(asked);
        CHECK(w.document()->path() == clean(tux));
    }
    SECTION("an empty list says so") {
        appSettings()->remove(QStringLiteral("recent"));
        emit menu->aboutToShow();
        REQUIRE(menu->actions().size() == 1);
        CHECK_FALSE(menu->actions()[0]->isEnabled());
    }
}

TEST_CASE("the start screen shows on a launch with no file and not with one", "[start-screen]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    w.show();
    SECTION("no file: the screen, over the window") {
        QPointer<StartScreen> s = startUp(&w, QString());
        REQUIRE(s);
        CHECK(s->isVisible());
        CHECK(s->parentWidget() == &w);
        CHECK(s->isModal());
        CHECK(startScreenOf(w) == s);
    }
    SECTION("a file: it opens, and no screen") {
        const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
        CHECK_FALSE(startUp(&w, tux));
        CHECK_FALSE(startScreenOf(w));
        CHECK(w.document()->path() == tux);
    }
    SECTION("a project directory: it opens, and no screen") {
        const QString dir = projectDirectoryAt(tmp.filePath(QStringLiteral("piece")));
        CHECK_FALSE(startUp(&w, dir));
        CHECK_FALSE(startScreenOf(w));
        CHECK(w.document()->path() == clean(dir));
        CHECK(w.document()->form() == FileForm::ProjectDirectory);
    }
}

TEST_CASE("at launch the start screen comes up before the window, which shows when it closes", "[start-screen]") {
    setStartScreenAtStartup(true);
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    SECTION("no file: the screen alone, a window of its own") {
        MainWindow w;
        QPointer<StartScreen> s = startUp(&w, QString());
        REQUIRE(s);
        CHECK(s->isVisible());
        CHECK(s->parentWidget() == nullptr);
        CHECK(s->isModal());
        CHECK_FALSE(w.isVisible());
        SECTION("New: the window shows with the canvas") {
            child<QPushButton>(s, "startNew")->click();
            QApplication::processEvents();
            CHECK(closed(s));
            CHECK(w.isVisible());
        }
        SECTION("Esc: the window shows with the default canvas") {
            s->reject();
            QApplication::processEvents();
            CHECK(closed(s));
            CHECK(w.isVisible());
            CHECK(w.document()->canvas().width() == kStartNewWidth);
        }
        SECTION("a file the system hands the app: the window shows with it") {
            const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
            w.openRequested(tux);
            QApplication::processEvents();
            CHECK(closed(s));
            CHECK(w.isVisible());
            CHECK(w.document()->path() == tux);
        }
    }
    SECTION("the window goes while the screen is up: the screen goes with it") {
        QPointer<StartScreen> s;
        {
            MainWindow w;
            s = startUp(&w, QString());
            REQUIRE(s);
        }
        CHECK_FALSE(s);
    }
    SECTION("a file: the window shows with it, and no screen") {
        const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
        MainWindow w;
        CHECK_FALSE(startUp(&w, tux));
        CHECK(w.isVisible());
        CHECK(w.document()->path() == tux);
    }
    SECTION("Show at startup off: the window shows, and no screen") {
        setStartScreenAtStartup(false);
        MainWindow w;
        CHECK_FALSE(startUp(&w, QString()));
        CHECK(w.isVisible());
        setStartScreenAtStartup(true);
    }
}

TEST_CASE("at launch the start screen comes up on the window's screen and fits it", "[start-screen][start-screen-screens]") {
    // Run with two screens of different heights (see tests/CMakeLists.txt).
    const auto screens = QGuiApplication::screens();
    if (screens.size() < 2)
        SKIP("one screen");
    QScreen* big = screens[0];
    QScreen* small = screens[0];
    for (QScreen* sc : screens) {
        if (sc->availableGeometry().height() > big->availableGeometry().height())
            big = sc;
        if (sc->availableGeometry().height() < small->availableGeometry().height())
            small = sc;
    }
    REQUIRE(big != small);
    setStartScreenAtStartup(true);
    for (const auto& [on, pointer] : {std::pair{big, small}, std::pair{small, big}}) {
        INFO("window on " << on->name().toStdString() << ", pointer on " << pointer->name().toStdString());
        MainWindow w;
        w.move(on->availableGeometry().topLeft() + QPoint(10, 10)); // where its saved geometry put it
        QCursor::setPos(pointer->geometry().center());
        QPointer<StartScreen> s = startUp(&w, QString());
        REQUIRE(s);
        REQUIRE(bwtest::showExposed(s));
        CHECK(QGuiApplication::screenAt(s->frameGeometry().center()) == on);
        CHECK(s->frameGeometry().height() <= on->availableGeometry().height() - 60);
        CHECK(on->availableGeometry().contains(s->frameGeometry()));
        s->reject();
        QApplication::processEvents();
    }
}

TEST_CASE("at launch a failed open says so over the start screen", "[start-screen][start-screen-screens]") {
    setStartScreenAtStartup(true);
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    QWidget* parent = nullptr;
    QRect box;
    QTimer::singleShot(0, s, [&] {
        auto* b = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!b)
            return;
        parent = b->parentWidget();
        box = b->frameGeometry();
        b->button(QMessageBox::Ok)->click();
    });
    w.openRequested(tmp.filePath(QStringLiteral("gone.ans")));
    QApplication::processEvents();
    REQUIRE(box.isValid());
    CHECK(parent == s.data());
    CHECK(s->frameGeometry().contains(box.center()));
    CHECK_FALSE(closed(s));
    CHECK_FALSE(w.isVisible());
}

TEST_CASE("at launch a file the system hands the app while the failed-open box is up opens it", "[start-screen]") {
    setStartScreenAtStartup(true);
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    MainWindow w;
    FileOpenForwarder forwarder(&w);
    QApplication::instance()->installEventFilter(&forwarder);
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    QPointer<QMessageBox> box;
    QTimer::singleShot(0, s, [&] {
        box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box)
            return;
        QFileOpenEvent open(tux); // from the Finder, while the box is up
        QApplication::sendEvent(QApplication::instance(), &open);
        // Dismissed if it is still up; gone with it if not.
        QMessageBox* b = box.data();
        QTimer::singleShot(50, b, [b] { b->button(QMessageBox::Ok)->click(); });
    });
    w.openRequested(tmp.filePath(QStringLiteral("gone.ans")));
    QApplication::processEvents();
    QApplication::instance()->removeEventFilter(&forwarder);
    CHECK(closed(s));
    CHECK_FALSE(box);
    CHECK(w.document()->path() == tux);
    CHECK(w.isVisible());
}

TEST_CASE("at launch the start screen's top-left stays on the work area", "[start-screen][start-screen-short]") {
    // Also run on a screen shorter than the start screen (see tests/CMakeLists.txt).
    setStartScreenAtStartup(true);
    MainWindow w;
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    const QRect area = w.screen()->availableGeometry();
    INFO("frame " << s->frameGeometry().x() << "," << s->frameGeometry().y() << " " << s->frameGeometry().width()
                  << "x" << s->frameGeometry().height() << " on a work area " << area.width() << "x" << area.height());
    CHECK(s->frameGeometry().top() >= area.top());
    CHECK(s->frameGeometry().left() >= area.left());
}

TEST_CASE("at launch File > Open's dialog is the start screen's", "[start-screen]") {
    if (!fileDialogsAllowed())
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    QtDialogs qtDialogs;
    setStartScreenAtStartup(true);
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    MainWindow w;
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    QWidget* parent = nullptr;
    QTimer peek;
    peek.setInterval(5);
    QObject::connect(&peek, &QTimer::timeout, [&parent] {
        if (auto* d = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
            parent = d->parentWidget();
    });
    peek.start();
    FileAnswer pick(tux);
    child<QPushButton>(s, "startOpen")->click();
    CHECK(pick.picked);
    CHECK(parent == s.data());
    CHECK(closed(s));
    CHECK(w.isVisible());
    CHECK(w.document()->path() == tux);
}

TEST_CASE("Show at startup is kept in the settings and respected", "[start-screen]") {
    setStartScreenAtStartup(true);
    MainWindow w;
    w.show();
    CHECK(startScreenAtStartup());
    {
        StartScreen s(&w);
        auto* show = child<QCheckBox>(&s, "startShowAtStartup");
        CHECK(show->isChecked());
        show->click();
        CHECK_FALSE(startScreenAtStartup());
        CHECK(appSettings()->value(QStringLiteral("startScreen/showAtStartup")).toBool() == false);
    }
    CHECK_FALSE(startUp(&w, QString()));
    CHECK_FALSE(startScreenOf(w));
    // and File > Start Screen... still brings it back, with the box clear
    w.findChild<QAction*>(QStringLiteral("startScreen"))->trigger();
    StartScreen* s = startScreenOf(w);
    REQUIRE(s);
    auto* show = child<QCheckBox>(s, "startShowAtStartup");
    CHECK_FALSE(show->isChecked());
    show->click();
    CHECK(startScreenAtStartup());
    s->reject();
    QApplication::processEvents();
    CHECK(startUp(&w, QString()));
}

TEST_CASE("New on the start screen makes a canvas of the size and iCE setting chosen", "[start-screen]") {
    MainWindow w;
    w.show();
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    auto* width = child<QSpinBox>(s, "startWidth");
    auto* height = child<QSpinBox>(s, "startHeight");
    auto* ice = child<QCheckBox>(s, "startIce");
    CHECK(width->value() == 80);
    CHECK(height->value() == 40);
    CHECK(ice->isChecked());

    SECTION("a size typed and iCE off, by the button") {
        width->setValue(132);
        height->setValue(61);
        ice->setChecked(false);
        child<QPushButton>(s, "startNew")->click();
        CHECK(w.document()->canvas().width() == 132);
        CHECK(w.document()->canvas().height() == 61);
        CHECK_FALSE(w.document()->ansi().ice);
        CHECK(w.renderer().imageSize() == QSize(132 * 8, 61 * 16));
        CHECK_FALSE(w.document()->isModified());
        CHECK(w.document()->path().isEmpty());
        CHECK(closed(s));
    }
    SECTION("a preset, iCE on, by N") {
        child<QPushButton>(s, "startPreset80x25")->click();
        CHECK(width->value() == 80);
        CHECK(height->value() == 25);
        child<QPushButton>(s, "startNew")->setFocus();
        bwtest::systemKey(s, Qt::Key_N, Qt::NoModifier, QStringLiteral("n"));
        CHECK(w.document()->canvas().width() == 80);
        CHECK(w.document()->canvas().height() == 25);
        CHECK(w.document()->ansi().ice);
        CHECK(closed(s));
    }
    SECTION("the 80 x 50 preset, by Enter from the height field") {
        child<QPushButton>(s, "startPreset80x50")->click();
        height->setFocus();
        bwtest::systemKey(s, Qt::Key_Return);
        CHECK(w.document()->canvas().width() == 80);
        CHECK(w.document()->canvas().height() == 50);
        CHECK(closed(s));
    }
    SECTION("the 80 x 40 preset puts back the default") {
        child<QPushButton>(s, "startPreset80x25")->click();
        child<QPushButton>(s, "startPreset80x40")->click();
        CHECK(width->value() == 80);
        CHECK(height->value() == 40);
    }
}

TEST_CASE("MainWindow::newCanvas refuses a size it can't make, changing nothing", "[start-screen]") {
    MainWindow w;
    QString error;
    REQUIRE(w.newCanvas(QSize(40, 12), false, &error));
    for (const QSize bad : {QSize(0, 10), QSize(10, 0), QSize(kMaxColumns + 1, 10), QSize(10, kMaxRows + 1)}) {
        error.clear();
        CHECK_FALSE(w.newCanvas(bad, true, &error));
        CHECK_FALSE(error.isEmpty());
        CHECK(w.document()->canvas().width() == 40);
        CHECK(w.document()->canvas().height() == 12);
        CHECK_FALSE(w.document()->ansi().ice);
    }
    REQUIRE(w.newCanvas(QSize(kMaxColumns, kMaxRows), true, &error));
    CHECK(w.document()->canvas().width() == kMaxColumns);
}

TEST_CASE("Open on the start screen goes through File > Open", "[start-screen]") {
    if (!fileDialogsAllowed())
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    QtDialogs qtDialogs;
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    MainWindow w;
    w.show();
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    // File > Open's own dialog, which titles itself "Open"
    QString title;
    QTimer peek;
    peek.setInterval(5);
    QObject::connect(&peek, &QTimer::timeout, [&title] {
        if (auto* d = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
            title = d->windowTitle();
    });
    peek.start();

    SECTION("by the button") {
        FileAnswer pick(tux);
        child<QPushButton>(s, "startOpen")->click();
        CHECK(pick.picked);
        CHECK(title == QStringLiteral("Open"));
        CHECK(w.document()->path() == tux);
        CHECK(closed(s));
    }
    SECTION("by O") {
        FileAnswer pick(tux);
        bwtest::systemKey(s, Qt::Key_O, Qt::NoModifier, QStringLiteral("o"));
        CHECK(pick.picked);
        CHECK(w.document()->path() == tux);
        CHECK(closed(s));
    }
    SECTION("by Ctrl+O") {
        FileAnswer pick(tux);
        bwtest::systemKey(s, Qt::Key_O, Qt::ControlModifier);
        CHECK(pick.picked);
        CHECK(w.document()->path() == tux);
        CHECK(closed(s));
    }
    SECTION("cancelled, the screen stays") {
        FileAnswer pick{QString()};
        child<QPushButton>(s, "startOpen")->click();
        CHECK(pick.picked);
        CHECK(w.document()->path().isEmpty());
        CHECK(s->isVisible());
    }
    SECTION("Open Project Directory, through File > Open Project Directory") {
        const QString dir = projectDirectoryAt(tmp.filePath(QStringLiteral("piece")));
        FileAnswer pick(dir);
        child<QPushButton>(s, "startOpenDirectory")->click();
        CHECK(pick.picked);
        CHECK(title == QStringLiteral("Open Project Directory"));
        CHECK(w.document()->path() == clean(dir));
        CHECK(w.document()->form() == FileForm::ProjectDirectory);
        CHECK(closed(s));
    }
}

TEST_CASE("the start screen's recent list opens a file and removes a missing one", "[start-screen][recent]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    const QString gone = tmp.filePath(QStringLiteral("gone.ans"));
    addRecentFile(gone);
    addRecentFile(tux);
    MainWindow w;
    w.show();
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    QApplication::processEvents();
    QList<QPushButton*> entries = s->findChildren<QPushButton*>(QStringLiteral("startRecentEntry"));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0]->property("path").toString() == clean(tux));
    CHECK(entries[0]->text() == QStringLiteral("tux.ans"));
    CHECK(entries[0]->isEnabled());
    CHECK(entries[1]->property("path").toString() == clean(gone));
    CHECK_FALSE(entries[1]->isEnabled());
    // only the missing one has a Remove button
    const QList<QPushButton*> removes = s->findChildren<QPushButton*>(QStringLiteral("startRecentRemove"));
    REQUIRE(removes.size() == 1);
    CHECK(removes[0]->parentWidget() == entries[1]->parentWidget());

    SECTION("Remove takes the missing one off the list") {
        removes[0]->click();
        QApplication::processEvents();
        CHECK(recentFiles() == QStringList{clean(tux)});
        CHECK(s->findChildren<QPushButton*>(QStringLiteral("startRecentEntry")).size() == 1);
        CHECK(s->findChildren<QPushButton*>(QStringLiteral("startRecentRemove")).isEmpty());
        CHECK(s->isVisible());
    }
    SECTION("clicking one opens it and closes the screen") {
        entries[0]->click();
        CHECK(w.document()->path() == clean(tux));
        CHECK(closed(s));
    }
    SECTION("one that went missing after the list was drawn says so and leaves the screen up") {
        REQUIRE(QFile::remove(tux));
        bool asked = false;
        answer(s, QMessageBox::Ok, &asked);
        entries[0]->click();
        CHECK(asked);
        CHECK(w.document()->path().isEmpty());
        CHECK(s->isVisible());
    }
}

// On macOS a document double-clicked in the Finder comes after launch as a
// QFileOpenEvent, with no file on the command line, so the screen is already
// up when it opens.
TEST_CASE("a file the system hands the app while the start screen is up closes it, and Esc keeps the file",
          "[start-screen]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const bool directory = GENERATE(false, true);
    const QString path = directory ? clean(projectDirectoryAt(tmp.filePath(QStringLiteral("piece"))))
                                   : tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    MainWindow w;
    QApplication::instance()->installEventFilter(new FileOpenForwarder(&w));
    w.show();
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    QFileOpenEvent open(path);
    CHECK(QApplication::sendEvent(QApplication::instance(), &open));
    REQUIRE(w.document()->path() == path);
    CHECK(closed(s));
    // Esc, had the screen stayed up, would have cleared the file away.
    if (s && s->isVisible())
        bwtest::systemKey(s, Qt::Key_Escape);
    CHECK(w.document()->path() == path);
    CHECK_FALSE(w.document()->isModified());
    QApplication::processEvents();
    CHECK_FALSE(startScreenOf(w));
}

TEST_CASE("Enter on a focused button on the start screen runs that button, not New", "[start-screen]") {
    if (!fileDialogsAllowed())
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    QtDialogs qtDialogs;
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString tux = tuxAt(tmp.filePath(QStringLiteral("tux.ans")));
    const QString gone = tmp.filePath(QStringLiteral("gone.ans"));
    addRecentFile(gone);
    addRecentFile(tux);
    MainWindow w;
    w.show();
    QPointer<StartScreen> s = startUp(&w, QString());
    REQUIRE(s);
    REQUIRE(bwtest::showExposed(s));
    // A size New would make, and no file opened tux's size, if New ran.
    child<QSpinBox>(s, "startWidth")->setValue(33);
    const auto newRan = [&w] { return w.document()->canvas().width() == 33; };

    SECTION("Open...") {
        FileAnswer pick(tux);
        child<QPushButton>(s, "startOpen")->setFocus();
        bwtest::systemKey(s, Qt::Key_Return);
        CHECK(pick.picked);
        CHECK_FALSE(newRan());
        CHECK(w.document()->path() == tux);
        CHECK(closed(s));
    }
    SECTION("Open Project Directory...") {
        const QString dir = projectDirectoryAt(tmp.filePath(QStringLiteral("piece")));
        FileAnswer pick(dir);
        child<QPushButton>(s, "startOpenDirectory")->setFocus();
        bwtest::systemKey(s, Qt::Key_Return);
        CHECK(pick.picked);
        CHECK_FALSE(newRan());
        CHECK(w.document()->path() == clean(dir));
        CHECK(w.document()->form() == FileForm::ProjectDirectory);
        CHECK(closed(s));
    }
    SECTION("a recent entry") {
        QList<QPushButton*> entries = s->findChildren<QPushButton*>(QStringLiteral("startRecentEntry"));
        REQUIRE(entries.size() == 2);
        REQUIRE(entries[0]->property("path").toString() == clean(tux));
        entries[0]->setFocus();
        bwtest::systemKey(s, Qt::Key_Return);
        CHECK_FALSE(newRan());
        CHECK(w.document()->path() == clean(tux));
        CHECK(closed(s));
    }
    SECTION("Remove, by Enter or the keypad's Enter") {
        const int key = GENERATE(int(Qt::Key_Return), int(Qt::Key_Enter));
        child<QPushButton>(s, "startRecentRemove")->setFocus();
        bwtest::systemKey(s, key, key == Qt::Key_Enter ? Qt::KeypadModifier : Qt::NoModifier);
        QApplication::processEvents();
        CHECK_FALSE(newRan());
        CHECK(recentFiles() == QStringList{clean(tux)});
        CHECK(s->isVisible());
    }
    SECTION("New, focused, still makes the canvas") {
        child<QPushButton>(s, "startNew")->setFocus();
        bwtest::systemKey(s, Qt::Key_Return);
        CHECK(newRan());
        CHECK(closed(s));
    }
}

TEST_CASE("Esc or closing the start screen gives a new 80 x 40 canvas", "[start-screen]") {
    MainWindow w;
    w.show();
    QString error;
    REQUIRE(w.newCanvas(QSize(30, 10), false, &error));
    auto* s = new StartScreen(&w);
    s->setAttribute(Qt::WA_DeleteOnClose);
    s->open();
    REQUIRE(bwtest::showExposed(s));
    const bool byKey = GENERATE(true, false);
    if (byKey)
        bwtest::systemKey(s, Qt::Key_Escape);
    else
        s->close();
    CHECK(w.document()->canvas().width() == 80);
    CHECK(w.document()->canvas().height() == 40);
    CHECK(w.document()->ansi().ice);
    CHECK(w.document()->path().isEmpty());
    CHECK_FALSE(w.document()->isModified());
    QApplication::processEvents();
    CHECK_FALSE(startScreenOf(w));
}

TEST_CASE("File > Start Screen asks to save a modified document first, as New does", "[start-screen]") {
    MainWindow w;
    w.show();
    QAction* again = w.findChild<QAction*>(QStringLiteral("startScreen"));
    REQUIRE(again);
    CHECK(again->text() == QStringLiteral("Star&t Screen..."));
    QString error;
    REQUIRE(w.newCanvas(QSize(30, 10), true, &error));
    QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QApplication::sendEvent(w.canvasView(), &press);
    REQUIRE(w.document()->isModified());
    bool asked = false;
    SECTION("Cancel keeps the document and shows no screen") {
        answer(&w, QMessageBox::Cancel, &asked);
        again->trigger();
        CHECK(asked);
        CHECK(w.document()->isModified());
        CHECK(w.document()->canvas().width() == 30);
        CHECK_FALSE(startScreenOf(w));
    }
    SECTION("Discard starts over with the screen over a new canvas") {
        answer(&w, QMessageBox::Discard, &asked);
        again->trigger();
        CHECK(asked);
        CHECK_FALSE(w.document()->isModified());
        CHECK(w.document()->canvas().width() == 80);
        StartScreen* s = startScreenOf(w);
        REQUIRE(s);
        CHECK(s->isVisible());
    }
}

TEST_CASE("the start screen draws the logo, or the wordmark without one", "[start-screen][logo]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    StartScreen::Options o;
    o.available = QSize(1920, 1080);

    SECTION("a logo resource that isn't built in: the wordmark") {
        o.logoPath = QStringLiteral(":/branding/no-such-logo.ans");
        REQUIRE_FALSE(QFile::exists(o.logoPath));
        StartScreen s(&w, o);
        CHECK(s.logoScale() == 0);
        CHECK(child<QLabel>(&s, "startWordmark")->text() == QStringLiteral("Blockwright"));
        CHECK_FALSE(s.findChild<QLabel*>(QStringLiteral("startLogo")));
    }
    SECTION("a missing, empty or unreadable file: the wordmark") {
        const QString path = GENERATE(as<QString>{}, QStringLiteral("missing.ans"), QStringLiteral("empty.ans"),
                                      QStringLiteral("dir"));
        o.logoPath = tmp.filePath(path);
        if (path == QStringLiteral("empty.ans"))
            writeBytes(o.logoPath, "");
        if (path == QStringLiteral("dir"))
            REQUIRE(QDir(tmp.path()).mkdir(path));
        StartScreen s(&w, o);
        CHECK(s.logoScale() == 0);
        CHECK(child<QLabel>(&s, "startWordmark")->text() == QStringLiteral("Blockwright"));
        CHECK(renderStartLogo(o.logoPath).isNull());
    }
    SECTION("a logo: the renderer's image of it, at 2x where it fits") {
        o.logoPath = logoAt(tmp.filePath(QStringLiteral("logo.ans")), 20, 5);
        const QImage expected = renderCanvas(logoDocument(20, 5).canvas, 8, 2);
        StartScreen s(&w, o);
        CHECK(s.logoScale() == 2);
        CHECK_FALSE(s.findChild<QLabel*>(QStringLiteral("startWordmark")));
        const QImage shown = child<QLabel>(&s, "startLogo")->pixmap().toImage().convertToFormat(expected.format());
        CHECK(shown.size() == QSize(20 * 8 * 2, 5 * 16 * 2));
        CHECK(shown == expected);
        CHECK(renderStartLogo(o.logoPath) == renderCanvas(logoDocument(20, 5).canvas, 8, 1));
    }
    SECTION("the version under it") {
        StartScreen s(&w, o);
        CHECK(child<QLabel>(&s, "startVersion")->text().startsWith(QStringLiteral("blockwright 0.")));
    }
}

TEST_CASE("the start screen fits a 720-high screen, the logo at 1x there and 2x on a large one",
          "[start-screen][logo]") {
    clearRecent();
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    // a full recent list, as tall as it gets
    for (int i = 0; i < kRecentLimit; ++i)
        addRecentFile(tmp.filePath(QStringLiteral("a-long-name-for-a-recent-piece-%1.ans").arg(i)));
    MainWindow w;
    StartScreen::Options o;
    o.logoPath = logoAt(tmp.filePath(QStringLiteral("logo.ans")), 80, 20);
    SECTION("1280 x 720 and 1366 x 768: 1x, and it fits with 60 pixels to spare") {
        o.available = GENERATE(QSize(1280, 720), QSize(1366, 768));
        StartScreen s(&w, o);
        CHECK(s.logoScale() == 1);
        const QSize hint = s.sizeHint();
        CHECK(hint.height() <= 660);
        CHECK(hint.width() <= o.available.width());
        CHECK(child<QLabel>(&s, "startLogo")->pixmap().size() == QSize(640, 320));
    }
    SECTION("1920 x 1080: 2x, and it still fits") {
        o.available = QSize(1920, 1080);
        StartScreen s(&w, o);
        CHECK(s.logoScale() == 2);
        CHECK(s.sizeHint().height() <= 1080 - 60);
        CHECK(child<QLabel>(&s, "startLogo")->pixmap().size() == QSize(1280, 640));
    }
}

TEST_CASE("startLogoScale picks 2x only when the doubled logo and the rest fit", "[start-screen][logo]") {
    clearRecent();
    const QSize logo(640, 320);
    const QSize rest(700, 300);
    // 2x: 1280 + 32 wide, 300 + 640 high, with 60 kept for the frame
    CHECK(startLogoScale(logo, rest, QSize(1312, 1000)) == 2);
    CHECK(startLogoScale(logo, rest, QSize(1311, 1000)) == 1);
    CHECK(startLogoScale(logo, rest, QSize(1920, 999)) == 1);
    CHECK(startLogoScale(logo, QSize(1400, 300), QSize(1399, 1080)) == 1);
}

TEST_CASE("the shipped start screen draws the real logo, 80 x 18 cells, with --version's first line under it",
          "[start-screen][logo][branding]") {
    clearRecent();
    // the resource is the large logo #205 ships, byte for byte
    CHECK(QString::fromLatin1(kStartLogoPath) == QString::fromLatin1(kLogoLarge));
    QFile f(QString::fromLatin1(kStartLogoPath));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    CHECK(bytes.toStdString() == bwtest::readFile(std::filesystem::path(BW_BRANDING_DIR) / "logo-large.ans"));
    const AnsiDocument doc = parseAnsi(std::string_view(bytes.constData(), size_t(bytes.size())));
    CHECK(doc.canvas.width() == 80);
    CHECK(doc.canvas.height() == 18);

    MainWindow w;
    const auto [available, scale] = GENERATE(std::pair(QSize(1280, 720), 1), std::pair(QSize(1920, 1080), 2));
    INFO(available.width() << "x" << available.height());
    StartScreen::Options o;  // the default logo path, as startUp() makes it
    o.available = available;
    StartScreen s(&w, o);
    CHECK(s.logoScale() == scale);
    CHECK_FALSE(s.findChild<QLabel*>(QStringLiteral("startWordmark")));
    RenderOptions opts;
    opts.scale = scale;
    const QImage expected = renderDocument(doc, opts);
    const QImage shown = child<QLabel>(&s, "startLogo")->pixmap().toImage().convertToFormat(expected.format());
    CHECK(shown.size() == expected.size());
    CHECK(shown == expected);
    CHECK(renderStartLogo(QString::fromLatin1(kStartLogoPath)).size() == expected.size() / scale);
    // the line under it is the first line blockwright --version prints
    CHECK(child<QLabel>(&s, "startVersion")->text() ==
          QStringLiteral("blockwright " BLOCKWRIGHT_VERSION " (" BLOCKWRIGHT_GIT_COMMIT ")"));
}
