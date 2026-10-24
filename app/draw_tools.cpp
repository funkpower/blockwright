#include "draw_tools.h"

#include "canvas_view.h"
#include "document.h"
#include "settings.h"

#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/brushes/smudge.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"

#include <QKeyEvent>

#include <algorithm>
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

const Canvas& DragTool::canvas() const { return view()->document()->layerCanvas(); }

std::uint8_t DragTool::colourFor(Qt::MouseButton button) const {
    const Pen& pen = view()->pen();
    return (button == Qt::RightButton ? pen.bg : pen.fg).paletteIndex();
}

void DragTool::takeSettings() {
    const Layer* layer = view()->document()->active();
    options_ = layer ? layerStrokeOptions(*layer) : StrokeOptions{};
    options_.selection = selectionMask(view()->selection(), canvas().width(), canvas().height());
    options_.ice = view()->document()->ansi().ice;
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

namespace {

QString compassName(Compass c) {
    static const char* const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return QString::fromLatin1(names[static_cast<int>(c)]);
}

} // namespace

QString formShapeName(FormShape shape) {
    switch (shape) {
    case FormShape::Cylinder:
        return FormTool::tr("Cylinder");
    case FormShape::Plane:
        return FormTool::tr("Plane");
    case FormShape::Dome:
        break;
    }
    return FormTool::tr("Dome");
}

FormTool::FormTool(QObject* parent) : BrushTool(QStringLiteral("form"), tr("F&orm Brush"), parent) {}

QString FormTool::status() const {
    if (!view())
        return {};
    return tr("Ramp %1, %2, light from %3")
        .arg(QString::fromStdString(view()->ramp().name), formShapeName(view()->formOptions().shape).toLower(),
             compassName(view()->brushSettings().lightDirection));
}

std::unique_ptr<Brush> FormTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<FormShadeBrush>(view()->ramp(), view()->formOptions());
}

SoftTool::SoftTool(QObject* parent) : BrushTool(QStringLiteral("soft"), tr("Sof&t Brush"), parent) {}

QString SoftTool::status() const {
    return view() ? tr("Ramp %1, hardness %2%")
                        .arg(QString::fromStdString(view()->ramp().name))
                        .arg(qRound(view()->hardness() * 100))
                  : QString();
}

std::unique_ptr<Brush> SoftTool::makeBrush(Qt::MouseButton button) const {
    return std::make_unique<SoftShadeBrush>(
        view()->ramp(), button == Qt::RightButton ? ShadeDirection::Darken : ShadeDirection::Lighten,
        view()->hardness());
}

DespeckleTool::DespeckleTool(QObject* parent)
    : BrushTool(QStringLiteral("despeckle"), tr("&Despeckle Brush"), parent) {}

QString DespeckleTool::status() const {
    return view() ? tr("Ramp %1").arg(QString::fromStdString(view()->ramp().name)) : QString();
}

std::unique_ptr<Brush> DespeckleTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<DespeckleBrush>(view()->ramp());
}

SmudgeTool::SmudgeTool(QObject* parent)
    : BrushTool(QStringLiteral("smudge"), tr("S&mudge Brush"), parent) {}

QString SmudgeTool::status() const {
    return view() ? tr("Ramp %1").arg(QString::fromStdString(view()->ramp().name)) : QString();
}

std::unique_ptr<Brush> SmudgeTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<SmudgeBrush>(view()->ramp());
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
        return std::make_unique<GlyphBrush>(view()->document()->blankCell());
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

void FillTool::rebuild() {
    if (!view()->sampleMerged()) {
        pixels_ = floodPixels(canvas(), at_.x(), at_.y(), options());
        return;
    }
    // The composite is flat cells, and the layer's lock is for the paint.
    StrokeOptions merged = options();
    merged.lockTransparent = false;
    merged.pixelLayer = false;
    pixels_ = floodPixels(view()->document()->canvas(), at_.x(), at_.y(), merged);
}

CellEdit FillTool::edit() const {
    if (!view()->sampleMerged() || options().pixelLayer || options().lockTransparent)
        return plotPixels(canvas(), pixels_, colour_, options(), mirror());
    // The region was found in the composite, so a pixel lands in a clear
    // cell of a cells layer over what that cell showed through to, the
    // layers below it: the half outside the region keeps that colour, not
    // the clear cell's black, and nothing is copied from the layers above.
    // (A pixel layer keeps that half clear itself; under the alpha lock
    // clear cells aren't painted.)
    Canvas base = canvas();
    const Document* doc = view()->document();
    for (int y = 0; y < base.height(); ++y)
        for (int x = 0; x < base.width(); ++x)
            if (base.at(x, y).transparent)
                base.at(x, y) = doc->shownBelow(x, y);
    return plotPixels(base, pixels_, colour_, options(), mirror());
}

namespace {

// The longest named ramp's top rung: a new gradient runs the whole of any.
int longestTopRung() {
    int top = 0;
    for (const Ramp& r : namedRamps())
        top = std::max(top, r.topRung());
    return top;
}

const QString kGradientShapeKey = QStringLiteral("gradient/shape");
const QString kGradientFromKey = QStringLiteral("gradient/from");
const QString kGradientToKey = QStringLiteral("gradient/to");

} // namespace

GradientTool::GradientTool(QObject* parent)
    : DragTool(QStringLiteral("gradient"), tr("Ramp Gradie&nt"), parent), to_(longestTopRung()) {
    const auto s = appSettings();
    if (s->value(kGradientShapeKey).toString() == QLatin1String("radial"))
        shape_ = GradientShape::Radial;
    from_ = std::max(0, s->value(kGradientFromKey, from_).toInt());
    to_ = std::max(0, s->value(kGradientToKey, to_).toInt());
}

