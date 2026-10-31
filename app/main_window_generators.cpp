// Generator layers (PLAN §7) in the window: a changed parameter or Run runs
// the layer's generator (GeneratorRunner), but only once the project's
// generators are trusted, which is asked the first time one would run in a
// session. Opening, saving and exporting never run one: they keep the
// cells the last run left. A refused or failed run leaves the cells too,
// and says why in the status bar. File > Open Scene... opens a Python scene
// as a new project of generator layers (scene_import.h), running the scene
// once, after asking.

#include "main_window.h"

#include "generator_runner.h"

#include "blockwright/generator.h"
#include "blockwright/scene_import.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

#include <exception>
#include <optional>
#include <utility>

namespace bw {

namespace {

// The project a path names, the same whichever way it is written.
QString projectKey(const QString& path) {
    if (path.isEmpty())
        return {};
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical;
}

} // namespace

void MainWindow::createGenerators() {
    runner_ = new GeneratorRunner(&doc_, this);
    // A new or opened document is trusted if its project was this session;
    // a trusted one saved under another name stays trusted.
    connect(&doc_, &Document::reset, this, [this] {
        generatorsTrusted_ = false;
        trustFromPath_ = true;
    });
    connect(&doc_, &Document::pathChanged, this, [this](const QString& path) {
        const QString key = projectKey(path);
        if (trustFromPath_)
            generatorsTrusted_ = !key.isEmpty() && trustedProjects_.contains(key);
        else if (generatorsTrusted_ && !key.isEmpty())
            trustedProjects_.insert(key);
        trustFromPath_ = false;
    });
    connect(&doc_, &Document::generatorRunWanted, this, [this](LayerId id, quint64 serial) {
        if (!trustGenerators(id)) {
            statusBar()->showMessage(tr("Generator not run: this project's generators are not trusted; "
                                        "the layer keeps its cells"),
                                     8000);
            return;
        }
        runner_->request(id, serial);
    });
    connect(runner_, &GeneratorRunner::runStarted, this,
            [this](LayerId) { statusBar()->showMessage(tr("Running generator...")); });
    connect(runner_, &GeneratorRunner::runFinished, this, [this](LayerId id, bool applied, const QString& error) {
        const Layer* l = doc_.layers().find(id);
        const QString name = l ? QString::fromStdString(l->props.name) : QString::number(id);
        if (applied)
            statusBar()->showMessage(tr("Ran generator %1").arg(name), 3000);
        else if (!error.isEmpty())
            statusBar()->showMessage(tr("Generator %1 failed; the layer keeps its cells: %2").arg(name, error));
        else
            statusBar()->clearMessage();
    });
}

bool MainWindow::trustGenerators(LayerId id) {
    if (generatorsTrusted_)
        return true;
    QString command;
    if (const std::optional<generator::Spec> spec = doc_.generatorSpec(id, nullptr))
        for (const std::string& arg : spec->command)
            command += (command.isEmpty() ? QString() : QStringLiteral(" ")) + QString::fromStdString(arg);
    const QString where = doc_.path().isEmpty() ? tr("this document") : QDir::toNativeSeparators(doc_.path());
    if (!askToRun(tr("Run the generators in %1?").arg(where),
                  tr("A generator is a program the project names, run on this computer with your "
                     "permissions. Run them only if you trust where the project came from."),
                  command.isEmpty() ? tr("(unknown)") : command))
        return false;
    generatorsTrusted_ = true;
    if (const QString key = projectKey(doc_.path()); !key.isEmpty())
        trustedProjects_.insert(key);
    return true;
}

bool MainWindow::askToRun(const QString& question, const QString& why, const QString& command) {
    QMessageBox box(QMessageBox::Warning, tr("Run Generators"), question, QMessageBox::Yes | QMessageBox::No, this);
    box.setObjectName(QStringLiteral("trustGenerators"));
    box.setInformativeText(tr("%1\n\nThis one runs: %2").arg(why, command));
    box.setDefaultButton(QMessageBox::No);
    return box.exec() == QMessageBox::Yes;
}

void MainWindow::openScene() {
    // Refused before anything is asked or run, the Python probe included:
    // openSceneFile would refuse it anyway (generator::killRunning knows
    // one generator run's group).
    if (runner_->busy()) {
        QMessageBox::warning(this, tr("Open Scene"), tr("A generator is running; open the scene when it ends."));
        return;
    }
    if (!maybeSave())
        return;
    const QString dir = doc_.path().isEmpty() ? QString() : QFileInfo(doc_.path()).absolutePath();
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Open Scene"), dir, tr("Python scenes (*.py);;All files (*)"));
    if (path.isEmpty())
        return;
    const QFileInfo fi(path);
    const QString scene =
        QDir::toNativeSeparators(fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath());
    // Which Python runs it, found before asking so the question names it;
    // on Windows that runs each candidate's --version (scene::findPython).
    scene::PythonCommand pythonCommand;
    try {
        pythonCommand = scene::findPython();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Open Scene"),
                             tr("Could not open %1:\n%2").arg(path, QString::fromStdString(e.what())));
        return;
    }
    const QString python = QString::fromStdString(scene::pythonText(pythonCommand)) + QLatin1Char(' ') +
                           QString::fromUtf8(scene::kNoBytecode.data(), qsizetype(scene::kNoBytecode.size()));
    if (!askToRun(tr("Run the scene %1?").arg(scene),
                  tr("Opening a scene runs it, a Python program, on this computer with your permissions, once "
                     "for its layers and once for each layer's cells; the project's layers run it again. Run it "
                     "only if you trust where it came from."),
                  tr("%1 %2 --list-layers, then --layer-ans for each layer").arg(python, scene)))
        return;
    QString error;
    if (!openSceneFile(path, &error, pythonCommand))
        QMessageBox::warning(this, tr("Open Scene"), tr("Could not open %1:\n%2").arg(path, error));
}

