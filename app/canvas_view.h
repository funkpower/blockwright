#pragma once
// The editing view: the canvas at an integer zoom with a cell cursor.
// Arrow keys, Home/End and Enter move the cursor; printable keys and
// input-method commits write the glyph in the pen's colours; F1-F10 write
// the glyphs of the current glyph set; Backspace and Delete clear. Every
// edit is one command on the document's undo stack.

#include "blockwright/canvas_renderer.h"
#include "blockwright/edit.h"

#include <QWidget>

namespace bw {

class Document;

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

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    // Every edit the view makes goes through here: it is pushed on the
    // document's undo stack as one command, and the cursor moves on.
    void commit(const EditResult& r);
    // Writes the printable characters of `text` over the replacement range
    // (relative to the cursor) as one edit. If any of them has no CP437 glyph
    // nothing is written and they are reported instead.
    void typeText(const QString& text, int replaceStart = 0, int replaceLength = 0);
    void updateSize();

    Document* doc_;
    CanvasRenderer* renderer_;
    int zoom_ = 2;
    Cursor cursor_;
    Pen pen_;
    int glyphSet_;
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
