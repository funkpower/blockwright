#include "device_grid.h"

#include <QWidget>

#include <algorithm>
#include <cmath>

namespace bw {

namespace {

// For a ratio or an origin a rounding error off a whole number.
constexpr double kEpsilon = 1e-6;

int ceilInt(double v) { return int(std::ceil(v - kEpsilon)); }

} // namespace

DeviceGrid::DeviceGrid(qreal ratio, QPointF origin)
    : ratio_(ratio > 0 ? ratio : 1), scale_(std::max(1, int(std::floor(ratio_ + kEpsilon)))),
      start_(ceilInt(origin.x()), ceilInt(origin.y())), offset_(QPointF(start_) - origin) {}

DeviceGrid DeviceGrid::of(const QWidget* w) {
    const qreal ratio = w->devicePixelRatioF();
    return {ratio, w->mapTo(w->window(), QPointF(0, 0)) * ratio};
}

QSize DeviceGrid::widgetSize(QSize view) const {
    // Off a whole ratio the grid starts up to a device pixel into the widget.
    const int slack = scale_ == ratio_ ? 0 : 1;
    return {ceilInt((double(view.width()) * scale_ + slack) / ratio_),
            ceilInt((double(view.height()) * scale_ + slack) / ratio_)};
}

QRect DeviceGrid::toWidget(const QRect& view) const {
    if (view.isEmpty())
        return {};
    const double x0 = offset_.x() + double(view.left()) * scale_, x1 = x0 + double(view.width()) * scale_;
    const double y0 = offset_.y() + double(view.top()) * scale_, y1 = y0 + double(view.height()) * scale_;
    return QRect(QPoint(int(std::floor(x0 / ratio_)), int(std::floor(y0 / ratio_))),
                 QPoint(int(std::ceil(x1 / ratio_)) - 1, int(std::ceil(y1 / ratio_)) - 1));
}

QRect DeviceGrid::toView(const QRect& widget) const {
    if (widget.isEmpty())
        return {};
    const double x0 = double(widget.left()) * ratio_ - offset_.x(), x1 = x0 + double(widget.width()) * ratio_;
    const double y0 = double(widget.top()) * ratio_ - offset_.y(), y1 = y0 + double(widget.height()) * ratio_;
    return QRect(QPoint(int(std::floor(x0 / scale_)), int(std::floor(y0 / scale_))),
                 QPoint(int(std::ceil(x1 / scale_)) - 1, int(std::ceil(y1 / scale_)) - 1));
}

QPointF DeviceGrid::toView(QPointF widget) const { return (widget * ratio_ - offset_) / scale_; }

QTransform DeviceGrid::viewTransform(const QTransform& device) const {
    return QTransform::fromScale(scale_, scale_) * QTransform::fromTranslate(start_.x(), start_.y()) *
           device.inverted();
}

} // namespace bw
