#include "blockwright/nomadnet_view.h"

#include "blockwright/micron.h"

namespace bw::nomadnet {

namespace {

// urwid's _CUBE_256_LOOKUP_16: the cube step for a channel's high nibble,
// the nearest step to the nibble scaled to 0-255 (int_scale: 17 * n), ties
// to the upper step (_value_lookup_table's midpoints 48 115 155 195 235).
constexpr int kStepOfNibble[16] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5};
// xterm's 256colres.h cube steps (urwid's _CUBE_STEPS_256).
constexpr std::uint8_t kCubeSteps[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};

} // namespace

int index256(Rgb c) {
    // _true_to_256 keeps each channel's high nibble ("#rgb"), and
    // _parse_color_256 makes it 16 + 36 r + 6 g + b in cube steps
    return 16 + 36 * kStepOfNibble[c.r >> 4] + 6 * kStepOfNibble[c.g >> 4] + kStepOfNibble[c.b >> 4];
}

Rgb colour256(Rgb c) {
    const int i = index256(c) - 16;
    return {kCubeSteps[i / 36], kCubeSteps[i / 6 % 6], kCubeSteps[i % 6]};
}

Canvas view256(const Canvas& canvas, const std::array<Rgb, 16>& palette, bool ice) {
    Canvas out = micron::pageCanvas(canvas, palette, ice);
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            Cell& c = out.at(x, y);
            c.fg = Color::fromRgb(colour256(c.fg.rgb));
            c.bg = Color::fromRgb(colour256(c.bg.rgb));
        }
    return out;
}

} // namespace bw::nomadnet
