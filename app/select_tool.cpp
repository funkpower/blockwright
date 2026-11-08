#include "select_tool.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/ansi.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/canvas_renderer.h"
#include "blockwright/render.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
    // The preview is the composite with the float on the active layer, so a
    // layer change (the alpha lock, which keeps the float) redraws it too.
    connections_ << connect(v->document(), &Document::cellsChanged, this, stale)
                 << connect(v->document(), &Document::formatChanged, this, stale)
                 << connect(v->document(), &Document::layersChanged, this, stale)
                 << connect(v, &CanvasView::mirrorChanged, this, stale)
                 << connect(v, &CanvasView::symmetryChanged, this, stale);
}

void SelectTool::deactivate() {
    // Switching tools keeps a floating selection's cells where they float,
    // and the selection with them. The registry doesn't switch from a float
    // that can't land (release); one the view lets go of all the same goes.
    if (!stamp())
        cancel();
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
    if (!view() || !stamp())
        return;
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
    resizable_.reset();
    floatName_.clear();
    refusal_.clear();
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
    refusal_.clear();  // moved, turned or re-cut: the next stamp looks again
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

namespace {

// The glyphs as text, for a message.
QString glyphText(const std::vector<Glyph>& glyphs) {
    QString out;
    for (const Glyph g : glyphs)
        if (!g.isContinuation())
            for (const char32_t cp : codePoints(g))
                out += QString::fromUcs4(&cp, 1);
    return out.isEmpty() ? SelectTool::tr("a wide character") : out;
}

} // namespace

QString SelectTool::landingProblem(const CellEdit& edit, bool* turnable) const {
    const Document* doc = view()->document();
    // A layer step's undo or redo (a lock, a hide, a bake) can leave a float
    // on a layer that takes no edit without a layersAboutToChange (which
    // drops one that can't land), and a lift doesn't check the layer: its
    // landing is refused for the layer's reason, as the Library's is
    // (Document::editProblem), rather than written to nothing.
    if (QString why = doc->editProblem(); !why.isEmpty())
        return why;
    const std::vector<Glyph> unheld = doc->unheldGlyphs(edit);
    if (unheld.empty())
        return {};
    if (turnable) {
        // whether a quarter turn of the float would land: each cell's glyph
        // turned, as the cells' places don't change what a layer holds
        CellEdit turned = edit;
        for (CellChange& c : turned.changes)
            c.after.ch = rotateGlyph(c.after.ch, true);
        *turnable = doc->unheldGlyphs(turned).empty();
    }
    return tr("a Pixels layer holds no %1").arg(glyphText(unheld));
}

CellEdit SelectTool::withSideways(const CellEdit& edit) const {
    CellEdit out = edit;
    const Canvas& c = canvas();
    for (int y = 0; y < float_.height; ++y)
        for (int x = 0; x < float_.width; ++x) {
            const Cell& cell = float_.at(x, y);
            const int cx = floatPos_.x() + x, cy = floatPos_.y() + y;
            if (!pixelsHoldTurned(cell.ch.codePointOr(0)) || !c.contains(cx, cy))
                continue;
            Cell shown = cell;
            shown.transparent = false;
            out.changes.push_back({cx, cy, c.at(cx, cy), shown});
        }
    return out;
}

bool SelectTool::stamp() {
    if (!view() || !floating_)
        return true;
    const Document* doc = view()->document();
    const CellEdit edit = stampBlock(canvas(), float_, floatPos_.x(), floatPos_.y(), effectivePasteMode(),
                                     lifted_ ? &liftedFrom_ : nullptr, doc->blankCell(), doc->pastePixels());
    // Checked as it would land, mirror mode's copy too: a float the layer
    // can't hold stays floating, as it was, and nothing is written.
    bool turnable = false;
    if (const QString why = landingProblem(view()->mirrored(withSideways(edit)), &turnable); !why.isEmpty()) {
        refusal_ = why;
        const QString what = floatName_.isEmpty() ? tr("Could not put the floating selection down: %1").arg(why)
                                                  : tr("Could not place %1: %2").arg(floatName_, why);
        view()->notify(turnable ? tr("%1; rotate it (Selection > Rotate) or press Esc to drop it").arg(what)
                                : tr("%1; press Esc to drop it").arg(what));
        previewStale_ = true;
        emit changed();
        return false;
    }
    const bool lifted = lifted_;
    floating_ = lifted_ = false;
    floatMode_.reset();
    resizable_.reset();
    floatName_.clear();
    refusal_.clear();
    drag_ = Drag::None;
    dropResizeRollback();
    previewStale_ = true;
    // a refused move (the alpha lock) leaves the selection on the cells
    // that stayed
    const bool applied = view()->applyEdit(edit);
    setSelection(!applied && lifted ? liftedFrom_ : Selection({floatPos_.x(), floatPos_.y(), float_.width, float_.height}));
    emit changed();
    return true;
}

void SelectTool::cancel() {
    const bool dragging = std::exchange(drag_, Drag::None) != Drag::None;
    if (floating_) {
        floating_ = false;
        floatMode_.reset();
        resizable_.reset();
        floatName_.clear();
        refusal_.clear();
        dropResizeRollback();
        setSelection(std::exchange(lifted_, false) ? liftedFrom_ : Selection());
        previewStale_ = true;
    } else if (!dragging) {
        return;
    }
    emit changed();
}

void SelectTool::cancelDrag() {
    const Drag d = std::exchange(drag_, Drag::None);
    if (d == Drag::Resize && floating_) {
        float_ = std::move(resizeOld_);
        resizable_ = std::move(resizeOldSlice_);
        dropResizeRollback();
        setFloatPos(resizeOldPos_);
    } else if (d == Drag::Float && floating_)
        setFloatPos(dragFrom_);
    else if (d != Drag::None)
        emit changed();
}

void SelectTool::dropResizeRollback() {
    // assigned, not cleared, so the cells' memory goes with them
    resizeOld_ = CellBlock();
    resizeOldSlice_.reset();
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
        resizable_.reset();
        floatName_.clear();
        refusal_.clear();
        drag_ = Drag::None;
        dropResizeRollback();
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

bool SelectTool::floatBlock(const CellBlock& block, QPoint cell, std::optional<PasteMode> mode,
                            const std::optional<StampSlice>& slice, const QString& name) {
    if (!view() || block.empty())
        return false;
    if (!stamp())
        return false;
    floatMode_ = mode;
    floatName_ = name;
    resizable_.reset();
    if (slice && sliceProblem(block, *slice).empty())
        resizable_ = Resizable{block, *slice, {}, block, *slice};
    float_ = block;
    floating_ = true;
    lifted_ = false;
    drag_ = Drag::None;
    setFloatPos(cell);
    return true;
}

void SelectTool::transform(Transform t) {
    if (!view())
        return;
    // A wide pair has no shape on its side (PLAN §16): a rotation of one,
    // or of half of one, is refused.
    const bool rotation = t == Transform::RotateClockwise || t == Transform::RotateCounterClockwise;
    if (rotation && (floating_ ? blockHoldsPair(float_) : selectionHoldsPair(canvas(), selection()))) {
        view()->notify(tr("Can't rotate a selection that holds a wide character"));
        return;
    }
    if (floating_) {
        // The cells turn as they are. A later re-cut cuts the stamp as saved
        // and turns it the same way (resizeFloat): turning first would
        // anchor a repeat of several cells at the other end.
        float_ = transformed(float_, t);
        if (resizable_) {
            addTurn(resizable_->turns, t);
            resizable_->source = transformed(resizable_->source, t);
            resizable_->slice = transformedSlice(resizable_->slice, t);
        }
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
    if (const int h = handleAt(e.pos); h >= 0) {
        drag_ = Drag::Resize;
        handle_ = h;
        resizeOld_ = float_;
        resizeOldPos_ = floatPos_;
        resizeOldSlice_ = resizable_;
        return true;
    }
    if (floating_ && QRect(floatPos_, QSize(float_.width, float_.height)).contains(c)) {
        drag_ = Drag::Float;
        dragOffset_ = c - floatPos_;
        dragFrom_ = floatPos_;
        return true;
    }
    if (!stamp())
        return true;  // a float that can't land stays, and the click with it
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
    case Drag::Resize: resizeToward(e.pos); break;
    case Drag::None: break;
    }
    return true;
}

void SelectTool::resizeToward(QPoint px) {
    // The dragged side goes to the cell edge nearest the pointer; the
    // opposite side stays, and the size keeps within the slice's bounds.
    if (!resizable())
        return;
    const QSize cs = view()->cellRect(QPoint(0, 0)).size();
    const auto edge = [](int p, int cell) { return int(std::floor((p + cell / 2.0) / cell)); };
    // the float as it stands, so a nudge or turn made during the drag
    // moves the side that stays with it
    const QRect f(floatPos_, QSize(float_.width, float_.height));
    const CellBlock& src = resizable_->source;
    const StampSlice& sl = resizable_->slice;
    const bool left = handle_ == 0 || handle_ == 6 || handle_ == 7;
    const bool right = handle_ >= 2 && handle_ <= 4;
    const bool top = handle_ <= 2;
    const bool bottom = handle_ >= 4 && handle_ <= 6;
    QRect r = f;
    const int wMin = sliceMinimumWidth(src, sl), wMax = sliceMaximumWidth(src, sl);
    const int hMin = sliceMinimumHeight(src, sl), hMax = sliceMaximumHeight(src, sl);
    // an axis whose corners alone are past a canvas's limit (a frame turned
    // on its side) can't resize, and std::clamp needs its bounds in order
    const bool wFits = wMin <= wMax, hFits = hMin <= hMax;
    if (left && wFits) {
        const int w = std::clamp(f.x() + f.width() - edge(px.x(), cs.width()), wMin, wMax);
        r.setLeft(f.x() + f.width() - w);
    } else if (right && wFits) {
        r.setWidth(std::clamp(edge(px.x(), cs.width()) - f.x(), wMin, wMax));
    }
    if (top && hFits) {
        const int h = std::clamp(f.y() + f.height() - edge(px.y(), cs.height()), hMin, hMax);
        r.setTop(f.y() + f.height() - h);
    } else if (bottom && hFits) {
        r.setHeight(std::clamp(edge(px.y(), cs.height()) - f.y(), hMin, hMax));
    }
    if (r != QRect(floatPos_, QSize(float_.width, float_.height)))
        resizeFloat(r);
}

bool SelectTool::mouseRelease(const ToolEvent& e) {
    if (e.button != Qt::LeftButton || drag_ == Drag::None || !view())
        return false;
    if (drag_ == Drag::Resize)
        resizeToward(e.pos);  // the release may be past the last move
    const Drag d = std::exchange(drag_, Drag::None);
    dropResizeRollback();
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
    pending_ = {};
    if (!view() || !floating_)
        return;
    const Document* doc = view()->document();
    const CellEdit edit = stampBlock(canvas(), float_, floatPos_.x(), floatPos_.y(), effectivePasteMode(),
                                     lifted_ ? &liftedFrom_ : nullptr, doc->blankCell(), doc->pastePixels());
    pending_ = view()->mirrored(edit);
    // A float the layer can't hold shows its cells as they are, over the
    // composite, until it is turned or dropped; it lands nothing.
    if (const CellEdit check = view()->mirrored(withSideways(edit)); !landingProblem(check).isEmpty())
        preview_.setUnheld(*view(), check);
    else
        preview_.set(*view(), pending_);
}

const CellEdit& SelectTool::pending() const {
    updatePreview();
    return pending_;
}

void SelectTool::paintCells(QPainter& p, const QRect& exposed) const {
    if (!view() || !floating_)
        return;
    updatePreview();
    preview_.paint(p, *view(), exposed);
}

const CellEdit& SelectTool::layerCells() const {
    if (!view() || !floating_)
        return Tool::layerCells();
    updatePreview();
    return preview_.layerCells();
}

void SelectTool::paint(QPainter& p) const {
    if (view() && floating_)
        view()->paintOutline(p, {floatPos_.x(), floatPos_.y(), float_.width, float_.height}, true);
    p.save();
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    for (const auto& h : handles())
        if (h)
            p.drawRect(h->x() - kHandle / 2, h->y() - kHandle / 2, kHandle - 1, kHandle - 1);
    p.restore();
}

bool SelectTool::resizeFloat(const QRect& rect) {
    if (!resizable())
        return false;
    // Cut the stamp as saved at the size it has before its turns, then turn
    // it as the float was turned; the limits are on the size as it shows.
    std::optional<CellBlock> b =
        resizeTurnedStamp(resizable_->saved, resizable_->savedSlice, resizable_->turns, rect.width(), rect.height());
    if (!b)
        return false;
    float_ = std::move(*b);
    setFloatPos(rect.topLeft());
    return true;
}

std::array<std::optional<QPoint>, 8> SelectTool::handles() const {
    std::array<std::optional<QPoint>, 8> out;
    if (!view() || !resizable())
        return out;
    const QRect px(view()->cellRect(floatPos_).topLeft(),
                   view()->cellRect(floatPos_ + QPoint(float_.width - 1, float_.height - 1)).bottomRight());
    const int l = px.left(), r = px.right() + 1, t = px.top(), b = px.bottom() + 1;
    const int mx = l + (r - l) / 2, my = t + (b - t) / 2;
    const StampSlice& s = resizable_->slice;
    const QPoint at[8] = {{l, t}, {mx, t}, {r, t}, {r, my}, {r, b}, {mx, b}, {l, b}, {l, my}};
    for (int i = 0; i < 8; ++i) {
        const bool corner = i % 2 == 0;
        const bool horizontal = i == 3 || i == 7;  // the left and right edges
        if (corner ? s.width && s.height : horizontal ? s.width : s.height)
            out[std::size_t(i)] = at[i];
    }
    return out;
}

int SelectTool::handleAt(QPoint pos) const {
    const auto hs = handles();
    for (int i = 0; i < 8; ++i)
        if (const auto& h = hs[std::size_t(i)];
            h && std::abs(pos.x() - h->x()) <= kHandle / 2 && std::abs(pos.y() - h->y()) <= kHandle / 2)
            return i;
    return -1;
}

} // namespace bw
