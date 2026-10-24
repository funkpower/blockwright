#include "blockwright/mirc.h"

#include "blockwright/cp437.h"

#include <algorithm>
#include <climits>
#include <utility>
#include <vector>

// Write a row of cells as one IRC line in as few bytes as possible.
//
// Many cells can be drawn more than one way: a space shows only its
// background, so it looks the same as a full block in that colour; a top
// half block is a bottom half block with the colours swapped; any glyph whose
// colours match is a solid cell. Which way is shortest depends on the colour
// codes around it, so the encoder searches all of them for the line with the
// fewest bytes. Colour numbers are written with one digit wherever the next
// character can't be read as part of the number.

namespace bw::irc {

namespace {

// A colour the client draws with, or kDefault for its own default. kDefault
// sorts below every colour.
constexpr int kDefault = -1;

struct State {
    int fg = kDefault;
    int bg = kDefault;
    auto operator<=>(const State&) const = default;
};

// What a way of drawing a cell needs: a colour, or kDefault for "anything".
struct Way {
    char32_t glyph;
    int fg;
    int bg;
};

struct Ways {
    std::array<Way, 2> way;
    int n;
};

bool blank(char32_t ch) { return ch == U' ' || ch == U' '; }

// The ways to draw a cell: at most two.
Ways ways(const MircCell& cell, bool swapShades) {
    const int f = cell.fg, b = cell.bg;
    const auto solid = [](int c) { return Ways{{{{U' ', kDefault, c}, {U'█', c, kDefault}}}, 2}; };
    if (blank(cell.ch))
        return solid(b);
    if (cell.ch == U'█' || f == b)
        return solid(f);
    const auto pair = [&](char32_t a, char32_t z) { return Ways{{{{a, f, b}, {z, b, f}}}, 2}; };
    switch (cell.ch) {
    case U'▀': return pair(U'▀', U'▄');
    case U'▄': return pair(U'▄', U'▀');
    case U'▌': return pair(U'▌', U'▐');
    case U'▐': return pair(U'▐', U'▌');
    case U'░': if (swapShades) return pair(U'░', U'▓'); break;
    case U'▓': if (swapShades) return pair(U'▓', U'░'); break;
    case U'▒': if (swapShades) return pair(U'▒', U'▒'); break;
    default: break;
    }
    const Way way{cell.ch, f, b};
    return Ways{{{way, way}}, 1};
}

bool asciiDigit(char32_t ch) { return ch >= U'0' && ch <= U'9'; }

// A run of formatting codes: the longest is six bytes.
struct Code {
    std::array<char, 8> buf{};
    std::uint8_t len = 0;

    void push(char b) { buf[len++] = b; }

    // A colour number (kDefault as 99), with a leading zero only when `next`
    // is a digit.
    void number(int n, char32_t next) {
        if (n == kDefault)
            n = 99;
        if (n >= 10 || asciiDigit(next))
            push(static_cast<char>('0' + n / 10));
        push(static_cast<char>('0' + n % 10));
    }