bool MainWindow::openSceneFile(const QString& path, QString* error, const std::vector<std::string>& python) {
    // generator::killRunning knows one run's group: wait for the runner's.
    if (runner_->busy()) {
        *error = tr("a generator is running; open the scene when it ends");
        return false;
    }
    struct Imported {
        std::optional<project::Project> project;
        QString error;
    };
    QProgressDialog progress(tr("Listing the scene's layers..."), QString(), 0, 0, this);
    progress.setObjectName(QStringLiteral("openSceneProgress"));
    progress.setWindowTitle(tr("Open Scene"));
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();
    QProgressDialog* dialog = &progress;
    const std::string scenePath = QFile::encodeName(path).toStdString();
    QFutureWatcher<Imported> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<Imported>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([scenePath, dialog, python] {
        Imported out;
        try {
            out.project = scene::importScene(
                scenePath, {}, [dialog](std::size_t index, std::size_t count, const std::string& name) {
                    // Queued to the dialog, and dropped once it is gone.
                    QMetaObject::invokeMethod(
                        dialog,
                        [dialog, index, count, name = QString::fromStdString(name)] {
                            dialog->setMaximum(int(count));
                            dialog->setValue(int(index));
                            dialog->setLabelText(
                                MainWindow::tr("Running layer %1 of %2 (%3)...").arg(index + 1).arg(count).arg(name));
                        },
                        Qt::QueuedConnection);
                },
                python);
        } catch (const std::exception& e) {
            out.error = QString::fromStdString(e.what());
        }
        return out;
    }));
    if (!watcher.isFinished())
        loop.exec();
    progress.hide();
    Imported result = watcher.result();
    if (!result.project) {
        *error = result.error;
        return false;
    }
    project::Project p = std::move(*result.project);
    // As openProject: the renderer takes the composite first, so a canvas
    // too large to display leaves the open document as it was.
    AnsiDocument candidate;
    candidate.ice = p.ice;
    candidate.sauce = p.sauce;
    candidate.canvas = composite(p.layers, {p.ice, Cell{}});
    try {
        renderer_.setCanvas(&candidate.canvas, glyphWidth(candidate));
    } catch (const std::exception&) {
        *error = tr("the canvas is too large to display");
        return false;
    }
    doc_.replace(std::move(p), QString(), FileForm::ProjectZip);
    doc_.markUnsaved();
    // They have just run, on the user's word.
    generatorsTrusted_ = true;
    statusBar()->showMessage(tr("Opened scene %1").arg(QFileInfo(path).fileName()), 5000);
    return true;
}

} // namespace bw
