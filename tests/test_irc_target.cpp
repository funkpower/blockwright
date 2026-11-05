// The IRC target (#350): the 99 swatches, what is sent, what an .ans loses
// and what lint says about it.

#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/lint_rules.h"
#include "blockwright/mirc.h"
#include "blockwright/palette.h"
#include "blockwright/ramp.h"
#include "blockwright/target.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace bw;
using namespace bw::lint;
namespace fs = std::filesystem;

namespace {

Rgb mircRgb(int i) {
    const std::uint32_t v = irc::kMircRgb[i];
    return {std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)};
}

Canvas oneCell(char32_t ch, Color fg, Color bg) {
    Canvas c(1, 1);
    c.at(0, 0).ch = ch;
    c.at(0, 0).fg = fg;
    c.at(0, 0).bg = bg;
    return c;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        out.push_back(text.substr(at, nl - at));
        at = nl == std::string::npos ? text.size() : nl + 1;
    }
    return out;
}

} // namespace

TEST_CASE("a target has a name and is read back from it in any case", "[target]") {
    CHECK(targetName(Target::Ansi) == "ansi");
    CHECK(targetName(Target::Irc) == "irc");
    CHECK(parseTarget("irc") == Target::Irc);
    CHECK(parseTarget("IRC") == Target::Irc);
    CHECK(parseTarget("Ansi") == Target::Ansi);
    CHECK_FALSE(parseTarget("mirc"));
    CHECK_FALSE(parseTarget(""));
}

TEST_CASE("IRC has 99 swatches and ANSI 16, each found again by its colour", "[target]") {
    CHECK(swatchCount(Target::Ansi) == 16);
    CHECK(swatchCount(Target::Irc) == 99);
    for (int i = 0; i < 16; ++i) {
        CHECK(swatchColor(i) == Color::indexed(std::uint8_t(i)));
        CHECK(swatchOf(swatchColor(i), Target::Irc) == i);
        CHECK(swatchOf(swatchColor(i), Target::Ansi) == i);
    }
    // the six that share an RGB with one of 0-15 come back as the shorter code
    const std::pair<int, int> same[] = {{52, 4}, {54, 8}, {58, 11}, {62, 13}, {88, 1}, {98, 0}};
    for (int i = 16; i < 99; ++i) {
        INFO("mIRC " << i);
        const Color c = swatchColor(i);
        REQUIRE_FALSE(c.isIndexed());
        CHECK(c.rgb == mircRgb(i));
        const auto twin = std::find_if(std::begin(same), std::end(same), [i](auto p) { return p.first == i; });
        const int code = irc::mircColor(c);
        if (twin == std::end(same)) {
            CHECK(code == i);
            CHECK(swatchOf(c, Target::Irc) == i);
        } else {
            CHECK(code == twin->second);
        }
        // under ANSI an RGB colour is the palette entry an .ans saves it as
        CHECK(swatchOf(c, Target::Ansi) == c.paletteIndex());
    }
    // out of range is clamped
    CHECK(swatchColor(-1) == swatchColor(0));
    CHECK(swatchColor(200) == swatchColor(98));
}

TEST_CASE("mIRC colours past 15 send as their own code under IRC with iCE off", "[target][irc]") {
    for (auto [swatch, code] : {std::pair{52, "4"}, std::pair{77, "77"}, std::pair{98, "0"}}) {
        INFO("swatch " << swatch);
        const Canvas c = oneCell(U'X', Color::indexed(0), swatchColor(swatch));
        const auto lines = irc::exportCanvas(irc::sentCanvas(c, false, Target::Irc));
        CHECK(lines == std::vector<std::string>{std::string("\x03" "1,") + code + "X"});
    }
}

