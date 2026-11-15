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
#include <string>
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

struct DecodeOptions {
    // The client's columns, where a wide cluster on the last one wraps
    // first; 0 for a row that doesn't wrap.
    int columns = 0;
    // The cluster pool's cap on this path (ClusterPool::kLossyCap), which
    // tests set low.
    std::size_t poolCap = ClusterPool::kLossyCap;
};

// What decodeLine couldn't keep of a line's clusters.
struct DecodeLoss {
    int marksDropped = 0;   // zero-width code points past kMaxClusterCodePoints
    int firstDropped = -1;  // the cell the first of them was to join
    int poolRefused = 0;    // clusters kept as their base: the pool was full
    int firstRefused = -1;  // the cell of the first of them
};

// A line of IRC text read as a client reads it: ^C sets colours (up to two
// digits each, 99 the client's own; ^C alone resets them), ^O resets every
// attribute, ^V swaps foreground and background, ^B ^] ^_ toggle bold,
// italic and underline (MircCell::attrs), and ^^ ^Q are read and leave the
// cells alone. Other bytes are UTF-8, laid out by the
// width table as a terminal lays them out (PLAN §16, Reading): a wide code
// point is a pair (a lead and a continuation in the same colours), wrapping
// first when it would start on the last of `opts.columns` (the column left
// is a blank in the client's colours) and U+FFFD on a one-column row; a
// zero-width code point joins the cluster before it unless it starts the
// line or follows a control (NUL and the rest of C0, DEL, C1, U+2028,
// U+2029), and takes a cell of
// its own then. A cluster keeps 16 code points and drops the rest, and one
// the pool can't take keeps its base; both are counted in `loss`. A byte
// that isn't valid UTF-8 is one U+FFFD cell. Each cell's `end` is the byte
// offset after its whole cluster, the same for a lead and its continuation.
std::vector<ShownCell> decodeLine(std::string_view line, const DecodeOptions& opts = {},
                                  DecodeLoss* loss = nullptr);

// One canvas row in the view.
struct ViewRow {
    std::size_t bytes = 0;  // the encoded line's size, as written
    int kept = 0;           // cells the line carries (after the trailing black cells are dropped)
    int shown = 0;          // of those, the cells within the budget
    bool trimmed = false;   // a blank row above or below the art, which an export drops
    // The sender drops cells of the line: some it carries are lost, whether
    // or not they lie within the view's width (a row of lone wide cells lays
    // out wider than the canvas, and can be cut past it).
    bool lost = false;
    bool cut() const { return lost || shown < kept; }
};

struct View {
    // The canvas as shown: every cell in 24-bit colour, the client's
    // background (blank) where IRC shows nothing.
    Canvas canvas;
    std::vector<ViewRow> rows;
    int cutRows() const;
};

// The view of `canvas` sent as IRC text with `opts`, each line cut to fit
// `budget` bytes as the ascii.py sender script 1.32.2 or later cuts a .txt
// line: loading it, the script encodes it again (compact: encode_irc, with
// two-digit colour codes, so it is measured as txtSentBytes measures it, not
// as written); a line that then fits is sent whole; a longer one the sender
// encodes again in its own way (two-digit colour codes, a space or a full
// block for a solid one-code-point cell, whichever is shorter, a cluster's
// glyph never traded and no code inside it), sends that whole if it fits,
// and otherwise keeps the clusters whose glyph ends within budget - 1 bytes
// of it, the last byte for the reset after them, and loses the rest. Where
// the script's Python's Unicode decides which code points join a cluster,
// the row keeps the fewest cells any of them keeps. The view is the
// canvas's size, the cells shown as the export draws them, the lost ones
// blank in the client's background.
View ircView(const Canvas& canvas, std::size_t budget = kEfnetWeechatBudget, const EncodeOptions& opts = {});

// A line of a .txt as the ascii script sends it, which is not the line as
// written: the script reads each .txt line into cells (parse_irc, which also
// takes formatting, ^D colours, tabs and other control bytes) and writes
// them again (compact, which is encode_irc: two-digit colour codes, so a
// line the encoder wrote with one-digit ones grows, and each change of
// formatting written again), and fit() then sends that whole if it fits
// `budget`, or else the shortest encoding of the cells (encode_irc_min, not
// tried on a line with formatting) whole if that does, each with the reset
// fit() puts after a trailing space. This is the size of the one it sends
// whole, or of the shorter of the two when neither fits, which is a line the
// script cuts. Which code points join the one before them depends on the
// Unicode of the script's Python, as that version's own categories have it
// (joinMask), so the line is sized under each Unicode joinUnicode lists, and
// a later one, once for each way they cluster it, and this is the
// largest. The row meter, fit, the IRC view, Export IRC Text and
// `export --irc` hold a line to it (#364).
std::size_t txtSentBytes(std::string_view line, std::size_t budget = kEfnetWeechatBudget);

// A .txt line the ascii script cuts: its txtSentBytes, and, when only some
// Unicode versions of the script's Python cut it, which ones (a clause
// starting "where the script's Python has"); empty when all of them do.
struct TxtLineSize {
    int line;  // from 1
    std::size_t bytes;
    std::string where;
};
// The lines of a .txt the ascii script cuts at `budget`.
std::vector<TxtLineSize> txtOverBudget(const std::vector<std::string>& lines, std::size_t budget = kEfnetWeechatBudget);

} // namespace bw::irc
