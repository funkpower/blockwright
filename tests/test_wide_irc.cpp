// IRC text on pairs and clusters (PLAN §16; slice 4). The encoder writes a
// lead as its cluster's UTF-8 bytes and its continuation as nothing, with no
// code between them; mircRow writes a lead a UTF-8 .ans would refuse as '?'
// and a space, and lists it as that writer does; fit never changes either
// half of a pair; decodeLine lays a line out as a terminal does (a wide
// cluster a pair, a zero-width code point joined to the cluster before it);
// and the IRC view cuts a line where the ascii.py sender script 1.32.2 does,
// between clusters, checked against tables the script made. Each part has
// a case for a pair, a pair on the last two columns, a lone wide cell, a
// cluster, a refused lead and a control. Nothing in the editor makes a pair
// or a cluster yet; these tests make them by hand.

#include "blockwright/ansi.h"
#include "blockwright/glyph.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/mirc.h"
#include "loss.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using namespace bw;
using irc::MircCell;

namespace {

const Glyph kCont = Glyph::continuation();

Cell cell(Glyph ch, std::uint8_t fg = 7, std::uint8_t bg = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

Glyph cluster(std::vector<char32_t> cps) {
    const std::optional<Glyph> g = clusterPool().intern(cps, PoolCap::Capped);
    REQUIRE(g);
    REQUIRE(g->isCluster());
    return *g;
}

std::vector<char32_t> cpsOf(Glyph g) {
    const GlyphCodePoints cps = codePoints(g);
    return {cps.begin(), cps.end()};
}

bool same(const MircCell& a, const MircCell& b) { return a.ch == b.ch && a.fg == b.fg && a.bg == b.bg; }

std::string encode(std::vector<MircCell> cells, bool solidEnd = false) {
    return irc::encodeMircRow(cells, false, solidEnd);
}

Canvas canvasOf(const std::vector<Cell>& cells) {
    Canvas c(int(cells.size()), 1);
    for (std::size_t x = 0; x < cells.size(); ++x)
        c.at(int(x), 0) = cells[x];
    return c;
}

// é as e + U+0301, the keycap 1️⃣ (narrow) and 👨 + ZWJ (wide)
Glyph eAcute() { return cluster({U'e', 0x301}); }
Glyph keycap() { return cluster({U'1', 0xFE0F, 0x20E3}); }
Glyph manZwj() { return cluster({0x1F468, 0x200D}); }

} // namespace

// --- encodeMircRow ---------------------------------------------------------

TEST_CASE("the encoder writes a pair's lead as its glyph and its continuation as nothing") {
    CHECK(encode({{U'中', 4, 1}, {kCont, 4, 1}, {U'A', 4, 1}}) == "\x03" "4,1中A");
    // the continuation's own colours draw nothing, so no code goes before it
    CHECK(encode({{U'中', 4, 1}, {kCont, 9, 2}, {U'A', 9, 2}}) == "\x03" "4,1中\x03" "9,2A");
    // a lead in one colour is still its glyph: it has one way to draw
    CHECK(encode({{U'中', 4, 4}, {kCont, 4, 4}}) == "\x03" "4,4中");
    CHECK(encode({{U'中', 4, 4}, {kCont, 4, 4}}, true) == "\x03" "4,4中");
    CHECK(encode({{U' ', 4, 1}, {kCont, 4, 1}}) == "\x03" "4,1 \x0f");
}

TEST_CASE("the encoder writes a pair on the last two columns whole") {
    CHECK(encode({{U'A', 4, 1}, {U'中', 4, 1}, {kCont, 4, 1}}) == "\x03" "4,1A中");
    // solidEnd has no last cell of one colour to draw as a block
    CHECK(encode({{U'A', 4, 1}, {U'中', 4, 1}, {kCont, 4, 1}}, true) == "\x03" "4,1A中");
    CHECK(encode({{U'A', 1, 1}, {U'中', 1, 1}, {kCont, 1, 1}}, true) == "\x03" "1,1 中");
}

TEST_CASE("the encoder writes a lone wide cell as one cell, as before") {
    CHECK(encode({{U'中', 4, 1}, {U'A', 4, 1}}) == "\x03" "4,1中A");
    CHECK(encode({{U'A', 4, 1}, {U'中', 4, 1}}) == "\x03" "4,1A中");
}

TEST_CASE("the encoder writes a cluster as all of its code points") {
    CHECK(encode({{eAcute(), 4, 1}, {U'A', 4, 1}}) == "\x03" "4,1e\xcc\x81" "A");
    // a number before a cluster that starts with a digit takes two digits
    CHECK(encode({{keycap(), 4, 1}}) == "\x03" "4,01" "1\xef\xb8\x8f\xe2\x83\xa3");
    CHECK(encode({{U'A', 4, 1}, {keycap(), 9, 1}}) == "\x03" "4,1A\x03" "09" "1\xef\xb8\x8f\xe2\x83\xa3");
    // a wide cluster as a pair: 4 bytes of code, 4 of 👨, 3 of the joiner
    const std::string man = encode({{manZwj(), 4, 1}, {kCont, 4, 1}});
    CHECK(man == "\x03" "4,1\xf0\x9f\x91\xa8\xe2\x80\x8d");
    CHECK(man.size() == 11);
    // a cluster on a space is not a space: no reset after it
    CHECK(encode({{cluster({U' ', 0x301}), 4, 1}}) == "\x03" "4,1 \xcc\x81");
}

TEST_CASE("the encoder refuses a continuation with no lead and a control is written as given") {
    CHECK_THROWS_AS(encode({{kCont, 4, 1}}), irc::IrcError);
    CHECK_THROWS_AS(encode({{U'A', 4, 1}, {kCont, 4, 1}, {kCont, 4, 1}}), irc::IrcError);
    CHECK_THROWS_AS(encode({{U'中', 4, 1}, {kCont, 4, 99}}), irc::IrcError);
    // the encoder takes what mircRow gives it: a C1 control mircRow has made '?'
    CHECK(encode({{char32_t(0x85), 4, 1}}) == "\x03" "4,1\xc2\x85");
}

// --- mircRow and encodeRow -------------------------------------------------

TEST_CASE("mircRow keeps a pair whole in its lead's colours") {
    // bright red on blue is mIRC 4 on 2; grey on black 15 on 1
    const std::vector<Cell> row{cell(U'中', 12, 1), cell(kCont, 14, 4), cell(U'A', 7, 0)};
    std::vector<Unwritable> lost;
    const std::vector<MircCell> m = irc::mircRow(row, &lost);
    REQUIRE(m.size() == 3);
    CHECK(same(m[0], {U'中', 4, 2}));
    CHECK(same(m[1], {kCont, 4, 2}));
    CHECK(same(m[2], {U'A', 15, 1}));
    CHECK(lost.empty());
    CHECK(irc::encodeRow(row) == "\x03" "4,2中\x03" "15,1A");
}

TEST_CASE("mircRow keeps a pair on the last two columns whole") {
    const std::vector<Cell> row{cell(U'A', 12, 1), cell(U'中', 12, 1), cell(kCont, 12, 1)};
    std::vector<Unwritable> lost;
    const std::vector<MircCell> m = irc::mircRow(row, &lost);
    CHECK(same(m[2], {kCont, 4, 2}));
    CHECK(lost.empty());
    CHECK(irc::encodeRow(row) == "\x03" "4,2A中");
}

TEST_CASE("mircRow writes a lone wide cell as its glyph") {
    std::vector<Unwritable> lost;
    const std::vector<Cell> row{cell(U'中', 12, 1), cell(U'A', 12, 1)};
    CHECK(same(irc::mircRow(row, &lost)[0], {U'中', 4, 2}));
    CHECK(lost.empty());
    CHECK(irc::encodeRow(row) == "\x03" "4,2中A");
}

TEST_CASE("mircRow writes a cluster as itself, and a NUL in one as a space") {
    std::vector<Unwritable> lost;
    const std::vector<Cell> row{cell(eAcute(), 12, 1), cell(cluster({0, 0x301}), 12, 1)};
    const std::vector<MircCell> m = irc::mircRow(row, &lost);
    CHECK(m[0].ch == eAcute());
    CHECK(cpsOf(m[1].ch) == std::vector<char32_t>{U' ', 0x301});
    CHECK(lost.empty());
    CHECK(irc::encodeRow(row) == "\x03" "4,2e\xcc\x81 \xcc\x81");
}

TEST_CASE("mircRow writes a lead a UTF-8 .ans refuses as '?' and a space, and lists it as that writer does") {
    struct Case {
        Glyph lead;
        Loss loss;
    };
    for (const Case& k : {Case{U'A', Loss::Wide}, Case{eAcute(), Loss::Wide}, Case{char32_t(0x1B), Loss::Control},
                          Case{char32_t(0x9B), Loss::Control}, Case{cluster({U'中', 0x301}), Loss::Wide}}) {
        INFO(k.lead);
        // the 中 + U+0301 lead is wide and writable: written whole
        const bool whole = cpsOf(k.lead).front() == U'中';
        const std::vector<Cell> row{cell(U'B', 12, 1), cell(k.lead, 12, 1), cell(kCont, 14, 4)};
        std::vector<Unwritable> lost;
        const std::vector<MircCell> m = irc::mircRow(row, &lost, 3);
        if (whole) {
            CHECK(m[1].ch == k.lead);
            CHECK(same(m[2], {kCont, 4, 2}));
            CHECK(lost.empty());
            continue;
        }
        CHECK(same(m[1], {U'?', 4, 2}));
        CHECK(same(m[2], {U' ', 8, 5}));  // in its own colours
        REQUIRE(lost.size() == 1);
        CHECK(lost[0].x == 1);
        CHECK(lost[0].y == 3);
        CHECK(lost[0].ch == k.lead);
        CHECK(lost[0].loss == k.loss);
        CHECK(lost[0].written == U'?');
        CHECK(lost[0].lead);
        // the UTF-8 .ans writer lists the same cell the same way
        AnsiDocument doc;
        doc.canvas = canvasOf(row);
        const AnsiOutput out = writeAnsi(doc, TextEncoding::Utf8);
        REQUIRE(out.unwritable.size() == 1);
        CHECK(out.unwritable[0].x == 1);
        CHECK(out.unwritable[0].ch == k.lead);
        CHECK(out.unwritable[0].loss == k.loss);
        CHECK(out.unwritable[0].lead);
    }
}

TEST_CASE("mircRow writes a control as '?' and lists it, and a C0 control as its CP437 glyph") {
    for (const char32_t ch : {char32_t(0x85), char32_t(0x9B), char32_t(0x2028), char32_t(0x2029), char32_t(0xD800)}) {
        INFO(int(ch));
        std::vector<Unwritable> lost;
        const std::vector<MircCell> m = irc::mircRow(std::vector{cell(U'A'), cell(ch, 12, 1)}, &lost);
        CHECK(same(m[1], {U'?', 4, 2}));
        REQUIRE(lost.size() == 1);
        CHECK(lost[0].x == 1);
        CHECK(lost[0].ch == Glyph(ch));
        CHECK(lost[0].loss == Loss::Control);
        CHECK_FALSE(lost[0].lead);
    }
    std::vector<Unwritable> lost;
    const std::vector<MircCell> m = irc::mircRow(std::vector{cell(char32_t(0x1B)), cell(char32_t(0))}, &lost);
    CHECK(m[0].ch == U'←');
    CHECK(m[1].ch == U' ');
    CHECK(lost.empty());
}

TEST_CASE("a transparent lead is drawn blank and its continuation as a space") {
    std::vector<Cell> row{cell(U'中', 12, 1), cell(kCont, 12, 1)};
    row[0].transparent = true;
    std::vector<Cell> plain{cell(U' ', 12, 1), cell(U' ', 12, 1)};
    plain[0].transparent = true;
    std::vector<Unwritable> lost;
    CHECK(irc::encodeRow(row) == irc::encodeRow(plain));
    irc::mircRow(row, &lost);
    CHECK(lost.empty());
}

TEST_CASE("unwritable lists IRC text's losses in cell order") {
    Canvas c(3, 2);
    c.at(1, 0).ch = char32_t(0x9B);
    c.at(0, 1).ch = U'A';
    c.at(1, 1).ch = kCont;
    c.at(2, 1).ch = U'中';  // a lone wide cell is no loss
    const std::vector<Unwritable> lost = irc::unwritable(c);
    REQUIRE(lost.size() == 2);
    CHECK((lost[0].x == 1 && lost[0].y == 0 && lost[0].loss == Loss::Control && !lost[0].lead));
    CHECK((lost[1].x == 0 && lost[1].y == 1 && lost[1].loss == Loss::Wide && lost[1].lead));
    CHECK(irc::unwritable(Canvas(3, 2)).empty());
}

TEST_CASE("a UTF-8 .ans writes U+2028 and U+2029 as '?', as it does C1") {
    CHECK_FALSE(utf8Writable(0x2028));
    CHECK_FALSE(utf8Writable(0x2029));
    CHECK(utf8Writable(0x2027));
    CHECK(utf8Writable(0x202A));
    CHECK(utf8WideGlyph(U'中'));
    CHECK(utf8WideGlyph(manZwj()));
    CHECK_FALSE(utf8WideGlyph(U'A'));
    CHECK_FALSE(utf8WideGlyph(eAcute()));
    CHECK_FALSE(utf8WideGlyph(kCont));
}

// --- fit -------------------------------------------------------------------

TEST_CASE("fit never changes either half of a pair, a cluster or a control") {
    std::vector<MircCell> row;
    for (int x = 0; x < 40; ++x)
        row.push_back({U'▀', std::uint8_t(x % 15 + 1), std::uint8_t((x * 7) % 15 + 1)});
    row[10] = {U'中', 4, 9};
    row[11] = {kCont, 4, 9};
    row[20] = {eAcute(), 4, 9};
    row[25] = {U'?', 4, 9};
    row[30] = {U'中', 4, 9};  // lone wide
    row[38] = {U'中', 3, 5};
    row[39] = {kCont, 3, 5};
    const std::size_t plain = irc::encodeMircRow(row).size();
    const std::size_t budget = plain * 3 / 4;
    const std::vector<MircCell> fitted = irc::fitCells(row, budget);
    REQUIRE(fitted.size() == row.size());
    CHECK(irc::encodeMircRow(fitted).size() <= budget);
    int changed = 0;
    for (std::size_t x = 0; x < row.size(); ++x)
        changed += same(row[x], fitted[x]) ? 0 : 1;
    CHECK(changed > 0);
    for (const std::size_t x : {10u, 11u, 20u, 25u, 30u, 38u, 39u}) {
        INFO("cell " << x);
        CHECK(same(fitted[x], row[x]));
    }
    const irc::Line line = irc::encodeMircLine(row, {.fit = budget});
    CHECK(line.text == irc::encodeMircRow(fitted));
    CHECK(line.merged == changed);
}

// --- decodeLine ------------------------------------------------------------

TEST_CASE("decodeLine reads a wide code point as a pair, both halves ending after it") {
    const auto cells = irc::decodeLine("\x03" "4,1中A");
    REQUIRE(cells.size() == 3);
    CHECK(same(cells[0].cell, {U'中', 4, 1}));
    CHECK(same(cells[1].cell, {kCont, 4, 1}));
    CHECK(same(cells[2].cell, {U'A', 4, 1}));
    CHECK(cells[0].end == 7);
    CHECK(cells[1].end == 7);
    CHECK(cells[2].end == 8);
}

TEST_CASE("decodeLine wraps a wide code point on the last column first") {
    const auto cells = irc::decodeLine("A中", {.columns = 2});
    REQUIRE(cells.size() == 4);
    CHECK(same(cells[0].cell, {U'A', 99, 99}));
    CHECK(same(cells[1].cell, {U' ', 99, 99}));  // the column the wrap leaves
    CHECK(cells[1].end == 1);
    CHECK(same(cells[2].cell, {U'中', 99, 99}));
    CHECK(cells[3].cell.ch == kCont);
    CHECK(cells[3].end == 4);
    // a pair that ends on the last column doesn't wrap
    CHECK(irc::decodeLine("中A", {.columns = 3}).size() == 3);
    // nor does one on the second row's first column
    const auto two = irc::decodeLine("AB中", {.columns = 2});
    REQUIRE(two.size() == 4);
    CHECK(two[2].cell.ch == U'中');
}

TEST_CASE("decodeLine reads a wide code point on a one-column row as U+FFFD") {
    const auto cells = irc::decodeLine("中A", {.columns = 1});
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].cell.ch == U'�');
    CHECK(cells[0].end == 3);
    CHECK(cells[1].cell.ch == U'A');
    const auto marked = irc::decodeLine("中́", {.columns = 1});
    REQUIRE(marked.size() == 1);
    CHECK(cpsOf(marked[0].cell.ch) == std::vector<char32_t>{0xFFFD, 0x301});
}

