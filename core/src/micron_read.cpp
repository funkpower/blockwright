// Reading a Micron page (.mu) into cells as NomadNet 1.5 draws it: the
// browser's strip_modifiers and page headers (nomadnet/util.py,
// nomadnet/ui/textui/Browser.py) and MicronParser.markup_to_attrmaps with
// its dark theme, laid out the way urwid lays out the widgets it builds
// (PLAN §17, Opening a page).

#include "blockwright/micron.h"

#include "blockwright/cp437.h"
#include "blockwright/width.h"

#include <QChar>

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace bw::micron {

namespace {

using Text = std::u32string;
using View = std::u32string_view;
constexpr std::size_t npos = Text::npos;

constexpr int kSectionIndent = 2;  // MicronParser.SECTION_INDENT
constexpr char32_t kDivider = U'─';
// DEFAULT_FOLD_GLYPHS: open, collapsed
constexpr char32_t kFoldOpen[] = U"▾";
constexpr char32_t kFoldClosed[] = U"▸";

// urwid's 24-bit colour for "g00" to "g99" (display/common.py
// _parse_color_true), each a grey: the level of all three channels.
constexpr std::uint8_t kGrey[100] = {
    0x00, 0x00, 0x08, 0x08, 0x08, 0x12, 0x12, 0x12, 0x12, 0x1c, 0x1c, 0x1c, 0x1c, 0x26, 0x26, 0x26, 0x26,
    0x30, 0x30, 0x30, 0x30, 0x3a, 0x3a, 0x3a, 0x3a, 0x44, 0x44, 0x44, 0x44, 0x4e, 0x4e, 0x4e, 0x4e, 0x58,
    0x58, 0x58, 0x58, 0x62, 0x62, 0x62, 0x62, 0x6c, 0x6c, 0x6c, 0x6c, 0x76, 0x76, 0x76, 0x76, 0x80, 0x80,
    0x84, 0x84, 0x84, 0x84, 0x94, 0x94, 0x94, 0x94, 0x94, 0x9e, 0x9e, 0x9e, 0x9e, 0xa8, 0xa8, 0xa8, 0xa8,
    0xb2, 0xb2, 0xb2, 0xb2, 0xbc, 0xbc, 0xbc, 0xbc, 0xc6, 0xc6, 0xc6, 0xc6, 0xd0, 0xd0, 0xd0, 0xd0, 0xda,
    0xda, 0xda, 0xda, 0xe4, 0xe4, 0xe4, 0xe4, 0xee, 0xee, 0xee, 0xee, 0xee, 0xff, 0xff, 0xff};

// A colour as the parser's state holds it: the text after `F or `B (or a
// header's value), "default", or a theme colour. make_style reads it.
struct Style {
    Text fg, bg;
    bool bold = false, underline = false, italic = false;
};

// STYLES_DARK's headings: depth 1, 2 and 3 (and deeper).
Style headingStyle(int depth) {
    switch (std::min(depth, 3)) {
    case 1:
        return {U"222", U"bbb"};
    case 2:
        return {U"111", U"999"};
    default:
        return {U"000", U"777"};
    }
}

// The value Python's int(c, base) gives one character: an ASCII digit or
// hex letter, or any Unicode decimal digit; -1 for anything else.
int digitValue(char32_t c, int base) {
    if (c >= U'0' && c <= U'9')
        return int(c - U'0');
    if (base == 16 && c >= U'a' && c <= U'f')
        return int(c - U'a') + 10;
    if (base == 16 && c >= U'A' && c <= U'F')
        return int(c - U'A') + 10;
    if (c > 0x7f && c <= 0x10ffff && QChar::category(c) == QChar::Number_DecimalDigit)
        return QChar::digitValue(c);
    return -1;
}

// make_style's high_color, as urwid then draws it in 24-bit mode: six hex
// digits as they are, three doubled, "gXY" urwid's grey XY; nothing (the
// terminal's own colour) for "default" or anything malformed.
std::optional<Rgb> highColour(View c) {
    if (c.size() == 6) {
        int v[6];
        for (int i = 0; i < 6; ++i)
            if ((v[i] = digitValue(c[std::size_t(i)], 16)) < 0)
                return std::nullopt;
        return Rgb{std::uint8_t(v[0] * 16 + v[1]), std::uint8_t(v[2] * 16 + v[3]), std::uint8_t(v[4] * 16 + v[5])};
    }
    if (c.size() == 3) {
        if (c[0] == U'g') {
            const int a = digitValue(c[1], 10), b = digitValue(c[2], 10);
            if (a < 0 || b < 0)
                return std::nullopt;
            const std::uint8_t l = kGrey[a * 10 + b];
            return Rgb{l, l, l};
        }
        int v[3];
        for (int i = 0; i < 3; ++i)
            if ((v[i] = digitValue(c[std::size_t(i)], 16)) < 0)
                return std::nullopt;
        return twelveBit(v[0], v[1], v[2]);
    }
    return std::nullopt;
}

// A colour as a cell holds it: one of kPalette as its index, the
// terminal's own as Grey or Black, any other as RGB.
Color cellColour(View c, bool fg) {
    const std::optional<Rgb> rgb = highColour(c);
    if (!rgb)
        return Color::indexed(fg ? 7 : 0);
    for (std::size_t i = 0; i < kPalette.size(); ++i)
        if (kPalette[i] == *rgb)
            return Color::indexed(std::uint8_t(i));
    return Color::fromRgb(*rgb);
}

Cell styledCell(char32_t cp, const Style& s) {
    Cell c;
    c.ch = Glyph(cp);
    c.fg = cellColour(s.fg, true);
    c.bg = cellColour(s.bg, false);
    c.attrs = std::uint8_t((s.bold ? attr::Bold : 0) | (s.italic ? attr::Italic : 0) |
                           (s.underline ? attr::Underline : 0));
    return c;
}

// util.STRIP_CONTROL_RE
bool strippedControl(char32_t cp) {
    return cp <= 0x08 || cp == 0x0b || cp == 0x0c || (cp >= 0x0e && cp <= 0x1f) || (cp >= 0x7f && cp <= 0x9f) ||
           (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
           cp == 0xfeff || (cp >= 0xfff0 && cp <= 0xfff8);
}

// What strip_modifiers takes out before its control pass: marks, format
// characters, variation selectors, emoji skin tones.
bool strippedModifier(char32_t cp) {
    if ((cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef) || (cp >= 0x1f3fb && cp <= 0x1f3ff))
        return true;
    switch (QChar::category(cp)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
    case QChar::Other_Format:
        return true;
    default:
        return false;
    }
}

// Python's str.isspace for what strip_modifiers leaves.
bool pySpace(char32_t cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\n' || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

// util.strip_modifiers, step by step: U+261D and the detective emoji as a
// space, modifiers out, "\r\n" and a lone "\r" as "\n", controls out, then
// the whitespace at either end of the page. `lead` is set to the newlines
// the start lost, so a line can be named by its number in the file.
Text stripModifiers(Text a, int& lead) {
    // each pass writes no further than it has read, so the text is edited
    // where it is
    std::size_t j = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char32_t cp = a[i];
        if (cp == 0x261d || (cp == 0x1f575 && i + 1 < a.size() && a[i + 1] == 0xfe0f)) {
            a[j++] = U' ';
            i += cp == 0x1f575;
        } else if (!strippedModifier(cp)) {
            a[j++] = cp;
        }
    }
    a.resize(j);
    j = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == U'\r') {
            a[j++] = U'\n';
            i += i + 1 < a.size() && a[i + 1] == U'\n';
        } else if (!strippedControl(a[i])) {
            a[j++] = a[i];
        }
    }
    a.resize(j);
    std::size_t s = 0, e = a.size();
    while (s < e && pySpace(a[s]))
        ++s;
    while (e > s && pySpace(a[e - 1]))
        --e;
    lead = int(std::count(a.begin(), a.begin() + std::ptrdiff_t(s), U'\n'));
    a.resize(e);
    a.erase(0, s);
    return a;
}

bool startsWith(View s, View p) { return s.substr(0, p.size()) == p; }

std::vector<View> split(View s, char32_t sep) {
    std::vector<View> out;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find(sep, from);
        out.push_back(s.substr(from, at == npos ? npos : at - from));
        if (at == npos)
            return out;
        from = at + 1;
    }
}

