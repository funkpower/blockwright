#pragma once
// A row of settings that sit side by side while the row is wide enough and
// wrap onto further lines when it isn't (#236): Tool Options pairs settings
// this way, so a wide column shows them on one line and the narrowest still
// needs no sideways scrolling.
//
// Each line is as tall as its tallest item. The width a line doesn't use goes
// to the items on it that can grow, equally, so a lone item fills the row as
// a form field does.
#include <QFormLayout>
#include <QLayout>
#include <QList>

class QLabel;
class QWidget;

namespace bw {

class FlowRow : public QLayout {
public:
    explicit FlowRow(QWidget* parent = nullptr);
    ~FlowRow() override;

    // `label` beside `field`, kept on one line; the label is the field's
    // buddy and takes no more than its own width.
    void addPair(QLabel* label, QWidget* field);
    // A layout as one item, kept on one line.
    void addLayout(QLayout* layout);

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;

    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    int horizontalGap() const;
    int verticalGap() const;
    // Lays the items out in `rect` (or only measures, when !apply) and
    // returns the height they take.
    int arrange(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> items_;
};

// The form FlowRows sit in. QFormLayout keeps the heights it worked out for
// two widths (its preferred width and the last one asked) but one table of
// row heights, for whichever it worked out last. Laid out at the one width
// after a question about the other, it gives a wrapped FlowRow the height
// of one line and the next row draws over it (#236: the Text tool's rows
// after another tool at the 1366×768 column). This form starts afresh each
// time it is laid out in a new place.
class FlowForm : public QFormLayout {
public:
    using QFormLayout::QFormLayout;
    void setGeometry(const QRect& rect) override;

private:
    QRect laidOut_;
};

} // namespace bw