    static Code colour(int fg, int bg, char32_t next) {
        Code c;
        c.push('\x03');
        if (bg != kDefault) {
            c.number(fg, U',');
            c.push(',');
            c.number(bg, next);
        } else {
            c.number(fg, next);
        }
        return c;
    }
};

struct Move {
    Code code;
    State state;
};

// The ways to get from `cur` to colours that draw `way`: (codes, new state).
void moves(State cur, const Way& way, std::vector<Move>& out) {
    out.clear();
    const char32_t g = way.glyph;
    const bool bgChange = way.bg != kDefault && cur.bg != way.bg;
    const bool fgChange = way.fg != kDefault && cur.fg != way.fg;
    if (bgChange) {
        // a background needs a foreground number in front of it; when the
        // glyph doesn't care which, try the one already set and short ones
        const int b = way.bg;
        std::array<int, 3> fgs = {way.fg != kDefault ? way.fg : cur.fg, kDefault, kDefault};
        if (way.fg == kDefault) {
            fgs[1] = b;
            if (cur.fg != kDefault && cur.fg >= 10)
                fgs[2] = 1;
        }
        for (size_t i = 0; i < fgs.size(); ++i) {
            const int f = fgs[i];
            if (i > 0 && (f == kDefault || std::find(fgs.begin(), fgs.begin() + i, f) != fgs.begin() + i))
                continue;
            out.push_back({Code::colour(f, b, g), {f, b}});
        }
        return;
    }
    if (!fgChange) {
        out.push_back({Code{}, cur});
        return;
    }
    const int f = way.fg;
    if (g == U',') {
        // "\x034," would read the comma as the start of a background
        if (cur.bg != kDefault)
            out.push_back({Code::colour(f, cur.bg, g), {f, cur.bg}});
        Code c = Code::colour(f, kDefault, U'x');
        c.push('\x02');
        c.push('\x02');
        out.push_back({c, {f, cur.bg}});
    } else {
        out.push_back({Code::colour(f, kDefault, g), {f, cur.bg}});
    }
}

// A trailing space needs a reset after it, or clients that strip trailing
// whitespace lose its background.
constexpr char kEndGuard = '\x0f';

// The colour state, and whether the last glyph was a space.
struct Key {
    State state;
    bool space = false;
    auto operator<=>(const Key&) const = default;
};

// Any state is one colour code (at most 6 bytes) from any other, so an entry
// further than that behind the best can't win.
constexpr detail::Cost kSlack = 6;

struct Entry {
    Key key;
    // 64 bits: a cell costs at most 10 bytes, so no row that fits in memory
    // reaches the top
    detail::Cost cost = 0;
    // An index into the layer before, which holds at most one entry per key:
    // 100 foregrounds (the 99 and the default) x 100 backgrounds x 2.
    std::uint32_t prev = 0;
    Code code;
    char32_t glyph = U' ';
};

std::uint32_t utf8Length(char32_t ch) { return ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4; }

bool scalar(char32_t ch) { return ch <= 0x10ffff && (ch < 0xd800 || ch > 0xdfff); }

} // namespace

std::uint8_t nearestMirc(std::uint32_t rgb) {
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    std::uint8_t best = 1;
    int bestD = INT_MAX;
    for (std::uint8_t i = 0; i < kMircRgb.size(); ++i) {
        const std::uint32_t c = kMircRgb[i];
        const int dr = int((c >> 16) & 255) - r, dg = int((c >> 8) & 255) - g, db = int(c & 255) - b;
        const int d = dr * dr + dg * dg + db * db;
        if (d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

std::string encodeMircRow(std::span<const MircCell> cells, bool swapShades, bool solidEnd) {
    return detail::encodeMircRowFrom(cells, swapShades, 0, solidEnd);
}

bool detail::withinSlack(Cost cost, Cost best) { return cost - best <= kSlack; }

std::string detail::encodeMircRowFrom(std::span<const MircCell> cells, bool swapShades, Cost start, bool solidEnd) {
    for (const MircCell& c : cells)
        if (!scalar(c.ch) || c.fg >= kMircRgb.size() || c.bg >= kMircRgb.size())
            throw IrcError("the IRC encoder was given an invalid cell");
    if (cells.empty())
        return {};
    // One layer of reachable (state, cost) entries per cell, each pointing
    // back at the entry in the layer before it that it was reached from.
    std::vector<std::vector<Entry>> layers;
    layers.reserve(cells.size());
    Entry first;
    first.cost = start;
    std::vector<Entry> cur{first};
    std::vector<Entry> next;
    std::vector<Move> mv;
    mv.reserve(3);
    for (const MircCell& cell : cells) {
        Ways options = ways(cell, swapShades);
        // solid() lists the space first
        if (solidEnd && &cell == &cells.back() && options.n == 2 && options.way[0].glyph == U' ') {
            options.way[0] = options.way[1];
            options.n = 1;
        }
        next.clear();
        for (size_t i = 0; i < cur.size(); ++i) {
            const Entry& e = cur[i];
            for (int w = 0; w < options.n; ++w) {
                const Way& way = options.way[w];
                moves(e.key.state, way, mv);
                for (const Move& m : mv) {
                    const detail::Cost cost = e.cost + m.code.len + utf8Length(way.glyph);
                    const Key key{m.state, way.glyph == U' '};
                    const Entry entry{key, cost, static_cast<std::uint32_t>(i), m.code, way.glyph};
                    const auto x = std::find_if(next.begin(), next.end(), [&](const Entry& n) { return n.key == key; });
                    if (x == next.end())
                        next.push_back(entry);
                    else if (cost < x->cost)
                        *x = entry;
                }
            }
        }
        detail::Cost best = next.front().cost;
        for (const Entry& e : next)
            best = std::min(best, e.cost);
        std::erase_if(next, [&](const Entry& e) { return !detail::withinSlack(e.cost, best); });
        std::swap(cur, next);
        layers.push_back(cur);
    }
    const auto finish = [](const Entry& e) { return e.cost + (e.key.space ? 1 : 0); };
    const auto last = std::min_element(cur.begin(), cur.end(), [&](const Entry& a, const Entry& b) {
        return std::pair(finish(a), a.key) < std::pair(finish(b), b.key);
    });
    // Walk back from the cheapest last entry, then write the pieces in order.
    std::vector<const Entry*> path(layers.size());
    std::uint32_t at = static_cast<std::uint32_t>(last - cur.begin());
    for (size_t l = layers.size(); l-- > 0;) {
        path[l] = &layers[l][at];
        at = path[l]->prev;
    }
    std::string line;
    line.reserve(static_cast<size_t>(finish(*last) - start));
    for (const Entry* e : path) {
        line.append(e->code.buf.data(), e->code.len);
        appendUtf8(line, e->glyph);
    }
    if (last->key.space)
        line += kEndGuard;
    return line;
}

} // namespace bw::irc
