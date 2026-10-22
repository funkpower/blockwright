#pragma once
// Whole device pixels under fractional display scaling (PLAN §4). At a
// device pixel ratio of 1.5 a logical pixel is a pixel and a half, and a
// canvas drawn in logical pixels has pixels one and two device pixels wide.
// So the canvas view and the preview draw in view pixels instead: each is a
// whole number of device pixels (the ratio rounded down, at least 1), and
// the first starts on the first whole device pixel in the widget. At a whole
// ratio a view pixel is a logical pixel.

#include <QPointF>
#include <QRect>
#include <QSize>
#include <QTransform>

class QWidget;

namespace bw {

class DeviceGrid {
public:
    // For a widget whose origin is `origin` in device pixels, `ratio`
    // device pixels to a logical pixel.
    DeviceGrid(qreal ratio, QPointF origin);
    // For `w` as it shows in its window's backing store.
    static DeviceGrid of(const QWidget* w);

    qreal ratio() const { return ratio_; }
    // Device pixels to a view pixel.
    int scale() const { return scale_; }

    // A logical size that holds `view` view pixels wherever the widget's
    // origin falls.
    QSize widgetSize(QSize view) const;
    // The logical pixels a view rect covers, rounded outward.
    QRect toWidget(const QRect& view) const;
    // The view pixels a logical rect covers, rounded outward.
    QRect toView(const QRect& widget) const;
    // A logical point in view pixels.
    QPointF toView(QPointF widget) const;
    // The world transform under which a painter whose device transform is
    // `device` (its origin the one this grid was made with) draws in view
    // pixels: view pixel (x, y) is device pixels start + scale * (x, y).
    QTransform viewTransform(const QTransform& device) const;

private:
    qreal ratio_;
    int scale_;
    QPoint start_;    // view pixel (0, 0) in device pixels
    QPointF offset_;  // and from the widget's origin, in [0, 1)
};

} // namespace bw
