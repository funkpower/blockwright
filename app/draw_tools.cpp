#include "draw_tools.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/brushes/drawing.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"

#include <QKeyEvent>

#include <set>

namespace bw {

void DragTool::deactivate() {
    // Switching tools mid-drag lands what the drag has drawn.
    finish();
    Tool::deactivate();
}

void DragTool::finish() {
    if (!dragging_ || !view())
        return;
    const CellEdit e = std::move(current_);
    drop();
    view()->applyEdit(e, /*mirrored=*/true);
}

void DragTool::settingsChanged() {
    if (!dragging_ || !view())
        return;
    takeSettings();
    rebuild();
    refresh();
}

bool DragTool::mousePress(const ToolEvent& e) {
    if (dragging_)
        return true;  // a second button during a drag does nothing
    if (!view() || (e.button != Qt::LeftButton && e.button != Qt::RightButton))
        return false;
    takeSettings();
    if (!begin(e))
        return false;
    dragging_ = true;
    button_ = e.button;
    refresh();
    return true;
}

bool DragTool::mouseMove(const ToolEvent& e) {
    if (!dragging_)
        return false;
    if (extend(e))
        refresh();
    return true;
}

bool DragTool::mouseRelease(const ToolEvent& e) {
    if (!dragging_)
        return false;
    if (e.button == button_) {
        if (extend(e))
            refresh();
        finish();
    }
    return true;
}

bool DragTool::keyPress(QKeyEvent* e) {
    if (dragging_ && e->key() == Qt::Key_Escape) {
        drop();
        return true;
    }
    return false;
}

void DragTool::paintCells(QPainter& p, const QRect& exposed) const {
    if (dragging_ && view())
        preview_.paint(p, *view(), exposed);
}

const Canvas& DragTool::canvas() const { return view()->document()->canvas(); }

std::uint8_t DragTool::colourFor(Qt::MouseButton button) const {
    const Pen& pen = view()->pen();
    return (button == Qt::RightButton ? pen.bg : pen.fg).paletteIndex();
}

void DragTool::takeSettings() {
    options_ = StrokeOptions{};
    options_.selection = selectionMask(view()->selection(), canvas().width(), canvas().height());
    options_.ice = view()->document()->ansi().ice;
    mirror_ = view()->mirror();
}

void DragTool::drop() {
    const bool was = dragging_;
    dragging_ = false;
    button_ = Qt::NoButton;
    current_ = {};
    preview_.clear();
    reset();
    if (was)
        emit changed();
}

void DragTool::refresh() {
    current_ = edit();
    preview_.set(*view(), current_);
    emit changed();
}

bool BrushTool::begin(const ToolEvent& e) {
    brush_ = makeBrush(e.button);
    dabs_.clear();
    rebuild();
    addDab(e.cell, true);
    return true;
}

bool BrushTool::extend(const ToolEvent& e) {
    if (!dabs_.empty() && dabs_.back() == e.cell)
        return false;
    addDab(e.cell, true);
    return true;
}

void BrushTool::addDab(QPoint cell, bool join) {
    dabs_.push_back(cell);
    stroke_->addDab(Dab{cell.x(), cell.y()}, join);
    if (unclipped_)
        unclipped_->addDab(Dab{cell.x(), cell.y()}, join);
    if (mirrorStroke_)
        mirrorStroke_->addDab(Dab{mirrorColumn(canvas().width(), cell.x()), cell.y()}, join);
}

void BrushTool::rebuild() {
    const std::vector<QPoint> dabs = std::move(dabs_);
    dabs_.clear();
    stroke_.emplace(canvas(), *brush_, BrushSettings{}, options());
    mirrorStroke_.reset();
    unclipped_.reset();
    if (mirror() && mirrorDabs()) {
        mirrorStroke_.emplace(canvas(), *brush_, BrushSettings{}, options());
    } else if (mirror()) {
        // the mirror is made from the whole stroke, then clipped on its own
        StrokeOptions all;
        all.ice = options().ice;
        unclipped_.emplace(canvas(), *brush_, BrushSettings{}, std::move(all));
    }
    for (const QPoint& d : dabs)
        addDab(d, true);
}

CellEdit BrushTool::edit() const {
    if (!stroke_)
        return {};
    CellEdit e = stroke_->edit();
    if (!mirror())
        return e;
    if (mirrorStroke_) {
        // The mirror cells as the mirrored dabs paint them; where both
        // strokes reach a cell (the middle column) this one's wins.
        for (const CellChange& c : mirrorStroke_->edit().changes)
            if (!stroke_->painted(c.x, c.y))
                e.changes.push_back(c);
        return e;
    }
    // The mirror image of the whole stroke, before the selection and the
    // lock clip it, kept where the mirror cell itself may be painted; where
    // the stroke reaches the mirror cell its own change wins (mirrorEdit).
    if (!unclipped_)
        return e;
    const CellEdit whole = unclipped_->edit();
    std::set<std::pair<int, int>> own;
    for (const CellChange& c : whole.changes)
        own.emplace(c.x, c.y);
    for (const CellChange& c : mirrorEdit(canvas(), whole).changes)
        if (!own.count({c.x, c.y}) && stroke_->paintable(c.x, c.y))
            e.changes.push_back(c);
    return e;
}

void BrushTool::reset() {
    stroke_.reset();
    mirrorStroke_.reset();
    unclipped_.reset();
    dabs_.clear();
    brush_.reset();
}

ShadeTool::ShadeTool(QObject* parent) : BrushTool(QStringLiteral("shade"), tr("Sh&ade Brush"), parent) {}

std::unique_ptr<Brush> ShadeTool::makeBrush(Qt::MouseButton button) const {
    return std::make_unique<ShadeBrush>(view()->pen(), button == Qt::RightButton);
}

GlyphTool::GlyphTool(QObject* parent)
    : BrushTool(QStringLiteral("glyph"), tr("&Glyph Brush"), parent), glyph_(kFullBlock) {}

void GlyphTool::setGlyph(char32_t glyph) {
    if (glyph == glyph_)
        return;
    glyph_ = glyph;
    emit changed();
}

bool GlyphTool::keyPress(QKeyEvent* e) {
    if (BrushTool::keyPress(e))
        return true;
    if (view() && e->key() >= Qt::Key_F1 && e->key() < Qt::Key_F1 + kGlyphSetKeys &&
        !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        setGlyph(glyphSetGlyph(view()->glyphSet(), e->key() - Qt::Key_F1));
        return true;
    }
    return false;
}

QString GlyphTool::status() const {
    const char32_t g = glyph_;
    return tr("Glyph %1").arg(QString::fromUcs4(&g, 1));
}

std::unique_ptr<Brush> GlyphTool::makeBrush(Qt::MouseButton button) const {
    if (button == Qt::RightButton)
        return std::make_unique<GlyphBrush>(Cell{});
    Cell c;
    c.ch = glyph_;
    c.fg = view()->pen().fg;
    c.bg = view()->pen().bg;
    return std::make_unique<GlyphBrush>(c);
}

ColorizeTool::ColorizeTool(QObject* parent) : BrushTool(QStringLiteral("colorize"), tr("&Colorize Brush"), parent) {}

std::unique_ptr<Brush> ColorizeTool::makeBrush(Qt::MouseButton button) const {
    const Pen& pen = view()->pen();
    if (button == Qt::RightButton)
        return std::make_unique<ColorizeBrush>(std::nullopt, pen.bg);
    return std::make_unique<ColorizeBrush>(pen.fg, std::nullopt);
}

HalfBlockTool::HalfBlockTool(QObject* parent)
    : DragTool(QStringLiteral("halfBlock"), tr("&Half-Block Brush"), parent) {}

bool HalfBlockTool::begin(const ToolEvent& e) {
    colour_ = colourFor(e.button);
    points_ = {QPoint(e.cell.x(), e.halfRow)};
    rebuild();
    return true;
}

bool HalfBlockTool::extend(const ToolEvent& e) {
    const QPoint p(e.cell.x(), e.halfRow);
    if (p == points_.back())
        return false;
    points_.push_back(p);
    stroke_->addPoint(p.x(), p.y());
    return true;
}

void HalfBlockTool::rebuild() {
    stroke_.emplace(canvas(), colour_, options(), mirror());
    for (const QPoint& p : points_)
        stroke_->addPoint(p.x(), p.y());
}

void HalfBlockTool::reset() {
    stroke_.reset();
    points_.clear();
}

CellEdit HalfBlockTool::edit() const { return stroke_ ? stroke_->edit() : CellEdit{}; }

namespace {

QString shapeId(ShapeTool::Shape s) {
    switch (s) {
    case ShapeTool::Shape::Line: return QStringLiteral("line");
    case ShapeTool::Shape::Rectangle: return QStringLiteral("rectangle");
    case ShapeTool::Shape::Ellipse: return QStringLiteral("ellipse");
    }
    return {};
}

QString shapeName(ShapeTool::Shape s) {
    switch (s) {
    case ShapeTool::Shape::Line: return ShapeTool::tr("&Line");
    case ShapeTool::Shape::Rectangle: return ShapeTool::tr("&Rectangle");
    case ShapeTool::Shape::Ellipse: return ShapeTool::tr("&Ellipse");
    }
    return {};
}

} // namespace

ShapeTool::ShapeTool(Shape shape, QObject* parent) : DragTool(shapeId(shape), shapeName(shape), parent), shape_(shape) {}

void ShapeTool::setFilled(bool on) {
    if (on == filled_)
        return;
    filled_ = on;
    settingsChanged();
    emit changed();
}

bool ShapeTool::begin(const ToolEvent& e) {
    from_ = to_ = QPoint(e.cell.x(), e.halfRow);
    colour_ = colourFor(e.button);
    return true;
}

bool ShapeTool::extend(const ToolEvent& e) {
    const QPoint to(e.cell.x(), e.halfRow);
    if (to == to_)
        return false;
    to_ = to;
    return true;
}

CellEdit ShapeTool::edit() const {
    const int w = canvas().width(), h = 2 * canvas().height();
    Pixels px;
    switch (shape_) {
    case Shape::Line: px = lineShape(from_.x(), from_.y(), to_.x(), to_.y(), w, h); break;
    case Shape::Rectangle: px = rectShape(from_.x(), from_.y(), to_.x(), to_.y(), filled_, w, h); break;
    case Shape::Ellipse: px = ellipseShape(from_.x(), from_.y(), to_.x(), to_.y(), filled_, w, h); break;
    }
    return plotPixels(canvas(), px, colour_, options(), mirror());
}

FillTool::FillTool(QObject* parent) : DragTool(QStringLiteral("fill"), tr("&Fill"), parent) {}

bool FillTool::begin(const ToolEvent& e) {
    at_ = QPoint(e.cell.x(), e.halfRow);
    colour_ = colourFor(e.button);
    rebuild();
    return true;
}

void FillTool::rebuild() { pixels_ = floodPixels(canvas(), at_.x(), at_.y(), options()); }

CellEdit FillTool::edit() const { return plotPixels(canvas(), pixels_, colour_, options(), mirror()); }

EyedropperTool::EyedropperTool(QObject* parent) : Tool(QStringLiteral("eyedropper"), tr("E&yedropper"), parent) {}

bool EyedropperTool::mousePress(const ToolEvent& e) {
    if (!view() || (e.button != Qt::LeftButton && e.button != Qt::RightButton))
        return false;
    const bool bg = e.button == Qt::RightButton;
    const Document* doc = view()->document();
    if (const auto c = pickColour(doc->canvas(), e.cell.x(), e.halfRow, bg, doc->ansi().ice)) {
        Pen pen = view()->pen();
        (bg ? pen.bg : pen.fg) = *c;
        view()->setPen(pen);
    }
    return true;
}

bool EyedropperTool::mouseMove(const ToolEvent&) { return false; }

bool EyedropperTool::mouseRelease(const ToolEvent&) { return view() != nullptr; }

} // namespace bw
