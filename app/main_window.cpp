#include "main_window.h"

#include "canvas_view.h"
#include "irc_meter.h"
#include "select_tool.h"
#include "palette_widget.h"
#include "publish.h"

#include "blockwright/credits.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/version.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>

#include <stdexcept>
#include <utility>

namespace bw {

namespace {

const QString kFilter = QObject::tr(
    "Blockwright projects and ANSI art (*.bwp *.ans);;Blockwright projects (*.bwp);;ANSI art (*.ans);;All files (*)");

} // namespace

QString lossDescription(const AnsiOutput& out) {
    QStringList parts;
    if (!out.unwritable.empty())
        parts << QObject::tr("%n cell(s) hold a glyph with no CP437 code and will be written as '?'", nullptr,
                             int(out.unwritable.size()));
    // From the editor this is blink the file had and lost when it was opened
    // with iCE colours that this save still writes without (see
    // Document::encode). It claims nothing about the original file, which a
    // Save As to a new path leaves alone.
    if (out.blinkDropped)
        parts << QObject::tr(
            "%n cell(s) lost their blink when the file was opened with iCE colours, and this save doesn't put it back",
            nullptr, out.blinkDropped);
    return parts.join(QStringLiteral(";\n"));
}

QString saveWarning(const AnsiOutput& out, bool project) {
    const QString before = project ? QObject::tr("Before saving the project:") : QObject::tr("Before saving as .ans:");
    return QObject::tr("%1\n%2.\n\nSave anyway?").arg(before, lossDescription(out));
}

SavePath resolveSavePath(const QString& chosen, const QString& suffix) {
    if (!QFileInfo(chosen).suffix().isEmpty())
        return {chosen, false};
    const QString path = chosen + QLatin1Char('.') + suffix;
    return {path, QFileInfo::exists(path)};
}

void configureSaveDialog(QFileDialog& dialog, const QString& path, bool project) {
    const QString ansi = QObject::tr("ANSI art (*.ans)"), bwp = QObject::tr("Blockwright project (*.bwp)");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilters({ansi, bwp, QObject::tr("All files (*)")});
    dialog.selectNameFilter(project ? bwp : ansi);
    dialog.setDefaultSuffix(project ? QStringLiteral("bwp") : QStringLiteral("ans"));
    // The suffix follows the filter; "All files" keeps the last one.
    QObject::connect(&dialog, &QFileDialog::filterSelected, &dialog, [&dialog, ansi, bwp](const QString& f) {
        if (f == ansi)
            dialog.setDefaultSuffix(QStringLiteral("ans"));
        else if (f == bwp)
            dialog.setDefaultSuffix(QStringLiteral("bwp"));
    });
    if (!path.isEmpty()) {
        const QFileInfo fi(path);
        dialog.setDirectory(fi.absolutePath());
        dialog.selectFile(fi.fileName());
    }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    renderer_.setCanvas(&doc_.canvas(), glyphWidth(doc_.ansi()));

    view_ = new CanvasView(&doc_, &renderer_);
    view_->setObjectName(QStringLiteral("canvasView"));
    auto* scroll = new GutterScrollArea;
    scroll_ = scroll;
    scroll_->setWidget(view_);
    scroll_->setAlignment(Qt::AlignCenter);
    scroll_->setBackgroundRole(QPalette::Dark);
    setCentralWidget(scroll_);
    ircMeter_ = new IrcMeter(&doc_, view_, scroll);

    preview_ = new PreviewWidget(&renderer_);
    auto* previewScroll = new QScrollArea;
    previewScroll->setWidget(preview_);
    previewScroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    previewScroll->setBackgroundRole(QPalette::Dark);
    previewDock_ = new QDockWidget(tr("Preview (1x)"), this);
    previewDock_->setObjectName(QStringLiteral("previewDock"));
    // Always visible: it can move and float, but not close.
    previewDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    previewDock_->setWidget(previewScroll);
    addDockWidget(Qt::RightDockWidgetArea, previewDock_);

    palette_ = new PaletteWidget;
    palette_->setPen(view_->pen());
    paletteDock_ = new QDockWidget(tr("Palette"), this);
    paletteDock_->setObjectName(QStringLiteral("paletteDock"));
    paletteDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    paletteDock_->setWidget(palette_);
    addDockWidget(Qt::RightDockWidgetArea, paletteDock_);

    glyphSetLabel_ = new QLabel;
    glyphSetLabel_->setObjectName(QStringLiteral("glyphSetLabel"));
    statusBar()->addPermanentWidget(glyphSetLabel_);
    status_ = new QLabel;
    statusBar()->addPermanentWidget(status_);

    connect(&doc_, &Document::cellsChanged, this, [this](const QRegion& cells) {
        renderer_.markDirty(cells);
        view_->cellsChanged(cells);
        preview_->cellsChanged(cells);
    });
    connect(&doc_, &Document::reset, this, &MainWindow::canvasReset);
    connect(&doc_, &Document::formatChanged, this, &MainWindow::formatChanged);
    connect(&doc_, &Document::modifiedChanged, this, &MainWindow::updateTitle);
    connect(&doc_, &Document::pathChanged, this, &MainWindow::updateTitle);
    connect(view_, &CanvasView::cursorMoved, this, [this] {
        const QRect r = view_->toWidget(view_->cellRect(view_->cellCursor()));
        scroll_->ensureVisible(r.center().x(), r.center().y(), r.width(), r.height());
        updateStatus();
    });
    connect(view_, &CanvasView::zoomChanged, this, &MainWindow::updateStatus);
    connect(palette_, &PaletteWidget::foregroundPicked, view_, &CanvasView::setForeground);
    connect(palette_, &PaletteWidget::backgroundPicked, view_, &CanvasView::setBackground);
    connect(view_, &CanvasView::penChanged, palette_, &PaletteWidget::setPen);
    connect(view_, &CanvasView::glyphSetChanged, this, &MainWindow::updateGlyphSet);
    connect(view_, &CanvasView::glyphRejected, this, [this](const QString& text) {
        statusBar()->showMessage(tr("%1 has no CP437 glyph; not typed").arg(text), 3000);
    });

    createActions();
    createLibrary();
    createDocks();
    updateTitle();
    updateStatus();
    updateGlyphSet();
    resize(1400, 900);
    restoreLayout();
    view_->setFocus();
}

void MainWindow::createActions() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    QAction* a = file->addAction(tr("&New"), this, &MainWindow::newFile);
    a->setShortcut(QKeySequence::New);
    a = file->addAction(tr("&Open..."), this, &MainWindow::open);
    a->setShortcut(QKeySequence::Open);
    a = file->addAction(tr("Open Project Di&rectory..."), this, &MainWindow::openDirectory);
    a->setObjectName(QStringLiteral("openDirectory"));
    a = file->addAction(tr("&Save"), this, &MainWindow::save);
    a->setShortcut(QKeySequence::Save);
    a = file->addAction(tr("Save &As..."), this, &MainWindow::saveAs);
    a->setShortcut(QKeySequence::SaveAs);
    createProjectActions(file);
    file->addSeparator();
    a = file->addAction(tr("SA&UCE..."), this, &MainWindow::editSauce);
    a->setObjectName(QStringLiteral("sauce"));
    file->addSeparator();
    (new Publisher(this))->addActions(file);
    file->addSeparator();
    a = file->addAction(tr("&Quit"), this, &QWidget::close);
    a->setShortcut(QKeySequence::Quit);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    a = doc_.undoStack()->createUndoAction(this, tr("&Undo"));
    a->setObjectName(QStringLiteral("undo"));
    a->setShortcuts(QKeySequence::Undo);
    edit->addAction(a);
    a = doc_.undoStack()->createRedoAction(this, tr("&Redo"));
    a->setObjectName(QStringLiteral("redo"));
    a->setShortcuts(QKeySequence::Redo);
    edit->addAction(a);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    a = view->addAction(tr("Zoom &In"), this, &MainWindow::zoomIn);
    a->setObjectName(QStringLiteral("zoomIn"));
    a->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)});
    a = view->addAction(tr("Zoom &Out"), this, &MainWindow::zoomOut);
    a->setObjectName(QStringLiteral("zoomOut"));
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    a = view->addAction(tr("&Actual Size"), this, &MainWindow::zoomReset);
    a->setObjectName(QStringLiteral("zoomReset"));
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    createFormatActions(view);
    createIrcActions(file, view);

    // Colours and glyph sets: Moebius's keys.
    const auto add = [this](QMenu* m, const QString& text, const char* name, QKeySequence key, auto slot) {
        QAction* act = m->addAction(text, this, slot);
        act->setObjectName(QString::fromLatin1(name));
        act->setShortcut(key);
        return act;
    };
    QMenu* colours = menuBar()->addMenu(tr("&Colours"));
    add(colours, tr("&Previous Foreground"), "previousForeground", QKeySequence(Qt::CTRL | Qt::Key_Up),
        [this] { view_->setForeground(view_->pen().fg.paletteIndex() + 15); });
    add(colours, tr("&Next Foreground"), "nextForeground", QKeySequence(Qt::CTRL | Qt::Key_Down),
        [this] { view_->setForeground(view_->pen().fg.paletteIndex() + 1); });
    add(colours, tr("P&revious Background"), "previousBackground", QKeySequence(Qt::CTRL | Qt::Key_Left),
        [this] { view_->setBackground(view_->pen().bg.paletteIndex() + 15); });
    add(colours, tr("N&ext Background"), "nextBackground", QKeySequence(Qt::CTRL | Qt::Key_Right),
        [this] { view_->setBackground(view_->pen().bg.paletteIndex() + 1); });
    colours->addSeparator();
    add(colours, tr("&Default Colours"), "defaultColours", QKeySequence(Qt::CTRL | Qt::Key_D),
        [this] { view_->setPen(Pen{}); });
    add(colours, tr("&Swap Colours"), "swapColours", QKeySequence(),
        [this] { view_->setPen(Pen{view_->pen().bg, view_->pen().fg}); });

    QMenu* glyphs = menuBar()->addMenu(tr("&Glyphs"));
    add(glyphs, tr("&Previous Set"), "previousGlyphSet", QKeySequence(Qt::CTRL | Qt::Key_Comma),
        [this] { view_->setGlyphSet(view_->glyphSet() - 1); });
    add(glyphs, tr("&Next Set"), "nextGlyphSet", QKeySequence(Qt::CTRL | Qt::Key_Period),
        [this] { view_->setGlyphSet(view_->glyphSet() + 1); });
    add(glyphs, tr("&Default Set"), "defaultGlyphSet", QKeySequence(Qt::CTRL | Qt::Key_Slash),
        [this] { view_->setGlyphSet(kDefaultGlyphSet); });
    glyphs->addSeparator();
    // Ctrl+Shift+F1-F10 pick sets 1-10; 11-16 are reached from this menu or by
    // stepping. Not Moebius's Alt+F-keys: desktops take those (Alt+F4 closes
    // the window), as they do Ctrl+F-keys (KDE and Xfce desktop switching) and
    // Ctrl+Alt+F-keys (Linux VT switching).
    auto* group = new QActionGroup(this);
    for (int i = 0; i < kGlyphSetCount; ++i) {
        QString shown;
        for (const char32_t ch : glyphSet(i))
            shown += QString::fromUcs4(&ch, 1);
        const QKeySequence key =
            i < kGlyphSetKeys ? QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key(Qt::Key_F1 + i)) : QKeySequence();
        QAction* set = add(glyphs, tr("Set %1: %2").arg(i + 1).arg(shown), "", key,
                           [this, i] { view_->setGlyphSet(i); });
        set->setObjectName(QStringLiteral("glyphSet%1").arg(i + 1));
        set->setCheckable(true);
        group->addAction(set);
        glyphSetActions_ << set;
    }

    createEditingActions(file, edit, view);
    // Filled in by createDocks.
    layerMenu_ = menuBar()->addMenu(tr("&Layer"));
    windowMenu_ = menuBar()->addMenu(tr("&Window"));

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Blockwright"), this, [this] {
        QMessageBox::about(this, tr("About Blockwright"),
                           tr("<p><b>Blockwright</b> %1 (%2)</p><p>ANSI and IRC art editor. GPL-3.0-or-later.</p>"
                              "<p>%3</p>")
                               .arg(QStringLiteral(BLOCKWRIGHT_VERSION), QStringLiteral(BLOCKWRIGHT_GIT_COMMIT),
                                    QString::fromUtf8(kFontCredit).toHtmlEscaped().replace('\n', ' ')));
    });
    help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
}

