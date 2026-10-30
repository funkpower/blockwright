// The Library parts of the main window (PLAN §12): the Library dock, placing
// a stamp on the canvas, and Selection > Save to Library....

#include "main_window.h"

#include "canvas_view.h"
#include "layer_style_dialog.h"
#include "library_dock.h"
#include "sauce_dialog.h"
#include "select_tool.h"
#include "tool.h"

#include "blockwright/library.h"

#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>

#include <algorithm>
#include <exception>
#include <optional>

namespace bw {

namespace {

std::filesystem::path fspath(const QString& s) { return std::filesystem::path(QFile::encodeName(s).toStdString()); }

QString qpath(const std::filesystem::path& p) { return QFile::decodeName(QByteArray::fromStdString(p.string())); }

} // namespace

void MainWindow::createLibrary() {
    library_ = new LibraryDock;
    libraryDock_ = new QDockWidget(tr("Library"), this);
    libraryDock_->setObjectName(QStringLiteral("libraryDock"));
    libraryDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    libraryDock_->setWidget(library_);
    addDockWidget(Qt::RightDockWidgetArea, libraryDock_);
    // The bundled library sits beside the real binary, not beside a symlink to
    // it (the dev install links ~/.local/bin/blockwright-dev into its prefix).
    const std::filesystem::path binDir = library::realBinDir(fspath(QCoreApplication::applicationFilePath()),
                                                             fspath(QCoreApplication::applicationDirPath()));
    library_->setDirectories(library::bundledLibraryDir(binDir),
                             library::userLibraryDir());

    // A drag can come from any application, so its payload is only a key to
    // look up in the current scan: it is never decoded into a path, and
    // anything that isn't a stamp in the dock is refused.
    const auto place = [this](const QByteArray& key, QPoint cell) {
        const library::Stamp* stamp = library_->findKey(key);
        if (!stamp) {
            statusBar()->showMessage(tr("Not a stamp in the Library; nothing placed"), 5000);
            return;
        }
        const std::filesystem::path path = stamp->path;
        QString error;
        if (!placeStamp(path, cell, &error))
            statusBar()->showMessage(tr("Could not place %1: %2").arg(qpath(path), error), 5000);
    };
    connect(library_, &LibraryDock::stampActivated, this, [this, place](const QByteArray& key) {
        const Cursor c = view_->cellCursor();
        place(key, {c.x, c.y});
    });
    auto* drops = new StampDropFilter(view_);
    connect(drops, &StampDropFilter::dropped, this, place);
}

bool MainWindow::placeStamp(const QString& path, QPoint cell, QString* error) {
    return placeStamp(fspath(path), cell, error);
}

bool MainWindow::placeStamp(const std::filesystem::path& path, QPoint cell, QString* error) {
    std::optional<library::Stamp> read;
    const library::Stamp* stamp = library_->find(path);
    if (!stamp) {
        try {
            read = library::readStamp(path);
        } catch (const std::exception& e) {
            // LibraryError, or anything else reading it threw (out of
            // memory): never out of an event handler.
            *error = QString::fromStdString(e.what());
            return false;
        }
        stamp = &*read;
    }
    const CellBlock block = stamp->block();
    if (block.empty()) {
        *error = tr("the stamp is empty");
        return false;
    }
    tools_->setActive(selectTool_);
    const Canvas& cv = doc_.canvas();
    cell = {std::clamp(cell.x(), 0, cv.width() - 1), std::clamp(cell.y(), 0, cv.height() - 1)};
    view_->setCellCursor({cell.x(), cell.y()});
    // Whatever floats already lands first, in its own mode; the stamp floats
    // in transparent mode without changing the Paste Mode setting.
    selectTool_->floatBlock(block, cell, PasteMode::Transparent);
    view_->setFocus();
    return true;
}

QString MainWindow::libraryPathFor(const QString& name, bool* exists, QString* error) const {
    const std::optional<std::string> bytes = toCp437(name.trimmed());
    try {
        const library::StampTarget t =
            library::stampPathFor(library_->userDir(), bytes ? *bytes : name.trimmed().toStdString());
        if (exists)
            *exists = t.exists;
        return qpath(t.path);
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return {};
    }
}

QString MainWindow::saveSelectionToLibrary(const QString& name, const QString& category, const QStringList& tags,
                                           QString* error, bool replace) {
    CellBlock cells = selectTool_->floating() ? selectTool_->floatingBlock()
                                              : copyBlock(doc_.canvas(), view_->selection());
    if (cells.pixelLayer)
        // a float lifted off a pixel layer: its pixels as a paste onto blank
        // cells writes them (pasteCell), with the document's iCE setting
        for (Cell& c : cells.cells)
            if (const auto p = pasteCell(c, Cell{}, PasteMode::Normal, {false, doc_.ansi().ice}, true))
                c = *p;
    if (cells.empty()) {
        *error = tr("nothing is selected");
        return {};
    }
    const std::optional<std::string> title = toCp437(name.trimmed());
    if (!title) {
        *error = tr("the name holds a character CP437 has no glyph for");
        return {};
    }
    if (library_->userDir().empty()) {
        *error = tr("there is no user library directory");
        return {};
    }
    std::vector<std::string> tagList;
    for (const QString& t : tags)
        tagList.push_back(t.trimmed().toStdString());
    QString path;
    try {
        // Inside the error boundary: a library folder that can't be listed
        // is refused here. The target is written by its native path, never
        // a QString round trip.
        const library::StampTarget t = library::stampPathFor(library_->userDir(), *title);
        path = qpath(t.path);
        if (!replace && t.exists) {
            *error = tr("a stamp named \"%1\" already exists (%2)").arg(name.trimmed(), path);
            return {};
        }
        const library::StampFiles files =
            library::encodeStamp(*title, category.trimmed().toStdString(), tagList, cells, doc_.ansi().ice);
        library::writeStamp(t.path, files);
    } catch (const std::exception& e) {
        // LibraryError, or anything else encoding or writing threw: never
        // out of the menu action's handler.
        *error = QString::fromStdString(e.what());
        return {};
    }
    library_->rescan();
    return path;
}

void MainWindow::saveToLibrary() {
    if (!selectTool_->floating() && view_->selection().empty())
        return;
    StampDialog dialog(library_->categories(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    bool replace = false;
    QString error;
    const QString path = libraryPathFor(dialog.name(), &replace, &error);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Save to Library"), tr("Could not save the stamp:\n%1").arg(error));
        return;
    }
    if (replace) {
        if (QMessageBox::warning(this, tr("Save to Library"),
                                 tr("A stamp named \"%1\" is already in the library (%2).\n"
                                    "Do you want to replace it?")
                                     .arg(dialog.name(), path),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    const QString written = saveSelectionToLibrary(dialog.name(), dialog.category(), dialog.tags(), &error, replace);
    if (written.isEmpty())
        QMessageBox::warning(this, tr("Save to Library"), tr("Could not save the stamp:\n%1").arg(error));
    else
        statusBar()->showMessage(tr("Saved %1 to the library").arg(written), 3000);
}

void MainWindow::editLayerStyle(LayerId id) {
    const Layer* l = doc_.layers().find(id);
    if (!l || !(l->props.kind == LayerKind::Cells || l->props.kind == LayerKind::Pixels ||
                (l->props.kind == LayerKind::Text && l->props.text)))
        return;
    auto* dialog = new LayerStyleDialog(&doc_, id, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPresetDir(library_->userDir(), library_->categories());
    connect(dialog, &LayerStyleDialog::presetSaved, this, [this](const QString& path) {
        library_->rescan();
        statusBar()->showMessage(tr("Saved %1 to the library").arg(path), 3000);
    });
    dialog->open();
}

} // namespace bw