void GradientTool::save() const {
    const auto s = appSettings();
    s->setValue(kGradientShapeKey,
                shape_ == GradientShape::Radial ? QStringLiteral("radial") : QStringLiteral("linear"));
    s->setValue(kGradientFromKey, from_);
    s->setValue(kGradientToKey, to_);
}

void GradientTool::setShape(GradientShape shape) {
    if (shape == shape_)
        return;
    shape_ = shape;
    save();
    settingsChanged();
    emit changed();
}

void GradientTool::setRungs(int from, int to) {
    from = std::max(0, from);
    to = std::max(0, to);
    if (from == from_ && to == to_)
        return;
    from_ = from;
    to_ = to;
    save();
    settingsChanged();
    emit changed();
}

QString GradientTool::status() const {
    if (!view())
        return {};
    const Ramp& ramp = view()->ramp();
    const int top = ramp.topRung();
    QString s = tr("Ramp %1, %2, rung %3 to %4")
                    .arg(QString::fromStdString(ramp.name),
                         shape_ == GradientShape::Radial ? tr("radial") : tr("linear"))
                    .arg(std::min(from_, top))
                    .arg(std::min(to_, top));
    if (!added_.empty())
        s += tr(", adds %n lint finding(s)", nullptr, int(added_.size()));
    return s;
}

bool GradientTool::begin(const ToolEvent& e) {
    start_ = end_ = e.cell;
    reversed_ = e.button == Qt::RightButton;
    return true;
}

bool GradientTool::extend(const ToolEvent& e) {
    if (e.cell == end_)
        return false;
    end_ = e.cell;
    return true;
}

CellEdit GradientTool::edit() const {
    Gradient g;
    g.shape = shape_;
    g.x0 = start_.x();
    g.y0 = start_.y();
    g.x1 = end_.x();
    g.y1 = end_.y();
    g.fromRung = reversed_ ? to_ : from_;
    g.toRung = reversed_ ? from_ : to_;
    CellEdit e = rampGradient(canvas(), view()->ramp(), g, options());
    judge(e);
    return e;
}

void GradientTool::judge(const CellEdit& e) const {
    added_.clear();
    if (e.empty())
        return;
    // The stroke's guard reads the canvas at the first preview; each move
    // after is judged against it a change at a time, as rampGradient's
    // `added` judges it from a fresh one.
    if (!lint_) {
        lint_ = std::make_unique<StrokeLint>(canvas(), view()->ramp());
        after_ = canvas();
    }
    std::vector<std::pair<int, int>> changed;
    changed.reserve(e.changes.size());
    for (const CellChange& c : e.changes)
        changed.push_back({c.x, c.y});
    e.apply(after_);
    added_ = lint_->judge(after_, changed);
    e.revert(after_);
}

void GradientTool::rebuild() {
    // the ramp or the layer under the drag may have changed
    lint_.reset();
}

void GradientTool::reset() {
    reversed_ = false;
    added_.clear();
    lint_.reset();
    after_ = Canvas(0, 0);
}

namespace {

// What the eyedropper picks from the active layer alone: nothing on a
// transparent cell or pixel, a pixel layer's pixel as its colour, and any
// other cell as it shows (pickColour).
std::optional<Color> pickLayerColour(const Document& doc, int x, int py, bool background) {
    const Canvas& cv = doc.layerCanvas();
    const Layer* layer = doc.active();
    if (!layer || py < 0 || !cv.contains(x, py / 2))
        return std::nullopt;
    const Cell& cell = cv.at(x, py / 2);
    if (layer->props.kind == LayerKind::Pixels) {
        const LayerPixels p = layerPixels(cell);
        const std::optional<std::uint8_t> half = py % 2 ? p.bottom : p.top;
        return half ? std::optional<Color>(Color::indexed(*half)) : std::nullopt;
    }
    if (cell.transparent)
        return std::nullopt;
    return pickColour(cv, x, py, background, doc.ansi().ice);
}

} // namespace

EyedropperTool::EyedropperTool(QObject* parent) : Tool(QStringLiteral("eyedropper"), tr("E&yedropper"), parent) {}

bool EyedropperTool::mousePress(const ToolEvent& e) {
    if (!view() || (e.button != Qt::LeftButton && e.button != Qt::RightButton))
        return false;
    const bool bg = e.button == Qt::RightButton;
    const Document* doc = view()->document();
    if (const auto c = view()->sampleMerged() ? pickColour(doc->canvas(), e.cell.x(), e.halfRow, bg, doc->ansi().ice)
                                              : pickLayerColour(*doc, e.cell.x(), e.halfRow, bg)) {
        Pen pen = view()->pen();
        (bg ? pen.bg : pen.fg) = *c;
        view()->setPen(pen);
    }
    return true;
}

bool EyedropperTool::mouseMove(const ToolEvent&) { return false; }

bool EyedropperTool::mouseRelease(const ToolEvent&) { return view() != nullptr; }

OutlineTool::OutlineTool(QObject* parent) : BrushTool(QStringLiteral("outline"), tr("&Outline Brush"), parent) {}

QString OutlineTool::status() const {
    return view() ? tr("Ramp %1").arg(QString::fromStdString(view()->ramp().name)) : QString();
}

std::unique_ptr<Brush> OutlineTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<OutlineBrush>(view()->ramp(), view()->outlineShape());
}

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
