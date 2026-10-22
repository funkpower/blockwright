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

#include "blockwright/canvas_renderer.h"
#include "blockwright/edit.h"
#include "blockwright/selection.h"

#include <QImage>
#include <QWidget>

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

    // The F-key glyph set (glyph_sets.h), wrapping round at either end.
    int glyphSet() const { return glyphSet_; }
    void setGlyphSet(int set);

    // The cursor's cell in widget pixels.
    QRect cellRect(Cursor c) const;
    // Cell `cell` in widget pixels, and the cell under a widget pixel
    // (rounded down, so it may be off the canvas).
    QRect cellRect(const QPoint& cell) const { return cellRect(Cursor{cell.x(), cell.y()}); }
    QPoint cellAt(const QPoint& pixel) const;

    Document* document() const { return doc_; }
    CanvasRenderer* renderer() const { return renderer_; }

    // The tool that sees events first; null for keyboard mode only.
    Tool* tool() const { return tool_; }
    void setTool(Tool* tool);

    // Pushes `edit` as one undo command, through mirror mode; the cursor
    // stays. False if it changed nothing. Any other change to the document
    // (undo, redo, an edit made elsewhere, a new canvas) cancels the tool.
    bool applyEdit(const CellEdit& edit);
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
    // The reference image, or whether it shows, changed.
    void referenceChanged();
    // The reference image could not be scaled for this zoom; it is not drawn
    // (or tried again) until the zoom, the canvas or the image changes.
    void referenceFailed();
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
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
    void paintOverlays(QPainter& p, const QRect& clip);
    void paintReference(QPainter& p, const QRect& clip);
    // The document changed under the tool: it drops what it has in progress.
    void cancelTool();

    Document* doc_;
    CanvasRenderer* renderer_;
    int zoom_ = 2;
    Cursor cursor_;
    Pen pen_;
    int glyphSet_;
    Tool* tool_ = nullptr;  // owned by the tool registry
    bool ownEdit_ = false;  // the view is pushing an edit
    bool toolMouse_ = false;  // the tool took a press not yet released
    Selection selection_;
    bool mirror_ = false;
    QSize guide_;
    QImage reference_;
    QImage referenceScaled_;  // see referenceCacheSize, made on paint
    QSize referenceTarget_;  // the view pixels referenceScaled_ covers
    QSizeF referenceDrawn_;  // the view size all of referenceScaled_ is drawn at
    bool referenceVisible_ = true;
    qreal referenceOpacity_ = 0.5;
};

// The canvas at 1x, for the always-visible preview dock.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(CanvasRenderer* renderer, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void canvasReset();
    void cellsChanged(const QRegion& cells);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    CanvasRenderer* renderer_;
};

} // namespace bw
