#pragma once
// The Move/Transform tool's settings in the Tool Options dock (PLAN §4,
// §11): the active reference layer's place and size in canvas pixels, and
// its scale against the picture's own size, as numbers. With Keep aspect on
// (the default) a new width or height takes the other side with it; a new
// scale sizes both sides from the picture's own. Each change is one undo
// step, "Move Layer" or "Resize Layer". On any other layer, or a hidden or
// locked one, the fields are off and a line says why.

#include <QSize>
#include <QWidget>

#include <memory>
#include <optional>
#include <string>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace bw {

class CanvasView;
struct Layer;

class TransformOptions : public QWidget {
    Q_OBJECT
public:
    explicit TransformOptions(CanvasView* view, QWidget* parent = nullptr);

    QSpinBox* xBox() const { return x_; }
    QSpinBox* yBox() const { return y_; }
    QSpinBox* widthBox() const { return width_; }
    QSpinBox* heightBox() const { return height_; }
    QCheckBox* keepAspectBox() const { return keep_; }
    QDoubleSpinBox* scaleBox() const { return scale_; }
    QLabel* noteLabel() const { return note_; }

private:
    // The active layer, if it is a reference layer the fields can place.
    const Layer* placeable() const;
    // The picture's own size, read from its header; empty if unreadable.
    QSize natural(const Layer& layer);
    // The width over the height Keep aspect holds the active layer to. It
    // is worked out once, not from each rounded size the last edit left,
    // so edits there and back land where they began; a scale sets it to
    // the picture's own, and any other change to the document (a free
    // edit, a drag, undo, another layer) works it out again from the size
    // it leaves.
    double keptAspect(const Layer& layer);
    void sync();
    // Sets the active reference layer's place and size, one undo step.
    void apply(int x, int y, int width, int height);
    void applyWidth(int width);
    void applyHeight(int height);
    void applyScale(double percent);
    // The scale an arrow step of `steps` sets: from the size there is, so no
    // side moves the wrong way whatever the box shows; none at the limit.
    std::optional<double> scaleStep(int steps);

    CanvasView* view_;
    QSpinBox* x_;
    QSpinBox* y_;
    QSpinBox* width_;
    QSpinBox* height_;
    QCheckBox* keep_;
    QDoubleSpinBox* scale_;
    QLabel* note_;
    QString scaleTip_;
    bool syncing_ = false;
    std::shared_ptr<const std::string> naturalOf_;
    QSize natural_;
    double aspect_ = 1;
    bool aspectKept_ = false;  // aspect_ holds: only kept edits since it was worked out
    bool keeping_ = false;     // a kept edit is landing
    bool scaleKept_ = false;   // scale_ shows the scale asked for: only scales since it was
    bool scaling_ = false;     // a scale is landing
};

} // namespace bw
