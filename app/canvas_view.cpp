#include "canvas_view.h"

#include "device_grid.h"
#include "document.h"
#include "reference_scale.h"
#include "tool.h"

#include "blockwright/glyph_sets.h"
#include "blockwright/selection.h"

#include <QApplication>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUndoStack>
#include <QWindow>

#include <algorithm>
#include <climits>
#include <cmath>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace bw {

namespace {

// `cells` (cell coordinates) as a widget region at `zoom`.
QRegion toPixels(const QRegion& cells, QSize cell, int zoom) {
    QRegion out;
    for (const QRect& r : cells)
        out |= QRect(r.x() * cell.width() * zoom, r.y() * cell.height() * zoom, r.width() * cell.width() * zoom,
                     r.height() * cell.height() * zoom);
    return out;
}

// A view region as the widget pixels that cover it.
QRegion toWidget(const DeviceGrid& grid, const QRegion& view) {
    QRegion out;
    for (const QRect& r : view)
        out |= grid.toWidget(r);
    return out;
}

// Makes `p` draw in view pixels and returns the view rect a widget rect
// `exposed` covers. Where that rect is off the view's `size` pixels (the part
// of a device pixel before the grid starts, and after it ends) it is filled
// with `background`.
QRect beginViewPaint(QPainter& p, const QRect& exposed, QSize size, const QColor& background) {
    const QTransform device = p.deviceTransform();
    const DeviceGrid grid(device.m11(), QPointF(device.dx(), device.dy()));
    p.setWorldTransform(grid.viewTransform(device));
    const QRect clip = grid.toView(exposed);
    for (const QRect& r : QRegion(clip) - QRegion(QRect(QPoint(0, 0), size)))
        p.fillRect(r, background);
    return clip;
}

// The 1x pixels a view rect at `zoom` shows.
QRect toSource(const QRect& r, int zoom) {
    const int x0 = r.left() / zoom, y0 = r.top() / zoom;
    return QRect(QPoint(x0, y0), QPoint(r.right() / zoom, r.bottom() / zoom));
}

// Whether a key is a shortcut rather than text. A key with no printable
// text is. Ctrl or Meta without Alt is too, even when the platform gives the
// key some text (macOS does for Cmd). Ctrl+Alt is AltGr on Windows and Alt
// is Option on macOS: both type the characters they give.
bool isShortcut(const QKeyEvent* e) {
    const QString t = e->text();
    if (std::none_of(t.begin(), t.end(), [](QChar c) { return c.unicode() >= 0x20 && c.unicode() != 0x7f; }))
        return true;
    const Qt::KeyboardModifiers m = e->modifiers();
    return (m & (Qt::ControlModifier | Qt::MetaModifier)) && !(m & Qt::AltModifier);
}

// x / d (d > 0) rounded down, for pixels left of or above the canvas; in
// 64 bits, so no int overflows.
long long floorDiv(long long x, long long d) { return x >= 0 ? x / d : -((-x + d - 1) / d); }

int clampInt(long long v) { return int(std::clamp(v, (long long)INT_MIN, (long long)INT_MAX)); }

// Saves the painter's state and puts it back, even when drawing throws.
class SavedPainter {
public:
    explicit SavedPainter(QPainter& p) : p_(p) { p_.save(); }
    ~SavedPainter() { p_.restore(); }
    SavedPainter(const SavedPainter&) = delete;
    SavedPainter& operator=(const SavedPainter&) = delete;

private:
    QPainter& p_;
};

} // namespace

CanvasView::CanvasView(Document* doc, CanvasRenderer* renderer, QWidget* parent)
    : QWidget(parent), doc_(doc), renderer_(renderer), glyphSet_(kDefaultGlyphSet) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // A quarter strength is one rung a pass on the ramp shade brush; a
    // mouse presses at 1, so pressure to size changes nothing without a
    // stylus.
    brush_.strength = 0.25;
    brush_.pressure = PressureTo::Size;
    ramp_ = *findRamp("steel");
    // Dead keys and compose sequences arrive as input-method commits.
    setAttribute(Qt::WA_InputMethodEnabled);
    // The pen's hover reaches the view, so a stylus drag whose release went
    // elsewhere lands when the pen comes back (tabletEvent).
    setTabletTracking(true);
    // Undo and redo put the cursor back where the edit was made or left it.
    connect(doc_, &Document::cursorRequested, this, &CanvasView::setCellCursor);
    // Undo, redo or any edit the view didn't push changes the cells under
    // the tool; a new canvas drops the selection too.
    connect(doc_, &Document::cellsChanged, this, [this] {
        if (!ownEdit_)
            cancelTool();
    });
    connect(doc_, &Document::reset, this, [this] {
        cancelTool();
        setSelection({});
        fitSymmetryCentre();
    });
    // Undo or redo of any command, a format change among them, and a format
    // change made any other way end a drag: what it shows was worked out
    // on the document before.
    connect(doc_->undoStack(), &QUndoStack::indexChanged, this, [this] {
        if (!ownEdit_)
            cancelDrag();
    });
    connect(doc_, &Document::formatChanged, this, &CanvasView::cancelDrag);
    // A layer panel change lands what the tool holds on the layer it was
    // editing first. The alpha lock is the active layer's, so a change of
    // layer properties or of active layer changes the brush settings; a new
    // active layer is other cells under the tool.
    connect(doc_, &Document::layersAboutToChange, this, &CanvasView::finishTool);
    connect(doc_, &Document::layersChanged, this, &CanvasView::brushSettingsChanged);
    connect(doc_, &Document::activeLayerChanged, this, [this] {
        cancelTool();
        brushSettingsChanged();
    });
    // A stylus stroke kept through a menu opened from the keyboard: the menu
    // bar has the focus, so no focus-out comes here to end the stroke. The
    // focus going anywhere but back here or to a menu ends it, as one here
    // would.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        const bool menu = now && (qobject_cast<QMenuBar*>(now) || now->window()->windowType() == Qt::Popup);
        if (drag_ == Pointer::Stylus && now != this && !menu)
            cancelDrag();
    });
    updateSize();
}

