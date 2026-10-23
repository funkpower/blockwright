#pragma once
// mIRC's 99 colours and the row encoder: cells in mIRC colours to the
// shortest IRC line that draws them. irc.h maps canvas cells onto it.

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace bw::irc {

class IrcError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// mIRC colours 0-98 as 0xRRGGBB (https://modern.ircdocs.horse/formatting#colors).
inline constexpr std::array<std::uint32_t, 99> kMircRgb = {
    0xffffff, 0x000000, 0x00007f, 0x009300, 0xff0000, 0x7f0000, 0x9c009c, 0xfc7f00,
    0xffff00, 0x00fc00, 0x009393, 0x00ffff, 0x0000fc, 0xff00ff, 0x7f7f7f, 0xd2d2d2,
    0x470000, 0x472100, 0x474700, 0x324700, 0x004700, 0x00472c, 0x004747, 0x002747,
    0x000047, 0x2e0047, 0x470047, 0x47002a, 0x740000, 0x743a00, 0x747400, 0x517400,
    0x007400, 0x007449, 0x007474, 0x004074, 0x000074, 0x4b0074, 0x740074, 0x740045,
    0xb50000, 0xb56300, 0xb5b500, 0x7db500, 0x00b500, 0x00b571, 0x00b5b5, 0x0063b5,
    0x0000b5, 0x7500b5, 0xb500b5, 0xb5006b, 0xff0000, 0xff8c00, 0xffff00, 0xb2ff00,
    0x00ff00, 0x00ffa0, 0x00ffff, 0x008cff, 0x0000ff, 0xa500ff, 0xff00ff, 0xff0098,
    0xff5959, 0xffb459, 0xffff71, 0xcfff60, 0x6fff6f, 0x65ffc9, 0x6dffff, 0x59b4ff,
    0x5959ff, 0xc459ff, 0xff66ff, 0xff59bc, 0xff9c9c, 0xffd39c, 0xffff9c, 0xe2ff9c,
    0x9cff9c, 0x9cffdb, 0x9cffff, 0x9cd3ff, 0x9c9cff, 0xdc9cff, 0xff9cff, 0xff94d3,
    0x000000, 0x131313, 0x282828, 0x363636, 0x4d4d4d, 0x656565, 0x818181, 0x9f9f9f,
    0xbcbcbc, 0xe2e2e2, 0xffffff,
};

// The mIRC colour nearest to 0xRRGGBB (squared distance; ties go to the
// lower number). Bits above the 24 are ignored.
std::uint8_t nearestMirc(std::uint32_t rgb);

// One cell in mIRC colours 0-98.
struct MircCell {
    char32_t ch = U' ';
    std::uint8_t fg = 0;
    std::uint8_t bg = 1;
};

// The shortest IRC line that draws `cells`, without a line ending. With
// `swapShades` the encoder may also draw ░ as ▓ with the colours reversed,
// and ▒ as itself reversed; the patterns differ a little, so it is opt-in.
// Throws IrcError for a code point that is not a Unicode scalar value or a
// colour over 98.
std::string encodeMircRow(std::span<const MircCell> cells, bool swapShades = false);

namespace detail {

using Cost = std::uint64_t;

// Whether an entry `cost` bytes long can still beat the best, `best` bytes
// (cost >= best): without overflow at any cost.
bool withinSlack(Cost cost, Cost best);

// encodeMircRow with the search's byte count starting at `start` rather than
// 0, so tests can reach the top of the cost range with a short row.
std::string encodeMircRowFrom(std::span<const MircCell> cells, bool swapShades, Cost start);

} // namespace detail

} // namespace bw::irc
