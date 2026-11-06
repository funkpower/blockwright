#include "draw_tools.h"

#include "canvas_view.h"
#include "document.h"
#include "plural.h"
#include "settings.h"

#include "blockwright/brushes/clone.h"
#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/brushes/smudge.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <climits>
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
    if (refusesLayer(button_)) {
        // what the drag would land now is what a press would be declined
        // for (the glyph brush's new glyph, or its glyph under a new
        // symmetry): it ends, and the status says why
        drop();
        return;
    }
    takeSettings();
    rebuild();
    refresh();
}

bool DragTool::mousePress(const ToolEvent& e) {
    if (dragging_)
        return true;  // a second button during a drag does nothing
    if (!view() || (e.button != Qt::LeftButton && e.button != Qt::RightButton))
        return false;
    if (refusesLayer(e.button))
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

const CellEdit& DragTool::layerCells() const {
    return dragging_ && view() ? preview_.layerCells() : Tool::layerCells();
}

const Canvas& DragTool::canvas() const { return view()->document()->layerCanvas(); }

std::uint8_t DragTool::colourFor(Qt::MouseButton button) const {
    const Pen& pen = view()->pen();
    return (button == Qt::RightButton ? pen.bg : pen.fg).paletteIndex();
}

bool DragTool::refusesLayer(Qt::MouseButton button) const {
    if (!view())
        return false;
    const Layer* layer = view()->document()->active();
    if (!layer || layer->props.kind != LayerKind::Pixels)
        return false;
    return paintsShades() || !unheldGlyphs(button).isEmpty();
}

QString DragTool::pixelRefusal() const {
    return tr("The %1 can't paint a Pixels layer: it holds no %2")
        .arg(QString(name()).remove(QLatin1Char('&')).toLower(),
             paintsShades() ? tr("shades") : unheldGlyphs(Qt::LeftButton));
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
    // a move to the last dab's cell is a dab only if it presses harder
    // with pressure on (bw::takesDab, which replay's strokes go by too)
    const Dab d{e.cell.x(), e.cell.y(), e.pressure};
    if (!dabs_.empty() && !takesDab(dabs_.back(), d, brushSettings().pressure))
        return false;
    addDab(d);
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

QString ShadeTool::status() const { return refusesLayer() ? pixelRefusal() : QString(); }

std::unique_ptr<Brush> ShadeTool::makeBrush(Qt::MouseButton button) const {
    return std::make_unique<ShadeBrush>(pen(), button == Qt::RightButton);
}

RampShadeTool::RampShadeTool(QObject* parent)
    : BrushTool(QStringLiteral("rampShade"), tr("&Ramp Shade Brush"), parent) {}

QString RampShadeTool::status() const {
    if (refusesLayer())
        return pixelRefusal();
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
    if (refusesLayer())
        return pixelRefusal();
    return tr("Ramp %1, %2, light from %3")
        .arg(QString::fromStdString(view()->ramp().name), formShapeName(view()->formOptions().shape).toLower(),
             compassName(view()->brushSettings().lightDirection));
}

std::unique_ptr<Brush> FormTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<FormShadeBrush>(view()->ramp(), view()->formOptions());
}

SoftTool::SoftTool(QObject* parent) : BrushTool(QStringLiteral("soft"), tr("Sof&t Brush"), parent) {}

QString SoftTool::status() const {
    if (refusesLayer())
        return pixelRefusal();
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
    if (refusesLayer())
        return pixelRefusal();
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
    // a drag under way is redone with it, or ends if the layer refuses it
    settingsChanged();
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
    if (refusesLayer())
        return pixelRefusal() + tr("; right button erases");
    const char32_t g = glyph_;
    return tr("Glyph %1").arg(QString::fromUcs4(&g, 1));
}

QString GlyphTool::unheldGlyphs(Qt::MouseButton button) const {
    if (button == Qt::RightButton || !view())
        return {};  // it erases, to a blank every layer holds
    const auto held = [](char32_t ch) {
        Cell c;
        c.ch = ch;
        return halfBlocks(c).has_value();
    };
    const char32_t g = glyph_;
    if (!held(g))
        return QString::fromUcs4(&g, 1);
    // the images mirror mode writes: flips keep █ ▀ ▄ and blanks, a quarter
    // turn puts a half block on its side
    const Canvas& c = canvas();
    for (const Dab& d : symmetryDabs(view()->symmetry(), Dab{c.width() / 2, c.height() / 2}, c.width(), c.height()))
        if (!held(dabGlyph(g, d)))
            return tr("%1 turned on its side, which radial symmetry writes").arg(QString::fromUcs4(&g, 1));
    return {};
}

std::unique_ptr<Brush> GlyphTool::makeBrush(Qt::MouseButton button) const {
    if (button == Qt::RightButton)
        return std::make_unique<GlyphBrush>(view()->document()->blankCell());
    Cell c;
    c.ch = glyph_;
    c.fg = pen().fg;
    c.bg = pen().bg;
    const Document* doc = view()->document();
    if (doc->target() == Target::Irc && !(doc->active() && doc->active()->props.kind == LayerKind::Pixels))
        c.attrs = pen().attrs;
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
const QString kGradientBandingKey = QStringLiteral("gradient/banding");
const QString kGradientFromKey = QStringLiteral("gradient/from");
const QString kGradientToKey = QStringLiteral("gradient/to");

} // namespace

GradientTool::GradientTool(QObject* parent)
    : DragTool(QStringLiteral("gradient"), tr("Ramp Gradie&nt"), parent), to_(longestTopRung()) {
    const auto s = appSettings();
    if (s->value(kGradientShapeKey).toString() == QLatin1String("radial"))
        shape_ = GradientShape::Radial;
    if (s->value(kGradientBandingKey).toString() == QLatin1String("bayer"))
        banding_ = GradientBanding::Bayer;
    from_ = std::max(0, s->value(kGradientFromKey, from_).toInt());
    to_ = std::max(0, s->value(kGradientToKey, to_).toInt());
}

void GradientTool::save() const {
    const auto s = appSettings();
    s->setValue(kGradientShapeKey,
                shape_ == GradientShape::Radial ? QStringLiteral("radial") : QStringLiteral("linear"));
    s->setValue(kGradientBandingKey,
                banding_ == GradientBanding::Bayer ? QStringLiteral("bayer") : QStringLiteral("cells"));
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

void GradientTool::setBanding(GradientBanding banding) {
    if (banding == banding_)
        return;
    banding_ = banding;
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
    if (refusesLayer())
        return pixelRefusal();
    const Ramp& ramp = view()->ramp();
    const int top = ramp.topRung();
    QString s = tr("Ramp %1, %2%3, rung %4 to %5")
                    .arg(QString::fromStdString(ramp.name),
                         shape_ == GradientShape::Radial ? tr("radial") : tr("linear"),
                         banding_ == GradientBanding::Bayer ? tr(", Bayer") : QString())
                    .arg(std::min(from_, top))
                    .arg(std::min(to_, top));
    if (!added_.empty())
        s += plural(int(added_.size()), tr(", adds %n lint finding"), tr(", adds %n lint findings"));
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
    g.banding = banding_;
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

MoveTool::MoveTool(QObject* parent) : Tool(QStringLiteral("move"), tr("Mo&ve/Transform"), parent) {}

void MoveTool::paint(QPainter& p) const {
    const std::optional<QRect> frame = view() ? view()->referenceFrame() : std::nullopt;
    if (!frame)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::white, 1, Qt::DashLine));
    p.drawRect(frame->adjusted(0, 0, -1, -1));
    p.setPen(QPen(Qt::black, 1, Qt::DotLine));
    p.drawRect(frame->adjusted(0, 0, -1, -1));
    constexpr int h = CanvasView::kHandle;
    const QPoint corners[] = {frame->topLeft(), {frame->x() + frame->width(), frame->y()},
                              {frame->x() + frame->width(), frame->y() + frame->height()},
                              {frame->x(), frame->y() + frame->height()}};
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    for (const QPoint& c : corners)
        p.drawRect(QRect(c - QPoint(h / 2, h / 2), QSize(h, h)).adjusted(0, 0, -1, -1));
    p.restore();
}

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
        // the foreground picks the cell's formatting with it, under IRC
        if (!bg && doc->target() == Target::Irc)
            pen.attrs = (view()->sampleMerged() ? doc->canvas() : doc->layerCanvas()).at(e.cell.x(), e.cell.y()).attrs &
                        attr::Formatting;
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

namespace {

const QString kRimKeepLintKey = QStringLiteral("rimLight/keepLint");

} // namespace

RimLightTool::RimLightTool(QObject* parent) : BrushTool(QStringLiteral("rimLight"), tr("R&im Light Brush"), parent) {
    keepLint_ = appSettings()->value(kRimKeepLintKey, false).toBool();
}

void RimLightTool::setKeepLint(bool keep) {
    if (keep == keepLint_)
        return;
    keepLint_ = keep;
    appSettings()->setValue(kRimKeepLintKey, keep);
    settingsChanged();
    emit changed();
}

QString RimLightTool::status() const {
    if (!view())
        return QString();
    if (refusesLayer())
        return pixelRefusal();
    const QString s = tr("Ramp %1, light from %2")
                          .arg(QString::fromStdString(view()->ramp().name),
                               compassName(view()->brushSettings().lightDirection));
    return keepLint_ ? tr("%1, keeping the lint clean").arg(s) : s;
}

std::unique_ptr<Brush> RimLightTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<RimLightBrush>(view()->ramp(), brushSettings().lightDirection, keepLint_);
}

