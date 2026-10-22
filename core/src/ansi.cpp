#include "blockwright/ansi.h"

#include "blockwright/cp437.h"

#include <algorithm>
#include <charconv>
#include <map>

namespace bw {

namespace {

// Numbers in escape sequences saturate here instead of overflowing.
constexpr int kMaxParam = 1'000'000;

enum class Fields {
    Csi,    // an empty or non-numeric field reads as 0
    Pablo,  // CSI t: empty fields are skipped (strtok, as libansilove:
            // loaders/ansi.c:488); a non-numeric one reads as 0
    Sgr,    // empty fields are skipped (strtok, loaders/ansi.c:399), and so
            // are private or non-numeric ones such as ?999: libansilove reads
            // those as 0 and resets everything, we don't (PLAN §8)
};

// Parameters of a CSI sequence. Outside SGR a leading private marker
// (?, = or >) is dropped first.
std::vector<int> parseParams(std::string_view raw, Fields mode = Fields::Csi) {
    std::vector<int> params;
    size_t lead = mode == Fields::Sgr ? 0 : raw.find_first_not_of("?=>");
    if (lead == std::string_view::npos)
        return params;
    raw.remove_prefix(lead);
    if (raw.empty())
        return params;
    for (;;) {
        size_t semi = raw.find(';');
        std::string_view p = raw.substr(0, semi);
        long long v = 0;
        bool digits = !p.empty() && std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits) {
            for (char c : p) {
                v = v * 10 + (c - '0');
                if (v > kMaxParam) {
                    v = kMaxParam;
                    break;
                }
            }
        }
        if (mode == Fields::Csi || (mode == Fields::Pablo && !p.empty()) || digits)
            params.push_back(static_cast<int>(v));
        if (semi == std::string_view::npos)
            break;
        raw.remove_prefix(semi + 1);
    }
    return params;
}

std::uint8_t xtermIndex(int n) {
    if (n >= 0 && n < 16)
        return static_cast<std::uint8_t>(ansiToCga(static_cast<std::uint8_t>(n & 7)) + (n >= 8 ? 8 : 0));
    return nearestPaletteIndex(xtermRgb(n));
}

std::uint8_t clampByte(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }

// The cursor follows libansilove (loaders/ansi.c:149-217): the column may
// reach the width, and the next byte that draws or moves the cursor first
// wraps it to the start of the next row. One deliberate divergence: when a
// glyph filled the last column, a LF after nothing but CRs, SGRs and CSI t
// ends that row instead of adding a blank one, so a full-width row may end
// in CR LF. libansilove wraps on the CR and then adds a row for the LF. Any
// other escape wraps first, as libansilove does on every ESC.
class Parser {
public:
    Parser(int width, bool ice) : width_(width), ice_(ice) {}

    void run(std::string_view data) {
        const size_t n = data.size();
        size_t i = 0;
        while (i < n) {
            const auto b = static_cast<unsigned char>(data[i]);
            if (b == 0x1b) {
                if (i + 1 < n && data[i + 1] == '[') {
                    size_t j = i + 2;
                    while (j < n && data[j] >= 0x30 && data[j] <= 0x3f)  // parameters
                        ++j;
                    size_t k = j;
                    while (k < n && data[k] >= 0x20 && data[k] <= 0x2f)  // intermediates
                        ++k;
                    if (k >= n)
                        break;  // cut off mid-sequence
                    const auto f = static_cast<unsigned char>(data[k]);
                    if (k != j || (f != 'm' && f != 't'))
                        wrapIfPending();
                    if (f < 0x40 || f > 0x7e) {
                        // No final byte: the sequence is malformed. Drop what
                        // was read and handle this byte as ordinary input, so
                        // an ESC starts a new escape and a CR or LF still acts.
                        i = k;
                        continue;
                    }
                    if (k == j)
                        csi(data[k], data.substr(i + 2, j - (i + 2)));
                    i = k + 1;
                    continue;
                }
                // Any other ESC is skipped on its own and the bytes after it
                // are read as usual, as libansilove does (loaders/ansi.c:172-178).
                wrapIfPending();
                ++i;
                continue;
            }
            ++i;
            switch (b) {
            case 0x0a:
                if (!glyphWrap_)
                    wrapIfPending();
                row_ = std::min(kMaxRows, row_ + 1);
                col_ = 0;
                glyphWrap_ = false;
                break;
            case 0x0d:
                // CR doesn't move the cursor, as in libansilove (loaders/ansi.c:164-165)
                if (!glyphWrap_)
                    wrapIfPending();
                break;
            case 0x09:
                // libansilove (loaders/ansi.c:166-167): to the next multiple of
                // 8, which may be the width or past it, so the next glyph wraps
                wrapIfPending();
                col_ = std::min(width_, col_ + 8 - col_ % 8);
                break;
            case 0x00:
                put(U' ');
                break;
            default:
                put(cp437ToUnicode(b));
            }
        }
    }