TEST_CASE("decodeLine reads a cluster as one cell and a wide one as a pair") {
    const auto e = irc::decodeLine("éA");
    REQUIRE(e.size() == 2);
    CHECK(e[0].cell.ch == eAcute());
    CHECK(e[0].end == 3);
    CHECK(e[1].end == 4);
    // a mark after a pair joins its lead
    const auto lead = irc::decodeLine("中́B");
    REQUIRE(lead.size() == 3);
    CHECK(cpsOf(lead[0].cell.ch) == std::vector<char32_t>{U'中', 0x301});
    CHECK(lead[1].cell.ch == kCont);
    CHECK(lead[0].end == 5);
    CHECK(lead[1].end == 5);
}

TEST_CASE("decodeLine reads 👨‍👩‍👧 at column 0 as leads at 0, 2 and 4") {
    const auto cells = irc::decodeLine("\U0001F468‍\U0001F469‍\U0001F467");
    REQUIRE(cells.size() == 6);
    CHECK(cpsOf(cells[0].cell.ch) == std::vector<char32_t>{0x1F468, 0x200D});
    CHECK(cells[1].cell.ch == kCont);
    CHECK(cpsOf(cells[2].cell.ch) == std::vector<char32_t>{0x1F469, 0x200D});
    CHECK(cells[3].cell.ch == kCont);
    CHECK(cells[4].cell.ch == Glyph(char32_t(0x1F467)));
    CHECK(cells[5].cell.ch == kCont);
    CHECK(cells[0].end == 7);
    CHECK(cells[2].end == 14);
    CHECK(cells[5].end == 18);
}

