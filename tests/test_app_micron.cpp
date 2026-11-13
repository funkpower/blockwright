// File > Export Micron...: the loss question, the options dialog and the
// page it writes (micron.h), and `blockwright-cli export --micron` writing
// the same page for a flat file.

#include "util.h"

#include "document.h"
#include "main_window.h"
#include "message_box.h"
#include "scratch_settings.h"

#include "blockwright/micron.h"
#include "blockwright/sauce.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdint>
#include <string>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

std::string readAll(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll().toStdString();
}

AnsiDocument tux() {
    const auto p = bwtest::dataDir() / "xbin" / "tux-palette-font.xb";
    const std::string data = bwtest::readFile(p);
    return parseArt(data, artFormatOf(p.string(), data));
}

// Answers what Export Micron asks, in turn: the loss question with `loss`
// (keeping its text), the options with `centre` and `cache`, and the file
// dialog with `path`.
struct MicronAnswers {
    QMessageBox::StandardButton loss = QMessageBox::Save;
    bool centre = false;
    std::optional<int> cache;
    QString path;
    QString lossText;
    bool askedLoss = false, setOptions = false, picked = false;
    QTimer timer;

    MicronAnswers() {
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            QWidget* modal = QApplication::activeModalWidget();
            if (auto* box = qobject_cast<QMessageBox*>(modal); box && !askedLoss) {
                askedLoss = true;
                lossText = boxText(box);
                box->button(loss)->click();
            } else if (auto* d = qobject_cast<QFileDialog*>(modal); d && !picked) {
                picked = true;
                auto* name = d->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
                REQUIRE(name);
                name->setText(path);
                QMetaObject::invokeMethod(d, "accept");
            } else if (auto* o = qobject_cast<QDialog*>(modal);
                       o && o->objectName() == QStringLiteral("micronOptions") && !setOptions) {
                setOptions = true;
                auto* c = o->findChild<QCheckBox*>(QStringLiteral("micronCentre"));
                auto* k = o->findChild<QCheckBox*>(QStringLiteral("micronCache"));
                auto* s = o->findChild<QSpinBox*>(QStringLiteral("micronCacheSeconds"));
                REQUIRE((c && k && s));
                CHECK_FALSE(s->isEnabled());
                c->setChecked(centre);
                k->setChecked(cache.has_value());
                CHECK(s->isEnabled() == cache.has_value());
                if (cache)
                    s->setValue(*cache);
                o->accept();
            }
        });
        timer.start();
    }
};

bool fileDialogsAllowed() {
    // A QFileDialog writes Qt's own settings when it is destroyed. Only where
    // they are in the scratch directory may this test build one.
    return bwtest::qtSettingsInScratch(QFileInfo(bwtest::scratchSettingsFile()).absolutePath());
}

} // namespace

TEST_CASE("exportMicronText writes the canvas as a Micron page in the piece's own palette") {
    QTemporaryDir dir;
    MainWindow w;
    const AnsiDocument t = tux();
    REQUIRE(t.palette);
    w.document()->replace(t, QString());
    const QString path = dir.filePath(QStringLiteral("tux.mu"));
    QString error;
    REQUIRE(w.exportMicronText(path, &error));
    CHECK(readAll(path) == micron::encode(t));
    REQUIRE(w.exportMicronText(path, &error, {.centre = true, .cacheSeconds = 60u}));
    CHECK(readAll(path) == micron::encode(t, {.centre = true, .cacheSeconds = 60u}));
    CHECK_FALSE(w.exportMicronText(dir.filePath(QStringLiteral("no/such/dir/x.mu")), &error));
    CHECK_FALSE(error.isEmpty());
}