    Canvas canvas() const {
        Canvas c(width_, height_);
        for (const auto& [y, row] : rows_)
            for (int x = 0; x < width_; ++x)
                c.at(x, y) = row[x];
        return c;
    }

    bool empty() const { return rows_.empty(); }

private:
    // At or past the width the cursor waits to wrap; this takes it to the
    // start of the next row.
    void wrapIfPending() {
        if (col_ >= width_) {
            row_ = std::min(kMaxRows, row_ + 1);
            col_ = 0;
            glyphWrap_ = false;
        }
    }

    void put(char32_t ch) {
        wrapIfPending();
        if (row_ >= kMaxRows)
            throw AnsiError("draws below row " + std::to_string(kMaxRows));
        // Rows exist only once something is drawn on them, so moving the
        // cursor far down or clearing the screen costs nothing per row.
        if (row_ != curRowIndex_) {
            std::vector<Cell>& r = rows_[row_];
            if (r.empty())
                r.resize(static_cast<size_t>(width_));
            curRow_ = &r;
            curRowIndex_ = row_;
        }
        height_ = std::max(height_, row_ + 1);
        Cell& c = (*curRow_)[static_cast<size_t>(col_)];
        c.ch = ch;
        // While reversed, PabloDraw 24-bit colours are ignored and the colours
        // they replaced are swapped, as libansilove does (loaders/ansi.c:201-209).
        const bool xfg = xfg_ >= 0 && !(reverse_ && xfgPablo_);
        const bool xbg = xbg_ >= 0 && !(reverse_ && xbgPablo_);
        int fg = xfg ? xfg_ : ansiToCga(fg_) + (bold_ ? 8 : 0);
        int bg = xbg ? xbg_ : ansiToCga(bg_) + (blink_ && ice_ ? 8 : 0);
        if (reverse_) {
            // As libansilove (loaders/ansi.c:201-203): the background takes the
            // foreground's colour without its bright bit, and the foreground
            // takes the background's colour plus the foreground's bright bit.
            // libansilove adds that bit, so bold on an iCE bright background
            // (1;5;31;44;7) gives index 20, past the palette; we OR it, and the
            // foreground is bright if either colour was (LightBlue there).
            const int f = fg;
            fg = bg | (f & 8);
            bg = f & 7;
        }
        c.fg = Color::indexed(static_cast<std::uint8_t>(fg));
        c.bg = Color::indexed(static_cast<std::uint8_t>(bg));
        c.attrs = blink_ && !ice_ ? attr::Blink : 0;
        c.transparent = false;
        ++col_;
        glyphWrap_ = col_ == width_;
    }

