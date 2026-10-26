#pragma once
// The drawing tools (PLAN §4, phase 2.1): the half-block, shade, glyph and
// colorize brushes, line, rectangle and ellipse, fill, the ramp gradient and
// the eyedropper.
//
// The left button draws with the pen's foreground, the right with its
// background (the shade brush builds up with the left button and takes
// back down with the right; the glyph brush erases with the right). A drag
// is shown as it goes and lands as one edit, one undo step, on release:
// the edit shown is the edit that lands. Esc, the undo key, undo or redo,
// a format change or a lost mouse drop it (the undo key undoes nothing
// then). Every tool paints only inside the selection when there is one,
// and mirror mode repeats what it paints under the view's symmetry
// (mirror X, mirror Y, both, or radial; PLAN §10); toggling mirror mode,
// changing the symmetry (or filled shapes) mid-drag redoes the drag with
// the new setting.
//
// The half-block brush, the shapes, fill and the eyedropper work in
// half-block pixels (halfblock.h): a cell's top and bottom halves, read and
// written by the colours they show under the document's iCE setting. In
// mirror mode the pixel tools repeat what they plot, so an image cell keeps
// its other half, and the brushes repeat their dabs (Stroke): each image
// paints the cells it reaches as they are, the shade brushes stepping an
// image cell's own level, the colorize brush keeping its glyph, the glyph
// brush writing its glyph mirrored. The selection clips each image on its
// own: the stroke and its images are each painted where they fall inside
// it.

#include "tool.h"

#include "blockwright/brush.h"
#include "blockwright/brushes/clone.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/guard.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/halfblock.h"

#include <memory>
#include <optional>
#include <vector>

namespace bw {

// A tool that edits by dragging: press starts, moves extend, release
// applies. Subclasses keep what the drag has done and say what edit it
// makes; the edit is worked out when the drag changes, not on every move.
class DragTool : public Tool {
    Q_OBJECT
public:
    using Tool::Tool;

    bool dragging() const override { return dragging_; }
    void settingsChanged() override;
    // The edit the drag lands if released now, mirrored and clipped: what
    // the preview shows. Empty when not dragging.
    const CellEdit& pending() const override { return current_; }

    void deactivate() override;
    void cancel() override { drop(); }
    void cancelDrag() override { drop(); }
    void finish() override;
    bool mousePress(const ToolEvent& e) override;
    bool mouseMove(const ToolEvent& e) override;
    bool mouseRelease(const ToolEvent& e) override;
    bool keyPress(QKeyEvent* e) override;
    void paintCells(QPainter& p, const QRect& exposed) const override;

protected:
    // Starts a drag; false declines the press.
    virtual bool begin(const ToolEvent& e) = 0;
    // Extends it; false when the edit it makes is unchanged.
    virtual bool extend(const ToolEvent& e) = 0;
    // The edit the drag makes so far, mirrored (mirror()) and clipped to
    // the selection.
    virtual CellEdit edit() const = 0;
    // Redoes the drag so far with the current options() and mirror().
    virtual void rebuild() {}
    // Forgets the drag.
    virtual void reset() = 0;

