// The Micron target (PLAN §17): the 99 swatches and the 4096 colours of the
// picker, what a page writes and what lint says about it.

#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/lint_rules.h"
#include "blockwright/micron.h"
#include "blockwright/palette.h"
#include "blockwright/target.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>

using namespace bw;
using namespace bw::lint;

namespace {

Canvas oneCell(char32_t ch, Color fg, Color bg) {
    Canvas c(1, 1);
    c.at(0, 0).ch = ch;
    c.at(0, 0).fg = fg;
    c.at(0, 0).bg = bg;
    return c;
}

std::vector<Finding> of(const std::vector<Finding>& all, Finding::Kind k) {
    std::vector<Finding> out;
    std::copy_if(all.begin(), all.end(), std::back_inserter(out), [k](const Finding& f) { return f.kind == k; });
    return out;
}

std::string hex6(Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

} // namespace

TEST_CASE("the Micron target has a name and is read back from it", "[target][micron]") {
    CHECK(targetName(Target::Micron) == "micron");
    CHECK(parseTarget("micron") == Target::Micron);
    CHECK(parseTarget("Micron") == Target::Micron);
    CHECK_FALSE(parseTarget("mu"));
    CHECK_FALSE(parseTarget("nomadnet"));
    CHECK(richTarget(Target::Micron));
    CHECK(richTarget(Target::Irc));
    CHECK_FALSE(richTarget(Target::Ansi));
}

TEST_CASE("Micron has IRC's 99 swatches, and a colour none of them has is no swatch", "[target][micron]") {
    CHECK(swatchCount(Target::Micron) == 99);
    for (int i = 0; i < 99; ++i) {
        INFO("swatch " << i);
        CHECK(swatchOf(swatchColor(i), Target::Micron) == swatchOf(swatchColor(i), Target::Irc));
    }
    CHECK(swatchOf(swatchColor(77), Target::Micron) == 77);
    const Color picked = Color::fromRgb(micron::twelveBit(1, 2, 3));
    CHECK(swatchOf(picked, Target::Micron) == -1);
    // IRC and ANSI read it as the palette entry nearest it
    CHECK(swatchOf(picked, Target::Irc) == picked.paletteIndex());
    CHECK(swatchOf(picked, Target::Ansi) == picked.paletteIndex());
}

TEST_CASE("the twelve-bit colours are the 4096 a three-digit code writes", "[micron]") {
    int count = 0;
    for (int r = 0; r < 16; ++r)
        for (int g = 0; g < 16; ++g)
            for (int b = 0; b < 16; ++b) {
                const Rgb c = micron::twelveBit(r, g, b);
                CHECK(c.r == r * 17);
                CHECK(c.g == g * 17);
                CHECK(c.b == b * 17);
                CHECK(micron::isTwelveBit(c));
                const char digits[] = "0123456789abcdef";
                CHECK(micron::colourCode(c) == std::string{digits[r], digits[g], digits[b]});
                ++count;
            }
    CHECK(count == 4096);
    int channel = 0;
    for (int v = 0; v < 256; ++v)
        channel += micron::isTwelveBit({std::uint8_t(v), 0, 0}) ? 1 : 0;
    CHECK(channel == 16);
    CHECK_FALSE(micron::isTwelveBit({0x12, 0x00, 0x00}));
    CHECK_FALSE(micron::isTwelveBit({0x00, 0x00, 0xfe}));
    // out of range digits are clamped
    CHECK(micron::twelveBit(-1, 16, 7) == Rgb{0x00, 0xff, 0x77});
}

TEST_CASE("Micron writes every mIRC colour exactly, in three digits or in six", "[micron][target]") {
    for (int i = 0; i < 99; ++i) {
        INFO("swatch " << i);
        const Color c = swatchColor(i);
        const Rgb rgb = c.isIndexed() ? kPalette[c.index] : c.rgb;
        const std::string code = micron::colourCode(rgb);
        if (micron::isTwelveBit(rgb)) {
            CHECK(code.size() == 3);
        } else {
            CHECK(code == "T" + hex6(rgb));
        }
    }
}

TEST_CASE("under Micron an RGB background isn't folded by iCE, on the canvas or the page", "[micron][target]") {
    Cell rgb;
    rgb.ch = U'X';
    rgb.fg = Color::indexed(7);
    rgb.bg = Color::fromRgb({0xff, 0x40, 0x40});  // nearest the bright red
    CHECK(irc::sentCell(rgb, false, Target::Micron) == rgb);
    CHECK(irc::sentCell(rgb, false, Target::Ansi) != rgb);
    const Canvas c = oneCell(U'X', Color::indexed(7), Color::fromRgb({0xff, 0x40, 0x40}));
    CHECK(micron::encodeLine(c, 0, kPalette, false) == "`Faaa`BTff4040X``");
}

TEST_CASE("under Micron lint keeps the craft rules and drops the IRC byte rules", "[micron][target][lint]") {
    // black to the edge of a row, blank rows above and below, and bold on a
    // blank: what IRC drops or pays for, and what a page keeps
    const Cell space{U' ', Color::indexed(7), Color::indexed(0), 0, false};
    const Cell solid{U'█', Color::indexed(15), Color::indexed(0), 0, false};
    Canvas c(10, 6);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 10; ++x)
            c.at(x, y) = space;
    for (int x = 0; x < 10; ++x)
        c.at(x, 1) = c.at(x, 3) = solid;
    for (int x = 0; x < 4; ++x)
        c.at(x, 2) = solid;
    c.at(5, 2).attrs = attr::Bold;
    c.at(9, 1).ch = U'€';  // no CP437 code