    void sgr(const std::vector<int>& p) {
        for (size_t i = 0; i < p.size(); ++i) {
            const int v = p[i];
            if (v == 0) {
                fg_ = 7, bg_ = 0, bold_ = blink_ = reverse_ = false, xfg_ = xbg_ = -1;
            } else if (v == 1) {
                // SGR 1 drops a PabloDraw foreground and SGR 5 a PabloDraw
                // background, whatever the iCE flag, as libansilove does
                // (loaders/ansi.c:413-427); a dropped colour stays dropped
                // after SGR 27.
                bold_ = true;
                if (xfgPablo_)
                    xfg_ = -1, xfgPablo_ = false;
            } else if (v == 5) {
                blink_ = true;
                if (xbgPablo_)
                    xbg_ = -1, xbgPablo_ = false;
            } else if (v == 7) {
                reverse_ = true;
            } else if (v == 27) {
                reverse_ = false;
            } else if (v == 21 || v == 22) {
                // 21, 22, 25, 39, 49 and the 9x/10x and 38/48 colours aren't
                // read by libansilove; we keep their usual meaning (PLAN §8).
                bold_ = false;
            } else if (v == 25) {
                blink_ = false;
            } else if (v >= 30 && v <= 37) {
                fg_ = v - 30, xfg_ = -1;
            } else if (v >= 90 && v <= 97) {
                fg_ = v - 90, xfg_ = ansiToCga(static_cast<std::uint8_t>(v - 90)) + 8, xfgPablo_ = false;
            } else if (v >= 40 && v <= 47) {
                bg_ = v - 40, xbg_ = -1;
            } else if (v >= 100 && v <= 107) {
                bg_ = v - 100, xbg_ = ansiToCga(static_cast<std::uint8_t>(v - 100)) + 8, xbgPablo_ = false;
            } else if (v == 39) {
                fg_ = 7, xfg_ = -1;
            } else if (v == 49) {
                bg_ = 0, xbg_ = -1;
            } else if (v == 38 || v == 48) {
                int& target = v == 38 ? xfg_ : xbg_;
                if (i + 2 < p.size() && p[i + 1] == 5) {
                    target = xtermIndex(p[i + 2]);
                    i += 2;
                } else if (i + 4 < p.size() && p[i + 1] == 2) {
                    target = nearestPaletteIndex({clampByte(p[i + 2]), clampByte(p[i + 3]), clampByte(p[i + 4])});
                    i += 4;
                } else {
                    break;  // malformed: ignore the rest
                }
                (v == 38 ? xfgPablo_ : xbgPablo_) = false;
            }
        }
    }

    // PabloDraw 24-bit colour, as libansilove reads it (loaders/ansi.c:478-509):
    // ESC[0;R;G;Bt sets the background and ESC[1;R;G;Bt the foreground; any
    // other selector does nothing. A missing value, or one past 255, reads
    // as 0, and black (0,0,0) means no 24-bit colour, which drops one set
    // before.
    void pablo(const std::vector<int>& p) {
        if (p.empty())
            return;
        auto at = [&](size_t k) { return k < p.size() && p[k] <= 255 ? p[k] : 0; };
        const int sel = at(0);
        if (sel != 0 && sel != 1)
            return;
        int& target = sel == 1 ? xfg_ : xbg_;
        bool& pablo = sel == 1 ? xfgPablo_ : xbgPablo_;
        const Rgb rgb{static_cast<std::uint8_t>(at(1)), static_cast<std::uint8_t>(at(2)), static_cast<std::uint8_t>(at(3))};
        if (rgb == Rgb{}) {
            if (pablo)
                target = -1, pablo = false;
            return;
        }
        target = nearestPaletteIndex(rgb);
        pablo = true;
    }

