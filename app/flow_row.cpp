#include "flow_row.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QWidget>

#include <algorithm>

namespace bw {

FlowRow::FlowRow(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }

FlowRow::~FlowRow() {
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void FlowRow::addPair(QLabel* label, QWidget* field) {
    label->setBuddy(field);
    label->setSizePolicy(QSizePolicy::Fixed, label->sizePolicy().verticalPolicy());
    auto* pair = new QHBoxLayout;
    pair->setContentsMargins(0, 0, 0, 0);
    pair->addWidget(label);
    pair->addWidget(field, 1);
    addLayout(pair);
}

void FlowRow::addLayout(QLayout* layout) {
    addChildLayout(layout);
    addItem(layout);
}

void FlowRow::addItem(QLayoutItem* item) { items_.append(item); }

int FlowRow::count() const { return int(items_.size()); }

QLayoutItem* FlowRow::itemAt(int index) const { return index >= 0 && index < items_.size() ? items_[index] : nullptr; }

QLayoutItem* FlowRow::takeAt(int index) {
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowRow::expandingDirections() const { return Qt::Horizontal; }

bool FlowRow::hasHeightForWidth() const { return true; }

int FlowRow::heightForWidth(int width) const {
    const QMargins m = contentsMargins();
    return arrange(QRect(0, 0, width, 0), false) + m.top() + m.bottom();
}

QSize FlowRow::minimumSize() const {
    // As narrow as the widest item: the rest wrap.
    QSize size;
    for (const QLayoutItem* item : items_)
        if (!item->isEmpty())
            size = size.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowRow::sizeHint() const {
    // Every item on one line.
    int width = 0, height = 0, shown = 0;
    for (const QLayoutItem* item : items_) {
        if (item->isEmpty())
            continue;
        const QSize hint = item->sizeHint();
        width += hint.width();
        height = std::max(height, hint.height());
        ++shown;
    }
    width += std::max(0, shown - 1) * horizontalGap();
    const QMargins m = contentsMargins();
    return {width + m.left() + m.right(), height + m.top() + m.bottom()};
}

void FlowRow::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

namespace {

int styleGap(const QLayout* layout, QStyle::PixelMetric metric, Qt::Orientation orientation) {
    const QWidget* w = layout->parentWidget();
    if (!w)
        return 6;
    const int gap = w->style()->pixelMetric(metric, nullptr, w);
    return gap >= 0 ? gap : w->style()->layoutSpacing(QSizePolicy::DefaultType, QSizePolicy::DefaultType, orientation);
}

} // namespace

int FlowRow::horizontalGap() const {
    return spacing() >= 0 ? spacing() : styleGap(this, QStyle::PM_LayoutHorizontalSpacing, Qt::Horizontal);
}

int FlowRow::verticalGap() const {
    return spacing() >= 0 ? spacing() : styleGap(this, QStyle::PM_LayoutVerticalSpacing, Qt::Vertical);
}

int FlowRow::arrange(const QRect& rect, bool apply) const {
    const QRect area = rect.marginsRemoved(contentsMargins());
    const int gap = horizontalGap();
    const int lineGap = verticalGap();
    int y = area.y();
    QList<QLayoutItem*> line;
    int lineWidth = 0;

    const auto finishLine = [&] {
        if (line.isEmpty())
            return;
        // Each item's width: its hint, capped at the row but never under its
        // minimum, plus a share of what the line leaves over if it can grow.
        QList<int> widths;
        int used = 0;
        for (QLayoutItem* item : line) {
            const int w = std::max(item->minimumSize().width(), std::min(item->sizeHint().width(), area.width()));
            widths.append(w);
            used += w;
        }
        used += int(line.size() - 1) * gap;
        int growers = 0;
        for (int i = 0; i < line.size(); ++i)
            if (line[i]->maximumSize().width() > widths[i])
                ++growers;
        int spare = std::max(0, area.width() - used);
        for (int i = 0; i < line.size() && growers > 0; ++i)
            if (line[i]->maximumSize().width() > widths[i]) {
                const int share = spare / growers--;
                const int w = std::min(widths[i] + share, line[i]->maximumSize().width());
                spare -= w - widths[i];
                widths[i] = w;
            }
        int height = 0;
        for (int i = 0; i < line.size(); ++i) {
            QLayoutItem* item = line[i];
            height = std::max(height, item->hasHeightForWidth() ? item->heightForWidth(widths[i])
                                                                : item->sizeHint().height());
        }
        if (apply) {
            int x = area.x();
            for (int i = 0; i < line.size(); ++i) {
                QLayoutItem* item = line[i];
                const int h = item->hasHeightForWidth() ? item->heightForWidth(widths[i])
                                                        : std::min(height, item->sizeHint().height());
                // Centred on the line, as a form row centres its label.
                item->setGeometry(QRect(x, y + (height - h) / 2, widths[i], h));
                x += widths[i] + gap;
            }
        }
        y += height + lineGap;
        line.clear();
        lineWidth = 0;
    };

    for (QLayoutItem* item : items_) {
        if (item->isEmpty())
            continue;
        const int w = std::min(item->sizeHint().width(), area.width());
        if (!line.isEmpty() && lineWidth + gap + w > area.width())
            finishLine();
        lineWidth += (line.isEmpty() ? 0 : gap) + w;
        line.append(item);
    }
    finishLine();
    return y == area.y() ? 0 : y - lineGap - area.y();
}

void FlowForm::setGeometry(const QRect& rect) {
    // invalidate() also asks for one more layout pass, which finds the same
    // rect and stops there.
    if (rect != laidOut_ && hasHeightForWidth())
        invalidate();
    laidOut_ = rect;
    QFormLayout::setGeometry(rect);
}

} // namespace bw