TEST_CASE("decodeLine joins a mark after a colour code, and gives one at the start a cell") {
    const auto after = irc::decodeLine("e\x03" "4́");
    REQUIRE(after.size() == 1);
    CHECK(after[0].cell.ch == eAcute());
    CHECK(after[0].cell.fg == 99);  // the cluster keeps its base's colours
    CHECK(after[0].end == 5);
    const auto first = irc::decodeLine("́" "A");
    REQUIRE(first.size() == 2);
    CHECK(first[0].cell.ch == Glyph(char32_t(0x301)));
    CHECK(first[1].cell.ch == U'A');
    // a colour code before it doesn't make it join anything
    REQUIRE(irc::decodeLine("\x03" "4́").size() == 1);
}

TEST_CASE("decodeLine never joins a mark to a control or NUL") {
    // NUL included, as ascii.py (1.32.2) has it: a client never sees one
    for (const std::string_view line : {std::string_view("\xc2\x85\xcc\x81"), std::string_view("\xe2\x80\xa8\xcc\x81"),
                                        std::string_view("\xe2\x80\xa9\xcc\x81"), std::string_view("\x01\xcc\x81"),
                                        std::string_view("\0\xcc\x81", 3)}) {
        const auto cells = irc::decodeLine(line);
        REQUIRE(cells.size() == 2);
        CHECK(cells[1].cell.ch == Glyph(char32_t(0x301)));
    }
}