    RuleOptions irc;
    irc.target = Target::Irc;
    const auto i = checkRules(c, irc);
    REQUIRE_FALSE(of(i, Finding::Kind::TrailingBlack).empty());
    REQUIRE_FALSE(of(i, Finding::Kind::BlankEdgeRows).empty());
    REQUIRE_FALSE(of(i, Finding::Kind::WastedFormat).empty());

    RuleOptions mu;
    mu.target = Target::Micron;
    const auto m = checkRules(c, mu);
    CHECK(of(m, Finding::Kind::TrailingBlack).empty());
    CHECK(of(m, Finding::Kind::BlankEdgeRows).empty());
    CHECK(of(m, Finding::Kind::WastedFormat).empty());
    CHECK(of(m, Finding::Kind::NoCp437).empty());
    const auto loss = of(m, Finding::Kind::AnsLoss);
    REQUIRE(loss.size() == 2);
    CHECK(loss[0].message == "1 cell holds a glyph a CP437 .ans saves as '?' (fine for Micron)");
    CHECK(loss[1].message == "1 cell has bold, italic or underline, which an .ans leaves out (fine for Micron)");

    // and under ANSI it is no-cp437 again
    const auto a = checkRules(c, RuleOptions{});
    CHECK(of(a, Finding::Kind::NoCp437).size() == 1);
    CHECK(of(a, Finding::Kind::AnsLoss).empty());
}


