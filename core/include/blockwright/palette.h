#pragma once
// The 16-colour CGA palette art is drawn in, and the ANSI colour numbering.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

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

// A document's 16 colours, in CGA order: index n of a cell means entry n
// (PLAN §8, Palettes). kPalette is the standard one.
using Palette = std::array<Rgb, 16>;

// CGA is BGR order, ANSI (SGR 30-37) is RGB order. The mapping is its own
// inverse, so one table converts both ways.
inline constexpr std::array<std::uint8_t, 8> kCgaAnsiSwap = {0, 4, 2, 6, 1, 5, 3, 7};
constexpr std::uint8_t cgaToAnsi(std::uint8_t cga) { return kCgaAnsiSwap[cga & 7]; }
constexpr std::uint8_t ansiToCga(std::uint8_t ansi) { return kCgaAnsiSwap[ansi & 7]; }

// The palette entry nearest to an RGB colour (squared distance; ties go to
// the lower index).
std::uint8_t nearestPaletteIndex(Rgb c);

// The entry of `palette` nearest to an RGB colour, by the same rule.
std::uint8_t nearestIndex(Rgb c, const Palette& palette);

// A colour as an XBIN holds it: each channel the nearest 6-bit value
// (round(v * 63 / 255)), widened back as the reader widens it
// (v << 2 | v >> 4). An XBIN writes these exactly; Edit Palette rounds
// what it is given to them.
Rgb xbinColour(Rgb c);
// How many of `palette`'s entries an XBIN can't write exactly (each one not
// its own xbinColour): writeXbin writes a channel's top 6 bits, so these
// read back as other colours.
int xbinRoundedEntries(const Palette& palette);

// A named palette the editor and the CLI offer (Colours > Palette, the
// CLI's --palette): its name ("cga"), a title for the menu and its colours
// in CGA order.
struct PalettePreset {
    std::string_view name;
    std::string_view title;
    Palette colours;
};

// The presets, in menu order, the standard one ("cga", kPalette) first.
std::span<const PalettePreset> palettePresets();
// The preset `name` names, in any case; "standard", "vga" and "ega" are
// "cga" (VGA and EGA text mode start with the CGA colours). Nothing for any
// other name.
const PalettePreset* palettePreset(std::string_view name);

// An xterm-256 colour number as RGB (0-15 are the palette's own colours, in
// ANSI order with bright = n >= 8).
Rgb xtermRgb(int n);

} // namespace bw
