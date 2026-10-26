#include "blockwright/pc_fonts.h"

#include <array>

namespace bw {

namespace {

constexpr std::array<std::uint8_t, 256 * 8> kBiosBits = {
#include "font_ibm_bios_8x8.inc"
};

constexpr std::array<std::uint8_t, 256 * 16> kVgaBits = {
#include "font_ibm_vga_8x16.inc"
};

} // namespace

std::span<const std::uint8_t, 256 * 8> ibmBios8x8() { return kBiosBits; }
std::span<const std::uint8_t, 256 * 16> ibmVga8x16() { return kVgaBits; }

} // namespace bw
