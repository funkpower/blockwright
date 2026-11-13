#include "blockwright/micron.h"

#include "blockwright/cp437.h"
#include "blockwright/glyph.h"
#include "blockwright/glyph_rows.h"
#include "blockwright/irc.h"

#include <QChar>

#include <algorithm>

namespace bw::micron {

namespace {

constexpr char kHex[] = "0123456789abcdef";

// What Micron's text mode reads as markup (MicronParser.make_output): ` starts
// a formatting code and \ escapes the next character.
bool needsEscape(char32_t cp) { return cp == U'`' || cp == U'\\'; }

bool dropsAny(Glyph g) {
    const GlyphCodePoints cps = codePoints(g);
    return std::any_of(cps.begin(), cps.end(), droppedByNomadNet);
}

// A cell as it is written: the text of its glyph and the colours and
// formatting it is drawn in.
struct Out {
    std::string text;  // empty for a wide glyph's continuation
    Rgb fg, bg;
    std::uint8_t attrs = 0;
    // Whether the cell shows its foreground (not a blank, unless underlined)
    // and its background (not a full block that covers its cell, as an
    // italic one doesn't): a colour it doesn't show isn't written for it.
    bool showsFg = true, showsBg = true;
};

// Whether a full block in formatting `attrs` still covers its whole cell, as
// the renderer draws it (formatRows) at 8 and 9 px: bold and underline add
// nothing to it, italic shears it off its background (PLAN §14, #360).
bool fullBlockCovers(std::uint8_t attrs) {
    for (const int w : {8, 9}) {
        GlyphRows full{};
        full.fill(std::uint16_t((1u << w) - 1));
        if (formatRows(full, attrs, w, 16) != full)
            return false;
    }
    return true;
}

Rgb resolve(const Color& c, const std::array<Rgb, 16>& palette) { return c.isIndexed() ? palette[c.index & 15] : c.rgb; }

// The cells of row `y`, as encodeLine writes them; the cells written as
// something else are added to `lost`, if given.
std::vector<Out> rowCells(const Canvas& canvas, int y, const std::array<Rgb, 16>& palette, bool ice,
                          std::vector<Unwritable>* lost) {
    const int w = canvas.width();
    std::vector<Cell> cells(static_cast<std::size_t>(w));
    for (int x = 0; x < w; ++x)
        cells[std::size_t(x)] = canvas.at(x, y);
    std::vector<irc::MircCell> glyphs = irc::mircRow(cells, lost, y);
    std::vector<Out> out(cells.size());
    for (std::size_t x = 0; x < cells.size(); ++x) {
        const Cell& c = cells[x];
        // drawn as the editor shows it: a transparent cell blank, a bright
        // background under iCE off as its dark colour; an RGB one as it is
        const Cell d = c.transparent ? Cell{} : (c.bg.isIndexed() ? iceCell(c, ice) : c);
        Out& o = out[x];
        o.fg = resolve(d.fg, palette);
        o.bg = resolve(d.bg, palette);
        o.attrs = std::uint8_t(d.attrs & attr::Formatting);
        Glyph g = glyphs[x].ch;
        if (g.isContinuation())
            continue;
        const bool pair = x + 1 < glyphs.size() && glyphs[x + 1].ch.isContinuation();
        if (dropsAny(g)) {
            if (lost)
                lost->push_back({int(x), y, c.ch, Loss::NoCode, U'?', pair});
            g = Glyph(U'?');
            if (pair)
                glyphs[x + 1].ch = Glyph(U' ');
        }
        o.showsFg = !((g == Glyph(U' ') || g == Glyph(U'\u00a0')) && !attr::showsOnBlank(o.attrs));
        o.showsBg = g != Glyph(U'\u2588') || !fullBlockCovers(o.attrs);
        const GlyphCodePoints cps = codePoints(g);
        for (const char32_t cp : cps) {
            if (needsEscape(cp))
                o.text += '\\';
            appendUtf8(o.text, cp);
        }
    }
    return out;
}

void appendColour(std::string& s, char which, Rgb c) {
    s += '`';
    s += which;
    s += colourCode(c);
}

} // namespace

std::string colourCode(Rgb c) {
    const auto doubled = [](std::uint8_t v) { return (v >> 4) == (v & 15); };
    std::string s;
    if (doubled(c.r) && doubled(c.g) && doubled(c.b)) {
        for (const std::uint8_t v : {c.r, c.g, c.b})
            s += kHex[v & 15];
        return s;
    }
    s += 'T';
    for (const std::uint8_t v : {c.r, c.g, c.b}) {
        s += kHex[v >> 4];
        s += kHex[v & 15];
    }
    return s;
}

bool droppedByNomadNet(char32_t cp) {
    // nomadnet/util.py STRIP_CONTROL_RE
    if (cp <= 0x08 || cp == 0x0b || cp == 0x0c || (cp >= 0x0e && cp <= 0x1f) || (cp >= 0x7f && cp <= 0x9f) ||
        (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
        cp == 0xfeff || (cp >= 0xfff0 && cp <= 0xfff8))
        return true;
    // strip_modifiers: its invalid_rendering glyph, variation selectors,
    // emoji modifiers, then every mark and format character
    if (cp == 0x261d || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef) ||
        (cp >= 0x1f3fb && cp <= 0x1f3ff))
        return true;
    if (cp > 0x10ffff)
        return true;
    switch (QChar::category(char32_t(cp))) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
    case QChar::Other_Format:
        return true;
    default:
        return false;
    }
}

