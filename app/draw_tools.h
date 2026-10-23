#pragma once
// The drawing tools (PLAN §4, phase 2.1): the half-block, shade, glyph and
// colorize brushes, line, rectangle and ellipse, fill and the eyedropper.
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
    const CellEdit& pending() const { return current_; }

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

    const Canvas& canvas() const;
    // What a stroke may paint (the selection, if any) and the document's
    // iCE setting, and whether to mirror: as they were at the press, or at
    // the last settingsChanged.
    const StrokeOptions& options() const { return options_; }
    bool mirror() const { return mirror_; }
    // The view's brush settings, as they were at the press or the last
    // settingsChanged.
    const BrushSettings& brushSettings() const { return brushSettings_; }
    // The pen colour for a button, as a palette index.
    std::uint8_t colourFor(Qt::MouseButton button) const;

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
// colours, right takes it a step down.
class ShadeTool : public BrushTool {
    Q_OBJECT
public:
    explicit ShadeTool(QObject* parent = nullptr);

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
};

// Ramp shade brush (PLAN §10): left lightens the cells under it along the
// view's ramp, right darkens them, a rung a pass at a quarter strength and
// up to a whole colour (four rungs) at full strength; pressure to strength
// scales that. Cells off the ramp are left alone. Its glyphs are symmetric,
// so mirror mode steps the mirror cells' own rungs.
class RampShadeTool : public BrushTool {
    Q_OBJECT
public:
    explicit RampShadeTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
};

// Glyph brush: left writes the brush's glyph in the pen's colours, right
// erases to blank. F1-F10 pick the glyph from the current glyph set while
// the tool is active; it starts as the full block.
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
// (the Brush dock).
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
// release wherever the pointer is.
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

// Eyedropper: left picks the colour under the pointer into the pen's
// foreground, right into its background. On a half-block pixel it is the
// half's colour as it shows under the document's iCE setting, elsewhere
// the cell's foreground or the background it shows.
class EyedropperTool : public Tool {
    Q_OBJECT
public:
    explicit EyedropperTool(QObject* parent = nullptr);

    bool mousePress(const ToolEvent& e) override;
    bool mouseMove(const ToolEvent& e) override;
    bool mouseRelease(const ToolEvent& e) override;
};

// Rim-light brush (PLAN §10, "Detail"): the edge cells of the view's ramp
// shape under it that face the Brush dock's light direction go one rung
// lighter (RimLightBrush). Either button.
class RimLightTool : public BrushTool {
    Q_OBJECT
public:
    explicit RimLightTool(QObject* parent = nullptr);

    QString status() const override;

protected:
    std::unique_ptr<Brush> makeBrush(Qt::MouseButton button) const override;
};

} // namespace bw
