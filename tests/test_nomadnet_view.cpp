// The NomadNet view (nomadnet_view.h): a page's colours as NomadNet 1.5.0
// draws them in its 256-colour mode, and the page's cells as Export Micron
// writes them.
//
// Every expected index and colour below was read from urwid itself (4.2.5;
// 3.0.5's tables and functions are the same): the "#rrggbb" that
// MicronParser.make_style's high_color makes, through
// AttrSpec(fg, bg, 256).foreground_number and _COLOR_VALUES_256 at it.

#include "blockwright/micron.h"
#include "blockwright/nomadnet_view.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace bw;

namespace {

struct Mapped {
    Rgb in;
    int index;
    Rgb shown;
};

void checkAll(const std::vector<Mapped>& cases) {
    for (const Mapped& m : cases) {
        CAPTURE(int(m.in.r), int(m.in.g), int(m.in.b));
        CHECK(nomadnet::index256(m.in) == m.index);
        CHECK(nomadnet::colour256(m.in) == m.shown);
    }
}

} // namespace

TEST_CASE("NomadNet at 256 colours draws the standard 16 as urwid maps them") {
    REQUIRE(kPalette.size() == 16);
    checkAll({
        {kPalette[0], 16, {0x00, 0x00, 0x00}},  {kPalette[1], 19, {0x00, 0x00, 0xaf}},
        {kPalette[2], 34, {0x00, 0xaf, 0x00}},  {kPalette[3], 37, {0x00, 0xaf, 0xaf}},
        {kPalette[4], 124, {0xaf, 0x00, 0x00}}, {kPalette[5], 127, {0xaf, 0x00, 0xaf}},
        {kPalette[6], 130, {0xaf, 0x5f, 0x00}}, {kPalette[7], 145, {0xaf, 0xaf, 0xaf}},
        {kPalette[8], 59, {0x5f, 0x5f, 0x5f}},  {kPalette[9], 63, {0x5f, 0x5f, 0xff}},
        {kPalette[10], 83, {0x5f, 0xff, 0x5f}}, {kPalette[11], 87, {0x5f, 0xff, 0xff}},
        {kPalette[12], 203, {0xff, 0x5f, 0x5f}}, {kPalette[13], 207, {0xff, 0x5f, 0xff}},
        {kPalette[14], 227, {0xff, 0xff, 0x5f}}, {kPalette[15], 231, {0xff, 0xff, 0xff}},
    });
}

TEST_CASE("NomadNet at 256 colours puts greys in the cube, never the grey ramp") {
    checkAll({
        {{0x08, 0x08, 0x08}, 16, {0x00, 0x00, 0x00}},  // the ramp's own first grey (232)
        {{0x22, 0x22, 0x22}, 16, {0x00, 0x00, 0x00}},
        {{0x33, 0x33, 0x33}, 59, {0x5f, 0x5f, 0x5f}},
        {{0x66, 0x66, 0x66}, 59, {0x5f, 0x5f, 0x5f}},
        {{0x77, 0x77, 0x77}, 102, {0x87, 0x87, 0x87}},
        {{0x7f, 0x7f, 0x7f}, 102, {0x87, 0x87, 0x87}},
        {{0x80, 0x80, 0x80}, 102, {0x87, 0x87, 0x87}},  // the ramp has 244 for it
        {{0x99, 0x99, 0x99}, 102, {0x87, 0x87, 0x87}},
        {{0xbb, 0xbb, 0xbb}, 145, {0xaf, 0xaf, 0xaf}},
        {{0xcc, 0xcc, 0xcc}, 188, {0xd7, 0xd7, 0xd7}},
        {{0xdd, 0xdd, 0xdd}, 188, {0xd7, 0xd7, 0xd7}},
        {{0xee, 0xee, 0xee}, 231, {0xff, 0xff, 0xff}},  // the ramp's last grey (255)
        {{0x26, 0x26, 0x26}, 16, {0x00, 0x00, 0x00}},
    });
}

