#include "blockwright/big_text.h"

#include "blockwright/irc.h"
#include "blockwright/project.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>

namespace bw {
namespace {
constexpr std::array<std::string_view, 6> names{"none", "shadow", "3d", "outline", "block", "rainbow"};
class BigFace final : public Face {
  public:
    std::string_view name() const override { return "big-block"; }
    int height() const override { return 16; }
    int letterSpacing() const override { return 0; }
    std::vector<ConfusablePair> confusables() const override { return {}; }
    std::optional<Glyph> glyph(char32_t ch) const override {
        auto base = blockFace().glyph(ch);
        if (!base)
            return std::nullopt;
        // Keep one font table. The reference uses a different IBM 8x8
        // variant, including a crossed Z and a tailed lowercase l.
        static constexpr std::pair<char32_t, std::uint64_t> deltas[] = {
#include "big_text_deltas.inc"
        };
        std::uint64_t delta = 0;
        for (const auto& [c, d] : deltas)
            if (ch == c) {
                delta = d;
                break;
            }
        Glyph out{InkMask(8, 16), std::nullopt};
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                if (base->mask.at(x, y) != bool((delta >> (8 * (7 - y))) & (0x80 >> x))) {
                    out.mask.set(x, 2 * y);
                    out.mask.set(x, 2 * y + 1);
                }
        return out;
    }
};
class SpacedBigFace final : public Face {
  public:
    explicit SpacedBigFace(int spacing) : spacing_(spacing) {}
    std::string_view name() const override { return bigTextFace().name(); }
    int height() const override { return 16; }
    int letterSpacing() const override { return spacing_; }
    std::optional<Glyph> glyph(char32_t ch) const override { return bigTextFace().glyph(ch); }
    std::vector<ConfusablePair> confusables() const override { return {}; }

  private:
    int spacing_;
};
std::u32string cased(const TextSource& s) {
    auto text = s.text;
    if (s.textCase == TextCase::Capitals)
        for (auto& ch : text)
            if (ch >= U'a' && ch <= U'z')
                ch += U'A' - U'a';
    return text;
}
int wrap(const TextSource& s) { return s.wrapWidth ? std::max(1, s.wrapWidth - 2) : 0; }

void checkSize(const TextExtent& e) {
    if (e.width > 65535 || e.height > 65535 ||
        std::uint64_t(e.width) * std::uint64_t(e.height) > project::kMaxLayerCells)
        throw TextError("the big text box exceeds a layer's size limit");
}
std::uint8_t paletteColour(std::uint8_t mirc) {
    for (std::uint8_t i = 0; i < 16; ++i)
        if (irc::mircColor(Color::indexed(i)) == mirc)
            return i;
    throw TextError("big text colour is not mIRC 0 to 15");
}
} // namespace

const Face& bigTextFace() {
    static const BigFace face;
    return face;
}
std::string_view bigTextStyleName(BigTextStyle style) {
    const auto i = static_cast<std::size_t>(style);
    return i < names.size() ? names[i] : std::string_view{};
}
std::optional<BigTextStyle> bigTextStyleFromName(std::string_view name) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return static_cast<BigTextStyle>(i);
    return std::nullopt;
}
std::vector<std::uint8_t> bigTextDefaultColours(BigTextStyle style) {
    switch (style) {
    case BigTextStyle::Shadow:
        return {7, 0};
    case BigTextStyle::ThreeD:
        return {8, 0};
    case BigTextStyle::Outline:
        return {0, 12};
    case BigTextStyle::Block:
        return {0};
    case BigTextStyle::Rainbow:
        return {4, 7, 8, 9, 11, 12, 13};
    case BigTextStyle::None:
        return {};
    }
    return {};
}
std::string bigTextColoursText(const std::vector<std::uint8_t>& colours) {
    std::string out;
    for (const auto c : colours) {
        if (!out.empty())
            out += ',';
        out += std::to_string(c);
    }
    return out;
}
std::optional<std::vector<std::uint8_t>> parseBigTextColours(std::string_view input) {
    std::string text(input);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    const auto trim = [](std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    };
    static const std::map<std::string_view, std::vector<std::uint8_t>> palettes{
        {"rainbow", {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 2}},
        {"fire", {4, 7, 8, 7, 4, 5}},
        {"ice", {0, 11, 12, 2, 12, 11}},
        {"neon", {13, 9, 11, 8}},
        {"sea", {2, 12, 10, 11}},
        {"forest", {3, 9, 3, 10}},
        {"candy", {13, 0, 11, 0}},
        {"steel", {15, 14, 0, 14}}};
    if (const auto it = palettes.find(trim(text)); it != palettes.end())
        return it->second;
    static const std::map<std::string_view, std::uint8_t> colourNames{
        {"white", 0}, {"black", 1},  {"navy", 2},   {"blue", 2},    {"green", 3},     {"red", 4},
        {"brown", 5}, {"maroon", 5}, {"purple", 6}, {"magenta", 6}, {"orange", 7},    {"yellow", 8},
        {"lime", 9},  {"teal", 10},  {"cyan", 11},  {"aqua", 11},   {"royal", 12},    {"lightblue", 12},
        {"pink", 13}, {"grey", 14},  {"gray", 14},  {"silver", 15}, {"lightgrey", 15}};
    std::vector<std::uint8_t> out;
    std::string_view rest = text;
    for (;;) {
        const auto comma = rest.find(',');
        const auto token = trim(rest.substr(0, comma));
        if (token.empty() || out.size() == TextSource::kMaxBigColours)
            return std::nullopt;
        if (const auto it = colourNames.find(token); it != colourNames.end())
            out.push_back(it->second);
        else {
            int n = -1;
            const auto [end, err] = std::from_chars(token.data(), token.data() + token.size(), n);
            if (err != std::errc{} || end != token.data() + token.size() || n < 0 || n > 15)
                return std::nullopt;
            out.push_back(static_cast<std::uint8_t>(n));
        }
        if (comma == rest.npos)
            break;
        rest.remove_prefix(comma + 1);
    }
    return out;
}

