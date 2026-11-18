#include "blockwright/palette_swap.h"

#include "blockwright/ansi.h"
#include "blockwright/micron.h"
#include "blockwright/ramp.h"
#include "blockwright/xbin.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <variant>
#include <vector>

namespace bw {

namespace {

void setError(std::string* error, std::string what) {
    if (error)
        *error = std::move(what);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::optional<int> number(std::string_view s, int lo, int hi) {
    s = trim(s);
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || ec != std::errc() || end != s.data() + s.size() || v < lo || v > hi)
        return std::nullopt;
    return v;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = char(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const size_t n = text.find('\n');
        out.push_back(trim(text.substr(0, n)));
        text = n == std::string_view::npos ? std::string_view() : text.substr(n + 1);
    }
    return out;
}

// "R G B [anything]" with each 0-255.
std::optional<Rgb> rgbLine(std::string_view line) {
    std::array<int, 3> v{};
    for (int& c : v) {
        line = trim(line);
        size_t n = 0;
        while (n < line.size() && !std::isspace(static_cast<unsigned char>(line[n])))
            ++n;
        const auto x = number(line.substr(0, n), 0, 255);
        if (!x)
            return std::nullopt;
        c = *x;
        line.remove_prefix(n);
    }
    return Rgb{std::uint8_t(v[0]), std::uint8_t(v[1]), std::uint8_t(v[2])};
}

std::optional<Rgb> hexLine(std::string_view line) {
    if (!line.empty() && line.front() == '#')
        line.remove_prefix(1);
    if (line.size() != 6)
        return std::nullopt;
    std::array<int, 6> d{};
    for (size_t i = 0; i < 6; ++i)
        if ((d[i] = hexDigit(line[i])) < 0)
            return std::nullopt;
    return Rgb{std::uint8_t(d[0] << 4 | d[1]), std::uint8_t(d[2] << 4 | d[3]), std::uint8_t(d[4] << 4 | d[5])};
}

} // namespace

ColourMap identityColourMap() {
    ColourMap m{};
    for (std::uint8_t i = 0; i < 16; ++i)
        m[i] = i;
    return m;
}

ColourMap swapColourMap(std::uint8_t a, std::uint8_t b) {
    ColourMap m = identityColourMap();
    std::swap(m[a & 15], m[b & 15]);
    return m;
}

std::optional<ColourMap> parseColourMap(std::string_view text, std::string* error) {
    std::string s;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            s += c;
    if (s.empty()) {
        setError(error, "the map is empty");
        return std::nullopt;
    }
    ColourMap m = identityColourMap();
    if (s.size() == 16 && s.find('=') == std::string::npos) {
        for (size_t i = 0; i < 16; ++i) {
            const int d = hexDigit(s[i]);
            if (d < 0) {
                setError(error, "\"" + s + "\" is not sixteen hex digits");
                return std::nullopt;
            }
            m[i] = std::uint8_t(d);
        }
        return m;
    }
    std::array<bool, 16> named{};
    std::string_view rest = s;
    while (true) {
        const size_t comma = rest.find(',');
        const std::string_view pair = rest.substr(0, comma);
        const size_t eq = pair.find('=');
        const auto from = eq == std::string_view::npos ? std::nullopt : number(pair.substr(0, eq), 0, 15);
        const auto to = eq == std::string_view::npos ? std::nullopt : number(pair.substr(eq + 1), 0, 15);
        if (!from || !to) {
            setError(error, "\"" + std::string(pair) + "\" is not a pair FROM=TO of colours 0-15");
            return std::nullopt;
        }
        if (named[std::size_t(*from)]) {
            setError(error, "colour " + std::to_string(*from) + " is mapped twice");
            return std::nullopt;
        }
        named[std::size_t(*from)] = true;
        m[std::size_t(*from)] = std::uint8_t(*to);
        if (comma == std::string_view::npos)
            break;
        rest.remove_prefix(comma + 1);
    }
    return m;
}

std::string colourMapText(const ColourMap& map) {
    std::string out;
    for (std::size_t i = 0; i < 16; ++i)
        if (map[i] != i)
            out += (out.empty() ? "" : ",") + std::to_string(i) + "=" + std::to_string(map[i]);
    return out;
}

Cell remapCell(Cell c, const ColourMap& map) {
    if (c.fg.isIndexed())
        c.fg = Color::indexed(map[c.fg.index & 15]);
    if (c.bg.isIndexed())
        c.bg = Color::indexed(map[c.bg.index & 15]);
    return c;
}

namespace {

// The cells of `canvas` in `area` that `f` changes the colours of.
template <typename F>
CellEdit colourEdit(const Canvas& canvas, std::optional<CellRect> area, bool transparentToo, F f) {
    const CellRect all{0, 0, canvas.width(), canvas.height()};
    const CellRect r = area ? area->intersected(all) : all;
    CellEdit edit;
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x) {
            const Cell& c = canvas.at(x, y);
            if (c.transparent && !transparentToo)
                continue;
            const Cell after = f(c);
            if (!(after.fg == c.fg && after.bg == c.bg))
                edit.changes.push_back({x, y, c, after});
        }
    return edit;
}

} // namespace