QString textureKindName(TextureKind kind) {
    switch (kind) {
    case TextureKind::Foliage:
        return TextureTool::tr("Foliage");
    case TextureKind::Hoard:
        return TextureTool::tr("Hoard");
    case TextureKind::Brick:
        return TextureTool::tr("Brick");
    case TextureKind::Noise:
        return TextureTool::tr("Noise");
    case TextureKind::Scales:
        break;
    }
    return TextureTool::tr("Scales");
}

TextureTool::TextureTool(QObject* parent) : BrushTool(QStringLiteral("texture"), tr("Te&xture Stamp"), parent) {}

QString TextureTool::status() const {
    if (!view())
        return QString();
    if (refusesLayer())
        return pixelRefusal();
    const TextureOptions& t = view()->textureOptions();
    return tr("Ramp %1, %2, density %3, seed %4, light from %5")
        .arg(QString::fromStdString(view()->ramp().name), textureKindName(t.kind).toLower())
        .arg(t.density, 0, 'f', 1)
        .arg(t.seed)
        .arg(compassName(view()->brushSettings().lightDirection));
}

std::unique_ptr<Brush> TextureTool::makeBrush(Qt::MouseButton) const {
    return std::make_unique<TextureBrush>(view()->ramp(), view()->textureOptions(), brushSettings().lightDirection);
}

