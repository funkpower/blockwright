#pragma once
// Colour ramps and the shading ladder.
//
// A ramp is a list of palette colours, dark to light. Between two neighbours
// A (dark) and B (light) the ladder has five rungs: A, B░A, B▒A, B▓A, B.
// Each shade can be written either way round (B▓A shows as A░B), and the
// way that keeps the background dark (0-7) is the one that works without
// iCE colours. A cell's place on the whole ramp is its rung: 4 rungs per
// neighbour pair, so a ramp of n colours has rungs 0 to 4(n-1), and rung 4i
// is the solid colour i. Only palette colours are on a ramp.

#include "blockwright/cell.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bw {

inline constexpr char32_t kShadeLight = U'░';   // ░, 1/4 foreground
inline constexpr char32_t kShadeMedium = U'▒';  // ▒, 1/2
inline constexpr char32_t kShadeDark = U'▓';    // ▓, 3/4
inline constexpr char32_t kFullBlock = U'█';    // █
inline constexpr char32_t kUpperHalf = U'▀';    // ▀
inline constexpr char32_t kLowerHalf = U'▄';    // ▄

inline constexpr int kRungsPerStep = 4;

struct Ramp {
    std::string name;
    std::vector<std::uint8_t> colours;  // palette indices, dark to light

    int topRung() const { return colours.empty() ? 0 : kRungsPerStep * (int(colours.size()) - 1); }
    // The colour's index in the ramp, or -1.
    int indexOf(std::uint8_t colour) const;
    bool operator==(const Ramp&) const = default;
};

// The named ramps: fire, sunset, ice, grass, steel,
// skin, sea, toxic, violet, wood.
const std::vector<Ramp>& namedRamps();
std::optional<Ramp> findRamp(std::string_view name);

// A ramp needs at least two colours, each 0-15 and none repeated (a repeat
// would make a rung ambiguous). Returns why it is unusable, or empty.
std::string rampProblem(const Ramp& ramp);

// The palette colour a cell shows as one flat colour: a space's background,
// a full block's foreground, or a block or shade glyph whose two colours are
// the same. Empty for anything else (text, half blocks, shades of two
// colours, and RGB colours, which aren't on any ramp).
std::optional<std::uint8_t> solidColour(const Cell& cell);

// The cell's rung on the ramp, or empty if the cell isn't on it: a solid
// ramp colour, or a ░▒▓ shade of two ramp neighbours. A shade with the darker
// colour as the foreground reads as the rung it shows (A░B on B is B▓A).
// Blinking cells are off the ladder (the blink would hide the shade).
std::optional<int> rungOf(const Ramp& ramp, const Cell& cell);

// Which colour of a shade's pair is the foreground.
enum class ShadeForm { LightOnTop, DarkOnTop };

// The cell for a rung (clamped to the ramp): a solid colour as it is
// written (a space on a dark background, a full block for a bright
// colour), or a shade glyph. A shade is written the way round that keeps
// the background dark when only one way does (the darker colour bright,
// the lighter not: K▓w, not w░K); otherwise in `form`, the lighter colour
// on top by default. Two bright neighbours give a bright background either
// way, which needs iCE colours.
Cell rungCell(const Ramp& ramp, int rung, ShadeForm form = ShadeForm::LightOnTop);
// The way round a shade cell is written; empty for anything else.
std::optional<ShadeForm> shadeForm(const Ramp& ramp, const Cell& cell);

} // namespace bw
