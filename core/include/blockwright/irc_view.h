#pragma once
// The IRC view (PLAN §3, §8): a canvas as an IRC client shows it once it is
// sent as IRC text. Each row goes through the row encoder (encodeCanvas),
// is read back as a client reads mIRC codes, and is cut at the byte budget,
// so what the view shows is what the lines carry: the mIRC palette, the
// trailing black cells and blank edge rows IRC drops, and the cells a line
// over the budget loses.
#include "blockwright/irc.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bw::irc {

// The colours a client shows where a line sets none (mIRC's default, 99):
// a dark grey background, so what IRC drops stands apart from the art's own
// black, and a light grey foreground.
inline constexpr std::uint32_t kClientBackground = 0x262626;
inline constexpr std::uint32_t kClientForeground = 0xd2d2d2;
// mIRC's number for the client's own colour.
inline constexpr std::uint8_t kClientColour = 99;

// One glyph of a line as a client shows it: its colours (0-98, or
// kClientColour), and the byte offset just past it in the line.
struct ShownCell {
    MircCell cell;
    std::size_t end = 0;
};

// A line of IRC text read as a client reads it: ^C sets colours (up to two
// digits each, 99 the client's own; ^C alone resets them), ^O resets every
// attribute, ^V swaps foreground and background, and ^B ^] ^_ ^^ ^Q are
// read and leave the colours alone. Other bytes are UTF-8 glyphs, one cell
// each; a byte that isn't valid UTF-8 is one U+FFFD cell.
std::vector<ShownCell> decodeLine(std::string_view line);

// One canvas row in the view.
struct ViewRow {
    std::size_t bytes = 0;  // the encoded line's size
    int kept = 0;           // cells the line carries (after the trailing black cells are dropped)
    int shown = 0;          // of those, the cells within the budget
    bool trimmed = false;   // a blank row above or below the art, which an export drops
    bool cut() const { return shown < kept; }
};

struct View {
    // The canvas as shown: every cell in 24-bit colour, the client's
    // background (blank) where IRC shows nothing.
    Canvas canvas;
    std::vector<ViewRow> rows;
    int cutRows() const;
};

// The view of `canvas` sent as IRC text with `opts`, each line cut to fit
// `budget` bytes as the sender cuts it (ascii.fit): a line that fits is sent
// whole; a longer one keeps the cells whose glyph ends within budget - 1
// bytes, the last byte for the reset after them, and loses the rest. The
// view is the canvas's size, the lost cells blank in the client's background.
View ircView(const Canvas& canvas, std::size_t budget = kEfnetWeechatBudget, const EncodeOptions& opts = {});

} // namespace bw::irc