TEST_CASE("decodeLine keeps 16 code points of a cluster and counts the rest") {
    std::string line = "e";
    for (int i = 0; i < 16; ++i)
        line += "́";
    line += "A";
    irc::DecodeLoss loss;
    const auto cells = irc::decodeLine(line, {}, &loss);
    REQUIRE(cells.size() == 2);
    const std::vector<char32_t> cps = cpsOf(cells[0].cell.ch);
    REQUIRE(cps.size() == 16);
    CHECK(cps.front() == U'e');
    CHECK(cells[0].end == line.size() - 1);
    CHECK(loss.marksDropped == 1);
    CHECK(loss.firstDropped == 0);
    CHECK(loss.poolRefused == 0);
    irc::DecodeLoss none;
    irc::decodeLine("é", {}, &none);
    CHECK(none.marksDropped == 0);
}

TEST_CASE("decodeLine keeps a cluster's base when the pool is full") {
    irc::DecodeLoss loss;
    const auto cells = irc::decodeLine("A́̂̃", {.poolCap = 0}, &loss);
    REQUIRE(cells.size() == 2);
    CHECK(cells[1].cell.ch == Glyph(char32_t(0xE123)));
    CHECK(cells[1].end == 10);
    CHECK(loss.poolRefused == 1);
    CHECK(loss.firstRefused == 1);
    // a cluster the pool holds is found whatever the cap
    const Glyph e = eAcute();
    irc::DecodeLoss found;
    CHECK(irc::decodeLine("é", {.poolCap = 0}, &found)[0].cell.ch == e);
    CHECK(found.poolRefused == 0);
}

