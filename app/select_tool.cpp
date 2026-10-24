#include "select_tool.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/ansi.h"
#include "blockwright/canvas_renderer.h"
#include "blockwright/render.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <exception>
#include <utility>

namespace bw {

SelectTool::SelectTool(QObject* parent) : Tool(QStringLiteral("select"), tr("&Select"), parent) {}

const Canvas& SelectTool::canvas() const { return view()->document()->layerCanvas(); }

CellBlock SelectTool::copied(const Selection& sel) const {
    CellBlock b = copyBlock(canvas(), sel);
    b.pixelLayer = view()->document()->pastePixels().pixelLayer;
    return b;
}

void SelectTool::activate(CanvasView* v) {
    Tool::activate(v);
    home_ = v;
    const auto stale = [this] {
        previewStale_ = true;
        if (floating_)
            emit changed();
    };
    connections_ << connect(v->document(), &Document::cellsChanged, this, stale)
                 << connect(v->document(), &Document::formatChanged, this, stale)
                 << connect(v, &CanvasView::mirrorChanged, this, stale)
                 << connect(v, &CanvasView::symmetryChanged, this, stale);
}

void SelectTool::deactivate() {
    // Switching tools keeps a floating selection's cells where they float,
    // and the selection with them.
    stamp();
    drag_ = Drag::None;
    for (const auto& c : connections_)
        disconnect(c);
    connections_.clear();
    Tool::deactivate();
}

const Selection& SelectTool::selection() const {
    static const Selection none;
    return home_ ? home_->selection() : none;
}

void SelectTool::setSelection(const Selection& s) {
    if (view())
        view()->setSelection(s);
}

void SelectTool::setPasteMode(PasteMode mode) {
    if (mode == mode_ && !floatMode_)
        return;
    mode_ = mode;
    floatMode_.reset();
    previewStale_ = true;
    emit changed();
}

void SelectTool::select(const CellRect& rect) {
    if (!view())
        return;
    stamp();
    setSelection(Selection(rect));
}

void SelectTool::selectAll() {
    if (view())
        select({0, 0, canvas().width(), canvas().height()});
}

void SelectTool::deselect() { select({}); }

void SelectTool::lift() {
    if (!view() || floating_ || selection().empty())
        return;
    liftedFrom_ = selection();
    float_ = copied(liftedFrom_);
    floatPos_ = {liftedFrom_.bounds().x, liftedFrom_.bounds().y};
    floating_ = lifted_ = true;
    previewStale_ = true;
    emit changed();
}

void SelectTool::setFloatPos(QPoint pos) {
    // Keep at least one cell on the canvas, so arrows can bring it back.
    const Canvas& cv = canvas();
    pos.setX(std::clamp(pos.x(), 1 - float_.width, cv.width() - 1));
    pos.setY(std::clamp(pos.y(), 1 - float_.height, cv.height() - 1));
    floatPos_ = pos;
    setSelection(Selection({pos.x(), pos.y(), float_.width, float_.height}));
    previewStale_ = true;
    emit changed();
}

void SelectTool::moveBy(int dx, int dy) {
    if (!view())
        return;
    lift();
    if (floating_)
        setFloatPos(floatPos_ + QPoint(dx, dy));
}

void SelectTool::stamp() {
    if (!view() || !floating_)
        return;
    const Document* doc = view()->document();
    const CellEdit edit = stampBlock(canvas(), float_, floatPos_.x(), floatPos_.y(), effectivePasteMode(),
                                     lifted_ ? &liftedFrom_ : nullptr, doc->blankCell(), doc->pastePixels());
    const bool lifted = lifted_;
    floating_ = lifted_ = false;
    floatMode_.reset();
    drag_ = Drag::None;
    previewStale_ = true;
    // a refused move (the alpha lock) leaves the selection on the cells
    // that stayed
    const bool applied = view()->applyEdit(edit);
    setSelection(!applied && lifted ? liftedFrom_ : Selection({floatPos_.x(), floatPos_.y(), float_.width, float_.height}));
    emit changed();
}

void SelectTool::cancel() {
    const bool dragging = std::exchange(drag_, Drag::None) != Drag::None;
    if (floating_) {
        floating_ = false;
        floatMode_.reset();
        setSelection(std::exchange(lifted_, false) ? liftedFrom_ : Selection());
        previewStale_ = true;
    } else if (!dragging) {
        return;
    }
    emit changed();
}

void SelectTool::cancelDrag() {
    const Drag d = std::exchange(drag_, Drag::None);
    if (d == Drag::Float && floating_)
        setFloatPos(dragFrom_);
    else if (d != Drag::None)
        emit changed();
}

void SelectTool::copy() {
    if (floating_)
        clipboard_ = float_;
    else if (view() && !selection().empty())
        clipboard_ = copied(selection());
    else
        return;
    emit changed();
}

void SelectTool::cut() {
    if (!floating_ && selection().empty())
        return;
    copy();
    clear();
}

void SelectTool::clear() {
    if (!view())
        return;
    if (floating_) {
        const CellEdit edit = lifted_ ? clearSelection(canvas(), liftedFrom_, view()->document()->blankCell()) : CellEdit{};
        floating_ = lifted_ = false;
        floatMode_.reset();
        drag_ = Drag::None;
        setSelection({});
        previewStale_ = true;
        view()->applyEdit(edit);
        emit changed();
    } else if (!selection().empty()) {
        view()->applyEdit(clearSelection(canvas(), selection(), view()->document()->blankCell()));
    }
}

void SelectTool::paste() {
    if (!view() || clipboard_.empty())
        return;
    const Cursor c = view()->cellCursor();
    floatBlock(clipboard_, {c.x, c.y});
}

void SelectTool::floatBlock(const CellBlock& block, QPoint cell, std::optional<PasteMode> mode) {
    if (!view() || block.empty())
        return;
    stamp();
    floatMode_ = mode;
    float_ = block;
    floating_ = true;
    lifted_ = false;
    drag_ = Drag::None;
    setFloatPos(cell);
}

void SelectTool::transform(Transform t) {
    if (!view())
        return;
    if (floating_) {
        float_ = transformed(float_, t);
        setFloatPos(floatPos_);
    } else if (!selection().empty()) {
        const SelectionEdit r = transformSelection(canvas(), selection(), t, view()->document()->blankCell());
        if (view()->applyEdit(r.edit))
            setSelection(r.selection);
    }
}

bool SelectTool::mousePress(const ToolEvent& e) {
    if (e.button != Qt::LeftButton || !view())
        return false;
    const QPoint c = e.cell;
    dragStart_ = c;
    dragMoved_ = false;
    if (floating_ && QRect(floatPos_, QSize(float_.width, float_.height)).contains(c)) {
        drag_ = Drag::Float;
        dragOffset_ = c - floatPos_;
        dragFrom_ = floatPos_;
        return true;
    }
    stamp();
    if (selection().contains(c.x(), c.y())) {
        drag_ = Drag::Pending;  // a move, once the mouse moves
        return true;
    }
    // A new selection; a click that never drags just deselects.
    drag_ = Drag::Select;
    setSelection({});
    view()->setCellCursor({c.x(), c.y()});
    return true;
}

bool SelectTool::mouseMove(const ToolEvent& e) {
    if (drag_ == Drag::None || !view())
        return false;
    const QPoint c = e.cell;
    if (c != dragStart_)
        dragMoved_ = true;
    switch (drag_) {
    case Drag::Select:
        if (dragMoved_) {
            setSelection(Selection(CellRect::fromCorners(dragStart_.x(), dragStart_.y(), c.x(), c.y())));
        }
        break;
    case Drag::Pending:
        if (!dragMoved_)
            break;
        lift();
        drag_ = Drag::Float;
        dragOffset_ = dragStart_ - floatPos_;
        dragFrom_ = floatPos_;
        [[fallthrough]];
    case Drag::Float:
        if (dragMoved_)
            setFloatPos(c - dragOffset_);
        break;
    case Drag::None: break;
    }
    return true;
}

bool SelectTool::mouseRelease(const ToolEvent& e) {
    if (e.button != Qt::LeftButton || drag_ == Drag::None || !view())
        return false;
    const Drag d = std::exchange(drag_, Drag::None);
    if (d == Drag::Float && !dragMoved_)
        stamp();  // a click on a floating selection
    else if (d == Drag::Pending)
        view()->setCellCursor({e.cell.x(), e.cell.y()});
    return true;
}

bool SelectTool::keyPress(QKeyEvent* e) {
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    const bool any = floating_ || !selection().empty();
    switch (e->key()) {
    case Qt::Key_Left: if (any) moveBy(-1, 0); return any;
    case Qt::Key_Right: if (any) moveBy(1, 0); return any;
    case Qt::Key_Up: if (any) moveBy(0, -1); return any;
    case Qt::Key_Down: if (any) moveBy(0, 1); return any;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!floating_)
            return false;
        stamp();
        return true;
    case Qt::Key_Escape:
        if (floating_ || drag_ != Drag::None)
            cancel();
        else if (any)
            deselect();
        else
            return false;
        return true;
    case Qt::Key_Delete:
        if (any)
            clear();
        return any;
    default: return false;
    }
}

void SelectTool::updatePreview() const {
    if (!previewStale_)
        return;
    previewStale_ = false;
    preview_.clear();
    if (!view() || !floating_)
        return;
    const Document* doc = view()->document();
    preview_.set(*view(), view()->mirrored(stampBlock(canvas(), float_, floatPos_.x(), floatPos_.y(),
                                                       effectivePasteMode(), lifted_ ? &liftedFrom_ : nullptr,
                                                       doc->blankCell(), doc->pastePixels())));
}

void SelectTool::paintCells(QPainter& p, const QRect& exposed) const {
    if (!view() || !floating_)
        return;
    updatePreview();
    preview_.paint(p, *view(), exposed);
}

void SelectTool::paint(QPainter& p) const {
    if (view() && floating_)
        view()->paintOutline(p, {floatPos_.x(), floatPos_.y(), float_.width, float_.height}, true);
}

} // namespace bw