    void csi(char final, std::string_view raw) {
        if (final == 'm') {
            // ESC[m has no fields and does nothing, as in libansilove.
            sgr(parseParams(raw, Fields::Sgr));
            return;
        }
        if (final == 't') {
            pablo(parseParams(raw, Fields::Pablo));
            return;
        }
        const std::vector<int> p = parseParams(raw);
        // With more than one parameter libansilove reads 1 here (strtonum
        // fails on "2;5"); we use the first (PLAN §8).
        const int n = (p.empty() ? 0 : p[0]) ? p[0] : 1;
        switch (final) {
        case 'A':
            row_ = std::max(0, row_ - n);
            break;
        case 'B':
            row_ = std::min(kMaxRows, row_ + n);
            break;
        case 'C':
            // up to the width, as libansilove (loaders/ansi.c:314-317): the
            // next glyph then starts the next row
            col_ = std::min(width_, col_ + n);
            break;
        case 'D':
            col_ = std::max(0, col_ - n);
            break;
        case 'H':
        case 'f':
            // 1-based; a column past the width wraps the next glyph, as in
            // libansilove (loaders/ansi.c:252-253). Row or column 0 is taken as
            // 1 (libansilove computes -1 there).
            row_ = std::clamp((p.empty() ? 1 : p[0]) - 1, 0, kMaxRows);
            col_ = std::clamp((p.size() > 1 ? p[1] : 1) - 1, 0, width_);
            break;
        case 'J':
            // ED 2 clears the screen and homes the cursor, as ANSI.SYS does and
            // as art relies on; libansilove (loaders/ansi.c:364-388) resets
            // the cursor and the buffer the same way. Other ED modes are ignored.
            if ((p.empty() ? 0 : p[0]) == 2) {
                rows_.clear();
                curRow_ = nullptr;
                curRowIndex_ = -1;
                height_ = 0;
                row_ = col_ = 0;
            }
            break;
        case 'K':
            // Erase in line is skipped, as libansilove does (loaders/ansi.c:471-475).
            break;
        case 's':
            savedRow_ = row_, savedCol_ = col_;
            break;
        case 'u':
            row_ = savedRow_, col_ = savedCol_;
            break;
        default:
            break;
        }
    }

    int width_;
    bool ice_;
    std::map<int, std::vector<Cell>> rows_;  // only the rows drawn on
    std::vector<Cell>* curRow_ = nullptr;
    int curRowIndex_ = -1;
    int height_ = 0;  // last row drawn on + 1
    int row_ = 0, col_ = 0;  // col_ == width_: the next glyph wraps
    bool glyphWrap_ = false;  // ... and a glyph put it there
    int fg_ = 7, bg_ = 0;  // ANSI numbers 0-7
    bool bold_ = false, blink_ = false, reverse_ = false;
    int xfg_ = -1, xbg_ = -1;  // palette indices set by 9x/10x, 38/48 or CSI t; -1 = none
    bool xfgPablo_ = false, xbgPablo_ = false;  // xfg_/xbg_ came from CSI t
    int savedRow_ = 0, savedCol_ = 0;
};

// A cell the trailing trim may drop, written with iCE colours `ice`. A
// blinking space written without iCE is not one: read back with iCE on it is
// a bright-black background. With iCE on its blink is dropped (and counted),
// so it is written as a plain black space and goes like one.
bool blankBlack(const Cell& c, bool ice) {
    return c.transparent || (c.ch == U' ' && c.bg.paletteIndex() == Black && (ice || !(c.attrs & attr::Blink)));
}

bool writableByte(std::uint8_t b) {
    // bytes the parser acts on instead of drawing
    return b != 0x09 && b != 0x0a && b != 0x0d && b != 0x1a && b != 0x1b;
}

} // namespace

bool isWritableGlyph(char32_t ch) {
    const std::optional<std::uint8_t> b = unicodeToCp437(ch);
    return b && *b != 0x00 && writableByte(*b);
}

Cell iceCell(Cell c, bool ice) {
    const std::uint8_t bg = c.bg.paletteIndex();
    if (ice && (c.attrs & attr::Blink)) {
        c.bg = Color::indexed(static_cast<std::uint8_t>(bg | 8));
        c.attrs &= static_cast<std::uint8_t>(~attr::Blink);
    } else if (!ice && bg >= 8) {
        c.bg = Color::indexed(static_cast<std::uint8_t>(bg & 7));
        c.attrs |= attr::Blink;
    }
    return c;
}

