#include "manual_dialog.h"

#include "help_text.h"
#include "shortcuts_dialog.h"
#include "tool.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QKeySequence>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <limits>

namespace bw {

QString manualMarkdown(const ToolRegistry& tools, const QWidget* window) {
    QStringList md;
    md << ManualDialog::tr("# Tools") << ManualDialog::tr(
        "Pick a tool from the toolbox or the Tools menu; its settings are in Tool Options. The key after a "
        "tool's name selects it, and Help > Keyboard Shortcuts lists every key.");
    for (const Tool* t : tools.tools()) {
        const QString text = help::toolText(t->id());
        if (text.isEmpty())
            continue;
        QString heading = QString(t->name()).remove(QLatin1Char('&'));
        const auto* a = window ? window->findChild<QAction*>(t->id() + QStringLiteral("Tool")) : nullptr;
        if (a && !a->shortcut().isEmpty())
            heading += QStringLiteral(" (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
        md << QStringLiteral("## ") + heading << text;
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
    : QDialog(window), text_(new QTextBrowser), shortcuts_(new QPushButton(tr("&Keyboard Shortcuts..."))) {
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
