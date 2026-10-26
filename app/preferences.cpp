#include "preferences.h"

#include "settings.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <filesystem>

namespace bw {

namespace {

std::filesystem::path fspath(const QString& p) { return std::filesystem::path(QFile::encodeName(p).toStdString()); }

QString qpath(const std::filesystem::path& p) { return QFile::decodeName(QByteArray::fromStdString(p.string())); }

// A path under the folder as the list shows it: relative to the folder.
QString shown(const FontCatalogue& c, const std::filesystem::path& p) {
    const std::filesystem::path rel = p.lexically_relative(c.folder);
    return qpath(rel.empty() ? p : rel);
}

} // namespace

QString fontsFolderSetting() { return appSettings()->value(QLatin1String(kFontsFolderKey)).toString(); }

void setFontsFolderSetting(const QString& folder) {
    const auto s = appSettings();
    if (folder.isEmpty())
        s->remove(QLatin1String(kFontsFolderKey));
    else
        s->setValue(QLatin1String(kFontsFolderKey), folder);
}

std::shared_ptr<const FontCatalogue> loadFontsFolder(const QString& folder) {
    if (folder.isEmpty())
        return nullptr;
    return std::make_shared<const FontCatalogue>(bw::scanFontsFolder(fspath(folder)));
}

PreferencesDialog::PreferencesDialog(const QString& folder, std::shared_ptr<const FontCatalogue> catalogue,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Preferences"));
    folder_ = new QLineEdit(folder.trimmed(), this);
    folder_->setPlaceholderText(tr("None"));
    browse_ = new QPushButton(tr("&Browse..."), this);
    clear_ = new QPushButton(tr("C&lear"), this);
    auto* row = new QHBoxLayout;
    row->addWidget(folder_, 1);
    row->addWidget(browse_);
    row->addWidget(clear_);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    list_ = new QPlainTextEdit(this);
    list_->setReadOnly(true);
    list_->setLineWrapMode(QPlainTextEdit::NoWrap);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* form = new QFormLayout;
    form->addRow(tr("&Fonts folder:"), row);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(summary_);
    layout->addWidget(list_, 1);
    layout->addWidget(buttons_);
    resize(560, 420);

    connect(folder_, &QLineEdit::editingFinished, this, [this] { rescan(); });
    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Fonts Folder"), fontsFolder());
        if (!dir.isEmpty()) {
            folder_->setText(dir);
            rescan();
        }
    });
    connect(clear_, &QPushButton::clicked, this, [this] {
        folder_->clear();
        rescan();
    });
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        rescan(true);  // a folder typed and not yet left, or fonts added to it since
        accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    scanned_ = fontsFolder();
    catalogue_ = catalogue ? std::move(catalogue) : loadFontsFolder(scanned_);
    showCatalogue();
}

QString PreferencesDialog::fontsFolder() const { return folder_->text().trimmed(); }

void PreferencesDialog::rescan(bool always) {
    if (!always && fontsFolder() == scanned_)
        return;
    scanned_ = fontsFolder();
    catalogue_ = loadFontsFolder(scanned_);
    showCatalogue();
}

void PreferencesDialog::showCatalogue() {
    if (!catalogue_) {
        summary_->setText(tr("No fonts folder: text layers can use the bundled faces and the fonts a project holds."));
        list_->clear();
        return;
    }
    const FontCatalogue& c = *catalogue_;
    // spelled out: no translator is loaded to pick %n's plural
    QString summary = c.fonts.size() == 1 ? tr("1 TheDraw font") : tr("%1 TheDraw fonts").arg(c.fonts.size());
    if (c.rejected.size() == 1)
        summary += tr("; 1 file not read");
    else if (!c.rejected.empty())
        summary += tr("; %1 files not read").arg(c.rejected.size());
    summary_->setText(summary);
    QStringList lines;
    for (const FontCatalogue::Font& f : c.fonts)
        lines << QString::fromStdString(f.ref.name) + QStringLiteral("\t") + shown(c, f.file);
    for (const FontCatalogue::Rejected& r : c.rejected)
        lines << tr("Not read: %1: %2").arg(shown(c, r.file), QString::fromStdString(r.reason));
    list_->setPlainText(lines.join(QLatin1Char('\n')));
}

} // namespace bw
