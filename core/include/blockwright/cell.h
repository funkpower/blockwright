#pragma once
// One character cell: a Unicode codepoint (CP437 mapping happens only at
// the file edges), a foreground and background colour, attribute bits, a
// transparent flag for layers and the editor's lost-blink mark.

#include "blockwright/palette.h"

#include <cstdint>

namespace bw {

// A colour as a palette index or as 24-bit RGB.
struct Color {
    enum class Kind : std::uint8_t { Index, Rgb };
    Kind kind = Kind::Index;
    std::uint8_t index = 0;
    Rgb rgb{};

    static constexpr Color indexed(std::uint8_t i) { return Color{Kind::Index, static_cast<std::uint8_t>(i & 15), {}}; }
    static constexpr Color fromRgb(Rgb c) { return Color{Kind::Rgb, 0, c}; }

    bool isIndexed() const { return kind == Kind::Index; }
    // The palette index, quantising an RGB colour to the nearest entry.
    std::uint8_t paletteIndex() const { return isIndexed() ? index : nearestPaletteIndex(rgb); }

    bool operator==(const Color& o) const {
        return kind == o.kind && (isIndexed() ? index == o.index : rgb == o.rgb);
    }
};

namespace attr {
// Blinking text, kept apart from the background colour when iCE colours are
// off. With iCE on, blink means a bright background and the parser folds it
// into the background index instead.
inline constexpr std::uint8_t Blink = 1u << 0;
} // namespace attr

struct Cell {
    char32_t ch = U' ';
    Color fg = Color::indexed(Grey);
    Color bg = Color::indexed(Black);
    std::uint8_t attrs = 0;
    bool transparent = false;
    // Editor only: this cell lost its blink when its file was opened
    // (openAsEditor). Moved and copied cells keep it, so the mark goes where
    // the cell goes (moves, flips, copies, undo and redo); a drawn cell has
    // none of its own (settleLostBlink in edit.h); the save counts the marked cells it still
    // writes without a blink (Document::encode). No file, export or renderer
    // reads it, and == leaves it out, so cells that look the same are equal
    // to lint, previews and tests. identical() sees it.
    bool lostBlink = false;

    bool operator==(const Cell& o) const {
        return ch == o.ch && fg == o.fg && bg == o.bg && attrs == o.attrs && transparent == o.transparent;
    }
};

// Whether `a` and `b` are the same cell, the lost-blink mark included: how an
// edit tells whether it changes a cell, so moving a marked cell onto one that
// looks the same still moves the mark.
inline bool identical(const Cell& a, const Cell& b) { return a == b && a.lostBlink == b.lostBlink; }

} // namespace bw
