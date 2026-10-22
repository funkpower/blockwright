#include "brush_dock.h"

#include "canvas_view.h"

#include "blockwright/palette.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QPainter>
#include <QPixmap>
#include <QScopedValueRollback>
#include <QSpinBox>

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

    auto* form = new QFormLayout(this);
    form->addRow(tr("Size"), size_);
    form->addRow(tr("Tip"), tip_);
    form->addRow(tr("Strength"), strength_);
    form->addRow(tr("Pressure"), pressure_);
    form->addRow(tr("Ramp"), ramp_);
    form->addRow(lock_);

    sync();
    connect(view_, &CanvasView::brushChanged, this, &BrushDock::sync);
    connect(size_, &QSpinBox::valueChanged, this, &BrushDock::apply);
    connect(strength_, &QSpinBox::valueChanged, this, &BrushDock::apply);
    connect(tip_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(pressure_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(ramp_, &QComboBox::currentIndexChanged, this, &BrushDock::apply);
    connect(lock_, &QCheckBox::toggled, this, &BrushDock::apply);
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
}

void BrushDock::apply() {
    if (syncing_)
        return;
    // Read every control first: each change below makes the view's signal
    // sync the controls back from settings not yet applied.
    BrushSettings s;
    s.size = size_->value();
    s.tip = Tip(tip_->currentData().toInt());
    s.strength = strength_->value() / 100.0;
    s.pressure = PressureTo(pressure_->currentData().toInt());
    const auto ramp = findRamp(ramp_->currentData().toString().toStdString());
    const bool lock = lock_->isChecked();
    view_->setBrushSettings(s);
    if (ramp)
        view_->setRamp(*ramp);
    view_->setLockTransparent(lock);
}

} // namespace bw