// Python's int(s) of a field's width or rows: optional whitespace and sign,
// digits (single underscores between them); nothing if it isn't one.
std::optional<long> pyInt(View s) {
    while (!s.empty() && pySpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && pySpace(s.back()))
        s.remove_suffix(1);
    bool neg = false;
    if (!s.empty() && (s[0] == U'+' || s[0] == U'-')) {
        neg = s[0] == U'-';
        s.remove_prefix(1);
    }
    if (s.empty() || s.front() == U'_' || s.back() == U'_')
        return std::nullopt;
    long v = 0;
    char32_t prev = 0;
    for (const char32_t c : s) {
        if (c == U'_') {
            if (prev == U'_')
                return std::nullopt;
        } else {
            const int d = digitValue(c, 10);
            if (d < 0)
                return std::nullopt;
            v = std::min(v * 10 + d, 1L << 40);
        }
        prev = c;
    }
    return neg ? -v : v;
}

// Python's float(s) of a partial's refresh time: whether it is one
// (whitespace around it, a sign, digits with single underscores between
// them, a point, an exponent; or inf, infinity, nan).
bool pyFloat(View s) {
    while (!s.empty() && pySpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && pySpace(s.back()))
        s.remove_suffix(1);
    if (!s.empty() && (s[0] == U'+' || s[0] == U'-'))
        s.remove_prefix(1);
    Text lower;
    for (const char32_t c : s)
        lower += c < 0x80 ? char32_t(std::tolower(int(c))) : c;
    if (lower == U"inf" || lower == U"infinity" || lower == U"nan")
        return true;
    std::size_t i = 0;
    // digits, a single underscore between two of them: how many there were
    const auto digits = [&] {
        std::size_t count = 0;
        while (i < s.size()) {
            if (digitValue(s[i], 10) >= 0) {
                ++count;
                ++i;
            } else if (s[i] == U'_' && count > 0 && i + 1 < s.size() && digitValue(s[i + 1], 10) >= 0) {
                ++i;
            } else {
                break;
            }
        }
        return count;
    };
    std::size_t mantissa = digits();
    if (i < s.size() && s[i] == U'.') {
        ++i;
        mantissa += digits();
    }
    if (mantissa == 0)
        return false;
    if (i < s.size() && (s[i] == U'e' || s[i] == U'E')) {
        ++i;
        if (i < s.size() && (s[i] == U'+' || s[i] == U'-'))
            ++i;
        if (digits() == 0)
            return false;
    }
    return i == s.size();
}

