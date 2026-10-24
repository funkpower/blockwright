// IRC byte budgets and the export options that change cells: padding and
// fit-to-budget (irc.h).

#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/project.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <span>

using namespace bw;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

irc::MircCell solid(std::uint8_t colour) { return {U' ', colour, colour}; }

bool same(const irc::MircCell& a, const irc::MircCell& b) {
    return a.ch == b.ch && a.fg == b.fg && a.bg == b.bg;
}

irc::EncodeOptions fitTo(std::size_t budget) {
    irc::EncodeOptions o;
    o.fit = budget;
    return o;
}

// The golden cocktober (120 rows) with every other solid cell turned into a
// half block of its colour over its other shade (index ^ 8): visible noise
// that costs a colour code a cell, enough to take rows past 407 bytes.
Canvas noisyPiece() {
    Canvas c = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "cocktober.ans")).canvas;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 1; x < c.width(); x += 2) {
            Cell& k = c.at(x, y);
            if ((k.ch != U' ' && k.ch != U'█') || !k.fg.isIndexed() || !k.bg.isIndexed())
                continue;
            const int colour = k.ch == U' ' ? k.bg.index : k.fg.index;
            k = cell(U'▀', std::uint8_t(colour), std::uint8_t((colour ^ 8) & 15));
        }
    return c;
}

// The colours a cell shows, as fit reads them: one for a solid cell, both
// for any other.
std::vector<std::uint8_t> shown(const irc::MircCell& c) {
    if (c.ch == U' ' || c.ch == U'\u00a0')
        return {c.bg};
    if (c.ch == U'█' || c.fg == c.bg)
        return {c.fg};
    return {c.fg, c.bg};
}

// The first cell fit changed into a colour that neither the cell as it was
// nor its left neighbour as it ends up shows; -1 for none.
int foreignCell(std::span<const irc::MircCell> in, std::span<const irc::MircCell> out) {
    for (std::size_t x = 0; x < in.size(); ++x) {
        if (same(in[x], out[x]))
            continue;
        std::vector<std::uint8_t> ok = shown(in[x]);
        if (x > 0)
            for (const std::uint8_t c : shown(out[x - 1]))
                ok.push_back(c);
        for (const std::uint8_t c : shown(out[x]))
            if (std::find(ok.begin(), ok.end(), c) == ok.end())
                return int(x);
    }
    return -1;
}

} // namespace

TEST_CASE("budgetFor leaves 512 minus the relayed prefix") {
    // ":" mask " PRIVMSG " target " :" text "\r\n": 14 bytes besides them
    CHECK(irc::budgetFor("fp!~fp@example.org", "#art") == 512 - 14 - 18 - 4);
    CHECK(irc::budgetFor("a", "b") == 496);
    // the longest prefix that leaves a byte
    CHECK(irc::budgetFor(std::string(496, 'm'), "#") == 1);
    CHECK_FALSE(irc::budgetFor(std::string(497, 'm'), "#"));
    CHECK_FALSE(irc::budgetFor("", "#art"));
    CHECK_FALSE(irc::budgetFor("fp!~fp@example.org", ""));
    CHECK_FALSE(irc::budgetFor("fp!~fp@example.org", "#a b"));
    CHECK_FALSE(irc::budgetFor("fp!~fp@exa\rmple.org", "#art"));
    CHECK_FALSE(irc::budgetFor("fp!~fp@example.org", "#art\n"));
    CHECK_FALSE(irc::budgetFor(std::string("fp\0x", 4), "#art"));
}

TEST_CASE("the EFnet / WeeChat preset is the project's default budget") {
    const project::IrcBudget def;
    REQUIRE_FALSE(irc::kBudgetPresets.empty());
    CHECK(irc::kBudgetPresets[0].profile == def.profile);
    CHECK(irc::kBudgetPresets[0].bytes == def.bytes);
    CHECK(def.bytes == 407);
    for (const auto& p : irc::kBudgetPresets)
        CHECK(p.profile != irc::kCustomProfile);
}