void CanvasView::setPen(const Pen& pen) {
    if (pen.fg == pen_.fg && pen.bg == pen_.bg)
        return;
    pen_ = pen;
    emit penChanged(pen_);
}

void CanvasView::setBrushSettings(const BrushSettings& settings) {
    BrushSettings s = settings;
    s.size = std::clamp(s.size, 1, kMaxBrushSize);
    s.strength = std::clamp(s.strength, 0.0, 1.0);
    if (s == brush_)
        return;
    brush_ = s;
    brushSettingsChanged();
}

void CanvasView::setRamp(const Ramp& ramp) {
    if (ramp == ramp_ || !rampProblem(ramp).empty())
        return;
    ramp_ = ramp;
    brushSettingsChanged();
}

bool CanvasView::lockTransparent() const {
    const Layer* l = doc_->active();
    return l && l->props.alphaLocked;
}

void CanvasView::setLockTransparent(bool on) {
    if (on == lockTransparent())
        return;
    // The view's own command: a drag in progress is redone with the new
    // lock (layersChanged), not dropped.
    const QScopedValueRollback<bool> own(ownEdit_, true);
    doc_->setLayerAlphaLocked(doc_->activeLayer(), on);
}

void CanvasView::setSampleMerged(bool on) {
    if (on == sampleMerged_)
        return;
    sampleMerged_ = on;
    brushSettingsChanged();
}

void CanvasView::setFormOptions(const FormOptions& options) {
    if (options == form_)
        return;
    form_ = options;
    brushSettingsChanged();
}

void CanvasView::setHardness(double hardness) {
    hardness = std::clamp(hardness, 0.0, 1.0);
    if (hardness == hardness_)
        return;
    hardness_ = hardness;
    brushSettingsChanged();
}

void CanvasView::setOutlineShape(OutlineShape shape) {
    if (shape == outlineShape_)
        return;
    outlineShape_ = shape;
    brushSettingsChanged();
}

void CanvasView::brushSettingsChanged() {
    emit brushChanged();
    if (tool_)
        tool_->settingsChanged();
}

void CanvasView::setForeground(int index) { setPen({Color::indexed(std::uint8_t(index & 15)), pen_.bg}); }

void CanvasView::setBackground(int index) { setPen({pen_.fg, Color::indexed(std::uint8_t(index & 15))}); }

void CanvasView::setGlyphSet(int set) {
    set = ((set % kGlyphSetCount) + kGlyphSetCount) % kGlyphSetCount;
    if (set == glyphSet_)
        return;
    glyphSet_ = set;
    emit glyphSetChanged(glyphSet_);
}

void CanvasView::setZoom(int zoom) {
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (zoom == zoom_)
        return;
    zoom_ = zoom;
    updateSize();
    update();
    emit zoomChanged(zoom_);
}

void CanvasView::setCellCursor(Cursor c) {
    const Canvas& cv = doc_->canvas();
    const Cursor on = moveCursor(cv, c, 0, 0);
    // Past the end only where the caller put it: on the last column.
    c = {on.x, on.y, c.pastEnd && on.x == c.x && on.y == c.y && on.x == cv.width() - 1};
    if (c == cursor_)
        return;
    update(toWidget(cellRect(cursor_)));
    cursor_ = c;
    update(toWidget(cellRect(cursor_)));
    if (hasFocus())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    emit cursorMoved(cursor_);
}

QRect CanvasView::cellRect(Cursor c) const {
    const QSize cs = renderer_->cellSize() * zoom_;
    return QRect(QPoint(c.x * cs.width(), c.y * cs.height()), cs);
}

QPoint CanvasView::cellAt(const QPoint& pixel) const {
    const QSize cs = renderer_->cellSize() * zoom_;
    if (cs.isEmpty())
        return {};
    return {clampInt(floorDiv(pixel.x(), cs.width())), clampInt(floorDiv(pixel.y(), cs.height()))};
}

