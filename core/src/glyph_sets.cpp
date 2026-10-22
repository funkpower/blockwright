#include "blockwright/glyph_sets.h"

#include "blockwright/cp437.h"

#include <cstdint>

namespace bw {

namespace {

// CP437 codes, from Moebius's app/prefs.js (F11 and F12, always blank
// there, are left out).
constexpr std::uint8_t kSets[kGlyphSetCount][kGlyphSetKeys] = {
    {218, 191, 192, 217, 196, 179, 195, 180, 193, 194},  // ┌┐└┘─│├┤┴┬
    {201, 187, 200, 188, 205, 186, 204, 185, 202, 203},  // ╔╗╚╝═║╠╣╩╦
    {213, 184, 212, 190, 205, 179, 198, 181, 207, 209},  // ╒╕╘╛═│╞╡╧╤
    {214, 183, 211, 189, 196, 186, 199, 182, 208, 210},  // ╓╖╙╜─║╟╢╨╥
    {197, 206, 216, 215, 232, 232, 155, 156, 153, 239},  // ┼╬╪╫ΦΦ¢£Ö∩
    {176, 177, 178, 219, 223, 220, 221, 222, 254, 250},  // ░▒▓█▀▄▌▐■·
    {1, 2, 3, 4, 5, 6, 240, 14, 15, 32},                 // ☺☻♥♦♣♠≡♫☼ and a space
    {24, 25, 30, 31, 16, 17, 18, 29, 20, 21},            // ↑↓▲▼►◄↕↔¶§
    {174, 175, 242, 243, 169, 170, 253, 246, 171, 172},  // «»≥≤⌐¬²÷½¼
    {227, 241, 244, 245, 234, 157, 228, 248, 251, 252},  // π±⌠⌡Ω¥Σ°√ⁿ
    {224, 225, 226, 229, 230, 231, 235, 236, 237, 238},  // αßΓσµτδ∞φε
    {128, 135, 165, 164, 152, 159, 247, 249, 173, 168},  // ÇçÑñÿƒ≈∙¡¿
    {131, 132, 133, 160, 166, 134, 142, 143, 145, 146},  // âäàáªåÄÅæÆ
    {136, 137, 138, 130, 144, 140, 139, 141, 161, 158},  // êëèéÉîïìí₧
    {147, 148, 149, 162, 167, 150, 129, 151, 163, 154},  // ôöòóºûüùúÜ
    {47, 92, 40, 41, 123, 125, 91, 93, 96, 39},          // /\(){}[]`'
};

std::array<GlyphSet, kGlyphSetCount> build() {
    std::array<GlyphSet, kGlyphSetCount> out{};
    for (int s = 0; s < kGlyphSetCount; ++s)
        for (int k = 0; k < kGlyphSetKeys; ++k)
            out[s][k] = cp437ToUnicode(kSets[s][k]);
    return out;
}

} // namespace

const GlyphSet& glyphSet(int set) {
    static const std::array<GlyphSet, kGlyphSetCount> sets = build();
    return sets[static_cast<std::size_t>(((set % kGlyphSetCount) + kGlyphSetCount) % kGlyphSetCount)];
}

} // namespace bw
