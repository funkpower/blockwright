// The MainWindow parts for the document format: File > SAUCE..., the
// View menu's iCE colours and letter-spacing toggles, Format > Canvas
// Size..., Format > Target, and Edit > Preferences... (the fonts folder
// TheDraw faces come from, the target new documents take).

#include "main_window.h"

#include "canvas_size_dialog.h"
#include "canvas_view.h"
#include "palette_widget.h"
#include "library_dock.h"
#include "preferences.h"
#include "shortcut_settings.h"
#include "sauce_dialog.h"

#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>

#include <stdexcept>

namespace bw {

void MainWindow::createFormatActions(QMenu* view) {
    view->addSeparator();
    iceAction_ = view->addAction(tr("i&CE Colours"));
    iceAction_->setObjectName(QStringLiteral("iceColours"));
    iceAction_->setCheckable(true);
    iceAction_->setToolTip(tr("Bright backgrounds as 16 colours; off, they blink on the dark colour"));
    connect(iceAction_, &QAction::triggered, this, &MainWindow::setIceColours);
    ninePxAction_ = view->addAction(tr("&9 px Letter Spacing"));
    ninePxAction_->setObjectName(QStringLiteral("ninePx"));
    ninePxAction_->setCheckable(true);
    ninePxAction_->setToolTip(tr("9 px cells, line-drawing glyphs 0xC0-0xDF joined as on VGA; off, 8 px"));
    connect(ninePxAction_, &QAction::triggered, this, &MainWindow::setNinePx);
    syncFormatActions();
}

void MainWindow::createTargetActions(QMenu* format) {
    // No key of its own: Photoshop's Ctrl+Alt+C is AltGr+C on Windows, which
    // types (PLAN §4, keyboard mode), and Preferences > Shortcuts can give
    // it one.
    QAction* size = format->addAction(tr("Canvas &Size..."), this, &MainWindow::canvasSize);
    size->setObjectName(QStringLiteral("canvasSize"));
    size->setStatusTip(tr("Make the canvas another size, keeping it at a side, a corner or the centre"));
    size->setToolTip(size->statusTip());
    format->addSeparator();
    QMenu* target = format->addMenu(tr("&Target"));
    target->setObjectName(QStringLiteral("targetMenu"));
    auto* group = new QActionGroup(this);
    const auto add = [&](const QString& text, const char* name, Target t, const QString& tip) {
        QAction* a = target->addAction(text);
        a->setObjectName(QString::fromLatin1(name));
        a->setCheckable(true);
        a->setToolTip(tip);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { doc_.setTarget(t); });
        return a;
    };
    ansiTargetAction_ = add(tr("&ANSI (.ans)"), "targetAnsi", Target::Ansi,
                            tr("The 16 colours and the glyphs an .ans holds"));
    ircTargetAction_ = add(tr("&IRC"), "targetIrc", Target::Irc,
                           tr("mIRC's 99 colours; saving as .ans, XBIN or BIN reports what it can't hold"));
    updateTarget();
}

void MainWindow::updateTarget() {
    const Target t = doc_.target();
    if (targetLabel_)
        targetLabel_->setText(t == Target::Irc ? tr("IRC") : tr("ANSI"));
    if (ansiTargetAction_) {
        ansiTargetAction_->setChecked(t == Target::Ansi);
        ircTargetAction_->setChecked(t == Target::Irc);
    }
    if (palette_)
        palette_->setTarget(t);
    if (formatBar_)
        formatBar_->setVisible(t == Target::Irc);
}

std::uint8_t MainWindow::formatBit(const QAction* a) const {
    static constexpr std::uint8_t bits[] = {attr::Bold, attr::Italic, attr::Underline};
    for (std::size_t i = 0; i < formatActions_.size(); ++i)
        if (formatActions_[i] == a)
            return bits[i];
    return 0;
}

void MainWindow::stepColour(bool foreground, int step) {
    const Target t = doc_.target();
    const int n = swatchCount(t);
    const Color& c = foreground ? view_->pen().fg : view_->pen().bg;
    const int next = ((swatchOf(c, t) % n) + step + n) % n;
    if (foreground)
        view_->setForeground(next);
    else
        view_->setBackground(next);
}

void MainWindow::syncFormatActions() {
    // The Library shows a style that needs iCE disabled while it is off.
    if (library_)
        library_->setIceColours(doc_.ansi().ice);
    if (!iceAction_)
        return;
    iceAction_->setChecked(doc_.ansi().ice);
    ninePxAction_->setChecked(glyphWidth(doc_.ansi()) == 9);
}

