#pragma once
// The 16-colour CGA palette art is drawn in, and the ANSI colour numbering.

#include <array>
#include <cstdint>

namespace bw {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

// CGA order: black, blue, green, cyan, red, magenta, brown, grey, then the
// bright eight. Indices 0-15 everywhere in the core mean this order.
inline constexpr std::array<Rgb, 16> kPalette = {{
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xaa}, {0x00, 0xaa, 0x00}, {0x00, 0xaa, 0xaa},
    {0xaa, 0x00, 0x00}, {0xaa, 0x00, 0xaa}, {0xaa, 0x55, 0x00}, {0xaa, 0xaa, 0xaa},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xff}, {0x55, 0xff, 0x55}, {0x55, 0xff, 0xff},
    {0xff, 0x55, 0x55}, {0xff, 0x55, 0xff}, {0xff, 0xff, 0x55}, {0xff, 0xff, 0xff},
}};

enum PaletteIndex : std::uint8_t {
    Black, Blue, Green, Cyan, Red, Magenta, Brown, Grey,
    DarkGrey, LightBlue, LightGreen, LightCyan, LightRed, LightMagenta, Yellow, White,
};

// CGA is BGR order, ANSI (SGR 30-37) is RGB order. The mapping is its own
// inverse, so one table converts both ways.
inline constexpr std::array<std::uint8_t, 8> kCgaAnsiSwap = {0, 4, 2, 6, 1, 5, 3, 7};
constexpr std::uint8_t cgaToAnsi(std::uint8_t cga) { return kCgaAnsiSwap[cga & 7]; }
constexpr std::uint8_t ansiToCga(std::uint8_t ansi) { return kCgaAnsiSwap[ansi & 7]; }

// The palette entry nearest to an RGB colour (squared distance; ties go to
// the lower index).
std::uint8_t nearestPaletteIndex(Rgb c);

// An xterm-256 colour number as RGB (0-15 are the palette's own colours, in
// ANSI order with bright = n >= 8).
Rgb xtermRgb(int n);

} // namespace bw
