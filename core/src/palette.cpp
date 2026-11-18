#include "blockwright/palette.h"

#include "blockwright/mirc.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace bw {

namespace {

constexpr Rgb hex(std::uint32_t v) { return {std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)}; }

// mIRC 0-15 at the index the IRC export maps each entry to (irc.cpp's
// kCgaToMirc): what an IRC client shows an indexed cell as.
constexpr Palette mircPalette() {
    constexpr std::array<std::uint8_t, 16> cgaToMirc = {1, 2, 3, 10, 5, 6, 7, 15, 14, 12, 9, 11, 4, 13, 8, 0};
    Palette p{};
    for (std::size_t i = 0; i < 16; ++i)
        p[i] = hex(irc::kMircRgb[cgaToMirc[i]]);
    return p;
}

// Each in CGA order: black, blue, green, cyan, red, magenta, brown/yellow,
// grey, then the bright eight.
constexpr std::array<PalettePreset, 6> kPresets = {{
    {"cga", "Standard (CGA, EGA, VGA)", kPalette},
    {"cga-yellow", "CGA without the brown fix", [] {
         Palette p = kPalette;
         p[6] = hex(0xaaaa00);
         return p;
     }()},
    {"xterm", "xterm", {{hex(0x000000), hex(0x0000ee), hex(0x00cd00), hex(0x00cdcd), hex(0xcd0000), hex(0xcd00cd),
                          hex(0xcdcd00), hex(0xe5e5e5), hex(0x7f7f7f), hex(0x5c5cff), hex(0x00ff00), hex(0x00ffff),
                          hex(0xff0000), hex(0xff00ff), hex(0xffff00), hex(0xffffff)}}},
    {"windows", "Windows console (legacy)",
     {{hex(0x000000), hex(0x000080), hex(0x008000), hex(0x008080), hex(0x800000), hex(0x800080), hex(0x808000),
       hex(0xc0c0c0), hex(0x808080), hex(0x0000ff), hex(0x00ff00), hex(0x00ffff), hex(0xff0000), hex(0xff00ff),
       hex(0xffff00), hex(0xffffff)}}},
    {"campbell", "Windows Terminal (Campbell)",
     {{hex(0x0c0c0c), hex(0x0037da), hex(0x13a10e), hex(0x3a96dd), hex(0xc50f1f), hex(0x881798), hex(0xc19c00),
       hex(0xcccccc), hex(0x767676), hex(0x3b78ff), hex(0x16c60c), hex(0x61d6d6), hex(0xe74856), hex(0xb4009e),
       hex(0xf9f1a5), hex(0xf2f2f2)}}},
    {"mirc", "mIRC", mircPalette()},
}};

} // namespace

std::span<const PalettePreset> palettePresets() { return kPresets; }

const PalettePreset* palettePreset(std::string_view name) {
    std::string s(name);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (s == "standard" || s == "vga" || s == "ega")
        s = "cga";
    for (const PalettePreset& p : kPresets)
        if (p.name == s)
            return &p;
    return nullptr;
}

std::uint8_t nearestIndex(Rgb c, const Palette& palette) {
    std::uint8_t best = 0;
    int bestD = -1;
    for (std::uint8_t i = 0; i < palette.size(); ++i) {
        const int dr = int(palette[i].r) - c.r, dg = int(palette[i].g) - c.g, db = int(palette[i].b) - c.b;
        const int d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

Rgb xbinColour(Rgb c) {
    const auto round = [](std::uint8_t v) {
        const int six = (int(v) * 63 + 127) / 255;
        return std::uint8_t(six << 2 | six >> 4);
    };
    return {round(c.r), round(c.g), round(c.b)};
}

int xbinRoundedEntries(const Palette& palette) {
    // what writeXbin writes (the top 6 bits), widened as parseXbin reads it
    const auto written = [](std::uint8_t v) { return std::uint8_t((v >> 2) << 2 | (v >> 2) >> 4); };
    return int(std::count_if(palette.begin(), palette.end(), [&](const Rgb& c) {
        return written(c.r) != c.r || written(c.g) != c.g || written(c.b) != c.b;
    }));
}

std::uint8_t nearestPaletteIndex(Rgb c) { return nearestIndex(c, kPalette); }

Rgb xtermRgb(int n) {
    if (n < 0 || n > 255)
        n = 0;
    if (n < 16)
        return kPalette[ansiToCga(static_cast<std::uint8_t>(n & 7)) + (n >= 8 ? 8 : 0)];
    if (n < 232) {
        static constexpr std::uint8_t levels[] = {0, 95, 135, 175, 215, 255};
        n -= 16;
        return {levels[n / 36], levels[(n / 6) % 6], levels[n % 6]};
    }
    const auto g = static_cast<std::uint8_t>(8 + 10 * (n - 232));
    return {g, g, g};
}

} // namespace bw