QSize CanvasView::viewSize() const { return renderer_->imageSize() * zoom_; }

QRect CanvasView::toWidget(const QRect& view) const { return DeviceGrid::of(this).toWidget(view); }

QPoint CanvasView::toView(const QPointF& widget) const {
    const QPointF p = DeviceGrid::of(this).toView(widget);
    return {clampInt((long long)std::floor(p.x())), clampInt((long long)std::floor(p.y()))};
}

int CanvasView::halfRowAt(const QPoint& pixel) const {
    const int h = renderer_->cellSize().height() * zoom_;
    if (h <= 0)
        return 0;
    return clampInt(floorDiv(2LL * pixel.y(), h));
}

void CanvasView::setTool(Tool* tool) {
    if (tool == tool_)
        return;
    if (tool_) {
        disconnect(tool_, nullptr, this, nullptr);
        tool_->deactivate();
    }
    tool_ = tool;
    setDrag(Pointer::None);
    if (tool_) {
        connect(tool_, &Tool::changed, this, qOverload<>(&QWidget::update));
        // A drag can end in the tool, from a menu action or the document:
        // the grab ends with it.
        connect(tool_, &Tool::changed, this, &CanvasView::syncDrag);
        tool_->activate(this);
    }
    update();
    emit toolChanged();
}

bool CanvasView::applyEdit(const CellEdit& edit, bool mirrored) {
    const QScopedValueRollback<bool> own(ownEdit_, true);
    return doc_->edit(mirrored ? edit : this->mirrored(edit), cursor_, cursor_);
}

CellEdit CanvasView::mirrored(const CellEdit& edit) const {
    return mirror_ && symmetry_.mode == SymmetryMode::MirrorX ? mirrorEdit(doc_->layerCanvas(), edit) : edit;
}

void CanvasView::setSelection(const Selection& selection) {
    const Selection s = selection.clipped(doc_->canvas());
    if (s == selection_)
        return;
    selection_ = s;
    update();
    emit selectionChanged();
    if (tool_)
        tool_->settingsChanged();
}

void CanvasView::cancelTool() {
    setDrag(Pointer::None);
    if (tool_)
        tool_->cancel();
}

void CanvasView::cancelDrag() {
    if (!tool_ || !tool_->dragging()) {
        syncDrag();
        return;
    }
    setDrag(Pointer::None);
    tool_->cancelDrag();
}

void CanvasView::finishTool() {
    if (tool_)
        tool_->finish();
    syncDrag();
}

void CanvasView::setMirror(bool on) {
    if (on == mirror_)
        return;
    mirror_ = on;
    emit mirrorChanged(on);
    emit symmetryChanged();
    if (tool_)
        tool_->settingsChanged();
}

Symmetry CanvasView::symmetry() const { return mirror_ ? symmetry_ : Symmetry{SymmetryMode::None, 4, std::nullopt}; }

void CanvasView::setSymmetry(const Symmetry& symmetry) {
    if (symmetry.mode == SymmetryMode::None) {
        setMirror(false);
        return;
    }
    Symmetry s = symmetry;
    s.folds = std::clamp(s.folds, kMinFolds, kMaxFolds);
    if (s == symmetry_ && mirror_)
        return;
    const bool was = mirror_;
    symmetry_ = s;
    mirror_ = true;
    if (!was)
        emit mirrorChanged(true);
    emit symmetryChanged();
    if (tool_)
        tool_->settingsChanged();
}

void CanvasView::fitSymmetryCentre() {
    // A set centre stays on the canvas, symmetry on or off, so turning it
    // on after a smaller canvas opens turns about the edge, not off it.
    if (!symmetry_.centre)
        return;
    const Canvas& c = doc_->canvas();
    const std::pair fitted{std::clamp(symmetry_.centre->first, 0, std::max(0, c.width() - 1)),
                           std::clamp(symmetry_.centre->second, 0, std::max(0, c.height() - 1))};
    if (fitted == *symmetry_.centre)
        return;
    symmetry_.centre = fitted;
    emit symmetryChanged();
    if (tool_ && mirror_)
        tool_->settingsChanged();
}

void CanvasView::setPencilConnectivity(Connectivity c) {
    if (c == pencil_)
        return;
    pencil_ = c;
    emit symmetryChanged();
    if (tool_)
        tool_->settingsChanged();
}

void CanvasView::setGuide(const QSize& size) {
    if (size == guide_)
        return;
    guide_ = size;
    update();
}

void CanvasView::setReferenceImage(const QImage& image) {
    reference_ = image;
    referenceScaled_ = {};
    referenceTarget_ = {};
    update();
    emit referenceChanged();
}

void CanvasView::setReferenceVisible(bool on) {
    if (on == referenceVisible_)
        return;
    referenceVisible_ = on;
    update();
    emit referenceChanged();
}

void CanvasView::setReferenceOpacity(qreal opacity) {
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (opacity == referenceOpacity_)
        return;
    referenceOpacity_ = opacity;
    update();
}

