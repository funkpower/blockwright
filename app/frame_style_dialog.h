#pragma once
// Selection > Save as Frame Style... (#345 slice 3): a name, the corners'
// size (the insets that cut the selection into corners, edges and a
// middle), whether the middle is drawn, and whether the cells keep the
// background under them; with a preview of the style drawn larger than the
// selection, so the repeat shows. OK waits until problem() is empty.

#include "blockwright/canvas.h"
#include "blockwright/frame.h"

#include <QDialog>

#include <optional>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace bw {

class FrameStyleDialog : public QDialog {
    Q_OBJECT
public:
    explicit FrameStyleDialog(const Canvas& cells, QWidget* parent = nullptr);

    QString name() const;
    int left() const;
    int right() const;
    int top() const;
    int bottom() const;
    bool fill() const;
    bool keepBackground() const;
    // The style the fields cut (frameStyleFromCells), or nothing.
    std::optional<FrameStyle> style() const;
    // Why the fields can't be saved, or empty.
    QString problem() const;

    QLineEdit* nameEdit() const { return name_; }
    QSpinBox* leftSpin() const { return left_; }
    QSpinBox* rightSpin() const { return right_; }
    QSpinBox* topSpin() const { return top_; }
    QSpinBox* bottomSpin() const { return bottom_; }
    QCheckBox* fillBox() const { return fill_; }
    QCheckBox* keepBackgroundBox() const { return keepBg_; }
    QLabel* preview() const { return preview_; }
    QDialogButtonBox* buttons() const { return buttons_; }

private:
    void check();

    Canvas cells_;
    QLineEdit* name_;
    QSpinBox *left_, *right_, *top_, *bottom_;
    QCheckBox* fill_;
    QCheckBox* keepBg_;
    QLabel* preview_;
    QLabel* problem_;
    QDialogButtonBox* buttons_;
};

} // namespace bw
