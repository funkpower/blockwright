#include "blockwright/irc_view.h"

#include "blockwright/cp437.h"
#include "blockwright/mirc.h"
#include "blockwright/ramp.h"
#include "blockwright/width.h"

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
char32_t glyph(std::string_view s, std::size_t& i) { return nextUtf8(s, i).value_or(U'\uFFFD'); }

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

// A code point the ascii script joins to the one before it, on a Python
// whose Unicode is joinUnicode(unicode): ascii.py's zero_width, Mn and Me
// from that version's own categories (joinMask).
bool zeroWidth(char32_t cp, int unicode) { return (joinMask(cp) >> unicode & 1) != 0; }

// Whether zero-width code points after `cp` join it (ascii.py's takes_marks):
// not after a control (C0, NUL included, DEL, C1) or U+2028 and U+2029.
bool takesMarks(char32_t cp) { return !(cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029); }

// The sender's handling of a .txt line, as the ascii.py sender script (1.32.2
// or later) does it: loading the file encodes each line again (compact:
// parse_irc, then encode_irc), and fit() encodes one still over the budget
// again in as few bytes as it can (parse_irc, then encode_irc_min). Both see
// a cluster (a base and the zero-width code points after it, any codes
// between them skipped) as one cell in its base's colours, whose glyph
// neither changes. Formatting is toggled per code as parse_irc toggles it,
// and a line with any on a cell, or with more than 2000 cells, gets
// encode_irc where encode_irc_min would run (ascii.py's fallback; the
// export's lines have at most kMaxColumns cells, so for them only the
// formatting half can apply). A colour is -1 where the line sets none (the
// client's own). It also models what the export never writes: ^D colours,
// tabs and other control bytes.
namespace sender {

constexpr int kNone = -1;
constexpr int kAny = -2;  // encode_irc_min: whatever the colour is now

// ascii.py's attribute bits and their codes, in ATTR_CODES order.
constexpr int kBold = 1, kItalic = 2, kUnderline = 4, kReverse = 8, kStrike = 16, kMono = 32;
constexpr std::pair<int, char> kAttrCodes[] = {{kBold, '\x02'},    {kItalic, '\x1d'}, {kUnderline, '\x1f'},
                                               {kReverse, '\x16'}, {kStrike, '\x1e'}, {kMono, '\x11'}};
// attributes that make a space's foreground colour visible
constexpr int kSpaceVisible = kUnderline | kReverse | kStrike;

struct SentCell {
    std::u32string ch;  // a cluster: its base, then its zero-width code points
    int fg, bg;
    int attrs = 0;
    bool is(char32_t c) const { return ch.size() == 1 && ch[0] == c; }
    // a space only shows its background
    bool space() const { return is(U' ') && !(attrs & kSpaceVisible); }
};

// Six hex digits at `i` as an RGB colour (advancing `i`); empty if there
// aren't six there.
std::optional<std::uint32_t> hexColour(std::string_view s, std::size_t& i) {
    if (s.size() - i < 6 || !std::all_of(s.begin() + i, s.begin() + i + 6, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
        return std::nullopt;
    const std::uint32_t rgb = std::stoul(std::string(s.substr(i, 6)), nullptr, 16);
    i += 6;
    return rgb;
}

// Python's str.isdigit(): a code point with a digit value (², not only 0-9).
bool pyDigit(char32_t ch) { return QChar::digitValue(ch) >= 0; }

std::string utf8(std::u32string_view cps) {
    std::string s;
    for (const char32_t cp : cps)
        appendUtf8(s, cp);
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

// parse_irc and cluster_cells, clustered as a reader on Unicode `unicode`
// does (zeroWidth). Each attribute code toggles its bit, a ^D colour is read
// as the nearest mIRC one, a tab is spaces to the next stop of eight and the
// other control bytes are dropped. A code between a cluster's base and a
// zero-width code point of it is read and the cluster kept whole.
std::vector<SentCell> parse(std::string_view line, int unicode) {
    std::vector<SentCell> cells;
    std::size_t unjoined = 0;  // parse_irc's cells, before cluster_cells joins them
    int fg = kNone, bg = kNone, attrs = 0;
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
        case '\x04': {
            // an RGB colour, as the nearest mIRC one
            ++i;
            const auto f = hexColour(line, i);
            if (!f) {
                fg = bg = kNone;
                break;
            }
            fg = nearestMirc(*f);
            if (i < line.size() && line[i] == ',') {
                std::size_t at = i + 1;
                if (const auto b = hexColour(line, at)) {
                    bg = nearestMirc(*b);
                    i = at;
                }
            }
            break;
        }
        case '\x0f':
            ++i;
            fg = bg = kNone;
            attrs = 0;
            break;
        case '\x02':
        case '\x11':
        case '\x16':
        case '\x1d':
        case '\x1e':
        case '\x1f':
            for (const auto& [bit, c] : kAttrCodes)
                if (c == line[i])
                    attrs ^= bit;
            ++i;
            break;
        case '\t':
            // spaces to the next stop of eight, counted in unjoined cells
            ++i;
            do {
                cells.push_back({U" ", fg, bg, attrs});
            } while (++unjoined % 8);
            break;
        default: {
            if (static_cast<unsigned char>(line[i]) < 0x20 || line[i] == '\x7f') {
                // the other controls are dropped
                ++i;
                break;
            }
            const char32_t cp = glyph(line, i);
            ++unjoined;
            if (!cells.empty() && zeroWidth(cp, unicode) && takesMarks(cells.back().ch[0]))
                cells.back().ch += cp;
            else
                cells.push_back({std::u32string(1, cp), fg, bg, attrs});
        }
        }
    }
    return cells;
}

// Whether a cell is one encode_irc drops at the end of a line.
bool blank(const SentCell& c) { return c.space() && c.bg == kNone; }

// encode_irc: as few codes as it can, a space's foreground left as it is.
std::string encode(const std::vector<SentCell>& cells) {
    std::size_t end = cells.size();
    while (end && blank(cells[end - 1]))
        --end;
    std::string out;
    int curFg = kNone, curBg = kNone, curAttrs = 0;
    for (std::size_t i = 0; i < end; ++i) {
        const SentCell& c = cells[i];
        if (c.attrs != curAttrs) {
            for (const auto& [bit, code] : kAttrCodes)
                if ((c.attrs ^ curAttrs) & bit)
                    out += code;
            curAttrs = c.attrs;
        }
        const std::string text = utf8(c.ch);
        const bool space = c.space();
        int wantFg = c.fg;
        if (space) {
            if (c.bg == curBg) {
                out += text;
                continue;
            }
            if (c.fg == kNone)
                wantFg = curFg;
        }
        if (wantFg == curFg && c.bg == curBg) {
            out += text;
            continue;
        }
        if ((wantFg == kNone && curFg != kNone) || (c.bg == kNone && curBg != kNone)) {
            out += '\x03';
            curFg = curBg = kNone;
            if (space)
                wantFg = kNone;
            if (wantFg == kNone && c.bg == kNone) {
                if (pyDigit(c.ch[0]) || c.ch[0] == U',')
                    out += "\x02\x02";
                out += text;
                continue;
            }
        }
        if (c.bg != kNone && c.bg != curBg) {
            out += colourCode(wantFg, c.bg);
            curFg = wantFg;
            curBg = c.bg;
        } else if (wantFg != curFg) {
            out += colourCode(wantFg);
            curFg = wantFg;
            if (c.ch[0] == U',')
                out += "\x02\x02";
        }
        out += text;
    }
    return out;
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
// drawn (a space or a full block of the same colour; a cluster has one way,
// its own glyph in its own colours). Kept in ascii.py's
// order throughout, so that of two encodings of one length it picks the one
// it picks, which decides where a cut falls. As ascii.py, a line with any
// attribute on a cell or more than 2000 cells is left to encode.
std::string encodeMin(const std::vector<SentCell>& cells) {
    if (cells.size() > 2000 || std::any_of(cells.begin(), cells.end(), [](const SentCell& c) { return c.attrs != 0; }))
        return encode(cells);
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
    while (end && blank(cells[end - 1]))
        --end;
    for (std::size_t i = 0; i < end; ++i) {
        const SentCell& c = cells[i];
        struct Option {
            std::u32string_view glyph;
            int fg, bg;
        };
        static constexpr char32_t kSpaceCp[] = {U' '}, kBlockCp[] = {kFullBlock};
        constexpr std::u32string_view space(kSpaceCp, 1), block(kBlockCp, 1);
        std::vector<Option> options;
        if (c.is(U' ')) {
            options.push_back({space, kAny, c.bg});
            options.push_back({space, c.fg, c.bg});
            if (c.bg != kNone)
                options.push_back({block, c.bg, kAny});
        } else if (c.is(kFullBlock) && c.fg != kNone) {
            options.push_back({block, c.fg, kAny});
            options.push_back({block, c.fg, c.bg});
            options.push_back({space, kAny, c.fg});
        } else {
            options.push_back({c.ch, c.fg, c.bg});
        }
        std::vector<Entry> next;
        for (std::size_t s = 0; s < states.size(); ++s)
            for (const Option& o : options) {
                auto [codes, after] = code(states[s].state, o.fg, o.bg, o.glyph[0]);
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

// The receivers of a .txt line: the ascii script's Python with each Unicode
// joinUnicode lists, and with a later one. Receivers whose joinMask bits
// agree on every code point of the line cluster it alike.
struct Receivers {
    int n = joinUnicodeCount() + 1;  // and a later one
    std::vector<std::pair<unsigned, char32_t>> varying;  // the line's masks other than none and all, each's first

    explicit Receivers(std::string_view line) {
        const unsigned all = (1u << n) - 1;
        for (std::size_t i = 0; i < line.size();) {
            const char32_t cp = glyph(line, i);
            if (const unsigned m = joinMask(cp);
                m && m != all && std::none_of(varying.begin(), varying.end(), [m](const auto& v) { return v.first == m; }))
                varying.push_back({m, cp});
        }
    }
    static bool bit(unsigned m, int r) { return (m >> r & 1) != 0; }
    bool alike(int a, int b) const {
        return std::all_of(varying.begin(), varying.end(), [&](const auto& v) { return bit(v.first, a) == bit(v.first, b); });
    }
    // The first receiver of each way of clustering the line.
    std::vector<int> distinct() const {
        std::vector<int> out;
        for (int r = 0; r < n; ++r)
            if (std::none_of(out.begin(), out.end(), [&](int q) { return alike(q, r); }))
                out.push_back(r);
        return out;
    }
};

} // namespace

std::vector<ShownCell> decodeLine(std::string_view line, const DecodeOptions& opts, DecodeLoss* loss) {
    std::vector<ShownCell> out;
    std::uint8_t fg = kClientColour, bg = kClientColour, attrs = 0;
    bool reverse = false;
    int col = 0;  // the cursor's column, opts.columns when a wrap is pending
    // The last cluster written, while a zero-width code point may still join
    // it, and its cell (its lead, for a pair).
    ClusterReader cluster(opts.poolCap);
    std::size_t at = 0;
    const auto finish = [&] {
        bool refused = false;
        if (const std::optional<Glyph> g = cluster.finish(&refused)) {
            out[at].cell.ch = *g;
        } else if (refused && loss) {
            // past the pool's cap: the cell keeps its base
            ++loss->poolRefused;
            if (loss->firstRefused < 0)
                loss->firstRefused = int(at);
        }
    };
    const auto put = [&](Glyph g, std::size_t end) {
        MircCell c{g, fg, bg, attrs};
        if (reverse)
            std::swap(c.fg, c.bg);
        out.push_back({c, end});
    };
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
            attrs = 0;
            reverse = false;
            break;
        case '\x16':
            ++i;
            reverse = !reverse;
            break;
        case '\x02':
            ++i;
            attrs ^= attr::Bold;
            break;
        case '\x1d':
            ++i;
            attrs ^= attr::Italic;
            break;
        case '\x1f':
            ++i;
            attrs ^= attr::Underline;
            break;
        case '\x11':
        case '\x1e':
            ++i;
            break;
        default: {
            const std::size_t start = i;
            char32_t ch = glyph(line, i);
            int width = codePointWidth(ch);
            if (width == 0 && cluster.open()) {
                // a zero-width code point joins the cluster before it
                if (!cluster.join(ch) && loss) {
                    ++loss->marksDropped;
                    if (loss->firstDropped < 0)
                        loss->firstDropped = int(at);
                }
                // a pair's lead and continuation both end after it
                for (std::size_t k = at; k < out.size(); ++k)
                    out[k].end = i;
                break;
            }
            finish();
            if (width == 2 && opts.columns == 1) {
                ch = U'\uFFFD';
                width = 1;
            }
            const int columns = std::max(width, 1);
            if (opts.columns > 0) {
                if (col >= opts.columns)
                    col = 0;
                if (col + columns > opts.columns) {
                    // a wide cluster on the last column wraps first, leaving
                    // that column blank
                    out.push_back({MircCell{U' ', kClientColour, kClientColour}, start});
                    col = 0;
                }
            }
            col += columns;
            at = out.size();
            put(ch, i);
            if (columns == 2)
                put(Glyph::continuation(), i);
            // a control (or NUL) is never joined
            cluster.start(ch);
            break;
        }
        }
    }
    finish();
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
        // laid out in the canvas's columns, as a client as wide as the art
        // lays it out: a lone wide cell takes two, and pushes the rest right
        const DecodeOptions layout{canvas.width()};
        const std::vector<ShownCell> cells = decodeLine(line, layout);
        row.kept = static_cast<int>(std::min(cells.size(), std::size_t(canvas.width())));
        // the cells the sender carries, counted past the view's width too: a
        // line of lone wide cells lays out wider than the canvas, and a cut
        // past the width the view shows is a cut all the same
        int sent = static_cast<int>(cells.size());
        // The ascii script loads a .txt line by reading it into cells and
        // writing them again (compact: encode_irc, two-digit colour codes),
        // so it fits that, not the line as written. Which code points it
        // joins depends on its Python's Unicode: the row shows the fewest
        // cells any of them keeps, as Publish refuses a line any of them cuts.
        for (const int r : Receivers(line).distinct()) {
            const std::vector<sender::SentCell> parsed = sender::parse(line, r);
            if (sender::sentSize(sender::encode(parsed)) <= budget)
                continue;
            // over the budget, the sender encodes the line again in as few
            // bytes as it can, and cuts that if it is still over, keeping a
            // byte for the reset it ends the line with. The encoding again can
            // be the shorter one and fit: a padded line ends on a
            // black-on-black █ that the sender sends as a space.
            const std::string again = sender::encodeMin(parsed);
            if (sender::sentSize(again) <= budget)
                continue;
            const std::size_t limit = budget == 0 ? 0 : budget - 1;
            // the sender cuts between clusters, dropping one that doesn't
            // fit whole, and a cell is carried when its cluster is: a pair's
            // halves end together, after the cluster's last byte
            const std::vector<ShownCell> carried = decodeLine(again, layout);
            sent = std::min(sent, static_cast<int>(std::count_if(carried.begin(), carried.end(), [limit](const ShownCell& s) {
                                return s.end <= limit;
                            })));
        }
        row.lost = sent < static_cast<int>(cells.size());
        // the cells are drawn as the export has them: the encodings again
        // only trade a space for a full block of the same colour, and keep a
        // cluster in its base's colours, as a client draws it from the export
        for (const ShownCell& s : cells) {
            if (row.shown >= sent || row.shown >= canvas.width())
                break;
            Cell& c = v.canvas.at(row.shown++, y);
            c.ch = s.cell.ch;
            c.fg = shownColour(s.cell.fg, kClientForeground);
            c.bg = shownColour(s.cell.bg, kClientBackground);
            c.attrs = s.cell.attrs;
        }
    }
    return v;
}

namespace {

// What the ascii script sends of a .txt line when its Python's Unicode is
// joinUnicode(unicode) (txtSentBytes).
std::size_t sentAt(std::string_view line, std::size_t budget, int unicode) {
    const std::vector<sender::SentCell> cells = sender::parse(line, unicode);
    const std::size_t loaded = sender::sentSize(sender::encode(cells));
    if (loaded <= budget)
        return loaded;
    return std::min(loaded, sender::sentSize(sender::encodeMin(cells)));
}

std::string codePoint(char32_t cp) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "U+%04X", static_cast<unsigned>(cp));
    return buf;
}

struct TxtSize {
    std::size_t bytes;
    std::string where;
};

// joinUnicode(i), or a later version than the table's.
std::string unicodeName(int i) { return joinUnicode(i) ? joinUnicode(i) : "a later one"; }

// The line's size under each Unicode a supported Python can have, once for
// each way of clustering the line they make (receivers whose joinMask bits
// agree on every code point of the line cluster it alike). Its size is the
// largest; where only some of them have it over `budget`, `where` names them
// and a code point that sets them apart.
TxtSize txtSize(std::string_view line, std::size_t budget) {
    const Receivers receivers(line);
    const int n = receivers.n;
    const auto& varying = receivers.varying;
    const auto bit = Receivers::bit;
    const auto alike = [&](int a, int b) { return receivers.alike(a, b); };
    std::size_t worst = 0;
    int at = 0;
    bool fits = false;
    for (const int r : receivers.distinct()) {
        const std::size_t bytes = sentAt(line, budget, r);
        fits = fits || bytes <= budget;
        if (bytes > worst) {
            worst = bytes;
            at = r;
        }
    }
    TxtSize out{worst, {}};
    if (worst <= budget || !fits)
        return out;
    std::vector<int> members;
    for (int r = 0; r < n; ++r)
        if (alike(at, r))
            members.push_back(r);
    // a code point the receiver just past them clusters otherwise, or else
    // the one just before them (they are not all, as one fits)
    const int other = members.back() + 1 < n ? members.back() + 1 : members.front() - 1;
    const auto [mask, cp] = *std::find_if(varying.begin(), varying.end(),
                                          [&](const auto& v) { return bit(v.first, at) != bit(v.first, other); });
    const bool joins = bit(mask, at);
    const bool contiguous = members.back() - members.front() + 1 == static_cast<int>(members.size());
    if (members == std::vector<int>{n - 1}) {
        out.where = "where the script's Python has a Unicode later than the width table's that makes " + codePoint(cp) +
                    " a mark, which joins it to the character before it";
        return out;
    }
    if (contiguous && members.front() == 0 && members.back() == n - 2) {
        out.where = "where the script's Python has a Unicode that has no mark at " + codePoint(cp) +
                    ", which sends it as a character of its own";
        return out;
    }
    std::string which;
    if (!contiguous) {
        for (std::size_t k = 0; k < members.size(); ++k)
            which += (k ? k + 1 == members.size() ? " or " : ", " : "") + unicodeName(members[k]);
    } else if (members.front() == 0) {
        which = unicodeName(members.back()) + " or older";
    } else if (members.back() == n - 1) {
        which = unicodeName(members.front()) + " or later";
    } else {
        which = unicodeName(members.front()) + " to " + unicodeName(members.back());
    }
    out.where = "where the script's Python has Unicode " + which + ", which " +
                (joins ? "joins " + codePoint(cp) + " to the character before it"
                       : "sends " + codePoint(cp) + " as a character of its own");
    return out;
}

} // namespace

std::size_t txtSentBytes(std::string_view line, std::size_t budget) { return txtSize(line, budget).bytes; }

std::vector<TxtLineSize> txtOverBudget(const std::vector<std::string>& lines, std::size_t budget) {
    std::vector<TxtLineSize> out;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (TxtSize t = txtSize(lines[i], budget); t.bytes > budget)
            out.push_back({static_cast<int>(i) + 1, t.bytes, std::move(t.where)});
    return out;
}

} // namespace bw::irc
