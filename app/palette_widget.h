#pragma once
// The palette, for the palette dock: two rows of eight swatches in CGA
// order and, beside them, the pen as GIMP shows it: the foreground swatch
// over the background one, a swap arrow in the corner between them and the
// default colours in the other. Under the IRC target, mIRC's colours 16-98
// follow below in mIRC's layout, smaller: six rows of 12, then the 11
// greys. A left click on a swatch picks the foreground, a right click the
// background; a swatch is its index 0-98 (swatchColor). Under Micron (PLAN
// §17) the same 99 show, and a left click on the pen's foreground or
// background swatch asks for the Micron colour dialog.

#include "blockwright/edit.h"
#include "blockwright/target.h"

#include <QPointer>
#include <QWidget>

class QAction;

namespace bw {

class PaletteWidget : public QWidget {
    Q_OBJECT
public:
    explicit PaletteWidget(QWidget* parent = nullptr);

    const Pen& pen() const { return pen_; }
    void setPen(const Pen& pen);
    // The target: which swatches show (swatchCount).
    Target target() const { return target_; }
    void setTarget(Target target);

    // Swatch `index` (0-15, or 0-98 under IRC) in widget pixels, and the
    // swatch at a point (-1 when there is none).
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

    // The Colours menu's Swap Colours and Default Colours, whose keys the
    // swap arrow's and reset corner's tooltips show.
    void setKeyActions(QAction* swap, QAction* reset);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void foregroundPicked(int index);
    void backgroundPicked(int index);
    void swapRequested();
    void resetRequested();
    // Micron only: the pen's foreground (true) or background swatch clicked.
    void colourDialogRequested(bool foreground);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    Pen pen_;
    Target target_ = Target::Ansi;
    QPointer<QAction> swap_;
    QPointer<QAction> reset_;
};

} // namespace bw