namespace {

const QString kCloneAlignedKey = QStringLiteral("clone/aligned");

} // namespace

CloneTool::CloneTool(QObject* parent) : BrushTool(QStringLiteral("clone"), tr("Clone Stamp"), parent) {
    clone_.aligned = appSettings()->value(kCloneAlignedKey, true).toBool();
}

void CloneTool::setSource(QPoint cell) {
    if (!view() || cell.x() < 0 || cell.y() < 0 || cell.x() >= canvas().width() || cell.y() >= canvas().height())
        return;
    clone_.setSource(cell.x(), cell.y());
    emit changed();
}

void CloneTool::forgetSource() {
    if (!clone_.source && !clone_.anchor)
        return;
    clone_.source.reset();
    clone_.anchor.reset();
    emit changed();
}

void CloneTool::setAligned(bool aligned) {
    if (aligned == clone_.aligned)
        return;
    clone_.aligned = aligned;
    appSettings()->setValue(kCloneAlignedKey, aligned);
    emit changed();
}

std::optional<QPoint> CloneTool::mark() const {
    if (dragging() && stamp_) {
        // The cell the stroke copies to the last dab, aligned or not.
        const auto s = stamp_->sampleAt(last_.x(), last_.y());
        if (s.first < INT_MIN || s.first > INT_MAX || s.second < INT_MIN || s.second > INT_MAX)
            return std::nullopt;
        return QPoint(int(s.first), int(s.second));
    }
    if (!clone_.source)
        return std::nullopt;
    return QPoint(clone_.source->first, clone_.source->second);
}

bool CloneTool::mousePress(const ToolEvent& e) {
    // Ctrl+click sets the source; it takes the press so it doesn't move the
    // cell cursor, and starts no drag.
    if (!dragging() && view() && (e.modifiers & Qt::ControlModifier) &&
        (e.button == Qt::LeftButton || e.button == Qt::RightButton)) {
        setSource(e.cell);
        return true;
    }
    return BrushTool::mousePress(e);
}

bool CloneTool::begin(const ToolEvent& e) {
    next_ = clone_;
    stamp_ = next_->beginStroke(e.cell.x(), e.cell.y());
    if (!stamp_) {
        next_.reset();
        return false;
    }
    last_ = e.cell;
    return BrushTool::begin(e);
}

bool CloneTool::extend(const ToolEvent& e) {
    last_ = e.cell;
    return BrushTool::extend(e);
}

void CloneTool::finish() {
    // A stroke that lands sets the distance aligned strokes keep; one
    // dropped with Esc leaves it.
    if (dragging() && next_)
        clone_.anchor = next_->anchor;
    BrushTool::finish();
}

