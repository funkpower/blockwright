#pragma once
// IRC text (mIRC colour codes): canvas cells through the row encoder in
// mirc.h. It is the only source of IRC byte counts (PLAN §3).

#include "blockwright/ansi.h"
#include "blockwright/mirc.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bw::irc {

// Bytes a line may take on EFnet as WeeChat sends it (PLAN §3).
inline constexpr std::size_t kEfnetWeechatBudget = 407;

struct EncodeOptions {
    // Let the encoder swap ░ for ▓ with the colours reversed (encodeMircRow).
    bool swapShades = false;
};

// The mIRC colour a palette or RGB colour is drawn as: a palette colour as
// the mIRC colour of the same name and shade, RGB as the nearest of the 99.
std::uint8_t mircColor(const Color& c);

// The shortest IRC line that draws `cells`, without a line ending. A
// transparent cell is drawn blank, a C0 control or DEL as its CP437 glyph,
// and a C1 control (U+0080-U+009F) or a non-scalar code point as '?'.
std::string encodeRow(std::span<const Cell> cells, const EncodeOptions& opts = {});

// One line per canvas row: trailing black spaces are
// dropped from each row first, since IRC shows them on its own background.
std::vector<std::string> encodeCanvas(const Canvas& canvas, const EncodeOptions& opts = {});

// The canvas as an IRC .txt: encodeCanvas with the blank rows above and below
// the art dropped.
std::vector<std::string> exportCanvas(const Canvas& canvas, const EncodeOptions& opts = {});

// The lines as a .txt file: each followed by '\n'.
std::string joinLines(const std::vector<std::string>& lines);

struct LineSize {
    int line;  // 1-based
    std::size_t bytes;
};

// The lines longer than `budget` bytes.
std::vector<LineSize> overBudget(const std::vector<std::string>& lines, std::size_t budget);

} // namespace bw::irc
