#include "manual_dialog.h"

#include "help_text.h"
#include "shortcuts_dialog.h"
#include "tool.h"
#include "tool_illustration.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QKeySequence>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include <limits>

namespace bw {

namespace {

constexpr auto kIllustrationScheme = "illustration";

// The manual's view: an image at illustration:<tool id> is the tool's
// picture (tool_illustration.h), made when the manual is first laid out.
class ManualBrowser : public QTextBrowser {
public:
    QVariant loadResource(int type, const QUrl& name) override {
        if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String(kIllustrationScheme)) {
            const help::Illustration* picture = help::toolIllustration(name.path());
            return picture ? QVariant(picture->image) : QVariant();
        }
        return QTextBrowser::loadResource(type, name);
    }
};

} // namespace

QUrl illustrationUrl(const QString& toolId) {
    QUrl url;
    url.setScheme(QLatin1String(kIllustrationScheme));
    url.setPath(toolId);
    return url;
}

QString manualMarkdown(const ToolRegistry& tools, const QWidget* window) {
    QStringList md;
    md << ManualDialog::tr("# Tools") << ManualDialog::tr(
        "Pick a tool from the toolbox or the Tools menu; its settings are in Tool Options. The key after a "
        "tool's name selects it, and Help > Keyboard Shortcuts lists every key.");
    for (const Tool* t : tools.tools()) {
        const QString text = help::toolText(t->id());
        if (text.isEmpty())
            continue;
        const QString plainName = QString(t->name()).remove(QLatin1Char('&'));
        QString heading = plainName;
        const auto* a = window ? window->findChild<QAction*>(t->id() + QStringLiteral("Tool")) : nullptr;
        if (a && !a->shortcut().isEmpty())
            heading += QStringLiteral(" (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
        md << QStringLiteral("## ") + heading;
        if (help::toolIllustration(t->id()))
            md << QStringLiteral("![%1](%2)").arg(ManualDialog::tr("The %1 tool at work").arg(plainName),
                                                  illustrationUrl(t->id()).toString());
        md << text;
    }
    md << ManualDialog::tr("# Layers")
       << ManualDialog::tr("The Layers dock adds, orders and hides layers. Each kind holds something different:");
    for (int k = 0; k <= std::numeric_limits<std::underlying_type_t<LayerKind>>::max(); ++k) {
        const auto kind = LayerKind(k);
        const QString text = help::layerText(kind);
        if (text.isEmpty())
            continue;
        QString name = QString::fromUtf8(layerKindName(kind).data(), qsizetype(layerKindName(kind).size()));
        name[0] = name[0].toUpper();
        md << QStringLiteral("## ") + name << text;
    }
    return md.join(QStringLiteral("\n\n")) + QLatin1Char('\n');
}

ManualDialog::ManualDialog(const ToolRegistry& tools, QWidget* window)
    : QDialog(window), text_(new ManualBrowser), shortcuts_(new QPushButton(tr("&Keyboard Shortcuts..."))) {
    setWindowTitle(tr("Blockwright Manual"));
    text_->setObjectName(QStringLiteral("manualText"));
    text_->setMarkdown(manualMarkdown(tools, window));
    shortcuts_->setObjectName(QStringLiteral("manualShortcuts"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->addButton(shortcuts_, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(shortcuts_, &QPushButton::clicked, this, [window] { showShortcuts(window); });
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(text_);
    layout->addWidget(buttons);
    resize(640, 560);
}

ManualDialog* showManual(const ToolRegistry& tools, QWidget* window) {
    auto* d = window->findChild<ManualDialog*>(QString(), Qt::FindDirectChildrenOnly);
    if (!d)
        d = new ManualDialog(tools, window);
    d->show();
    d->raise();
    d->activateWindow();
    return d;
}

} // namespace bw