TEST_CASE("decodeLine of the encoder's line gives back the cells") {
    const std::vector<MircCell> row{{U'A', 4, 1},  {U'中', 4, 1}, {kCont, 4, 1},   {eAcute(), 9, 2},
                                    {manZwj(), 3, 5}, {kCont, 3, 5}, {keycap(), 3, 5}, {U'中', 12, 2},
                                    {kCont, 12, 2}};
    const auto cells = irc::decodeLine(irc::encodeMircRow(row));
    REQUIRE(cells.size() == row.size());
    for (std::size_t x = 0; x < row.size(); ++x) {
        INFO("cell " << x);
        CHECK(same(cells[x].cell, row[x]));
    }
}

// --- the IRC view and the sender's cut --------------------------------------

namespace {

irc::View view(const std::vector<Cell>& cells, std::size_t budget = irc::kEfnetWeechatBudget) {
    return irc::ircView(canvasOf(cells), budget);
}

} // namespace

TEST_CASE("the IRC view shows a pair where the canvas has it") {
    const irc::View v = view({cell(U'A', 12, 1), cell(U'中', 12, 1), cell(kCont, 12, 1), cell(U'B', 12, 1)});
    CHECK(v.canvas.at(1, 0).ch == U'中');
    CHECK(v.canvas.at(2, 0).ch == kCont);
    CHECK(v.canvas.at(3, 0).ch == U'B');
    CHECK(v.rows[0].kept == 4);
    CHECK(v.rows[0].shown == 4);
    const irc::View end = view({cell(U'A', 12, 1), cell(U'B', 12, 1), cell(U'中', 12, 1), cell(kCont, 12, 1)});
    CHECK(end.canvas.at(2, 0).ch == U'中');
    CHECK(end.canvas.at(3, 0).ch == kCont);
    CHECK(end.rows[0].shown == 4);
}

