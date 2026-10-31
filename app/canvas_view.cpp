#include "canvas_view.h"

#include "device_grid.h"
#include "document.h"
#include "reference_scale.h"
#include "settings.h"
#include "tool.h"

#include "blockwright/composite.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/selection.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QBuffer>
#include <QGuiApplication>
#include <QImageReader>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUndoStack>
#include <QWhatsThis>
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

// The texture stamp's settings: its kind by name (a name not known reads as
// scales), density and seed.
const QString kTextureKindKey = QStringLiteral("texture/kind");
const QString kTextureDensityKey = QStringLiteral("texture/density");
const QString kTextureSeedKey = QStringLiteral("texture/seed");
constexpr std::pair<TextureKind, const char*> kTextureKindNames[] = {
    {TextureKind::Scales, "scales"}, {TextureKind::Foliage, "foliage"}, {TextureKind::Hoard, "hoard"},
    {TextureKind::Brick, "brick"},   {TextureKind::Noise, "noise"},
};

TextureOptions savedTextureOptions() {
    const auto s = appSettings();
    TextureOptions t;
    const QString kind = s->value(kTextureKindKey).toString();
    for (const auto& [k, name] : kTextureKindNames)
        if (kind == QLatin1String(name))
            t.kind = k;
    bool ok = false;
    const double density = s->value(kTextureDensityKey).toDouble(&ok);
    if (ok && std::isfinite(density))
        t.density = std::clamp(density, kMinTextureDensity, kMaxTextureDensity);
    const int seed = s->value(kTextureSeedKey).toInt(&ok);
    if (ok)
        t.seed = std::clamp(seed, 0, kMaxTextureSeed);
    return t;
}

