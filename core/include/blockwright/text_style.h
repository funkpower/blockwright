#pragma once
// Text styles (PLAN §15): a stack of effects on a text, cells or pixels
// layer, kept in its properties (LayerProperties::style). Only the types and
// their checks live here, so layer.h can hold one; drawing a style is in
// style_draw.h.
//
// Each effect has an on/off switch and its own settings. The stack may list
// them in any order, at most one of each kind; they are drawn in the fixed
// order of EffectKind (Plate, Glow, Shadow, Outline, Fill, Shade, Bevel),
// whatever order the stack lists them in.

#include "blockwright/brush.h"
#include "blockwright/cell.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bw {

// The effects, in the order they are drawn.
enum class EffectKind : std::uint8_t { Plate, Glow, Shadow, Outline, Fill, Shade, Bevel };

// A band of one colour behind the text: the text's box and `padding` cells
// round it.
struct PlateEffect {
    std::uint8_t colour = Blue;
    int padding = 1;
    bool operator==(const PlateEffect&) const = default;
};

// A halo round the letters (grown by the outline when it is on), `width`
// cells wide: the ring next to the letters is the ramp's rung `brightness`,
// and each ring out one rung darker, down to rung 1 (#184). A ring the
// halo's edge can't be reached from without crossing the first, as in a
// counter, holds the first ring's rung.
struct GlowEffect {
    std::string ramp = "fire";
    int width = 3;
    int brightness = 3;
    bool operator==(const GlowEffect&) const = default;
};

// A drop shadow: the letters (grown by the outline when it is on) moved
// `distance` pixels towards `direction`; or a 3D extrusion: every place
// from one pixel to `distance` pixels along it.
enum class ShadowForm : std::uint8_t { Drop, Extrude };
struct ShadowEffect {
    ShadowForm form = ShadowForm::Drop;
    Compass direction = Compass::SE;
    int distance = 1;
    std::uint8_t colour = DarkGrey;
    bool operator==(const ShadowEffect&) const = default;
};

// A ring round the letters: one half-block pixel wide (the eight pixels
// round each ink pixel), or a ring of whole cells (the eight cells round
// each cell with ink).
enum class OutlineForm : std::uint8_t { Pixels, Cells };
struct OutlineEffect {
    OutlineForm form = OutlineForm::Pixels;
    std::uint8_t colour = Blue;
    bool operator==(const OutlineEffect&) const = default;
};

// The letters' colour: flat (`colour`), a ramp gradient down the rows or
// across the columns of each line, or one ramp colour per letter.
enum class FillMode : std::uint8_t { Flat, Rows, Columns, Rainbow };
struct FillEffect {
    FillMode mode = FillMode::Flat;
    std::uint8_t colour = Grey;
    std::string ramp = "fire";  // the ramp of every mode but flat
    bool operator==(const FillEffect&) const = default;
};

// Full ink cells stepped down the ramp's ladder one rung a cell away from
// the side the light comes from, across each letter.
struct ShadeEffect {
    std::string ramp = "fire";
    Compass light = Compass::N;
    bool operator==(const ShadeEffect&) const = default;
};

// The letters' full edge cells facing the light one rung lighter, by the
// rim-light rule.
struct BevelEffect {
    std::string ramp = "fire";
    Compass light = Compass::NW;
    bool operator==(const BevelEffect&) const = default;
};

// The variant's order is EffectKind's.
using EffectSettings =
    std::variant<PlateEffect, GlowEffect, ShadowEffect, OutlineEffect, FillEffect, ShadeEffect, BevelEffect>;

struct Effect {
    bool enabled = true;
    EffectSettings settings;

    EffectKind kind() const { return static_cast<EffectKind>(settings.index()); }
    bool operator==(const Effect&) const = default;
};

struct TextStyle {
    std::vector<Effect> effects;  // any order, one of each kind at most

    static constexpr std::size_t kMaxEffects = 7;
    static constexpr int kMaxPadding = 16;   // plate padding, cells
    static constexpr int kMaxGlow = 16;      // glow width, cells
    static constexpr int kMaxDistance = 16;  // shadow distance or depth, pixels

    // The effect of `kind` when the stack has it switched on; null otherwise.
    template <typename T>
    const T* get() const {
        for (const Effect& e : effects)
            if (e.enabled)
                if (const T* t = std::get_if<T>(&e.settings))
                    return t;
        return nullptr;
    }

    bool operator==(const TextStyle&) const = default;
};

// Stable names for the project format (layers/<id>.json): plate, glow,
// shadow, outline, fill, shade, bevel; drop, extrude; pixels, cells; flat,
// rows, columns, rainbow; n, ne, e, se, s, sw, w, nw.
std::string_view effectKindName(EffectKind kind);
std::optional<EffectKind> effectKindFromName(std::string_view name);
std::string_view shadowFormName(ShadowForm form);
std::optional<ShadowForm> shadowFormFromName(std::string_view name);
std::string_view outlineFormName(OutlineForm form);
std::optional<OutlineForm> outlineFormFromName(std::string_view name);
std::string_view fillModeName(FillMode mode);
std::optional<FillMode> fillModeFromName(std::string_view name);
std::string_view compassPointName(Compass c);
std::optional<Compass> compassPointFromName(std::string_view name);

// Why the stack can't be drawn (more than kMaxEffects effects, two of one
// kind, a ramp this build doesn't have, a colour or setting out of range),
// or nothing. Effects switched off are checked too: switching one on
// mustn't make a stack undrawable.
std::optional<std::string> textStyleProblem(const TextStyle& style);

// The cells the style draws beyond the letters on each side: the outline's
// width (a cell, for either form) plus the largest of the glow's width and
// the shadow's reach that way (its distance in pixels: columns across, rows
// rounded up down), plus the plate's padding. A live text layer is its
// text's box grown by these, the text at (left, top) in it.
struct StyleMargins {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const StyleMargins&) const = default;
};
StyleMargins styleMargins(const TextStyle& style);

} // namespace bw
