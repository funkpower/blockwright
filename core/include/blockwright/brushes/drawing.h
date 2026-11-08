#pragma once
// Drawing brushes (PLAN §4 tools): the cell brushes of the phase 2 drawing
// tools. Each paints a cell once per stroke, like every brush on Stroke's
// default dab.

#include "blockwright/brush.h"

namespace bw {

// The shade level of a cell's glyph: 0 for a space (NUL, 0xFF), 1-3 for
// ░ ▒ ▓, 4 for █; empty for any other glyph, a cluster and the continuation included.
std::optional<int> shadeLevel(Glyph g);

// Shade brush: builds a cell up one step of space → ░ → ▒ → ▓ → █ in the
// pen's colours, or with `reduce` takes it one step back down to a space.
// Any other glyph is left alone either way: text, and the half blocks ▀ ▄
// whose two colours are pixels (halfblock.h) a shade can't keep.
class ShadeBrush : public Brush {
public:
    ShadeBrush(Pen pen, bool reduce) : pen_(pen), reduce_(reduce) {}

    std::string_view name() const override { return "shade"; }
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

private:
    Pen pen_;
    bool reduce_;
};

// The glyph a dab's image writes for `ch`: mirrored as the image is
// flipped, and turned a quarter clockwise for each of its turns
// (rotateGlyph), so a radial image a quarter round writes ▀ as ▐.
char32_t dabGlyph(char32_t ch, const Dab& d);

// Whether a Pixels layer holds `ch`: only █ ▀ ▄ and the blanks, the glyphs
// halfBlocks reads as two pixels.
bool pixelsHoldGlyph(char32_t ch);

// Whether a Pixels layer can't hold `ch` but holds it turned a quarter
// (rotateGlyph, either way): ▌ and ▐, which turn to ▀ and ▄. A Library
// stamp holding them may float on a Pixels layer, to be turned before it
// lands (PLAN §12).
bool pixelsHoldTurned(char32_t ch);

// The glyphs a Pixels layer can't hold that the glyph brush writes for `ch`
// under `symmetry` on a width × height canvas (dabGlyph of each of
// symmetryDabs's images), each once in image order: `ch` itself alone when
// the layer can't hold it, else the turned glyphs a radial quarter turn
// writes (▀ is ▐ and ▌), as a flip keeps █ ▀ ▄ and blanks. Empty when the
// layer holds every image.
std::vector<char32_t> pixelsUnheldGlyphs(char32_t ch, const Symmetry& symmetry, int width, int height);

// Glyph brush: writes one cell (a glyph in the pen's colours; a blank cell
// erases), its glyph as each image writes it (dabGlyph).
class GlyphBrush : public Brush {
public:
    explicit GlyphBrush(Cell cell) : cell_(cell) {}

    std::string_view name() const override { return "glyph"; }
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

private:
    Cell cell_;
};

// Colorize brush: recolours the cells it passes, keeping their glyphs. An
// empty colour is left as it is.
class ColorizeBrush : public Brush {
public:
    ColorizeBrush(std::optional<Color> fg, std::optional<Color> bg) : fg_(fg), bg_(bg) {}

    std::string_view name() const override { return "colorize"; }
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

private:
    std::optional<Color> fg_, bg_;
};

} // namespace bw
