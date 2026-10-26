// The MainWindow parts for the document format: File > SAUCE..., the
// View menu's iCE colours and letter-spacing toggles, and Edit > Preferences...
// (the fonts folder TheDraw faces come from).

#include "main_window.h"

#include "canvas_view.h"
#include "preferences.h"
#include "sauce_dialog.h"

#include <QAction>
#include <QMenu>
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

void MainWindow::syncFormatActions() {
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
    if (dialog.exec() != QDialog::Accepted)
        return;
    setFontsFolderSetting(dialog.fontsFolder());
    doc_.setFontsFolder(dialog.catalogue());
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