TEST_CASE("NomadNet at 256 colours reads each channel's high nibble at the cube's boundaries") {
    // each step changes between nibble 2|3, 6|7, 9|10 (a), 11|12 (c), 13|14 (e)
    checkAll({
        {{0x2f, 0x00, 0x00}, 16, {0x00, 0x00, 0x00}},  {{0x30, 0x00, 0x00}, 52, {0x5f, 0x00, 0x00}},
        {{0x00, 0x6f, 0x00}, 22, {0x00, 0x5f, 0x00}},  {{0x00, 0x70, 0x00}, 28, {0x00, 0x87, 0x00}},
        {{0x00, 0x00, 0x9f}, 18, {0x00, 0x00, 0x87}},  {{0x00, 0x00, 0xa0}, 19, {0x00, 0x00, 0xaf}},
        {{0xbf, 0x00, 0x00}, 124, {0xaf, 0x00, 0x00}}, {{0xc0, 0x00, 0x00}, 160, {0xd7, 0x00, 0x00}},
        {{0x00, 0xdf, 0x00}, 40, {0x00, 0xd7, 0x00}},  {{0x00, 0xe0, 0x00}, 46, {0x00, 0xff, 0x00}},
        // truecolour, the low nibble ignored
        {{0x12, 0x34, 0x56}, 23, {0x00, 0x5f, 0x5f}},  {{0xfe, 0xdc, 0xba}, 223, {0xff, 0xd7, 0xaf}},
    });
    // every nibble, one channel at a time: urwid's _CUBE_256_LOOKUP_16
    const int steps[16] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5};
    for (int n = 0; n < 16; ++n) {
        CAPTURE(n);
        const auto v = std::uint8_t(n * 17);
        CHECK(nomadnet::index256({v, 0, 0}) == 16 + 36 * steps[n]);
        CHECK(nomadnet::index256({0, v, 0}) == 16 + 6 * steps[n]);
        CHECK(nomadnet::index256({0, 0, v}) == 16 + steps[n]);
    }
}

TEST_CASE("the NomadNet view is the page's cells in the 256-colour mode's colours") {
    Canvas c(4, 2);
    auto set = [&](int x, int y, char32_t ch, Color fg, Color bg, std::uint8_t attrs = 0) {
        Cell& cell = c.at(x, y);
        cell.ch = ch;
        cell.fg = fg;
        cell.bg = bg;
        cell.attrs = attrs;
    };
    set(0, 0, U'A', Color::indexed(6), Color::indexed(1), attr::Bold);                    // brown on blue
    set(1, 0, U'B', Color::fromRgb({0x12, 0x34, 0x56}), Color::fromRgb({0x80, 0x80, 0x80}));
    set(2, 0, U'C', Color::indexed(15), Color::indexed(12));                               // bright red bg
    set(3, 0, 0x200b, Color::indexed(7), Color::indexed(0));                                // NomadNet drops it
    // a wide glyph NomadNet strips: a thumbs-up with a skin tone
    const std::vector<char32_t> thumb{0x1f44d, 0x1f3fb};
    c.at(0, 1).ch = *clusterPool().intern(thumb, PoolCap::Uncapped);
    c.at(1, 1).ch = Glyph::continuation();

    SECTION("with iCE on") {
        const Canvas v = nomadnet::view256(c, kPalette, true);
        REQUIRE(v.width() == 4);
        REQUIRE(v.height() == 2);
        CHECK(v.at(0, 0).ch == Glyph(U'A'));
        CHECK(v.at(0, 0).fg == Color::fromRgb({0xaf, 0x5f, 0x00}));
        CHECK(v.at(0, 0).bg == Color::fromRgb({0x00, 0x00, 0xaf}));
        CHECK(v.at(0, 0).attrs == attr::Bold);
        CHECK(v.at(1, 0).fg == Color::fromRgb({0x00, 0x5f, 0x5f}));
        CHECK(v.at(1, 0).bg == Color::fromRgb({0x87, 0x87, 0x87}));
        CHECK(v.at(2, 0).bg == Color::fromRgb({0xff, 0x5f, 0x5f}));
        // what the page writes for the glyphs it loses, as micron::unwritable lists them
        CHECK(v.at(3, 0).ch == Glyph(U'?'));
        CHECK(v.at(0, 1).ch == Glyph(U'?'));
        CHECK(v.at(1, 1).ch == Glyph(U' '));
        CHECK(micron::unwritable(c).size() == 2);
    }
    SECTION("with iCE off a bright background is its dark colour, as the page writes it") {
        const Canvas v = nomadnet::view256(c, kPalette, false);
        CHECK(v.at(2, 0).bg == Color::fromRgb({0xaf, 0x00, 0x00}));
        // an RGB background is kept
        CHECK(v.at(1, 0).bg == Color::fromRgb({0x87, 0x87, 0x87}));
    }
    SECTION("the 24-bit page keeps every colour") {
        const Canvas p = micron::pageCanvas(c, kPalette, true);
        CHECK(p.at(0, 0).fg == Color::fromRgb(kPalette[6]));
        CHECK(p.at(1, 0).fg == Color::fromRgb({0x12, 0x34, 0x56}));
        CHECK(p.at(3, 0).ch == Glyph(U'?'));
    }
}