void MainWindow::updateGlyphSet() {
    const int set = view_->glyphSet();
    QStringList glyphs, tips;
    for (int k = 0; k < kGlyphSetKeys; ++k) {
        const char32_t ch = glyphSetGlyph(set, k);
        glyphs << QString::fromUcs4(&ch, 1);
        tips << tr("F%1 %2").arg(k + 1).arg(glyphs.back());
    }
    glyphSetLabel_->setText(tr("F-keys set %1: %2").arg(set + 1).arg(glyphs.join(QLatin1Char(' '))));
    const auto keys = [this](const char* name) {
        return findChild<QAction*>(QString::fromLatin1(name))->shortcut().toString(QKeySequence::NativeText);
    };
    tips << QString()
         << tr("%1 and %2 step through the sets, %3 goes back to the default")
                .arg(keys("previousGlyphSet"), keys("nextGlyphSet"), keys("defaultGlyphSet"))
         << tr("%1 to %2 pick sets 1-%3; every set is in the Glyphs menu")
                .arg(keys("glyphSet1"), keys("glyphSet10"))
                .arg(kGlyphSetKeys);
    glyphSetLabel_->setToolTip(tips.join(QLatin1Char('\n')));
    if (set < glyphSetActions_.size())
        glyphSetActions_[set]->setChecked(true);
}

