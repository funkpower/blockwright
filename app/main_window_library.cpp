// The Library parts of the main window (PLAN §12): the Library dock, placing
// a stamp on the canvas, applying a text style or dropping one as a new text
// layer, Selection > Save to Library... and Selection > Save as Frame
// Style....

#include "main_window.h"

#include "canvas_view.h"
#include "frame_style_dialog.h"
#include "frame_tool.h"
#include "layer_style_dialog.h"
#include "library_dock.h"
#include "sauce_dialog.h"
#include "select_tool.h"
#include "text_tool.h"
#include "tool.h"

#include "blockwright/brushes/drawing.h"
#include "blockwright/glyph.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/selection.h"

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
#include <vector>

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
    // The Frame tool lists the frame styles the libraries hold after its own.
    if (auto* frame = qobject_cast<FrameTool*>(tools_->find(QStringLiteral("frame")))) {
        connect(library_, &LibraryDock::rescanned, frame, [this, frame] { frame->setLibraryStyles(library_->frameStyles()); });
    }
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

    // A text style's key is looked up the same way.
    connect(library_, &LibraryDock::textStyleActivated, this, [this](const QByteArray& key) {
        const library::TextStyleEntry* style = library_->findStyleKey(key);
        QString error = tr("not a text style in the Library");
        if (!style || !applyTextStyle(*style, &error))
            statusBar()->showMessage(tr("Text style not applied: %1").arg(error), 5000);
    });
    connect(drops, &StampDropFilter::styleDropped, this, [this](const QByteArray& key, QPoint cell) {
        const library::TextStyleEntry* style = library_->findStyleKey(key);
        QString error = tr("not a text style in the Library");
        if (!style || !newStyledText(*style, cell, &error))
            statusBar()->showMessage(tr("No text layer made: %1").arg(error), 5000);
    });
}

namespace {

// Why `entry` can't be used while the document's iCE setting is `ice`.
QString iceProblem(const library::TextStyleEntry& entry, bool ice) {
    return entry.iceRequired && !ice
               ? MainWindow::tr("%1 needs iCE colours, which are off (View > iCE Colours)").arg(QString::fromStdString(entry.name))
               : QString();
}

// The glyphs of `block` a Pixels layer can't hold, each once in reading
// order: what fitLayerEdit refuses when the stamp lands there in transparent
// mode (its blank cells and holes write nothing, a half pixel is held).
QString unheldOnPixels(const CellBlock& block) {
    QString out;
    std::vector<Glyph> seen;
    for (const Cell& c : block.cells) {
        if (c.transparent || isBlank(c) || c.ch.isContinuation() || halfBlocks(c, true))
            continue;
        if (std::find(seen.begin(), seen.end(), c.ch) != seen.end())
            continue;
        seen.push_back(c.ch);
        for (const char32_t cp : codePoints(c.ch))
            out += QString::fromUcs4(&cp, 1);
    }
    return out;
}

// Why the active layer can't take `block` as a stamp (Document::editable and
// fitLayerEdit's reasons), or nothing when it can.
QString stampProblem(const Document& doc, const CellBlock& block) {
    if (QString why = doc.editProblem(); !why.isEmpty())
        return why;
    if (doc.active()->props.kind != LayerKind::Pixels)
        return {};
    // ▌ ▐ alone float: a quarter turn makes them ▀ ▄, and the landing
    // refuses them if they are still there (SelectTool::stamp). Any other
    // glyph the layer can't hold is refused here, with them.
    if (const QString unheld = unheldOnPixels(block); !unheld.isEmpty()) {
        const auto cps = unheld.toUcs4();
        if (!std::all_of(cps.begin(), cps.end(), [](char32_t cp) { return pixelsHoldTurned(cp); }))
            return MainWindow::tr("a Pixels layer holds no %1").arg(unheld);
    }
    return {};
}

} // namespace

