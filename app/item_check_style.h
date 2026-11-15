#pragma once

#include <QProxyStyle>

class QAbstractItemView;

namespace bw {

// Draws an item view's check boxes with the indicator's rectangle at the
// painter's origin. QMacStyle before Qt's fix for QTBUG-150017 (macOS 27)
// draws the box shifted by its own position in the view, so only a box at
// the view's top left shows: the first row's, in the first column.
//
// Qt polishes only the application style with a new palette, and QMacStyle
// in Qt 6.8 drops the native controls it drew under the old light or dark
// appearance only then. So the widgets this style polishes pass each
// application palette change on to the base style, once per palette: a
// top-level view sees it as ApplicationPaletteChange, a child view as
// PaletteChange.
class ItemCheckStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    using QProxyStyle::polish;
    using QProxyStyle::unpolish;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p,
                       const QWidget* w = nullptr) const override;
    void polish(QWidget* w) override;
    void unpolish(QWidget* w) override;

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    qint64 polishedPalette_ = 0;
};

// Gives `view` an ItemCheckStyle when the application runs under QMacStyle;
// under any other style it changes nothing.
void fixItemChecks(QAbstractItemView* view);

// For tests: makes fixItemChecks act under any style, as under QMacStyle.
void forceItemChecks(bool on);

} // namespace bw
