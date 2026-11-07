// mIRC colours and the row encoder (mirc.h).

#include "blockwright/irc.h"
#include "blockwright/mirc.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <tuple>

using namespace bw;
using irc::MircCell;

namespace {

std::vector<MircCell> cells(std::initializer_list<MircCell> spec) { return spec; }

// A cell as a client draws it; nullopt is the client's default colour.
struct Drawn {
    char32_t ch;
    std::optional<int> fg, bg;
};

std::vector<char32_t> codePoints(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const int n = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
        char32_t cp = n == 1 ? b : b & (0x3f >> (n - 1));
        for (int k = 1; k < n; ++k)
            cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

// The cells an IRC line draws, reading colour codes as clients do and
// skipping other formatting.
std::vector<Drawn> decode(const std::string& line) {
    const std::vector<char32_t> c = codePoints(line);
    const auto digit = [&](size_t i) { return i < c.size() && c[i] >= U'0' && c[i] <= U'9'; };
    const auto digits = [&](size_t& i) {
        std::optional<int> n;
        for (int k = 0; k < 2 && digit(i); ++k, ++i)
            n = n.value_or(0) * 10 + int(c[i] - U'0');
        return n;
    };
    const auto colour = [](std::optional<int> n) { return n && *n < 99 ? n : std::nullopt; };
    std::optional<int> fg, bg;
    std::vector<Drawn> out;
    for (size_t i = 0; i < c.size();) {
        switch (c[i]) {
        case U'\x03':
            ++i;
            if (const auto f = digits(i)) {
                fg = colour(f);
                if (i + 1 < c.size() && c[i] == U',' && digit(i + 1)) {
                    ++i;
                    bg = colour(digits(i));
                }
            } else {
                fg = bg = std::nullopt;
            }
            break;
        case U'\x0f':
            fg = bg = std::nullopt;
            ++i;
            break;
        case U'\x02':
        case U'\x11':
        case U'\x16':
        case U'\x1d':
        case U'\x1e':
        case U'\x1f': ++i; break;
        default: out.push_back({c[i++], fg, bg});
        }
    }
    return out;
}

// How a cell looks, so two drawings of it compare equal: (kind, glyph,
// colours).
using Look = std::tuple<int, char32_t, std::optional<int>, std::optional<int>>;

Look look(char32_t ch, std::optional<int> fg, std::optional<int> bg) {
    enum { Solid, Half, Split, Glyph };
    const auto two = [](int kind, std::optional<int> a, std::optional<int> b) {
        return a == b ? Look{Solid, 0, a, {}} : Look{kind, 0, a, b};
    };
    if (ch == U' ' || ch == U' ')
        return {Solid, 0, bg, {}};
    if (ch == U'█' || (fg == bg && fg))
        return {Solid, 0, fg, {}};
    switch (ch) {
    case U'▀': return two(Half, fg, bg);
    case U'▄': return two(Half, bg, fg);
    case U'▌': return two(Split, fg, bg);
    case U'▐': return two(Split, bg, fg);
    default: return {Glyph, ch, fg, bg};
    }
}

void sameLook(const std::vector<MircCell>& row, const std::string& line) {
    INFO(line);
    const std::vector<Drawn> got = decode(line);
    REQUIRE(got.size() == row.size());
    for (size_t i = 0; i < row.size(); ++i) {
        INFO("cell " << i);
        CHECK(look(row[i].ch.codePoint().value(), row[i].fg, row[i].bg) == look(got[i].ch, got[i].fg, got[i].bg));
    }
}

int distance2(std::uint32_t a, std::uint32_t b) {
    const auto d = [&](int shift) { return int((a >> shift) & 255) - int((b >> shift) & 255); };
    return d(16) * d(16) + d(8) * d(8) + d(0) * d(0);
}

} // namespace

TEST_CASE("encodeMircRow writes one digit unless a digit follows") {
    const auto row = cells({{U'a', 4, 1}, {U'7', 9, 1}});
    const std::string line = irc::encodeMircRow(row);
    CHECK(line == "\x03" "4,1a\x03" "097");
    sameLook(row, line);
}

TEST_CASE("encodeMircRow swaps spaces and blocks") {
    // a red block between red-background spaces needs no colour change
    const auto row = cells({{U' ', 15, 4}, {U'█', 4, 1}, {U' ', 15, 4}});
    const std::string line = irc::encodeMircRow(row);
    CHECK(line == "\x03" "4,4   \x0f");
    sameLook(row, line);
}

TEST_CASE("encodeMircRow swaps half blocks") {
    const auto row = cells({{U'▀', 4, 2}, {U'▄', 2, 4}, {U'▀', 4, 2}});
    const std::string line = irc::encodeMircRow(row);
    CHECK(line == "\x03" "2,4▄▄▄");
    sameLook(row, line);
}

TEST_CASE("encodeMircRow keeps a comma after a colour from reading as a background") {
    const auto row = cells({{U'a', 4, 1}, {U',', 9, 1}});
    sameLook(row, irc::encodeMircRow(row));
    // where repeating the background is longer, ^B^B ends the number
    const auto wide = cells({{U'a', 4, 12}, {U',', 9, 12}});
    const std::string line = irc::encodeMircRow(wide);
    CHECK(line == "\x03" "4,12a\x03" "9\x02\x02,");
    sameLook(wide, line);
}

TEST_CASE("encodeMircRow keeps a trailing space's background") {
    const auto row = cells({{U'x', 4, 2}, {U' ', 0, 2}});
    const std::string line = irc::encodeMircRow(row);
    CHECK((line.ends_with(' ') || line.ends_with("█") || line.ends_with('\x0f')));
    sameLook(row, line);
}

TEST_CASE("encodeMircRow swaps shades only when asked") {
    const auto row = cells({{U'▓', 2, 4}, {U'░', 4, 2}, {U'▒', 4, 2}, {U'▒', 2, 4}});
    CHECK(irc::encodeMircRow(row) == "\x03" "2,4▓\x03" "4,2░▒\x03" "2,4▒");
    CHECK(irc::encodeMircRow(row, true) == "\x03" "2,4▓▓▒▒");
}

TEST_CASE("encodeMircRow keeps every entry within six bytes of the best") {
    // the cheapest line goes through an entry three bytes behind after the
    // second cell (13 against 16), so a slack under three loses it; the
    // six-byte bound itself is pinned by the withinSlack checks
    CHECK(irc::encodeMircRow(cells({{U'▀', 1, 10}, {U'▀', 4, 10}, {U',', 10, 4}})) ==
          "\x03" "1,10▀\x03" "10,4▄,");
}

TEST_CASE("encodeMircRow tries foreground 1 when a background changes under a two-digit foreground") {
    // the space after the 'a' shows only its background, and the cell after it
    // asks for no other colour, so 1 is the cheapest foreground to write
    CHECK(irc::encodeMircRow(cells({{U'a', 10, 12}, {U' ', 1, 14}, {U'\u00a0', 14, 14}})) ==
          "\x03" "10,12a\x03" "1,14  \x0f");
}

TEST_CASE("encodeMircRow keeps the first of equally short ways") {
    // as written a space and a block cost the same here, and the space comes
    // first
    const std::vector<MircCell> row = cells({{U' ', 1, 1}, {U',', 4, 2}});
    CHECK(irc::detail::encodeMircRowWith(row, false, false, irc::detail::Objective::Written).line ==
          "\x03" "1,1 \x03" "4,2,");
    // as sent the block is a byte shorter: the script writes no background
    // for it
    CHECK(irc::encodeMircRow(row) == "\x03" "1\xE2\x96\x88\x03" "4,2,");
}

TEST_CASE("encodeMircRow's byte count can't wrap") {
    // four-byte glyphs in alternating colours cost ten bytes a cell: the
    // count passes 2^32 here, where a 32-bit count pruned every entry
    std::vector<MircCell> row;
    for (int i = 0; i < 8; ++i)
        row.push_back({U'\U0001F600', std::uint8_t(i % 2 ? 12 : 10), std::uint8_t(i % 2 ? 13 : 11)});
    const std::string line = irc::encodeMircRow(row);
    CHECK(irc::detail::encodeMircRowFrom(row, false, (std::uint64_t(1) << 32) - 16) == line);
    // and the pruning holds at the top of the 64-bit range
    CHECK(irc::detail::encodeMircRowFrom(std::span(row).first(1), false, UINT64_MAX - 15) ==
          irc::encodeMircRow(std::span(row).first(1)));
    CHECK(irc::detail::withinSlack(UINT64_MAX, UINT64_MAX - 6));
    CHECK_FALSE(irc::detail::withinSlack(UINT64_MAX, UINT64_MAX - 7));
    CHECK(irc::detail::withinSlack(6, 0));
    CHECK_FALSE(irc::detail::withinSlack(7, 0));
}

TEST_CASE("encodeMircRow refuses cells it cannot write") {
    CHECK(irc::encodeMircRow(std::vector<MircCell>{}).empty());
    CHECK_THROWS_AS(irc::encodeMircRow(cells({{U'a', 99, 1}})), irc::IrcError);
    CHECK_THROWS_AS(irc::encodeMircRow(cells({{U'a', 4, 99}})), irc::IrcError);
    CHECK_THROWS_AS(irc::encodeMircRow(cells({{char32_t(0xd800), 4, 1}})), irc::IrcError);
    // a continuation with no lead: first, or right of another continuation
    // (0x110000 was this test's stray value; a Glyph makes it U+FFFD now)
    CHECK_THROWS_AS(irc::encodeMircRow(cells({{bw::Glyph::continuation(), 4, 1}})), irc::IrcError);
    CHECK_THROWS_AS(irc::encodeMircRow(cells({{U'a', 4, 1}, {bw::Glyph::continuation(), 4, 1},
                                              {bw::Glyph::continuation(), 4, 1}})),
                    irc::IrcError);
    // the other C0 controls and C1 are scalar values: written as they are
    CHECK(irc::encodeMircRow(cells({{char32_t(0x85), 4, 1}})) == "\x03" "4,1\xc2\x85");
}

TEST_CASE("nearestMirc picks the first of equally near colours") {
    for (std::uint8_t i = 0; i < irc::kMircRgb.size(); ++i) {
        INFO("colour " << int(i));
        const auto first = std::find(irc::kMircRgb.begin(), irc::kMircRgb.end(), irc::kMircRgb[i]);
        CHECK(irc::nearestMirc(irc::kMircRgb[i]) == first - irc::kMircRgb.begin());
    }
    CHECK(irc::nearestMirc(0xff000000u | 0x0000fc) == 12);
    CHECK(irc::nearestMirc(0x010101) == 1);
}

TEST_CASE("each dark palette colour is its nearest of mIRC's first sixteen") {
    for (std::uint8_t i = 0; i < 8; ++i) {
        INFO("colour " << int(i));
        const Rgb pc = kPalette[i];
        const std::uint32_t rgb = std::uint32_t(pc.r) << 16 | std::uint32_t(pc.g) << 8 | pc.b;
        std::uint8_t nearest = 0;
        for (std::uint8_t c = 1; c < 16; ++c)
            if (distance2(irc::kMircRgb[c], rgb) < distance2(irc::kMircRgb[nearest], rgb))
                nearest = c;
        CHECK(irc::mircColor(Color::indexed(i)) == nearest);
    }
}

TEST_CASE("bright and dark palette colours are different mIRC colours") {
    std::vector<int> all;
    for (std::uint8_t i = 0; i < 8; ++i) {
        CHECK(irc::mircColor(Color::indexed(i)) != irc::mircColor(Color::indexed(i + 8)));
        all.push_back(irc::mircColor(Color::indexed(i)));
        all.push_back(irc::mircColor(Color::indexed(i + 8)));
    }
    std::sort(all.begin(), all.end());
    for (int i = 0; i < 16; ++i)
        CHECK(all[size_t(i)] == i);
}
