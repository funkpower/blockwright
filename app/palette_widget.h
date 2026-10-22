#pragma once
// The 16-colour palette, for the palette dock: two rows of eight swatches
// in CGA order and, beside them, the pen (its foreground on its background).
// A left click picks the foreground, a right click the background.

#include "blockwright/edit.h"

#include <QWidget>

namespace bw {

class PaletteWidget : public QWidget {
    Q_OBJECT
public:
    explicit PaletteWidget(QWidget* parent = nullptr);

    const Pen& pen() const { return pen_; }
    void setPen(const Pen& pen);

    // Swatch `index` (0-15) in widget pixels, and the swatch at a point
    // (-1 when there is none).
    QRect swatchRect(int index) const;
    int indexAt(QPoint p) const;
    // Where the pen is shown.
    QRect penRect() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void foregroundPicked(int index);
    void backgroundPicked(int index);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    Pen pen_;
};

} // namespace bw
