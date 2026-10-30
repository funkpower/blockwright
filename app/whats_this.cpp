#include "whats_this.h"

#include "help_text.h"

#include <QHelpEvent>
#include <QLabel>
#include <QTextDocument>
#include <QWhatsThis>
#include <QWidget>

namespace bw {

namespace {

// On root: Qt passes the What's This events a widget doesn't take up to
// its parents, so root sees every one its widgets leave.
class ToolTipWhatsThis : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* e) override {
        if (e->type() != QEvent::QueryWhatsThis && e->type() != QEvent::WhatsThis)
            return false;
        auto* root = static_cast<QWidget*>(watched);
        auto* help = static_cast<QHelpEvent*>(e);
        QWidget* w = root->childAt(help->pos());
        const QString text = toolTipWhatsThis(root, w ? w : root);
        if (text.isEmpty())
            return false;
        if (e->type() == QEvent::WhatsThis)
            QWhatsThis::showText(help->globalPos(), text, w ? w : root);
        e->accept();
        return true;
    }
};

} // namespace

QString toolWhatsThis(const QString& id) {
    const QString md = help::toolText(id);
    if (md.isEmpty())
        return {};
    QTextDocument doc;
    doc.setMarkdown(md);
    return doc.toHtml();
}

void whatsThisFromToolTips(QWidget* root) { root->installEventFilter(new ToolTipWhatsThis(root)); }

QString toolTipWhatsThis(const QWidget* root, const QWidget* w) {
    for (; w; w = w == root ? nullptr : w->parentWidget()) {
        if (!w->whatsThis().isEmpty())
            return w->whatsThis();
        if (!w->toolTip().isEmpty())
            return w->toolTip();
        if (auto* label = qobject_cast<const QLabel*>(w); label && label->buddy())
            if (const QString tip = toolTipWhatsThis(root, label->buddy()); !tip.isEmpty())
                return tip;
    }
    return {};
}

} // namespace bw