TEST_CASE("under IRC an RGB background isn't folded by iCE; an indexed one still is", "[target][irc]") {
    Cell rgb;
    rgb.ch = U'X';
    rgb.fg = Color::indexed(7);
    rgb.bg = Color::fromRgb({0xff, 0x40, 0x40});  // nearest the bright red
    CHECK(irc::sentCell(rgb, false, Target::Irc) == rgb);
    // under ANSI it folds to its dark colour, as an .ans would show it
    CHECK(irc::sentCell(rgb, false, Target::Ansi) != rgb);

    Cell bright = rgb;
    bright.bg = Color::indexed(10);
    CHECK(irc::sentCell(bright, false, Target::Irc) == irc::sentCell(bright, false, Target::Ansi));
    CHECK(irc::sentCell(bright, false, Target::Irc) != bright);
    // with iCE on nothing folds under either
    CHECK(irc::sentCell(rgb, true, Target::Irc) == rgb);
    CHECK(irc::sentCell(bright, true, Target::Ansi) == bright);
}

TEST_CASE("an .ans, XBIN or BIN counts the cells whose colour it quantises", "[target][ansi]") {
    const Color off = swatchColor(77);                     // no palette entry has it
    const Rgb red = kPalette[12];
    const Color exact = Color::fromRgb(red);                // a palette entry's own RGB
    auto count = [](const Canvas& c) {
        AnsiDocument d;
        d.canvas = c;
        const int ans = writeAnsi(d).coloursQuantised;
        CHECK(writeXbin(d).coloursQuantised == ans);
        CHECK(writeBin(d).coloursQuantised == ans);
        return ans;
    };
    CHECK(count(oneCell(U'X', off, Color::indexed(0))) == 1);
    CHECK(count(oneCell(U'X', Color::indexed(7), off)) == 1);
    CHECK(count(oneCell(U'X', off, off)) == 1);  // a cell, not a colour
    CHECK(count(oneCell(U'X', exact, exact)) == 0);
    // a colour that doesn't show isn't lost
    CHECK(count(oneCell(U' ', off, Color::indexed(0))) == 0);
    CHECK(count(oneCell(U'█', Color::indexed(7), off)) == 0);
    Canvas t = oneCell(U'X', off, off);
    t.at(0, 0).transparent = true;
    CHECK(count(t) == 0);
    // a wide pair is one glyph
    Canvas wide(2, 1);
    wide.at(0, 0).ch = U'\u4e2d';
    wide.at(0, 0).fg = off;
    wide.at(1, 0).ch = Glyph::continuation();
    wide.at(1, 0).fg = off;
    CHECK(quantisedColourCount(wide) == 1);

    AnsiDocument d;
    d.canvas = oneCell(U'X', off, Color::indexed(0));
    CHECK(writeAnsi(d).lossy());
    d.canvas = oneCell(U'X', exact, Color::indexed(0));
    CHECK_FALSE(writeAnsi(d).lossy());
}

TEST_CASE("under IRC lint reports what an .ans would lose instead of no-cp437", "[target][lint]") {
    Canvas c(4, 1);
    c.at(0, 0).ch = U'€';  // no CP437 code
    c.at(2, 0).ch = U'X';
    c.at(2, 0).fg = swatchColor(77);
    c.at(3, 0).ch = U'X';
    c.at(3, 0).fg = swatchColor(77);

    RuleOptions ansi;
    const auto a = checkRules(c, ansi);
    CHECK(std::any_of(a.begin(), a.end(), [](const Finding& f) { return f.kind == Finding::Kind::NoCp437; }));
    CHECK(std::none_of(a.begin(), a.end(), [](const Finding& f) { return f.kind == Finding::Kind::AnsLoss; }));

    RuleOptions irc;
    irc.target = Target::Irc;
    const auto i = checkRules(c, irc);
    CHECK(std::none_of(i.begin(), i.end(), [](const Finding& f) { return f.kind == Finding::Kind::NoCp437; }));
    std::vector<Finding> loss;
    std::copy_if(i.begin(), i.end(), std::back_inserter(loss), [](const Finding& f) { return f.kind == Finding::Kind::AnsLoss; });
    REQUIRE(loss.size() == 2);
    CHECK(kindName(Finding::Kind::AnsLoss) == "ans-loss");
    CHECK(loss[0].x == 0);
    CHECK(loss[0].message.find("1 cell holds a glyph") != std::string::npos);
    CHECK(loss[1].x == 2);
    CHECK(loss[1].message.find("2 cells are in colours") != std::string::npos);

    LintOptions lo;
    lo.target = Target::Irc;
    const LintReport r = lintCanvas(c, lo);
    CHECK(std::count_if(r.findings.begin(), r.findings.end(),
                        [](const Finding& f) { return f.kind == Finding::Kind::AnsLoss; }) == 2);

    // a canvas an .ans holds whole has nothing to report
    Canvas clean(2, 1);
    clean.at(0, 0).ch = U'X';
    const auto none = checkRules(clean, irc);
    CHECK(std::none_of(none.begin(), none.end(), [](const Finding& f) { return f.kind == Finding::Kind::AnsLoss; }));
}

