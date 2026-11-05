#pragma once
// Interactive canvas drawing for the GUI (PLAN §4). A renderer keeps a cache
// of the canvas at 1x and redraws only the cells marked dirty; views draw
// from it at any integer zoom. The interface lets a shader renderer replace
// the QPainter one if large canvases get slow.

#include "blockwright/canvas.h"
#include "blockwright/render.h"

#include <QHash>
#include <QImage>
#include <QRect>
#include <QRegion>

#include <array>
#include <memory>
#include <utility>

class QPainter;

namespace bw {

class CanvasRenderer {
public:
    virtual ~CanvasRenderer() = default;

    // Starts over on `canvas`, which is not owned and must outlive the
    // renderer or the next setCanvas call. Every cell is dirty afterwards.
    // `glyphWidth` is 8 or 9 (see bw::glyphWidth). Throws
    // std::invalid_argument for a bad width and std::length_error if the
    // canvas is too large to cache, and then changes nothing.
    virtual void setCanvas(const Canvas* canvas, int glyphWidth) = 0;

    // How bright backgrounds and blink show (bw::iceCell): with iCE colours
    // on (the default) as 16 background colours, with them off as blink on
    // the dark colour in its visible phase, as renderCanvas draws blink.
    // A change marks every cell dirty.
    virtual void setIceColours(bool on) = 0;
    virtual bool iceColours() const = 0;

    // One cell in pixels at 1x (glyph width by font height).
    virtual QSize cellSize() const = 0;
    // The whole canvas in pixels at 1x.
    virtual QSize imageSize() const = 0;

    // Marks cells (in cell coordinates) as needing a redraw; the parts
    // outside the canvas are ignored.
    virtual void markDirty(const QRegion& cells) = 0;
    // The cells waiting for a redraw.
    virtual QRegion dirtyCells() const = 0;
    // Redraws the dirty cells into the cache and returns them.
    virtual QRegion flush() = 0;

    // Draws the 1x canvas pixels `source` scaled by `zoom` (integer, no
    // smoothing) with their top-left at `target`. Flushes first.
    virtual void paint(QPainter& painter, const QRect& source, const QPoint& target, int zoom) = 0;
};

// Glyph images for one font and cell width, drawn in each palette colour on
// a transparent background, so a cell is a background fill plus one
// drawImage from a source rect. 24-bit colours get their own glyph images,
// made on first use. In 9 px mode the 9th column repeats the 8th for the
// line-drawing glyphs 0xC0-0xDF, as in renderCanvas. A cell drawn from rows
// (bw::cellImage: Unifont, a cluster, either half of a pair) gets an image
// per (rows, colour) on first use.
class GlyphAtlas {
public:
    GlyphAtlas(const BitmapFont& font, int glyphWidth);

    QSize cellSize() const { return {gw_, font_->height}; }
    const QImage& image() const { return atlas_; }
    // Where glyph `glyph` in palette colour `index` sits in image().
    QRect sourceRect(std::uint8_t glyph, std::uint8_t index) const;
    // A glyph in any colour: the atlas and a source rect in it.
    std::pair<const QImage*, QRect> glyph(std::uint8_t glyph, const Color& fg);
    // Whether the glyph has no foreground pixels (a space, say).
    bool blank(std::uint8_t glyph) const { return blank_[glyph]; }
    // The image of `rows` (bw::cellImage) in `fg`.
    const QImage& rows(const GlyphRows& rows, const Color& fg);

private:
    QImage drawGlyph(std::uint8_t glyph, QRgb color) const;

    const BitmapFont* font_;
    int gw_;
    QImage atlas_;  // 256 glyphs across, 16 colours down
    std::array<bool, 256> blank_{};
    QHash<quint64, QImage> rgb_;  // (glyph, rgb) -> glyph image
    QHash<std::pair<const GlyphRows*, QRgb>, QImage> rows_;  // (rows, rgb) -> image
};

// The QPainter renderer: a 1x RGB32 cache filled from a GlyphAtlas. It draws
// each cell as renderCanvas draws iceCell(cell, iceColours()).
class AtlasRenderer final : public CanvasRenderer {
public:
    explicit AtlasRenderer(const BitmapFont& font = vgaFont());

    void setCanvas(const Canvas* canvas, int glyphWidth) override;
    void setIceColours(bool on) override;
    bool iceColours() const override { return ice_; }
    QSize cellSize() const override;
    QSize imageSize() const override { return cache_.size(); }
    void markDirty(const QRegion& cells) override;
    QRegion dirtyCells() const override { return dirty_; }
    QRegion flush() override;
    void paint(QPainter& painter, const QRect& source, const QPoint& target, int zoom) override;

    // The 1x cache, as of the last flush.
    const QImage& cache() const { return cache_; }

private:
    const BitmapFont* font_;
    const Canvas* canvas_ = nullptr;
    std::unique_ptr<GlyphAtlas> atlas_;
    QImage cache_;
    QRegion dirty_;
    bool ice_ = true;
};

} // namespace bw
