#pragma once

#include <QHeaderView>
#include <QTreeWidget>

#include <algorithm>

namespace bw {

// A list that asks for its header and a set number of rows, so its dock's
// default height (PLAN §11) shows them with the controls around the list.
class RowTree : public QTreeWidget {
public:
    explicit RowTree(int rows, QWidget* parent = nullptr) : QTreeWidget(parent), rows_(rows) {}

    QSize sizeHint() const override {
        const int row = std::max(sizeHintForRow(0), fontMetrics().height());
        const int head = isHeaderHidden() ? 0 : header()->sizeHint().height();
        return {QTreeWidget::sizeHint().width(), head + rows_ * row + 2 * frameWidth()};
    }

private:
    int rows_;
};

} // namespace bw
