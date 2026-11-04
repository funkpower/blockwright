#include "tool_options.h"

#include "shortcut_settings.h"

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "flow_row.h"
#include "frame_tool.h"
#include "help_text.h"
#include "select_tool.h"
#include "settings.h"
#include "text_options.h"
#include "text_tool.h"
#include "tool.h"
#include "tool_illustration.h"
#include "transform_options.h"
#include "whats_this.h"

#include "blockwright/frame.h"
#include "blockwright/palette.h"
#include "blockwright/render.h"

#include <QAction>
#include <QBoxLayout>
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
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace bw {

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

namespace {

const QString kShowHelpKey = QStringLiteral("toolOptions/showHelp");

QString keyText(const QKeySequence& key) { return key.toString(QKeySequence::NativeText); }

// `text` with ", <key>" for its %1, or nothing when `a` has no key.
QString withKey(const QString& text, const QAction* a) {
    return text.arg(a->shortcut().isEmpty() ? QString() : QStringLiteral(", ") + keyText(a->shortcut()));
}

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
        {QStringLiteral("move"), O::Transform},
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
        {QStringLiteral("frame"), draw | O::FrameStyle},
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
        // Stars ignore the ramp; haze and smoke walk it.
        {QStringLiteral("spray"), brush | O::Ramp | O::Spray},
        {QStringLiteral("text"), O::Text},
    };
    const auto it = table.find(toolId);
    if (known)
        *known = it != table.end();
    if (it != table.end())
        return *it;
    return brush | O::Strength | O::Ramp | O::Light | O::Form | O::Hardness | O::OutlineSelection | O::Texture |
           O::Spray;
}

void ToolOptions::followKeys(QAction* smaller, QAction* larger, QAction* mirror) {
    keepToolTip(size_, {smaller, larger}, [smaller, larger] {
        if (smaller->shortcut().isEmpty() || larger->shortcut().isEmpty())
            return tr("Brush width in cells (Tools > Smaller Brush and Larger Brush step it)");
        return tr("Brush width in cells (%1 and %2 step it)")
            .arg(keyText(smaller->shortcut()), keyText(larger->shortcut()));
    });
    keepToolTip(symmetry_, {mirror}, [mirror] {
        return withKey(tr("What every brush repeats a stroke as (mirror mode%1)"), mirror);
    });
}