CellEdit remapEdit(const Canvas& canvas, const ColourMap& map, std::optional<CellRect> area, bool transparentToo) {
    return colourEdit(canvas, area, transparentToo, [&](const Cell& c) { return remapCell(c, map); });
}

Cell micronSnapCell(Cell c) {
    if (!c.fg.isIndexed())
        c.fg = Color::fromRgb(micron::nearestTwelveBit(c.fg.rgb));
    if (!c.bg.isIndexed())
        c.bg = Color::fromRgb(micron::nearestTwelveBit(c.bg.rgb));
    return c;
}

CellEdit micronSnapEdit(const Canvas& canvas, std::optional<CellRect> area, bool transparentToo) {
    return colourEdit(canvas, area, transparentToo, [](const Cell& c) { return micronSnapCell(c); });
}

int remapCanvas(Canvas& canvas, const ColourMap& map) {
    const CellEdit e = remapEdit(canvas, map);
    e.apply(canvas);
    return int(e.changes.size());
}

bool remappableLayer(const Layer& l) {
    switch (l.props.kind) {
    case LayerKind::Cells:
    case LayerKind::Pixels: return true;
    case LayerKind::Text: return !l.props.text;
    case LayerKind::Generator:
    case LayerKind::Reference: return false;
    }
    return false;
}

std::vector<std::pair<LayerId, CellEdit>> remapLayers(const LayerStack& stack, const ColourMap& map, int* skipped) {
    std::vector<std::pair<LayerId, CellEdit>> edits;
    int left = 0;
    for (const Layer& l : stack.layers()) {
        if (!remappableLayer(l)) {
            left += l.props.kind != LayerKind::Reference;  // a picture has no colours to leave
            continue;
        }
        CellEdit e = remapEdit(l.cells, map, std::nullopt, l.props.kind == LayerKind::Pixels);
        if (!e.changes.empty())
            edits.emplace_back(l.props.id, std::move(e));
    }
    if (skipped)
        *skipped = left;
    return edits;
}

TextStyle remapStyle(TextStyle style, const ColourMap& map) {
    for (Effect& e : style.effects)
        std::visit(
            [&](auto& fx) {
                if constexpr (requires { fx.colour; })
                    fx.colour = map[fx.colour & 15];
            },
            e.settings);
    return style;
}

std::vector<std::pair<LayerId, TextStyle>> remapStyles(const LayerStack& stack, const ColourMap& map) {
    std::vector<std::pair<LayerId, TextStyle>> out;
    for (const Layer& l : stack.layers()) {
        if (!remappableLayer(l) || !l.props.style)
            continue;
        TextStyle after = remapStyle(*l.props.style, map);
        if (after != *l.props.style)
            out.emplace_back(l.props.id, std::move(after));
    }
    return out;
}

ColourMap fitColourMap(const Palette& from, const Palette& to) {
    ColourMap m{};
    for (std::size_t i = 0; i < 16; ++i)
        m[i] = nearestIndex(from[i], to);
    return m;
}

int fitLoss(const Canvas& canvas, const Palette& from, const Palette& to, const ColourMap& map) {
    const auto lost = [&](const Color& c) { return c.isIndexed() && to[map[c.index & 15]] != from[c.index & 15]; };
    int n = 0;
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x) {
            const Cell& c = canvas.at(x, y);
            n += !c.transparent && (lost(c.fg) || lost(c.bg));
        }
    return n;
}