void saveTextureOptions(const TextureOptions& t) {
    const auto s = appSettings();
    for (const auto& [k, name] : kTextureKindNames)
        if (k == t.kind)
            s->setValue(kTextureKindKey, QLatin1String(name));
    s->setValue(kTextureDensityKey, t.density);
    s->setValue(kTextureSeedKey, t.seed);
}

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
    texture_ = savedTextureOptions();
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
        // The new document already has its own composite, possibly with the
        // same layer ids. No preview from the old document belongs on it.
        const std::optional<LayerMove> m = layerMove_;
        if (m)
            emit layerMoveStatus({});
        layerMove_.reset();
        if (m)
            emit layerShown(m->id);
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
    connect(doc_, &Document::commandAboutToPush, this, [this] {
        endLayerMove(false);
    });
    // A layer panel change lands what the tool holds on the layer it was
    // editing first. The alpha lock is the active layer's, so a change of
    // layer properties or of active layer changes the brush settings; a new
    // active layer is other cells under the tool.
    connect(doc_, &Document::layersAboutToChange, this, &CanvasView::finishTool);
    connect(doc_, &Document::layersChanged, this, &CanvasView::brushSettingsChanged);
    // a reference layer's picture, place or opacity changes no cell
    connect(doc_, &Document::layersChanged, this, qOverload<>(&QWidget::update));
    // nor does art that covers one the way the composite already showed
    connect(doc_, &Document::layerCellsChanged, this, &CanvasView::cellsChanged);
    connect(doc_, &Document::activeLayerChanged, this, [this] {
        cancelTool();
        brushSettingsChanged();
        update();  // the Move/Transform tool frames the active reference layer
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

void CanvasView::setTextureOptions(const TextureOptions& options) {
    TextureOptions t = options;
    t.density = std::clamp(t.density, kMinTextureDensity, kMaxTextureDensity);
    t.seed = std::clamp(t.seed, 0, kMaxTextureSeed);
    if (t == texture_)
        return;
    texture_ = t;
    saveTextureOptions(t);
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
    tellInputMethod();
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
    endLayerMove(false);
    if (tool_) {
        disconnect(tool_, nullptr, this, nullptr);
        tool_->deactivate();
    }
    tool_ = tool;
    if (!pan_)  // a pan goes on under the new tool, and keeps its owner
        setDrag(Pointer::None);
    if (tool_) {
        connect(tool_, &Tool::changed, this, qOverload<>(&QWidget::update));
        // A drag can end in the tool, from a menu action or the document:
        // its filter ends with it.
        connect(tool_, &Tool::changed, this, &CanvasView::syncDrag);
        // its text cursor may have moved
        connect(tool_, &Tool::changed, this, &CanvasView::tellInputMethod);
        tool_->activate(this);
        connect(tool_, &Tool::changed, this, &CanvasView::followCaret);
    }
    // a tool picked up doesn't scroll to a text cursor it has
    caret_ = tool_ ? tool_->drawnCaret() : std::nullopt;
    update();
    tellInputMethod();
    emit toolChanged();
}

bool CanvasView::applyEdit(const CellEdit& edit, bool mirrored) {
    const QScopedValueRollback<bool> own(ownEdit_, true);
    return doc_->edit(mirrored ? edit : this->mirrored(edit), cursor_, cursor_);
}

void CanvasView::ownChange(const std::function<void()>& change) {
    const QScopedValueRollback<bool> own(ownEdit_, true);
    change();
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
    endLayerMove(false);
    if (!pan_)
        setDrag(Pointer::None);
    if (tool_)
        tool_->cancel();
}

void CanvasView::cancelDrag() {
    endLayerMove(false);
    endPan();
    if (!tool_ || !tool_->dragging()) {
        syncDrag();
        return;
    }
    setDrag(Pointer::None);
    tool_->cancelDrag();
}

void CanvasView::finishTool() {
    endLayerMove(false);
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

QSize CanvasView::sizeHint() const { return DeviceGrid::of(this).widgetSize(viewSize()); }

QVariant CanvasView::inputMethodQuery(Qt::InputMethodQuery query) const {
    // where the text goes: the tool's text cursor, else the cell cursor
    if (query == Qt::ImCursorRectangle) {
        const std::optional<QRect> caret = tool_ ? tool_->caretRect() : std::nullopt;
        return toWidget(caret ? *caret : cellRect(cursor_));
    }
    return QWidget::inputMethodQuery(query);
}

void CanvasView::tellInputMethod() {
    if (hasFocus())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
}

void CanvasView::followCaret() {
    // only a move: a change that leaves the text cursor where it was
    // doesn't take the view back from where it was scrolled to. During an
    // Alt+drag it rides with the letters unfollowed: the view scrolling
    // under a held pointer would move the layer again. It is kept, so the
    // move landing where the drag showed it doesn't scroll either.
    const std::optional<QRect> at = tool_->drawnCaret();
    if (at == caret_)
        return;
    caret_ = at;
    if (at && !layerMove_)
        emit caretMoved(*at);
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
    // The tool's cells in progress are the composite as they will leave it;
    // each reference layer is drawn over them where the art above it, those
    // cells among it, leaves it uncovered (uncoveredBy).
    if (tool_) {
        const SavedPainter saved(p);
        tool_->paintCells(p, clip);
    }
    paintReferenceLayers(p, clip);
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

void CanvasView::paintReferenceLayers(QPainter& p, const QRect& clip) {
    const LayerStack& stack = doc_->layers();
    std::vector<const Layer*> shown;
    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const LayerProperties& l = stack[i].props;
        if (l.kind == LayerKind::Reference && l.visible && l.reference) {
            shown.push_back(&stack[i]);
            at.push_back(i);
        }
    }
    // drop what no visible layer shows any more
    const auto shows = [&](const std::shared_ptr<const std::string>& bytes) {
        for (const Layer* l : shown)
            if (l->props.reference->bytes == bytes || *l->props.reference->bytes == *bytes)
                return true;
        return false;
    };
    std::erase_if(pictures_, [&](const Picture& k) { return !shows(k.bytes); });
    std::erase_if(scaled_, [&](const Scaled& k) {
        const Layer* l = stack.find(k.id);
        return !l || !l->props.visible || !l->props.reference || l->props.reference->bytes != k.bytes;
    });
    if (shown.empty())
        return;
    const QRect canvas = clip & QRect(QPoint(0, 0), viewSize());
    // the copies' share of the cap
    const qint64 budget = kMaxReferencePixels / qint64(shown.size());
    for (std::size_t n = 0; n < shown.size(); ++n) {
        const Layer* l = shown[n];
        const ReferenceImage& r = *l->props.reference;
        // A move or resize in progress shows the layer where the drag has it,
        // the copy made for where it was stretched across that: a resize
        // scales it again only once it lands.
        const bool dragged = layerMove_ && layerMove_->reference && layerMove_->id == l->props.id;
        const QRect place = dragged ? layerMove_->frame : QRect(r.x, r.y, r.width, r.height);
        // canvas pixels to view pixels: the zoom
        const QRectF to(QPointF(place.x(), place.y()) * zoom_, QSizeF(place.width(), place.height()) * zoom_);
        const QSize target(int(std::min<double>(to.width(), INT_MAX)), int(std::min<double>(to.height(), INT_MAX)));
        const QSize cache = fitPixels(to.width(), to.height(), budget);
        auto it = std::find_if(scaled_.begin(), scaled_.end(), [&](const Scaled& k) { return k.id == l->props.id; });
        const bool stretch = dragged && it != scaled_.end() && !it->image.isNull();
        if (!stretch && (it == scaled_.end() || it->target != target || it->cache != cache)) {
            if (it == scaled_.end())
                it = scaled_.insert(scaled_.end(), Scaled{l->props.id, r.bytes, {}, {}, {}});
            it->target = target;
            it->cache = cache;
            it->image = {};
            const QImage& source = referencePicture(r);
            try {
                QImage scaled = source.isNull() ? QImage{} : QImage(cache, QImage::Format_ARGB32_Premultiplied);
                if (!source.isNull() && !scaled.isNull()) {
                    scaled.fill(Qt::transparent);
                    const double kx = double(cache.width()) / source.width();
                    const double ky = double(cache.height()) / source.height();
                    bool ok = true;
                    if (kx >= 1 && ky >= 1) {
                        // enlarged: interpolated
                        QPainter sp(&scaled);
                        sp.setRenderHint(QPainter::SmoothPixmapTransform);
                        sp.drawImage(QRect(QPoint(0, 0), cache), source);
                    } else {
                        // reduced: averaged, as interpolating would alias
                        ok = areaScale(source, scaled, kx, ky);
                    }
                    if (ok)
                        it->image = std::move(scaled);
                }
            } catch (const std::bad_alloc&) {
                it->image = {};
            }
            // a failure stays cached with its sizes, so it isn't retried on
            // every paint
            if (it->image.isNull())
                emit referenceFailed();
        }
        if (it->image.isNull() || !to.intersects(canvas))
            continue;
        const SavedPainter saved(p);
        p.setClipRect(canvas, Qt::IntersectClip);
        // under the art above it in the stack
        p.setClipRegion(uncoveredBy(at[n], canvas & to.toAlignedRect()), Qt::IntersectClip);
        p.setOpacity(r.opacity / 100.0);
        if (it->image.size() == target && to.topLeft() == to.toRect().topLeft()) {
            const QRect part = canvas & to.toRect();
            p.drawImage(part.topLeft(), it->image, part.translated(-to.toRect().topLeft()));
        } else {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(to, it->image);
        }
    }
}

QRegion CanvasView::uncoveredBy(std::size_t index, const QRect& view) const {
    const QSize cs = renderer_->cellSize() * zoom_;
    if (view.isEmpty() || cs.isEmpty())
        return {};
    const QPoint a = cellAt(view.topLeft()), b = cellAt(view.bottomRight());
    const CompositeOptions options{doc_->format().ice, {}};
    // an art layer's move shows it where the drag has it
    ShownOffset shown;
    if (layerMove_ && layerMove_->id && !layerMove_->reference)
        shown = {LayerId(layerMove_->id), layerMove_->shown.x(), layerMove_->shown.y()};
    // and the active layer's cells in progress, as the tool will leave them
    ShownCells pending;
    if (tool_)
        if (const CellEdit& cells = tool_->layerCells(); !cells.changes.empty())
            pending = {doc_->activeLayer(), &cells.changes};
    const Coverage cover = coverageAbove(doc_->layers(), index, {a.x(), a.y(), b.x() - a.x() + 1, b.y() - a.y() + 1},
                                         options, shown, pending);
    if (cover.rect.empty())
        return {};
    // A cell's top half is the view rows halfRowAt reads as it.
    const int top = (cs.height() + 1) / 2;
    // the uncovered runs of each half row, top to bottom: y-x banded
    std::vector<QRect> rects;
    for (int y = cover.rect.y; y < cover.rect.y + cover.rect.height; ++y)
        for (const std::uint8_t half : {Coverage::kTop, Coverage::kBottom}) {
            const int y0 = y * cs.height() + (half == Coverage::kTop ? 0 : top);
            const int h = half == Coverage::kTop ? top : cs.height() - top;
            if (h <= 0)
                continue;
            for (int x = cover.rect.x; x < cover.rect.x + cover.rect.width;) {
                if (cover.at(x, y) & half) {
                    ++x;
                    continue;
                }
                const int x0 = x;
                while (x < cover.rect.x + cover.rect.width && !(cover.at(x, y) & half))
                    ++x;
                rects.emplace_back(x0 * cs.width(), y0, (x - x0) * cs.width(), h);
            }
        }
    QRegion region;
    region.setRects(rects.data(), int(rects.size()));
    return region;
}

QSize CanvasView::referenceCacheSize(LayerId id) const {
    for (const Scaled& k : scaled_)
        if (k.id == id)
            return k.image.size();
    return {};
}

const QImage& CanvasView::referencePicture(const ReferenceImage& picture) {
    for (const Picture& k : pictures_)
        if (k.bytes == picture.bytes || *k.bytes == *picture.bytes)
            return k.image;
    QImage image = readReferencePicture(
        QByteArray::fromRawData(picture.bytes->data(), qsizetype(picture.bytes->size())), kMaxReferencePixels);
    if (!image.isNull())
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    pictures_.push_back(Picture{picture.bytes, std::move(image)});
    return pictures_.back().image;
}

void CanvasView::keepReferencePicture(const std::shared_ptr<const std::string>& bytes, QImage image) {
    for (const Picture& k : pictures_)
        if (k.bytes == bytes || *k.bytes == *bytes)
            return;
    if (!image.isNull())
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    pictures_.push_back(Picture{bytes, std::move(image)});
}

QSize CanvasView::referencePictureSize(LayerId id) const {
    const Layer* l = doc_->layers().find(id);
    if (!l || !l->props.reference)
        return {};
    for (const Picture& k : pictures_)
        if (*k.bytes == *l->props.reference->bytes)
            return k.image.size();
    return {};
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
    // with nothing to undo. So is Shift+F1, What's This's key: the mode
    // would take the drag's release and leave it stuck. It is F1 with Shift
    // held, as Shift+F2 to F10 are (the glyph brush re-picks its glyph).
    if (e->type() == QEvent::ShortcutOverride && dragging()) {
        const auto* k = static_cast<QKeyEvent*>(e);
        if (k->matches(QKeySequence::Undo) ||
            QKeySequence(k->keyCombination()) == QKeySequence(Qt::SHIFT | Qt::Key_F1)) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

bool CanvasView::eventFilter(QObject* watched, QEvent* e) {
    // The application, during a drag the mouse's events drive (setDrag):
    // another device's mouse events reach no widget, so they leave Qt's
    // record of the drag's press as it is.
    if (filtering_ && watched->isWindowType()) {
        switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
            if (pointerOf(static_cast<QMouseEvent*>(e)) != drag_)
                return true;
            break;
        default: break;
        }
    }
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
    if (layerMoveModifier(e))
        return;
    if (layerMove_ && e->key() == Qt::Key_Escape) {
        cancelDrag();
        return;
    }
    if (dragging() && e->matches(QKeySequence::Undo)) {
        cancelDrag();
        return;
    }
    if (tool_ && tool_->keyPress(e)) {
        syncDrag();
        return;
    }
    const Canvas& cv = doc_->layerCanvas();
    // With Ctrl or Meta held these are chords an action can have (PLAN §3),
    // as Ctrl+arrows step the pen, so keyboard mode leaves them alone.
    if (!(e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
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

void CanvasView::keyReleaseEvent(QKeyEvent* e) {
    if (!layerMoveModifier(e))
        QWidget::keyReleaseEvent(e);
}

bool CanvasView::layerMoveModifier(QKeyEvent* e) {
    if (!layerMove_ || e->key() != Qt::Key_Shift)
        return false;
    // A resize frees or keeps its aspect as Shift goes down or up, where
    // the pointer is, not on its next move: the frame shown is the one a
    // release there lands.
    if (layerMove_->reference && layerMove_->corner >= 0) {
        Qt::KeyboardModifiers modifiers = e->modifiers();
        modifiers.setFlag(Qt::ShiftModifier, e->type() == QEvent::KeyPress);
        moveLayerTo(layerMove_->at, modifiers);
    }
    return true;
}

void CanvasView::inputMethodEvent(QInputMethodEvent* e) {
    if (tool_ && (!e->commitString().isEmpty() || e->replacementLength() > 0) &&
        tool_->inputText(e->commitString(), e->replacementStart(), e->replacementLength())) {
        e->accept();
        return;
    }
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
    // A drag the mouse's events drive keeps the other device's mouse events
    // from every window (eventFilter). Qt sends a press's moves and release
    // to the widget it was pressed on only until a release with no buttons
    // held, and another device's release is one (#45): the drag's own would
    // then go nowhere. The view takes no mouse grab for that: the grab would
    // be the window system's too, which Wayland grants only to a popup,
    // warning at every grab and release (#101). Moves and the release out of
    // the view or the window reach it without one, by Qt's record of the
    // press and the window system's implicit grab on it.
    // A view that isn't shown filters nothing: no window input reaches it.
    const bool filter = (p == Pointer::Mouse || p == Pointer::StylusMouse) && dragging() && isVisible();
    if (filter == filtering_)
        return;
    filtering_ = filter;
    if (filter)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
}

void CanvasView::syncDrag() {
    if (!dragging())
        setDrag(Pointer::None);
}

bool CanvasView::dragging() const {
    return layerMove_.has_value() || pan_.has_value() || (tool_ && tool_->dragging());
}

QPoint CanvasView::canvasPixelAt(QPoint p) const {
    return {clampInt(floorDiv(p.x(), zoom_)), clampInt(floorDiv(p.y(), zoom_))};
}

namespace {

// A reference layer's place and size in canvas pixels, as a rect.
QRect referenceRect(const ReferenceImage& r) { return {r.x, r.y, r.width, r.height}; }

// The corners of a frame, in referenceHandleAt's order: its edges, not
// its last pixels.
QPoint cornerOf(const QRect& r, int corner) {
    const bool left = corner == 0 || corner == 3, top = corner == 0 || corner == 1;
    return {left ? r.x() : r.x() + r.width(), top ? r.y() : r.y() + r.height()};
}

int clampPlace(long long v) {
    return int(std::clamp<long long>(v, -ReferenceImage::kMaxPlace, ReferenceImage::kMaxPlace));
}

} // namespace

std::optional<QRect> CanvasView::referenceFrame() const {
    const Layer* layer = doc_->active();
    if (!layer || layer->props.kind != LayerKind::Reference || !layer->props.visible || !layer->props.reference)
        return std::nullopt;
    const QRect r = layerMove_ && layerMove_->reference && layerMove_->id == layer->props.id
                        ? layerMove_->frame
                        : referenceRect(*layer->props.reference);
    const auto view = [&](long long v) { return clampInt(v * zoom_); };
    return QRect(QPoint(view(r.x()), view(r.y())),
                 QSize(view((long long)r.width()), view((long long)r.height())));
}

int CanvasView::referenceHandleAt(QPoint p) const {
    const std::optional<QRect> frame = referenceFrame();
    if (!frame)
        return -1;
    for (int c = 0; c < 4; ++c) {
        const QPoint at = cornerOf(*frame, c);
        if (std::abs((long long)p.x() - at.x()) <= kHandle / 2 && std::abs((long long)p.y() - at.y()) <= kHandle / 2)
            return c;
    }
    return -1;
}

void CanvasView::beginLayerMove(QPoint p) {
    finishTool();
    setFocus(Qt::MouseFocusReason);
    const QPoint cell = cellAt(p);
    const Layer* layer = doc_->active();
    const bool reference = layer && layer->props.kind == LayerKind::Reference;
    if (!layer || layer->props.locked || !layer->props.visible || (reference && !layer->props.reference)) {
        layerMove_ = LayerMove{0, cell, {}, {}, {}};
        emit layerMoveStatus(!layer                 ? tr("No active layer to move")
                             : layer->props.locked  ? tr("Cannot move layer: locked")
                             : !layer->props.visible ? tr("Cannot move layer: hidden")
                                                     : tr("Cannot move layer: no picture"));
    } else if (reference) {
        const QRect from = referenceRect(*layer->props.reference);
        LayerMove m{layer->props.id, canvasPixelAt(p), from.topLeft(), from.topLeft(), from.size()};
        m.reference = true;
        m.from = m.frame = from;
        m.corner = tool_ && tool_->movesLayer() ? referenceHandleAt(p) : -1;
        layerMove_ = m;
        moveLayerTo(p, {});
    } else {
        const QPoint origin(layer->props.x, layer->props.y);
        layerMove_ = LayerMove{layer->props.id, cell, origin, origin, {layer->width(), layer->height()}};
        moveLayerTo(p, {});
    }
}

std::optional<QPoint> CanvasView::shownOffset(LayerId id) const {
    if (layerMove_ && id && layerMove_->id == id && !layerMove_->reference)
        return layerMove_->shown;
    return std::nullopt;
}

void CanvasView::moveLayerTo(QPoint p, Qt::KeyboardModifiers modifiers) {
    if (!layerMove_ || !layerMove_->id)
        return;
    LayerMove& m = *layerMove_;
    if (m.reference) {
        m.at = p;
        const QPoint at = canvasPixelAt(p);
        const long long dx = (long long)at.x() - m.press.x(), dy = (long long)at.y() - m.press.y();
        QRect frame;
        if (m.corner < 0) {
            frame = QRect(QPoint(clampPlace(m.from.x() + dx), clampPlace(m.from.y() + dy)), m.from.size());
        } else {
            // The opposite corner stays put; the dragged one follows the
            // pointer, by how far it has gone since the press.
            const bool left = m.corner == 0 || m.corner == 3, top = m.corner == 0 || m.corner == 1;
            const QPoint anchor = cornerOf(m.from, (m.corner + 2) % 4);
            const QPoint start = cornerOf(m.from, m.corner);
            const long long m1 = ReferenceImage::kMaxPlace;
            long long w = std::clamp<long long>(left ? anchor.x() - (start.x() + dx) : start.x() + dx - anchor.x(), 1, m1);
            long long h = std::clamp<long long>(top ? anchor.y() - (start.y() + dy) : start.y() + dy - anchor.y(), 1, m1);
            if (!(modifiers & Qt::ShiftModifier)) {
                // the aspect kept: the side the pointer took further leads
                const double w0 = m.from.width(), h0 = m.from.height();
                const double s = std::min(std::max(w / w0, h / h0), m1 / std::max(w0, h0));
                w = std::clamp<long long>(std::llround(w0 * s), 1, m1);
                h = std::clamp<long long>(std::llround(h0 * s), 1, m1);
            }
            frame = QRect(QPoint(clampPlace(left ? anchor.x() - w : anchor.x()), clampPlace(top ? anchor.y() - h : anchor.y())),
                          QSize(int(w), int(h)));
        }
        if (frame != m.frame) {
            m.frame = frame;
            update();
        }
        if (m.corner < 0)
            emit layerMoveStatus(tr("Move Layer: dx %1, dy %2 | x %3, y %4")
                                     .arg((long long)frame.x() - m.from.x())
                                     .arg((long long)frame.y() - m.from.y())
                                     .arg(frame.x())
                                     .arg(frame.y()));
        else
            emit layerMoveStatus(tr("Resize Layer: w %1, h %2 | x %3, y %4")
                                     .arg(frame.width())
                                     .arg(frame.height())
                                     .arg(frame.x())
                                     .arg(frame.y()));
        return;
    }
    const QPoint cell = cellAt(p);
    const QPoint to(clampInt((long long)m.origin.x() + cell.x() - m.press.x()),
                    clampInt((long long)m.origin.y() + cell.y() - m.press.y()));
    if (to != m.shown) {
        const QPoint from = m.shown;
        m.shown = to;
        ownChange([&] {
            doc_->previewLayerOffset(m.id, {from.x(), from.y(), m.size.width(), m.size.height()}, to);
        });
        emit layerShown(m.id);
    }
    emit layerMoveStatus(tr("Move Layer: dx %1, dy %2 | x %3, y %4")
                             .arg((long long)to.x() - m.origin.x())
                             .arg((long long)to.y() - m.origin.y())
                             .arg(to.x())
                             .arg(to.y()));
}

void CanvasView::endLayerMove(bool commit) {
    if (!layerMove_)
        return;
    const LayerMove m = *layerMove_;
    layerMove_.reset();
    setDrag(Pointer::None);
    emit layerMoveStatus({});
    if (!m.id)
        return;
    if (m.reference) {
        // Only the view showed the drag: it shows the document's place again,
        // or the one the edit lands.
        update();
        const Layer* layer = doc_->layers().find(m.id);
        if (commit && m.frame != m.from && layer && layer->props.reference) {
            LayerProperties props = layer->props;
            props.reference->x = m.frame.x();
            props.reference->y = m.frame.y();
            props.reference->width = m.frame.width();
            props.reference->height = m.frame.height();
            ownChange([&] {
                doc_->setLayerProperties(m.id, std::move(props),
                                         m.frame.size() == m.from.size() ? tr("Move Layer") : tr("Resize Layer"));
            });
        }
        return;
    }
    ownChange([&] {
        // A command from elsewhere may already have moved or removed the
        // layer. Drop the preview onto the current document, not its start.
        const Layer* current = doc_->layers().find(m.id);
        const QPoint actual = current ? QPoint(current->props.x, current->props.y) : m.origin;
        doc_->previewLayerOffset(m.id, {m.shown.x(), m.shown.y(), m.size.width(), m.size.height()}, actual);
        if (commit && m.shown != m.origin) {
            if (const Layer* layer = doc_->layers().find(m.id)) {
                LayerProperties props = layer->props;
                props.x = m.shown.x();
                props.y = m.shown.y();
                doc_->setLayerProperties(m.id, std::move(props), tr("Move Layer"));
            }
        }
    });
    // A landed move shows the layer where the preview did, and its step tells.
    const Layer* landed = doc_->layers().find(m.id);
    if (!landed || QPoint(landed->props.x, landed->props.y) != m.shown)
        emit layerShown(m.id);
}

bool CanvasView::othersDrag(Pointer p) {
    syncDrag();
    return drag_ != Pointer::None && drag_ != p;
}

namespace {

// The scroll area `w` is in, if any.
QAbstractScrollArea* scrollAreaOf(const QWidget* w) {
    for (QWidget* p = w->parentWidget(); p; p = p->parentWidget())
        if (auto* s = qobject_cast<QAbstractScrollArea*>(p))
            return s;
    return nullptr;
}

} // namespace

void CanvasView::panTo(QPointF global) {
    if (QAbstractScrollArea* s = scrollAreaOf(this)) {
        const QPoint by = (global - pan_->start).toPoint();
        s->horizontalScrollBar()->setValue(pan_->scroll.x() - by.x());
        s->verticalScrollBar()->setValue(pan_->scroll.y() - by.y());
    }
}

void CanvasView::endPan() {
    if (!pan_)
        return;
    pan_.reset();
    unsetCursor();
    syncDrag();
}

void CanvasView::mouseMoveEvent(QMouseEvent* e) {
    if (othersDrag(pointerOf(e)))
        return;
    if (pan_) {
        panTo(e->globalPosition());
        return;
    }
    if (layerMove_) {
        moveLayerTo(toView(e->position()), e->modifiers());
        return;
    }
    if (!tool_ || !tool_->mouseMove(toolEvent(*this, e)))
        QWidget::mouseMoveEvent(e);
}

void CanvasView::mouseReleaseEvent(QMouseEvent* e) {
    if (othersDrag(pointerOf(e)))
        return;
    if (pan_) {
        if (e->button() == Qt::MiddleButton) {
            panTo(e->globalPosition());
            endPan();
        }
        return;
    }
    if (layerMove_) {
        if (e->button() == Qt::LeftButton) {
            moveLayerTo(toView(e->position()), e->modifiers());
            endLayerMove(true);
        }
        return;
    }
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
    // What's This mode takes mouse clicks only: a pen not already drawing
    // leaves its events to Qt, which makes the click the mode takes.
    const bool whatsThis = QWhatsThis::inWhatsThisMode() && drag_ != Pointer::Stylus;
    const bool taken = !whatsThis && stylusEvent(e);
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
        if (layerMove_) {
            if (b == Qt::LeftButton) {
                if (seen)
                    moveLayerTo(p, e->modifiers());
                endLayerMove(true);
            }
            taken = true;
        } else if (tool_) {
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
        if (layerMove_) {
            taken = true;
        } else if (b == Qt::LeftButton && ((e->modifiers() & Qt::AltModifier) || (tool_ && tool_->movesLayer()))) {
            beginLayerMove(p);
            taken = true;
            setDrag(Pointer::Stylus);
        } else if (tool_ && tool_->mousePress(te)) {
            taken = true;
            setDrag(Pointer::Stylus);
            setFocus(Qt::MouseFocusReason);
        }
    }
    if (!pressed && !here) {
        te.button = Qt::NoButton;
        if (layerMove_) {
            moveLayerTo(p, e->modifiers());
            taken = true;
        } else {
            taken = tool_ && tool_->mouseMove(te);
        }
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
    // undo key for the view (eventFilter). A pan ends where it is.
    const bool menu = e->reason() == Qt::PopupFocusReason || e->reason() == Qt::MenuBarFocusReason;
    if (layerMove_ || !menu || drag_ != Pointer::Stylus)
        cancelDrag();
    else
        window()->installEventFilter(this);
    QWidget::focusOutEvent(e);
}

void CanvasView::mousePressEvent(QMouseEvent* e) {
    if (pan_)
        return;
    if (othersDrag(pointerOf(e)))
        return;
    if (layerMove_)
        return;
    if (QAbstractScrollArea* s = scrollAreaOf(this);
        s && e->button() == Qt::MiddleButton && e->buttons() == Qt::MiddleButton && !dragging()) {
        pan_ = Pan{e->globalPosition(), {s->horizontalScrollBar()->value(), s->verticalScrollBar()->value()}};
        setCursor(Qt::ClosedHandCursor);
        setDrag(pointerOf(e));
        return;
    }
    if (e->button() == Qt::LeftButton && ((e->modifiers() & Qt::AltModifier) || (tool_ && tool_->movesLayer()))) {
        beginLayerMove(toView(e->position()));
        setDrag(pointerOf(e));
        return;
    }
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
