#include "blockwright/canvas_renderer.h"

#include <QPainter>

#include <stdexcept>

namespace bw {

namespace {

// Matches renderCanvas's limit, so anything the CLI can render the GUI can
// show.
constexpr qint64 kMaxPixels = qint64(1) << 28;

QRgb toQRgb(Rgb c) { return qRgb(c.r, c.g, c.b); }

} // namespace

GlyphAtlas::GlyphAtlas(const BitmapFont& font, int glyphWidth) : font_(&font), gw_(glyphWidth) {
    if (gw_ != 8 && gw_ != 9)
        throw std::invalid_argument("font width must be 8 or 9");
    atlas_ = QImage(256 * gw_, 16 * font.height, QImage::Format_ARGB32_Premultiplied);
    atlas_.fill(Qt::transparent);
    for (int g = 0; g < 256; ++g) {
        bool any = false;
        for (int y = 0; y < font.height && !any; ++y)
            for (int x = 0; x < 8 && !any; ++x)
                any = font.pixel(std::uint8_t(g), x, y);
        blank_[g] = !any;
    }
    QPainter p(&atlas_);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    for (int c = 0; c < 16; ++c)
        for (int g = 0; g < 256; ++g)
            p.drawImage(g * gw_, c * font.height, drawGlyph(std::uint8_t(g), toQRgb(kPalette[c])));
}

QImage GlyphAtlas::drawGlyph(std::uint8_t g, QRgb color) const {
    QImage img(gw_, font_->height, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const bool lineDrawing = g >= 0xC0 && g <= 0xDF;
    for (int y = 0; y < font_->height; ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < gw_; ++x)
            if (x < 8 ? font_->pixel(g, x, y) : lineDrawing && font_->pixel(g, 7, y))
                line[x] = color | 0xff000000u;
    }
    return img;
}

QRect GlyphAtlas::sourceRect(std::uint8_t g, std::uint8_t index) const {
    return {g * gw_, (index & 15) * font_->height, gw_, font_->height};
}

std::pair<const QImage*, QRect> GlyphAtlas::glyph(std::uint8_t g, const Color& fg) {
    if (fg.isIndexed())
        return {&atlas_, sourceRect(g, fg.index)};
    const quint64 key = (quint64(g) << 24) | (quint64(fg.rgb.r) << 16) | (quint64(fg.rgb.g) << 8) | fg.rgb.b;
    auto it = rgb_.find(key);
    if (it == rgb_.end())
        it = rgb_.insert(key, drawGlyph(g, toQRgb(fg.rgb)));
    return {&*it, QRect(QPoint(0, 0), cellSize())};
}

AtlasRenderer::AtlasRenderer(const BitmapFont& font) : font_(&font) {}

void AtlasRenderer::setCanvas(const Canvas* canvas, int glyphWidth) {
    auto atlas = std::make_unique<GlyphAtlas>(*font_, glyphWidth);
    const qint64 w = qint64(canvas->width()) * glyphWidth, h = qint64(canvas->height()) * font_->height;
    if (w <= 0 || h <= 0 || w * h > kMaxPixels)
        throw std::length_error("canvas too large to display");
    // A cache of the right size is reused (it is all dirty below), so
    // switching to an equal-sized canvas allocates nothing.
    if (cache_.size() != QSize(int(w), int(h))) {
        QImage cache(int(w), int(h), QImage::Format_RGB32);
        if (cache.isNull())
            throw std::length_error("canvas too large to display");
        cache_ = std::move(cache);
    }
    canvas_ = canvas;
    atlas_ = std::move(atlas);
    dirty_ = QRegion(0, 0, canvas->width(), canvas->height());
}

void AtlasRenderer::setIceColours(bool on) {
    if (ice_ == on)
        return;
    ice_ = on;
    if (canvas_)
        dirty_ = QRegion(0, 0, canvas_->width(), canvas_->height());
}

QSize AtlasRenderer::cellSize() const { return atlas_ ? atlas_->cellSize() : QSize(); }

void AtlasRenderer::markDirty(const QRegion& cells) {
    if (canvas_)
        dirty_ |= cells & QRect(0, 0, canvas_->width(), canvas_->height());
}

QRegion AtlasRenderer::flush() {
    if (!canvas_ || dirty_.isEmpty())
        return {};
    const QSize cs = atlas_->cellSize();
    QPainter p(&cache_);
    for (const QRect& r : dirty_) {
        for (int cy = r.top(); cy <= r.bottom(); ++cy) {
            for (int cx = r.left(); cx <= r.right(); ++cx) {
                const Cell cell = iceCell(canvas_->at(cx, cy), ice_);
                const QRect target(QPoint(cx * cs.width(), cy * cs.height()), cs);
                p.fillRect(target, QColor(toQRgb(resolveColor(cell.bg))));
                const std::uint8_t g = glyphIndex(cell.ch);
                if (atlas_->blank(g))
                    continue;
                const auto [img, src] = atlas_->glyph(g, cell.fg);
                p.drawImage(target.topLeft(), *img, src);
            }
        }
    }
    QRegion done;
    std::swap(done, dirty_);
    return done;
}

void AtlasRenderer::paint(QPainter& painter, const QRect& source, const QPoint& target, int zoom) {
    flush();
    const QRect src = source & cache_.rect();
    if (src.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QRect(target + (src.topLeft() - source.topLeft()) * zoom, src.size() * zoom), cache_, src);
    painter.restore();
}

} // namespace bw