    // The active layer's cells (Document::layerCanvas), which the tools
    // edit.
    const Canvas& canvas() const;
    // What a stroke may paint (the selection, if any, and the active
    // layer's alpha lock, layerStrokeOptions) and the document's iCE
    // setting, and whether to mirror: as they were at the press, or at the
    // last settingsChanged.
    const StrokeOptions& options() const { return options_; }
    bool mirror() const { return mirror_; }
    // The view's brush settings, as they were at the press or the last
    // settingsChanged.
    const BrushSettings& brushSettings() const { return brushSettings_; }
    // The pen colour for a button, as a palette index.
    std::uint8_t colourFor(Qt::MouseButton button) const;
    // True for a tool whose cells are shades (░▒▓), which a Pixels layer
    // can't hold (PLAN §3): on one it declines a press, and its status()
    // says why (pixelRefusal). A text layer is painted as a cells layer.
    virtual bool paintsShades() const { return false; }
    // The same for glyphs: what the tool writes with `button` that a Pixels
    // layer can't hold, as pixelRefusal names it ("stars", the glyph brush's
    // glyph), or empty when it holds all of it. A Pixels layer holds only
    // █ ▀ ▄ and blanks.
    virtual QString unheldGlyphs(Qt::MouseButton) const { return {}; }
    // Whether the tool declines the active layer for a press with `button`,
    // as above. A setting changed mid-drag (settingsChanged) that makes it
    // decline drops the drag.
    bool refusesLayer(Qt::MouseButton button = Qt::LeftButton) const;
    // The status-bar text saying why, for the left button.
    QString pixelRefusal() const;

private:
    void drop();
    void refresh();
    void takeSettings();

    bool dragging_ = false;
    Qt::MouseButton button_ = Qt::NoButton;
    StrokeOptions options_;
    BrushSettings brushSettings_;
    bool mirror_ = false;
    CellEdit current_;
    EditPreview preview_;
};

// A brush on the cell grid, one Stroke per drag, painting with the view's
// brush settings: size, tip, strength and what a stylus's pressure scales.
// Each move adds a dab with the event's pressure; staying on a cell adds
// one only when pressing harder there can grow the brush. In mirror mode
// the stroke paints the dabs' images too (StrokeOptions::symmetry), tips
// mirrored, over the image cells as they are.
class BrushTool : public DragTool {
    Q_OBJECT
public:
    using DragTool::DragTool;

protected:
    // The brush for a drag with this button, in pen().
    virtual std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const = 0;
    // The pen the drag was pressed with: a pen change mid-stroke waits for
    // the next one.
    const Pen& pen() const { return pen_; }

    bool begin(const ToolEvent& e) override;
    bool extend(const ToolEvent& e) override;
    CellEdit edit() const override;
    void rebuild() override;
    void reset() override;

private:
    void addDab(const Dab& d);

    Qt::MouseButton button_ = Qt::NoButton;
    Pen pen_;
    std::unique_ptr<Brush> brush_;
    std::optional<Stroke> stroke_;
    std::vector<Dab> dabs_;
};

// Shade brush: left builds a cell up a step of space ░ ▒ ▓ █ in the pen's
// colours, right takes it a step down. It declines a press on a Pixels
// layer, which can't hold its shades, and says so in its status.
class ShadeTool : public BrushTool {
    Q_OBJECT
public:
    explicit ShadeTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool paintsShades() const override { return true; }
};

// Ramp shade brush (PLAN §10): left lightens the cells under it along the
// view's ramp, right darkens them, a rung a pass at a quarter strength and
// up to a whole colour (four rungs) at full strength; pressure to strength
// scales that. Cells off the ramp are left alone. Its glyphs are symmetric,
// so mirror mode steps the mirror cells' own rungs. It declines a press on
// a Pixels layer, which can't hold its shades, and says so in its status.
class RampShadeTool : public BrushTool {
    Q_OBJECT
public:
    explicit RampShadeTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool paintsShades() const override { return true; }
};

// The name the form brush's shape goes by in Tool Options and the status
// bar.
QString formShapeName(FormShape shape);

// Form brush (PLAN §10): shades the selection's shape, or with nothing
// selected the cells the stroke covers, as the view's form (a dome, a
// cylinder or a plane) lit from Tool Options' light direction, along the
// view's ramp. Strength (pressure, with pressure to strength) is how far a
// cell moves towards its tone. Each symmetry image shades its own form.
class FormTool : public BrushTool {
    Q_OBJECT
public:
    explicit FormTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool paintsShades() const override { return true; }
};

// Soft brush (PLAN §10): the ramp shade brush with a banded falloff, left
// lightens and right darkens, the most in the middle of the tip; the
// view's hardness narrows the falloff.
class SoftTool : public BrushTool {
    Q_OBJECT
public:
    explicit SoftTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool paintsShades() const override { return true; }
};

// Despeckle brush (PLAN §10): clears the specks under it, lone cells
// lighter or darker than all their neighbours on the view's ramp.
class DespeckleTool : public BrushTool {
    Q_OBJECT
public:
    explicit DespeckleTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
};

// Smudge brush (PLAN §10): drags the ramp's rungs under it along the
// stroke, either button, by the brush's strength.
class SmudgeTool : public BrushTool {
    Q_OBJECT
public:
    explicit SmudgeTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool paintsShades() const override { return true; }
};

// Glyph brush: left writes the brush's glyph in the pen's colours, right
// erases to blank. F1-F10 pick the glyph from the current glyph set while
// the tool is active; it starts as the full block. On a Pixels layer the
// left button declines a press when the glyph, or an image of it mirror
// mode writes (▀ turned on its side by radial symmetry), is one the layer
// can't hold, and the status says so; the right button still erases. A
// glyph picked mid-drag redoes the drag with it, or ends the drag when the
// layer refuses it.
class GlyphTool : public BrushTool {
    Q_OBJECT
public:
    explicit GlyphTool(QObject* parent = nullptr);

