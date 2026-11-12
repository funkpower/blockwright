#include "publish.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "message_box.h"
#include "settings.h"

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>

namespace bw {

namespace {

std::filesystem::path fspath(const QString& s) { return std::filesystem::path(QFile::encodeName(s).toStdString()); }

QString qpath(const std::filesystem::path& p) { return QFile::decodeName(QByteArray::fromStdString(p.string())); }

const QString kLibraryKey = QStringLiteral("pipeline/library");
const QString kHostKey = QStringLiteral("pipeline/host");
const QString kWipKey = QStringLiteral("pipeline/wipDir");
QString roundKey(const QString& slug) { return QStringLiteral("pipeline/rounds/") + slug; }

QString lossRefusal(const AnsiOutput& out) {
    return QObject::tr("The .ans would lose data:\n%1.").arg(lossDescription(out, true));
}

QDialog* formDialog(QWidget* parent, const QString& title, QFormLayout** form, QDialogButtonBox** buttons) {
    auto* d = new QDialog(parent);
    d->setWindowTitle(title);
    *form = new QFormLayout(d);
    *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    QObject::connect(*buttons, &QDialogButtonBox::accepted, d, &QDialog::accept);
    QObject::connect(*buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
    return d;
}

QLineEdit* field(QDialog* d, const char* name, const QString& value) {
    auto* e = new QLineEdit(value, d);
    e->setObjectName(QString::fromLatin1(name));
    return e;
}

} // namespace

Publisher::Publisher(MainWindow* window) : QObject(window), window_(window) {
    setObjectName(QStringLiteral("publisher"));
}

void Publisher::addActions(QMenu* file) {
    QAction* a = file->addAction(tr("Publish to &Library..."), this, &Publisher::publishInteractive);
    a->setObjectName(QStringLiteral("publishToLibrary"));
    a = file->addAction(tr("Send to &WeeChat..."), this, &Publisher::sendInteractive);
    a->setObjectName(QStringLiteral("sendToWeechat"));
}

QString Publisher::defaultSlug() const {
    const QString path = window_->document()->path();
    if (path.isEmpty())
        return {};
    return QString::fromStdString(pipeline::slugFrom(QFileInfo(path).completeBaseName().toStdString()));
}

std::optional<pipeline::PublishPlan> Publisher::planPublish(const QString& library, const QString& slug,
                                                            const QString& tags, QString* error,
                                                            bool allowLossy) {
    // A floating selection is published where it floats, as Save does;
    // one that can't land stops it.
    if (const QString why = window_->canvasView()->finishTool(); !why.isEmpty()) {
        *error = tr("the floating selection can't land: %1").arg(why);
        return std::nullopt;
    }
    const AnsiOutput out = payload();
    if (out.lossy() && !allowLossy) {
        *error = lossRefusal(out);
        return std::nullopt;
    }
    try {
        pipeline::PublishRequest r;
        r.library = fspath(library);
        r.slug = slug.toStdString();
        r.tags = pipeline::parseTags(tags.toStdString());
        r.ans = out.bytes;
        r.lines = ircText();
        const pipeline::SendBudget budget = sendBudget();
        r.budget = budget.bytes;
        r.budgetSource = budget.source;
        r.now = QDateTime::currentSecsSinceEpoch();
        return pipeline::planPublish(r);
    } catch (const pipeline::PipelineError& e) {
        *error = QString::fromStdString(e.what());
        return std::nullopt;
    }
}

bool Publisher::publish(const pipeline::PublishPlan& plan, QString* error) {
    // The .txt was held to the budget when the plan was worked out; one the
    // document has had since may hold it to another (#393).
    if (plan.budget != sendBudget()) {
        *error = tr("The document's IRC budget changed after the publish was worked out, so nothing was written; "
                    "publish again to hold the lines to it");
        return false;
    }
    try {
        pipeline::publish(plan);
        return true;
    } catch (const pipeline::PipelineError& e) {
        *error = QString::fromStdString(e.what());
        return false;
    }
}

AnsiOutput Publisher::payload() const {
    // CP437 whatever the document is saved as: the library and WeeChat's
    // ascii script read CP437, and so does the IRC text Publish makes from it.
    return window_->document()->encode(ArtFormat::Ansi, TextEncoding::Cp437);
}

std::optional<std::vector<std::string>> Publisher::ircText() const {
    const Document* doc = window_->document();
    if (doc->target() != Target::Irc && doc->textEncoding() != TextEncoding::Utf8)
        return std::nullopt;
    return irc::exportCanvas(irc::sentCanvas(doc->canvas(), doc->ansi().ice, doc->target()));
}

pipeline::SendBudget Publisher::sendBudget() const {
    return pipeline::sendBudget(window_->document()->extras().ircBudget.bytes);
}

int Publisher::nextRound(const QString& slug) const { return appSettings()->value(roundKey(slug), 0).toInt() + 1; }

Publisher::SendResult Publisher::sendToWeechat(const QString& host, const QString& wipDir, const QString& slug,
                                               int round, bool allowLossy) {
    SendResult result;
    if (const QString why = window_->canvasView()->finishTool(); !why.isEmpty()) {
        result.message = tr("the floating selection can't land: %1").arg(why);
        return result;
    }
    const AnsiOutput out = payload();
    if (out.lossy() && !allowLossy) {
        result.message = lossRefusal(out);
        return result;
    }
    const auto ircLines = ircText();
    const std::string text = ircLines ? irc::joinLines(*ircLines) : std::string();
    QStringList args;
    try {
        if (ircLines) {
            const pipeline::SendBudget budget = sendBudget();
            pipeline::requireBudget(*ircLines, budget.bytes, budget.source);
        }
        for (const std::string& a :
             pipeline::sshArguments({host.toStdString(), wipDir.toStdString()}, slug.toStdString(), round,
                                    out.bytes.size(), ircLines ? std::optional(text.size()) : std::nullopt))
            args << QString::fromStdString(a);
    } catch (const pipeline::PipelineError& e) {
        result.message = QString::fromStdString(e.what());
        return result;
    }

    QProcess ssh;
    ssh.start(ssh_, args);
    if (!ssh.waitForStarted(5000)) {
        result.message = tr("Could not run %1: %2").arg(ssh_, ssh.errorString());
        return result;
    }
    ssh.write(out.bytes.data(), qint64(out.bytes.size()));
    ssh.write(text.data(), qint64(text.size()));
    ssh.closeWriteChannel();
    // The connect timeout is 10 s and the fifo write 5 s.
    if (!ssh.waitForFinished(30000)) {
        ssh.kill();
        ssh.waitForFinished(1000);
        result.message = tr("%1 took more than 30 seconds and was stopped").arg(ssh_);
        return result;
    }
    const QStringList lines =
        QString::fromUtf8(ssh.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const QString last = lines.isEmpty() ? QString() : lines.last().trimmed();
    if (ssh.exitStatus() != QProcess::NormalExit || ssh.exitCode() != 0 ||
        (last != QLatin1String(pipeline::kReloaded.data(), qsizetype(pipeline::kReloaded.size())) &&
         last != QLatin1String(pipeline::kNoFifo.data(), qsizetype(pipeline::kNoFifo.size())))) {
        const QString err = QString::fromUtf8(ssh.readAllStandardError()).trimmed();
        result.message = tr("Sending to %1 failed (exit %2)%3").arg(host).arg(ssh.exitCode()).arg(
            err.isEmpty() ? QString() : QStringLiteral(":\n") + err);
        return result;
    }
    result.ok = true;
    result.reloaded = last == QLatin1String(pipeline::kReloaded.data(), qsizetype(pipeline::kReloaded.size()));
    if (!result.reloaded)
        result.message = tr("No running WeeChat on %1 has a fifo: run /fifo enable, then /ascii reload, in WeeChat")
                             .arg(host);
    const auto s = appSettings();
    if (s->value(roundKey(slug), 0).toInt() < round)
        s->setValue(roundKey(slug), round);
    return result;
}

void Publisher::publishInteractive() {
    QFormLayout* form;
    QDialogButtonBox* buttons;
    QDialog* d = formDialog(window_, tr("Publish to Library"), &form, &buttons);
    d->setObjectName(QStringLiteral("publishDialog"));
    QLineEdit* library = field(d, "publishLibrary", appSettings()->value(kLibraryKey).toString());
    auto* browse = new QPushButton(tr("Browse..."), d);
    auto* row = new QHBoxLayout;
    row->addWidget(library);
    row->addWidget(browse);
    QLineEdit* slug = field(d, "publishSlug", defaultSlug());
    QLineEdit* tags = field(d, "publishTags", QString());
    tags->setPlaceholderText(tr("comma or space separated; %1 is always added")
                                 .arg(QString::fromLatin1(pipeline::kArtist.data(),
                                                          qsizetype(pipeline::kArtist.size()))));
    form->addRow(tr("Library folder:"), row);
    form->addRow(tr("Piece name:"), slug);
    form->addRow(tr("Tags:"), tags);
    form->addRow(buttons);
    const auto validate = [=] {
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(!library->text().trimmed().isEmpty() && pipeline::validSlug(slug->text().toStdString()));
    };
    connect(library, &QLineEdit::textChanged, d, validate);
    connect(slug, &QLineEdit::textChanged, d, validate);
    connect(browse, &QPushButton::clicked, d, [=] {
        const QString dir = QFileDialog::getExistingDirectory(d, tr("Library Folder"), library->text());
        if (!dir.isEmpty())
            library->setText(dir);
    });
    validate();
    const bool accepted = d->exec() == QDialog::Accepted;
    const QString libraryDir = library->text().trimmed(), name = slug->text(), tagText = tags->text();
    delete d;
    if (!accepted)
        return;

    QString error;
    const std::optional<pipeline::PublishPlan> plan = planPublish(libraryDir, name, tagText, &error, true);
    if (!plan) {
        bw::warning(window_, tr("Publish to Library"), tr("Could not publish:\n%1").arg(error));
        return;
    }
    appSettings()->setValue(kLibraryKey, libraryDir);
    QStringList tagList;
    for (const std::string& t : plan->tags)
        tagList << QString::fromStdString(t);
    QString question =
        tr("%1 %2, tagged %3.\n\nThis writes:\n%4\n%5\n%6\n%7")
            .arg(plan->replaces ? tr("Replace") : tr("Publish"), QString::fromStdString(plan->key),
                 tagList.join(QStringLiteral(", ")), qpath(plan->ansPath), qpath(plan->txtPath),
                 qpath(plan->tagsPath), qpath(plan->datesPath));
    const AnsiOutput out = payload();
    if (out.lossy())
        question += QStringLiteral("\n\n") + pipelineWarning(out, PipelineAction::Publish, ircText().has_value());
    else
        question += tr("\n\nPublish?");
    if (bw::question(window_, tr("Publish to Library"), question, QMessageBox::Yes | QMessageBox::No,
                     QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!publish(*plan, &error)) {
        bw::warning(window_, tr("Publish to Library"), tr("Could not publish:\n%1").arg(error));
        return;
    }
    window_->statusBar()->showMessage(tr("Published %1").arg(QString::fromStdString(plan->key)), 5000);
}

void Publisher::sendInteractive() {
    QFormLayout* form;
    QDialogButtonBox* buttons;
    QDialog* d = formDialog(window_, tr("Send to WeeChat"), &form, &buttons);
    d->setObjectName(QStringLiteral("sendDialog"));
    const auto s = appSettings();
    QLineEdit* host = field(d, "sendHost", s->value(kHostKey).toString());
    host->setPlaceholderText(tr("ssh host, as in ssh's config"));
    QLineEdit* wip = field(d, "sendWipDir", s->value(kWipKey, QString::fromStdString(pipeline::WeechatTarget{}.wipDir)).toString());
    QLineEdit* slug = field(d, "sendSlug", defaultSlug());
    auto* round = new QSpinBox(d);
    round->setObjectName(QStringLiteral("sendRound"));
    round->setRange(1, 999);
    round->setValue(nextRound(slug->text()));
    form->addRow(tr("Host:"), host);
    form->addRow(tr("Wip folder:"), wip);
    form->addRow(tr("Piece name:"), slug);
    form->addRow(tr("Round:"), round);
    form->addRow(buttons);
    const auto validate = [=] {
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(pipeline::validHost(host->text().toStdString()) &&
                         pipeline::validRemoteDir(wip->text().toStdString()) &&
                         pipeline::validSlug(slug->text().toStdString()));
    };
    connect(host, &QLineEdit::textChanged, d, validate);
    connect(wip, &QLineEdit::textChanged, d, validate);
    connect(slug, &QLineEdit::textChanged, d, validate);
    connect(slug, &QLineEdit::textChanged, d, [=, this](const QString& text) { round->setValue(nextRound(text)); });
    validate();
    const bool accepted = d->exec() == QDialog::Accepted;
    const QString hostName = host->text(), wipDir = wip->text(), name = slug->text();
    const int roundNo = round->value();
    delete d;
    if (!accepted)
        return;

    // Stamped before the loss check below, so the warning covers it.
    if (const QString why = window_->canvasView()->finishTool(); !why.isEmpty()) {
        bw::warning(window_, tr("Send to WeeChat"), tr("the floating selection can't land: %1").arg(why));
        return;
    }
    QString target = QStringLiteral("%1:%2/%3")
                         .arg(hostName, wipDir, QString::fromStdString(pipeline::wipName(name.toStdString(), roundNo)));
    const bool text = ircText().has_value();
    if (text)
        target = tr("%1 and %2").arg(target, QString::fromStdString(pipeline::wipTextName(name.toStdString(), roundNo)));
    QString question = tr("Copy the piece to %1 and reload WeeChat's ascii script?").arg(target);
    const AnsiOutput out = payload();
    if (out.lossy())
        question += QStringLiteral("\n\n") + pipelineWarning(out, PipelineAction::Send, text);
    if (bw::question(window_, tr("Send to WeeChat"), question, QMessageBox::Yes | QMessageBox::No,
                     QMessageBox::No) != QMessageBox::Yes)
        return;
    s->setValue(kHostKey, hostName);
    s->setValue(kWipKey, wipDir);
    const SendResult r = sendToWeechat(hostName, wipDir, name, roundNo, true);
    if (!r.ok) {
        bw::warning(window_, tr("Send to WeeChat"), r.message);
        return;
    }
    if (!r.reloaded) {
        bw::information(window_, tr("Send to WeeChat"), tr("Copied to %1.\n\n%2").arg(target, r.message));
        return;
    }
    window_->statusBar()->showMessage(
        tr("Sent; /ascii %1 is ready").arg(QString::fromStdString(pipeline::wipName(name.toStdString(), roundNo))
                                                .chopped(4)),
        5000);
}

} // namespace bw