void MainWindow::editSauce() {
    Sauce shown = doc_.ansi().sauce.value_or(Sauce::fresh(doc_.canvas().width(), doc_.canvas().height()));
    SauceDialog dialog(shown, this);
    if (dialog.exec() == QDialog::Accepted && dialog.problem().isEmpty())
        doc_.setSauceFields(dialog.fields());
}

void MainWindow::editPreferences() {
    // scanned afresh, so OK on the same folder picks up fonts added since
    PreferencesDialog dialog(fontsFolderSetting(), nullptr, this);
    auto* keys = new ShortcutsEditor(this);
    dialog.addPage(keys, tr("Shortcuts"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    keys->apply();
    setDefaultTargetSetting(dialog.defaultTarget());
    if (dialog.fontsFolderChanged())
        setFontsFolderSetting(dialog.fontsFolder());
    doc_.scanFontsFolder(dialog.fontsFolder());
}

void MainWindow::setIceColours(bool on) {
    doc_.setIce(on);
    syncFormatActions();
}

void MainWindow::setNinePx(bool on) {
    if (on && glyphWidth(doc_.ansi()) != 9) {
        // A canvas that fits at 8 px can be too large at 9: try it first, so
        // the document only changes if it can be shown.
        try {
            renderer_.setCanvas(&doc_.canvas(), 9);
        } catch (const std::exception&) {
            statusBar()->showMessage(tr("The canvas is too large to display at 9 px"), 5000);
            syncFormatActions();
            return;
        }
    }
    // formatChanged (from the push) sizes the views; if nothing was pushed
    // the renderer still goes back to the document's width
    if (!doc_.setLetterSpacing(on ? 9 : 8))
        formatChanged();
}

void MainWindow::canvasSize() {
    // what the tool holds lands first, so the dialog starts at the size and
    // the step moves what the canvas shows
    view_->finishTool();
    CanvasSizeDialog dialog(QSize(doc_.canvas().width(), doc_.canvas().height()), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!resizeCanvas(dialog.canvasSize(), dialog.anchor(), &error))
        QMessageBox::warning(this, tr("Canvas Size"), tr("The canvas can't be made that size: %1.").arg(error));
}

bool MainWindow::resizeCanvas(QSize size, Anchor anchor, QString* error) {
    view_->finishTool();
    if (const auto problem = canvasSizeProblem(doc_.layers(), size.width(), size.height())) {
        *error = QString::fromStdString(*problem);
        return false;
    }
    if (size == QSize(doc_.canvas().width(), doc_.canvas().height()))
        return true;
    // As newCanvas: the renderer takes the size first, so one too large to
    // display leaves the document as it was; the step's canvasResized
    // points it at the document's canvas.
    const int gw = glyphWidth(doc_.ansi());
    const Canvas candidate(size.width(), size.height());
    try {
        renderer_.setCanvas(&candidate, gw);
    } catch (const std::exception&) {
        renderer_.setCanvas(&doc_.canvas(), gw);
        *error = tr("the canvas is too large to display");
        return false;
    }
    if (!doc_.resizeCanvas(size.width(), size.height(), anchor, view_->selection(), view_->cellCursor(), error)) {
        renderer_.setCanvas(&doc_.canvas(), gw);
        return false;
    }
    return true;
}

void MainWindow::canvasResized() {
    // Letter spacing is an undo step too, so every state an undo or redo
    // comes back to was shown before at its own spacing. Should one still
    // not fit at 9 px it is shown at 8, which every canvas within the
    // limits fits.
    const int gw = glyphWidth(doc_.ansi());
    try {
        renderer_.setCanvas(&doc_.canvas(), gw);
    } catch (const std::exception&) {
        try {
            renderer_.setCanvas(&doc_.canvas(), 8);
            statusBar()->showMessage(tr("The canvas is too large to display at 9 px; shown at 8 px"), 5000);
        } catch (const std::exception&) {
        }
    }
    view_->canvasReset();
    preview_->canvasReset();
    updateStatus();
}

void MainWindow::formatChanged() {
    renderer_.setIceColours(doc_.ansi().ice);
    const int gw = glyphWidth(doc_.ansi());
    if (renderer_.cellSize().width() != gw) {
        // 9 px was tried before the change was pushed, and 8 px is smaller
        // than what is shown, so this fits; if not, the old width stays.
        try {
            renderer_.setCanvas(&doc_.canvas(), gw);
        } catch (const std::exception&) {
        }
    }
    view_->canvasReset();
    preview_->canvasReset();
    syncFormatActions();
    updateStatus();
}

} // namespace bw