TEST_CASE("solidEnd draws a last solid cell as a block") {
    const std::vector<irc::MircCell> black{solid(1), solid(1), solid(1)};
    CHECK(irc::encodeMircRow(black) == "\x03" "1,1   \x0f");
    CHECK(irc::encodeMircRow(black, false, true) == "\x03" "1,1  █");
    // a last cell that isn't solid is drawn as it is
    const std::vector<irc::MircCell> half{solid(1), {U'▀', 4, 2}};
    CHECK(irc::encodeMircRow(half, false, true) == irc::encodeMircRow(half));
}

TEST_CASE("pad keeps trailing black cells and ends the row on a black block") {
    Canvas c(4, 1);
    c.at(0, 0) = cell(U'X', LightRed, Black);
    irc::EncodeOptions pad;
    pad.pad = true;
    const std::string plain = irc::encodeCanvas(c)[0];
    const std::string padded = irc::encodeCanvas(c, pad)[0];
    CHECK(plain == "\x03" "4,1X");
    CHECK(padded == "\x03" "4,1X  \x03" "1█");
    // a row that ends on a coloured cell is the same either way
    Canvas red(2, 1);
    red.at(0, 0) = cell(U'X', LightRed, Black);
    red.at(1, 0) = cell(U' ', Grey, Red);
    CHECK(irc::encodeCanvas(red, pad) == irc::encodeCanvas(red));
}

TEST_CASE("pad keeps the blank rows above and below the art") {
    Canvas c(3, 4);
    c.at(1, 1) = cell(U'a', Grey, Black);
    irc::EncodeOptions pad;
    pad.pad = true;
    CHECK(irc::exportCanvas(c).size() == 1);
    const auto lines = irc::exportCanvas(c, pad);
    REQUIRE(lines.size() == 4);
    for (const int y : {0, 2, 3}) {
        INFO("row " << y);
        CHECK(lines[y] == "\x03" "1,1  █");
    }
    CHECK(lines[1] == "\x03" "1,1 \x03" "15a\x03" "1█");
}

TEST_CASE("fitCells leaves a row that fits alone") {
    const std::vector<irc::MircCell> row{{U'▀', 4, 2}, {U'x', 0, 1}, solid(3)};
    const std::size_t size = irc::encodeMircRow(row).size();
    const auto out = irc::fitCells(row, size);
    REQUIRE(out.size() == row.size());
    for (std::size_t x = 0; x < row.size(); ++x)
        CHECK(same(out[x], row[x]));
}

TEST_CASE("fitCells merges the least visible colours first") {
    // red (4) with, in the first half, mIRC 52 between: the same #ff0000;
    // in the second half dark blue (12) between
    std::vector<irc::MircCell> row;
    for (int i = 0; i < 30; ++i) {
        row.push_back(solid(4));
        row.push_back(solid(i < 15 ? 52 : 12));
    }
    std::vector<irc::MircCell> merged = row;
    for (auto& m : merged)
        if (m.bg == 52)
            m = solid(4);
    const std::size_t budget = irc::encodeMircRow(merged).size();
    REQUIRE(irc::encodeMircRow(row).size() > budget);
    const auto out = irc::fitCells(row, budget);
    CHECK(irc::encodeMircRow(out).size() <= budget);
    int changed = 0;
    for (std::size_t x = 0; x < row.size(); ++x) {
        INFO("cell " << x);
        if (row[x].bg != 52)
            CHECK(same(out[x], row[x]));
        else if (!same(out[x], row[x])) {
            CHECK(same(out[x], solid(4)));
            ++changed;
        }
    }
    CHECK(changed > 0);
}

