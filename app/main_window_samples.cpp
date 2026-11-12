// The sample projects (PLAN §12, samples.h): Help > Samples and the start
// screen's Samples open one as an untitled copy.

#include "main_window.h"
#include "message_box.h"

#include "blockwright/composite.h"
#include "blockwright/library.h"
#include "blockwright/samples.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>

#include <exception>

namespace bw {

namespace {

std::filesystem::path fspath(const QString& s) { return std::filesystem::path(QFile::encodeName(s).toStdString()); }

QString qpath(const std::filesystem::path& p) { return QFile::decodeName(QByteArray::fromStdString(p.string())); }

} // namespace

std::vector<samples::Sample> MainWindow::bundledSamples() {
    // Beside the real binary, as the bundled Library is (createLibrary).
    const std::filesystem::path binDir = library::realBinDir(fspath(QCoreApplication::applicationFilePath()),
                                                             fspath(QCoreApplication::applicationDirPath()));
    return samples::list(samples::bundledDir(binDir));
}

bool MainWindow::openSampleFile(const QString& path, QString* error) {
    project::Project p;
    if (!Document::readProject(path, &p, error))
        return false;
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
    // Untitled, so Save asks where: the installed sample is never written.
    // Unmodified until it is edited, and off the recent list, which holds
    // the user's own files. Its generators wait to be trusted, as any
    // project's do.
    doc_.replace(std::move(p), QString(), FileForm::ProjectZip);
    statusBar()->showMessage(tr("Opened a copy of the sample %1").arg(QFileInfo(path).fileName()), 5000);
    return true;
}

void MainWindow::openSample(const QString& path) {
    if (!maybeSave())
        return;
    QString error;
    if (!openSampleFile(path, &error))
        bw::warning(promptParent(), tr("Open Sample"),
                    tr("Could not open the sample %1:\n%2").arg(QFileInfo(path).fileName(), error));
}

void MainWindow::fillSamplesMenu() {
    samplesMenu_->clear();
    const std::vector<samples::Sample> list = bundledSamples();
    for (const samples::Sample& s : list) {
        const QString path = qpath(s.path);
        QString text = QString::fromStdString(s.title);
        QAction* a = samplesMenu_->addAction(text.replace(QLatin1Char('&'), QStringLiteral("&&")), this,
                                             [this, path] { openSample(path); });
        a->setProperty("path", path);
        if (s.problem.empty()) {
            a->setStatusTip(QString::fromStdString(s.summary));
            a->setToolTip(QString::fromStdString(s.summary));
        } else {
            a->setEnabled(false);
            a->setStatusTip(tr("Can't be read: %1").arg(QString::fromStdString(s.problem)));
            a->setToolTip(a->statusTip());
        }
    }
    if (list.empty())
        samplesMenu_->addAction(tr("(none installed)"))->setEnabled(false);
}

void MainWindow::createSamplesMenu(QMenu* help) {
    samplesMenu_ = help->addMenu(tr("&Samples"));
    samplesMenu_->setObjectName(QStringLiteral("samplesMenu"));
    samplesMenu_->setToolTipsVisible(true);
    connect(samplesMenu_, &QMenu::aboutToShow, this, &MainWindow::fillSamplesMenu);
}

} // namespace bw