TEST_CASE("ans-loss is a note: reported, but the report is clean", "[target][lint]") {
    CHECK(noteKind(Finding::Kind::AnsLoss));
    for (Finding::Kind k : {Finding::Kind::NoCp437, Finding::Kind::Merge, Finding::Kind::RungSkip, Finding::Kind::Noise})
        CHECK_FALSE(noteKind(k));

    // mIRC 77 only: the note alone
    Canvas c(2, 1);
    c.at(0, 0).ch = U'X';
    c.at(0, 0).fg = swatchColor(77);
    LintOptions lo;
    lo.target = Target::Irc;
    const LintReport r = lintCanvas(c, lo);
    REQUIRE(r.findings.size() == 1);
    CHECK(r.findings[0].kind == Finding::Kind::AnsLoss);
    CHECK(r.clean());
    CHECK(summary(r) == "clean, 1 note (1 ans-loss), noise 0.0000, iCE on");

    // and beside a merge: one finding, one note
    c.at(1, 0).ch = U'A';
    c.at(1, 0).fg = Color::indexed(Blue);
    c.at(1, 0).bg = Color::indexed(Blue);
    const LintReport m = lintCanvas(c, lo);
    CHECK_FALSE(m.clean());
    CHECK(summary(m) == "1 finding (1 merge), 1 note (1 ans-loss), noise 0.0000, iCE on");
}

TEST_CASE("under IRC a ladder finding at an RGB cell says it was judged by the nearest of the 16", "[target][lint]") {
    // test_lint's fallback speck: a field of ░ of grey on dark grey (steel's
    // rung 5), cut by a yellow shape's edge whose free half is the mIRC
    // colour read as grey
    int grey = 16;
    while (grey < 99 && swatchColor(grey).paletteIndex() != Grey)
        ++grey;
    REQUIRE(grey < 99);
    Canvas c(7, 5);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x) {
            c.at(x, y).ch = U'░';
            c.at(x, y).fg = Color::indexed(Grey);
            c.at(x, y).bg = Color::indexed(DarkGrey);
        }
    c.at(3, 1).ch = U'█';
    c.at(3, 1).fg = Color::indexed(Yellow);
    c.at(3, 2).ch = U'▀';
    c.at(3, 2).fg = Color::indexed(Yellow);
    c.at(3, 2).bg = swatchColor(grey);
    LintOptions lo;
    for (const Ramp& r : namedRamps())
        if (r.name == "steel")
            lo.ramps = {r};
    REQUIRE(lo.ramps.size() == 1);
    lo.target = Target::Irc;
    const auto speck = [](const LintReport& r) {
        return std::find_if(r.findings.begin(), r.findings.end(),
                            [](const Finding& f) { return f.kind == Finding::Kind::FallbackSpeck; });
    };
    const std::string suffix = " (an RGB cell, judged by the nearest of the 16)";
    const LintReport irc = lintCanvas(c, lo);
    REQUIRE(speck(irc) != irc.findings.end());
    CHECK(speck(irc)->x == 3);
    CHECK(speck(irc)->y == 2);
    CHECK(speck(irc)->message.starts_with("cell 3,2: a fallback speck"));
    CHECK(speck(irc)->message.ends_with(suffix));

    // the ANSI target, or an indexed cell, says nothing of it
    lo.target = Target::Ansi;
    const LintReport ansi = lintCanvas(c, lo);
    REQUIRE(speck(ansi) != ansi.findings.end());
    CHECK_FALSE(speck(ansi)->message.ends_with(suffix));
    c.at(3, 2).bg = Color::indexed(Grey);
    lo.target = Target::Irc;
    const LintReport indexed = lintCanvas(c, lo);
    REQUIRE(speck(indexed) != indexed.findings.end());
    CHECK_FALSE(speck(indexed)->message.ends_with(suffix));
}

