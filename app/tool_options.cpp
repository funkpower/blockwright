#include "tool_options.h"

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "select_tool.h"
#include "tool.h"

#include "blockwright/palette.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QStandardItemModel>

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

QString keyText(const QKeySequence& key) { return key.toString(QKeySequence::NativeText); }

using O = ToolOptions::Option;

} // namespace

ToolOptions::Options ToolOptions::optionsFor(const QString& toolId, bool* known) {
    // Every drawing tool paints through the alpha lock and the symmetry; the
    // brushes take the size, tip and pressure; strength is read by the
    // brushes that step along the ramp by it.
    const Options draw = O::AlphaLock | O::Symmetry;
    const Options brush = draw | O::Size | O::Tip | O::Pressure;
    static const QHash<QString, Options> table{
        {QStringLiteral("keyboard"), {}},
        {QStringLiteral("select"), O::PasteMode},
        {QStringLiteral("halfBlock"), draw | O::Pencil},
        {QStringLiteral("shade"), brush},
        {QStringLiteral("rampShade"), brush | O::Strength | O::Ramp},
        {QStringLiteral("form"), brush | O::Strength | O::Ramp | O::Light | O::Form},
        {QStringLiteral("soft"), brush | O::Strength | O::Ramp | O::Hardness},
        {QStringLiteral("despeckle"), brush | O::Ramp},
        {QStringLiteral("smudge"), brush | O::Strength | O::Ramp},
        {QStringLiteral("glyph"), brush | O::Glyph},
        {QStringLiteral("colorize"), brush},
        {QStringLiteral("line"), draw},
        {QStringLiteral("rectangle"), draw | O::Filled},
        {QStringLiteral("ellipse"), draw | O::Filled},
        {QStringLiteral("fill"), draw | O::SampleMerged},
        // The gradient fills every cell it may paint, so mirror mode has
        // nothing to add (GradientTool).
        {QStringLiteral("gradient"), O::AlphaLock | O::Ramp | O::Gradient},
        {QStringLiteral("eyedropper"), O::SampleMerged},
        {QStringLiteral("outline"), brush | O::Ramp | O::OutlineSelection},
        {QStringLiteral("rimLight"), brush | O::Ramp | O::Light | O::RimLint},
        {QStringLiteral("texture"), brush | O::Ramp | O::Light | O::Texture},
        // The clone stamp copies whole cells: no strength, no ramp.
        {QStringLiteral("clone"), brush | O::CloneAligned},
    };
    const auto it = table.find(toolId);
    if (known)
        *known = it != table.end();
    if (it != table.end())
        return *it;
    return brush | O::Strength | O::Ramp | O::Light | O::Form | O::Hardness | O::OutlineSelection | O::Texture;
}

