#pragma once
// The Tool Options dock (PLAN §11): the settings of the active tool and
// only those. It holds the view's brush settings (size, tip, strength, what
// a stylus's pressure scales), the ramp the ramp brushes walk, the alpha
// lock, the light direction (the rim-light and form brushes), the form
// brush's form, rim light and glint, the soft brush's hardness, what the
// outline brush takes as the shape, the texture stamp's kind, density and
// seed, the symmetry and the pencil's
// connectivity, the glyph brush's glyph, Filled Shapes for
// the rectangle and ellipse, the ramp gradient's shape, rungs and banding,
// the select tool's paste mode, sample merged for the eyedropper and
// fill, the clone stamp's Aligned, the rim light's Keep the lint clean,
// and the glyph spray's kind, density and seed. Each row is shown while
// the active tool reads it (optionsFor). It edits the view's
// settings and the tools' and follows them, so the menus and the dock stay
// in step.

#include <QFlags>
#include <QWidget>

class QAction;
class QCheckBox;
class QIcon;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QSpinBox;

namespace bw {

struct Ramp;

// The ramp's colours dark to light, a swatch each: the ramp lists' icons.
QIcon rampIcon(const Ramp& ramp);

class CanvasView;
class CloneTool;
class SprayTool;
class GlyphTool;
class GradientTool;
class RimLightTool;
class SelectTool;
class Tool;
class ToolRegistry;

class ToolOptions : public QWidget {
    Q_OBJECT
public:
    // A setting, or a group of settings shown together.
    enum class Option {
        Size = 1 << 0,
        Tip = 1 << 1,
        Strength = 1 << 2,
        Pressure = 1 << 3,
        Ramp = 1 << 4,
        AlphaLock = 1 << 5,
        Light = 1 << 6,
        Form = 1 << 7,      // the form, its rim light and glint
        Hardness = 1 << 8,
        Symmetry = 1 << 9,  // the mode, radial's folds and centre
        Pencil = 1 << 10,   // 4-connected
        Glyph = 1 << 11,
        Filled = 1 << 12,
        PasteMode = 1 << 13,
        OutlineSelection = 1 << 14,  // the outline brush's shape is the selection
        SampleMerged = 1 << 15,
        Gradient = 1 << 16,  // the ramp gradient's shape, rungs and banding
        Texture = 1 << 17,   // the texture stamp's kind, density and seed
        CloneAligned = 1 << 18,  // the clone stamp keeps its distance across strokes
        RimLint = 1 << 19,       // the rim light keeps the ladder lint clean
        Spray = 1 << 20,         // the glyph spray's kind, density and seed
    };
    Q_DECLARE_FLAGS(Options, Option)

    // The settings the tool with this id reads; empty for a tool with none
    // (keyboard mode). False in `known` for an id it has no
    // entry for: such a tool shows every brush setting.
    static Options optionsFor(const QString& toolId, bool* known = nullptr);

    // `filledShapes` is the Tools menu's Filled Shapes toggle, which the
    // dock's checkbox mirrors.
    ToolOptions(CanvasView* view, ToolRegistry* tools, SelectTool* select, QAction* filledShapes,
                QWidget* parent = nullptr);

    // The options the dock shows now, for the active tool.
    Options shown() const { return shown_; }

    QSpinBox* sizeBox() const { return size_; }
    QComboBox* tipBox() const { return tip_; }
    QSpinBox* strengthBox() const { return strength_; }
    QComboBox* pressureBox() const { return pressure_; }
    QComboBox* rampBox() const { return ramp_; }
    QCheckBox* alphaLockBox() const { return lock_; }
    QComboBox* lightBox() const { return light_; }
    QComboBox* formBox() const { return form_; }
    QCheckBox* rimBox() const { return rim_; }
    QCheckBox* specularBox() const { return specular_; }
    QSpinBox* hardnessBox() const { return hardness_; }
    QCheckBox* outlineSelectionBox() const { return outlineSelection_; }
    QComboBox* textureKindBox() const { return textureKind_; }
    QDoubleSpinBox* textureDensityBox() const { return textureDensity_; }
    QSpinBox* textureSeedBox() const { return textureSeed_; }
    QComboBox* symmetryBox() const { return symmetry_; }
    QSpinBox* foldsBox() const { return folds_; }
    QCheckBox* centreBox() const { return centre_; }
    QSpinBox* centreXBox() const { return centreX_; }
    QSpinBox* centreYBox() const { return centreY_; }
    QCheckBox* pencil4Box() const { return pencil4_; }
    QLabel* glyphLabel() const { return glyph_; }
    QCheckBox* filledBox() const { return filled_; }
    QComboBox* pasteModeBox() const { return pasteMode_; }
    QCheckBox* sampleMergedBox() const { return merged_; }
    QComboBox* gradientShapeBox() const { return gradientShape_; }
    QSpinBox* gradientFromBox() const { return gradientFrom_; }
    QSpinBox* gradientToBox() const { return gradientTo_; }
    QComboBox* gradientBandingBox() const { return gradientBanding_; }
    QCheckBox* cloneAlignedBox() const { return cloneAligned_; }
    QComboBox* sprayKindBox() const { return sprayKind_; }
    QSpinBox* sprayDensityBox() const { return sprayDensity_; }
    QSpinBox* spraySeedBox() const { return spraySeed_; }
    QCheckBox* rimLintBox() const { return rimLint_; }

private:
    // Shows the view's and the tools' settings.
    void sync();
    // Limits the centre spins to the canvas.
    void fitCentre();
    // Sets the view's from the controls.
    void apply();
    // Sets the ramp gradient's shape, from rung or to rung from its control.
    void applyGradientShape();
    void applyGradientFrom(int from);
    void applyGradientTo(int to);
    // Shows the rows the active tool reads.
    void showFor(Tool* tool);

    CanvasView* view_;
    SelectTool* select_;
    GlyphTool* glyphTool_;
    GradientTool* gradientTool_;
    CloneTool* cloneTool_;
    SprayTool* sprayTool_;
    RimLightTool* rimTool_;
    QFormLayout* form_layout_;
    QLabel* title_;
    QLabel* none_;
    QSpinBox* size_;
    QComboBox* tip_;
    QSpinBox* strength_;
    QComboBox* pressure_;
    QComboBox* ramp_;
    QCheckBox* lock_;
    QCheckBox* merged_;
    QComboBox* light_;
    QComboBox* form_;
    QCheckBox* rim_;
    QCheckBox* specular_;
    QSpinBox* hardness_;
    QCheckBox* outlineSelection_;
    QComboBox* textureKind_;
    QDoubleSpinBox* textureDensity_;
    QSpinBox* textureSeed_;
    QComboBox* symmetry_;
    QSpinBox* folds_;
    QCheckBox* centre_;
    QSpinBox* centreX_;
    QSpinBox* centreY_;
    QCheckBox* pencil4_;
    QLabel* glyph_;
    QCheckBox* filled_;
    QComboBox* pasteMode_;
    QComboBox* gradientShape_;
    QSpinBox* gradientFrom_;
    QSpinBox* gradientTo_;
    QComboBox* gradientBanding_;
    QCheckBox* cloneAligned_;
    QComboBox* sprayKind_;
    QSpinBox* sprayDensity_;
    QSpinBox* spraySeed_;
    QCheckBox* rimLint_;
    Options shown_;
    bool syncing_ = false;
};

} // namespace bw

Q_DECLARE_OPERATORS_FOR_FLAGS(bw::ToolOptions::Options)