QSize CanvasView::sizeHint() const { return DeviceGrid::of(this).widgetSize(viewSize()); }

QVariant CanvasView::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle)
        return toWidget(cellRect(cursor_));
    return QWidget::inputMethodQuery(query);
}

void CanvasView::canvasReset() {
    updateSize();
    update();
    const Cursor c = moveCursor(doc_->canvas(), cursor_, 0, 0);
    if (c != cursor_) {
        cursor_ = c;
        emit cursorMoved(cursor_);
    }
}

void CanvasView::cellsChanged(const QRegion& cells) {
    update(bw::toWidget(DeviceGrid::of(this), toPixels(cells, renderer_->cellSize(), zoom_)));
}

void CanvasView::updateSize() {
    sizedDpr_ = devicePixelRatioF();
    setFixedSize(sizeHint());
}

void CanvasView::dprChanged() {
    updateSize();
    update();
}

void CanvasView::paintEvent(QPaintEvent* e) {
#if !BW_DPR_CHANGE_EVENT
    if (devicePixelRatioF() != sizedDpr_)
        QTimer::singleShot(0, this, &CanvasView::dprChanged);
#endif
    QPainter p(this);
    const QRect clip = beginViewPaint(p, e->rect(), viewSize(), palette().color(backgroundRole()));
    // Nothing thrown here may leave paintEvent: what can't be drawn is left
    // out of this paint.
    try {
        const QRect shown = clip & QRect(QPoint(0, 0), viewSize());
        if (!shown.isEmpty()) {
            const QRect src = toSource(shown, zoom_);
            renderer_->paint(p, src, src.topLeft() * zoom_, zoom_);
        }
        paintOverlays(p, clip);
    } catch (const std::exception&) {
    }

    // The cursor: a frame drawn with XOR so it shows on any colour.
    const QRect r = cellRect(cursor_);
    const int t = std::max(1, zoom_);
    p.setCompositionMode(QPainter::RasterOp_SourceXorDestination);
    const QColor white(Qt::white);
    p.fillRect(QRect(r.left(), r.top(), r.width(), t), white);
    p.fillRect(QRect(r.left(), r.bottom() - t + 1, r.width(), t), white);
    p.fillRect(QRect(r.left(), r.top() + t, t, r.height() - 2 * t), white);
    p.fillRect(QRect(r.right() - t + 1, r.top() + t, t, r.height() - 2 * t), white);
}

void CanvasView::paintOverlays(QPainter& p, const QRect& clip) {
    if (tool_) {
        const SavedPainter saved(p);
        tool_->paintCells(p, clip);
    }
    paintReference(p, clip);
    if (!guide_.isEmpty()) {
        const Canvas& cv = doc_->canvas();
        const QSize size = viewSize();
        const QSize cs = renderer_->cellSize() * zoom_;
        const SavedPainter saved(p);
        p.setPen(QPen(QColor(255, 0, 255), std::max(1, zoom_ / 2), Qt::DashLine));
        if (guide_.width() < cv.width()) {
            const int x = guide_.width() * cs.width();
            p.drawLine(x, 0, x, size.height() - 1);
        }
        for (int row = guide_.height(); row < cv.height(); row += guide_.height()) {
            const int y = row * cs.height();
            p.drawLine(0, y, size.width() - 1, y);
        }
    }
    if (!(tool_ && tool_->drawsSelection()))
        paintOutline(p, selection_.bounds(), false);
    if (tool_) {
        const SavedPainter saved(p);
        tool_->paint(p);
    }
}