    char32_t glyph() const { return glyph_; }
    void setGlyph(char32_t glyph);

    bool keyPress(QKeyEvent* e) override;
    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    QString unheldGlyphs(Qt::MouseButton button) const override;

private:
    char32_t glyph_;
};

// Colorize brush: recolours cells and keeps their glyphs; left sets the
// foreground to the pen's, right the background.
class ColorizeTool : public BrushTool {
    Q_OBJECT
public:
    explicit ColorizeTool(QObject* parent = nullptr);

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
};

// Half-block brush: a pencil of half-block pixels, each drag joined up
// 4-connected, or 8-connected when the view's pencil connectivity says so
// (Tool Options).
class HalfBlockTool : public DragTool {
    Q_OBJECT
public:
    explicit HalfBlockTool(QObject* parent = nullptr);

protected:
    bool begin(const ToolEvent& e) override;
    bool extend(const ToolEvent& e) override;
    CellEdit edit() const override;
    void rebuild() override;
    void reset() override;

private:
    std::optional<HalfBlockStroke> stroke_;
    std::vector<QPoint> points_;  // (x, half-block row)
    std::uint8_t colour_ = 0;
};

// Line, rectangle and ellipse in half-block pixels, from the press to the
// pointer. Rectangles and ellipses are outlines, or filled with setFilled
// (mid-drag too).
class ShapeTool : public DragTool {
    Q_OBJECT
public:
    enum class Shape { Line, Rectangle, Ellipse };
    ShapeTool(Shape shape, QObject* parent = nullptr);

    Shape shape() const { return shape_; }
    bool filled() const { return filled_; }
    void setFilled(bool on);

protected:
    bool begin(const ToolEvent& e) override;
    bool extend(const ToolEvent& e) override;
    CellEdit edit() const override;
    void reset() override {}

private:
    Shape shape_;
    bool filled_ = false;
    QPoint from_, to_;  // (x, half-block row)
    std::uint8_t colour_ = 0;
};

// Fill: floods the 4-connected half-block pixels of the colour under the
// press (halfblock.h, floodPixels), once, at the press; it lands on
// release wherever the pointer is. It floods the active layer, or with
// sample merged (CanvasView::sampleMerged) the composite, and paints the
// region into the active layer either way. Sample merged, a pixel painted
// into a clear cell of a cells layer keeps the other half as the layers
// below the active one show it (Document::shownBelow), so only the region
// changes and nothing from the layers above is copied into the layer.
class FillTool : public DragTool {
    Q_OBJECT
public:
    explicit FillTool(QObject* parent = nullptr);

protected:
    bool begin(const ToolEvent& e) override;
    bool extend(const ToolEvent&) override { return false; }
    CellEdit edit() const override;
    void rebuild() override;
    void reset() override { pixels_.clear(); }

private:
    QPoint at_;  // (x, half-block row)
    std::uint8_t colour_ = 0;
    std::vector<std::pair<int, int>> pixels_;
};

// Ramp gradient (PLAN §10): a linear or radial fill along the view's ramp,
// cell-banded (rampGradient), from the press to the pointer; a radial one is
// centred on the press. The left button runs from the from rung to the to
// rung, the right the other way round. It fills every cell the selection and
// the alpha lock let it paint, fitted to the cells round it, and so has
// nothing for mirror mode to add: an image could land only on a cell the
// fill may not paint, so the symmetry isn't read. Its shape and rungs are
// kept between runs (appSettings, "gradient/..."); the ramp is the view's,
// shared with the ramp brushes. It declines a press on a Pixels layer, which
// can't hold its shades, and says so in its status.
class GradientTool : public DragTool {
    Q_OBJECT
public:
    explicit GradientTool(QObject* parent = nullptr);