TEST_CASE("fitCells settles a shade into the colour it covers most") {
    // ░ is a quarter foreground: losing the foreground is the smaller change
    std::vector<irc::MircCell> row;
    for (int i = 0; i < 40; ++i)
        row.push_back({U'░', std::uint8_t(i % 2 ? 2 : 3), 1});
    const std::size_t budget = irc::encodeMircRow(row).size() * 2 / 3;
    const auto out = irc::fitCells(row, budget);
    CHECK(irc::encodeMircRow(out).size() <= budget);
    int changed = 0;
    for (std::size_t x = 0; x < row.size(); ++x)
        if (!same(out[x], row[x])) {
            INFO("cell " << x);
            CHECK(same(out[x], solid(1)));
            ++changed;
        }
    CHECK(changed > 0);
}

TEST_CASE("fitCells never changes text, and gives back a row it can't fit") {
    std::vector<irc::MircCell> row;
    for (int i = 0; i < 20; ++i)
        row.push_back({U'a', std::uint8_t(i % 2 ? 4 : 9), 1});
    const std::size_t budget = irc::encodeMircRow(row).size() - 1;
    const auto out = irc::fitCells(row, budget);
    for (std::size_t x = 0; x < row.size(); ++x)
        CHECK(same(out[x], row[x]));

    // half blocks beside the text fit; the text stays
    std::vector<irc::MircCell> mixed;
    for (int i = 0; i < 20; ++i) {
        mixed.push_back({U'a', 0, 1});
        mixed.push_back({U'▀', 4, std::uint8_t(i % 2 ? 52 : 5)});
    }
    const std::size_t size = irc::encodeMircRow(mixed).size();
    const auto fitted = irc::fitCells(mixed, size - 20);
    CHECK(irc::encodeMircRow(fitted).size() <= size - 20);
    for (std::size_t x = 0; x < mixed.size(); x += 2)
        CHECK(same(fitted[x], mixed[x]));
}

TEST_CASE("fitCells fits a row where no two neighbours share a colour") {
    // a half-block rainbow: settling every cell to one of its own colours
    // still leaves a colour code a cell, so cells must change again into
    // their neighbours' colours
    std::vector<irc::MircCell> row;
    for (int i = 0; i < 80; ++i)
        row.push_back({U'▀', std::uint8_t(2 + i % 8), std::uint8_t(2 + (i + 3) % 8)});
    std::vector<irc::MircCell> once;
    for (const auto& c : row)
        once.push_back(solid(c.fg));
    const std::size_t budget = irc::encodeMircRow(once).size() - 40;
    const auto out = irc::fitCells(row, budget);
    CHECK(irc::encodeMircRow(out).size() <= budget);
}

TEST_CASE("fit measures the row as padded when it pads") {
    std::vector<irc::MircCell> row;
    for (int i = 0; i < 30; ++i)
        row.push_back(solid(std::uint8_t(i % 2 ? 4 : 52)));
    for (int i = 0; i < 20; ++i)
        row.push_back(solid(1));
    irc::EncodeOptions pad;
    pad.pad = true;
    const std::size_t padded = irc::encodeMircRow(row, false, true).size();
    const auto out = irc::fitCells(row, padded - 10, pad);
    CHECK(irc::encodeMircRow(out, false, true).size() <= padded - 10);
    // the black tail is kept: merging it would not save a byte
    for (std::size_t x = 30; x < row.size(); ++x)
        CHECK(same(out[x], row[x]));
}

TEST_CASE("encodeLine counts the cells fit changed") {
    Canvas c(40, 2);
    for (int x = 0; x < 40; ++x) {
        c.at(x, 0) = cell(U'▀', std::uint8_t(x % 2 ? Red : LightRed), Black);
        c.at(x, 1) = cell(U'x', Grey, Black);
    }
    const auto plain = irc::encodeLine(c, 0);
    CHECK(plain.merged == 0);
    CHECK(plain.text == irc::encodeCanvas(c)[0]);
    const auto fitted = irc::encodeLine(c, 0, fitTo(plain.text.size() - 10));
    CHECK(fitted.text.size() <= plain.text.size() - 10);
    CHECK(fitted.merged > 0);
    CHECK(irc::encodeCanvas(c, fitTo(plain.text.size() - 10))[0] == fitted.text);
    // a row that fits already
    const auto text = irc::encodeLine(c, 1, fitTo(407));
    CHECK(text.merged == 0);
    CHECK(text.text == irc::encodeCanvas(c)[1]);
}

