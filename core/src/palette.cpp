#include "blockwright/palette.h"

namespace bw {

std::uint8_t nearestPaletteIndex(Rgb c) {
    std::uint8_t best = 0;
    int bestD = -1;
    for (std::uint8_t i = 0; i < kPalette.size(); ++i) {
        const int dr = int(kPalette[i].r) - c.r, dg = int(kPalette[i].g) - c.g, db = int(kPalette[i].b) - c.b;
        const int d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

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
