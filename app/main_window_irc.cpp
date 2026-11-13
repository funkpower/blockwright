// The MainWindow parts for IRC (PLAN §8): File > Export IRC Text... and
// the View > IRC menu, the row meter, the document's budget and the
// encoder options it measures with.

#include "main_window.h"

#include "canvas_view.h"
#include "irc_meter.h"
#include "message_box.h"
#include "plural.h"

#include "blockwright/irc_view.h"
#include "blockwright/micron.h"

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
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
    a->setStatusTip(tr("Write the canvas as IRC lines, one message each, as the row meter measures them"));
    a->setToolTip(a->statusTip());
    connect(a, &QAction::triggered, this, &MainWindow::exportIrc);
    // Above File > Publish to Library. Publish writes its own lines: plain,
    // without fit or pad, against the tighter of the document's budget and
    // the EFnet / WeeChat one (PLAN §8, §9; #393).
    file->insertAction(findChild<QAction*>(QStringLiteral("publishToLibrary")), a);
    a = new QAction(tr("Export &Micron..."), this);
    a->setObjectName(QStringLiteral("exportMicron"));
    a->setStatusTip(tr("Write the canvas as a Micron page (.mu) for NomadNet"));
    a->setToolTip(a->statusTip());
    connect(a, &QAction::triggered, this, &MainWindow::exportMicron);
    file->insertAction(findChild<QAction*>(QStringLiteral("publishToLibrary")), a);

    QMenu* m = view->addMenu(tr("I&RC"));
    m->setObjectName(QStringLiteral("ircMenu"));
    const auto toggle = [&](const QString& text, const char* name, const QString& tip, bool on, auto slot) {
        QAction* t = m->addAction(text);
        t->setObjectName(QString::fromLatin1(name));
        t->setStatusTip(tip);
        t->setToolTip(tip);
        t->setCheckable(true);
        t->setChecked(on);
        connect(t, &QAction::toggled, ircMeter_, slot);
        return t;
    };
    toggle(tr("Row &Meter"), "ircMeterShown", tr("Every row's size in bytes as an IRC line, beside the canvas"), true,
           &IrcMeter::setShown);
    a = m->addAction(tr("&Budget..."), this, &MainWindow::editIrcBudget);
    a->setObjectName(QStringLiteral("ircBudget"));
    a->setStatusTip(tr("Set the bytes an IRC line may take, which the row meter and Export IRC Text hold lines to"));
    m->addSeparator();
    toggle(tr("&Fit Lines to the Budget"), "ircFit",
           tr("Merge the least visible colours in a line over the budget until it fits"), ircMeter_->fit(),
           &IrcMeter::setFit);
    toggle(tr("&Pad Lines"), "ircPad",
           tr("Keep blank rows and trailing black, so clients that trim spaces keep the shape"), ircMeter_->pad(),
           &IrcMeter::setPad);
    toggle(tr("&Swap Shades"), "ircSwapShades",
           tr("Draw ░ as ▓ with the colours swapped, and ▓ as ░, where it saves bytes"), ircMeter_->swapShades(),
           &IrcMeter::setSwapShades);
}

void MainWindow::editIrcBudget() {
    IrcBudgetDialog dialog(ircMeter_->budget(), this);
    if (dialog.exec() == QDialog::Accepted)
        doc_.setIrcBudget(dialog.budget());
}

