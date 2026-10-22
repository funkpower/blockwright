#include "canvas_view.h"

#include "device_grid.h"
#include "document.h"
#include "reference_scale.h"
#include "tool.h"

#include "blockwright/glyph_sets.h"
#include "blockwright/selection.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <climits>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

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
    // Dead keys and compose sequences arrive as input-method commits.
    setAttribute(Qt::WA_InputMethodEnabled);
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
    });
    // Undo or redo of any command, a format change among them, and a format
    // change made any other way end a drag: what it shows was worked out
    // on the document before.
    connect(doc_->undoStack(), &QUndoStack::indexChanged, this, [this] {
        if (!ownEdit_)
            cancelDrag();
    });
    connect(doc_, &Document::formatChanged, this, &CanvasView::cancelDrag);
    updateSize();
}

void CanvasView::setPen(const Pen& pen) {
    if (pen.fg == pen_.fg && pen.bg == pen_.bg)
        return;
    pen_ = pen;
    emit penChanged(pen_);
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
    toolMouse_ = false;
    if (tool_) {
        connect(tool_, &Tool::changed, this, qOverload<>(&QWidget::update));
        tool_->activate(this);
    }
    update();
}

bool CanvasView::applyEdit(const CellEdit& edit, bool mirrored) {
    const QScopedValueRollback<bool> own(ownEdit_, true);
    return doc_->edit(mirrored ? edit : this->mirrored(edit), cursor_, cursor_);
}

CellEdit CanvasView::mirrored(const CellEdit& edit) const {
    return mirror_ ? mirrorEdit(doc_->canvas(), edit) : edit;
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
    toolMouse_ = false;
    if (tool_)
        tool_->cancel();
}

void CanvasView::cancelDrag() {
    if (!tool_ || !tool_->dragging())
        return;
    toolMouse_ = false;
    tool_->cancelDrag();
}

void CanvasView::finishTool() {
    if (tool_)
        tool_->finish();
}

void CanvasView::setMirror(bool on) {
    if (on == mirror_)
        return;
    mirror_ = on;
    emit mirrorChanged(on);
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

void CanvasView::keyPressEvent(QKeyEvent* e) {
    if (tool_ && tool_->dragging() && e->matches(QKeySequence::Undo)) {
        cancelDrag();
        return;
    }
    if (tool_ && tool_->keyPress(e))
        return;
    const Canvas& cv = doc_->canvas();
    switch (e->key()) {
    case Qt::Key_Left: setCellCursor(moveCursor(cv, cursor_, -1, 0)); return;
    case Qt::Key_Right: setCellCursor(moveCursor(cv, cursor_, 1, 0)); return;
    case Qt::Key_Up: setCellCursor(moveCursor(cv, cursor_, 0, -1)); return;
    case Qt::Key_Down: setCellCursor(moveCursor(cv, cursor_, 0, 1)); return;
    case Qt::Key_Home: setCellCursor({0, cursor_.y}); return;
    case Qt::Key_End: setCellCursor({cv.width() - 1, cursor_.y}); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: setCellCursor(moveCursor(cv, {0, cursor_.y}, 0, 1)); return;
    case Qt::Key_Backspace: finishTool(); commit(backspace(cv, cursor_)); return;
    case Qt::Key_Delete: finishTool(); commit(deleteCell(cv, cursor_)); return;
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
    commit(replaceText(doc_->canvas(), cursor_, replaceStart, replaceLength, glyphs, pen_));
}

namespace {

ToolEvent toolEvent(const CanvasView& v, const QMouseEvent* e) {
    const QPoint p = v.toView(e->position());
    return {v.cellAt(p), v.halfRowAt(p), e->button(), e->buttons(), e->modifiers()};
}

} // namespace

void CanvasView::mouseMoveEvent(QMouseEvent* e) {
    if (!tool_ || !tool_->mouseMove(toolEvent(*this, e)))
        QWidget::mouseMoveEvent(e);
}

void CanvasView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->buttons() == Qt::NoButton)
        toolMouse_ = false;
    if (!tool_ || !tool_->mouseRelease(toolEvent(*this, e)))
        QWidget::mouseReleaseEvent(e);
}

void CanvasView::focusOutEvent(QFocusEvent* e) {
    // Losing focus mid-drag (a window switch, a dialog) loses the mouse too:
    // the drag ends where it started, but a floating selection from before
    // it stays. A menu popup doesn't lose the mouse, and a floating selection
    // stays for the menus to work on.
    if (toolMouse_ && e->reason() != Qt::PopupFocusReason) {
        toolMouse_ = false;
        if (tool_)
            tool_->cancelDrag();
    }
    QWidget::focusOutEvent(e);
}

void CanvasView::mousePressEvent(QMouseEvent* e) {
    if (tool_ && tool_->mousePress(toolEvent(*this, e))) {
        toolMouse_ = true;
        setFocus(Qt::MouseFocusReason);
        return;
    }
    const QSize cs = renderer_->cellSize() * zoom_;
    const QPoint p = toView(e->position());
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
