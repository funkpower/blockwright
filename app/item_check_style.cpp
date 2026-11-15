#include "item_check_style.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPalette>
#include <QStyleOption>

#include <memory>

namespace bw {

namespace {
bool forced = false;
}

void ItemCheckStyle::drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                                   const QWidget* w) const {
    if (pe != PE_IndicatorItemViewItemCheck || !opt || !p || opt->rect.topLeft().isNull()) {
        QProxyStyle::drawPrimitive(pe, opt, p, w);
        return;
    }
    std::unique_ptr<QStyleOption> at(qstyleoption_cast<const QStyleOptionViewItem*>(opt)
                                         ? new QStyleOptionViewItem(*qstyleoption_cast<const QStyleOptionViewItem*>(opt))
                                         : new QStyleOption(*opt));
    at->rect.moveTopLeft({0, 0});
    p->save();
    p->translate(opt->rect.topLeft());
    QProxyStyle::drawPrimitive(pe, at.get(), p, w);
    p->restore();
}

void ItemCheckStyle::polish(QWidget* w) {
    QProxyStyle::polish(w);
    w->installEventFilter(this);
}

void ItemCheckStyle::unpolish(QWidget* w) {
    w->removeEventFilter(this);
    QProxyStyle::unpolish(w);
}

bool ItemCheckStyle::eventFilter(QObject* o, QEvent* e) {
    // A top-level view gets ApplicationPaletteChange; a child view, as the
    // Layers tree and the effect list are, gets PaletteChange instead.
    if (e->type() == QEvent::ApplicationPaletteChange || e->type() == QEvent::PaletteChange) {
        QPalette pal = QApplication::palette();
        if (pal.cacheKey() != polishedPalette_) {
            polishedPalette_ = pal.cacheKey();
            baseStyle()->polish(pal);
        }
    }
    return QProxyStyle::eventFilter(o, e);
}

void fixItemChecks(QAbstractItemView* view) {
    if (!forced && QApplication::style()->name() != QLatin1String("macos"))
        return;
    auto* style = new ItemCheckStyle(QApplication::style()->name());
    style->setParent(view);
    view->setStyle(style);
}

void forceItemChecks(bool on) {
    forced = on;
}

} // namespace bw