TEST_CASE("the IRC view shows a lone wide cell as a client lays it out") {
    // mid-row it takes two columns and pushes the rest right
    const irc::View mid = view({cell(U'中', 12, 1), cell(U'A', 12, 1), cell(U'B', 12, 1), cell(U'C', 12, 1)});
    CHECK(mid.canvas.at(0, 0).ch == U'中');
    CHECK(mid.canvas.at(1, 0).ch == kCont);
    CHECK(mid.canvas.at(2, 0).ch == U'A');
    CHECK(mid.canvas.at(3, 0).ch == U'B');
    CHECK(mid.rows[0].kept == 4);
    // on the last column it wraps, leaving that column blank
    const irc::View last = view({cell(U'A', 12, 1), cell(U'B', 12, 1), cell(U'C', 12, 1), cell(U'中', 12, 1)});
    CHECK(last.canvas.at(2, 0).ch == U'C');
    CHECK(last.canvas.at(3, 0).ch == U' ');
    CHECK(last.canvas.at(3, 0).bg == Color::fromRgb(Rgb{0x26, 0x26, 0x26}));
}

TEST_CASE("the IRC view shows a cluster, a refused lead and a control as they are sent") {
    const irc::View v = view({cell(eAcute(), 12, 1), cell(U'A', 12, 1), cell(kCont, 12, 1), cell(char32_t(0x9B), 12, 1)});
    CHECK(v.canvas.at(0, 0).ch == eAcute());
    CHECK(v.canvas.at(1, 0).ch == U'?');
    CHECK(v.canvas.at(2, 0).ch == U' ');
    CHECK(v.canvas.at(3, 0).ch == U'?');
    CHECK(v.rows[0].shown == 4);
}

TEST_CASE("the IRC view's cut never shows a lead without its continuation or part of a cluster") {
    std::vector<Cell> row;
    for (int x = 0; x < 80; ++x)
        row.push_back(cell(U'▀', std::uint8_t(x % 15 + 1), std::uint8_t((x * 7) % 16)));
    row[40] = cell(U'中', 12, 1);
    row[41] = cell(kCont, 12, 1);
    row[50] = cell(cluster({U'e', 0x301, 0x302, 0x303}), 12, 1);
    row[60] = cell(U'中', 9, 2);  // lone wide: from here a client is a column on
    const Canvas c = canvasOf(row);
    std::set<int> seen;
    for (std::size_t budget = 100; budget <= 700; ++budget) {
        INFO("budget " << budget);
        const irc::View v = irc::ircView(c, budget);
        const int shown = v.rows[0].shown;
        seen.insert(shown);
        CHECK(shown != 41);
        CHECK(shown != 61);
        if (shown > 40)
            CHECK(v.canvas.at(41, 0).ch == kCont);
        else
            CHECK(v.canvas.at(40, 0).ch == U' ');
        if (shown > 50)
            CHECK(v.canvas.at(50, 0).ch == row[50].ch);
    }
    CHECK(seen.count(40));
    CHECK(seen.count(42));
    CHECK(seen.count(50));
    CHECK(seen.count(51));
    CHECK(seen.count(60));
    CHECK(seen.count(62));
}