TEST_CASE("Export Micron asks about glyphs NomadNet drops, then for the options and the file") {
    if (!fileDialogsAllowed())
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    QTemporaryDir dir;
    MainWindow w;
    AnsiDocument d;
    d.canvas = Canvas(4, 2);
    d.canvas.at(1, 0).ch = U'​';
    w.document()->replace(d, QString());
    const QString path = dir.filePath(QStringLiteral("piece.mu"));

    {  // Cancel at the loss question writes nothing
        MicronAnswers a;
        a.loss = QMessageBox::Cancel;
        a.path = path;
        act(w, "exportMicron")->trigger();
        CHECK(a.askedLoss);
        CHECK(a.lossText.contains(QStringLiteral("Micron page")));
        CHECK_FALSE(a.setOptions);
        CHECK_FALSE(QFile::exists(path));
    }
    {  // export anyway, centred with a cache header
        MicronAnswers a;
        a.centre = true;
        a.cache = 120;
        a.path = path;
        act(w, "exportMicron")->trigger();
        CHECK(a.askedLoss);
        CHECK(a.setOptions);
        CHECK(a.picked);
        CHECK(readAll(path) == micron::encode(d, {.centre = true, .cacheSeconds = 120u}));
    }
}

TEST_CASE("Export Micron asks nothing about a piece it writes whole") {
    if (!fileDialogsAllowed())
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    QTemporaryDir dir;
    MainWindow w;
    const AnsiDocument t = tux();
    w.document()->replace(t, QString());
    MicronAnswers a;
    a.path = dir.filePath(QStringLiteral("tux.mu"));
    act(w, "exportMicron")->trigger();
    CHECK_FALSE(a.askedLoss);
    CHECK(a.setOptions);
    CHECK(readAll(a.path) == micron::encode(t));
}

namespace {

// A file of `body` with SAUCE `width` columns wide and its iCE flag `ice`.
QString writeFlat(const QTemporaryDir& tmp, const char* name, const std::string& body, int width, bool ice) {
    Sauce s = Sauce::fresh(width, 1);
    s.setIce(ice);
    const QString path = tmp.filePath(QString::fromLatin1(name));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    const std::string bytes = body + encodeSauce(s, std::uint32_t(body.size()));
    REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
    return path;
}

// The page `blockwright-cli export --micron` writes for `path`.
std::string cliMicron(const QString& path) {
    const QString mu = path + QStringLiteral(".cli.mu");
    QFile::remove(mu);
    QProcess p;
    p.start(QStringLiteral(BW_CLI), {QStringLiteral("export"), QStringLiteral("--micron"), path, mu});
    REQUIRE(p.waitForFinished(60000));
    INFO(QString::fromUtf8(p.readAllStandardError()).toStdString());
    REQUIRE(p.exitStatus() == QProcess::NormalExit);
    REQUIRE(p.exitCode() == 0);
    return readAll(mu);
}

// The page the app's Export Micron writes for `path`, opened in a window.
std::string appMicron(const QTemporaryDir& tmp, const QString& path) {
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    const QString mu = tmp.filePath(QFileInfo(path).fileName() + QStringLiteral(".app.mu"));
    REQUIRE(w.exportMicronText(mu, &error));
    return readAll(mu);
}

} // namespace

TEST_CASE("cli export --micron writes a flat file as the app opens and exports it", "[cli][micron]") {
    // The editor opens an iCE-off file holding a bright background with iCE
    // on (openAsEditor), so the background is written bright; the CLI opens
    // it the same way, and keeps the blink rules the app's open gives
    QTemporaryDir tmp;
    const QString bright = writeFlat(tmp, "bright.ans", "\x1b[101m ", 1, false);
    CHECK(appMicron(tmp, bright) == "`Bf55 ``\n");
    CHECK(cliMicron(bright) == "`Bf55 ``\n");

    struct Case {
        const char* name;
        const char* body;
        int width;
        bool ice;
    };
    const Case cases[] = {
        // a blinking cell beside a bright background: iCE turns on, the blink
        // is dropped and the cell keeps its dark background
        {"blink-bright.ans", "\x1b[0;5;31;40m\xdb\x1b[0;104mA", 2, false},
        // blink without a bright background: iCE stays off, the cell is dark
        {"blink-dark.ans", "\x1b[5;41m ", 1, false},
        // iCE on: the blink is a bright background
        {"blink-ice.ans", "\x1b[5;41m ", 1, true},
        // a 24-bit background in an iCE-off file
        {"rgb.ans", "\x1b[48;2;255;64;64m ", 1, false},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const QString path = writeFlat(tmp, c.name, c.body, c.width, c.ice);
        CHECK(cliMicron(path) == appMicron(tmp, path));
    }
}