void CanvasView::paintReference(QPainter& p, const QRect& clip) {
    const QSize size = viewSize();
    if (!referenceVisible_ || reference_.isNull() || size.isEmpty())
        return;
    // Across the canvas width at the picture's own aspect, top-aligned: one
    // scale both ways, the canvas width over the picture's. Only the part
    // over the canvas is scaled, and each of its pixels is what the whole
    // picture scaled by that factor has there.
    const int w = size.width();
    const double scale = double(w) / reference_.width();
    const double fullH = reference_.height() * scale;
    const QSize target(w, int(std::clamp<double>(std::ceil(fullH), 1, size.height())));
    if (target != referenceTarget_) {
        referenceTarget_ = target;
        referenceScaled_ = {};
        QSize cache = target;  // the cache pixels over the target
        int margin = 0;  // cache rows below the target
        const qint64 pixels = qint64(target.width()) * target.height();
        if (pixels > kMaxReferencePixels) {
            // Drawn scaled up, so where the picture goes on below the
            // canvas, a row more: the interpolation in the last view row,
            // which reads the cache's last row and the one after it, then
            // reads the picture's next row, not the last one over again. The
            // picture ends at the right edge anyway.
            margin = fullH > target.height() ? 1 : 0;
            // The largest f with (width * f) * (height * f + margin) <= the cap.
            const double tw = target.width(), th = target.height(), cap = double(kMaxReferencePixels);
            const double f = (std::sqrt(tw * tw * margin * margin + 4 * tw * th * cap) - tw * margin) / (2 * tw * th);
            cache = QSize(std::max(1, int(tw * f)), std::max(1, int(th * f)));
            if (qint64(cache.width()) * (cache.height() + margin) > kMaxReferencePixels)
                cache.setHeight(std::max(1, int(kMaxReferencePixels / cache.width()) - margin));
        }
        // Cache pixels to a source pixel: `scale` both ways, but for the
        // rounding of a cache shrunk to the cap.
        const double kx = double(cache.width()) / reference_.width();
        const double ky = scale * cache.height() / target.height();
        // Only a row the picture covers whole: one it covers in part would be
        // that part opaque and fade the last view row. Short of a whole row
        // the interpolation reads the cache's last row again, as the whole
        // picture's does at its own bottom edge.
        margin = int(std::clamp(std::floor(reference_.height() * ky) - cache.height(), 0.0, double(margin)));
        referenceDrawn_ = QSizeF(target.width(), double(target.height()) * (cache.height() + margin) / cache.height());
        try {
            QImage scaled(cache.width(), cache.height() + margin, QImage::Format_ARGB32_Premultiplied);
            if (!scaled.isNull()) {
                scaled.fill(Qt::transparent);
                bool ok = true;
                if (kx >= 1 && ky >= 1) {
                    // Enlarged: interpolated. The source rows under the
                    // cache and two more, so the interpolation at its bottom
                    // edge reads the rows the whole picture has there.
                    const int srcH =
                        std::min(reference_.height(), int(std::ceil(scaled.height() / ky)) + 2);
                    QPainter sp(&scaled);
                    sp.setRenderHint(QPainter::SmoothPixmapTransform);
                    sp.drawImage(QRectF(0, 0, scaled.width(), srcH * ky), reference_,
                                 QRectF(0, 0, reference_.width(), srcH));
                } else {
                    // Reduced: averaged, as interpolating would alias.
                    ok = areaScale(reference_, scaled, kx, ky);
                }
                if (ok)
                    referenceScaled_ = std::move(scaled);
            }
        } catch (const std::bad_alloc&) {
            referenceScaled_ = {};
        }
        // A failure stays cached with its target, so it isn't retried on
        // every paint.
        if (referenceScaled_.isNull())
            emit referenceFailed();
    }
    if (referenceScaled_.isNull())
        return;
    const QRect r = clip & QRect(QPoint(0, 0), target);
    if (r.isEmpty())
        return;
    const SavedPainter saved(p);
    p.setOpacity(referenceOpacity_);
    if (referenceScaled_.size() == target) {
        p.drawImage(r.topLeft(), referenceScaled_, r);
        return;
    }
    // The whole cache over the whole target and its margin, clipped to what
    // is repainted, so a pixel doesn't depend on which part of the view is.
    p.setClipRect(r, Qt::IntersectClip);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRectF(QPointF(0, 0), referenceDrawn_), referenceScaled_);
}

void CanvasView::paintOutline(QPainter& p, const CellRect& r, bool floating) const {
    if (r.empty())
        return;
    const QRect px(cellRect(QPoint(r.x, r.y)).topLeft(),
                   cellRect(QPoint(r.x + r.width - 1, r.y + r.height - 1)).bottomRight());
    // Marching-ants style: black dashes over white, seen on any colour.
    const SavedPainter saved(p);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::white, 1));
    p.drawRect(px.adjusted(0, 0, -1, -1));
    p.setPen(QPen(Qt::black, 1, floating ? Qt::DotLine : Qt::DashLine));
    p.drawRect(px.adjusted(0, 0, -1, -1));
}

void CanvasView::commit(const EditResult& r) {
    {
        const QScopedValueRollback<bool> own(ownEdit_, true);
        doc_->edit(mirrored(r.edit), cursor_, r.cursor);
    }
    setCellCursor(r.cursor);
}