TEST_CASE("the IRC view's cut is the one the ascii.py sender 1.32.2 makes") {
    // Each table is what ascii.py 1.32.2 (3f16708) sends of the
    // row's line at each budget from `from` on, as the columns a client lays
    // it out in. Derived by loading ascii.py with python3 -I and running
    // fit(compact(line), budget) on the line below (compact is what the
    // script does to each .txt line it loads), then parse_irc and cluster_cells
    // on what it returns, counting a cluster two columns when its base is
    // East Asian Wide or Fullwidth, up to the row's width. Not from the view.
    struct ScriptCut {
        const char* name;
        std::vector<Cell> row;
        std::string line;
        std::size_t from;
        std::vector<int> columns;
    };
    std::vector<ScriptCut> cuts;
    {
        // a cluster of three marks and 👨+ZWJ: the review's repro
        std::vector<Cell> row(20, cell(U'z'));
        row[0] = cell(U'a');
        row[1] = cell(U'b');
        row[2] = cell(U'c');
        row[3] = cell(cluster({U'e', 0x301, 0x302, 0x303}));
        row[5] = cell(manZwj());
        row[6] = cell(kCont);
        cuts.push_back({"marks and a joiner", row,
                        "\x03" "15,1abce\xCC\x81\xCC\x82\xCC\x83z\xF0\x9F\x91\xA8\xE2\x80\x8Dzzzzzzzzzzzzz", 8,
                        {1, 2, 3, 3, 3, 3, 3, 3, 3, 4, 5, 5, 5, 5, 5, 5, 5, 7}});
    }
    {
        // 👨‍👩‍👧 held as one pair: a client lays it out as three
        std::vector<Cell> row(18, cell(U'z'));
        row[0] = cell(U'a');
        row[1] = cell(U'b');
        row[2] = cell(U'c');
        row[3] = cell(cluster({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467}));
        row[4] = cell(kCont);
        cuts.push_back({"family", row,
                        "\x03" "15,1abc\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7zzzzzzzzzzzzz",
                        8, {1, 2, 3, 3, 3, 3, 3, 3, 3, 5, 5, 5, 5, 5, 5, 5, 7, 7, 7, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 18, 18, 18}});
    }
    {
        // ❤️: a narrow base and VS16
        std::vector<Cell> row(16, cell(U'z'));
        row[0] = cell(U'a');
        row[1] = cell(U'b');
        row[2] = cell(U'c');
        row[3] = cell(cluster({0x2764, 0xFE0F}));
        cuts.push_back({"heart", row, "\x03" "15,1abc\xE2\x9D\xA4\xEF\xB8\x8Fzzzzzzzzzzzz", 8,
                        {1, 2, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16}});
    }
    {
        // █ and a space carrying U+0301, coloured so that sending either as
        // the other would be shorter: the script keeps their glyphs
        std::vector<Cell> row{cell(U'x', 7, 4), cell(cluster({0x2588, 0x301}), 4, 2), cell(U'y', 7, 2),
                              cell(U' ', 7, 4), cell(cluster({0x2588, 0x301}), 4, 1), cell(U'█', 4, 1),
                              cell(cluster({U' ', 0x301}), 7, 4), cell(U'█', 2, 4)};
        for (int i = 0; i < 10; ++i)
            row.push_back(cell(U'z', 7, 1));
        cuts.push_back({"full block and space with a mark", row,
                        "\x03" "15,5x\x03" "5,3\xE2\x96\x88\xCC\x81\x03" "15y\x03" "5,5 \x03" "5,2\xE2\x96\x88\xCC\x81\x03"
                        "5,5 \x03" "15 \xCC\x81\x03" "3,3 \x03" "15,2zzzzzzzzzz",
                        8, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4,
                            4, 4, 4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 8, 8, 8}});
    }
    {
        // a mark and a ZWJ in cells of their own, in other colours than the
        // cell before: the line has a colour code inside a cluster, which
        // the script encodes again before it measures the line
        std::vector<Cell> row{cell(U'x', 7, 1), cell(char32_t(0x301), 12, 4)};
        for (int i = 0; i < 6; ++i)
            row.push_back(cell(U'y', 7, 1));
        row.push_back(cell(char32_t(0x200D), 9, 2));
        for (int i = 0; i < 8; ++i)
            row.push_back(cell(U'w', 3, 5));
        cuts.push_back({"mark after a colour code", row,
                        "\x03" "15,2x\x03" "4,5\xCC\x81\x03" "15,2yyyyyy\x03" "12,3\xE2\x80\x8D\x03" "10,6wwwwwwww", 4,
                        {0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  6,  6,  6,  7,  7,  7,  7,
                         7,  7,  7,  8,  9,  10, 11, 12, 13, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                         15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15}});
    }
    for (const ScriptCut& cut : cuts) {
        INFO(cut.name);
        const Canvas c = canvasOf(cut.row);
        const std::vector<std::string> lines = irc::encodeCanvas(c, {});
        REQUIRE(lines[0] == cut.line);
        for (std::size_t i = 0; i < cut.columns.size(); ++i) {
            const std::size_t budget = cut.from + i;
            INFO("budget " << budget);
            CHECK(irc::ircView(c, budget).rows[0].shown == cut.columns[i]);
        }
    }
    // the cluster the script keeps is drawn whole, its glyph as held
    const irc::View marks = irc::ircView(canvasOf(cuts[0].row), 17);
    CHECK(marks.canvas.at(3, 0).ch == cuts[0].row[3].ch);
    const irc::View block = irc::ircView(canvasOf(cuts[3].row), 19);
    REQUIRE(block.rows[0].shown == 2);
    CHECK(block.canvas.at(1, 0).ch == cuts[3].row[1].ch);
}