bool MainWindow::exportIrcText(const QString& path, QString* error, bool allowOverBudget) {
    if (const QString why = view_->finishTool(); !why.isEmpty()) {
        *error = tr("the floating selection can't land: %1").arg(why);
        return false;
    }
    const std::vector<std::string> lines = texts(ircMeter_->lines());
    // as the ascii script sends each line, which it writes again (#364)
    const auto over = irc::txtOverBudget(lines, ircMeter_->budget().bytes);
    if (!over.empty() && !allowOverBudget) {
        *error = plural(int(over.size()), tr("%n line over the %1-byte budget as WeeChat's ascii script sends it"),
                        tr("%n lines over the %1-byte budget as WeeChat's ascii script sends them"))
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

bool MainWindow::exportMicronText(const QString& path, QString* error, const micron::Options& opts) {
    if (const QString why = view_->finishTool(); !why.isEmpty()) {
        *error = tr("the floating selection can't land: %1").arg(why);
        return false;
    }
    const AnsiDocument& a = doc_.ansi();
    const std::string bytes = micron::encodeCanvas(doc_.canvas(), a.palette.value_or(kPalette), a.ice, opts);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) ||
        !f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

void MainWindow::exportMicron() {
    if (const QString why = view_->finishTool(); !why.isEmpty()) {
        statusBar()->showMessage(tr("Not exported: the floating selection can't land: %1").arg(why), 5000);
        return;
    }
    if (const QString lost = micronLossDescription(micron::unwritable(doc_.canvas()));
        !lost.isEmpty() &&
        bw::warning(this, tr("Export Micron"), tr("Before exporting a Micron page:\n%1.\n\nExport anyway?").arg(lost),
                    QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save)
        return;
    QDialog options(this);
    options.setObjectName(QStringLiteral("micronOptions"));
    options.setWindowTitle(tr("Export Micron"));
    auto* centre = new QCheckBox(tr("&Centre the piece on the page"), &options);
    centre->setObjectName(QStringLiteral("micronCentre"));
    auto* cache = new QCheckBox(tr("Write a page &cache header, seconds:"), &options);
    cache->setObjectName(QStringLiteral("micronCache"));
    auto* seconds = new QSpinBox(&options);
    seconds->setObjectName(QStringLiteral("micronCacheSeconds"));
    seconds->setRange(0, 31536000);
    seconds->setValue(0);
    seconds->setEnabled(false);
    seconds->setToolTip(tr("How long a peer may keep the page; 0: never cache it"));
    connect(cache, &QCheckBox::toggled, seconds, &QWidget::setEnabled);
    auto* row = new QHBoxLayout;
    row->addWidget(cache);
    row->addWidget(seconds);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &options);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    auto* layout = new QVBoxLayout(&options);
    layout->addWidget(centre);
    layout->addLayout(row);
    layout->addWidget(buttons);
    if (options.exec() != QDialog::Accepted)
        return;
    micron::Options opts;
    opts.centre = centre->isChecked();
    if (cache->isChecked())
        opts.cacheSeconds = unsigned(seconds->value());
    QString start;
    if (!doc_.path().isEmpty()) {
        const QFileInfo fi(doc_.path());
        start = fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(".mu"));
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Micron"), start,
                                                      tr("Micron pages (*.mu);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exportMicronText(path, &error, opts)) {
        bw::warning(this, tr("Export Micron"), tr("Could not write %1:\n%2").arg(path, error));
        return;
    }
    statusBar()->showMessage(tr("Exported a Micron page to %1").arg(path), 3000);
}

void MainWindow::exportIrc() {
    if (const QString why = view_->finishTool(); !why.isEmpty()) {
        statusBar()->showMessage(tr("Not exported: the floating selection can't land: %1").arg(why), 5000);
        return;
    }
    const std::vector<std::string> lines = texts(ircMeter_->lines());
    const std::size_t budget = ircMeter_->budget().bytes;
    const auto over = irc::txtOverBudget(lines, budget);
    if (!over.empty()) {
        int longest = 0;
        for (const auto& o : over)
            longest = std::max(longest, int(o.bytes));
        const auto answer = bw::warning(
            this, tr("Export IRC Text"),
            plural(int(over.size()),
                   tr("%n line is over the %1-byte budget, at %2 bytes as WeeChat's ascii script sends it: the "
                      "script cuts it short.\n\nExport anyway?"),
                   tr("%n lines are over the %1-byte budget, the longest %2 bytes as WeeChat's ascii script sends "
                      "them: the script cuts them short.\n\nExport anyway?"))
                .arg(budget)
                .arg(longest),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save)
            return;
    }
    // what IRC text writes as '?', named as Save names a UTF-8 .ans's losses
    if (const QString lost = ircLossDescription(irc::unwritable(irc::sentCanvas(doc_.canvas(), doc_.ansi().ice, doc_.target())));
        !lost.isEmpty() &&
        bw::warning(this, tr("Export IRC Text"), tr("Before exporting IRC text:\n%1.\n\nExport anyway?").arg(lost),
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
        bw::warning(this, tr("Export IRC Text"), tr("Could not write %1:\n%2").arg(path, error));
        return;
    }
    statusBar()->showMessage(
        plural(int(lines.size()), tr("Exported %n IRC line to %1"), tr("Exported %n IRC lines to %1")).arg(path), 3000);
}

} // namespace bw
