#pragma once
// The selection tool (PLAN §4). Drag to select a rectangle; drag inside it,
// or press an arrow key, to lift the cells into a floating selection and
// move them. A paste floats too. A floating selection is only drawn until
// Enter or a click stamps it onto the canvas as one edit, in the paste mode;
// Esc puts it back. Delete clears the selection. Flips and rotations work on
// the floating cells, or on the selection in place as one edit.
//
// The selection itself is the view's (CanvasView::selection), so it outlives
// the tool. A floating selection doesn't: switching tools, typing or saving
// stamps it, and undo, redo or a new canvas drops it (cancel).
//
// The clipboard is the tool's own: cells with their colours, which the
// system clipboard has no format for.

#include "tool.h"

#include "blockwright/selection.h"

#include <QImage>
#include <QMetaObject>

namespace bw {

class SelectTool : public Tool {
    Q_OBJECT
public:
    explicit SelectTool(QObject* parent = nullptr);

    // The view's selection (empty before the tool was first active).
    const Selection& selection() const;
    // Selects `rect`, clipped to the canvas; stamps a floating selection
    // first. Needs the tool to be active.
    void select(const CellRect& rect);
    void selectAll();
    void deselect();

    bool floating() const { return floating_; }
    const CellBlock& floatingBlock() const { return float_; }
    QPoint floatingPos() const { return floatPos_; }

    const CellBlock& clipboard() const { return clipboard_; }
    PasteMode pasteMode() const { return mode_; }
    void setPasteMode(PasteMode mode);

    // The Edit and Selection menu actions. Each that changes the canvas is
    // one undo step; with nothing selected they do nothing.
    void copy();
    void cut();
    // Floats the clipboard at the cell cursor.
    void paste();
    // Delete: clears the selected cells, or drops a floating selection
    // (clearing the cells it was lifted from).
    void clear();
    void transform(Transform t);
    // Moves the floating selection, lifting the selected cells first.
    void moveBy(int dx, int dy);
    // Writes the floating selection onto the canvas.
    void stamp();

    void activate(CanvasView* view) override;
    void deactivate() override;
    // Esc: ends a drag and drops a floating selection, which puts lifted
    // cells back where they were (the lift never edited the canvas).
    void cancel() override;
    // Focus lost mid-drag: a dragged floating selection goes back to where
    // the drag picked it up and stays floating; a new rectangle just ends.
    void cancelDrag() override;
    void finish() override { stamp(); }
    bool mousePress(const ToolEvent& e) override;
    bool mouseMove(const ToolEvent& e) override;
    bool mouseRelease(const ToolEvent& e) override;
    bool keyPress(QKeyEvent* e) override;
    void paintCells(QPainter& p, const QRect& exposed) const override;
    void paint(QPainter& p) const override;
    bool drawsSelection() const override { return floating_; }

private:
    enum class Drag { None, Select, Pending, Float };

    const Canvas& canvas() const;
    // Lifts the selected cells into a floating selection.
    void lift();
    void setFloatPos(QPoint pos);
    void setSelection(const Selection& s);
    // What stamping would write, as a 1x picture of the cells it changes.
    void updatePreview() const;

    CanvasView* home_ = nullptr;  // the view the tool was last active in
    bool floating_ = false;
    CellBlock float_;
    QPoint floatPos_;
    bool lifted_ = false;
    Selection liftedFrom_;
    CellBlock clipboard_;
    PasteMode mode_ = PasteMode::Normal;

    Drag drag_ = Drag::None;
    QPoint dragStart_, dragOffset_;
    QPoint dragFrom_;  // where the floating selection was when the drag took it
    bool dragMoved_ = false;

    mutable bool previewStale_ = true;
    mutable QImage preview_;
    mutable QPoint previewCell_;

    QList<QMetaObject::Connection> connections_;
};

} // namespace bw