// The cells a code point takes as urwid lays it out, a zero-width one none.
// A tab, the page's only control left, takes at least one: a row's tabs are
// set out at their stops (expandTabs) once the row is built.
int cellWidth(char32_t cp) {
    if (cp == U'\t')
        return 1;
    return std::max(0, codePointWidth(cp));
}

int textWidth(View t) {
    int w = 0;
    for (const char32_t cp : t)
        w += cellWidth(cp);
    return w;
}

// `s` with each tab as the blanks to the next multiple of 8 columns, as
// urwid's Text lays one out, `col` the column `s` starts at within its
// widget (moved past `s`). Nothing is kept once the text is past
// kMaxColumns, which no row draws.
Text detab(View s, int& col) {
    Text out;
    for (const char32_t cp : s) {
        if (col > kMaxColumns)
            break;
        if (cp == U'\t') {
            const int n = 8 - col % 8;
            out.append(std::size_t(n), U' ');
            col += n;
        } else if (const int w = cellWidth(cp); w > 0) {
            out += cp;
            col += w;
        }
    }
    return out;
}

// `t` without its zero-width code points (no row draws them) and cut once
// it is past kMaxColumns, a tab counted as one column.
void capText(Text& t) {
    int w = 0;
    std::size_t j = 0;
    for (std::size_t i = 0; i < t.size() && w <= kMaxColumns; ++i) {
        const int cw = cellWidth(t[i]);
        if (cw > 0) {
            t[j++] = t[i];
            w += cw;
        }
    }
    t.resize(j);
}

// The next `c` in `s` at or after a position. It searches again only when
// asked past the one it found, so a line is read once however many codes
// ask in turn.
struct Next {
    View s;
    char32_t c;
    std::size_t from = npos, at = npos;
    std::size_t operator()(std::size_t f) {
        if (f >= s.size())
            return npos;
        if (from == npos || f < from || (at != npos && at < f)) {
            at = s.find(c, f);
            from = f;
        }
        return at;
    }
};

// Calls `f` with each line of `s` (split at '\n', the last one possibly
// empty), none of them copied.
template <class F>
void forEachLine(View s, F&& f) {
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find(U'\n', from);
        f(s.substr(from, at == npos ? npos : at - from));
        if (at == npos)
            return;
        from = at + 1;
    }
}

// parse_line's prefixes, taken off `line`: a collapsible heading's "`+" or
// "`-", the '>'s of a line with a field, an escape. False for a comment.
bool prefixes(View& line, bool& collapsible, bool& collapsed, bool& preEscape) {
    if ((startsWith(line, U"`+") || startsWith(line, U"`-")) && line.size() > 2 && line[2] == U'>') {
        collapsible = true;
        collapsed = line[1] == U'-';
        line.remove_prefix(2);
    }
    if (line[0] == U'>' && line.find(U"`<") != npos)
        line.remove_prefix(line.find_first_not_of(U'>'));  // a line with a field is never a heading
    if (line[0] == U'\\') {
        line.remove_prefix(1);
        preEscape = true;
    } else if (line[0] == U'#') {
        return false;
    }
    return true;
}

enum class Align { Left, Centre, Right };

struct Seg {
    Text text;
    Style style;
};