void CloneTool::reset() {
    BrushTool::reset();
    stamp_.reset();
    next_.reset();
}

std::unique_ptr<Brush> CloneTool::makeBrush(Qt::MouseButton) const { return std::make_unique<CloneBrush>(*stamp_); }

void CloneTool::paint(QPainter& p) const {
    if (const auto m = mark(); view() && m)
        view()->paintOutline(p, {m->x(), m->y(), 1, 1}, true);
}

QString CloneTool::status() const {
    if (!clone_.source)
        return tr("Ctrl+click to set the source");
    return tr("Source %1, %2%3")
        .arg(clone_.source->first)
        .arg(clone_.source->second)
        .arg(clone_.aligned ? tr(", aligned") : QString());
}


QString sprayKindName(SprayKind kind) {
    switch (kind) {
    case SprayKind::Haze:
        return SprayTool::tr("Haze");
    case SprayKind::Smoke:
        return SprayTool::tr("Smoke");
    case SprayKind::Stars:
        break;
    }
    return SprayTool::tr("Stars");
}

namespace {

const QString kSprayKindKey = QStringLiteral("spray/kind");
const QString kSprayDensityKey = QStringLiteral("spray/density");
const QString kSpraySeedKey = QStringLiteral("spray/seed");

// The kinds as the settings store them
QString sprayKindKey(SprayKind kind) {
    switch (kind) {
    case SprayKind::Haze:
        return QStringLiteral("haze");
    case SprayKind::Smoke:
        return QStringLiteral("smoke");
    case SprayKind::Stars:
        break;
    }
    return QStringLiteral("stars");
}

} // namespace

SprayTool::SprayTool(QObject* parent) : BrushTool(QStringLiteral("spray"), tr("Gl&yph Spray"), parent) {
    const auto s = appSettings();
    const QString kind = s->value(kSprayKindKey).toString();
    for (const SprayKind k : {SprayKind::Stars, SprayKind::Haze, SprayKind::Smoke})
        if (kind == sprayKindKey(k))
            options_.kind = k;
    options_.density = std::clamp(s->value(kSprayDensityKey, options_.density).toInt(), kMinSprayDensity, kMaxSprayDensity);
    options_.seed = std::clamp(s->value(kSpraySeedKey, options_.seed).toInt(), 0, kMaxSpraySeed);
}

void SprayTool::setOptions(const SprayOptions& options) {
    SprayOptions o = options;
    o.density = std::clamp(o.density, kMinSprayDensity, kMaxSprayDensity);
    o.seed = std::clamp(o.seed, 0, kMaxSpraySeed);
    if (o == options_)
        return;
    options_ = o;
    const auto s = appSettings();
    s->setValue(kSprayKindKey, sprayKindKey(o.kind));
    s->setValue(kSprayDensityKey, o.density);
    s->setValue(kSpraySeedKey, o.seed);
    // a drag under way is redone with them, as for the other tools' settings
    settingsChanged();
    emit changed();
}

void SprayTool::setKind(SprayKind kind) {
    SprayOptions o = options_;
    o.kind = kind;
    setOptions(o);
}

void SprayTool::setDensity(int density) {
    SprayOptions o = options_;
    o.density = density;
    setOptions(o);
}

void SprayTool::setSeed(int seed) {
    SprayOptions o = options_;
    o.seed = seed;
    setOptions(o);
}

QString SprayTool::status() const {
    if (!view())
        return QString();
    if (refusesLayer())
        return pixelRefusal();
    if (options_.kind == SprayKind::Stars)
        return tr("Stars, density %1%, seed %2; right button takes stars off")
            .arg(options_.density)
            .arg(options_.seed);
    return tr("Ramp %1, %2, density %3%, seed %4; right button darkens")
        .arg(QString::fromStdString(view()->ramp().name), sprayKindName(options_.kind).toLower())
        .arg(options_.density)
        .arg(options_.seed);
}

QString SprayTool::unheldGlyphs(Qt::MouseButton) const {
    // the right button takes stars off, and a Pixels layer has none
    return options_.kind == SprayKind::Stars ? tr("stars") : QString();
}

std::unique_ptr<Brush> SprayTool::makeBrush(Qt::MouseButton button) const {
    return std::make_unique<SprayBrush>(view()->ramp(), options_, pen().fg,
                                        button == Qt::RightButton ? ShadeDirection::Darken : ShadeDirection::Lighten);
}

} // namespace bw
