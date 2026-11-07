#pragma once
// Format > Canvas Size... (PLAN §5, #397): the canvas's new width and
// height, 1 to kMaxColumns by 1 to kMaxRows as File > New holds them, and
// where the old canvas sits in the new one, picked on a 3x3 grid of anchors
// (top-left first, as a piece is read and grows). A line under the grid
// says how many columns and rows each side gains or loses.

#include "blockwright/canvas_size.h"

#include <QDialog>
#include <QSize>
#include <QString>

#include <array>

class QAbstractButton;
class QButtonGroup;
class QDialogButtonBox;
class QLabel;
class QSpinBox;

namespace bw {

class CanvasSizeDialog : public QDialog {
    Q_OBJECT
public:
    // Starts at the canvas's size, `canvas`, anchored top-left.
    explicit CanvasSizeDialog(QSize canvas, QWidget* parent = nullptr);

    QSize canvasSize() const;
    Anchor anchor() const;
    void setAnchor(Anchor anchor);

    QSpinBox* widthBox() const { return width_; }
    QSpinBox* heightBox() const { return height_; }
    QAbstractButton* anchorButton(Anchor anchor) const { return anchors_[std::size_t(anchor)]; }
    QLabel* summary() const { return summary_; }
    QDialogButtonBox* buttons() const { return buttons_; }

private:
    void refresh();

    QSize old_;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QButtonGroup* group_ = nullptr;
    std::array<QAbstractButton*, 9> anchors_{};
    QLabel* summary_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

// What a change of canvas from `from` to `to` at `anchor` does to each side:
// "2 columns added on the left and 3 on the right; 5 rows cut at the
// bottom", or "No change".
QString canvasSizeSummary(QSize from, QSize to, Anchor anchor);

} // namespace bw