struct Row {
    enum class Kind { Text, Heading, Divider, Empty } kind = Kind::Empty;
    std::vector<Seg> segs;
    // The row's own attribute: the end-of-line style (the AttrMap
    // markup_to_attrmaps wraps a row in), a heading's first part's.
    Style fill;
    char32_t divider = kDivider;
    int indent = 0;  // a Padding's left and right (a heading's is in its text)
    Align align = Align::Left;
    int line = 0;
};

// A row's tabs set out at their stops. urwid counts a tab's column from the
// start of the Text widget it is in: the whole row (a section's indent is
// outside it, a heading's inside), or in a row with a field, laid out as
// form columns, each part on its own.
void expandTabs(std::vector<Seg>& segs, bool eachOwn) {
    int col = 0, total = 0;
    for (std::size_t k = 0; k < segs.size(); ++k) {
        if (total > kMaxColumns) {
            segs.resize(k);
            return;
        }
        if (eachOwn)
            col = 0;
        Text& t = segs[k].text;
        const int start = col;
        if (t.find(U'\t') == npos)
            col += textWidth(t);
        else
            t = detab(t, col);
        total += col - start;
    }
}

// The rows a page draws: the first kMaxRows kept, the rest only counted as
// they come, so a page past the limit holds no more than one at it.
struct Rows {
    std::vector<Row> kept;
    MicronLoss& past;
    std::size_t widest = 0;  // code points in the longest row built

    void push(Row&& r) {
        std::size_t w = 0;
        for (const Seg& s : r.segs)
            w += s.text.size();
        widest = std::max(widest, w);
        if (kept.size() < std::size_t(kMaxRows))
            kept.push_back(std::move(r));
        else
            past.add(r.line);
    }
};

struct State {
    bool literal = false;
    bool table = false;
    int tableLine = 0;
    int depth = 0;
    Style style;
    Text defaultFg, defaultBg;
    Align align = Align::Left;  // default_align is always left
    Text foldOpen = kFoldOpen, foldClosed = kFoldClosed;
};

class Reader {
public:
    // `page` is the page's text the lines passed to line() are views of.
    Reader(State& st, MicronReadLoss& loss, View page) : st_(st), loss_(loss), page_(page) {}