TEST_CASE("micron-colour notes the cells a page writes in the truecolour form", "[micron][lint]") {
    const Color twelve = Color::fromRgb(micron::twelveBit(10, 5, 0));  // aa5500, written `Fa50
    const Color odd = Color::fromRgb({0x12, 0x34, 0x56});              // written `FT123456
    Canvas c(6, 1);
    c.at(0, 0).ch = U'X';
    c.at(0, 0).fg = twelve;
    c.at(1, 0).ch = U'X';
    c.at(1, 0).fg = odd;  // shown: noted
    c.at(2, 0).ch = U' ';
    c.at(2, 0).fg = odd;  // a blank shows no foreground
    c.at(3, 0).ch = U'█';
    c.at(3, 0).bg = odd;  // a full block covers its background
    c.at(4, 0).ch = U'X';
    c.at(4, 0).bg = odd;  // shown: noted
    c.at(5, 0).ch = U'X';
    c.at(5, 0).bg = twelve;

    // the cells and the encoder agree: truecolour exactly where noted
    CHECK(micron::truecolourCells(c, kPalette, true) == std::vector<std::pair<int, int>>{{1, 0}, {4, 0}});
    const std::string page = micron::encodeCanvas(c, kPalette, true);
    CHECK(page.find("`FT123456") != std::string::npos);
    CHECK(page.find("`BT123456") != std::string::npos);
    CHECK(page.find("`Fa50") != std::string::npos);

    RuleOptions mu;
    mu.target = Target::Micron;
    const auto notes = of(checkRules(c, mu), Finding::Kind::MicronColour);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].x == 1);
    CHECK(notes[0].y == 0);
    CHECK(notes[0].message ==
          "2 cells are in colours three hex digits can't write: the page writes them exactly, as truecolour "
          "(`FTrrggbb or `BTrrggbb), not a twelve-bit `Frgb or `Brgb");
    CHECK(noteKind(Finding::Kind::MicronColour));
    CHECK(kindName(Finding::Kind::MicronColour) == "micron-colour");

    // only under Micron
    RuleOptions irc;
    irc.target = Target::Irc;
    CHECK(of(checkRules(c, irc), Finding::Kind::MicronColour).empty());
    CHECK(of(checkRules(c, RuleOptions{}), Finding::Kind::MicronColour).empty());

    // twelve-bit colours only: no note, and the page has no truecolour code
    c.at(1, 0).fg = twelve;
    c.at(4, 0).bg = twelve;
    CHECK(micron::truecolourCells(c, kPalette, true).empty());
    const std::string clean = micron::encodeCanvas(c, kPalette, true);
    CHECK(clean.find("`FT") == std::string::npos);
    CHECK(clean.find("`BT") == std::string::npos);
    CHECK(of(checkRules(c, mu), Finding::Kind::MicronColour).empty());
}

TEST_CASE("micron-colour reads an indexed colour from the document's palette", "[micron][lint]") {
    Canvas c(1, 1);
    c.at(0, 0).ch = U'X';
    c.at(0, 0).fg = Color::indexed(4);
    RuleOptions mu;
    mu.target = Target::Micron;
    // every colour of the 16 is twelve-bit
    CHECK(std::all_of(kPalette.begin(), kPalette.end(), micron::isTwelveBit));
    CHECK(of(checkRules(c, mu), Finding::Kind::MicronColour).empty());
    // an XBIN's own colour isn't, and the page writes it as truecolour
    std::array<Rgb, 16> pal = kPalette;
    pal[4] = {0x12, 0x34, 0x56};
    mu.palette = pal;
    REQUIRE(of(checkRules(c, mu), Finding::Kind::MicronColour).size() == 1);
    CHECK(micron::encodeCanvas(c, pal, true).find("`FT123456") != std::string::npos);
    LintOptions lo;
    lo.target = Target::Micron;
    lo.palette = pal;
    CHECK(of(lintCanvas(c, lo).findings, Finding::Kind::MicronColour).size() == 1);
}

TEST_CASE("micron-glyph notes the cells a page writes as '?'", "[micron][lint]") {
    Canvas c(4, 1);
    c.at(0, 0).ch = U'A';
    c.at(1, 0).ch = U'☝';  // NomadNet draws it as a space: dropped
    c.at(2, 0).ch = U'B';
    c.at(3, 0).ch = U'☝';
    REQUIRE(micron::unwritable(c).size() == 2);
    RuleOptions mu;
    mu.target = Target::Micron;
    const auto notes = of(checkRules(c, mu), Finding::Kind::MicronGlyph);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].x == 1);
    CHECK(notes[0].message == "2 cells hold glyphs a Micron page writes as '?': NomadNet strips their code points "
                              "from a page, or IRC text can't write them");
    CHECK(noteKind(Finding::Kind::MicronGlyph));
    CHECK(kindName(Finding::Kind::MicronGlyph) == "micron-glyph");
    RuleOptions irc;
    irc.target = Target::Irc;
    CHECK(of(checkRules(c, irc), Finding::Kind::MicronGlyph).empty());

    // notes: the report is clean
    LintOptions lo;
    lo.target = Target::Micron;
    c.at(0, 0).fg = Color::fromRgb({0x12, 0x34, 0x56});
    const LintReport r = lintCanvas(c, lo);
    CHECK(r.clean());
    CHECK(summary(r) == "clean, 4 notes (2 ans-loss, 1 micron-colour, 1 micron-glyph), noise 0.0000, iCE on");
}
