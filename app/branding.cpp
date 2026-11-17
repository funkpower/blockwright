#include "branding.h"

#include "blockwright/credits.h"
#include "blockwright/render.h"
#include "blockwright/version.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>

#include <cmath>

namespace bw {

std::optional<AnsiDocument> loadLogo(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray bytes = f.readAll();
    try {
        return parseAnsi(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

QPixmap logoPixmap(const QString& path, qreal devicePixelRatio) {
    const auto doc = loadLogo(path);
    if (!doc)
        return {};
    const int scale = std::clamp(static_cast<int>(std::ceil(devicePixelRatio)), 1, kMaxScale);
    try {
        QPixmap pm = QPixmap::fromImage(renderDocument(*doc, RenderOptions{.fontWidth = 0, .scale = scale}));
        pm.setDevicePixelRatio(scale);
        return pm;
    } catch (const std::exception&) {
        return {};
    }
}

QIcon applicationIcon() {
    QIcon icon;
    for (const int size : {16, 32, 48, 128, 256, 512})
        icon.addFile(QStringLiteral(":/branding/blockwright-%1.png").arg(size), QSize(size, size));
    return icon;
}

QString desktopFileName(const QString& argv0, const QString& flatpakId) {
    if (!flatpakId.isEmpty())
        return flatpakId;
    return QFileInfo(argv0).fileName();
}

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(tr("About Blockwright"));
    auto* layout = new QVBoxLayout(this);

    // The logo on black, the colour its cells are drawn on, whatever the theme
    auto* logo = logo_ = new QLabel(this);
    logo->setObjectName(QStringLiteral("aboutLogo"));
    logo->setAccessibleName(tr("Blockwright"));
    logo->setAlignment(Qt::AlignCenter);
    logo->setAutoFillBackground(true);
    QPalette pal = logo->palette();
    pal.setColor(QPalette::Window, Qt::black);
    logo->setPalette(pal);
    // QLabel::margin, not contentsMargins: polishing the label resets those
    logo->setMargin(16);
    drawLogo();
    layout->addWidget(logo);

    auto* text = new QLabel(
        tr("<p><b>Blockwright</b> %1 (%2)</p><p>ANSI and IRC art editor. GPL-3.0-or-later.</p>"
           "<p>%3</p><p>%4</p><p>%5</p><p>%6</p>")
            .arg(QStringLiteral(BLOCKWRIGHT_VERSION), QStringLiteral(BLOCKWRIGHT_GIT_COMMIT),
                 QString::fromUtf8(kFontCredit).toHtmlEscaped().replace('\n', ' '),
                 QString::fromUtf8(kUnifontCredit).toHtmlEscaped().replace('\n', ' '),
                 QString::fromUtf8(kIconCredit).toHtmlEscaped().replace('\n', ' '),
                 QString::fromUtf8(kUiFontCredit).toHtmlEscaped().replace('\n', ' ')),
        this);
    text->setObjectName(QStringLiteral("aboutText"));
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextBrowserInteraction);
    text->setOpenExternalLinks(true);
    // as wide as the logo's panel, so the credits wrap under it
    text->setMinimumWidth(logo->sizeHint().width());
    layout->addWidget(text);
    help_ = new QLabel(this);
    help_->setObjectName(QStringLiteral("aboutHelp"));
    help_->setWordWrap(true);
    help_->setTextFormat(Qt::PlainText);
    help_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    help_->hide();
    layout->addWidget(help_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void AboutDialog::drawLogo() {
    const QPixmap pm = logoPixmap(QString::fromLatin1(kLogoSmall), devicePixelRatioF());
    logo_->setPixmap(pm);
    logo_->setVisible(!pm.isNull());
}

bool AboutDialog::event(QEvent* e) {
    if (e->type() == QEvent::DevicePixelRatioChange)
        drawLogo();
    return QDialog::event(e);
}

void AboutDialog::setHelpLine(const QString& line) {
    help_->setText(line);
    help_->setVisible(!line.isEmpty());
}

AboutDialog* showAbout(QWidget* window, const QString& helpLine) {
    auto* d = window->findChild<AboutDialog*>(QString(), Qt::FindDirectChildrenOnly);
    if (!d)
        d = new AboutDialog(window);
    d->setHelpLine(helpLine);
    d->show();
    d->raise();
    d->activateWindow();
    return d;
}

} // namespace bw
