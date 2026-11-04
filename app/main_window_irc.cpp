// The MainWindow parts for IRC (PLAN §8): File > Export IRC Text... and
// the View > IRC menu, the row meter, the document's budget and the
// encoder options it measures with.

#include "main_window.h"

#include "canvas_view.h"
#include "irc_meter.h"
#include "plural.h"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QSaveFile>
#include <QStatusBar>

namespace bw {

namespace {

std::vector<std::string> texts(const std::vector<irc::Line>& lines) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const irc::Line& l : lines)
        out.push_back(l.text);
    return out;
}

} // namespace

void MainWindow::createIrcActions(QMenu* file, QMenu* view) {
    QAction* a = new QAction(tr("Export &IRC Text..."), this);
    a->setObjectName(QStringLiteral("exportIrc"));
    a->setToolTip(tr("The canvas as IRC lines, one message each, as the row meter measures them"));
    connect(a, &QAction::triggered, this, &MainWindow::exportIrc);
    // Above File > Publish to Library. Publish writes its own lines: plain,
    // without fit or pad, against the EFnet / WeeChat budget (PLAN §8).
    file->insertAction(findChild<QAction*>(QStringLiteral("publishToLibrary")), a);

    QMenu* m = view->addMenu(tr("I&RC"));
    m->setObjectName(QStringLiteral("ircMenu"));
    const auto toggle = [&](const QString& text, const char* name, bool on, auto slot) {
        QAction* t = m->addAction(text);
        t->setObjectName(QString::fromLatin1(name));
        t->setCheckable(true);
        t->setChecked(on);
        connect(t, &QAction::toggled, ircMeter_, slot);
        return t;
    };
    toggle(tr("Row &Meter"), "ircMeterShown", true, &IrcMeter::setShown)
        ->setToolTip(tr("Every row's size in bytes as an IRC line, beside the canvas"));
    a = m->addAction(tr("&Budget..."), this, &MainWindow::editIrcBudget);
    a->setObjectName(QStringLiteral("ircBudget"));
    m->addSeparator();
    toggle(tr("&Fit Lines to the Budget"), "ircFit", ircMeter_->fit(), &IrcMeter::setFit)
        ->setToolTip(tr("Merge the least visible colours in a line over the budget until it fits"));
    toggle(tr("&Pad Lines"), "ircPad", ircMeter_->pad(), &IrcMeter::setPad)
        ->setToolTip(tr("Keep blank rows and trailing black, so clients that trim spaces keep the shape"));
    toggle(tr("&Swap Shades"), "ircSwapShades", ircMeter_->swapShades(), &IrcMeter::setSwapShades)
        ->setToolTip(tr("Draw ░ as ▓ with the colours swapped, and ▓ as ░, where it saves bytes"));
}

void MainWindow::editIrcBudget() {
    IrcBudgetDialog dialog(ircMeter_->budget(), this);
    if (dialog.exec() == QDialog::Accepted)
        doc_.setIrcBudget(dialog.budget());
}

bool MainWindow::exportIrcText(const QString& path, QString* error, bool allowOverBudget) {
    view_->finishTool();
    const std::vector<std::string> lines = texts(ircMeter_->lines());
    const auto over = irc::overBudget(lines, ircMeter_->budget().bytes);
    if (!over.empty() && !allowOverBudget) {
        *error = plural(int(over.size()), tr("%n line over the %1-byte budget"), tr("%n lines over the %1-byte budget"))
                     .arg(ircMeter_->budget().bytes);
        return false;
    }
    const std::string bytes = irc::joinLines(lines);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) ||
        !f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

void MainWindow::exportIrc() {
    view_->finishTool();
    const std::vector<std::string> lines = texts(ircMeter_->lines());
    const std::size_t budget = ircMeter_->budget().bytes;
    const auto over = irc::overBudget(lines, budget);
    if (!over.empty()) {
        int longest = 0;
        for (const auto& o : over)
            longest = std::max(longest, int(o.bytes));
        const auto answer = QMessageBox::warning(
            this, tr("Export IRC Text"),
            plural(int(over.size()),
                   tr("%n line is over the %1-byte budget, at %2 bytes: a client cuts it short or drops it.\n\n"
                      "Export anyway?"),
                   tr("%n lines are over the %1-byte budget, the longest %2 bytes: a client cuts them short or drops "
                      "them.\n\nExport anyway?"))
                .arg(budget)
                .arg(longest),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save)
            return;
    }
    // what IRC text writes as '?', named as Save names a UTF-8 .ans's losses
    if (const QString lost = ircLossDescription(irc::unwritable(irc::sentCanvas(doc_.canvas(), doc_.ansi().ice, doc_.target())));
        !lost.isEmpty() &&
        QMessageBox::warning(this, tr("Export IRC Text"), tr("Before exporting IRC text:\n%1.\n\nExport anyway?").arg(lost),
                             QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save)
        return;
    QString start;
    if (!doc_.path().isEmpty()) {
        const QFileInfo fi(doc_.path());
        start = fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(".txt"));
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export IRC Text"), start,
                                                      tr("IRC text (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exportIrcText(path, &error, true)) {
        QMessageBox::warning(this, tr("Export IRC Text"), tr("Could not write %1:\n%2").arg(path, error));
        return;
    }
    statusBar()->showMessage(
        plural(int(lines.size()), tr("Exported %n IRC line to %1"), tr("Exported %n IRC lines to %1")).arg(path), 3000);
}

} // namespace bw