void MainWindow::canvasReset() {
    // openFile has already shown the renderer this canvas, and a blank one
    // always fits, so this does not throw.
    renderer_.setCanvas(&doc_.canvas(), glyphWidth(doc_.ansi()));
    renderer_.setIceColours(doc_.ansi().ice);
    syncFormatActions();
    view_->canvasReset();
    preview_->canvasReset();
    view_->setCellCursor({0, 0});
    updateStatus();
}

bool MainWindow::openFile(const QString& path, QString* error) {
    if (project::isProject(QFile::encodeName(path).toStdString()) || fileFormFor(path) != FileForm::Ansi)
        return openProject(path, error);
    AnsiDocument candidate;
    if (!Document::read(path, &candidate, error))
        return false;
    // Let the renderer take the new canvas before the document does, so a
    // canvas too large to display leaves the open one as it was. setCanvas
    // changes nothing when it throws, and canvasReset (from replace) points
    // it at the document's copy before anything draws.
    try {
        renderer_.setCanvas(&candidate.canvas, glyphWidth(candidate));
    } catch (const std::exception&) {
        *error = tr("the canvas is too large to display");
        return false;
    }
    doc_.replace(std::move(candidate), path);
    return true;
}

bool MainWindow::saveFile(const QString& path, QString* error, bool allowLossy) {
    const FileForm form = path == doc_.path() ? doc_.form() : fileFormFor(path);
    return writeFile(path, form, error, allowLossy);
}

