// Generator layers (PLAN §7) in the window: a changed parameter or Run runs
// the layer's generator (GeneratorRunner), but only once the project's
// generators are trusted, which is asked the first time one would run in a
// session. Opening, saving and exporting never run one: they keep the
// cells the last run left. A refused or failed run leaves the cells too,
// and says why in the status bar.

#include "main_window.h"

#include "generator_runner.h"

#include "blockwright/generator.h"

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>

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
    QMessageBox box(QMessageBox::Warning, tr("Run Generators"),
                    tr("Run the generators in %1?").arg(where), QMessageBox::Yes | QMessageBox::No, this);
    box.setObjectName(QStringLiteral("trustGenerators"));
    box.setInformativeText(tr("A generator is a program the project names, run on this computer with your "
                              "permissions. Run them only if you trust where the project came from.\n\n"
                              "This one runs: %1")
                               .arg(command.isEmpty() ? tr("(unknown)") : command));
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes)
        return false;
    generatorsTrusted_ = true;
    if (const QString key = projectKey(doc_.path()); !key.isEmpty())
        trustedProjects_.insert(key);
    return true;
}

} // namespace bw
