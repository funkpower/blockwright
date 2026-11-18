#include "shown_palette.h"

#include <QComboBox>
#include <QPainter>
#include <QPixmap>
#include <QVariant>

namespace bw {

namespace {

constexpr const char* kKind = "bwSwatchList";

QColor qcolor(int index) {
    const Rgb c = shownPalette()[std::size_t(index) & 15];
    return QColor(c.r, c.g, c.b);
}

} // namespace

ShownPalette& ShownPalette::instance() {
    static ShownPalette* p = new ShownPalette;  // outlives every widget
    return *p;
}

void ShownPalette::set(const Palette& colours) {
    if (colours == colours_)
        return;
    colours_ = colours;
    emit changed();
}

const Palette& shownPalette() { return ShownPalette::instance().colours(); }

QIcon swatchIcon(int index) {
    QPixmap pm(16, 16);
    pm.fill(qcolor(index));
    return QIcon(pm);
}

QIcon rampIcon(const Ramp& ramp) {
    constexpr int kSwatch = 8, kHeight = 12;
    QPixmap pm(kSwatch * int(ramp.colours.size()), kHeight);
    QPainter p(&pm);
    for (size_t i = 0; i < ramp.colours.size(); ++i)
        p.fillRect(int(i) * kSwatch, 0, kSwatch, kHeight, qcolor(ramp.colours[i]));
    return QIcon(pm);
}

void markSwatches(QComboBox* box, SwatchList kind) { box->setProperty(kKind, int(kind)); }

void refreshSwatches(QWidget* root) {
    for (QComboBox* box : root->findChildren<QComboBox*>()) {
        const QVariant kind = box->property(kKind);
        if (!kind.isValid())
            continue;
        for (int i = 0; i < box->count(); ++i) {
            if (SwatchList(kind.toInt()) == SwatchList::Colours) {
                box->setItemIcon(i, swatchIcon(i));
            } else if (const auto r = findRamp(box->itemData(i).toString().toStdString())) {
                box->setItemIcon(i, rampIcon(*r));
            }
        }
    }
}

} // namespace bw