TEST_CASE("a 120-row piece over the budget exports with every line in 407 bytes") {
    const Canvas c = noisyPiece();
    REQUIRE(c.height() == 120);
    const auto plain = irc::exportCanvas(c);
    REQUIRE(irc::overBudget(plain, 407).size() >= 10);
    const auto o = fitTo(407);
    const auto lines = irc::exportCanvas(c, o);
    CHECK(lines.size() == plain.size());
    CHECK(irc::overBudget(lines, 407).empty());
    // only the rows that were over changed
    const auto rows = irc::encodeCanvas(c, o);
    const auto rawRows = irc::encodeCanvas(c);
    for (int y = 0; y < c.height(); ++y) {
        INFO("row " << y);
        const auto line = irc::encodeLine(c, y, o);
        CHECK(line.text == rows[std::size_t(y)]);
        CHECK((line.merged > 0) == (rawRows[std::size_t(y)].size() > 407));
    }
}

TEST_CASE("fitCells puts nothing back that leaves a cell with a colour from two cells away") {
    // Cell 2 settles into cell 1's black (taken from cell 0) and cell 1 is
    // then put back: cell 2 must not keep a black nothing beside it shows.
    const std::vector<irc::MircCell> in = {
        {U'▄', 14, 1}, {U'░', 7, 13}, {U'▒', 14, 3}, {U'▀', 5, 2}, {U'▒', 14, 13}, {U'▀', 0, 8}};
    REQUIRE(irc::encodeMircRow(in).size() == 47);
    const auto out = irc::fitCells(in, 41);
    CHECK(irc::encodeMircRow(out).size() <= 41);
    INFO("cell 2 out: fg " << int(out[2].fg) << " bg " << int(out[2].bg));
    CHECK(foreignCell(in, out) == -1);
}

TEST_CASE("fitCells takes a colour only from the cell or its left neighbour as it ends up") {
    std::mt19937 rng(14);
    const char32_t glyphs[] = {U' ', U'█', U'▀', U'▄', U'▌', U'▐', U'░', U'▒', U'▓', U'a'};
    int fitted = 0, foreign = 0;
    for (int t = 0; t < 4000; ++t) {
        const int width = 2 + int(rng() % 60);
        std::vector<irc::MircCell> row;
        std::uint8_t fg = 0, bg = 1;
        for (int x = 0; x < width; ++x) {
            // runs of shared colours now and then, as in a piece
            if (rng() % 3 != 0) {
                fg = std::uint8_t(rng() % 16);
                bg = std::uint8_t(rng() % 16);
            }
            row.push_back({glyphs[rng() % 10], fg, bg});
        }
        irc::EncodeOptions o;
        o.pad = rng() % 2 == 0;
        o.swapShades = rng() % 2 == 0;
        const std::size_t full = irc::encodeMircRow(row, o.swapShades).size();
        const std::size_t budget = full * (70 + rng() % 30) / 100;
        const auto out = irc::fitCells(row, budget, o);
        REQUIRE(out.size() == row.size());
        bool changed = false;
        for (std::size_t x = 0; x < row.size(); ++x)
            changed |= !same(out[x], row[x]);
        if (!changed)
            continue;
        ++fitted;
        if (const int x = foreignCell(row, out); x >= 0) {
            if (++foreign <= 5)
                UNSCOPED_INFO("row " << t << " cell " << x);
        }
    }
    CHECK(fitted > 1000);
    CHECK(foreign == 0);
}
