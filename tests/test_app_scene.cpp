// File > Open Scene... (PLAN §7): a Python scene opens as an untitled,
// unsaved project of generator layers, after the trust question names the
// scene's command; No runs nothing. tests/data/scenes/lighthouse is the
// scene (scene_env.h says what it needs).

#include "document.h"
#include "main_window.h"
#include "scratch_settings.h"

#include "blockwright/generator.h"

#include "scene_env.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QDir>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>

#include <filesystem>
#include <string>

using namespace bw;
namespace fs = std::filesystem;

namespace {

template <typename F>
bool waitFor(F done, int ms = 30000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// The lighthouse scene copied into `tmp`; its path.
QString copyScene(const QTemporaryDir& tmp) {
    const QString path = tmp.filePath(QStringLiteral("scene.py"));
    REQUIRE(QFile::copy(QString::fromStdString((bwtest::dataDir() / "scenes" / "lighthouse" / "scene.py").string()),
                        path));
    return path;
}

std::vector<std::string> names(const Document& d) {
    std::vector<std::string> out;
    for (const Layer& l : d.layers().layers())
        out.push_back(l.props.name);
    return out;
}

// Picks `path` in the file dialog, then answers the trust question with
// `button`, keeping its text in `question` and `command`.
struct SceneAnswers {
    QString path;
    QMessageBox::StandardButton button;
    QString question, informative;
    bool picked = false;
    QTimer timer;

    SceneAnswers(QString p, QMessageBox::StandardButton b) : path(std::move(p)), button(b) {
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            QWidget* modal = QApplication::activeModalWidget();
            if (auto* dialog = qobject_cast<QFileDialog*>(modal); dialog && !picked) {
                picked = true;
                // typed into Qt's dialog, as a user would
                auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
                REQUIRE(name);
                name->setText(path);
                QMetaObject::invokeMethod(dialog, "accept");
            } else if (auto* box = qobject_cast<QMessageBox*>(modal);
                       box && box->objectName() == QStringLiteral("trustGenerators")) {
                question = box->text();
                informative = box->informativeText();
                box->button(button)->click();
            }
        });
        timer.start();
    }
};

} // namespace

TEST_CASE("openSceneFile opens a scene as an unsaved project of generator layers", "[scene]") {
    if (const std::string why = bwtest::sceneSkipReason(); !why.empty())
        SKIP(why);
    QTemporaryDir tmp;
    const QString scene = copyScene(tmp);
    MainWindow w;
    QString error;
    REQUIRE(w.openSceneFile(scene, &error));
    CHECK(error.isEmpty());
    Document& d = *w.document();
    CHECK(names(d) == std::vector<std::string>{"sky", "sea", "tower", "beam"});
    for (const Layer& l : d.layers().layers())
        CHECK(l.props.kind == LayerKind::Generator);
    CHECK(d.canvas().width() == 24);
    CHECK(d.canvas().height() == 8);
    CHECK(d.path().isEmpty());
    CHECK(d.form() == FileForm::ProjectZip);
    CHECK(d.isModified());
    CHECK(w.generatorsTrusted());
    // the layers' runs name the scene where it is
    const std::optional<generator::Spec> spec = d.generatorSpec(d.layers()[1].props.id, nullptr);
    REQUIRE(spec);
    CHECK(spec->command[2] == QFileInfo(scene).canonicalFilePath().toStdString());

    // saved, it is a project like any other
    const QString saved = tmp.filePath(QStringLiteral("lighthouse.bwp"));
    REQUIRE(w.saveFile(saved, &error));
    CHECK_FALSE(d.isModified());
    MainWindow again;
    REQUIRE(again.openFile(saved, &error));
    CHECK(names(*again.document()) == names(d));
    CHECK(again.document()->canvas() == d.canvas());
}

TEST_CASE("a scene that fails leaves the open document as it was", "[scene]") {
    if (const std::string why = bwtest::sceneSkipReason(); !why.empty())
        SKIP(why);
    QTemporaryDir tmp;
    const QString broken = tmp.filePath(QStringLiteral("broken.py"));
    QFile f(broken);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("import sys\nsys.exit('no layers here')\n");
    f.close();
    MainWindow w;
    const std::vector<std::string> before = names(*w.document());
    QString error;
    CHECK_FALSE(w.openSceneFile(broken, &error));
    CHECK(error.contains(QStringLiteral("no layers here")));
    CHECK(names(*w.document()) == before);
    CHECK_FALSE(w.document()->isModified());
    CHECK_FALSE(w.openSceneFile(tmp.filePath(QStringLiteral("missing.py")), &error));
    CHECK(error.contains(QStringLiteral("is not a file")));
}

TEST_CASE("File > Open Scene asks before it runs the scene", "[scene]") {
    if (const std::string why = bwtest::sceneSkipReason(); !why.empty())
        SKIP(why);
    // A QFileDialog writes Qt's own settings when it is destroyed. Only where
    // they are in the scratch directory may this test build one.
    if (!bwtest::qtSettingsInScratch(QFileInfo(bwtest::scratchSettingsFile()).absolutePath()))
        SKIP("a QFileDialog would write Qt's native settings, which can't be moved off the user's on this platform");
    // Qt's own dialog, which a test can answer, rather than the platform's.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    struct Restore {
        ~Restore() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false); }
    } restore;
    QTemporaryDir tmp;
    const QString scene = copyScene(tmp);
    const QString canonical = QFileInfo(scene).canonicalFilePath();
    MainWindow w;
    auto* open = w.findChild<QAction*>(QStringLiteral("openScene"));
    REQUIRE(open);
    CHECK(open->text() == QStringLiteral("Open S&cene..."));

    SECTION("No runs nothing") {
        SceneAnswers answers(scene, QMessageBox::No);
        const std::vector<std::string> before = names(*w.document());
        open->trigger();
        CHECK(answers.picked);
        CHECK(answers.question.contains(canonical));
        CHECK(names(*w.document()) == before);
        CHECK_FALSE(w.generatorsTrusted());
        // and wrote nothing beside the scene
        CHECK(QDir(tmp.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries) ==
              QStringList{QStringLiteral("scene.py")});
    }
    SECTION("Yes opens it") {
        SceneAnswers answers(scene, QMessageBox::Yes);
        open->trigger();
        CHECK(answers.picked);
        CHECK(answers.question == QStringLiteral("Run the scene %1?").arg(canonical));
        CHECK(answers.informative.contains(QStringLiteral("python3 -B %1 --list-layers").arg(canonical)));
        REQUIRE(waitFor([&] { return w.document()->layers().size() == 4; }));
        CHECK(names(*w.document()) == std::vector<std::string>{"sky", "sea", "tower", "beam"});
        CHECK(w.document()->isModified());
        CHECK(w.generatorsTrusted());
    }
}