AnsiDocument parseAnsi(std::string_view file, const ParseOptions& opts) {
    SauceSplit split = splitSauce(file);
    AnsiDocument doc;
    doc.sauce = std::move(split.sauce);
    int width = opts.defaultWidth, minHeight = 0;
    if (doc.sauce && doc.sauce->isCharacterArt()) {
        if (doc.sauce->tinfo1)
            width = doc.sauce->tinfo1;
        minHeight = std::min<int>(doc.sauce->tinfo2, kMaxRows);
        doc.ice = doc.sauce->ice();
    }
    if (width < 1 || width > kMaxColumns)
        throw AnsiError("width " + std::to_string(width) + " is outside 1-" + std::to_string(kMaxColumns) + " columns");
    Parser parser(width, doc.ice);
    parser.run(split.content);
    if (parser.empty())
        throw AnsiError("no ANSI content (nothing is drawn)");
    doc.canvas = parser.canvas();
    if (doc.canvas.height() < minHeight)
        doc.canvas.resize(width, minHeight);
    return doc;
}

AnsiOutput writeAnsi(const AnsiDocument& doc) {
    AnsiOutput out;
    std::string& s = out.bytes;
    const Canvas& cv = doc.canvas;
    const Cell blank{};
    // Without iCE colours a bright background can't be written (SGR 5 would
    // mean blink on a dark one, and 100-107 aren't read by classic viewers),
    // so a canvas with any bright background is written with iCE on. Blink
    // can't be written with iCE on: those cells lose it, and that's reported.
    bool ice = doc.ice;
    for (int y = 0; y < cv.height() && !ice; ++y)
        for (int x = 0; x < cv.width() && !ice; ++x)
            ice = !cv.at(x, y).transparent && cv.at(x, y).bg.paletteIndex() >= 8;
    if (ice)
        for (int y = 0; y < cv.height(); ++y)
            for (int x = 0; x < cv.width(); ++x)
                if (!cv.at(x, y).transparent && (cv.at(x, y).attrs & attr::Blink))
                    ++out.blinkDropped;
    s = "\x1b[0m";
    for (int y = 0; y < cv.height(); ++y) {
        int end = cv.width();
        while (end > 0 && blankBlack(cv.at(end - 1, y), ice))
            --end;
        int cur = -1;
        for (int x = 0; x < end; ++x) {
            const Cell& c = cv.at(x, y).transparent ? blank : cv.at(x, y);
            const std::uint8_t fg = c.fg.paletteIndex(), bg = c.bg.paletteIndex();
            const bool blink = bg >= 8 || (!ice && (c.attrs & attr::Blink));
            const int key = fg | (bg << 4) | (blink << 8);
            if (key != cur) {
                s += "\x1b[0";
                if (fg >= 8)
                    s += ";1";
                if (blink)
                    s += ";5";
                s += ";3";
                s += static_cast<char>('0' + cgaToAnsi(fg));
                s += ";4";
                s += static_cast<char>('0' + cgaToAnsi(bg));
                s += 'm';
                cur = key;
            }
            std::optional<std::uint8_t> b = unicodeToCp437(c.ch);
            if (b && *b == 0x00)
                b = ' ';  // NUL draws as a blank
            if (!b || !writableByte(*b)) {
                out.unwritable.push_back({x, y, c.ch});
                b = '?';
            }
            s += static_cast<char>(*b);
        }
        // A row never starts with its line break: after a full-width row the
        // cursor may be waiting to wrap, and a bare CR LF would end that row.
        if (!end)
            s += ' ';
        s += "\x1b[0m";
        if (end < cv.width())  // a full-width row wraps on its own
            s += "\r\n";
    }
    Sauce sauce = doc.sauce ? *doc.sauce : Sauce::fresh(cv.width(), cv.height());
    sauce.dataType = 1;
    sauce.fileType = 1;
    sauce.tinfo1 = static_cast<std::uint16_t>(cv.width());
    sauce.tinfo2 = static_cast<std::uint16_t>(cv.height());
    sauce.setIce(ice);
    const auto contentSize = static_cast<std::uint32_t>(s.size());
    s += encodeSauce(sauce, contentSize);
    return out;
}

} // namespace bw