    GradientShape shape() const { return shape_; }
    void setShape(GradientShape shape);
    // The rungs at the press and at the pointer, as set: rampGradient clamps
    // them to the ramp, so a rung past a short ramp's top comes back on a
    // longer one.
    int fromRung() const { return from_; }
    int toRung() const { return to_; }
    void setRungs(int from, int to);
    // What the drag's edit adds to the ladder lint's findings (rampGradient's
    // `added`): the documented L-shaped wide rung where a slanted band meets
    // the cells round the fill, or with iCE off a rung it can't reach. Empty
    // when not dragging.
    const std::vector<lint::LadderFinding>& added() const { return added_; }

    QString status() const override;

protected:
    bool begin(const ToolEvent& e) override;
    bool extend(const ToolEvent& e) override;
    CellEdit edit() const override;
    void rebuild() override;
    void reset() override;
    bool paintsShades() const override { return true; }

private:
    void save() const;
    // Sets added_ to what `e` adds to the canvas at the drag's start.
    void judge(const CellEdit& e) const;

    GradientShape shape_ = GradientShape::Linear;
    int from_ = 0, to_ = 0;
    QPoint start_, end_;  // cells
    bool reversed_ = false;  // the right button
    mutable std::vector<lint::LadderFinding> added_;
    // The drag's lint guard and a copy of the canvas it holds, taken at the
    // first preview and kept until the drag or the settings change.
    mutable std::unique_ptr<StrokeLint> lint_;
    mutable Canvas after_{0, 0};
};

// Eyedropper: left picks the colour under the pointer into the pen's
// foreground, right into its background. On a half-block pixel it is the
// half's colour as it shows under the document's iCE setting, elsewhere
// the cell's foreground or the background it shows. It picks from the
// active layer, where a transparent cell or pixel picks nothing, or with
// sample merged (CanvasView::sampleMerged) from the composite.
class EyedropperTool : public Tool {
    Q_OBJECT
public:
    explicit EyedropperTool(QObject* parent = nullptr);

    bool mousePress(const ToolEvent& e) override;
    bool mouseMove(const ToolEvent& e) override;
    bool mouseRelease(const ToolEvent& e) override;
};

// Outline brush (PLAN §10, "Detail"): the edge cells of the view's ramp
// shape under it become the ramp colour below them, one cell wide, the
// outline the ladder lint accepts (OutlineBrush). With *Outline the
// selection* on, the selection is the shape, so a seam takes the line.
// Either button.
class OutlineTool : public BrushTool {
    Q_OBJECT
public:
    explicit OutlineTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
};

// Rim-light brush (PLAN §10, "Detail"): the edge cells of the view's ramp
// shape under it that face Tool Options' light direction go one rung
// lighter (RimLightBrush). Keep the lint clean (off by default, kept in the
// settings) holds each stamp to the stroke's lint guard: a cell whose step
// would add a ladder-lint finding stays as it is. Either button.
class RimLightTool : public BrushTool {
    Q_OBJECT
public:
    explicit RimLightTool(QObject* parent = nullptr);

