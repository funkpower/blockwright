// Projects (PLAN §6) in the window: opening a .bwp zip or a project
// directory with its layers, saving one in its form, Save As across forms
// and snapshots.

#include "main_window.h"

#include "canvas_view.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>

#include <stdexcept>
#include <utility>

namespace bw {

void MainWindow::createProjectActions(QMenu* file) {
    QAction* a = file->addAction(tr("Save As Project &Directory..."), this, &MainWindow::saveAsOtherForm);
    a->setObjectName(QStringLiteral("saveAsOtherForm"));
    otherFormAction_ = a;
    a = file->addAction(tr("Take Sna&pshot"), this, &MainWindow::takeSnapshot);
    a->setObjectName(QStringLiteral("snapshot"));
    snapshotAction_ = a;
    connect(&doc_, &Document::pathChanged, this, &MainWindow::syncProjectActions);
    syncProjectActions();
}

void MainWindow::syncProjectActions() {
    const bool dir = doc_.form() == FileForm::ProjectDirectory;
    otherFormAction_->setText(dir ? tr("Save As &File...") : tr("Save As Project &Directory..."));
    snapshotAction_->setEnabled(doc_.form() != FileForm::Ansi);
}

bool MainWindow::openProject(const QString& path, QString* error) {
    project::Project p;
    if (!Document::readProject(path, &p, error))
        return false;
    // As openFile: the renderer takes the composite first, so a canvas too
    // large to display leaves the open document as it was.
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
    const QFileInfo fi(path);
    const FileForm form = fi.isDir() ? FileForm::ProjectDirectory : FileForm::ProjectZip;
    // Cleaned, so a directory given as "piece/" is titled "piece".
    doc_.replace(std::move(p), QDir::cleanPath(fi.absoluteFilePath()), form);
    return true;
}

AnsiOutput MainWindow::saveLoss(const QString& path, FileForm form) const {
    if (form == FileForm::Ansi)
        return doc_.encode(artFormatFor(QFile::encodeName(path).toStdString()));
    // A project keeps every glyph; only blink lost on open is gone, and an
    // XBIN's palette and font, which it has no place for.
    AnsiOutput out;
    out.blinkDropped = doc_.lostBlinkOnOpen();
    out.paletteDropped = customPalette(doc_.ansi());
    out.fontDropped = doc_.ansi().font.has_value();
    return out;
}

bool MainWindow::writeFile(const QString& path, FileForm form, QString* error, bool allowLossy) {
    // A floating selection is saved where it floats.
    view_->finishTool();
    const AnsiOutput out = saveLoss(path, form);
    if (out.lossy() && !allowLossy) {
        *error = lossDescription(out);
        return false;
    }
    return form == FileForm::Ansi ? doc_.save(path, out, error) : doc_.saveProject(path, form, error);
}

void MainWindow::openDirectory() {
    if (!maybeSave())
        return;
    const QString dir = doc_.path().isEmpty() ? QString() : QFileInfo(doc_.path()).absolutePath();
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open Project Directory"), dir);
    if (!path.isEmpty())
        openInteractive(path);
}

bool MainWindow::saveAsOtherForm() {
    return doc_.form() == FileForm::ProjectDirectory ? saveAsFile() : saveAsDirectory();
}

bool MainWindow::saveAsDirectory() {
    const QString start = doc_.path().isEmpty() ? QString() : QFileInfo(doc_.path()).absolutePath();
    const QString chosen = QFileDialog::getExistingDirectory(this, tr("Save As Project Directory"), start);
    if (chosen.isEmpty())
        return false;
    const QString path = QDir::cleanPath(QFileInfo(chosen).absoluteFilePath());
    // writeDirectory refuses a directory holding anything but a project; one
    // that holds another project is replaced, so ask first.
    if (path != doc_.path() && project::isProject(QFile::encodeName(path).toStdString()) &&
        QMessageBox::warning(this, tr("Save As"),
                             tr("%1 already holds a project.\nDo you want to replace it?").arg(path),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    return saveInteractive(path, FileForm::ProjectDirectory);
}

int MainWindow::snapshot(QString* error, bool allowLossy) {
    view_->finishTool();
    if (doc_.form() == FileForm::Ansi) {
        *error = tr("only a project keeps snapshots: save it as a project first");
        return 0;
    }
    project::Snapshot s;
    AnsiOutput out;
    if (!doc_.nextSnapshot(&s, &out, error))
        return 0;
    if (out.lossy() && !allowLossy) {
        *error = lossDescription(out);
        return 0;
    }
    const int round = s.round;
    doc_.addSnapshot(std::move(s));
    return round;
}

void MainWindow::takeSnapshot() {
    view_->finishTool();
    project::Snapshot s;
    AnsiOutput out;
    QString error;
    if (doc_.form() == FileForm::Ansi || !doc_.nextSnapshot(&s, &out, &error)) {
        QMessageBox::warning(this, tr("Take Snapshot"), tr("Could not take a snapshot:\n%1").arg(error));
        return;
    }
    if (out.lossy() &&
        QMessageBox::warning(this, tr("Take Snapshot"),
                             tr("Snapshot r%1, as .ans:\n%2.\n\nTake it anyway?").arg(s.round).arg(lossDescription(out)),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    const int round = s.round;
    doc_.addSnapshot(std::move(s));
    statusBar()->showMessage(tr("Took snapshot r%1; saving the project keeps it").arg(round), 3000);
}

} // namespace bw