std::string encodeLine(const Canvas& canvas, int y, const std::array<Rgb, 16>& palette, bool ice,
                       const Options& opts) {
    const std::vector<Out> cells = rowCells(canvas, y, palette, ice, nullptr);
    std::string s;
    if (opts.centre)
        s += "`c";
    std::optional<Rgb> fg, bg;
    std::uint8_t attrs = 0;
    char last = 0;  // the last text character written, for "#!"
    for (const Out& o : cells) {
        if (o.text.empty())
            continue;
        if (o.showsFg && fg != o.fg) {
            appendColour(s, 'F', o.fg);
            fg = o.fg;
            last = 0;
        }
        if (o.showsBg && bg != o.bg) {
            appendColour(s, 'B', o.bg);
            bg = o.bg;
            last = 0;
        }
        if (attrs != o.attrs) {
            const std::uint8_t flip = attrs ^ o.attrs;
            if (flip & attr::Bold)
                s += "`!";
            if (flip & attr::Italic)
                s += "`*";
            if (flip & attr::Underline)
                s += "`_";
            attrs = o.attrs;
            last = 0;
        }
        // NomadNet's browser looks for "#!bg=" and "#!fg=" headers anywhere in
        // a page; an escaped ! keeps art text from reading as one
        if (last == '#' && o.text[0] == '!')
            s += '\\';
        s += o.text;
        last = o.text.back();
    }
    // `` resets the alignment too, and NomadNet aligns a line by the state at
    // its end, so a centred line closes its formatting one code at a time
    if (opts.centre) {
        if (attrs & attr::Bold)
            s += "`!";
        if (attrs & attr::Italic)
            s += "`*";
        if (attrs & attr::Underline)
            s += "`_";
        s += "`f`b";
    } else {
        s += "``";
    }
    return s;
}

std::string encodeCanvas(const Canvas& canvas, const std::array<Rgb, 16>& palette, bool ice, const Options& opts) {
    std::string s;
    if (opts.cacheSeconds)
        s += "#!c=" + std::to_string(*opts.cacheSeconds) + "\n";
    for (int y = 0; y < canvas.height(); ++y) {
        s += encodeLine(canvas, y, palette, ice, opts);
        s += '\n';
    }
    return s;
}

std::string encode(const AnsiDocument& doc, const Options& opts) {
    return encodeCanvas(doc.canvas, doc.palette.value_or(kPalette), doc.ice, opts);
}

std::vector<Unwritable> unwritable(const Canvas& canvas) {
    std::vector<Unwritable> lost;
    for (int y = 0; y < canvas.height(); ++y)
        rowCells(canvas, y, kPalette, true, &lost);
    return lost;
}

} // namespace bw::micron
