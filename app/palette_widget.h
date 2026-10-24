#pragma once
// The 16-colour palette, for the palette dock: two rows of eight swatches
// in CGA order and, beside them, the pen as GIMP shows it: the foreground
// swatch over the background one, a swap arrow in the corner between them
// and the default colours in the other. A left click on a swatch picks the
// foreground, a right click the background.

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
    // Where the pen is shown, and its parts: the foreground swatch (top
    // left), the background one under it (bottom right), the swap arrow (top
    // right) and the reset to the default colours (bottom left).
    QRect penRect() const;
    QRect foregroundRect() const;
    QRect backgroundRect() const;
    QRect swapRect() const;
    QRect resetRect() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void foregroundPicked(int index);
    void backgroundPicked(int index);
    void swapRequested();
    void resetRequested();

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    Pen pen_;
};

} // namespace bw
