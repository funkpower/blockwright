#pragma once
// IRC text (mIRC colour codes): canvas cells through the row encoder in
// mirc.h. It is the only source of IRC byte counts (PLAN §3).

#include "blockwright/ansi.h"
#include "blockwright/mirc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bw::irc {

// Bytes a line may take on EFnet as WeeChat sends it (PLAN §3).
inline constexpr std::size_t kEfnetWeechatBudget = 407;

// A named byte budget (project::IrcBudget keeps the profile and the bytes).
struct BudgetPreset {
    std::string_view profile;  // what a project stores
    std::string_view name;     // what the editor shows
    std::size_t bytes;
};
inline constexpr std::array<BudgetPreset, 1> kBudgetPresets = {{
    {"efnet-weechat", "EFnet / WeeChat", kEfnetWeechatBudget},
}};
// The profile of a budget typed in or worked out with budgetFor.
inline constexpr std::string_view kCustomProfile = "custom";

// The byte budgets a line can have: at least one byte, and no more than an
// IRC line's 512 (CR LF included), which nothing over can be sent in. The
// project format, the Budget dialog and the CLI's --budget all take this
// range (validBudget).
inline constexpr std::size_t kMinBudget = 1;
inline constexpr std::size_t kMaxBudget = 512;
constexpr bool validBudget(std::size_t bytes) { return bytes >= kMinBudget && bytes <= kMaxBudget; }

// The bytes of text one PRIVMSG to `target` can carry when the server relays
// it to the channel as ":<mask> PRIVMSG <target> :<text>\r\n" within IRC's
// 512 bytes: 498 minus the mask and the target. `mask` is the sender's
// nick!user@host as the server shows it. Nothing if either is empty, holds a
// space, CR, LF or NUL, or leaves no room for text.
std::optional<std::size_t> budgetFor(std::string_view mask, std::string_view target);

struct EncodeOptions {
    // Let the encoder swap ░ for ▓ with the colours reversed (encodeMircRow).
    bool swapShades = false;
    // Keep each row's trailing black cells and end the row on a black-on-black
    // █, so IRC keeps them, and the blank rows above and below the art with
    // them (PLAN §3). Off, they are dropped.
    bool pad = false;
    // Fit lines over this many bytes by merging their least visible colours
    // into their neighbours' (fitCells); 0 leaves every cell as it is.
    std::size_t fit = 0;
};

// The mIRC colour a palette or RGB colour is drawn as: a palette colour as
// the mIRC colour of the same name and shade, RGB as the nearest of the 99.
std::uint8_t mircColor(const Color& c);

// The shortest IRC line that draws `cells`, without a line ending. A
// transparent cell is drawn blank, a C0 control or DEL as its CP437 glyph,
// and a C1 control (U+0080-U+009F) or a non-scalar code point as '?'.
std::string encodeRow(std::span<const Cell> cells, const EncodeOptions& opts = {});

// The cells as the row encoder takes them: drawn as encodeRow draws them,
// in mIRC colours.
std::vector<MircCell> mircRow(std::span<const Cell> cells);

// A cell of the editor's canvas as it is sent: as it shows, and is saved,
// with iCE colours `ice` (iceCell). With iCE off a bright background goes as
// blink on its dark colour, which IRC shows as the dark colour. A
// transparent cell is left as it is; the encoder draws it blank. The row
// meter, File > Export IRC Text and the IRC View dock all send these.
Cell sentCell(const Cell& c, bool ice);
// The canvas with every cell as sentCell has it.
Canvas sentCanvas(const Canvas& canvas, bool ice);

// A canvas row as one IRC line, and how many of its cells fit changed.
struct Line {
    std::string text;
    int merged = 0;
};

// Row `y` of the canvas as encodeCanvas writes it. The editor's byte meter
// counts this, so it agrees with the export.
Line encodeLine(const Canvas& canvas, int y, const EncodeOptions& opts = {});
// encodeLine of a row already in mIRC colours (mircRow).
Line encodeMircLine(std::span<const MircCell> cells, const EncodeOptions& opts = {});

// One line per canvas row: trailing black spaces are dropped from each row
// first (unless opts.pad), since IRC shows them on its own background.
std::vector<std::string> encodeCanvas(const Canvas& canvas, const EncodeOptions& opts = {});

// The canvas as an IRC .txt: encodeCanvas with the blank rows above and below
// the art dropped.
std::vector<std::string> exportCanvas(const Canvas& canvas, const EncodeOptions& opts = {});
// exportCanvas's lines with what fit changed in each.
std::vector<Line> exportLines(const Canvas& canvas, const EncodeOptions& opts = {});
// Drops the blank lines above and below the art, as exportLines does.
void trimBlankLines(std::vector<Line>& lines);

// The cells, one row in mIRC colours, changed as little as can be seen so
// that the row encodes in `budget` bytes with `opts` (its trimming, padding
// and shade swaps; opts.fit is ignored). Each step settles one cell into a
// colour it or its left neighbour already shows: a solid cell takes its
// neighbour's colour, a half block or shade one of its own two colours or
// one of the neighbour's, the cheapest first by squared RGB distance times
// the area that changes, until the row fits. A cell can settle up to three
// times (a half block into a solid, the solid into its neighbour's colour),
// its costs adding up, though never in the same round as a neighbour, and
// never so that a changed cell beside it loses the colour it took. Then
// every change the row still fits without is put back, the most visible
// first, with the changes right of it that took its colour. So every cell
// fit changes ends up showing only colours it showed before or its left
// neighbour shows in the result. A glyph other than a blank, a block, a half block or a shade is
// never changed. Rows that fit already, or that can't be made to fit, come
// back unchanged.
std::vector<MircCell> fitCells(std::span<const MircCell> cells, std::size_t budget, const EncodeOptions& opts = {});

// The lines as a .txt file: each followed by '\n'.
std::string joinLines(const std::vector<std::string>& lines);

struct LineSize {
    int line;  // 1-based
    std::size_t bytes;
};

// The lines longer than `budget` bytes.
std::vector<LineSize> overBudget(const std::vector<std::string>& lines, std::size_t budget);

} // namespace bw::irc