ToolOptions::ToolOptions(CanvasView* view, ToolRegistry* tools, SelectTool* select, QAction* filledShapes,
                         QWidget* parent)
    : QWidget(parent), view_(view), select_(select),
      glyphTool_(qobject_cast<GlyphTool*>(tools->find(QStringLiteral("glyph")))),
      gradientTool_(qobject_cast<GradientTool*>(tools->find(QStringLiteral("gradient")))),
      cloneTool_(qobject_cast<CloneTool*>(tools->find(QStringLiteral("clone")))),
      sprayTool_(qobject_cast<SprayTool*>(tools->find(QStringLiteral("spray")))),
      frameTool_(qobject_cast<FrameTool*>(tools->find(QStringLiteral("frame")))),
      rimTool_(qobject_cast<RimLightTool*>(tools->find(QStringLiteral("rimLight")))) {
    title_ = new QLabel;
    title_->setObjectName(QStringLiteral("toolOptionsTitle"));
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    // A long name wraps rather than push the help toggle past the narrowest
    // column when the two share a line (#236).
    title_->setWordWrap(true);

    none_ = new QLabel(tr("This tool has no options."));
    none_->setObjectName(QStringLiteral("toolOptionsNone"));

    helpToggle_ = new QToolButton;
    helpToggle_->setObjectName(QStringLiteral("toolHelpToggle"));
    helpToggle_->setText(tr("About this tool"));
    helpToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    helpToggle_->setAutoRaise(true);
    helpToggle_->setCheckable(true);
    helpToggle_->setToolTip(tr("Show or hide what the tool does and what its options change"));
    help_ = new QLabel;
    help_->setObjectName(QStringLiteral("toolHelp"));
    help_->setTextFormat(Qt::MarkdownText);
    help_->setWordWrap(true);
    help_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(help_, &QLabel::linkActivated, this, &ToolOptions::shortcutsRequested);
    illustration_ = new QLabel;
    illustration_->setObjectName(QStringLiteral("toolIllustration"));

    size_ = new QSpinBox;
    size_->setObjectName(QStringLiteral("brushSize"));
    size_->setRange(1, kMaxBrushSize);
    size_->setToolTip(tr("Brush width in cells"));  // and its keys, from followKeys

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
    symmetry_->setToolTip(tr("What every brush repeats a stroke as (mirror mode)"));

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
    keepToolTip(filled_, {filledShapes}, [filledShapes] {
        return withKey(tr("Rectangles and ellipses are filled, not outlines (Filled Shapes%1)"), filledShapes);
    });

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
    gradientBanding_->addItem(tr("Cell-banded"), int(GradientBanding::Cells));
    gradientBanding_->addItem(tr("4×4 Bayer"), int(GradientBanding::Bayer));
    gradientBanding_->setItemData(0, tr("Each ░▒▓ rung a band of whole cells"), Qt::ToolTipRole);
    gradientBanding_->setItemData(1, tr("The same bands, each edge mixed in a 4×4 ordered dither (the lint "
                                        "doesn't call the dither's cells specks)"),
                                  Qt::ToolTipRole);
    gradientBanding_->setToolTip(tr("Ramp gradient: how the rungs blend"));

    cloneAligned_ = new QCheckBox(tr("Aligned"));
    cloneAligned_->setObjectName(QStringLiteral("cloneAligned"));
    cloneAligned_->setToolTip(tr("Clone stamp: keep the distance to the source from stroke to stroke; off, "
                                 "each stroke copies from the source again (Ctrl+click sets it)"));

    frameStyle_ = new QComboBox;
    frameStyle_->setObjectName(QStringLiteral("frameStyle"));
    frameStyle_->setIconSize(QSize(40, 48));
    // the names are long; the list opens wide enough to show them
    frameStyle_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    frameStyle_->setMinimumContentsLength(8);
    for (const FrameStyle& st : bundledFrameStyles()) {
        // the style drawn small, its open middle shown as the canvas shows it
        const auto thumb = drawFrame(st, std::max(5, st.minimumWidth()), std::max(3, st.minimumHeight()));
        const QIcon icon = thumb ? QIcon(QPixmap::fromImage(renderCanvas(*thumb, 8))) : QIcon();
        frameStyle_->addItem(icon, QString::fromStdString(st.name), QString::fromStdString(st.id));
    }
    frameStyle_->setToolTip(tr("Frame: the border drawn to fit the drag"));
    frameColours_ = new QComboBox;
    frameColours_->setObjectName(QStringLiteral("frameColours"));
    frameColours_->addItem(tr("Style colours"), false);
    frameColours_->addItem(tr("Pen colours"), true);
    frameColours_->setItemData(0, tr("The frame in the colours it was drawn in"), Qt::ToolTipRole);
    frameColours_->setItemData(1, tr("The frame recoloured to the pen's foreground and background; line, half-block and shaded cells keep the background under them"), Qt::ToolTipRole);
    frameColours_->setToolTip(tr("Frame: its own colours, or the pen's"));

    sprayKind_ = new QComboBox;
    sprayKind_->setObjectName(QStringLiteral("sprayKind"));
    for (const SprayKind k : {SprayKind::Stars, SprayKind::Haze, SprayKind::Smoke})
        sprayKind_->addItem(sprayKindName(k), int(k));
    sprayKind_->setItemData(0, tr("Small glyphs in the pen's colour, on open ground only"), Qt::ToolTipRole);
    sprayKind_->setItemData(1, tr("A ░▒ lift along the ramp in long, low bands"), Qt::ToolTipRole);
    sprayKind_->setItemData(2, tr("A ░▒ lift along the ramp in tall, curling wisps"), Qt::ToolTipRole);
    sprayKind_->setToolTip(tr("Glyph spray: what it scatters (the right button takes stars off, or darkens)"));
    sprayDensity_ = new QSpinBox;
    sprayDensity_->setObjectName(QStringLiteral("sprayDensity"));
    sprayDensity_->setRange(kMinSprayDensity, kMaxSprayDensity);
    sprayDensity_->setSuffix(QStringLiteral("%"));
    sprayDensity_->setToolTip(tr("Glyph spray: how many stars, or how much of the canvas the haze or smoke covers"));
    spraySeed_ = new QSpinBox;
    spraySeed_->setObjectName(QStringLiteral("spraySeed"));
    spraySeed_->setRange(0, kMaxSpraySeed);
    spraySeed_->setToolTip(tr("Glyph spray: the seed fixes the pattern to the canvas, so strokes over one another "
                              "paint the same one"));
    rimLint_ = new QCheckBox(tr("Keep the lint clean"));
    rimLint_->setObjectName(QStringLiteral("rimLint"));
    rimLint_->setToolTip(tr("Rim light: leave a cell unlit where lighting it would add a ladder-lint finding "
                            "(a rung skip, a speck); off, the rim is an accent the lint checks"));

    if (auto* textTool = qobject_cast<TextTool*>(tools->find(QStringLiteral("text")))) {
        text_ = new TextOptions(view, textTool);
        text_->setObjectName(QStringLiteral("textOptions"));
    }

    transform_ = new TransformOptions(view);
    transform_->setObjectName(QStringLiteral("transformOptions"));

    // The heading and help sit above the settings' form rather than in it:
    // a form row lays a word-wrapped label out at its preferred height, not
    // its height for the column's width, and cuts the help off.
    auto* box = new QVBoxLayout(this);
    auto* form = form_layout_ = new FlowForm;
    form->setContentsMargins(0, 0, 0, 0);
    // Rows a little closer than the style's spacing: the column is short of
    // height for the tools with the most settings (#236).
    form->setVerticalSpacing(4);
    // What's This on a setting shows its tooltip (whats_this.h).
    whatsThisFromToolTips(this);
    // Settings that belong together share a row while the column is wide
    // enough and wrap under each other when it isn't (#236). The row's own
    // label goes with its first field, which shows whenever any of the row
    // does.
    const auto pairRow = [this, form](const QString& text, QWidget* field,
                                      std::initializer_list<std::pair<QString, QWidget*>> beside) {
        auto* label = new QLabel(text);
        label->setBuddy(field);
        auto* row = new FlowRow;
        shared_.insert(field, {label, row});
        row->addWidget(field);
        for (const auto& [besideText, besideField] : beside) {
            auto* besideLabel = new QLabel(besideText);
            shared_.insert(besideField, {besideLabel, row});
            row->addPair(besideLabel, besideField);
        }
        form->addRow(label, row);
    };
    // The title and the help toggle sit side by side; while the picture
    // shows they stack beside it, and the picture goes under them when the
    // column is too narrow for both.
    titleBlock_ = new QBoxLayout(QBoxLayout::LeftToRight);
    titleBlock_->addWidget(title_, 1);
    helpToggle_->setSizePolicy(QSizePolicy::Fixed, helpToggle_->sizePolicy().verticalPolicy());
    titleBlock_->addWidget(helpToggle_);
    illustration_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* heading = new FlowRow;
    heading->addLayout(titleBlock_);
    heading->addWidget(illustration_);
    box->setContentsMargins(6, 6, 6, 6);
    box->addLayout(heading);
    box->addWidget(help_);
    // The form takes the height to spare: the rows stay at the top, and the
    // Text tool's face list grows into it.
    box->addLayout(form, 1);
    form->addRow(none_);
    form->addRow(tr("Glyph"), glyph_);
    pairRow(tr("Size"), size_, {{tr("Tip"), tip_}});
    pairRow(tr("Pressure"), pressure_, {{tr("Strength"), strength_}});
    form->addRow(tr("Ramp"), ramp_);
    pairRow(tr("Gradient"), gradientShape_, {{tr("Banding"), gradientBanding_}});
    pairRow(tr("From rung"), gradientFrom_, {{tr("To rung"), gradientTo_}});
    pairRow(tr("Light"), light_, {{tr("Form"), form_}});
    form->addRow(rimLint_);
    auto* formChecks = new FlowRow;
    formChecks->addWidget(rim_);
    formChecks->addWidget(specular_);
    shared_.insert(rim_, {nullptr, formChecks});
    shared_.insert(specular_, {nullptr, formChecks});
    form->addRow(formChecks);
    form->addRow(tr("Hardness"), hardness_);
    form->addRow(tr("Texture"), textureKind_);
    pairRow(tr("Density"), textureDensity_, {{tr("Seed"), textureSeed_}});
    form->addRow(cloneAligned_);
    form->addRow(tr("Frame"), frameStyle_);
    form->addRow(tr("Colours"), frameColours_);
    form->addRow(tr("Spray"), sprayKind_);
    pairRow(tr("Density"), sprayDensity_, {{tr("Seed"), spraySeed_}});
    if (text_)
        form->addRow(text_);
    form->addRow(transform_);
    form->addRow(filled_);
    form->addRow(pencil4_);
    form->addRow(tr("Paste"), pasteMode_);
    // The checkboxes every tool may have share a line with the outline
    // brush's.
    auto* checks = new FlowRow;
    for (QWidget* check : {static_cast<QWidget*>(outlineSelection_), static_cast<QWidget*>(lock_),
                           static_cast<QWidget*>(merged_)}) {
        checks->addWidget(check);
        shared_.insert(check, {nullptr, checks});
    }
    form->addRow(checks);
    pairRow(tr("Symmetry"), symmetry_, {{tr("Folds"), folds_}});
    form->addRow(centreRow);

    fitCentre();
    sync();
    helpToggle_->setChecked(appSettings()->value(kShowHelpKey, true).toBool());
    showFor(tools->active());
    connect(helpToggle_, &QToolButton::toggled, this, &ToolOptions::showHelp);
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
    if (sprayTool_) {
        connect(sprayTool_, &Tool::changed, this, &ToolOptions::sync);
        connect(sprayKind_, &QComboBox::currentIndexChanged, this, [this] {
            if (!syncing_)
                sprayTool_->setKind(SprayKind(sprayKind_->currentData().toInt()));
        });
        connect(sprayDensity_, &QSpinBox::valueChanged, this, [this](int v) {
            if (!syncing_)
                sprayTool_->setDensity(v);
        });
        connect(spraySeed_, &QSpinBox::valueChanged, this, [this](int v) {
            if (!syncing_)
                sprayTool_->setSeed(v);
        });
    }
    if (frameTool_) {
        connect(frameTool_, &Tool::changed, this, &ToolOptions::sync);
        connect(frameStyle_, &QComboBox::currentIndexChanged, this, [this] {
            if (!syncing_)
                frameTool_->setStyle(frameStyle_->currentData().toString());
        });
        connect(frameColours_, &QComboBox::currentIndexChanged, this, [this] {
            if (!syncing_)
                frameTool_->setPenColours(frameColours_->currentData().toBool());
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
    connect(gradientBanding_, &QComboBox::currentIndexChanged, this, &ToolOptions::applyGradientBanding);
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
    const QString help = tool ? help::toolText(tool->id()) : QString();
    help_->setText(help.isEmpty() ? help
                                  : help + tr("\n\n[All keyboard shortcuts](shortcuts) (Help > Keyboard Shortcuts)"));
    const help::Illustration* picture = tool ? help::toolIllustration(tool->id()) : nullptr;
    illustration_->setPixmap(picture ? QPixmap::fromImage(picture->image) : QPixmap());
    illustration_->setAccessibleName(picture ? tr("Picture of the %1 tool at work").arg(title_->text()) : QString());
    QFormLayout* f = form_layout_;
    // A setting in a row of its own shows with its row; one that shares a
    // row shows with its label, and the row with any of them (a hidden
    // row takes no spacing).
    QList<std::pair<QWidget*, bool>> sharing;
    QHash<QLayout*, bool> sharedRows;
    const auto show = [this, f, &sharing, &sharedRows](QWidget* field, bool on) {
        if (const auto it = shared_.constFind(field); it != shared_.cend()) {
            sharing.append({field, on});
            sharedRows[it->row] |= on;
        } else {
            f->setRowVisible(field, on);
        }
    };
    helpToggle_->setVisible(!help.isEmpty());
    illustration_->setVisible(picture && !help.isEmpty() && helpToggle_->isChecked());
    help_->setVisible(!help.isEmpty() && helpToggle_->isChecked());
    stackTitle();
    helpToggle_->setArrowType(helpToggle_->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    f->setRowVisible(none_, !o);
    show(glyph_, o.testFlag(O::Glyph));
    show(size_, o.testFlag(O::Size));
    show(tip_, o.testFlag(O::Tip));
    show(strength_, o.testFlag(O::Strength));
    show(pressure_, o.testFlag(O::Pressure));
    show(ramp_, o.testFlag(O::Ramp));
    show(gradientShape_, o.testFlag(O::Gradient));
    show(gradientFrom_, o.testFlag(O::Gradient));
    show(gradientTo_, o.testFlag(O::Gradient));
    show(gradientBanding_, o.testFlag(O::Gradient));
    show(light_, o.testFlag(O::Light));
    show(form_, o.testFlag(O::Form));
    show(rim_, o.testFlag(O::Form));
    show(specular_, o.testFlag(O::Form));
    show(hardness_, o.testFlag(O::Hardness));
    show(outlineSelection_, o.testFlag(O::OutlineSelection));
    show(textureKind_, o.testFlag(O::Texture));
    show(textureDensity_, o.testFlag(O::Texture));
    show(textureSeed_, o.testFlag(O::Texture));
    show(cloneAligned_, o.testFlag(O::CloneAligned));
    show(frameStyle_, o.testFlag(O::FrameStyle));
    show(frameColours_, o.testFlag(O::FrameStyle));
    show(sprayKind_, o.testFlag(O::Spray));
    show(sprayDensity_, o.testFlag(O::Spray));
    show(spraySeed_, o.testFlag(O::Spray));
    show(rimLint_, o.testFlag(O::RimLint));
    if (text_)
        show(text_, o.testFlag(O::Text));
    show(transform_, o.testFlag(O::Transform));
    show(filled_, o.testFlag(O::Filled));
    show(pencil4_, o.testFlag(O::Pencil));
    show(pasteMode_, o.testFlag(O::PasteMode));
    show(lock_, o.testFlag(O::AlphaLock));
    show(merged_, o.testFlag(O::SampleMerged));
    show(symmetry_, o.testFlag(O::Symmetry));
    show(folds_, o.testFlag(O::Symmetry));
    f->setRowVisible(f->rowCount() - 1, o.testFlag(O::Symmetry));  // the radial centre
    for (auto it = sharedRows.cbegin(); it != sharedRows.cend(); ++it)
        f->setRowVisible(it.key(), it.value());
    for (const auto& [field, on] : sharing) {
        field->setVisible(on);
        if (QLabel* label = shared_.value(field).label)
            label->setVisible(on);
    }
}

void ToolOptions::showHelp(bool shown) {
    illustration_->setVisible(shown && !help_->text().isEmpty() && !illustration_->pixmap().isNull());
    help_->setVisible(shown && !help_->text().isEmpty());
    stackTitle();
    helpToggle_->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    appSettings()->setValue(kShowHelpKey, shown);
}

void ToolOptions::stackTitle() {
    titleBlock_->setDirection(illustration_->isVisibleTo(this) ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
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
        gradientBanding_->setCurrentIndex(gradientBanding_->findData(int(gradientTool_->banding())));
    }
    if (cloneTool_)
        cloneAligned_->setChecked(cloneTool_->aligned());
    if (sprayTool_) {
        const SprayOptions& o = sprayTool_->options();
        sprayKind_->setCurrentIndex(sprayKind_->findData(int(o.kind)));
        sprayDensity_->setValue(o.density);
        spraySeed_->setValue(o.seed);
    }
    if (frameTool_) {
        frameStyle_->setCurrentIndex(frameStyle_->findData(frameTool_->styleId()));
        frameColours_->setCurrentIndex(frameColours_->findData(frameTool_->penColours()));
    }
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

void ToolOptions::applyGradientBanding() {
    if (!syncing_ && gradientTool_)
        gradientTool_->setBanding(GradientBanding(gradientBanding_->currentData().toInt()));
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