    // MicronParser.make_output: the line's parts, each in the style it was
    // drawn in, or none for a line that draws nothing. `field` is set if a
    // field is among them (the line is laid out as form columns). A part
    // keeps no more than the row can draw: nothing once the row is past
    // kMaxColumns, and no zero-width code point.
    std::vector<Seg> output(View line, bool preEscape, int lineNo, bool& field) {
        std::vector<Seg> out;
        field = false;
        used_ = 0;
        if (st_.literal) {
            putPart(out, line == U"\\`=" ? View(U"`=") : line);
            return out;
        }
        const std::size_t n = line.size();
        Next nextClose{line, U']'}, nextTick{line, U'`'}, nextGt{line, U'>'};

        Text part;
        bool seen = false;  // a code point went into the part, kept or not
        const auto flush = [&] {
            if (!part.empty() || (seen && out.empty()))
                out.push_back({part, st_.style});
            part.clear();
            seen = false;
        };
        bool formatting = false;
        bool escape = preEscape;
        std::size_t skip = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char32_t c = line[i];
            if (skip > 0) {
                --skip;
                continue;
            }
            if (!formatting) {
                if (c == U'\\' && !escape) {
                    escape = true;
                } else if (c == U'`' && !escape) {
                    formatting = true;
                    flush();
                } else {
                    put(part, c);
                    seen = true;
                    escape = false;
                }
                continue;
            }
            switch (c) {
            case U'_':
                st_.style.underline = !st_.style.underline;
                break;
            case U'!':
                st_.style.bold = !st_.style.bold;
                break;
            case U'*':
                st_.style.italic = !st_.style.italic;
                break;
            case U'F':
            case U'B':
                if (n >= i + 4) {
                    Text& colour = c == U'F' ? st_.style.fg : st_.style.bg;
                    if (line[i + 1] == U'T' && n >= i + 8) {
                        colour = Text(line.substr(i + 2, 6));
                        skip = 7;
                    } else {
                        colour = Text(line.substr(i + 1, 3));
                        skip = 3;
                    }
                }
                break;
            case U'f':
                st_.style.fg = st_.defaultFg;
                break;
            case U'b':
                st_.style.bg = st_.defaultBg;
                break;
            case U'`':
                st_.style = {st_.defaultFg, st_.defaultBg};
                st_.align = Align::Left;
                break;
            case U'c':
                st_.align = Align::Centre;
                break;
            case U'l':
            case U'a':
                st_.align = Align::Left;
                break;
            case U'r':
                st_.align = Align::Right;
                break;
            case U':': {
                // an anchor: a name of letters, digits, _ and -, drawing nothing
                std::size_t end = i + 1;
                while (end < n && (QChar::isLetterOrNumber(line[end]) || line[end] == U'_' || line[end] == U'-'))
                    ++end;
                skip = end - i - 1;
                break;
            }
            case U'<':
                field = readField(line, i, nextTick(i + 1), nextGt, out, skip, lineNo) || field;
                break;
            case U'[': {
                const std::size_t end = nextClose(i);
                if (end == npos)
                    break;
                const View data = line.substr(i + 1, end - i - 1);
                skip = end - i;
                const std::vector<View> parts = split(data, U'`');
                View label, url;
                if (parts.size() == 1) {
                    url = data;
                } else if (parts.size() == 2 || parts.size() == 3) {
                    label = parts[0];
                    url = parts[1];
                }
                if (!url.empty()) {
                    putPart(out, label.empty() ? url : label);
                    loss_.links.add(lineNo);
                }
                break;
            }
            default:
                break;
            }
            formatting = false;
        }
        flush();
        return out;
    }

    // MicronParser.parse_line: adds the row the line draws, if any (a
    // table's closing line adds the table's), and keeps the state.
    void line(View line, int lineNo, Rows& rows) {
        // where the line is in the page, before any prefix comes off it
        const std::size_t start = std::size_t(line.data() - page_.data()), end = start + line.size();
        bool preEscape = false;
        bool collapsible = false, collapsed = false;
        for (;;) {
            if (line.empty())
                return;
            if (line == U"`=") {
                st_.literal = !st_.literal;
                return;
            }
            if (!st_.literal) {
                if (!prefixes(line, collapsible, collapsed, preEscape)) {
                    foldGlyphs(line);
                    return;
                }
                if (startsWith(line, U"`t")) {
                    if (st_.table) {
                        endTable(rows, start);
                    } else {
                        st_.table = true;
                        st_.tableLine = lineNo;
                        tableFrom_ = end + 1;
                        tableLines_ = 0;
                    }
                    return;
                }
                if (st_.table) {
                    // read again from the page when the table ends
                    ++tableLines_;
                    return;
                }
                if (startsWith(line, U"`{")) {
                    if (isPartial(line.substr(2)))
                        loss_.partials.add(lineNo);
                    return;
                }
                if (startsWith(line, U"`(")) {
                    if (isImage(line.substr(2)))
                        loss_.images.add(lineNo);
                    return;
                }
                // an escaped line is never a reset, a heading or a divider
                const char32_t first = preEscape ? U'\\' : line[0];
                if (first == U'<') {
                    // the section reset, then the rest of the line afresh
                    st_.depth = 0;
                    line.remove_prefix(1);
                    preEscape = collapsible = collapsed = false;
                    continue;
                }
                if (first == U'>') {
                    heading(line, lineNo, collapsible, collapsed, rows);
                    return;
                }
                if (first == U'-') {
                    Row r;
                    r.kind = Row::Kind::Divider;
                    if (line.size() == 2 && line[1] >= 32 && codePointWidth(line[1]) == 1)
                        r.divider = line[1];
                    r.fill = st_.style;
                    r.indent = st_.depth > 0 ? (st_.depth - 1) * kSectionIndent : 0;
                    r.line = lineNo;
                    loss_.dividers.add(lineNo);
                    if (r.indent > 0)
                        loss_.sections.add(lineNo);
                    rows.push(std::move(r));
                    return;
                }
            }
            bool field = false;
            std::vector<Seg> out = output(line, preEscape, lineNo, field);
            if (out.empty())
                return;
            Row r;
            r.kind = Row::Kind::Text;
            r.segs = std::move(out);
            expandTabs(r.segs, field);
            r.fill = st_.style;
            // form columns pack their widgets from the left
            r.align = field ? Align::Left : st_.align;
            r.indent = st_.depth > 0 ? (st_.depth - 1) * kSectionIndent : 0;
            r.line = lineNo;
            if (r.align != Align::Left)
                loss_.aligned.add(lineNo);
            if (r.indent > 0)
                loss_.sections.add(lineNo);
            rows.push(std::move(r));
            return;
        }
    }

    // A table still open at the end of the page is not drawn: NomadNet
    // draws one only when a second "`t" closes it.
    void finish(Rows&) {
        if (!st_.table)
            return;
        st_.table = false;
        if (tableLines_ >= 2)
            loss_.unclosedTables.add(st_.tableLine);
    }