ToolOptions::ToolOptions(CanvasView* view, ToolRegistry* tools, SelectTool* select, QAction* filledShapes,
                         QWidget* parent)
    : QWidget(parent), view_(view), select_(select),
      glyphTool_(qobject_cast<GlyphTool*>(tools->find(QStringLiteral("glyph")))),
      gradientTool_(qobject_cast<GradientTool*>(tools->find(QStringLiteral("gradient")))),
      cloneTool_(qobject_cast<CloneTool*>(tools->find(QStringLiteral("clone")))),
      rimTool_(qobject_cast<RimLightTool*>(tools->find(QStringLiteral("rimLight")))) {
    title_ = new QLabel;
    title_->setObjectName(QStringLiteral("toolOptionsTitle"));
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);

    none_ = new QLabel(tr("This tool has no options."));
    none_->setObjectName(QStringLiteral("toolOptionsNone"));

    size_ = new QSpinBox;
    size_->setObjectName(QStringLiteral("brushSize"));
    size_->setRange(1, kMaxBrushSize);
    size_->setToolTip(tr("Brush width in cells (%1 and %2 step it)")
                          .arg(keyText(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft)),
                               keyText(QKeySequence(Qt::CTRL | Qt::Key_BracketRight))));

    tip_ = new QComboBox;
    tip_->setObjectName(QStringLiteral("brushTip"));
    tip_->addItem(tr("Square"), int(Tip::Square));
    tip_->addItem(tr("Round"), int(Tip::Round));
    tip_->setToolTip(tr("The brush's shape"));

    strength_ = new QSpinBox;
    strength_->setObjectName(QStringLiteral("brushStrength"));
    strength_->setRange(0, 100);
    strength_->setSingleStep(25);
    strength_->setSuffix(QStringLiteral("%"));
    strength_->setToolTip(tr("Ramp shade: 25% is a rung a pass, 100% a whole colour. Smudge: how far it drags"));

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
    ramp_->setToolTip(tr("The colours the brush steps through, dark to light"));

    lock_ = new QCheckBox(tr("Alpha lock"));
    lock_->setObjectName(QStringLiteral("brushAlphaLock"));
    lock_->setToolTip(tr("Leave transparent cells alone (the active layer's alpha lock)"));

    merged_ = new QCheckBox(tr("Sample merged"));
    merged_->setObjectName(QStringLiteral("sampleMerged"));
    merged_->setToolTip(tr("Pick and fill by what the canvas shows, not the active layer alone"));

    light_ = new QComboBox;
    light_->setObjectName(QStringLiteral("brushLightDirection"));
    const QString compass[] = {tr("N"), tr("NE"), tr("E"), tr("SE"), tr("S"), tr("SW"), tr("W"), tr("NW")};
    for (int i = 0; i < 8; ++i)
        light_->addItem(compass[i], i);
    light_->setToolTip(tr("Where the light comes from, for the rim-light, form and texture brushes"));

    form_ = new QComboBox;
    form_->setObjectName(QStringLiteral("brushForm"));
    for (const FormShape f : {FormShape::Dome, FormShape::Cylinder, FormShape::Plane})
        form_->addItem(formShapeName(f), int(f));
    form_->setToolTip(tr("The form brush's shape"));

    rim_ = new QCheckBox(tr("Form rim light"));
    rim_->setObjectName(QStringLiteral("brushRim"));
    rim_->setToolTip(tr("Form brush: a rung of reflected light on the edge away from the light"));

    specular_ = new QCheckBox(tr("Glint"));
    specular_->setObjectName(QStringLiteral("brushSpecular"));
    specular_->setToolTip(tr("Form brush: the ramp's top colour at the brightest point"));

    hardness_ = new QSpinBox;
    hardness_->setObjectName(QStringLiteral("brushHardness"));
    hardness_->setRange(0, 100);
    hardness_->setSingleStep(25);
    hardness_->setSuffix(QStringLiteral("%"));
    hardness_->setToolTip(tr("Soft brush: how little of the tip fades out"));

    outlineSelection_ = new QCheckBox(tr("Outline the selection"));
    outlineSelection_->setObjectName(QStringLiteral("brushOutlineSelection"));
    outlineSelection_->setToolTip(
        tr("Outline brush: the selection is the shape, so its seam with a shape beside it takes the line"));

    textureKind_ = new QComboBox;
    textureKind_->setObjectName(QStringLiteral("brushTextureKind"));
    for (const TextureKind k : {TextureKind::Scales, TextureKind::Foliage, TextureKind::Hoard, TextureKind::Brick,
                                TextureKind::Noise})
        textureKind_->addItem(textureKindName(k), int(k));
    textureKind_->setToolTip(tr("Texture stamp: the lit forms it stamps"));

    textureDensity_ = new QDoubleSpinBox;
    textureDensity_->setObjectName(QStringLiteral("brushTextureDensity"));
    textureDensity_->setRange(kMinTextureDensity, kMaxTextureDensity);
    textureDensity_->setDecimals(1);
    textureDensity_->setSingleStep(0.1);
    textureDensity_->setToolTip(tr("Texture stamp: how closely the forms pack (under 1 leaves gaps); "
                                       "for brick, how small the bricks are; for noise, how fine its grain"));

    textureSeed_ = new QSpinBox;
    textureSeed_->setObjectName(QStringLiteral("brushTextureSeed"));
    textureSeed_->setRange(0, kMaxTextureSeed);
    textureSeed_->setToolTip(tr("Texture stamp: which texture; the same seed paints the same one"));

    symmetry_ = new QComboBox;
    symmetry_->setObjectName(QStringLiteral("brushSymmetry"));
    symmetry_->addItem(tr("Off"), int(SymmetryMode::None));
    symmetry_->addItem(tr("Mirror X"), int(SymmetryMode::MirrorX));
    symmetry_->addItem(tr("Mirror Y"), int(SymmetryMode::MirrorY));
    symmetry_->addItem(tr("Mirror X and Y"), int(SymmetryMode::MirrorXY));
    symmetry_->addItem(tr("Radial"), int(SymmetryMode::Radial));
    symmetry_->setToolTip(tr("What every brush repeats a stroke as (mirror mode, %1)")
                              .arg(keyText(QKeySequence(Qt::CTRL | Qt::Key_M))));

    folds_ = new QSpinBox;
    folds_->setObjectName(QStringLiteral("brushSymmetryFolds"));
    folds_->setRange(kMinFolds, kMaxFolds);
    folds_->setToolTip(tr("Radial: how many turns of the stroke"));

    centre_ = new QCheckBox(tr("Radial centre at"));
    centre_->setObjectName(QStringLiteral("brushSymmetryCentre"));
    centre_->setToolTip(tr("Turn about this cell instead of the canvas centre"));
    centreX_ = new QSpinBox;
    centreX_->setObjectName(QStringLiteral("brushSymmetryCentreX"));
    centreX_->setToolTip(tr("Radial centre: the column"));
    centreY_ = new QSpinBox;
    centreY_->setObjectName(QStringLiteral("brushSymmetryCentreY"));
    centreY_->setToolTip(tr("Radial centre: the row"));
    auto* centreRow = new QHBoxLayout;
    centreRow->addWidget(centre_);
    centreRow->addWidget(centreX_);
    centreRow->addWidget(centreY_);

    pencil4_ = new QCheckBox(tr("Pencil 4-connected"));
    pencil4_->setObjectName(QStringLiteral("brushPencil4"));
    pencil4_->setToolTip(tr("Half-block pencil lines step only sideways and up or down; off steps diagonally too"));

    glyph_ = new QLabel;
    glyph_->setObjectName(QStringLiteral("brushGlyph"));
    glyph_->setToolTip(tr("The glyph brush's glyph: F1-F10 or the glyph bar pick it from the current set"));

    filled_ = new QCheckBox(tr("Filled"));
    filled_->setObjectName(QStringLiteral("shapeFilled"));
    filled_->setToolTip(tr("Rectangles and ellipses are filled, not outlines (Filled Shapes, %1)")
                            .arg(keyText(filledShapes->shortcut())));

    pasteMode_ = new QComboBox;
    pasteMode_->setObjectName(QStringLiteral("selectPasteMode"));
    pasteMode_->addItem(tr("Normal"), int(PasteMode::Normal));
    pasteMode_->addItem(tr("Transparent"), int(PasteMode::Transparent));
    pasteMode_->addItem(tr("Under"), int(PasteMode::Under));
    pasteMode_->setItemData(0, tr("Every cell of the block is written"), Qt::ToolTipRole);
    pasteMode_->setItemData(1, tr("Blank cells don't paste"), Qt::ToolTipRole);
    pasteMode_->setItemData(2, tr("Only into blank cells"), Qt::ToolTipRole);
    pasteMode_->setToolTip(tr("How a paste or a moved selection lands (Selection > Paste Mode)"));

    gradientShape_ = new QComboBox;
    gradientShape_->setObjectName(QStringLiteral("gradientShape"));
    gradientShape_->addItem(tr("Linear"), int(GradientShape::Linear));
    gradientShape_->addItem(tr("Radial"), int(GradientShape::Radial));
    gradientShape_->setToolTip(tr("Ramp gradient: along the drag, or in rings round the press"));

    gradientFrom_ = new QSpinBox;
    gradientFrom_->setObjectName(QStringLiteral("gradientFrom"));
    gradientFrom_->setToolTip(tr("Ramp gradient: the rung at the press, 0 the ramp's darkest (the right button "
                                 "swaps the ends)"));
    gradientTo_ = new QSpinBox;
    gradientTo_->setObjectName(QStringLiteral("gradientTo"));
    gradientTo_->setToolTip(tr("Ramp gradient: the rung at the pointer, four rungs a colour"));

    gradientBanding_ = new QComboBox;
    gradientBanding_->setObjectName(QStringLiteral("gradientBanding"));
    gradientBanding_->addItem(tr("Cell-banded"));
    gradientBanding_->addItem(tr("4×4 Bayer"));
    // Bayer dither is speckle to the ladder lint; it waits on that (PLAN §10).
    if (auto* model = qobject_cast<QStandardItemModel*>(gradientBanding_->model()))
        model->item(1)->setEnabled(false);
    gradientBanding_->setItemData(0, tr("Each ░▒▓ rung a band of whole cells"), Qt::ToolTipRole);
    gradientBanding_->setItemData(1, tr("Not yet: a dither is speckle to the ladder lint"), Qt::ToolTipRole);
    gradientBanding_->setToolTip(tr("Ramp gradient: how the rungs blend"));

    cloneAligned_ = new QCheckBox(tr("Aligned"));
    cloneAligned_->setObjectName(QStringLiteral("cloneAligned"));
    cloneAligned_->setToolTip(tr("Clone stamp: keep the distance to the source from stroke to stroke; off, "
                                 "each stroke copies from the source again (Ctrl+click sets it)"));

    rimLint_ = new QCheckBox(tr("Keep the lint clean"));
    rimLint_->setObjectName(QStringLiteral("rimLint"));
    rimLint_->setToolTip(tr("Rim light: leave a cell unlit where lighting it would add a ladder-lint finding "
                            "(a rung skip, a speck); off, the rim is an accent the lint checks"));

    auto* form = form_layout_ = new QFormLayout(this);
    form->addRow(title_);
    form->addRow(none_);
    form->addRow(tr("Glyph"), glyph_);
    form->addRow(tr("Size"), size_);
    form->addRow(tr("Tip"), tip_);
    form->addRow(tr("Strength"), strength_);
    form->addRow(tr("Pressure"), pressure_);
    form->addRow(tr("Ramp"), ramp_);
    form->addRow(tr("Gradient"), gradientShape_);
    form->addRow(tr("From rung"), gradientFrom_);
    form->addRow(tr("To rung"), gradientTo_);
    form->addRow(tr("Banding"), gradientBanding_);
    form->addRow(tr("Light"), light_);
    form->addRow(rimLint_);
    form->addRow(tr("Form"), form_);
    form->addRow(rim_);
    form->addRow(specular_);
    form->addRow(tr("Hardness"), hardness_);
    form->addRow(outlineSelection_);
    form->addRow(tr("Texture"), textureKind_);
    form->addRow(tr("Density"), textureDensity_);
    form->addRow(tr("Seed"), textureSeed_);
    form->addRow(cloneAligned_);
    form->addRow(filled_);
    form->addRow(pencil4_);
    form->addRow(tr("Paste"), pasteMode_);
    form->addRow(lock_);
    form->addRow(merged_);
    form->addRow(tr("Symmetry"), symmetry_);
    form->addRow(tr("Folds"), folds_);
    form->addRow(centreRow);

    fitCentre();
    sync();
    showFor(tools->active());
    connect(tools, &ToolRegistry::activeChanged, this, &ToolOptions::showFor);
    connect(view_->document(), &Document::reset, this, &ToolOptions::fitCentre);
    connect(view_, &CanvasView::brushChanged, this, &ToolOptions::sync);
    connect(view_, &CanvasView::symmetryChanged, this, &ToolOptions::sync);
    if (glyphTool_)
        connect(glyphTool_, &Tool::changed, this, &ToolOptions::sync);
    if (gradientTool_)
        connect(gradientTool_, &Tool::changed, this, &ToolOptions::sync);
    if (cloneTool_) {
        connect(cloneTool_, &Tool::changed, this, &ToolOptions::sync);
        connect(cloneAligned_, &QCheckBox::toggled, this, [this](bool on) {
            if (!syncing_)
                cloneTool_->setAligned(on);
        });
    }
    if (rimTool_) {
        connect(rimTool_, &Tool::changed, this, &ToolOptions::sync);
        connect(rimLint_, &QCheckBox::toggled, this, [this](bool on) {
            if (!syncing_)
                rimTool_->setKeepLint(on);
        });
    }
    connect(gradientShape_, &QComboBox::currentIndexChanged, this, &ToolOptions::applyGradientShape);
    connect(gradientFrom_, &QSpinBox::valueChanged, this, &ToolOptions::applyGradientFrom);
    connect(gradientTo_, &QSpinBox::valueChanged, this, &ToolOptions::applyGradientTo);
    connect(select_, &Tool::changed, this, &ToolOptions::sync);
    connect(size_, &QSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(strength_, &QSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(tip_, &QComboBox::currentIndexChanged, this, &ToolOptions::apply);
    connect(pressure_, &QComboBox::currentIndexChanged, this, &ToolOptions::apply);
    connect(ramp_, &QComboBox::currentIndexChanged, this, &ToolOptions::apply);
    connect(lock_, &QCheckBox::toggled, this, &ToolOptions::apply);
    connect(merged_, &QCheckBox::toggled, this, &ToolOptions::apply);
    connect(form_, &QComboBox::currentIndexChanged, this, &ToolOptions::apply);
    connect(rim_, &QCheckBox::toggled, this, &ToolOptions::apply);
    connect(specular_, &QCheckBox::toggled, this, &ToolOptions::apply);
    connect(hardness_, &QSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(outlineSelection_, &QCheckBox::toggled, this, &ToolOptions::apply);
    connect(textureKind_, &QComboBox::currentIndexChanged, this, &ToolOptions::apply);
    connect(textureDensity_, &QDoubleSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(textureSeed_, &QSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(light_, &QComboBox::currentIndexChanged, this, &ToolOptions::apply);
    connect(symmetry_, &QComboBox::currentIndexChanged, this, &ToolOptions::apply);
    connect(folds_, &QSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(centre_, &QCheckBox::toggled, this, &ToolOptions::apply);
    connect(centreX_, &QSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(centreY_, &QSpinBox::valueChanged, this, &ToolOptions::apply);
    connect(pencil4_, &QCheckBox::toggled, this, &ToolOptions::apply);
    // Filled Shapes is the menu toggle's; paste mode is the select tool's.
    filled_->setChecked(filledShapes->isChecked());
    connect(filled_, &QCheckBox::toggled, filledShapes, &QAction::setChecked);
    connect(filledShapes, &QAction::toggled, filled_, &QCheckBox::setChecked);
    connect(pasteMode_, &QComboBox::activated, this, [this] {
        select_->setPasteMode(PasteMode(pasteMode_->currentData().toInt()));
    });
}

void ToolOptions::showFor(Tool* tool) {
    const Options o = tool ? optionsFor(tool->id()) : Options();
    shown_ = o;
    title_->setText(tool ? QString(tool->name()).remove(QLatin1Char('&')) : QString());
    QFormLayout* f = form_layout_;
    f->setRowVisible(none_, !o);
    f->setRowVisible(glyph_, o.testFlag(O::Glyph));
    f->setRowVisible(size_, o.testFlag(O::Size));
    f->setRowVisible(tip_, o.testFlag(O::Tip));
    f->setRowVisible(strength_, o.testFlag(O::Strength));
    f->setRowVisible(pressure_, o.testFlag(O::Pressure));
    f->setRowVisible(ramp_, o.testFlag(O::Ramp));
    f->setRowVisible(gradientShape_, o.testFlag(O::Gradient));
    f->setRowVisible(gradientFrom_, o.testFlag(O::Gradient));
    f->setRowVisible(gradientTo_, o.testFlag(O::Gradient));
    f->setRowVisible(gradientBanding_, o.testFlag(O::Gradient));
    f->setRowVisible(light_, o.testFlag(O::Light));
    f->setRowVisible(form_, o.testFlag(O::Form));
    f->setRowVisible(rim_, o.testFlag(O::Form));
    f->setRowVisible(specular_, o.testFlag(O::Form));
    f->setRowVisible(hardness_, o.testFlag(O::Hardness));
    f->setRowVisible(outlineSelection_, o.testFlag(O::OutlineSelection));
    f->setRowVisible(textureKind_, o.testFlag(O::Texture));
    f->setRowVisible(textureDensity_, o.testFlag(O::Texture));
    f->setRowVisible(textureSeed_, o.testFlag(O::Texture));
    f->setRowVisible(cloneAligned_, o.testFlag(O::CloneAligned));
    f->setRowVisible(rimLint_, o.testFlag(O::RimLint));
    f->setRowVisible(filled_, o.testFlag(O::Filled));
    f->setRowVisible(pencil4_, o.testFlag(O::Pencil));
    f->setRowVisible(pasteMode_, o.testFlag(O::PasteMode));
    f->setRowVisible(lock_, o.testFlag(O::AlphaLock));
    f->setRowVisible(merged_, o.testFlag(O::SampleMerged));
    f->setRowVisible(symmetry_, o.testFlag(O::Symmetry));
    f->setRowVisible(folds_, o.testFlag(O::Symmetry));
    f->setRowVisible(f->rowCount() - 1, o.testFlag(O::Symmetry));  // the radial centre
}

void ToolOptions::fitCentre() {
    // A centre on the canvas only. A smaller canvas pulls a set centre in
    // to its edge; the view pulls its saved centre in too, symmetry on or
    // off (CanvasView::fitSymmetryCentre).
    const Canvas& c = view_->document()->canvas();
    centreX_->setRange(0, std::max(0, c.width() - 1));
    centreY_->setRange(0, std::max(0, c.height() - 1));
}

void ToolOptions::sync() {
    const QScopedValueRollback<bool> guard(syncing_, true);
    const BrushSettings& s = view_->brushSettings();
    size_->setValue(s.size);
    tip_->setCurrentIndex(tip_->findData(int(s.tip)));
    strength_->setValue(int(std::lround(s.strength * 100)));
    pressure_->setCurrentIndex(pressure_->findData(int(s.pressure)));
    ramp_->setCurrentIndex(ramp_->findData(QString::fromStdString(view_->ramp().name)));
    lock_->setChecked(view_->lockTransparent());
    merged_->setChecked(view_->sampleMerged());
    light_->setCurrentIndex(light_->findData(int(s.lightDirection)));
    const FormOptions& f = view_->formOptions();
    form_->setCurrentIndex(form_->findData(int(f.shape)));
    rim_->setChecked(f.rim);
    specular_->setChecked(f.specular);
    hardness_->setValue(int(std::lround(view_->hardness() * 100)));
    outlineSelection_->setChecked(view_->outlineShape() == OutlineShape::Selection);
    const TextureOptions& t = view_->textureOptions();
    textureKind_->setCurrentIndex(textureKind_->findData(int(t.kind)));
    textureDensity_->setValue(t.density);
    textureSeed_->setValue(t.seed);
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
    if (glyphTool_) {
        const char32_t g = glyphTool_->glyph();
        glyph_->setText(QString::fromUcs4(&g, 1));
    }
    if (gradientTool_) {
        // A rung past the ramp's top shows as the top, which is what the
        // gradient paints; the tool keeps it for a longer ramp.
        const int top = view_->ramp().topRung();
        gradientShape_->setCurrentIndex(gradientShape_->findData(int(gradientTool_->shape())));
        gradientFrom_->setRange(0, top);
        gradientTo_->setRange(0, top);
        gradientFrom_->setValue(gradientTool_->fromRung());
        gradientTo_->setValue(gradientTool_->toRung());
    }
    if (cloneTool_)
        cloneAligned_->setChecked(cloneTool_->aligned());
    if (rimTool_)
        rimLint_->setChecked(rimTool_->keepLint());
    // A Library stamp floats in a mode of its own; the dock shows it.
    pasteMode_->setCurrentIndex(pasteMode_->findData(int(select_->effectivePasteMode())));
}

void ToolOptions::apply() {
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
    const bool merged = merged_->isChecked();
    s.lightDirection = Compass(light_->currentData().toInt());
    FormOptions f;
    f.shape = FormShape(form_->currentData().toInt());
    f.rim = rim_->isChecked();
    f.specular = specular_->isChecked();
    const double hardness = hardness_->value() / 100.0;
    const OutlineShape outline = outlineSelection_->isChecked() ? OutlineShape::Selection : OutlineShape::Ramp;
    const TextureOptions texture{TextureKind(textureKind_->currentData().toInt()), textureDensity_->value(),
                                 textureSeed_->value()};
    Symmetry sym{SymmetryMode(symmetry_->currentData().toInt()), folds_->value(), std::nullopt};
    if (centre_->isChecked())
        sym.centre = std::pair{centreX_->value(), centreY_->value()};
    const Connectivity pencil = pencil4_->isChecked() ? Connectivity::Four : Connectivity::Eight;
    view_->setBrushSettings(s);
    if (ramp)
        view_->setRamp(*ramp);
    view_->setLockTransparent(lock);
    view_->setSampleMerged(merged);
    view_->setFormOptions(f);
    view_->setHardness(hardness);
    view_->setOutlineShape(outline);
    view_->setTextureOptions(texture);
    view_->setSymmetry(sym);
    view_->setPencilConnectivity(pencil);
}

// Each control writes its own setting only: a rung the spinboxes show
// clamped to a short ramp's top stays as set on the tool.
void ToolOptions::applyGradientShape() {
    if (!syncing_ && gradientTool_)
        gradientTool_->setShape(GradientShape(gradientShape_->currentData().toInt()));
}

void ToolOptions::applyGradientFrom(int from) {
    if (!syncing_ && gradientTool_)
        gradientTool_->setRungs(from, gradientTool_->toRung());
}

void ToolOptions::applyGradientTo(int to) {
    if (!syncing_ && gradientTool_)
        gradientTool_->setRungs(gradientTool_->fromRung(), to);
}

} // namespace bw
