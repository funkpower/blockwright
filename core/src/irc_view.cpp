#include "blockwright/irc_view.h"

#include "blockwright/cp437.h"
#include "blockwright/ramp.h"

#include <QChar>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
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

// The sender's re-encoding of a line over the budget, as ascii.py
// (1.32.1) does it in fit(): parse_irc, then encode_irc_min.
// (ascii.py falls back on a plainer encoding for a line of more than 2000
// cells or one with attributes; the export's lines have at most kMaxColumns
// cells, and their only attributes are the ^B pairs that cancel out.)
// A colour is -1 where the line sets none (the client's own).
namespace sender {

constexpr int kNone = -1;
constexpr int kAny = -2;  // encode_irc_min: whatever the colour is now

struct SentCell {
    char32_t ch;
    int fg, bg;
};

// Python's str.isdigit(): a code point with a digit value (², not only 0-9).
bool pyDigit(char32_t ch) { return QChar::digitValue(ch) >= 0; }

std::string utf8(char32_t ch) {
    std::string s;
    appendUtf8(s, ch);
    return s;
}

std::string colourCode(int fg, int bg = kNone) {
    char buf[8];
    if (bg == kNone)
        std::snprintf(buf, sizeof buf, "\x03%02d", fg);
    else
        std::snprintf(buf, sizeof buf, "\x03%02d,%02d", fg == kNone ? 99 : fg, bg);
    return buf;
}

// parse_irc, for the export's lines: no ^D colours, tabs or other control
// bytes, and attributes that leave the cells as they are.
std::vector<SentCell> parse(std::string_view line) {
    std::vector<SentCell> cells;
    int fg = kNone, bg = kNone;
    auto mirc = [](std::uint8_t n) { return n < 99 ? int(n) : kNone; };
    std::size_t i = 0;
    while (i < line.size()) {
        switch (line[i]) {
        case '\x03': {
            ++i;
            const auto f = number(line, i);
            if (!f) {
                fg = bg = kNone;
                break;
            }
            fg = mirc(*f);
            if (i + 1 < line.size() && line[i] == ',' && digit(line[i + 1])) {
                ++i;
                bg = mirc(*number(line, i));
            }
            break;
        }
        case '\x0f':
            ++i;
            fg = bg = kNone;
            break;
        case '\x02':
        case '\x11':
        case '\x16':
        case '\x1d':
        case '\x1e':
        case '\x1f':
            ++i;
            break;
        default:
            cells.push_back({glyph(line, i), fg, bg});
        }
    }
    return cells;
}

struct Colours {
    int fg, bg;
    bool operator==(const Colours&) const = default;
};

// _code: the codes that take the colours `cur` to ones that show `ch` in
// `fg` on `bg` (either may be kAny), and the colours after them.
std::pair<std::string, Colours> code(Colours cur, int fg, int bg, char32_t ch) {
    std::string codes;
    if ((fg == kNone && cur.fg != kNone) || (bg == kNone && cur.bg != kNone)) {
        codes = "\x03";
        cur = {kNone, kNone};
        if ((fg == kNone || fg == kAny) && (bg == kNone || bg == kAny)) {
            if (pyDigit(ch) || ch == U',')
                codes += "\x02\x02";
            return {codes, cur};
        }
    }
    if (bg != kNone && bg != kAny && bg != cur.bg) {
        const int f = fg == kAny ? cur.fg : fg;
        return {codes + colourCode(f, bg), {f, bg}};
    }
    if (fg != kNone && fg != kAny && fg != cur.fg)
        return {codes + colourCode(fg) + (ch == U',' ? "\x02\x02" : ""), {fg, cur.bg}};
    return {codes, cur};
}

// encode_irc_min: the shortest encoding over the ways each cell can be
// drawn (a space or a full block of the same colour). Kept in ascii.py's
// order throughout, so that of two encodings of one length it picks the one
// it picks, which decides where a cut falls.
std::string encodeMin(const std::vector<SentCell>& cells) {
    struct Entry {
        Colours state;
        std::size_t cost;
        std::size_t prev;  // its state's place in the step before
        std::string text;
    };
    std::vector<std::vector<Entry>> steps;
    std::vector<Entry> states{{{kNone, kNone}, 0, 0, {}}};
    // trailing spaces in the client's colours aren't sent
    std::size_t end = cells.size();
    while (end && cells[end - 1].ch == U' ' && cells[end - 1].bg == kNone)
        --end;
    for (std::size_t i = 0; i < end; ++i) {
        const SentCell& c = cells[i];
        struct Option {
            char32_t glyph;
            int fg, bg;
        };
        std::vector<Option> options;
        if (c.ch == U' ') {
            options.push_back({U' ', kAny, c.bg});
            options.push_back({U' ', c.fg, c.bg});
            if (c.bg != kNone)
                options.push_back({kFullBlock, c.bg, kAny});
        } else if (c.ch == kFullBlock && c.fg != kNone) {
            options.push_back({kFullBlock, c.fg, kAny});
            options.push_back({kFullBlock, c.fg, c.bg});
            options.push_back({U' ', kAny, c.fg});
        } else {
            options.push_back({c.ch, c.fg, c.bg});
        }
        std::vector<Entry> next;
        for (std::size_t s = 0; s < states.size(); ++s)
            for (const Option& o : options) {
                auto [codes, after] = code(states[s].state, o.fg, o.bg, o.glyph);
                const std::string glyphText = utf8(o.glyph);
                const std::size_t total = states[s].cost + codes.size() + glyphText.size();
                const auto at = std::find_if(next.begin(), next.end(), [&](const Entry& e) { return e.state == after; });
                if (at == next.end())
                    next.push_back({after, total, s, codes + glyphText});
                else if (total < at->cost)
                    *at = {after, total, s, codes + glyphText};
            }
        // a state more than a colour code (9 bytes) behind the best can't win
        std::size_t best = next.front().cost;
        for (const Entry& e : next)
            best = std::min(best, e.cost);
        std::erase_if(next, [&](const Entry& e) { return e.cost > best + 9; });
        steps.push_back(next);
        states = std::move(next);
    }
    if (steps.empty())
        return {};
    std::size_t at = 0;
    for (std::size_t s = 1; s < states.size(); ++s)
        if (states[s].cost < states[at].cost)
            at = s;
    std::vector<const std::string*> parts;
    for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
        parts.push_back(&(*step)[at].text);
        at = (*step)[at].prev;
    }
    std::string out;
    for (auto p = parts.rbegin(); p != parts.rend(); ++p)
        out += **p;
    return out;
}

// fit()'s measure: a line ending in a space is sent with a reset after it.
std::size_t sentSize(const std::string& line) { return line.size() + (!line.empty() && line.back() == ' '); }

} // namespace sender

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
        int sent = row.kept;
        if (sender::sentSize(line) > budget) {
            // over the budget, the sender encodes the line again in as few
            // bytes as it can, and cuts that if it is still over, keeping a
            // byte for the reset it ends the line with. The encoding again can
            // be the shorter one and fit: a padded line ends on a
            // black-on-black █ that the sender sends as a space.
            const std::string again = sender::encodeMin(sender::parse(line));
            if (sender::sentSize(again) > budget) {
                const std::size_t limit = budget == 0 ? 0 : budget - 1;
                const std::vector<ShownCell> carried = decodeLine(again);
                sent = static_cast<int>(std::count_if(carried.begin(), carried.end(),
                                                      [limit](const ShownCell& s) { return s.end <= limit; }));
            }
        }
        // the cells are drawn as the export has them: the encoding again
        // only trades a space for a full block of the same colour
        for (const ShownCell& s : cells) {
            if (row.shown >= sent || row.shown >= canvas.width())
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
