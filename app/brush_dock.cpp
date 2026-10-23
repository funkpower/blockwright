#include "brush_dock.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/palette.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QScopedValueRollback>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

namespace bw {

namespace {

// The ramp's colours dark to light, a swatch each.
QIcon rampIcon(const Ramp& ramp) {
    constexpr int kSwatch = 8, kHeight = 12;
    QPixmap pm(kSwatch * int(ramp.colours.size()), kHeight);
    QPainter p(&pm);
    for (size_t i = 0; i < ramp.colours.size(); ++i) {
        const Rgb c = kPalette[ramp.colours[i] & 15];
        p.fillRect(int(i) * kSwatch, 0, kSwatch, kHeight, QColor(c.r, c.g, c.b));
    }
    return QIcon(pm);
}

} // namespace

BrushDock::BrushDock(CanvasView* view, QWidget* parent) : QWidget(parent), view_(view) {
    size_ = new QSpinBox;
    size_->setObjectName(QStringLiteral("brushSize"));
    size_->setRange(1, kMaxBrushSize);
    size_->setToolTip(tr("Brush width in cells (Ctrl+[ and Ctrl+] step it)"));

    tip_ = new QComboBox;
    tip_->setObjectName(QStringLiteral("brushTip"));
    tip_->addItem(tr("Square"), int(Tip::Square));
    tip_->addItem(tr("Round"), int(Tip::Round));

    strength_ = new QSpinBox;
    strength_->setObjectName(QStringLiteral("brushStrength"));
    strength_->setRange(0, 100);
    strength_->setSingleStep(25);
    strength_->setSuffix(QStringLiteral("%"));
    strength_->setToolTip(tr("Ramp shade: 25% is a rung a pass, 100% a whole colour"));

    pressure_ = new QComboBox;
    pressure_->setObjectName(QStringLiteral("brushPressure"));
    pressure_->addItem(tr("None"), int(PressureTo::Nothing));
    pressure_->addItem(tr("Size"), int(PressureTo::Size));
    pressure_->addItem(tr("Strength"), int(PressureTo::Strength));
    pressure_->setToolTip(tr("What a stylus's pressure scales; a mouse presses fully"));

    ramp_ = new QComboBox;
    ramp_->setObjectName(QStringLiteral("brushRamp"));
    for (const Ramp& r : namedRamps())
        ramp_->addItem(rampIcon(r), QString::fromStdString(r.name), QString::fromStdString(r.name));
    ramp_->setIconSize(QSize(48, 12));

    lock_ = new QCheckBox(tr("Alpha lock"));
    lock_->setObjectName(QStringLiteral("brushAlphaLock"));
    lock_->setToolTip(tr("Leave transparent cells alone"));

    light_ = new QComboBox;
    light_->setObjectName(QStringLiteral("brushLightDirection"));
    const QString compass[] = {tr("N"), tr("NE"), tr("E"), tr("SE"), tr("S"), tr("SW"), tr("W"), tr("NW")};
    for (int i = 0; i < 8; ++i)
        light_->addItem(compass[i], i);
    light_->setToolTip(tr("Where the rim-light brush's light comes from"));

    symmetry_ = new QComboBox;
    symmetry_->setObjectName(QStringLiteral("brushSymmetry"));
    symmetry_->addItem(tr("Off"), int(SymmetryMode::None));
    symmetry_->addItem(tr("Mirror X"), int(SymmetryMode::MirrorX));
    symmetry_->addItem(tr("Mirror Y"), int(SymmetryMode::MirrorY));
    symmetry_->addItem(tr("Mirror X and Y"), int(SymmetryMode::MirrorXY));
    symmetry_->addItem(tr("Radial"), int(SymmetryMode::Radial));
    symmetry_->setToolTip(tr("What every brush repeats a stroke as (mirror mode, Ctrl+M)"));

    folds_ = new QSpinBox;
    folds_->setObjectName(QStringLiteral("brushSymmetryFolds"));
    folds_->setRange(kMinFolds, kMaxFolds);
    folds_->setToolTip(tr("Radial: how many turns of the stroke"));

    centre_ = new QCheckBox(tr("Radial centre at"));
    centre_->setObjectName(QStringLiteral("brushSymmetryCentre"));
    centre_->setToolTip(tr("Turn about this cell instead of the canvas centre"));
    centreX_ = new QSpinBox;
    centreX_->setObjectName(QStringLiteral("brushSymmetryCentreX"));
    centreY_ = new QSpinBox;
    centreY_->setObjectName(QStringLiteral("brushSymmetryCentreY"));
    auto* centreRow = new QHBoxLayout;
    centreRow->addWidget(centre_);
    centreRow->addWidget(centreX_);
    centreRow->addWidget(centreY_);

    pencil4_ = new QCheckBox(tr("Pencil 4-connected"));
    pencil4_->setObjectName(QStringLiteral("brushPencil4"));
    pencil4_->setToolTip(tr("Half-block pencil lines step only sideways and up or down; off steps diagonally too"));

    auto* form = new QFormLayout(this);
    form->addRow(tr("Size"), size_);
    form->addRow(tr("Tip"), tip_);
    form->addRow(tr("Strength"), strength_);
    form->addRow(tr("Pressure"), pressure_);
    form->addRow(tr("Ramp"), ramp_);
    form->addRow(lock_);
    form->addRow(tr("Light"), light_);
    form->addRow(tr("Symmetry"), symmetry_);
    form->addRow(tr("Folds"), folds_);
    form->addRow(centreRow);
    form->addRow(pencil4_);

    fitCentre();
    sync();
    connect(view_->document(), &Document::reset, this, &BrushDock::fitCentre);
    connect(view_, &CanvasView::brushChanged, this, &BrushDock::sync);
    connect(size_, &QSpinBox::valueChanged, this, &BrushDock::apply);
    connect(strength_, &QSpinBox::valueChanged, this, &BrushDock::apply);
    connect(tip_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(pressure_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(ramp_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(lock_, &QCheckBox::toggled, this, &BrushDock::apply);
    connect(view_, &CanvasView::symmetryChanged, this, &BrushDock::sync);
    connect(light_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(symmetry_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(folds_, &QSpinBox::valueChanged, this, &BrushDock::apply);
    connect(centre_, &QCheckBox::toggled, this, &BrushDock::apply);
    connect(centreX_, &QSpinBox::valueChanged, this, &BrushDock::apply);
    connect(centreY_, &QSpinBox::valueChanged, this, &BrushDock::apply);
    connect(pencil4_, &QCheckBox::toggled, this, &BrushDock::apply);
}

void BrushDock::fitCentre() {
    // A centre on the canvas only. A smaller canvas pulls a set centre in
    // to its edge; the view pulls its saved centre in too, symmetry on or
    // off (CanvasView::fitSymmetryCentre).
    const Canvas& c = view_->document()->canvas();
    centreX_->setRange(0, std::max(0, c.width() - 1));
    centreY_->setRange(0, std::max(0, c.height() - 1));
}

void BrushDock::sync() {
    const QScopedValueRollback<bool> guard(syncing_, true);
    const BrushSettings& s = view_->brushSettings();
    size_->setValue(s.size);
    tip_->setCurrentIndex(tip_->findData(int(s.tip)));
    strength_->setValue(int(std::lround(s.strength * 100)));
    pressure_->setCurrentIndex(pressure_->findData(int(s.pressure)));
    ramp_->setCurrentIndex(ramp_->findData(QString::fromStdString(view_->ramp().name)));
    lock_->setChecked(view_->lockTransparent());
    light_->setCurrentIndex(light_->findData(int(s.lightDirection)));
    const Symmetry& choice = view_->symmetryChoice();
    symmetry_->setCurrentIndex(symmetry_->findData(int(view_->symmetry().mode)));
    folds_->setValue(choice.folds);
    centre_->setChecked(choice.centre.has_value());
    if (choice.centre) {
        centreX_->setValue(choice.centre->first);
        centreY_->setValue(choice.centre->second);
    }
    const bool radial = view_->symmetry().mode == SymmetryMode::Radial;
    folds_->setEnabled(radial);
    centre_->setEnabled(radial);
    centreX_->setEnabled(radial && choice.centre);
    centreY_->setEnabled(radial && choice.centre);
    pencil4_->setChecked(view_->pencilConnectivity() == Connectivity::Four);
}

void BrushDock::apply() {
    if (syncing_)
        return;
    // Read every control first: each change below makes the view's signal
    // sync the controls back from settings not yet applied.
    BrushSettings s = view_->brushSettings();
    s.size = size_->value();
    s.tip = Tip(tip_->currentData().toInt());
    s.strength = strength_->value() / 100.0;
    s.pressure = PressureTo(pressure_->currentData().toInt());
    const auto ramp = findRamp(ramp_->currentData().toString().toStdString());
    const bool lock = lock_->isChecked();
    s.lightDirection = Compass(light_->currentData().toInt());
    Symmetry sym{SymmetryMode(symmetry_->currentData().toInt()), folds_->value(), std::nullopt};
    if (centre_->isChecked())
        sym.centre = std::pair{centreX_->value(), centreY_->value()};
    const Connectivity pencil = pencil4_->isChecked() ? Connectivity::Four : Connectivity::Eight;
    view_->setBrushSettings(s);
    if (ramp)
        view_->setRamp(*ramp);
    view_->setLockTransparent(lock);
    view_->setSymmetry(sym);
    view_->setPencilConnectivity(pencil);
}

} // namespace bw
