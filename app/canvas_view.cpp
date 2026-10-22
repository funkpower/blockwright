#include "canvas_view.h"

#include "document.h"

#include "blockwright/glyph_sets.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <string>

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

// The 1x pixels a widget rect at `zoom` shows.
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

} // namespace

CanvasView::CanvasView(Document* doc, CanvasRenderer* renderer, QWidget* parent)
    : QWidget(parent), doc_(doc), renderer_(renderer), glyphSet_(kDefaultGlyphSet) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Dead keys and compose sequences arrive as input-method commits.
    setAttribute(Qt::WA_InputMethodEnabled);
    // Undo and redo put the cursor back where the edit was made or left it.
    connect(doc_, &Document::cursorRequested, this, &CanvasView::setCellCursor);
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
    update(cellRect(cursor_));
    cursor_ = c;
    update(cellRect(cursor_));
    if (hasFocus())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    emit cursorMoved(cursor_);
}

QRect CanvasView::cellRect(Cursor c) const {
    const QSize cs = renderer_->cellSize() * zoom_;
    return QRect(QPoint(c.x * cs.width(), c.y * cs.height()), cs);
}

QSize CanvasView::sizeHint() const { return renderer_->imageSize() * zoom_; }

QVariant CanvasView::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle)
        return cellRect(cursor_);
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

void CanvasView::cellsChanged(const QRegion& cells) { update(toPixels(cells, renderer_->cellSize(), zoom_)); }

void CanvasView::updateSize() { setFixedSize(sizeHint()); }

void CanvasView::paintEvent(QPaintEvent* e) {
    QPainter p(this);
    const QRect src = toSource(e->rect(), zoom_);
    renderer_->paint(p, src, src.topLeft() * zoom_, zoom_);

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

void CanvasView::commit(const EditResult& r) {
    doc_->edit(r.edit, cursor_, r.cursor);
    setCellCursor(r.cursor);
}

void CanvasView::keyPressEvent(QKeyEvent* e) {
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
    case Qt::Key_Backspace: commit(backspace(cv, cursor_)); return;
    case Qt::Key_Delete: commit(deleteCell(cv, cursor_)); return;
    default: break;
    }
    if (e->key() >= Qt::Key_F1 && e->key() < Qt::Key_F1 + kGlyphSetKeys &&
        !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
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
    commit(replaceText(doc_->canvas(), cursor_, replaceStart, replaceLength, glyphs, pen_));
}

void CanvasView::mousePressEvent(QMouseEvent* e) {
    const QSize cs = renderer_->cellSize() * zoom_;
    const QPoint p = e->position().toPoint();
    if (cs.isEmpty() || p.x() < 0 || p.y() < 0)
        return;
    setCellCursor({p.x() / cs.width(), p.y() / cs.height()});
    setFocus(Qt::MouseFocusReason);
}

PreviewWidget::PreviewWidget(CanvasRenderer* renderer, QWidget* parent) : QWidget(parent), renderer_(renderer) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFixedSize(sizeHint());
}

QSize PreviewWidget::sizeHint() const { return renderer_->imageSize(); }

void PreviewWidget::canvasReset() {
    setFixedSize(sizeHint());
    update();
}

void PreviewWidget::cellsChanged(const QRegion& cells) { update(toPixels(cells, renderer_->cellSize(), 1)); }

void PreviewWidget::paintEvent(QPaintEvent* e) {
    QPainter p(this);
    renderer_->paint(p, e->rect(), e->rect().topLeft(), 1);
}

} // namespace bw