private:
    // Adds `cp` to `t`, unless it is zero-width or the row is already past
    // kMaxColumns (a tab counted as one column, the least it takes).
    void put(Text& t, char32_t cp) {
        if (used_ > kMaxColumns)
            return;
        if (const int w = cellWidth(cp); w > 0) {
            t += cp;
            used_ += w;
        }
    }

    // `s` as a part of its own in the current style: as much of it as put
    // keeps, the part added even if that is nothing when it is the first.
    void putPart(std::vector<Seg>& out, View s) {
        Text t;
        for (const char32_t cp : s) {
            if (used_ > kMaxColumns)
                break;
            put(t, cp);
        }
        if (!t.empty() || out.empty())
            out.push_back({std::move(t), st_.style});
    }

    // parse_partial on what follows "`{": a partial NomadNet draws as its
    // placeholder, else nothing (no "}", no URL, more than three parts, or
    // a refresh time float() refuses).
    static bool isPartial(View rest) {
        const std::size_t end = rest.find(U'}');
        if (end == npos)
            return false;
        const std::vector<View> parts = split(rest.substr(0, end), U'`');
        if (parts.size() > 3)
            return false;
        if (parts.size() >= 2 && !pyFloat(parts[1]))
            return false;
        return !parts[0].empty();
    }

    // parse_image on what follows "`(": an image NomadNet draws (or its
    // error line), else nothing (no ")" past the first character, no "`").
    static bool isImage(View rest) {
        const std::size_t end = rest.rfind(U')');
        if (end == npos || end == 0)
            return false;
        return rest.substr(0, end).find(U'`') != npos;
    }

    // A field `<flags|name`data>: a text field as its data within its width
    // (masked as *), a check box or radio button as urwid draws one. Each is
    // a widget of its own, its tabs counted from its start.
    bool readField(View line, std::size_t i, std::size_t tick, Next& nextGt, std::vector<Seg>& out, std::size_t& skip,
                   int lineNo) {
        if (tick == npos)
            return false;
        const View content = line.substr(i + 1, tick - i - 1);
        enum class Type { Field, Check, Radio } type = Type::Field;
        bool masked = false, prechecked = false;
        long width = 24;
        if (content.find(U'|') != npos) {
            const std::vector<View> comps = split(content, U'|');
            Text flags(comps[0]);
            const auto take = [&](char32_t f) {
                if (flags.find(f) == npos)
                    return false;
                std::erase(flags, f);
                return true;
            };
            if (take(U'^'))
                type = Type::Radio;
            else if (take(U'?'))
                type = Type::Check;
            else if (take(U'!'))
                masked = true;
            if (!flags.empty()) {
                const std::size_t x = flags.find(U'x');
                if (const std::optional<long> v = pyInt(View(flags).substr(0, x)))
                    width = std::min(*v, 256L);
            }
            prechecked = comps.size() > 3 && comps[3] == U"*";
        }
        const std::size_t end = nextGt(tick);
        if (end == npos)
            return false;
        const View data = line.substr(tick + 1, end - tick - 1);
        Text shown;
        bool zeroWidth = false;  // a zero-width code point within the width, not kept
        if (type == Type::Field) {
            const int w = int(std::max(0L, width));
            int used = 0;
            for (const char32_t cp : data) {
                const char32_t g = masked ? U'*' : cp;
                const int cw = g == U'\t' ? 8 - used % 8 : cellWidth(g);
                if (used + cw > w)
                    break;
                if (g == U'\t')
                    shown.append(std::size_t(cw), U' ');
                else if (cw > 0)
                    shown += g;
                else
                    zeroWidth = true;
                used += cw;
            }
            shown.append(std::size_t(w - used), U' ');
        } else {
            // the label is a Text of its own beside the "[ ] "
            int col = 0;
            shown = type == Type::Check ? (prechecked ? U"[X] " : U"[ ] ") : (prechecked ? U"(X) " : U"( ) ");
            shown += detab(data, col);
        }
        if (!shown.empty() || zeroWidth)
            putPart(out, shown);
        skip = end - i;
        loss_.fields.add(lineNo);
        return true;
    }

    void heading(View line, int lineNo, bool collapsible, bool collapsed, Rows& rows) {
        int depth = 0;
        while (std::size_t(depth) < line.size() && line[std::size_t(depth)] == U'>')
            ++depth;
        st_.depth = depth;
        line.remove_prefix(std::size_t(depth));
        if (line.empty())
            return;
        // the heading's style for its text; the line's own codes still
        // change it, and only alignment outlives the line
        const Style latched = st_.style;
        st_.style = headingStyle(depth);
        bool field = false;
        std::vector<Seg> out = output(line, false, lineNo, field);
        st_.style = latched;
        if (out.empty())
            return;
        Row r;
        r.kind = Row::Kind::Heading;
        r.fill = out.front().style;
        Text lead((std::size_t(depth) - 1) * kSectionIndent, U' ');
        if (collapsible)
            lead += (collapsed ? st_.foldClosed : st_.foldOpen) + U" ";
        if (!lead.empty())
            out.insert(out.begin(), Seg{std::move(lead), r.fill});
        r.segs = std::move(out);
        expandTabs(r.segs, false);
        r.align = st_.align;
        r.line = lineNo;
        loss_.headings.add(lineNo);
        if (r.align != Align::Left)
            loss_.aligned.add(lineNo);
        rows.push(std::move(r));
    }

    // "#!fold open closed" sets the glyphs a collapsible heading shows.
    void foldGlyphs(View line) {
        std::vector<Text> parts;
        Text word;
        for (const char32_t c : line) {
            if (pySpace(c)) {
                if (!word.empty())
                    parts.push_back(std::move(word));
                word.clear();
            } else {
                word += c;
            }
        }
        if (!word.empty())
            parts.push_back(std::move(word));
        if (parts.empty() || parts[0] != U"#!fold" || parts.size() < 2)
            return;
        st_.foldOpen = parts[1];
        st_.foldClosed = parts.size() >= 3 ? parts[2] : parts[1];
        // a glyph is drawn on every collapsible heading: only what a row can show
        capText(st_.foldOpen);
        capText(st_.foldClosed);
    }

    // NomadNet draws a table through RNS's markdown renderer, and nothing
    // for one of fewer than two lines; its lines are kept here as text, one
    // row each, as the rest of the page draws them. They are read again from
    // the page, from the line after the table's start to `to`, the start of
    // its closing line (or the page's end), so a table holds no copy of them:
    // what parse_line put in the table's buffer, the lines that are not
    // empty, not "`=", not in literal mode and not comments.
    void endTable(Rows& rows, std::size_t to) {
        st_.table = false;
        if (tableLines_ < 2)
            return;
        loss_.tables.add(st_.tableLine);
        if (tableFrom_ >= to)
            return;
        int at = st_.tableLine + 1;
        bool literal = false;
        forEachLine(page_.substr(tableFrom_, to - tableFrom_), [&](View text) {
            const int lineNo = at++;
            if (text.empty())
                return;
            if (text == U"`=") {
                literal = !literal;
                return;
            }
            bool collapsible = false, collapsed = false, preEscape = false;
            if (literal || !prefixes(text, collapsible, collapsed, preEscape))
                return;
            bool field = false;
            std::vector<Seg> out = output(text, false, lineNo, field);
            if (out.empty())
                return;
            Row r;
            r.kind = Row::Kind::Text;
            r.segs = std::move(out);
            expandTabs(r.segs, field);
            r.fill = st_.style;
            r.line = lineNo;
            rows.push(std::move(r));
        });
    }

    State& st_;
    MicronReadLoss& loss_;
    View page_;
    int used_ = 0;                // the columns output() has kept for its row
    std::size_t tableFrom_ = 0;   // where the open table's first line starts
    int tableLines_ = 0;          // the lines the open table has buffered
};

