#pragma once
// The editing view: the canvas at an integer zoom with a cell cursor.
// Arrow keys, Home/End and Enter move the cursor; printable keys and
// input-method commits write the glyph in the pen's colours; F1-F10 write
// the glyphs of the current glyph set; Backspace and Delete clear. Every
// edit is one command on the document's undo stack.
//
// A tool (tool.h) sees mouse and key events first; what it doesn't take
// falls through to the keyboard behaviour above. The view holds the
// selection, which every tool reads. Over the canvas it draws, in order, the
// tool's cells in progress, a reference image, the screen-size guides, the
// selection's outline and the tool's marks; none of them is part of the
// document.
//
// The view and the preview draw in view pixels (device_grid.h): logical
// pixels at a whole device pixel ratio, whole device pixels at a fractional
// one, so every canvas pixel is a whole block of device pixels.

#include "blockwright/brush.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/canvas_renderer.h"
#include "blockwright/edit.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <QImage>
#include <QWidget>
#include <QtGlobal>

// Qt 6.6 added QEvent::DevicePixelRatioChange. Before it a scale change is
// seen on the next paint (Qt repaints the window at the new ratio), which
// queues the same resize the event does.
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
#define BW_DPR_CHANGE_EVENT 1
#else
#define BW_DPR_CHANGE_EVENT 0
#endif

namespace bw {

class Document;
class Tool;

class CanvasView : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMinZoom = 1, kMaxZoom = kMaxScale;

    CanvasView(Document* doc, CanvasRenderer* renderer, QWidget* parent = nullptr);

    int zoom() const { return zoom_; }
    void setZoom(int zoom);  // clamped to kMinZoom-kMaxZoom

    Cursor cellCursor() const { return cursor_; }
    void setCellCursor(Cursor c);  // kept on the canvas

    const Pen& pen() const { return pen_; }
    void setPen(const Pen& pen);
    // The pen's colours one at a time, as palette indices.
    void setForeground(int index);
    void setBackground(int index);

    // What the brush tools paint with, like the pen: the brush settings
    // (size, tip, strength, what a stylus's pressure scales), the ramp the
    // ramp brushes walk, and the alpha lock, which leaves transparent cells
    // alone: the active layer's (PLAN §5), so setting it is an undo step on
    // the document and it follows the active layer. A change redoes a drag
    // in progress, so what it shows stays what it lands.
    const BrushSettings& brushSettings() const { return brush_; }
    void setBrushSettings(const BrushSettings& settings);  // size and strength clamped
    const Ramp& ramp() const { return ramp_; }
    void setRamp(const Ramp& ramp);  // ignored if rampProblem finds one
    bool lockTransparent() const;
    void setLockTransparent(bool on);
    // Sample merged (PLAN §5): the eyedropper picks, and fill floods by,
    // what the canvas shows (the composite) instead of the active layer.
    bool sampleMerged() const { return sampleMerged_; }
    void setSampleMerged(bool on);
    // The form brush's shape, rim light and glint (its light is the brush
    // settings' lightDirection), and the soft brush's hardness (0-1).
    const FormOptions& formOptions() const { return form_; }
    void setFormOptions(const FormOptions& options);
    double hardness() const { return hardness_; }
    void setHardness(double hardness);  // clamped
    // What the outline brush takes as the shape: the ramp's cells, or those
    // in the selection (a seam beside another shape takes the line).
    OutlineShape outlineShape() const { return outlineShape_; }
    void setOutlineShape(OutlineShape shape);
    // The texture stamp's kind, density and seed (brushes/texture.h), kept
    // in the settings between runs.
    const TextureOptions& textureOptions() const { return texture_; }
    void setTextureOptions(const TextureOptions& options);  // density and seed clamped

    // The F-key glyph set (glyph_sets.h), wrapping round at either end.
    int glyphSet() const { return glyphSet_; }
    void setGlyphSet(int set);

    // The cursor's cell in view pixels: the canvas at this zoom is
    // viewSize() of them, and tools paint in them.
    QRect cellRect(Cursor c) const;
    // Cell `cell` in view pixels, and the cell under a view pixel (rounded
    // down, so it may be off the canvas).
    QRect cellRect(const QPoint& cell) const { return cellRect(Cursor{cell.x(), cell.y()}); }
    QPoint cellAt(const QPoint& pixel) const;
    // The half-block pixel row under a view pixel: two to a cell row,
    // rounded down.
    int halfRowAt(const QPoint& pixel) const;
    QSize viewSize() const;
    // The widget (logical) pixels a view rect covers, and the view pixel
    // under a widget point.
    QRect toWidget(const QRect& view) const;
    QPoint toView(const QPointF& widget) const;