TEST_CASE("under IRC the RGB note on a ladder finding counts only a colour the cell shows", "[target][lint]") {
    // a speck of grey in a field of ░ of grey on dark grey (steel's rung 5),
    // under a yellow block
    const auto field = [] {
        Canvas c(7, 5);
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 7; ++x) {
                c.at(x, y).ch = U'░';
                c.at(x, y).fg = Color::indexed(Grey);
                c.at(x, y).bg = Color::indexed(DarkGrey);
            }
        c.at(3, 1).ch = U'█';
        c.at(3, 1).fg = Color::indexed(Yellow);
        return c;
    };
    int grey = 16;
    while (grey < 99 && swatchColor(grey).paletteIndex() != Grey)
        ++grey;
    REQUIRE(grey < 99);
    LintOptions lo;
    for (const Ramp& r : namedRamps())
        if (r.name == "steel")
            lo.ramps = {r};
    REQUIRE(lo.ramps.size() == 1);
    lo.target = Target::Irc;
    const std::string suffix = " (an RGB cell, judged by the nearest of the 16)";
    const auto speck = [&](const Canvas& c) {
        const LintReport r = lintCanvas(c, lo);
        const auto f = std::find_if(r.findings.begin(), r.findings.end(), [](const Finding& f) {
            return f.kind == Finding::Kind::Speckle || f.kind == Finding::Kind::FallbackSpeck;
        });
        REQUIRE(f != r.findings.end());
        CHECK(f->x == 3);
        CHECK(f->y == 2);
        return f->message;
    };
    const Color unseen = Color::fromRgb(Rgb{0x12, 0x34, 0x56});
    // a space shows only its background, a full block only its foreground
    Canvas space = field();
    space.at(3, 2).ch = U' ';
    space.at(3, 2).bg = Color::indexed(Grey);
    space.at(3, 2).fg = unseen;
    Canvas block = field();
    block.at(3, 2).ch = U'█';
    block.at(3, 2).fg = Color::indexed(Grey);
    block.at(3, 2).bg = unseen;
    CHECK_FALSE(speck(space).ends_with(suffix));
    CHECK_FALSE(speck(block).ends_with(suffix));
    // a half block shows both: with the mIRC grey below the yellow, the
    // fallback speck was judged by the nearest of the 16
    Canvas half = field();
    half.at(3, 2).ch = U'▀';
    half.at(3, 2).fg = Color::indexed(Yellow);
    half.at(3, 2).bg = swatchColor(grey);
    CHECK(speck(half).ends_with(suffix));
}

// Every golden piece is indexed only, so it sends the same bytes, and the
// golden-irc bytes, under either target.
TEST_CASE("golden pieces send golden-irc's bytes under both targets", "[target][irc]") {
    std::vector<fs::path> pieces;
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden"))
        if (e.path().extension() == ".ans")
            pieces.push_back(e.path());
    std::sort(pieces.begin(), pieces.end());
    REQUIRE(pieces.size() >= 12);
    for (const auto& p : pieces) {
        DYNAMIC_SECTION(p.filename().string()) {
            const std::string want = bwtest::readFile(bwtest::dataDir() / "golden-irc" / (p.stem().string() + ".txt"));
            const AnsiDocument doc = parseAnsi(bwtest::readFile(p));
            for (Target t : {Target::Ansi, Target::Irc}) {
                INFO(targetName(t));
                const auto lines = irc::exportCanvas(irc::sentCanvas(doc.canvas, doc.ice, t));
                CHECK(lines.size() == splitLines(want).size());
                CHECK(irc::joinLines(lines) == want);
            }
        }
    }
}