int rowWidth(const Row& r) {
    int w = 0;
    for (const Seg& s : r.segs)
        w += textWidth(s.text);
    switch (r.kind) {
    case Row::Kind::Text:
        return r.indent + w + r.indent;
    case Row::Kind::Heading:
        return w;
    default:
        return 0;
    }
}

void place(Canvas& canvas, int y, const Row& r, MicronReadLoss& loss) {
    const int w = canvas.width();
    for (int x = 0; x < w; ++x)
        canvas.at(x, y) = styledCell(U' ', r.fill);
    const int left = std::min(r.indent, w);
    const int right = std::min(r.indent, w - left);
    if (r.kind == Row::Kind::Empty)
        return;
    if (r.kind == Row::Kind::Divider) {
        for (int x = left; x < w - right; ++x)
            canvas.at(x, y) = styledCell(r.divider, r.fill);
        return;
    }
    const int avail = w - left - right;
    int tw = 0;
    for (const Seg& s : r.segs)
        tw += textWidth(s.text);
    if (tw > avail)
        loss.clipped.add(r.line);
    int x = left;
    if (tw < avail)
        x += r.align == Align::Right ? avail - tw : r.align == Align::Centre ? (avail - tw + 1) / 2 : 0;
    const int end = left + avail;
    for (const Seg& s : r.segs) {
        for (const char32_t cp : s.text) {
            const int cw = cellWidth(cp);
            if (cw == 0)
                continue;
            if (x + cw > end) {
                if (x < end)
                    canvas.at(x, y) = styledCell(U' ', s.style);
                return;
            }
            canvas.at(x, y) = styledCell(cp, s.style);
            if (cw == 2) {
                Cell cont = canvas.at(x, y);
                cont.ch = Glyph::continuation();
                canvas.at(x + 1, y) = cont;
            }
            x += cw;
        }
    }
}

} // namespace