    Document* document() const { return doc_; }
    CanvasRenderer* renderer() const { return renderer_; }

    // The tool that sees events first; null for keyboard mode only.
    Tool* tool() const { return tool_; }
    void setTool(Tool* tool);

    // Pushes `edit` as one undo command, through mirror mode unless it is
    // `mirrored` already (a drawing tool mirrors what it draws); the cursor
    // stays. False if it changed nothing. Any other change to the document
    // (undo, redo, an edit made elsewhere, a new canvas) cancels the tool,
    // and undo, redo or a format change ends a drag the tool has going.
    bool applyEdit(const CellEdit& edit, bool mirrored = false);
    // `edit` as mirror mode would push it: mirrored when it is on.
    CellEdit mirrored(const CellEdit& edit) const;
    // The tool writes what it has in progress (a floating selection) as an
    // edit: before the view's own edits, and before the document is saved
    // or checked for unsaved changes.
    void finishTool();

    // The selected cells, clipped to the canvas; empty for none. It stays
    // across tool switches and is cleared by a new canvas.
    const Selection& selection() const { return selection_; }
    void setSelection(const Selection& selection);
    // Draws the outline of cells `r` as the selection's (dashed) or a
    // floating selection's (dotted) marquee.
    void paintOutline(QPainter& p, const CellRect& r, bool floating) const;

    // Mirror mode: every edit is repeated at the mirrored column with its
    // glyphs flipped (mirrorEdit).
    bool mirror() const { return mirror_; }
    void setMirror(bool on);
    // The symmetry the drawing tools repeat what they paint with (PLAN
    // §10): the chosen mode while mirror mode is on, none while it is off.
    // Mirror X is chosen at first. Setting a mode turns mirror mode on,
    // setting None turns it off and keeps the mode chosen before. Edits
    // made outside the drawing tools (typing, paste, delete) are mirrored
    // in mirror X only (mirrored()).
    Symmetry symmetry() const;
    const Symmetry& symmetryChoice() const { return symmetry_; }
    void setSymmetry(const Symmetry& symmetry);  // folds clamped to 2-8
    // Pulls a set radial centre onto the canvas (on Document::reset).
    void fitSymmetryCentre();
    // How the half-block pencil joins its points: 4-connected by default.
    Connectivity pencilConnectivity() const { return pencil_; }
    void setPencilConnectivity(Connectivity c);

    // Screen-size guides: a line after column `size.width()` and one after
    // every `size.height()` rows. An empty size shows none.
    QSize guide() const { return guide_; }
    void setGuide(const QSize& size);

    // A picture to trace over, drawn across the canvas width at its own
    // aspect, at `opacity` (0-1) over the canvas. Never saved.
    const QImage& referenceImage() const { return reference_; }
    void setReferenceImage(const QImage& image);
    bool referenceVisible() const { return referenceVisible_; }
    void setReferenceVisible(bool on);
    qreal referenceOpacity() const { return referenceOpacity_; }
    void setReferenceOpacity(qreal opacity);
    // The scaled copy of the reference image the view draws from: only the
    // part that covers the canvas, at most the canvas size at this zoom and
    // at most kMaxReferencePixels (drawn scaled up past that, with a row
    // more where the picture goes on below the canvas by at least that).
    QSize referenceCacheSize() const { return referenceScaled_.size(); }
    static constexpr qint64 kMaxReferencePixels = qint64(1) << 24;

    QSize sizeHint() const override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

