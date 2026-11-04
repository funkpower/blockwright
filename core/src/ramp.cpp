#include "blockwright/ramp.h"

#include <algorithm>
#include <cstdlib>

namespace bw {

int Ramp::indexOf(std::uint8_t colour) const {
    const auto it = std::find(colours.begin(), colours.end(), colour);
    return it == colours.end() ? -1 : int(it - colours.begin());
}

const std::vector<Ramp>& namedRamps() {
    static const std::vector<Ramp> ramps = {
        {"fire", {Black, Red, LightRed, Yellow, White}},
        // a hue ramp: not sorted by brightness
        {"sunset", {Black, Blue, Magenta, LightMagenta, LightRed, Yellow}},
        {"ice", {Black, Blue, LightBlue, LightCyan, White}},
        {"grass", {Black, Green, LightGreen, Yellow}},
        {"steel", {Black, DarkGrey, Grey, White}},
        {"skin", {Black, Red, Brown, LightRed, Yellow}},
        {"sea", {Black, Blue, Cyan, LightCyan}},
        {"toxic", {Black, Green, LightGreen, LightCyan, White}},
        {"violet", {Black, Magenta, LightMagenta, White}},
        {"wood", {Black, Red, Brown, Yellow}},
    };
    return ramps;
}

std::optional<Ramp> findRamp(std::string_view name) {
    for (const Ramp& r : namedRamps())
        if (r.name == name)
            return r;
    return std::nullopt;
}

std::string rampProblem(const Ramp& ramp) {
    if (ramp.colours.size() < 2)
        return "a ramp needs at least two colours";
    for (size_t i = 0; i < ramp.colours.size(); ++i) {
        if (ramp.colours[i] > 15)
            return "colour " + std::to_string(ramp.colours[i]) + " is not a palette index (0-15)";
        for (size_t j = 0; j < i; ++j)
            if (ramp.colours[j] == ramp.colours[i])
                return "colour " + std::to_string(ramp.colours[i]) + " appears twice";
    }
    return {};
}

namespace {

int shadeQuarters(Glyph g) {
    switch (g.codePointOr(kNoCodePoint)) {
    case kShadeLight: return 1;
    case kShadeMedium: return 2;
    case kShadeDark: return 3;
    default: return 0;
    }
}

char32_t shadeGlyph(int quarters) {
    return quarters == 1 ? kShadeLight : quarters == 2 ? kShadeMedium : kShadeDark;
}

} // namespace

std::optional<std::uint8_t> solidColour(const Cell& cell) {
    const bool fgOk = cell.fg.isIndexed(), bgOk = cell.bg.isIndexed();
    const std::uint8_t fg = cell.fg.paletteIndex(), bg = cell.bg.paletteIndex();
    switch (cell.ch.codePointOr(kNoCodePoint)) {
    case U' ': return bgOk ? std::optional<std::uint8_t>(bg) : std::nullopt;
    case kFullBlock: return fgOk ? std::optional<std::uint8_t>(fg) : std::nullopt;
    case kUpperHalf:
    case kLowerHalf:
    case kShadeLight:
    case kShadeMedium:
    case kShadeDark:
        if (fgOk && bgOk && fg == bg)
            return fg;
        return std::nullopt;
    default: return std::nullopt;
    }
}

std::optional<int> rungOf(const Ramp& ramp, const Cell& cell) {
    if (cell.attrs & attr::Blink)
        return std::nullopt;
    if (const auto c = solidColour(cell)) {
        const int i = ramp.indexOf(*c);
        if (i < 0)
            return std::nullopt;
        return i * kRungsPerStep;
    }
    const int q = shadeQuarters(cell.ch);
    if (!q || !cell.fg.isIndexed() || !cell.bg.isIndexed())
        return std::nullopt;
    const int f = ramp.indexOf(cell.fg.paletteIndex()), b = ramp.indexOf(cell.bg.paletteIndex());
    if (f < 0 || b < 0 || std::abs(f - b) != 1)
        return std::nullopt;
    // q quarters of the foreground over the background
    return f > b ? b * kRungsPerStep + q : f * kRungsPerStep + (kRungsPerStep - q);
}

std::optional<ShadeForm> shadeForm(const Ramp& ramp, const Cell& cell) {
    const auto g = rungOf(ramp, cell);
    if (!g || *g % kRungsPerStep == 0)
        return std::nullopt;
    return ramp.indexOf(cell.fg.paletteIndex()) > ramp.indexOf(cell.bg.paletteIndex()) ? ShadeForm::LightOnTop
                                                                                        : ShadeForm::DarkOnTop;
}

Cell rungCell(const Ramp& ramp, int rung, ShadeForm form) {
    Cell c;
    if (ramp.colours.empty())
        return c;
    rung = std::clamp(rung, 0, ramp.topRung());
    const int i = rung / kRungsPerStep, q = rung % kRungsPerStep;
    if (q == 0) {
        const std::uint8_t col = ramp.colours[static_cast<size_t>(i)];
        if (col < 8) {
            c.ch = U' ';
            c.fg = Color::indexed(Black);
            c.bg = Color::indexed(col);
        } else {
            c.ch = kFullBlock;
            c.fg = Color::indexed(col);
            c.bg = Color::indexed(Black);
        }
        return c;
    }
    const std::uint8_t dark = ramp.colours[static_cast<size_t>(i)], light = ramp.colours[static_cast<size_t>(i) + 1];
    // Only one way round keeps the background dark: use it.
    if (dark >= 8 && light < 8)
        form = ShadeForm::DarkOnTop;
    else if (light >= 8 && dark < 8)
        form = ShadeForm::LightOnTop;
    if (form == ShadeForm::LightOnTop) {
        c.ch = shadeGlyph(q);
        c.fg = Color::indexed(light);
        c.bg = Color::indexed(dark);
    } else {
        c.ch = shadeGlyph(kRungsPerStep - q);
        c.fg = Color::indexed(dark);
        c.bg = Color::indexed(light);
    }
    return c;
}

} // namespace bw