bool isPage(std::string_view path) {
    if (path.size() < 3)
        return false;
    const std::string_view ext = path.substr(path.size() - 3);
    return ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') && (ext[2] == 'u' || ext[2] == 'U');
}

AnsiDocument parse(std::string_view page, ParseStats* stats) {
    AnsiDocument doc;
    doc.encoding = TextEncoding::Utf8;
    doc.ice = true;
    MicronReadLoss& loss = doc.readLoss.micron;

    // the page as text, a byte that isn't well-formed UTF-8 as U+FFFD
    Text raw;
    raw.reserve(page.size());
    int lineNo = 1;
    for (std::size_t i = 0; i < page.size();) {
        if (const std::optional<char32_t> cp = nextUtf8(page, i)) {
            raw += *cp;
            lineNo += *cp == U'\n';
        } else {
            raw += U'�';
            loss.badUtf8.add(lineNo);
        }
    }

    // Browser: "#!bg=" and "#!fg=" anywhere in the page, their value up to
    // the next newline, used if it is 3 or 6 characters long
    const auto header = [&](View tag) -> std::optional<Text> {
        const std::size_t at = raw.find(tag);
        if (at == npos)
            return std::nullopt;
        const std::size_t end = raw.find(U'\n', at);
        if (end == npos)
            return std::nullopt;
        const std::size_t len = end - (at + tag.size());
        if (len != 3 && len != 6)
            return std::nullopt;
        return raw.substr(at + tag.size(), len);
    };
    State st;
    st.defaultFg = header(U"#!fg=").value_or(U"ddd");
    st.defaultBg = header(U"#!bg=").value_or(U"default");
    st.style = {st.defaultFg, st.defaultBg};

    int lead = 0;
    const Text text = stripModifiers(std::move(raw), lead);
    Rows rows{{}, loss.rows};
    Reader reader(st, loss, text);
    int n = lead + 1;
    forEachLine(text, [&](View line) {
        if (line.empty()) {
            Row r;
            r.fill = st.style;
            r.line = n;
            rows.push(std::move(r));
        } else {
            reader.line(line, n, rows);
        }
        ++n;
    });
    reader.finish(rows);
    if (stats) {
        stats->rowsHeld = rows.kept.size();
        stats->widestRow = rows.widest;
    }

    int width = 0;
    for (const Row& r : rows.kept)
        width = std::max(width, rowWidth(r));
    width = width == 0 ? Canvas::kDefaultWidth : std::min(width, kMaxColumns);
    doc.canvas = Canvas(width, std::max<int>(1, int(rows.kept.size())));
    if (rows.kept.empty()) {
        for (int x = 0; x < width; ++x)
            doc.canvas.at(x, 0) = styledCell(U' ', st.style);
    }
    for (std::size_t y = 0; y < rows.kept.size(); ++y)
        place(doc.canvas, int(y), rows.kept[y], loss);
    return doc;
}

std::vector<std::string> readNotes(const ReadLoss& loss) {
    std::vector<std::string> out;
    const auto add = [&](const MicronLoss& l, const std::string& one, const std::string& many) {
        if (!l.count)
            return;
        out.push_back(std::to_string(l.count) + (l.count == 1 ? one : many) +
                      (l.count == 1 ? ", on line " : ", the first on line ") + std::to_string(l.line));
    };
    const MicronReadLoss& m = loss.micron;
    add(m.links, " link drawn as its label", " links drawn as their labels");
    add(m.fields, " field drawn as its text", " fields drawn as their text");
    add(m.headings, " heading drawn as text in its heading colours", " headings drawn as text in their heading colours");
    add(m.dividers, " divider drawn as a row of its glyph", " dividers drawn as rows of their glyph");
    add(m.sections, " line indented by its section depth", " lines indented by their section depth");
    add(m.aligned, " line centred or right-aligned at the page width",
        " lines centred or right-aligned at the page width");
    add(m.tables, " table kept as its lines of text", " tables kept as their lines of text");
    add(m.unclosedTables, " table never closed, left out", " tables never closed, left out");
    add(m.partials, " partial left out", " partials left out");
    add(m.images, " image left out", " images left out");
    const std::string cols = std::to_string(kMaxColumns), last = std::to_string(kMaxRows);
    add(m.clipped, " row cut at " + cols + " columns", " rows cut at " + cols + " columns");
    add(m.rows, " line past row " + last + " left out", " lines past row " + last + " left out");
    add(m.badUtf8, " byte that isn't UTF-8 read as U+FFFD", " bytes that aren't UTF-8 read as U+FFFD");
    return out;
}

} // namespace bw::micron