    // Call after the document was replaced: resizes and repaints.
    void canvasReset();
    // Repaints these cells (cell coordinates).
    void cellsChanged(const QRegion& cells);

signals:
    void cursorMoved(bw::Cursor cursor);
    void zoomChanged(int zoom);
    void penChanged(const bw::Pen& pen);
    void glyphSetChanged(int set);
    // The characters of typed or committed text with no CP437 glyph the file
    // can hold; the text was not written.
    void glyphRejected(const QString& text);
    void mirrorChanged(bool on);
    // The symmetry or the pencil's connectivity changed (and mirror mode
    // with it, if it did).
    void symmetryChanged();
    // The reference image, or whether it shows, changed.
    void referenceChanged();
    // The reference image could not be scaled for this zoom; it is not drawn
    // (or tried again) until the zoom, the canvas or the image changes.
    void referenceFailed();
    void selectionChanged();
    // setTool switched to another tool.
    void toolChanged();
    // The brush settings, the ramp or the alpha lock changed.
    void brushChanged();

protected:
    bool event(QEvent* e) override;
    // Installed on the window while a stylus stroke goes on through a menu
    // opened from the keyboard.
    bool eventFilter(QObject* watched, QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    // A stylus drives the tool as the mouse does, with its pressure in the
    // ToolEvent; what the tool doesn't take is left for the mouse event Qt
    // makes from it.
    void tabletEvent(QTabletEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    // Every edit the view makes goes through here: it is pushed on the
    // document's undo stack as one command, and the cursor moves on.
    void commit(const EditResult& r);
    // Writes the printable characters of `text` over the replacement range
    // (relative to the cursor) as one edit. If any of them has no CP437 glyph
    // nothing is written and they are reported instead.
    void typeText(const QString& text, int replaceStart = 0, int replaceLength = 0);
    void updateSize();
    // The device pixel ratio changed: view pixels changed size.
    void dprChanged();
    void paintOverlays(QPainter& p, const QRect& clip);
    void paintReference(QPainter& p, const QRect& clip);
    // The document changed under the tool: it drops what it has in progress.
    void cancelTool();
    // Ends a drag the tool has going, if any, without an edit.
    void cancelDrag();
    void brushSettingsChanged();

    Document* doc_;
    CanvasRenderer* renderer_;
    int zoom_ = 2;
    Cursor cursor_;
    Pen pen_;
    int glyphSet_;
    Tool* tool_ = nullptr;  // owned by the tool registry
    bool ownEdit_ = false;  // the view is pushing an edit
    // The device whose press started the tool's drag: it owns the drag, and
    // the other device's events are ignored while the drag goes on. The
    // ownership ends with the drag, however it ends (a release, Esc, a lost
    // focus), whatever buttons are still held. A stylus's events
    // reach the view as tablet events, or as the mouse events Qt makes of
    // the ones the view doesn't accept.
    enum class Pointer { None, Mouse, Stylus, StylusMouse };
    Pointer drag_ = Pointer::None;
    static Pointer pointerOf(const QMouseEvent* e);
    // Sets the drag's owner; while the owner is one whose mouse events drive
    // the drag and the view is shown, the view filters the application's
    // events and drops another device's mouse events (filtering_).
    void setDrag(Pointer p);
    bool filtering_ = false;
    // Clears the owner of a drag that has ended, and stops its filter;
    // runs on every Tool::changed.
    void syncDrag();
    // Whether the tool's drag is going on and another device than `p` owns
    // it; clears the owner of a drag that has ended.
    bool othersDrag(Pointer p);
    // A press no tool takes, at view point `p`: the cell cursor goes to the
    // cell under it.
    void clickCell(QPoint p);
    // A stylus tablet event, by the view's own record of the pen's buttons
    // (tabletEvent keeps Qt's): returns whether the view took it.
    bool stylusEvent(QTabletEvent* e);
    BrushSettings brush_;
    Ramp ramp_;
    bool sampleMerged_ = false;
    FormOptions form_;
    double hardness_ = 0.0;
    OutlineShape outlineShape_ = OutlineShape::Ramp;
    TextureOptions texture_;
    double stylusPressure_ = 1.0;  // the last press or move's, for its release
    // The stylus's buttons as its last tablet event here held them, and the
    // point the view last saw it at (tabletEvent).
    Qt::MouseButtons stylusButtons_;
    // Qt's own record of the stylus's buttons, as the view last knew it, and
    // whether Qt's tablet grab is the view's.
    Qt::MouseButtons qtStylusButtons_;
    bool stylusGrab_ = false;
    QPoint stylusCell_;
    int stylusHalfRow_ = 0;
    Selection selection_;
    bool mirror_ = false;
    Symmetry symmetry_{SymmetryMode::MirrorX, 4, std::nullopt};
    Connectivity pencil_ = Connectivity::Four;
    QSize guide_;
    QImage reference_;
    QImage referenceScaled_;  // see referenceCacheSize, made on paint
    QSize referenceTarget_;  // the view pixels referenceScaled_ covers
    QSizeF referenceDrawn_;  // the view size all of referenceScaled_ is drawn at
    bool referenceVisible_ = true;
    qreal referenceOpacity_ = 0.5;
    qreal sizedDpr_ = 0;  // the device pixel ratio updateSize last ran at
};

// The canvas at 1x, for the always-visible preview dock: one view pixel to a
// canvas pixel.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(CanvasRenderer* renderer, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void canvasReset();
    void cellsChanged(const QRegion& cells);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void updateSize();
    void dprChanged();

    CanvasRenderer* renderer_;
    qreal sizedDpr_ = 0;  // the device pixel ratio updateSize last ran at
};

} // namespace bw
