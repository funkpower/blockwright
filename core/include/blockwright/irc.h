#pragma once
// IRC text (mIRC colour codes): canvas cells through the row encoder in
// mirc.h. It is the only source of IRC byte counts (PLAN §3).

#include "blockwright/ansi.h"
#include "blockwright/mirc.h"
#include "blockwright/target.h"

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
    // Fit lines over this many bytes as the ascii script sends them
    // (txtSentBytes) by merging their least visible colours into their
    // neighbours' (fitCells); 0 leaves every cell as it is.
    std::size_t fit = 0;
};

// The mIRC colour a palette or RGB colour is drawn as: a palette colour as
// the mIRC colour of the same name and shade, RGB as the nearest of the 99.
std::uint8_t mircColor(const Color& c);

// The palette colour (0-15) drawn as mIRC colour `mirc` (0-15): mircColor's
// inverse over the 16. Out of range is taken mod 16.
std::uint8_t paletteOfMirc(std::uint8_t mirc);

// `c` as IRC text in 16 colours sends it (#496): a palette colour as it is,
// an RGB one as the palette colour whose mIRC colour is the nearest of mIRC
// 0-15 to it (nearestMirc16), so mircColor of the result is 0-15.
Color fold16(const Color& c);

// The shortest IRC line that draws `cells`, without a line ending. A
// transparent cell is drawn blank, NUL as a space, a C0 control or DEL as
// its CP437 glyph, and a C1 control (U+0080-U+009F), U+2028, U+2029 or a
// non-scalar code point as '?'. A lead is written as its cluster when a
// UTF-8 .ans would write it whole (utf8WideGlyph), its continuation as
// nothing; any other lead as '?' and its continuation as a space, each in
// its own colours, so the columns stay (PLAN §16, Writing).
std::string encodeRow(std::span<const Cell> cells, const EncodeOptions& opts = {});

// The cells as the row encoder takes them: drawn as encodeRow draws them,
// in mIRC colours, a pair written whole in its lead's colours. Each cell
// not written as its glyph that a UTF-8 .ans would list is added to `lost`,
// if given, at column x and row `y`, as the UTF-8 .ans writer lists it: a
// lead written '?' (Loss::Wide, or Loss::Control when a code point isn't
// writable; `lead` set), or a control written '?' (Loss::Control). A C0
// control or DEL, which IRC draws as its CP437 glyph, isn't listed.
std::vector<MircCell> mircRow(std::span<const Cell> cells, std::vector<Unwritable>* lost = nullptr, int y = 0);

// The cells of `canvas` that IRC text writes as something else, in cell
// order (mircRow's `lost`, row by row).
std::vector<Unwritable> unwritable(const Canvas& canvas);

// A cell of the editor's canvas as it is sent: as it shows, and is saved,
// with iCE colours `ice` (iceCell). With iCE off a bright background goes as
// blink on its dark colour, which IRC shows as the dark colour. A
// transparent cell is left as it is; the encoder draws it blank. Under the
// IRC or Micron target a cell with an RGB background (an mIRC colour past 15, or the
// eyedropper's) is left as it is too: iCE is the .ans's, and folding it
// would send mIRC 80 as CGA green (#350); an indexed cell folds as under
// ANSI, so it sends the same bytes. With `colours` 16 (the document's
// setting as sentIrcColours has it) its foreground and background are then
// folded to 0-15 (fold16); an indexed colour is left as it is, so a cell of
// the 16 sends the same bytes either way. The row meter, File > Export IRC
// Text, the IRC View dock and `cli export --irc` all send these.
Cell sentCell(const Cell& c, bool ice, Target target = Target::Ansi, IrcColours colours = IrcColours::Mirc99);
// The canvas with every cell as sentCell has it.
Canvas sentCanvas(const Canvas& canvas, bool ice, Target target = Target::Ansi,
                  IrcColours colours = IrcColours::Mirc99);

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
// that the row encoded with `opts` (its trimming, padding and shade swaps;
// opts.fit is ignored) is sent in `budget` bytes by the ascii script, which
// writes the line again before it fits it (irc::txtSentBytes, irc_view.h). Each step settles one cell into a
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
// never changed, and neither is either half of a pair. Rows that fit already, or that can't be made to fit, come
// back unchanged.
std::vector<MircCell> fitCells(std::span<const MircCell> cells, std::size_t budget, const EncodeOptions& opts = {});

// The lines as a .txt file: each followed by '\n'.
std::string joinLines(const std::vector<std::string>& lines);

struct LineSize {
    int line;  // 1-based
    std::size_t bytes;
};

// The lines longer than `budget` bytes as written. What the ascii script
// sends of a .txt line can be longer: irc::txtOverBudget measures that.
std::vector<LineSize> overBudget(const std::vector<std::string>& lines, std::size_t budget);

} // namespace bw::irc
