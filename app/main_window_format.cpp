// The MainWindow parts for the document format: File > SAUCE..., the
// View menu's iCE colours and letter-spacing toggles, Format > Target, and
// Edit > Preferences... (the fonts folder TheDraw faces come from, the
// target new documents take).

#include "main_window.h"

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