bool CanvasView::event(QEvent* e) {
#if BW_DPR_CHANGE_EVENT
    // A screen with another scale: view pixels change size.
    if (e->type() == QEvent::DevicePixelRatioChange)
        dprChanged();
#endif
    // The undo key during a drag is the view's (keyPressEvent), not the
    // Undo action's: it must not undo a step the drag can't see, and works
    // with nothing to undo.
    if (e->type() == QEvent::ShortcutOverride && tool_ && tool_->dragging() &&
        static_cast<QKeyEvent*>(e)->matches(QKeySequence::Undo)) {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

bool CanvasView::eventFilter(QObject* watched, QEvent* e) {
    // The window, while a stylus stroke is kept through a menu opened from
    // the keyboard: the menu bar has the focus and passes the undo key on,
    // and it is still the view's (event()).
    if ((e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) && drag_ == Pointer::Stylus &&
        tool_ && tool_->dragging() && static_cast<QKeyEvent*>(e)->matches(QKeySequence::Undo)) {
        if (e->type() == QEvent::KeyPress)
            cancelDrag();
        e->accept();
        return true;
    }
    return QWidget::eventFilter(watched, e);
}

void CanvasView::keyPressEvent(QKeyEvent* e) {
    if (tool_ && tool_->dragging() && e->matches(QKeySequence::Undo)) {
        cancelDrag();
        return;
    }
    if (tool_ && tool_->keyPress(e)) {
        syncDrag();
        return;
    }
    const Canvas& cv = doc_->layerCanvas();
    switch (e->key()) {
    case Qt::Key_Left: setCellCursor(moveCursor(cv, cursor_, -1, 0)); return;
    case Qt::Key_Right: setCellCursor(moveCursor(cv, cursor_, 1, 0)); return;
    case Qt::Key_Up: setCellCursor(moveCursor(cv, cursor_, 0, -1)); return;
    case Qt::Key_Down: setCellCursor(moveCursor(cv, cursor_, 0, 1)); return;
    case Qt::Key_Home: setCellCursor({0, cursor_.y}); return;
    case Qt::Key_End: setCellCursor({cv.width() - 1, cursor_.y}); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: setCellCursor(moveCursor(cv, {0, cursor_.y}, 0, 1)); return;
    case Qt::Key_Backspace: finishTool(); commit(backspace(cv, cursor_, doc_->blankCell())); return;
    case Qt::Key_Delete: finishTool(); commit(deleteCell(cv, cursor_, doc_->blankCell())); return;
    default: break;
    }
    if (e->key() >= Qt::Key_F1 && e->key() < Qt::Key_F1 + kGlyphSetKeys &&
        !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        finishTool();
        commit(typeGlyph(cv, cursor_, glyphSetGlyph(glyphSet_, e->key() - Qt::Key_F1), pen_));
        return;
    }
    if (isShortcut(e)) {
        QWidget::keyPressEvent(e);
        return;
    }
    typeText(e->text());
}

void CanvasView::inputMethodEvent(QInputMethodEvent* e) {
    typeText(e->commitString(), e->replacementStart(), e->replacementLength());
    e->accept();
}

void CanvasView::typeText(const QString& text, int replaceStart, int replaceLength) {
    std::u32string glyphs;
    QString rejected;
    // NFC first: "e" + U+0301 is the glyph é, which CP437 holds.
    for (const char32_t ch : text.normalized(QString::NormalizationForm_C).toUcs4()) {
        if (ch < 0x20 || ch == 0x7f)
            continue;
        if (isTypeable(ch))
            glyphs += ch;
        else
            rejected += QString::fromUcs4(&ch, 1);
    }
    if (!rejected.isEmpty()) {
        emit glyphRejected(rejected);
        return;
    }
    if (glyphs.empty() && replaceLength <= 0)
        return;
    // A floating selection lands first; the typing goes on top of it.
    finishTool();
    commit(replaceText(doc_->layerCanvas(), cursor_, replaceStart, replaceLength, glyphs, pen_, doc_->blankCell()));
}

namespace {

// Which device a mouse event came from: Qt makes mouse events of a stylus's
// unaccepted tablet events, from the stylus device.
bool fromStylus(const QMouseEvent* e) {
    const QPointingDevice* d = e->pointingDevice();
    if (!d)
        return false;
    switch (d->type()) {
    case QInputDevice::DeviceType::Stylus:
    case QInputDevice::DeviceType::Airbrush:
    case QInputDevice::DeviceType::Puck:
        return true;
    default:
        return false;
    }
}

// The buttons in `bs`, lowest first.
std::vector<Qt::MouseButton> buttonsIn(Qt::MouseButtons bs) {
    std::vector<Qt::MouseButton> out;
    for (unsigned b = 1; b && b <= unsigned(Qt::MaxMouseButton); b <<= 1)
        if (bs.toInt() & b)
            out.push_back(Qt::MouseButton(b));
    return out;
}

ToolEvent toolEvent(const CanvasView& v, const QMouseEvent* e) {
    const QPoint p = v.toView(e->position());
    return {v.cellAt(p), v.halfRowAt(p), e->button(), e->buttons(), e->modifiers()};
}

} // namespace

CanvasView::Pointer CanvasView::pointerOf(const QMouseEvent* e) {
    return fromStylus(e) ? Pointer::StylusMouse : Pointer::Mouse;
}

void CanvasView::setDrag(Pointer p) {
    drag_ = p;
    if (p != Pointer::Stylus)
        window()->removeEventFilter(this);
    // A drag the mouse's events drive holds the mouse grab. Qt sends a
    // press's moves and release to the widget it was pressed on only until
    // a release with no buttons held, and another device's release is one
    // (#45): without the grab the drag's own would then go nowhere.
    // A view that isn't shown takes none: Qt would grab for a widget no
    // input can reach.
    const bool grab =
        (p == Pointer::Mouse || p == Pointer::StylusMouse) && tool_ && tool_->dragging() && isVisible();
    if (grab == (QWidget::mouseGrabber() == this))
        return;
    if (grab)
        grabMouse();
    else
        releaseMouse();
    // A popup that opened during the drag holds the pointer grab of the
    // window system, and the view's grab or release can take it away: on
    // xcb a release ungrabs the device whatever window holds it. Qt grabs
    // for the popup before it tells the view the popup took the focus,
    // which ends the drag, so the popup's grab is set again here.
    if (QWidget* popup = QApplication::activePopupWidget())
        if (QWindow* w = popup->windowHandle())
            w->setMouseGrabEnabled(true);
}

void CanvasView::syncDrag() {
    if (!tool_ || !tool_->dragging())
        setDrag(Pointer::None);
}

bool CanvasView::othersDrag(Pointer p) {
    syncDrag();
    return drag_ != Pointer::None && drag_ != p;
}

void CanvasView::mouseMoveEvent(QMouseEvent* e) {
    if (othersDrag(pointerOf(e)))
        return;
    if (!tool_ || !tool_->mouseMove(toolEvent(*this, e)))
        QWidget::mouseMoveEvent(e);
}

void CanvasView::mouseReleaseEvent(QMouseEvent* e) {
    if (othersDrag(pointerOf(e)))
        return;
    if (e->buttons() == Qt::NoButton)
        setDrag(Pointer::None);
    if (!tool_ || !tool_->mouseRelease(toolEvent(*this, e)))
        QWidget::mouseReleaseEvent(e);
    syncDrag();
}

void CanvasView::tabletEvent(QTabletEvent* e) {
    // Qt's tablet grab: a press Qt sends here takes it and a release of the
    // last button lets it go, so with it every event of the pen comes here,
    // through an open popup first.
    const bool throughPopup = stylusGrab_ && QApplication::activePopupWidget();
    if (e->type() == QEvent::TabletPress)
        stylusGrab_ = true;
    else if (e->type() == QEvent::TabletRelease && e->buttons() == Qt::NoButton)
        stylusGrab_ = false;
    const bool taken = stylusEvent(e);
    // Qt records the buttons of every event but one a widget takes through
    // a popup. One no widget takes there it passes again, through the view's
    // own window and so here, and records: the view sees it twice, and the
    // second time, its buttons already the view's own, it is at most a
    // button the tool declined, let go and pressed again, declined again.
    if (!throughPopup || !taken)
        qtStylusButtons_ = e->buttons();
    e->setAccepted(taken);
}

bool CanvasView::stylusEvent(QTabletEvent* e) {
    // Qt tells a press and a release from a move, and which button changed,
    // by its own record of the pen's buttons, and an event the canvas takes
    // through an open popup leaves that record as it was (#46): a release
    // under a popup leaves the button held, so the next press can come as a
    // move, which sets no tablet grab, or as a press of the button let go
    // (the lowest one that changed); a press under a popup comes again with
    // the next event. So the view decides the type and the button of every
    // event by its own record, the buttons the pen's last tablet event here
    // held. A button this event holds that the record doesn't is pressed
    // here. One the record holds that this event doesn't was let go: here
    // if Qt says this event is its release; otherwise Qt saw it go up where
    // the view couldn't, or the event doesn't show, so it lands the stylus's
    // drag where the view last saw the pen (tablet tracking is on, so the
    // pen hovering back does that). A button both hold is held on, unless Qt
    // saw it go up out of view and comes back with it pressed again.
    const Qt::MouseButtons was = std::exchange(stylusButtons_, e->buttons());
    if (othersDrag(Pointer::Stylus)) {
        // The mouse's drag: taken, so Qt makes no mouse event of it. A drag
        // the stylus's mouse events started: left for Qt to go on making
        // those, which drive it.
        return drag_ == Pointer::Mouse;
    }
    const Qt::MouseButtons now = e->buttons();
    // The button Qt says changed. One both records hold that Qt says is
    // pressed went up out of view and is pressed again if the view knew
    // Qt's record held it; if Qt never recorded its press (one the view
    // took through a popup), this is that press again.
    const Qt::MouseButton flipped = e->type() == QEvent::TabletMove ? Qt::NoButton : e->button();
    const Qt::MouseButtons missed = was & now & qtStylusButtons_ & flipped;
    const Qt::MouseButtons lifted = was & ~now;
    const Qt::MouseButtons pressed = (now & ~was) | missed;
    const Qt::MouseButtons here = lifted & flipped;
    const QPoint p = toView(e->position());
    ToolEvent te{cellAt(p), halfRowAt(p), Qt::NoButton, was, e->modifiers(), stylusPressure_};
    // What the view makes of the event, if it's one Qt could have said.
    QEvent::Type type = QEvent::TabletMove;
    Qt::MouseButton button = Qt::NoButton;
    bool taken = false;
    for (const Qt::MouseButton b : buttonsIn(lifted | missed)) {
        const bool seen = here.testFlag(b);
        if (!seen && drag_ != Pointer::Stylus)
            continue;
        te.button = b;
        te.buttons &= ~b;
        if (te.buttons == Qt::NoButton)
            setDrag(Pointer::None);
        if (seen) {
            type = QEvent::TabletRelease;
            button = b;
        }
        if (tool_) {
            if (seen)
                taken = tool_->mouseRelease(te) || taken;
            else
                tool_->mouseRelease({stylusCell_, stylusHalfRow_, b, te.buttons, e->modifiers(), stylusPressure_});
        }
    }
    te.buttons = now & ~pressed;
    // A release reads no pressure: it lands with the pressure the stroke had.
    // Whether the event is one is the view's call, not Qt's.
    if (pressed || !here)
        te.pressure = stylusPressure_ = std::clamp(e->pressure(), 0.0, 1.0);
    for (const Qt::MouseButton b : buttonsIn(pressed)) {
        if (type != QEvent::TabletPress) {
            type = QEvent::TabletPress;
            button = b;
        }
        te.button = b;
        te.buttons |= b;
        if (tool_ && tool_->mousePress(te)) {
            taken = true;
            setDrag(Pointer::Stylus);
            setFocus(Qt::MouseFocusReason);
        }
    }
    if (!pressed && !here) {
        te.button = Qt::NoButton;
        taken = tool_ && tool_->mouseMove(te);
    }
    stylusCell_ = te.cell;
    stylusHalfRow_ = te.halfRow;
    // Taken, Qt makes no mouse event of it; otherwise the mouse event moves
    // the cell cursor as a click does. The mouse event Qt would make of an
    // event it has the wrong type or button for would be wrong the same way,
    // so the view does here what the right one would have done and takes it.
    if (!taken && (e->type() != type || e->button() != button)) {
        if (type == QEvent::TabletPress)
            clickCell(p);
        taken = true;
    }
    return taken;
}

void CanvasView::focusOutEvent(QFocusEvent* e) {
    // Losing focus mid-drag (a window switch, a dialog, a popup) loses the
    // mouse too: the drag ends where it started, but a floating selection
    // from before it stays. Once a popup has had a mouse event, Qt no longer
    // sends the drag's release here, so a popup that opens ends a mouse drag
    // at once. A stylus's tablet events come here popup or not, so its drag
    // goes on and its own release ends it, through a menu opened from the
    // keyboard too (Alt+F takes the focus for the popup, then for the menu
    // bar; Alt tapped alone, for the menu bar). The window then watches the
    // undo key for the view (eventFilter).
    const bool menu = e->reason() == Qt::PopupFocusReason || e->reason() == Qt::MenuBarFocusReason;
    if (!menu || drag_ != Pointer::Stylus)
        cancelDrag();
    else
        window()->installEventFilter(this);
    QWidget::focusOutEvent(e);
}

void CanvasView::mousePressEvent(QMouseEvent* e) {
    if (othersDrag(pointerOf(e)))
        return;
    if (tool_ && tool_->mousePress(toolEvent(*this, e))) {
        setDrag(pointerOf(e));
        setFocus(Qt::MouseFocusReason);
        return;
    }
    clickCell(toView(e->position()));
}

void CanvasView::clickCell(QPoint p) {
    const QSize cs = renderer_->cellSize() * zoom_;
    if (cs.isEmpty() || p.x() < 0 || p.y() < 0)
        return;
    setCellCursor({p.x() / cs.width(), p.y() / cs.height()});
    setFocus(Qt::MouseFocusReason);
}

PreviewWidget::PreviewWidget(CanvasRenderer* renderer, QWidget* parent) : QWidget(parent), renderer_(renderer) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    updateSize();
}

QSize PreviewWidget::sizeHint() const { return DeviceGrid::of(this).widgetSize(renderer_->imageSize()); }

void PreviewWidget::canvasReset() {
    updateSize();
    update();
}

void PreviewWidget::updateSize() {
    sizedDpr_ = devicePixelRatioF();
    setFixedSize(sizeHint());
}

void PreviewWidget::dprChanged() {
    updateSize();
    update();
}

void PreviewWidget::cellsChanged(const QRegion& cells) {
    update(toWidget(DeviceGrid::of(this), toPixels(cells, renderer_->cellSize(), 1)));
}

bool PreviewWidget::event(QEvent* e) {
#if BW_DPR_CHANGE_EVENT
    if (e->type() == QEvent::DevicePixelRatioChange)
        dprChanged();
#endif
    return QWidget::event(e);
}

void PreviewWidget::paintEvent(QPaintEvent* e) {
#if !BW_DPR_CHANGE_EVENT
    if (devicePixelRatioF() != sizedDpr_)
        QTimer::singleShot(0, this, &PreviewWidget::dprChanged);
#endif
    QPainter p(this);
    const QRect clip = beginViewPaint(p, e->rect(), renderer_->imageSize(), palette().color(backgroundRole()));
    const QRect src = clip & QRect(QPoint(0, 0), renderer_->imageSize());
    renderer_->paint(p, src, src.topLeft(), 1);
}

} // namespace bw
