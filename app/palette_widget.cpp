#include "palette_widget.h"

#include <QHelpEvent>
#include <QAction>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>

namespace bw {

namespace {

constexpr int kSwatch = 22, kGap = 2, kColumns = 8;
// mIRC's 16-98: rows of 12, so the greys (88-98) make a row of their own.
constexpr int kSmall = 16, kMircColumns = 12;
// The pen's swatches overlap by a third; the corners left over hold the
// swap arrow and the reset.
constexpr int kPen = 2 * kSwatch + kGap, kPenSwatch = kPen * 2 / 3;

} // namespace

QColor PaletteWidget::toQColor(const Color& c) const {
    const Rgb rgb = c.isIndexed() ? colours_[c.index & 15] : c.rgb;
    return QColor(rgb.r, rgb.g, rgb.b);
}

void PaletteWidget::setColours(const Palette& colours) {
    if (colours == colours_)
        return;
    colours_ = colours;
    update();
}

PaletteWidget::PaletteWidget(QWidget* parent) : QWidget(parent) {
    // A fixed horizontal policy caps the entire dock column at this hint.
    // Keep every swatch visible, but let the column grow for the other docks.
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setToolTip(tr("Left click: foreground. Right click: background."));
}

void PaletteWidget::setKeyActions(QAction* swap, QAction* reset) {
    swap_ = swap;
    reset_ = reset;
}

void PaletteWidget::setTarget(Target target, IrcColours colours) {
    if (target == target_ && colours == ircColours_)
        return;
    target_ = target;
    ircColours_ = colours;
    updateGeometry();
    update();
}

void PaletteWidget::setPen(const Pen& pen) {
    if (pen.fg == pen_.fg && pen.bg == pen_.bg)
        return;
    pen_ = pen;
    update();
}

QRect PaletteWidget::penRect() const { return QRect(kGap, kGap, kPen, kPen); }

QRect PaletteWidget::foregroundRect() const {
    return QRect(penRect().topLeft(), QSize(kPenSwatch, kPenSwatch));
}

QRect PaletteWidget::backgroundRect() const {
    return QRect(penRect().bottomRight() - QPoint(kPenSwatch - 1, kPenSwatch - 1), QSize(kPenSwatch, kPenSwatch));
}

QRect PaletteWidget::swapRect() const {
    const QRect pr = penRect();
    return QRect(QPoint(foregroundRect().right() + 1, pr.top()), QPoint(pr.right(), backgroundRect().top() - 1));
}

QRect PaletteWidget::resetRect() const {
    const QRect pr = penRect();
    return QRect(QPoint(pr.left(), foregroundRect().bottom() + 1), QPoint(backgroundRect().left() - 1, pr.bottom()));
}

QRect PaletteWidget::swatchRect(int index) const {
    if (index < 0 || index >= swatchCount(target_, ircColours_))
        return {};
    if (index >= 16) {
        const int top = penRect().bottom() + 1 + 2 * kGap;
        const int at = index - 16;
        return QRect(kGap + (at % kMircColumns) * (kSmall + kGap), top + (at / kMircColumns) * (kSmall + kGap),
                     kSmall, kSmall);
    }
    const int left = penRect().right() + 1 + 3 * kGap;
    return QRect(left + (index % kColumns) * (kSwatch + kGap), kGap + (index / kColumns) * (kSwatch + kGap), kSwatch,
                 kSwatch);
}

int PaletteWidget::indexAt(QPoint p) const {
    for (int i = 0; i < swatchCount(target_, ircColours_); ++i)
        if (swatchRect(i).contains(p))
            return i;
    return -1;
}

QSize PaletteWidget::sizeHint() const {
    const int w = std::max(swatchRect(15).right(), swatchRect(swatchCount(target_, ircColours_) - 1).right()) + 1 + kGap;
    const int last = swatchCount(target_, ircColours_) > 16 ? swatchRect(swatchCount(target_, ircColours_) - 1).bottom() : penRect().bottom();
    return QSize(w, last + 1 + kGap);
}

void PaletteWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // The pen: the foreground over the background, each framed.
    const QColor ink = palette().color(QPalette::WindowText);
    for (const auto& [r, c] : {std::pair{backgroundRect(), pen_.bg}, std::pair{foregroundRect(), pen_.fg}}) {
        p.fillRect(r, toQColor(c));
        p.setPen(ink);
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.setPen(palette().color(QPalette::Window));
        p.drawRect(r.adjusted(1, 1, -2, -2));
    }
    // The swap: an arrow bent round the corner, a head at each end.
    {
        const QRectF s = QRectF(swapRect()).adjusted(1.5, 1.5, -1.5, -1.5);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(ink, 1.2));
        QPainterPath arc;
        arc.moveTo(s.left() + 2, s.top() + 2);
        arc.quadTo(s.right() - 2, s.top() + 2, s.right() - 2, s.bottom() - 2);
        p.drawPath(arc);
        p.drawLine(QPointF(s.left() + 2, s.top() + 2), QPointF(s.left() + 5, s.top()));
        p.drawLine(QPointF(s.left() + 2, s.top() + 2), QPointF(s.left() + 5, s.top() + 5));
        p.drawLine(QPointF(s.right() - 2, s.bottom() - 2), QPointF(s.right(), s.bottom() - 5));
        p.drawLine(QPointF(s.right() - 2, s.bottom() - 2), QPointF(s.right() - 5, s.bottom() - 5));
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    // The reset: the default pen in small, light grey over black.
    {
        const QRect r = resetRect().adjusted(1, 1, -1, -1);
        const int w = r.width() * 2 / 3, h = r.height() * 2 / 3;
        const QRect back(r.right() - w + 1, r.bottom() - h + 1, w, h), front(r.left(), r.top(), w, h);
        p.fillRect(back, toQColor(Color::indexed(0)));
        p.setPen(ink);
        p.drawRect(back.adjusted(0, 0, -1, -1));
        p.fillRect(front, toQColor(Color::indexed(7)));
        p.drawRect(front.adjusted(0, 0, -1, -1));
    }