    bool keepLint() const { return keepLint_; }
    void setKeepLint(bool keep);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool paintsShades() const override { return true; }

private:
    bool keepLint_ = false;
};

// The name a texture kind goes by in Tool Options and the status bar.
QString textureKindName(TextureKind kind);

// Texture stamp (PLAN §10, "Detail"): stamps the view's texture (its kind,
// density and seed) along the view's ramp, lit from Tool Options' light
// direction (TextureBrush). The texture is fixed to the canvas by its seed,
// so strokes over one another paint one texture. Either button.
class TextureTool : public BrushTool {
    Q_OBJECT
public:
    explicit TextureTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool paintsShades() const override { return true; }
};

// Clone stamp (PLAN §10, "Detail"): Ctrl+click (Cmd+click on macOS) sets
// the source cell and paints nothing; a stroke then copies whole cells from
// the source to the cells under the tip, at the distance from the source to
// the stroke's first dab (CloneBrush). Aligned (the default, kept in the
// settings) keeps that distance for later strokes; not aligned, each stroke
// copies from the source again. Without a source a press paints nothing; a
// new document (open, New) has none.
// The view marks the source cell, and during a stroke the cell being copied
// from. Either button.
class CloneTool : public BrushTool {
    Q_OBJECT
public:
    explicit CloneTool(QObject* parent = nullptr);

    // Where the strokes copy from, and the distance the last stroke set.
    const CloneSource& source() const { return clone_; }
    // Sets the source cell (a cell off the canvas is ignored): the next
    // stroke copies from it.
    void setSource(QPoint cell);
    // Forgets the source and the distance: they are cells of a document
    // another one (open, New) replaces. The main window calls it on
    // Document::reset, whichever tool is active.
    void forgetSource();
    bool aligned() const { return clone_.aligned; }
    void setAligned(bool aligned);
    // The cell the stroke copies to its last dab (aligned or not), or with
    // no stroke the source; empty without a source.
    std::optional<QPoint> mark() const;

    bool mousePress(const ToolEvent& e) override;
    void finish() override;
    void paint(QPainter& p) const override;
    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    bool begin(const ToolEvent& e) override;
    bool extend(const ToolEvent& e) override;
    void reset() override;

private:
    CloneSource clone_;
    // The drag's: clone_ with the stroke's distance, kept if it lands.
    std::optional<CloneSource> next_;
    std::optional<CloneBrush> stamp_;
    QPoint last_;  // the drag's last dab
};

// The name a spray kind goes by in Tool Options and the status bar.
QString sprayKindName(SprayKind kind);

// Glyph spray (PLAN §10, "Detail"): stars, haze or smoke, at the tool's
// density and seed, kept in the settings (SprayBrush). Stars are drawn in
// the pen's foreground on open ground, and the right button takes them off;
// haze and smoke lift ░▒ along the view's ramp, lighter with the left
// button and darker with the right. Each kind is fixed to the canvas by its
// seed, so strokes over one another paint one field. Every kind declines a
// press on a Pixels layer, which holds neither stars nor shades, and says
// so in its status; changing the kind changes what it names.
class SprayTool : public BrushTool {
    Q_OBJECT
public:
    explicit SprayTool(QObject* parent = nullptr);

    const SprayOptions& options() const { return options_; }
    // Each takes its value within range, stores it in the settings, redoes a
    // drag under way with it (settingsChanged) and emits changed().
    void setKind(SprayKind kind);
    void setDensity(int density);
    void setSeed(int seed);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
    // haze and smoke; stars are glyphs
    bool paintsShades() const override { return options_.kind != SprayKind::Stars; }
    QString unheldGlyphs(Qt::MouseButton) const override;

private:
    void setOptions(const SprayOptions& options);

    SprayOptions options_;
};

} // namespace bw
