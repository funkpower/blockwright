#include "palette_widget.h"

#include <QMouseEvent>
#include <QPainter>

namespace bw {

namespace {

constexpr int kSwatch = 22, kGap = 2, kColumns = 8;

QColor toQColor(const Color& c) {
    const Rgb rgb = c.isIndexed() ? kPalette[c.index] : c.rgb;
    return QColor(rgb.r, rgb.g, rgb.b);
}

} // namespace

PaletteWidget::PaletteWidget(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setToolTip(tr("Left click: foreground. Right click: background."));
}

void PaletteWidget::setPen(const Pen& pen) {
    if (pen.fg == pen_.fg && pen.bg == pen_.bg)
        return;
    pen_ = pen;
    update();
}

QRect PaletteWidget::penRect() const { return QRect(kGap, kGap, 2 * kSwatch + kGap, 2 * kSwatch + kGap); }

QRect PaletteWidget::swatchRect(int index) const {
    if (index < 0 || index > 15)
        return {};
    const int left = penRect().right() + 1 + 3 * kGap;
    return QRect(left + (index % kColumns) * (kSwatch + kGap), kGap + (index / kColumns) * (kSwatch + kGap), kSwatch,
                 kSwatch);
}

int PaletteWidget::indexAt(QPoint p) const {
    for (int i = 0; i < 16; ++i)
        if (swatchRect(i).contains(p))
            return i;
    return -1;
}

QSize PaletteWidget::sizeHint() const { return QSize(swatchRect(15).right() + 1 + kGap, penRect().bottom() + 1 + kGap); }

void PaletteWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // The pen: the background as a square, the foreground as one inset in it.
    const QRect pr = penRect();
    p.fillRect(pr, toQColor(pen_.bg));
    p.fillRect(pr.adjusted(pr.width() / 4, pr.height() / 4, -pr.width() / 4, -pr.height() / 4), toQColor(pen_.fg));
    p.setPen(palette().color(QPalette::WindowText));
    p.drawRect(pr.adjusted(0, 0, -1, -1));

    const int fg = pen_.fg.paletteIndex(), bg = pen_.bg.paletteIndex();
    QFont f = font();
    f.setBold(true);
    f.setPixelSize(kSwatch / 2);
    p.setFont(f);
    for (int i = 0; i < 16; ++i) {
        const QRect r = swatchRect(i);
        const QColor c = toQColor(Color::indexed(std::uint8_t(i)));
        p.fillRect(r, c);
        // F and B mark the pen's colours, in black or white for contrast.
        const QColor mark = c.lightness() > 110 ? Qt::black : Qt::white;
        p.setPen(mark);
        if (i == fg)
            p.drawText(r.adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("F"));
        if (i == bg)
            p.drawText(r.adjusted(0, 0, -2, 0), Qt::AlignRight | Qt::AlignBottom, QStringLiteral("B"));
        if (i == fg || i == bg)
            p.drawRect(r.adjusted(0, 0, -1, -1));
    }
}

void PaletteWidget::mousePressEvent(QMouseEvent* e) {
    const int i = indexAt(e->position().toPoint());
    if (i < 0)
        return;
    if (e->button() == Qt::LeftButton)
        emit foregroundPicked(i);
    else if (e->button() == Qt::RightButton)
        emit backgroundPicked(i);
}

} // namespace bw
