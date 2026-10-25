#pragma once
// The canvas view's tools. The view hands its mouse and key events to the
// active tool first, in cell coordinates; whatever the tool doesn't take
// goes on to the view's keyboard mode (typing, the cell cursor). A tool edits
// through CanvasView::applyEdit, so each edit is one undo step and mirror
// mode applies to it, and draws what it has in progress (a floating block, a
// marquee) over the canvas. The selection belongs to the view, so it lasts
// across tool switches and every tool can read it. The base class takes
// nothing, so a plain Tool is keyboard mode.
//
// The registry holds the tools and which one is active; the main window
// builds the Tools menu from it and points the view at the active one.

#include "blockwright/edit.h"

#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>

#include <vector>

class QKeyEvent;
class QPainter;

namespace bw {

class CanvasView;

// A mouse event in cells. The cell may be off the canvas (a drag past the
// edge); tools clamp what they need. `halfRow` is the half-block pixel row
// under the pointer (halfblock.h): twice the cell row, plus one in a cell's
// bottom half.
struct ToolEvent {
    QPoint cell;
    int halfRow = 0;
    Qt::MouseButton button = Qt::NoButton;  // the button pressed or released
    Qt::MouseButtons buttons;               // the buttons held
    Qt::KeyboardModifiers modifiers;
    double pressure = 1.0;  // a stylus's pressure, 0-1; a mouse gives 1
};

class Tool : public QObject {
    Q_OBJECT
public:
    Tool(const QString& id, const QString& name, QObject* parent = nullptr);

    QString id() const { return id_; }
    QString name() const { return name_; }
    // The view while the tool is active, else null.
    CanvasView* view() const { return view_; }

    // The view calls these when the tool becomes active or stops being.
    virtual void activate(CanvasView* view);
    virtual void deactivate();

    // Each returns whether the tool took the event.
    virtual bool mousePress(const ToolEvent&) { return false; }
    virtual bool mouseMove(const ToolEvent&) { return false; }
    virtual bool mouseRelease(const ToolEvent&) { return false; }
    virtual bool keyPress(QKeyEvent*) { return false; }

    // Whether a drag the tool took is in progress. The undo key during one
    // cancels the drag and undoes nothing; undo, redo or a format change
    // made any other way cancels it too.
    virtual bool dragging() const { return false; }
    // The view's mirror mode or selection changed (or a setting of the
    // tool's own): a drag in progress redoes itself with them, so what it
    // shows stays what it lands.
    virtual void settingsChanged() {}

    // Drops whatever is in progress (a drag, a floating selection) without
    // an edit. The view calls it when the document changes under the tool
    // (undo, redo, an edit the view didn't make, a new canvas).
    virtual void cancel() {}
    // Ends a drag without an edit and undoes what the drag moved; what was
    // there before the drag (a floating selection) stays. The view calls it
    // when it loses focus during a drag the tool took.
    virtual void cancelDrag() {}
    // Writes whatever is in progress onto the canvas as an edit. The view
    // calls it before an edit of its own (typing) and before the document is
    // saved or its unsaved changes are checked.
    virtual void finish() {}

    // Draws, in view pixels, cells as the canvas will hold them once the
    // tool finishes (a floating selection): over the canvas, under the
    // reference image and the guides. `exposed` is the part being painted.
    virtual void paintCells(QPainter&, const QRect& /*exposed*/) const {}
    // Draws the tool's marks (a marquee) over everything but the cell
    // cursor.
    virtual void paint(QPainter&) const {}
    // Whether paint() draws the selection's outline, so the view doesn't.
    virtual bool drawsSelection() const { return false; }
    // The edit finish() would write, as the document gets it (mirrored, on
    // the active layer); Document::previewEdit makes it the composite cells
    // paintCells draws. Empty when nothing is in progress.
    virtual const CellEdit& pending() const;
    // A word on the tool's settings for the status bar (the glyph brush's
    // glyph); empty for none.
    virtual QString status() const { return {}; }

signals:
    // The tool's state or overlay changed: the view repaints and the menus
    // update. A drag that ends emits it, after dragging() turns false, so
    // the view stops filtering for the drag.
    void changed();

private:
    QString id_, name_;
    CanvasView* view_ = nullptr;
};

// What a tool's edit in progress will leave, drawn over the canvas: the
// changed cells as the renderer draws them. The edit is shown as given
// (mirror it first for mirror mode, CanvasView::mirrored), and only the
// cells being painted are rendered, so a preview costs what it shows.
class EditPreview {
public:
    void clear();
    bool empty() const { return changes_.empty(); }
    // Keeps what `edit`, built on the active layer, makes of the composite
    // (Document::previewEdit), to draw over the view's canvas.
    void set(const CanvasView& view, const CellEdit& edit);
    // Draws it in view pixels, the part within `exposed`.
    void paint(QPainter& p, const CanvasView& view, const QRect& exposed) const;

private:
    std::vector<CellChange> changes_;  // on the canvas, row-major
    QRect cells_;                      // their bounding box, in cells
};

class ToolRegistry : public QObject {
    Q_OBJECT
public:
    explicit ToolRegistry(QObject* parent = nullptr) : QObject(parent) {}

    // Adds a tool and takes ownership of it. The first tool added is active.
    void add(Tool* tool);
    const QList<Tool*>& tools() const { return tools_; }
    Tool* find(const QString& id) const;

    Tool* active() const { return active_; }
    void setActive(Tool* tool);
    void setActive(const QString& id) { setActive(find(id)); }

signals:
    void activeChanged(bw::Tool* tool);

private:
    QList<Tool*> tools_;
    Tool* active_ = nullptr;
};

} // namespace bw