bool MainWindow::applyTextStyle(const library::TextStyleEntry& entry, QString* error) {
    if (const QString why = iceProblem(entry, doc_.ansi().ice); !why.isEmpty()) {
        *error = why;
        return false;
    }
    const Layer* l = doc_.active();
    if (!l || !(l->props.kind == LayerKind::Cells || l->props.kind == LayerKind::Pixels ||
                (l->props.kind == LayerKind::Text && l->props.text))) {
        *error = tr("a text style goes on a live text, cells or pixels layer");
        return false;
    }
    LayerProperties props = l->props;
    props.style = entry.style;
    if (props.style == l->props.style) {
        *error = tr("the layer has this style already");
        return false;
    }
    if (const auto problem = layerStyleProblem(props, doc_.fontSources())) {
        *error = QString::fromStdString(*problem);
        return false;
    }
    if (!doc_.setLayerProperties(l->props.id, std::move(props),
                                 tr("Text Style %1").arg(QString::fromStdString(entry.name)))) {
        *error = tr("the layer can't take it");
        return false;
    }
    return true;
}

LayerId MainWindow::newStyledText(const library::TextStyleEntry& entry, QPoint cell, QString* error) {
    if (const QString why = iceProblem(entry, doc_.ansi().ice); !why.isEmpty()) {
        *error = why;
        return 0;
    }
    auto* text = qobject_cast<TextTool*>(tools_->find(QStringLiteral("text")));
    if (!text) {
        *error = tr("there is no Text tool");
        return 0;
    }
    const Canvas& cv = doc_.canvas();
    cell = {std::clamp(cell.x(), 0, cv.width() - 1), std::clamp(cell.y(), 0, cv.height() - 1)};
    tools_->setActive(text);
    if (tools_->active() != text) {
        // the tool before kept what it couldn't land (a floating selection)
        *error = tr("the floating selection can't land: %1").arg(tools_->active()->unlanded());
        return 0;
    }
    const LayerId id = text->placeStyled(cell, U"Text", entry.style);
    if (!id) {
        *error = tr("the text can't be drawn in %1").arg(QString::fromStdString(entry.name));
        return 0;
    }
    view_->setFocus();
    return id;
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
    // The tool before lands what it holds first: the Text tool's new layer,
    // emptied, goes, and the layer under it is the one the stamp goes on.
    tools_->setActive(selectTool_);
    // A layer that can't take the stamp is refused here, before anything
    // floats: the float would show nothing and land nothing. A block that
    // already floats stays floating.
    if (const QString why = stampProblem(doc_, block); !why.isEmpty()) {
        *error = why;
        return false;
    }
    const Canvas& cv = doc_.canvas();
    cell = {std::clamp(cell.x(), 0, cv.width() - 1), std::clamp(cell.y(), 0, cv.height() - 1)};
    view_->setCellCursor({cell.x(), cell.y()});
    // Whatever floats already lands first, in its own mode; the stamp floats
    // in transparent mode without changing the Paste Mode setting. One that
    // can't land stays floating, and this stamp doesn't float.
    if (!selectTool_->floatBlock(block, cell, PasteMode::Transparent, stamp->meta.slice, qpath(path))) {
        *error = tr("the floating selection can't land: %1").arg(selectTool_->unlanded());
        return false;
    }
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

CellBlock MainWindow::selectionCells() const {
    CellBlock cells = selectTool_->floating() ? selectTool_->floatingBlock()
                                              : copyBlock(doc_.canvas(), view_->selection());
    if (cells.pixelLayer)
        // a float lifted off a pixel layer: its pixels as a paste onto blank
        // cells writes them (pasteCell), with the document's iCE setting
        for (Cell& c : cells.cells)
            if (const auto p = pasteCell(c, Cell{}, PasteMode::Normal, {false, doc_.ansi().ice}, true))
                c = *p;
    return cells;
}

QString MainWindow::saveSelectionToLibrary(const QString& name, const QString& category, const QStringList& tags,
                                           QString* error, bool replace, const std::optional<StampSlice>& slice) {
    const CellBlock cells = selectionCells();
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
            library::encodeStamp(*title, category.trimmed().toStdString(), tagList, cells, doc_.ansi().ice, slice);
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
    dialog.setSlice(selectTool_->floatingSlice());  // a placed frame, saved again
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
    const QString written = saveSelectionToLibrary(dialog.name(), dialog.category(), dialog.tags(), &error, replace, dialog.slice());
    if (written.isEmpty())
        QMessageBox::warning(this, tr("Save to Library"), tr("Could not save the stamp:\n%1").arg(error));
    else
        statusBar()->showMessage(tr("Saved %1 to the library").arg(written), 3000);
}

namespace {

Canvas canvasOf(const CellBlock& block) {
    Canvas c(std::max(1, block.width), std::max(1, block.height));
    for (int y = 0; y < block.height; ++y)
        for (int x = 0; x < block.width; ++x)
            c.at(x, y) = block.at(x, y);
    return c;
}

} // namespace

QString MainWindow::saveSelectionAsFrameStyle(const QString& name, int left, int right, int top, int bottom,
                                              bool fill, bool keepBackground, QString* error) {
    const CellBlock cells = selectionCells();
    if (cells.empty()) {
        *error = tr("nothing is selected");
        return {};
    }
    if (name.trimmed().isEmpty()) {
        *error = tr("the frame style has no name");
        return {};
    }
    if (library_->userDir().empty()) {
        *error = tr("there is no user library directory");
        return {};
    }
    std::string why;
    const std::optional<FrameStyle> style = frameStyleFromCells(canvasOf(cells), std::string(),
                                                                name.trimmed().toStdString(), left, right, top,
                                                                bottom, fill, keepBackground, &why);
    if (!style) {
        *error = QString::fromStdString(why);
        return {};
    }
    std::filesystem::path written;
    try {
        written = library::writeFrameStyle(library_->userDir(), *style);
    } catch (const std::exception& e) {
        // LibraryError, or anything else writing threw: never out of the
        // menu action's handler.
        *error = QString::fromStdString(e.what());
        return {};
    }
    library_->rescan();
    // the dock names it by its path (LibraryDock::frameStyles)
    if (auto* frame = qobject_cast<FrameTool*>(tools_->find(QStringLiteral("frame"))))
        frame->setStyle(frameStyleId(written));
    return qpath(written);
}

void MainWindow::saveFrameStyle() {
    if (!selectTool_->floating() && view_->selection().empty())
        return;
    const CellBlock cells = selectionCells();
    if (cells.empty())
        return;
    FrameStyleDialog dialog(canvasOf(cells), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    const QString written = saveSelectionAsFrameStyle(dialog.name(), dialog.left(), dialog.right(), dialog.top(),
                                                      dialog.bottom(), dialog.fill(), dialog.keepBackground(), &error);
    if (written.isEmpty())
        QMessageBox::warning(this, tr("Save as Frame Style"), tr("Could not save the frame style:\n%1").arg(error));
    else
        statusBar()->showMessage(tr("Saved %1 to the library; the Frame tool lists it").arg(written), 3000);
}

void MainWindow::editLayerStyle(LayerId id) {
    const Layer* l = doc_.layers().find(id);
    if (!l || !(l->props.kind == LayerKind::Cells || l->props.kind == LayerKind::Pixels ||
                (l->props.kind == LayerKind::Text && l->props.text)))
        return;
    auto* dialog = new LayerStyleDialog(&doc_, id, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPresetDir(library_->userDir(), library_->categories());
    std::vector<library::TextStyleEntry> presets;
    for (const LibraryDock::StyleEntry& e : library_->styles())
        presets.push_back(e.style);
    dialog->setPresets(std::move(presets));
    connect(dialog, &LayerStyleDialog::presetSaved, this, [this](const QString& path) {
        library_->rescan();
        statusBar()->showMessage(tr("Saved %1 to the library").arg(path), 3000);
    });
    dialog->open();
}

} // namespace bw