    const int fg = swatchOf(pen_.fg, target_, ircColours_), bg = swatchOf(pen_.bg, target_, ircColours_);
    QFont f = font();
    f.setBold(true);
    f.setPixelSize(kSwatch / 2);
    p.setFont(f);
    for (int i = 0; i < swatchCount(target_, ircColours_); ++i) {
        if (i == 16) {
            f.setPixelSize(kSmall / 2);
            p.setFont(f);
        }
        const QRect r = swatchRect(i);
        const QColor c = toQColor(swatchColor(i));
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

bool PaletteWidget::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        const QPoint at = static_cast<QHelpEvent*>(e)->pos();
        QString tip;
        // with the key of the Colours menu's item, if it has one
        const auto key = [](const QString& text, const QAction* a) {
            return a && !a->shortcut().isEmpty()
                       ? tr("%1 (%2)").arg(text, a->shortcut().toString(QKeySequence::NativeText))
                       : text;
        };
        if (swapRect().contains(at))
            tip = key(tr("Swap the foreground and background"), swap_);
        else if (resetRect().contains(at))
            tip = key(tr("Default colours"), reset_);
        else if (foregroundRect().contains(at))
            tip = target_ == Target::Micron ? tr("The foreground colour %1: click to pick another").arg(toQColor(pen_.fg).name())
                                            : tr("The foreground colour");
        else if (backgroundRect().contains(at))
            tip = target_ == Target::Micron ? tr("The background colour %1: click to pick another").arg(toQColor(pen_.bg).name())
                                            : tr("The background colour");
        else if (const int i = indexAt(at); i >= 16)
            tip = tr("mIRC %1 (%2)").arg(i).arg(toQColor(swatchColor(i)).name());
        else
            tip = toolTip();
        QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(e);
}

void PaletteWidget::mousePressEvent(QMouseEvent* e) {
    const QPoint at = e->position().toPoint();
    if (e->button() == Qt::LeftButton && swapRect().contains(at)) {
        emit swapRequested();
        return;
    }
    if (e->button() == Qt::LeftButton && resetRect().contains(at)) {
        emit resetRequested();
        return;
    }
    // the background swatch is under the foreground's corner, so the
    // foreground is asked first, as it is drawn on top
    if (e->button() == Qt::LeftButton && target_ == Target::Micron &&
        (foregroundRect().contains(at) || backgroundRect().contains(at))) {
        emit colourDialogRequested(foregroundRect().contains(at));
        return;
    }
    const int i = indexAt(at);
    if (i < 0)
        return;
    if (e->button() == Qt::LeftButton)
        emit foregroundPicked(i);
    else if (e->button() == Qt::RightButton)
        emit backgroundPicked(i);
}

} // namespace bw