TextExtent measureBigText(const TextSource& s) {
    const auto e = measureText(SpacedBigFace(s.letterSpacing), cased(s), {wrap(s)});
    return {std::max<std::int64_t>(s.wrapWidth, e.width + 2),
            e.lines * 10 + std::max<std::int64_t>(0, e.lines - 1) * (s.lineSpacing + 1), e.lines};
}

BigTextRender renderBigText(const TextSource& s) {
    if (s.bigStyle == BigTextStyle::None || bigTextStyleName(s.bigStyle).empty())
        throw TextError("no big text style");
    if (s.bigColours.size() > TextSource::kMaxBigColours)
        throw TextError("too many big text colours");
    const auto defaults = bigTextDefaultColours(s.bigStyle);
    const auto colours = s.bigColours.empty() ? defaults : s.bigColours;
    for (const auto c : colours)
        if (c > 15)
            throw TextError("big text colour is not mIRC 0 to 15");
    const auto e = measureBigText(s);
    checkSize(e); // before layoutText allocates the mask
    const auto layout = layoutText(SpacedBigFace(s.letterSpacing), cased(s), {wrap(s)});
    BigTextRender out;
    out.drawing.missing = layout.missing;
    Canvas canvas(int(e.width), int(e.height));
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x)
            canvas.at(x, y) = transparentCell();
    int top = 0;
    for (std::size_t l = 0; l < layout.lines.size(); ++l) {
        const auto& line = layout.lines[l];
        const int width = int(e.width);
        const int shift = s.align == TextAlign::Left    ? 0
                          : s.align == TextAlign::Right ? width - line.width - 2
                                                        : (width - line.width - 2) / 2;
        const auto on = [&](int x, int y) {
            return x >= 1 && x <= line.width && y >= 1 && y <= 8 &&
                   layout.mask.at(x - 1, int(l) * 16 + 2 * (y - 1));
        };
        // Rainbow walks the colours a character at a time, spaces included,
        // from each line's first character: the colour slot of every column.
        std::vector<std::size_t> slot;
        if (s.bigStyle == BigTextStyle::Rainbow) {
            slot.assign(std::size_t(line.width + 2), 0);
            const auto& face = bigTextFace();
            const auto text = cased(s);
            int x = 1;
            for (std::size_t k = 0; k < line.count; ++k) {
                auto g = face.glyph(text[line.first + k]);
                if (!g)
                    g = face.glyph(U' ');
                const int w = g ? g->mask.width : 0;
                for (int i = 0; i < w && x + i < int(slot.size()); ++i)
                    slot[std::size_t(x + i)] = k;
                x += w + s.letterSpacing;
            }
        }
        // Record colour rather than glyph: a full foreground block gives
        // the reference's background-colour pixel with iCE on AND off.
        std::vector<int> pixels(std::size_t(line.width + 2) * 10, -1);
        int firstRow = 10, lastRow = -1;
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < line.width + 2; ++x) {
                int c = -1;
                if (on(x, y))
                    c = slot.empty() ? colours[0] : colours[slot[std::size_t(x)] % colours.size()];
                else if ((s.bigStyle == BigTextStyle::Shadow && on(x + 1, y)) ||
                         (s.bigStyle == BigTextStyle::ThreeD && on(x - 1, y - 1)))
                    c = colours.size() > 1 ? colours[1] : defaults.back();
                else if (s.bigStyle == BigTextStyle::Outline) {
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if (on(x + dx, y + dy))
                                c = colours.size() > 1 ? colours[1] : defaults.back();
                }
                if (c >= 0) {
                    pixels[std::size_t(y) * (line.width + 2) + x] = c;
                    firstRow = std::min(firstRow, y);
                    lastRow = std::max(lastRow, y);
                }
            }
        // Thinned Rainbow: a pixel with no edge-neighbour of its own colour
        // is a speck to the ladder lint unless it is white, the top rung of
        // every ramp, so it is drawn white.
        if (!slot.empty()) {
            const int w = line.width + 2;
            const auto at = [&](int x, int y) {
                return x < 0 || y < 0 || x >= w || y >= 10 ? -1 : pixels[std::size_t(y) * w + x];
            };
            std::vector<std::size_t> lone;
            for (int y = 0; y < 10; ++y)
                for (int x = 0; x < w; ++x)
                    if (const int c = at(x, y); c >= 0 && at(x - 1, y) != c && at(x + 1, y) != c &&
                                                at(x, y - 1) != c && at(x, y + 1) != c)
                        lone.push_back(std::size_t(y) * w + x);
            for (const auto i : lone)
                pixels[i] = 0;
        }
        // An empty editor line keeps its height so Enter remains editable.
        if (lastRow < 0) {
            firstRow = 1;
            lastRow = 8;
        }
        out.lines.push_back({line.first, line.count, shift + 1, top + 1 - firstRow, top});
        for (int y = firstRow; y <= lastRow; ++y)
            for (int x = 0; x < line.width + 2; ++x) {
                const int c = pixels[std::size_t(y) * (line.width + 2) + x];
                if (c < 0)
                    continue;
                Cell cell;
                cell.ch = U'\u2588';
                cell.fg = Color::indexed(paletteColour(static_cast<std::uint8_t>(c)));
                cell.bg = Color::indexed(Black);
                canvas.at(x + shift, top + y - firstRow) = cell;
            }
        top += lastRow - firstRow + 1 + s.lineSpacing + 1;
    }
    int left = canvas.width(), right = -1;
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x)
            if (!canvas.at(x, y).transparent) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
    if (right < 0) {
        left = 0;
        right = 0;
    }
    // A wrap/alignment box retains its width; natural-sized text is tight.
    if (s.wrapWidth) {
        left = 0;
        right = canvas.width() - 1;
    }
    const int height = std::max(0, top - s.lineSpacing - 1);
    out.drawing.cells = Canvas(right - left + 1, height);
    for (int y = 0; y < height; ++y)
        for (int x = left; x <= right; ++x)
            out.drawing.cells.at(x - left, y) = canvas.at(x, y);
    for (auto& line : out.lines)
        line.left -= left;
    return out;
}
} // namespace bw
