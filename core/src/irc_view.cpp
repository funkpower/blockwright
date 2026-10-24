#include "blockwright/irc_view.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace bw::irc {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

// Up to two digits at `i` as a colour number (advancing `i`); empty if there
// is no digit there.
std::optional<std::uint8_t> number(std::string_view s, std::size_t& i) {
    if (i >= s.size() || !digit(s[i]))
        return std::nullopt;
    int n = s[i++] - '0';
    if (i < s.size() && digit(s[i]))
        n = n * 10 + (s[i++] - '0');
    return static_cast<std::uint8_t>(n);
}

// mIRC colours past 98 are the client's own, as 99 is.
std::uint8_t colour(std::uint8_t n) { return n > 98 ? kClientColour : n; }

// The UTF-8 sequence at `i` (advancing `i`); U+FFFD for a byte that doesn't
// start a valid one, which is then one byte long.
char32_t glyph(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    int len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xe ? 3 : (b >> 3) == 0x1e ? 4 : 0;
    char32_t cp = len == 1 ? b : len == 2 ? (b & 0x1f) : len == 3 ? (b & 0x0f) : (b & 0x07);
    bool ok = len > 0 && i + std::size_t(len) <= s.size();
    for (int k = 1; ok && k < len; ++k) {
        const auto c = static_cast<unsigned char>(s[i + std::size_t(k)]);
        ok = (c >> 6) == 0x2;
        cp = (cp << 6) | (c & 0x3f);
    }
    // overlong forms, surrogates and code points past U+10FFFF
    static constexpr char32_t kLeast[] = {0, 0, 0x80, 0x800, 0x10000};
    ok = ok && cp >= kLeast[len] && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
    if (!ok) {
        ++i;
        return U'�';
    }
    i += std::size_t(len);
    return cp;
}

Color shownColour(std::uint8_t mirc, std::uint32_t client) {
    const std::uint32_t rgb = mirc == kClientColour ? client : kMircRgb[mirc];
    return Color::fromRgb(Rgb{std::uint8_t(rgb >> 16), std::uint8_t(rgb >> 8), std::uint8_t(rgb)});
}

Cell clientBlank() {
    Cell c;
    c.fg = shownColour(kClientColour, kClientForeground);
    c.bg = shownColour(kClientColour, kClientBackground);
    return c;
}

} // namespace

std::vector<ShownCell> decodeLine(std::string_view line) {
    std::vector<ShownCell> out;
    std::uint8_t fg = kClientColour, bg = kClientColour;
    bool reverse = false;
    std::size_t i = 0;
    while (i < line.size()) {
        switch (line[i]) {
        case '\x03': {
            ++i;
            const auto f = number(line, i);
            if (!f) {
                fg = bg = kClientColour;
                break;
            }
            fg = colour(*f);
            // a comma only starts a background when a digit follows it
            if (i + 1 < line.size() && line[i] == ',' && digit(line[i + 1])) {
                ++i;
                bg = colour(*number(line, i));
            }
            break;
        }
        case '\x0f':
            ++i;
            fg = bg = kClientColour;
            reverse = false;
            break;
        case '\x16':
            ++i;
            reverse = !reverse;
            break;
        case '\x02':
        case '\x11':
        case '\x1d':
        case '\x1e':
        case '\x1f':
            ++i;
            break;
        default: {
            const char32_t ch = glyph(line, i);
            MircCell c{ch, fg, bg};
            if (reverse)
                std::swap(c.fg, c.bg);
            out.push_back({c, i});
            break;
        }
        }
    }
    return out;
}

int View::cutRows() const {
    return static_cast<int>(std::count_if(rows.begin(), rows.end(), [](const ViewRow& r) { return r.cut(); }));
}

View ircView(const Canvas& canvas, std::size_t budget, const EncodeOptions& opts) {
    View v;
    v.canvas = Canvas(canvas.width(), canvas.height());
    const Cell blank = clientBlank();
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x)
            v.canvas.at(x, y) = blank;
    const std::vector<std::string> lines = encodeCanvas(canvas, opts);
    const int h = static_cast<int>(lines.size());
    int top = 0, bottom = h;
    while (top < bottom && lines[std::size_t(top)].empty())
        ++top;
    while (bottom > top && lines[std::size_t(bottom) - 1].empty())
        --bottom;
    v.rows.resize(std::size_t(h));
    for (int y = 0; y < h; ++y) {
        ViewRow& row = v.rows[std::size_t(y)];
        const std::string& line = lines[std::size_t(y)];
        row.bytes = line.size();
        row.trimmed = y < top || y >= bottom;
        const std::vector<ShownCell> cells = decodeLine(line);
        row.kept = static_cast<int>(cells.size());
        // a cut line keeps a byte for the reset the sender ends it with
        const std::size_t limit = line.size() <= budget || budget == 0 ? budget : budget - 1;
        for (const ShownCell& s : cells) {
            if (s.end > limit || row.shown >= canvas.width())
                break;
            Cell& c = v.canvas.at(row.shown++, y);
            c.ch = s.cell.ch;
            c.fg = shownColour(s.cell.fg, kClientForeground);
            c.bg = shownColour(s.cell.bg, kClientBackground);
        }
    }
    return v;
}

} // namespace bw::irc
