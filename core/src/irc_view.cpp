#include "blockwright/irc_view.h"

#include "blockwright/cp437.h"
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

// A code point a client draws on the one before it, joining its cluster:
// the width table's 0 (ascii.py's zero_width is the same rule, with Mn and Me
// from its Python's Unicode, so it lacks the marks assigned since).
bool zeroWidth(char32_t cp) { return codePointWidth(cp) == 0; }

// Whether zero-width code points after `cp` join it (ascii.py's takes_marks):
// not after a control (C0, NUL included, DEL, C1) or U+2028 and U+2029.
bool takesMarks(char32_t cp) { return !(cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029); }

// The sender's handling of a line, as the ascii.py sender script (1.32.2 or
// later) does it in fit(): a line with a code inside a cluster is encoded
// again first (parse_irc, then encode_irc), and one still over the budget
// again in as few bytes as it can (parse_irc, then encode_irc_min). Both see
// a cluster (a base and the zero-width code points after it, any codes
// between them skipped) as one cell in its base's colours, whose glyph
// neither changes. (ascii.py falls back on a plainer encoding for a line of
// more than 2000 cells or one with attributes; the export's lines have at
// most kMaxColumns cells, and their only attributes are the ^B pairs that
// cancel out.) A colour is -1 where the line sets none (the client's own).
namespace sender {

constexpr int kNone = -1;
constexpr int kAny = -2;  // encode_irc_min: whatever the colour is now

struct SentCell {
    std::u32string ch;  // a cluster: its base, then its zero-width code points
    int fg, bg;
    bool is(char32_t c) const { return ch.size() == 1 && ch[0] == c; }
};

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

// parse_irc and cluster_cells, for the export's lines: no ^D colours, tabs
// or other control bytes, and attributes that leave the cells as they are.
// `codeInCluster` is set when a code sits between a cluster's base and a
// zero-width code point of it (code_in_cluster).
std::vector<SentCell> parse(std::string_view line, bool* codeInCluster = nullptr) {
    std::vector<SentCell> cells;
    int fg = kNone, bg = kNone;
    bool code = false;  // a code since the last code point
    auto mirc = [](std::uint8_t n) { return n < 99 ? int(n) : kNone; };
    std::size_t i = 0;
    while (i < line.size()) {
        switch (line[i]) {
        case '\x03': {
            ++i;
            code = true;
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
            code = true;
            fg = bg = kNone;
            break;
        case '\x02':
        case '\x11':
        case '\x16':
        case '\x1d':
        case '\x1e':
        case '\x1f':
            ++i;
            code = true;
            break;
        default: {
            const char32_t cp = glyph(line, i);
            if (!cells.empty() && zeroWidth(cp) && takesMarks(cells.back().ch[0])) {
                cells.back().ch += cp;
                if (code && codeInCluster)
                    *codeInCluster = true;
            } else {
                cells.push_back({std::u32string(1, cp), fg, bg});
            }
            code = false;
        }
        }
    }
    return cells;
}

// Whether a cell is one encode_irc drops at the end of a line.
bool blank(const SentCell& c) { return c.is(U' ') && c.bg == kNone; }

// encode_irc: as few codes as it can, a space's foreground left as it is.
std::string encode(const std::vector<SentCell>& cells) {
    std::size_t end = cells.size();
    while (end && blank(cells[end - 1]))
        --end;
    std::string out;
    int curFg = kNone, curBg = kNone;
    for (std::size_t i = 0; i < end; ++i) {
        const SentCell& c = cells[i];
        const std::string text = utf8(c.ch);
        const bool space = c.is(U' ');
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

} // namespace

std::vector<ShownCell> decodeLine(std::string_view line, const DecodeOptions& opts, DecodeLoss* loss) {
    std::vector<ShownCell> out;
    std::uint8_t fg = kClientColour, bg = kClientColour;
    bool reverse = false;
    int col = 0;  // the cursor's column, opts.columns when a wrap is pending
    // The last cluster written, while a zero-width code point may still join
    // it: its code points, and its cell (its lead, for a pair). It goes into
    // the pool once, when it is done, so no prefix of it is interned.
    std::vector<char32_t> cluster;
    std::size_t at = 0;
    const auto finish = [&] {
        if (cluster.size() > 1) {
            if (const std::optional<Glyph> g = clusterPool().intern(cluster, opts.poolCap)) {
                out[at].cell.ch = *g;
            } else if (loss) {
                // past the pool's cap: the cell keeps its base
                ++loss->poolRefused;
                if (loss->firstRefused < 0)
                    loss->firstRefused = int(at);
            }
        }
        cluster.clear();
    };
    const auto put = [&](Glyph g, std::size_t end) {
        MircCell c{g, fg, bg};
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
            const std::size_t start = i;
            char32_t ch = glyph(line, i);
            int width = codePointWidth(ch);
            if (width == 0 && !cluster.empty()) {
                // a zero-width code point joins the cluster before it
                if (cluster.size() < kMaxClusterCodePoints) {
                    cluster.push_back(ch);
                } else if (loss) {
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
            if (takesMarks(ch))
                cluster.push_back(ch);
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
        int sent = row.kept;
        // a code inside a cluster (a lone zero-width cell after a cell in
        // other colours) has the sender encode the line again first
        bool codeInCluster = false;
        const std::vector<sender::SentCell> parsed = sender::parse(line, &codeInCluster);
        const std::string sentLine = codeInCluster ? sender::encode(parsed) : line;
        if (sender::sentSize(sentLine) > budget) {
            // over the budget, the sender encodes the line again in as few
            // bytes as it can, and cuts that if it is still over, keeping a
            // byte for the reset it ends the line with. The encoding again can
            // be the shorter one and fit: a padded line ends on a
            // black-on-black █ that the sender sends as a space.
            const std::string again = sender::encodeMin(parsed);
            if (sender::sentSize(again) > budget) {
                const std::size_t limit = budget == 0 ? 0 : budget - 1;
                // the sender cuts between clusters, dropping one that doesn't
                // fit whole, and a cell is carried when its cluster is: a
                // pair's halves end together, after the cluster's last byte
                const std::vector<ShownCell> carried = decodeLine(again, layout);
                sent = std::min(row.kept, static_cast<int>(std::count_if(carried.begin(), carried.end(), [limit](const ShownCell& s) {
                                                          return s.end <= limit;
                                                      })));
            }
        }
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
        }
    }
    return v;
}

} // namespace bw::irc