int lookChanged(const Canvas& before, const Palette& from, const Canvas& after, const Palette& to) {
    const auto rgb = [](const Color& c, const Palette& p) { return c.isIndexed() ? p[c.index & 15] : c.rgb; };
    // the one colour a cell shows all over: a blank's background, a full
    // block's foreground, a half block or shade in one colour twice
    const auto solid = [&](const Cell& c, const Palette& p) -> std::optional<Rgb> {
        if (attr::showsOnBlank(c.attrs))
            return std::nullopt;
        if (blankGlyph(c.ch))
            return rgb(c.bg, p);
        switch (c.ch.codePointOr(kNoCodePoint)) {
        case kFullBlock: return rgb(c.fg, p);
        case kUpperHalf:
        case kLowerHalf:
        case kShadeLight:
        case kShadeMedium:
        case kShadeDark:
            if (rgb(c.fg, p) == rgb(c.bg, p))
                return rgb(c.fg, p);
            return std::nullopt;
        default: return std::nullopt;
        }
    };
    const int w = std::min(before.width(), after.width()), h = std::min(before.height(), after.height());
    int n = std::max(before.width(), after.width()) * std::max(before.height(), after.height()) - w * h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Cell &a = before.at(x, y), &b = after.at(x, y);
            if (a.transparent || b.transparent) {
                n += a.transparent != b.transparent;
                continue;
            }
            const std::optional<Rgb> sa = solid(a, from), sb = solid(b, to);
            if (sa || sb) {
                n += sa != sb || (a.attrs & attr::Blink) != (b.attrs & attr::Blink);
                continue;
            }
            n += a.ch != b.ch || a.attrs != b.attrs || rgb(a.fg, from) != rgb(b.fg, to) ||
                 rgb(a.bg, from) != rgb(b.bg, to);
        }
    return n;
}

std::optional<Palette> readPaletteFile(std::string_view path, std::string_view bytes, std::string* error,
                                       int* colours) {
    std::vector<Rgb> found;
    if (artFormatOf(path, bytes) == ArtFormat::Xbin) {
        try {
            const Palette p = parseXbin(bytes).palette.value_or(kPalette);
            found.assign(p.begin(), p.end());
        } catch (const AnsiError& e) {
            setError(error, std::string("not a palette: ") + e.what());
            return std::nullopt;
        }
    } else {
        std::vector<std::string_view> ls = lines(bytes);
        const std::string_view first = ls.empty() ? std::string_view() : ls.front();
        if (first == "GIMP Palette") {
            for (size_t i = 1; i < ls.size(); ++i) {
                const std::string_view l = ls[i];
                if (l.empty() || l.front() == '#' || l.starts_with("Name:") || l.starts_with("Columns:"))
                    continue;
                const auto c = rgbLine(l);
                if (!c) {
                    setError(error, "line " + std::to_string(i + 1) + " is not R G B");
                    return std::nullopt;
                }
                found.push_back(*c);
            }
        } else if (first == "JASC-PAL") {
            const auto count = ls.size() > 2 ? number(ls[2], 1, 256) : std::nullopt;
            if (ls.size() < 3 || ls[1] != "0100" || !count) {
                setError(error, "not a JASC palette: no version 0100 and count");
                return std::nullopt;
            }
            for (size_t i = 3; i < ls.size() && int(found.size()) < *count; ++i) {
                if (ls[i].empty())
                    continue;
                const auto c = rgbLine(ls[i]);
                if (!c) {
                    setError(error, "line " + std::to_string(i + 1) + " is not R G B");
                    return std::nullopt;
                }
                found.push_back(*c);
            }
        } else {
            for (size_t i = 0; i < ls.size(); ++i) {
                if (ls[i].empty())
                    continue;
                const auto c = hexLine(ls[i]);
                if (!c) {
                    setError(error, "not a palette: line " + std::to_string(i + 1) +
                                        " is not a colour (GIMP, JASC, a hex colour a line, or an XBIN)");
                    return std::nullopt;
                }
                found.push_back(*c);
            }
        }
    }
    if (colours)
        *colours = int(found.size());
    if (found.size() < 16) {
        setError(error, "the palette has " + std::to_string(found.size()) + (found.size() == 1 ? " colour" : " colours") +
                            "; it needs 16");
        return std::nullopt;
    }
    Palette p{};
    std::copy_n(found.begin(), 16, p.begin());
    return p;
}

} // namespace bw