TEST_CASE("the sender's cut keeps a pair at the row's end whole, and cuts a refused lead and a control as sent") {
    // plain text in one colour, so the sender's re-encoding of a prefix costs
    // what the encoder's does and every cut is reachable
    std::vector<Cell> row(80, cell(U'x', 7, 1));
    row[30] = cell(U'A', 12, 1);  // a lead a UTF-8 .ans refuses: '?' and a space
    row[31] = cell(kCont, 12, 1);
    row[45] = cell(char32_t(0x9B), 12, 1);  // a control: '?'
    row[78] = cell(U'中', 9, 2);
    row[79] = cell(kCont, 9, 2);
    const Canvas c = canvasOf(row);
    std::set<int> seen;
    for (std::size_t budget = 1; budget <= 200; ++budget) {
        INFO("budget " << budget);
        const irc::View v = irc::ircView(c, budget);
        const int shown = v.rows[0].shown;
        seen.insert(shown);
        CHECK(shown != 79);
        if (shown > 30)
            CHECK(v.canvas.at(30, 0).ch == U'?');
        if (shown > 31)
            CHECK(v.canvas.at(31, 0).ch == U' ');
        if (shown > 45)
            CHECK(v.canvas.at(45, 0).ch == U'?');
        if (shown == 80) {
            CHECK(v.canvas.at(78, 0).ch == U'中');
            CHECK(v.canvas.at(79, 0).ch == kCont);
        }
    }
    // the refused lead's two cells are narrow, so a cut may fall between them
    CHECK(seen.count(31));
    CHECK(seen.count(45));
    CHECK(seen.count(46));
    CHECK(seen.count(78));
    CHECK(seen.count(80));
}

// --- the CLI's warnings ----------------------------------------------------

TEST_CASE("the CLI says a UTF-8 .ans writes a control-holding lead as '?' and a space") {
    AnsiDocument doc;
    doc.canvas = canvasOf({cell(char32_t(0x1B)), cell(kCont), cell(char32_t(0x85))});
    const AnsiOutput out = writeAnsi(doc, TextEncoding::Utf8);
    const std::vector<std::string> w = bwcli::unwritableWarnings(out);
    REQUIRE(w.size() == 2);
    // the control on its own is written '?'
    CHECK(w[0] == "1 cell holds a control character a UTF-8 .ans can't hold and is written as '?' "
                  "(first: U+0085 at column 3, row 1)");
    // the lead is written '?' and its continuation a space
    CHECK(w[1] == "1 cell holds a control character a UTF-8 .ans can't hold before a continuation and is "
                  "written as '?' and a space (first: U+001B at column 1, row 1)");

    doc.canvas = canvasOf({cell(char32_t(0x1B)), cell(kCont), cell(char32_t(0x9B)), cell(kCont)});
    const std::vector<std::string> two = bwcli::unwritableWarnings(writeAnsi(doc, TextEncoding::Utf8));
    REQUIRE(two.size() == 1);
    CHECK(two[0] == "2 cells hold control characters a UTF-8 .ans can't hold before a continuation and are "
                    "written as '?' and a space (first: U+001B at column 1, row 1)");
}

TEST_CASE("the CLI words IRC text's losses as it does a UTF-8 .ans's") {
    Canvas c(5, 2);
    c.at(0, 0).ch = U'A';
    c.at(1, 0).ch = kCont;
    c.at(3, 1).ch = char32_t(0x85);
    const std::vector<std::string> w = bwcli::ircUnwritableWarnings(irc::unwritable(c));
    REQUIRE(w.size() == 2);
    CHECK(w[0] == "1 cell holds a control character IRC text can't hold and is written as '?' "
                  "(first: U+0085 at column 4, row 2)");
    CHECK(w[1] == "1 cell holds a glyph a terminal doesn't draw two columns wide before a continuation and is "
                  "written as '?' and a space (first: U+0041 'A' at column 1, row 1)");
    CHECK(bwcli::ircUnwritableWarnings({}).empty());
}
