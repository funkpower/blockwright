#include "blockwright/target.h"

#include "blockwright/ansi.h"
#include "blockwright/mirc.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace bw {

std::string_view targetName(Target t) {
    switch (t) {
    case Target::Irc: return "irc";
    case Target::Micron: return "micron";
    default: return "ansi";
    }
}

std::optional<Target> parseTarget(std::string_view name) {
    std::string s(name);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (s == "ansi")
        return Target::Ansi;
    if (s == "irc")
        return Target::Irc;
    if (s == "micron")
        return Target::Micron;
    return std::nullopt;
}

TypingPolicy typingPolicy(Target target, TextEncoding encoding) {
    return richTarget(target) || encoding == TextEncoding::Utf8 ? TypingPolicy::Unicode : TypingPolicy::Cp437;
}

Color swatchColor(int i) {
    i = std::clamp(i, 0, swatchCount(Target::Irc) - 1);
    if (i < 16)
        return Color::indexed(std::uint8_t(i));
    const std::uint32_t c = irc::kMircRgb[std::size_t(i)];
    return Color::fromRgb({std::uint8_t(c >> 16), std::uint8_t(c >> 8), std::uint8_t(c)});
}

int swatchOf(const Color& c, Target t) {
    if (c.isIndexed())
        return c.index;
    if (richTarget(t))
        for (int i = 16; i < swatchCount(t); ++i)
            if (swatchColor(i) == c)
                return i;
    return t == Target::Micron ? -1 : c.paletteIndex();
}

} // namespace bw
