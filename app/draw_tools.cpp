#include "draw_tools.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/shading.h"
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
    options_.lockTransparent = view()->lockTransparent();
    options_.symmetry = view()->symmetry();
    brushSettings_ = view()->brushSettings();
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
    button_ = e.button;
    pen_ = view()->pen();
    dabs_.clear();
    rebuild();
    addDab(Dab{e.cell.x(), e.cell.y(), e.pressure});
    return true;
}

bool BrushTool::extend(const ToolEvent& e) {
    if (!dabs_.empty() && dabs_.back().x == e.cell.x() && dabs_.back().y == e.cell.y()) {
        // Pressing harder where the pen is can grow the brush; anything else
        // here paints nothing new (a stroke paints each cell once).
        if (brushSettings().pressure == PressureTo::Nothing || e.pressure <= dabs_.back().pressure)
            return false;
    }
    addDab(Dab{e.cell.x(), e.cell.y(), e.pressure});
    return true;
}

void BrushTool::addDab(const Dab& d) {
    dabs_.push_back(d);
    stroke_->addDab(d);
}

void BrushTool::rebuild() {
    const std::vector<Dab> dabs = std::move(dabs_);
    dabs_.clear();
    // The stroke holds the brush: it goes first. A new brush takes the ramp
    // as it is now, and the pen the drag was pressed with. The stroke
    // repeats its dabs under the options' symmetry.
    stroke_.reset();
    brush_ = makeBrush(button_);
    stroke_.emplace(canvas(), *brush_, brushSettings(), options());
    for (const Dab& d : dabs)
        addDab(d);
}

CellEdit BrushTool::edit() const { return stroke_ ? stroke_->edit() : CellEdit{}; }

void BrushTool::reset() {
    stroke_.reset();
    dabs_.clear();
    brush_.reset();
    button_ = Qt::NoButton;
}

ShadeTool::ShadeTool(QObject* parent) : BrushTool(QStringLiteral("shade"), tr("Sh&ade Brush"), parent) {}

std::unique_ptr<Brush> ShadeTool::makeBrush(Qt::MouseButton button) const {
    return std::make_unique<ShadeBrush>(pen(), button == Qt::RightButton);
}

RampShadeTool::RampShadeTool(QObject* parent)
    : BrushTool(QStringLiteral("rampShade"), tr("&Ramp Shade Brush"), parent) {}

QString RampShadeTool::status() const {
    return view() ? tr("Ramp %1").arg(QString::fromStdString(view()->ramp().name)) : QString();
}

std::unique_ptr<Brush> RampShadeTool::makeBrush(Qt::MouseButton button) const {
    // At full strength a pass moves a whole colour, four rungs; Stroke scales
    // that by the strength (and pressure), rounded and never below a rung.
    return std::make_unique<RampShadeBrush>(view()->ramp(),
                                            button == Qt::RightButton ? ShadeDirection::Darken
                                                                      : ShadeDirection::Lighten,
                                            kRungsPerStep);
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
    c.fg = pen().fg;
    c.bg = pen().bg;
    return std::make_unique<GlyphBrush>(c);
}

ColorizeTool::ColorizeTool(QObject* parent) : BrushTool(QStringLiteral("colorize"), tr("&Colorize Brush"), parent) {}

std::unique_ptr<Brush> ColorizeTool::makeBrush(Qt::MouseButton button) const {
    if (button == Qt::RightButton)
        return std::make_unique<ColorizeBrush>(std::nullopt, pen().bg);
    return std::make_unique<ColorizeBrush>(pen().fg, std::nullopt);
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
    stroke_.emplace(canvas(), colour_, options(), false, view()->pencilConnectivity());
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
    // the shape as far as its images under the symmetry can reach the
    // canvas; the painter clips each image on its own
    const PixelRect clip = symmetryReach(options().symmetry, canvas().width(), canvas().height());
    Pixels px;
    switch (shape_) {
    case Shape::Line: px = lineShape(from_.x(), from_.y(), to_.x(), to_.y(), clip); break;
    case Shape::Rectangle: px = rectShape(from_.x(), from_.y(), to_.x(), to_.y(), filled_, clip); break;
    case Shape::Ellipse: px = ellipseShape(from_.x(), from_.y(), to_.x(), to_.y(), filled_, clip); break;
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

namespace {

QString compassName(Compass c) {
    static const char* const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return QString::fromLatin1(names[static_cast<int>(c)]);
}

} // namespace

RimLightTool::RimLightTool(QObject* parent) : BrushTool(QStringLiteral("rimLight"), tr("R&im Light Brush"), parent) {}

QString RimLightTool::status() const {
    return view() ? tr("Ramp %1, light from %2")
                        .arg(QString::fromStdString(view()->ramp().name),
                             compassName(view()->brushSettings().lightDirection))
                  : QString();
}

std::unique_ptr<Brush> RimLightTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<RimLightBrush>(view()->ramp(), brushSettings().lightDirection);
}

} // namespace bw
