#pragma once
// The brush dock: the view's brush settings (size, tip, strength, what a
// stylus's pressure scales), the ramp the ramp brushes walk, the alpha
// lock, the rim brush's light, the symmetry and the pencil's connectivity. It edits the view's and follows it, so the menu's size keys and the
// dock stay in step.

#include <QWidget>

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace bw {

class CanvasView;

class BrushDock : public QWidget {
    Q_OBJECT
public:
    explicit BrushDock(CanvasView* view, QWidget* parent = nullptr);

    QSpinBox* sizeBox() const { return size_; }
    QComboBox* tipBox() const { return tip_; }
    QSpinBox* strengthBox() const { return strength_; }
    QComboBox* pressureBox() const { return pressure_; }
    QComboBox* rampBox() const { return ramp_; }
    QCheckBox* alphaLockBox() const { return lock_; }
    QComboBox* lightBox() const { return light_; }
    QComboBox* symmetryBox() const { return symmetry_; }
    QSpinBox* foldsBox() const { return folds_; }
    QCheckBox* centreBox() const { return centre_; }
    QSpinBox* centreXBox() const { return centreX_; }
    QSpinBox* centreYBox() const { return centreY_; }
    QCheckBox* pencil4Box() const { return pencil4_; }

private:
    // Shows the view's settings.
    void sync();
    // Limits the centre spins to the canvas.
    void fitCentre();
    // Sets the view's from the controls.
    void apply();

    CanvasView* view_;
    QSpinBox* size_;
    QComboBox* tip_;
    QSpinBox* strength_;
    QComboBox* pressure_;
    QComboBox* ramp_;
    QCheckBox* lock_;
    QComboBox* light_;
    QComboBox* symmetry_;
    QSpinBox* folds_;
    QCheckBox* centre_;
    QSpinBox* centreX_;
    QSpinBox* centreY_;
    QCheckBox* pencil4_;
    bool syncing_ = false;
};

} // namespace bw