void MainWindow::newFile() {
    if (maybeSave())
        doc_.clear();
}

void MainWindow::open() {
    if (!maybeSave())
        return;
    const QString dir = doc_.path().isEmpty() ? QString() : QFileInfo(doc_.path()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open"), dir, kFilter);
    if (!path.isEmpty())
        openInteractive(path);
}

void MainWindow::openInteractive(const QString& path) {
    QString error;
    if (!openFile(path, &error))
        QMessageBox::warning(this, tr("Open"), tr("Could not open %1:\n%2").arg(path, error));
}

bool MainWindow::save() { return doc_.path().isEmpty() ? saveAs() : saveInteractive(doc_.path(), doc_.form()); }

bool MainWindow::saveAs() {
    return doc_.form() == FileForm::ProjectDirectory ? saveAsDirectory() : saveAsFile();
}

bool MainWindow::saveAsFile() {
    QFileDialog dialog(this, tr("Save As"));
    configureSaveDialog(dialog, doc_.path(), doc_.form() != FileForm::Ansi);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return false;
    // A platform dialog may not apply the default suffix; then the name
    // written is one the dialog never checked.
    const SavePath p = resolveSavePath(dialog.selectedFiles().constFirst(), dialog.defaultSuffix());
    if (p.confirmOverwrite &&
        QMessageBox::warning(this, tr("Save As"), tr("%1 already exists.\nDo you want to replace it?").arg(p.path),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    return saveInteractive(p.path, fileFormFor(p.path));
}

bool MainWindow::saveInteractive(const QString& path, FileForm form) {
    view_->finishTool();
    const AnsiOutput out = saveLoss(form);
    if (out.lossy()) {
        const auto answer = QMessageBox::warning(
            this, tr("Save"), saveWarning(out, form != FileForm::Ansi),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save)
            return false;
    }
    QString error;
    if (!(form == FileForm::Ansi ? doc_.save(path, out, &error) : doc_.saveProject(path, form, &error))) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(path, error));
        return false;
    }
    if (form == FileForm::Ansi && doc_.path() != path)
        statusBar()->showMessage(tr("Exported %1 flattened; the document is still %2").arg(path, doc_.path()), 5000);
    else if (doc_.isModified())
        statusBar()->showMessage(tr("Saved %1 flattened; only a project keeps the layers").arg(path), 5000);
    else
        statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
    return true;
}

void MainWindow::zoomIn() { view_->setZoom(view_->zoom() + 1); }
void MainWindow::zoomOut() { view_->setZoom(view_->zoom() - 1); }
void MainWindow::zoomReset() { view_->setZoom(1); }

bool MainWindow::maybeSave() {
    // A floating selection is an unsaved change: stamped, it marks the
    // document modified and is saved with it.
    view_->finishTool();
    if (!doc_.isModified())
        return true;
    QString text = tr("The canvas has unsaved changes.");
    // Save writes the .ans, which can't hold the layers: they stay open.
    const bool flatLoses = doc_.form() == FileForm::Ansi && !doc_.path().isEmpty() && !doc_.flatHoldsAll();
    if (flatLoses)
        text += tr("\nSave writes %1 flattened and keeps the layers open; only a project saves them.")
                    .arg(QFileInfo(doc_.path()).fileName());
    const auto answer = QMessageBox::warning(this, tr("Blockwright"), text,
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                             QMessageBox::Save);
    // A save that leaves the document modified didn't keep all of it.
    if (answer == QMessageBox::Save)
        return save() && !doc_.isModified();
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (maybeSave()) {
        saveLayout();
        e->accept();
    } else {
        e->ignore();
    }
}

void MainWindow::updateTitle() {
    const QString name = doc_.path().isEmpty() ? tr("untitled.ans") : QFileInfo(doc_.path()).fileName();
    setWindowTitle(name + QStringLiteral("[*] - Blockwright"));
    setWindowFilePath(doc_.path());
    setWindowModified(doc_.isModified());
}

void MainWindow::updateStatus() {
    const Canvas& cv = doc_.canvas();
    const Cursor c = view_->cellCursor();
    status_->setText(tr("%1, %2  |  %3x%4  |  %5 px  |  %6x")
                         .arg(c.x)
                         .arg(c.y)
                         .arg(cv.width())
                         .arg(cv.height())
                         .arg(renderer_.cellSize().width())
                         .arg(view_->zoom()));
}

} // namespace bw
