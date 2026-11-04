// The whole lint (lint_rules.h): each craft rule on a hand-drawn canvas,
// what it reports and what it leaves alone, and lintCanvas over ramps, the
// box and the noise.

#include "blockwright/ansi.h"
#include "blockwright/halfblock.h"
#include "blockwright/irc.h"
#include "blockwright/lint_rules.h"
#include "util.h"

#include "../cli/lint.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace bw;
using Kind = lint::Finding::Kind;

namespace {

Cell cell(char32_t g, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = g;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

Canvas filled(int w, int h, const Cell& c) {
    Canvas canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.at(x, y) = c;
    return canvas;
}

// A canvas of half-block pixels, one string per pixel row (two per cell
// row): '.' black, 'k' dark grey, 'w' grey, 'W' white, 'G' light green.
Canvas pixels(const std::vector<std::string>& rows) {
    auto colour = [](char c) -> std::uint8_t {
        switch (c) {
        case 'k': return DarkGrey;
        case 'w': return Grey;
        case 'W': return White;
        case 'G': return LightGreen;
        default: return Black;
        }
    };
    const int w = int(rows.front().size()), h = int(rows.size()) / 2;
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = halfBlockCell({colour(rows[std::size_t(2 * y)][std::size_t(x)]),
                                        colour(rows[std::size_t(2 * y + 1)][std::size_t(x)])});
    return c;
}

std::vector<lint::Finding> of(const std::vector<lint::Finding>& all, Kind k) {
    std::vector<lint::Finding> out;
    std::copy_if(all.begin(), all.end(), std::back_inserter(out), [&](const auto& f) { return f.kind == k; });
    return out;
}

std::vector<lint::Finding> rules(const Canvas& c, Kind k, lint::RuleOptions o = {}) {
    return of(lint::checkRules(c, o), k);
}

} // namespace

TEST_CASE("a lone glint pixel is a stray; a cluster, a star or a detail of another hue isn't", "[lint][rules]") {
    // white on grey: a lighter rung of the steel ramp
    auto glint = pixels({
        "wwwww",
        "wwwww",
        "wwWww",
        "wwwww",
        "wwwww",
        "wwwww",
    });
    auto found = rules(glint, Kind::LonePixel);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 2);
    CHECK(found[0].y == 1);  // pixel row 2 is cell row 1's top
    CHECK(found[0].ramp.empty());
    CHECK(found[0].message.find("glint") != std::string::npos);

    // two pixels: a highlight
    CHECK(rules(pixels({"wwwww", "wwwww", "wwWWw", "wwwww", "wwwww", "wwwww"}), Kind::LonePixel).empty());
    // on black: a star
    CHECK(rules(pixels({".....", ".....", "..W..", ".....", ".....", "....."}), Kind::LonePixel).empty());
    // a darker pixel, or another hue (an LED on a grey panel), is a detail
    CHECK(rules(pixels({"wwwww", "wwwww", "wwkww", "wwwww", "wwwww", "wwwww"}), Kind::LonePixel).empty());
    CHECK(rules(pixels({"kkkkk", "kkkkk", "kkGkk", "kkkkk", "kkkkk", "kkkkk"}), Kind::LonePixel).empty());
    // a field of two colours round it isn't a field
    CHECK(rules(pixels({"wwwww", "wwwkw", "wwWww", "wwwww", "wwwww", "wwwww"}), Kind::LonePixel).empty());
}

TEST_CASE("a one-cell break in a thin line is a gap; a dashed or thick line isn't", "[lint][rules]") {
    const Cell space = cell(U' ', Grey, Black);
    const Cell rule = cell(U'─', White, Black);
    Canvas c = filled(14, 5, space);
    for (int x = 0; x < 14; ++x)
        c.at(x, 2) = rule;
    CHECK(rules(c, Kind::Gap).empty());

    c.at(6, 2) = space;
    auto found = rules(c, Kind::Gap);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 6);
    CHECK(found[0].y == 2);
    CHECK(found[0].message == "a one-cell break in a line across row 2");

    SECTION("down a column") {
        Canvas d = filled(5, 14, space);
        for (int y = 0; y < 14; ++y)
            d.at(2, y) = cell(U'│', White, Black);
        d.at(2, 7) = space;
        found = rules(d, Kind::Gap);
        REQUIRE(found.size() == 1);
        CHECK(found[0].x == 2);
        CHECK(found[0].y == 7);
        CHECK(found[0].message == "a one-cell break in a line down column 2");
    }
    SECTION("a dashed line breaks on a pattern") {
        for (const int x : {3, 9})
            c.at(x, 2) = space;
        CHECK(rules(c, Kind::Gap).empty());
    }
    SECTION("a line two cells thick is a band, not a line") {
        for (int x = 0; x < 14; ++x)
            c.at(x, 3) = rule;
        CHECK(rules(c, Kind::Gap).empty());
    }
    SECTION("a cell of another colour in the line isn't the field showing through") {
        c.at(6, 2) = cell(U'─', LightRed, Black);
        CHECK(rules(c, Kind::Gap).empty());
    }
}

TEST_CASE("a break along the canvas's edge isn't a gap: the line may be thicker past it", "[lint][rules]") {
    // the review's repro: a column of grey dither down the edge with a cell
    // of the field in it, which the lint called a break in a line
    const Cell space = cell(U' ', Grey, Black), dither = cell(kShadeLight, Grey, Black);
    for (const int x : {0, 4}) {
        INFO("column " << x);
        Canvas c = filled(5, 14, space);
        for (int y = 0; y < 14; ++y)
            c.at(x, y) = dither;
        c.at(x, 7) = space;
        CHECK(rules(c, Kind::Gap).empty());
        // one column in from the edge, the same break is a gap
        Canvas in = filled(5, 14, space);
        const int x1 = x == 0 ? 1 : 3;
        for (int y = 0; y < 14; ++y)
            in.at(x1, y) = dither;
        in.at(x1, 7) = space;
        const auto found = rules(in, Kind::Gap);
        REQUIRE(found.size() == 1);
        CHECK(found[0].x == x1);
        CHECK(found[0].y == 7);
    }
    // a line along the top or bottom row
    for (const int y : {0, 4}) {
        INFO("row " << y);
        Canvas c = filled(14, 5, space);
        for (int x = 0; x < 14; ++x)
            c.at(x, y) = cell(U'─', White, Black);
        c.at(6, y) = space;
        CHECK(rules(c, Kind::Gap).empty());
    }
}

TEST_CASE("a gap in a short line is found; a dash pattern of short dashes isn't one", "[lint][rules]") {
    const Cell space = cell(U' ', Grey, Black), red = cell(kFullBlock, Red, Black);
    for (const int arm : {2, 3, 4, 5, 6}) {
        INFO("arms of " << arm);
        // a blank row; a black cell, an arm, the gap, an arm, a black cell; a blank row
        Canvas c = filled(2 * arm + 3, 3, space);
        for (int i = 0; i < arm; ++i)
            c.at(1 + i, 1) = c.at(arm + 2 + i, 1) = red;
        auto found = rules(c, Kind::Gap);
        REQUIRE(found.size() == 1);
        CHECK(found[0].x == arm + 1);
        CHECK(found[0].y == 1);

        // the line running to the canvas's edges
        Canvas e = filled(2 * arm + 1, 3, space);
        for (int i = 0; i < arm; ++i)
            e.at(i, 1) = e.at(arm + 1 + i, 1) = red;
        found = rules(e, Kind::Gap);
        REQUIRE(found.size() == 1);
        CHECK(found[0].x == arm);

        // down a column
        Canvas d = filled(3, 2 * arm + 3, space);
        for (int i = 0; i < arm; ++i)
            d.at(1, 1 + i) = d.at(1, arm + 2 + i) = red;
        found = rules(d, Kind::Gap);
        REQUIRE(found.size() == 1);
        CHECK(found[0].y == arm + 1);
        CHECK(found[0].message == "a one-cell break in a line down column 1");
    }
    // dashes of two, three and four cells, each break a step of the pattern
    for (const int dash : {2, 3, 4}) {
        INFO("dashes of " << dash);
        Canvas c = filled(5 * (dash + 1) + 1, 3, space);
        for (int x = 1; x + 1 < c.width(); ++x)
            if (x % (dash + 1) != 0)
                c.at(x, 1) = red;
        CHECK(rules(c, Kind::Gap).empty());
    }
}

TEST_CASE("a glyph in its own background colour is a merge", "[lint][rules]") {
    Canvas c = filled(6, 3, cell(U' ', Grey, Blue));
    c.at(1, 1) = cell(U'A', Blue, Blue);
    c.at(3, 1) = cell(U'A', White, Blue);
    c.at(4, 1) = cell(kUpperHalf, Blue, Blue);  // a pixel cell: one flat colour, fine
    auto found = rules(c, Kind::Merge);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 1);
    CHECK(found[0].y == 1);

    // with iCE off a bright background shows its dark colour
    c.at(3, 1) = cell(U'A', Red, LightRed);
    lint::RuleOptions off;
    off.ice = false;
    CHECK(rules(c, Kind::Merge).size() == 1);
    CHECK(rules(c, Kind::Merge, off).size() == 2);
}

TEST_CASE("a 1 px line joined only at its corners is a dotted diagonal", "[lint][rules]") {
    auto found = rules(pixels({
                           "........",
                           ".W......",
                           "..W.....",
                           "...W....",
                           "....W...",
                           "........",
                       }),
                       Kind::DottedDiagonal);
    REQUIRE(found.size() == 1);
    // from its top end to its bottom end, in cells
    CHECK(found[0].x == 1);
    CHECK(found[0].y == 0);
    CHECK(found[0].x2 == 4);
    CHECK(found[0].y2 == 2);
    CHECK(found[0].message.find("4 pixels") != std::string::npos);

    // down to the left: the ends keep their direction
    found = rules(pixels({"........", "....W...", "...W....", "..W.....", ".W......", "........"}),
                  Kind::DottedDiagonal);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 4);
    CHECK(found[0].x2 == 1);

    // three pixels is a corner or a detail
    CHECK(rules(pixels({"........", ".W......", "..W.....", "...W....", "........", "........"}),
                Kind::DottedDiagonal)
              .empty());
    // stepped 4-connected, it is a line
    CHECK(rules(pixels({"........", ".WW.....", "..WW....", "...WW...", "....WW..", "........"}),
                Kind::DottedDiagonal)
              .empty());
    // a checker is a dither, not a line
    CHECK(rules(pixels({"W.W.W.W.", ".W.W.W.W", "W.W.W.W.", ".W.W.W.W", "W.W.W.W.", ".W.W.W.W"}),
                Kind::DottedDiagonal)
              .empty());
}

TEST_CASE("a block of flat windows is a flat area", "[lint][rules]") {
    Canvas c = filled(16, 4, cell(U' ', Grey, Blue));  // 4 x 2 windows of one look
    lint::RuleOptions boxed;
    boxed.box = lint::Box{0, 0, 15, 3};
    auto found = rules(c, Kind::Flat, boxed);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 0);
    CHECK(found[0].y == 0);
    CHECK(found[0].x2 == 15);
    CHECK(found[0].y2 == 3);
    CHECK(found[0].message.find("64 cells in 16x4") != std::string::npos);

    // under minFlatCells
    lint::RuleOptions o = boxed;
    o.minFlatCells = 65;
    CHECK(rules(c, Kind::Flat, o).empty());
    CHECK(rules(filled(8, 4, cell(U' ', Grey, Blue)), Kind::Flat, boxed).empty());
    // black is the ground
    CHECK(rules(filled(16, 4, cell(U' ', Grey, Black)), Kind::Flat, boxed).empty());
    // texture in every 4x2 window, at any offset, breaks it up
    for (int y = 0; y < 4; y += 2)
        for (int x = 0; x < 16; x += 4)
            c.at(x + 1, y) = cell(kShadeLight, Cyan, Blue);
    CHECK(rules(c, Kind::Flat, boxed).empty());
    // the box clips the area: only its part in the box is counted
    Canvas wide = filled(16, 4, cell(U' ', Grey, Blue));
    o = boxed;
    o.box = lint::Box{4, 0, 30, 3};
    found = rules(wide, Kind::Flat, o);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 4);
    CHECK(found[0].message.find("48 cells in 12x4") != std::string::npos);
}

TEST_CASE("flat is judged in the box, where a field is found the same at any offset", "[lint][rules]") {
    const Cell blue = cell(U' ', Grey, Blue);
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 4; ++dx) {
            INFO("the field at " << dx << "," << dy + 1);
            // a 12x4 field of one look: 48 cells, the least a flat area has
            Canvas c = filled(20, 8, cell(U' ', Grey, Black));
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 12; ++x)
                    c.at(dx + x, dy + 1 + y) = blue;
            lint::LintOptions o;
            o.box = lint::Box{0, 0, 19, 7};
            auto found = of(lint::lintCanvas(c, o).findings, Kind::Flat);
            REQUIRE(found.size() == 1);
            CHECK(found[0].x == dx);
            CHECK(found[0].y == dy + 1);
            CHECK(found[0].x2 == dx + 11);
            CHECK(found[0].y2 == dy + 4);
            CHECK(found[0].message.find("48 cells in 12x4") != std::string::npos);
            // a field a cell short of that isn't one
            for (int y = 0; y < 4; ++y)
                c.at(dx + 11, dy + 1 + y) = cell(U' ', Grey, Black);
            CHECK(of(lint::lintCanvas(c, o).findings, Kind::Flat).empty());
        }
}

TEST_CASE("without a box flat isn't judged: grounds and backdrops are the artist's call", "[lint][rules][golden]") {
    // a ground of one look over the whole canvas
    CHECK(of(lint::lintCanvas(filled(16, 4, cell(U' ', Grey, Blue))).findings, Kind::Flat).empty());
    // cocktober's magenta backdrop (0,14, 44x32)
    const Canvas c = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "cocktober.ans")).canvas;
    lint::LintOptions o;
    o.ramps = namedRamps();
    CHECK(of(lint::lintCanvas(c, o).findings, Kind::Flat).empty());
    // its subject boxed is judged: the backdrop is flat
    o.box = lint::Box{0, 14, 43, 45};
    CHECK_FALSE(of(lint::lintCanvas(c, o).findings, Kind::Flat).empty());
}

TEST_CASE("the black cells and blank rows IRC drops are found where they show", "[lint][rules][irc]") {
    const Cell space = cell(U' ', Grey, Black), solid = cell(kFullBlock, White, Black);
    Canvas c = filled(10, 6, space);
    for (int x = 0; x < 10; ++x)
        c.at(x, 1) = c.at(x, 3) = solid;
    for (int x = 0; x < 4; ++x)
        c.at(x, 2) = solid;

    const auto all = lint::checkRules(c);
    auto trailing = of(all, Kind::TrailingBlack);
    REQUIRE(trailing.size() == 1);
    CHECK(trailing[0].x == 4);
    CHECK(trailing[0].y == 2);
    CHECK(trailing[0].x2 == 9);
    CHECK(trailing[0].y2 == 2);
    CHECK(trailing[0].message == "cells 4-9 of row 2 are black to the edge, beside rows that reach it: IRC drops them");

    auto blank = of(all, Kind::BlankEdgeRows);
    REQUIRE(blank.size() == 2);
    CHECK((blank[0].y == 0 && blank[0].y2 == 0));
    CHECK((blank[1].y == 4 && blank[1].y2 == 5));
    CHECK(blank[1].message.find("rows 4-5 below the art") == 0);

    SECTION("the encoder drops the same cells") {
        const auto lines = irc::encodeCanvas(c);
        CHECK(lines[0].empty());
        CHECK(lines[4].empty());
        CHECK(lines[5].empty());
        CHECK(irc::exportCanvas(c).size() == 3);
    }
    SECTION("a black-on-black block keeps the row's end, as a margin") {
        for (int x = 4; x < 10; ++x)
            c.at(x, 2) = cell(kFullBlock, Black, Black);
        c.at(0, 0) = c.at(0, 4) = c.at(0, 5) = cell(kFullBlock, Black, Black);
        CHECK(of(lint::checkRules(c), Kind::TrailingBlack).size() == 2);  // rows 0 and 4 now
        CHECK(of(lint::checkRules(c), Kind::BlankEdgeRows).empty());
    }
    SECTION("rows short of the edge beside rows also short of it are the art's own shape") {
        for (int x = 8; x < 10; ++x)
            c.at(x, 1) = c.at(x, 3) = space;
        CHECK(of(lint::checkRules(c), Kind::TrailingBlack).empty());
    }
    SECTION("a blank canvas has nothing to trim") {
        CHECK(lint::checkRules(filled(10, 6, space)).empty());
    }
}

TEST_CASE("a glyph with no CP437 code is found; CP437's own aren't", "[lint][rules]") {
    Canvas c = filled(6, 2, cell(U' ', Grey, Black));
    c.at(1, 0) = cell(U'☃', White, Black);
    c.at(2, 0) = cell(U'░', White, Black);
    c.at(3, 0) = cell(U'é', White, Black);
    c.at(4, 0) = cell(char32_t(0x01), White, Black);  // a control code is CP437's ☺
    auto found = rules(c, Kind::NoCp437);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 1);
    CHECK(found[0].message == "U+2603 ☃ has no CP437 code: an .ans saves it as '?'");
}

TEST_CASE("no-cp437 names a control or bidi override by its code point and never echoes it", "[lint][rules]") {
    // #327 finding 3: a UTF-8 .ans can put these in cells. U+009B is a CSI
    // to a terminal that honours C1; U+202E reverses the rest of the line.
    Canvas c = filled(6, 1, cell(U' ', Grey, Black));
    c.at(0, 0) = cell(char32_t(0x9B), White, Black);
    c.at(1, 0) = cell(char32_t(0x202E), White, Black);
    c.at(2, 0) = cell(char32_t(0x2066), White, Black);
    c.at(3, 0) = cell(char32_t(0x200F), White, Black);
    c.at(4, 0) = cell(char32_t(0x2028), White, Black);
    c.at(5, 0) = cell(U'★', White, Black);
    const auto found = rules(c, Kind::NoCp437);
    REQUIRE(found.size() == 6);
    CHECK(found[0].message == "U+009B has no CP437 code: an .ans saves it as '?'");
    CHECK(found[1].message == "U+202E has no CP437 code: an .ans saves it as '?'");
    CHECK(found[2].message == "U+2066 has no CP437 code: an .ans saves it as '?'");
    CHECK(found[3].message == "U+200F has no CP437 code: an .ans saves it as '?'");
    CHECK(found[4].message == "U+2028 has no CP437 code: an .ans saves it as '?'");
    CHECK(found[5].message == "U+2605 ★ has no CP437 code: an .ans saves it as '?'");
    for (std::size_t i = 0; i < 5; ++i)
        CHECK(std::all_of(found[i].message.begin(), found[i].message.end(),
                          [](char ch) { return ch >= 0x20 && ch < 0x7F; }));
}

TEST_CASE("no-cp437 names an invisible glyph or a noncharacter by its code point alone", "[lint][rules]") {
    // review of #332, N1 and N2: U+FE0F (the second half of ❤️), a
    // lone U+0301, the Hangul filler U+3164 and the noncharacter U+FFFE show
    // nothing, or a mark on the cell before, so only their code is given
    Canvas c = filled(6, 1, cell(U' ', Grey, Black));
    c.at(0, 0) = cell(U'\u2764', White, Black);
    c.at(1, 0) = cell(char32_t(0xFE0F), White, Black);
    c.at(2, 0) = cell(char32_t(0x0301), White, Black);
    c.at(3, 0) = cell(char32_t(0x3164), White, Black);
    c.at(4, 0) = cell(char32_t(0xFFFE), White, Black);
    c.at(5, 0) = cell(char32_t(0x034F), White, Black);
    const auto found = rules(c, Kind::NoCp437);
    REQUIRE(found.size() == 6);
    CHECK(found[0].message == "U+2764 ❤ has no CP437 code: an .ans saves it as '?'");
    CHECK(found[1].message == "U+FE0F has no CP437 code: an .ans saves it as '?'");
    CHECK(found[2].message == "U+0301 has no CP437 code: an .ans saves it as '?'");
    CHECK(found[3].message == "U+3164 has no CP437 code: an .ans saves it as '?'");
    CHECK(found[4].message == "U+FFFE has no CP437 code: an .ans saves it as '?'");
    CHECK(found[5].message == "U+034F has no CP437 code: an .ans saves it as '?'");
}

TEST_CASE("lint text and JSON name a noncharacter from a UTF-8 file by its code point alone", "[lint][rules]") {
    // review of #332, N2: a UTF-8 .ans of █, U+FFFE, U+FE0F and U+3164, a
    // save cursor (ESC[s) between them so the two zero-width ones don't join
    // the glyph before them and each keeps a cell of its own
    const AnsiDocument doc = parseAnsi("\xe2\x96\x88\xef\xbf\xbe\x1b[s\xef\xb8\x8f\x1b[s\xe3\x85\xa4\r\n",
                                       {.encoding = TextEncoding::Utf8});
    const lint::LintReport r = bwcli::lintDocument(doc, {});
    const std::string text = bwcli::lintText(r);
    CHECK(text.find("1,0 no-cp437 - U+FFFE has no CP437 code: an .ans saves it as '?'\n") != std::string::npos);
    CHECK(text.find("2,0 no-cp437 - U+FE0F has no CP437 code") != std::string::npos);
    CHECK(text.find("3,0 no-cp437 - U+3164 has no CP437 code") != std::string::npos);
    const std::string json = bwcli::lintJson(r);
    for (const std::string& out : {text, json}) {
        INFO(out);
        CHECK(out.find("\xef\xbf\xbe") == std::string::npos);  // U+FFFE
        CHECK(out.find("\xef\xb8\x8f") == std::string::npos);  // U+FE0F
        CHECK(out.find("\xe3\x85\xa4") == std::string::npos);  // U+3164
    }
    // still one valid JSON document, the code given once, not escaped again
    QJsonParseError error;
    const QJsonDocument parsed = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    std::vector<std::string> messages;
    for (const QJsonValue& f : parsed.object().value(QStringLiteral("findings")).toArray())
        if (f.toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("no-cp437"))
            messages.push_back(f.toObject().value(QStringLiteral("message")).toString().toStdString());
    REQUIRE(messages.size() == 3);
    CHECK(messages[0] == "U+FFFE has no CP437 code: an .ans saves it as '?'");
    CHECK(messages[1] == "U+FE0F has no CP437 code: an .ans saves it as '?'");
    CHECK(messages[2] == "U+3164 has no CP437 code: an .ans saves it as '?'");
}

TEST_CASE("a glyph whose CP437 byte .ans reads as a control code is found, with why", "[lint][rules]") {
    // #300: an .ans save writes these as '?', so no-cp437 flags them too
    Canvas c = filled(7, 1, cell(U' ', Grey, Black));
    const std::u32string row = U"○◙♪→←•☺";
    for (int x = 0; x < 7; ++x)
        c.at(x, 0) = cell(row[std::size_t(x)], White, Black);
    auto found = rules(c, Kind::NoCp437);
    REQUIRE(found.size() == 5);
    CHECK(found[0].x == 0);
    CHECK(found[0].message ==
          "U+25CB ○ is CP437 0x09, which .ans reads as TAB: an .ans saves it as '?' (XBIN and BIN keep it)");
    CHECK(found[1].message ==
          "U+25D9 ◙ is CP437 0x0A, which .ans reads as LF: an .ans saves it as '?' (XBIN and BIN keep it)");
    CHECK(found[2].message ==
          "U+266A ♪ is CP437 0x0D, which .ans reads as CR: an .ans saves it as '?' (XBIN and BIN keep it)");
    CHECK(found[3].message ==
          "U+2192 → is CP437 0x1A, which .ans reads as SUB: an .ans saves it as '?' (XBIN and BIN keep it)");
    CHECK(found[4].message ==
          "U+2190 ← is CP437 0x1B, which .ans reads as ESC: an .ans saves it as '?' (XBIN and BIN keep it)");
    CHECK(found[4].x == 4);  // • and ☺ (0x07, 0x01) draw, so aren't found
}

TEST_CASE("kinds have distinct names and only the ladder's name a ramp", "[lint][rules]") {
    std::set<std::string_view> names;
    for (int k = int(Kind::NonNeighbourShade); k <= int(Kind::BidiControl); ++k)
        names.insert(lint::kindName(Kind(k)));
    CHECK(names.size() == 18);
    CHECK(lint::kindName(Kind::RungSkip) == "rung-skip");
    CHECK(lint::ladderKind(Kind::OneGlyph));
    CHECK_FALSE(lint::ladderKind(Kind::Noise));
    CHECK_FALSE(lint::ladderKind(Kind::LonePixel));
}

TEST_CASE("the summary counts the findings by kind, in the kinds' order", "[lint][rules]") {
    lint::LintReport r;
    CHECK(lint::summary(r) == "clean, noise 0.0000, iCE on");
    r.findings = {{Kind::Gap, "", 0, 0, 0, 0, ""}, {Kind::Speckle, "steel", 1, 0, 1, 0, ""},
                  {Kind::Gap, "", 2, 0, 2, 0, ""}};
    r.noise = 0.00714;
    r.ice = false;
    CHECK(lint::summary(r) == "3 findings (1 speckle, 2 gap), noise 0.0071, iCE off");
    r.findings.resize(1);
    CHECK(lint::summary(r) == "1 finding (1 gap), noise 0.0071, iCE off");
}

namespace {

// Steel (k K w W, rungs 0, 4, 8, 12) planted as the ladder tests plant it,
// in four places on black: rung skips (K▓k to w░K to w) at the top left, a
// wide rung (w▒K six cells wide) below them, a speck at 23,3 and a shade of
// non-neighbours at 23,9.
Canvas planted() {
    const Ramp steel = *findRamp("steel");
    Canvas c = filled(30, 12, cell(U' ', Black, Black));
    for (int y = 0; y < 6; ++y) {
        for (int x = 4; x <= 6; ++x)
            c.at(x, y) = cell(kShadeDark, DarkGrey, Black);
        for (int x = 7; x <= 9; ++x)
            c.at(x, y) = cell(kShadeLight, Grey, DarkGrey);
        for (int x = 10; x <= 11; ++x)
            c.at(x, y) = cell(U' ', Black, Grey);
    }
    for (int y = 6; y < 12; ++y)
        for (int x = 0; x < 12; ++x)
            c.at(x, y) = rungCell(steel, x < 2 ? 4 : x < 3 ? 5 : x < 9 ? 6 : x < 10 ? 7 : 8);
    for (int y = 0; y < 12; ++y)
        for (int x = 18; x < 30; ++x)
            c.at(x, y) = cell(U' ', Black, Grey);
    c.at(23, 3) = rungCell(steel, 10);
    c.at(23, 9) = cell(kShadeMedium, White, DarkGrey);
    return c;
}

bool has(const lint::LintReport& r, Kind k, int x, int y, const std::string& ramp) {
    return std::any_of(r.findings.begin(), r.findings.end(),
                       [&](const auto& f) { return f.kind == k && f.x == x && f.y == y && f.ramp == ramp; });
}

} // namespace

TEST_CASE("the lint reports the ladder check's findings under the ramp they came from", "[lint][rules]") {
    const Ramp steel = *findRamp("steel");
    const Canvas c = planted();
    lint::LintOptions o;
    o.ramps = {steel};
    const auto r = lint::lintCanvas(c, o);

    lint::LadderOptions lo;
    lo.fallback = true;
    const auto ladder = lint::checkLadder(c, steel, std::nullopt, lo);
    REQUIRE_FALSE(ladder.findings.empty());
    for (const auto& lf : ladder.findings) {
        INFO(lf.message);
        CHECK(has(r, lint::Finding::Kind(int(lf.kind)), lf.x, lf.y, "steel"));
    }
    CHECK(has(r, Kind::RungSkip, 6, 2, "steel"));
    CHECK(has(r, Kind::WideRung, 3, 8, "steel"));
    CHECK(has(r, Kind::Speckle, 23, 3, "steel"));
    CHECK(has(r, Kind::NonNeighbourShade, 23, 9, "steel"));
    // a skip names its other cell, across the jump; a wide rung its run's end
    for (const auto& f : r.findings) {
        if (f.kind == Kind::RungSkip && f.x == 6 && f.y == 2)
            CHECK((f.x2 == 7 && f.y2 == 2));
        if (f.kind == Kind::WideRung && f.x == 3 && f.y == 8)
            CHECK((f.x2 == 8 && f.y2 == 8));
    }
    // in reading order
    CHECK(std::is_sorted(r.findings.begin(), r.findings.end(),
                         [](const auto& a, const auto& b) { return std::pair{a.y, a.x} < std::pair{b.y, b.x}; }));
}

TEST_CASE("a finding seen under two ramps is reported once, under the first", "[lint][rules]") {
    const Ramp steel = *findRamp("steel");
    const Ramp twin{"twin", steel.colours};
    lint::LintOptions one, both;
    one.ramps = {steel};
    both.ramps = {steel, twin};
    const auto a = lint::lintCanvas(planted(), one), b = lint::lintCanvas(planted(), both);
    CHECK(a.findings == b.findings);
    both.ramps = {twin, steel};
    const auto c = lint::lintCanvas(planted(), both);
    CHECK(c.findings.size() == a.findings.size());
    CHECK(std::none_of(c.findings.begin(), c.findings.end(), [](const auto& f) { return f.ramp == "steel"; }));
}

TEST_CASE("the box keeps the findings with a cell in it, each as the whole lint has it", "[lint][rules]") {
    lint::LintOptions o;
    o.ramps = {*findRamp("steel")};
    const auto whole = lint::lintCanvas(planted(), o);
    o.box = lint::Box{18, 0, 29, 5};
    const auto boxed = lint::lintCanvas(planted(), o);
    REQUIRE_FALSE(boxed.findings.empty());
    CHECK(boxed.findings.size() < whole.findings.size());
    CHECK(has(boxed, Kind::Speckle, 23, 3, "steel"));
    CHECK_FALSE(has(boxed, Kind::RungSkip, 6, 2, "steel"));
    CHECK_FALSE(has(boxed, Kind::NonNeighbourShade, 23, 9, "steel"));
    for (const auto& f : boxed.findings) {
        CHECK(std::max(f.x, f.x2) >= 18);
        CHECK(std::min(f.y, f.y2) <= 5);
        // flat is judged only in a box, and lies in it
        if (f.kind == Kind::Flat) {
            CHECK(std::min(f.x, f.x2) >= 18);
            CHECK(std::max(f.x, f.x2) <= 29);
            CHECK(std::max(f.y, f.y2) <= 5);
        } else {
            CHECK(std::find(whole.findings.begin(), whole.findings.end(), f) != whole.findings.end());
        }
    }
    CHECK(std::none_of(whole.findings.begin(), whole.findings.end(), [](const auto& f) { return f.kind == Kind::Flat; }));
    // corners either way round
    o.box = lint::Box{29, 5, 18, 0};
    CHECK(lint::lintCanvas(planted(), o).findings == boxed.findings);
}

TEST_CASE("the box keeps a dotted diagonal by its pixels' cells, not its corners", "[lint][rules]") {
    // eight pixels from 0,0 down to 7,3 (cells): ▀ and ▄ by turns
    const Canvas c = pixels({
        "W.......",
        ".W......",
        "..W.....",
        "...W....",
        "....W...",
        ".....W..",
        "......W.",
        ".......W",
    });
    lint::LintOptions o;
    o.ramps = {*findRamp("steel")};
    REQUIRE(of(lint::lintCanvas(c, o).findings, Kind::DottedDiagonal).size() == 1);
    // the top right corner of its rectangle: black, none of its cells
    o.box = lint::Box{6, 0, 7, 0};
    CHECK(of(lint::lintCanvas(c, o).findings, Kind::DottedDiagonal).empty());
    o.box = lint::Box{0, 2, 1, 3};
    CHECK(of(lint::lintCanvas(c, o).findings, Kind::DottedDiagonal).empty());
    // a cell of it
    o.box = lint::Box{6, 3, 7, 3};
    CHECK(of(lint::lintCanvas(c, o).findings, Kind::DottedDiagonal).size() == 1);
    o.box = lint::Box{2, 1, 2, 1};
    CHECK(of(lint::lintCanvas(c, o).findings, Kind::DottedDiagonal).size() == 1);
}

TEST_CASE("a shade of neighbours on any ramp linted is no fault", "[lint][rules]") {
    // ░░▒▒▓▓ of LightRed over Red: neighbours on fire, not on skin (brown is between)
    Canvas c = filled(8, 1, cell(U' ', Black, Black));
    const char32_t shades[] = {kShadeLight, kShadeLight, kShadeMedium, kShadeMedium, kShadeDark, kShadeDark};
    for (int x = 0; x < 6; ++x)
        c.at(x + 1, 0) = cell(shades[x], LightRed, Red);
    const Ramp fire = *findRamp("fire"), skin = *findRamp("skin");
    auto shades_ = [&](std::vector<Ramp> ramps) {
        lint::LintOptions o;
        o.ramps = std::move(ramps);
        return of(lint::lintCanvas(c, o).findings, Kind::NonNeighbourShade);
    };
    CHECK(shades_(namedRamps()).empty());
    CHECK(shades_({fire, skin}).empty());
    CHECK(shades_({skin, fire}).empty());
    CHECK(shades_({skin}).size() == 6);

    SECTION("magenta on black is violet's, yellow on brown wood's") {
        c.at(1, 0) = cell(kShadeLight, Magenta, Black);
        c.at(2, 0) = cell(kShadeMedium, Yellow, Brown);
        CHECK(shades_(namedRamps()).empty());
        CHECK(shades_({*findRamp("sunset"), skin}).size() == 6);
    }
    SECTION("a shade of colours no ramp has as neighbours is one, and says its colours") {
        c.at(1, 0) = cell(kShadeMedium, White, DarkGrey);
        const auto found = shades_(namedRamps());
        REQUIRE(found.size() == 1);
        CHECK(found[0].x == 1);
        CHECK(found[0].ramp == "steel");
        CHECK(found[0].message == "cell 1,0: ▒ of 15 on 8, colours no ramp linted has as neighbours");
    }
}

namespace {

// ░░▒▒▓▓ of `light` over `dark` between black cells, linted under every
// named ramp, with or without the craft pairs: its non-neighbour shades.
std::vector<lint::Finding> ladderShades(std::uint8_t light, std::uint8_t dark, bool craftPairs) {
    Canvas c = filled(8, 1, cell(U' ', Black, Black));
    const char32_t shades[] = {kShadeLight, kShadeLight, kShadeMedium, kShadeMedium, kShadeDark, kShadeDark};
    for (int x = 0; x < 6; ++x)
        c.at(x + 1, 0) = cell(shades[x], light, dark);
    lint::LintOptions o;
    o.ramps = namedRamps();
    o.craftPairs = craftPairs;
    return of(lint::lintCanvas(c, o).findings, Kind::NonNeighbourShade);
}

} // namespace

TEST_CASE("the review's repros: brown and yellow into black are no fault by default", "[lint][rules]") {
    // printf '\033[0;33;40m\260\260\261\261\262\262' and the same in 1;33
    CHECK(ladderShades(Brown, Black, true).empty());
    CHECK(ladderShades(Yellow, Black, true).empty());
    // what they gave before: skin's Black and Brown, fire's Black and Yellow
    CHECK(ladderShades(Brown, Black, false).size() == 6);
    CHECK(ladderShades(Yellow, Black, false).size() == 6);
}

TEST_CASE("every craft pair is no fault under the default lint", "[lint][rules]") {
    struct Pair {
        std::uint8_t light, dark;
        const char* from;
    };
    const Pair allowed[] = {
        // into black: every hue (a saturated hero colour, a one-hue ladder
        // over black), dark grey and grey
        {Blue, Black, "night k b"},
        {Green, Black, "green k g"},
        {Cyan, Black, "a one-hue ladder over black"},
        {Red, Black, "fire k r"},
        {Magenta, Black, "cool cloth k m"},
        {Brown, Black, "warm skin k n"},
        {Grey, Black, "two lights k w"},
        {DarkGrey, Black, "dark object k K"},
        {LightBlue, Black, "a saturated hero colour into black"},
        {LightGreen, Black, "a saturated hero colour into black"},
        {LightCyan, Black, "a saturated hero colour into black"},
        {LightRed, Black, "a saturated hero colour into black"},
        {LightMagenta, Black, "a saturated hero colour into black"},
        {Yellow, Black, "bright cloth k Y, Y▓k"},
        // the measured ladders' other steps
        {LightBlue, Blue, "night b B"},
        {White, LightBlue, "W▓B"},
        {Grey, DarkGrey, "pale skin K w"},
        {White, Grey, "pale skin w W"},
        {LightRed, Red, "fire r R"},
        {Yellow, LightRed, "fire R Y"},
        {Red, Magenta, "pockets m░r"},
        {LightRed, Brown, "warm skin n R"},
        {LightRed, Grey, "top planes R▓w"},
        {LightGreen, Green, "green g G"},
        {White, Yellow, "bright cloth Y W"},
        {LightBlue, Magenta, "cool cloth B▓m"},
        {LightBlue, Cyan, "cool cloth B▒c"},
        {Grey, Cyan, "cool cloth c w"},
        {Magenta, DarkGrey, "K▓m"},
        {LightCyan, Cyan, "C░c"},
        {Yellow, Red, "Y░r"},
        {Yellow, Brown, "hair Y░n"},
        {LightGreen, DarkGrey, "storm green G▒K"},
        {Red, DarkGrey, "crest r▒K"},
        {Green, DarkGrey, "storm swells k K g"},
    };
    for (const Pair& p : allowed) {
        INFO(int(p.light) << " on " << int(p.dark) << ": " << p.from);
        CHECK(ladderShades(p.light, p.dark, true).empty());
        CHECK(lint::craftPair(p.light, p.dark));
        CHECK(lint::craftPair(p.dark, p.light));
    }
}

TEST_CASE("pairs that are not craft pairs are still flagged", "[lint][rules]") {
    const std::pair<std::uint8_t, std::uint8_t> flagged[] = {
        {White, Black},         // white on black is a stroke, not a ladder
        {LightRed, Magenta},    // the pocket is m░r, not m░R
        {Yellow, Green},        // grass is g G Y
        {White, Red},
        {White, DarkGrey},      // the ladder is K w W
        {White, Blue},
        {LightCyan, Blue},      // ice is b B C
    };
    for (const auto& [light, dark] : flagged) {
        INFO(int(light) << " on " << int(dark));
        CHECK(ladderShades(light, dark, true).size() == 6);
        CHECK_FALSE(lint::craftPair(light, dark));
    }
}

TEST_CASE("a ramp linted alone doesn't take the craft pairs", "[lint][rules]") {
    // --ramp skin asks whether the piece is shaded on skin: Brown over Black
    // is not one of skin's steps, whatever warm skin does
    Canvas c = filled(8, 1, cell(U' ', Black, Black));
    for (int x = 1; x < 7; ++x)
        c.at(x, 0) = cell(kShadeMedium, Brown, Black);
    lint::LintOptions o;
    o.ramps = {*findRamp("skin")};
    CHECK(of(lint::lintCanvas(c, o).findings, Kind::NonNeighbourShade).size() == 6);
    o.craftPairs = true;
    CHECK(of(lint::lintCanvas(c, o).findings, Kind::NonNeighbourShade).empty());
}

TEST_CASE("the named ramps put the colours they share in the same order", "[lint][rules]") {
    // so a step that is up on one ramp is up on every ramp that reads it,
    // and only which colours are neighbours differs between them
    for (const Ramp& a : namedRamps())
        for (const Ramp& b : namedRamps())
            for (const std::uint8_t c : a.colours)
                for (const std::uint8_t d : a.colours) {
                    if (b.indexOf(c) < 0 || b.indexOf(d) < 0)
                        continue;
                    INFO(a.name << " and " << b.name << ": " << int(c) << ", " << int(d));
                    CHECK((a.indexOf(c) < a.indexOf(d)) == (b.indexOf(c) < b.indexOf(d)));
                }
}

TEST_CASE("noise over the limit is one finding of no ramp; a bad ramp is refused", "[lint][rules]") {
    // a checker of pixels: every pixel unlike its neighbours
    std::vector<std::string> rows;
    for (int py = 0; py < 12; ++py) {
        std::string row;
        for (int x = 0; x < 12; ++x)
            row += (x + py) % 2 ? 'W' : 'k';
        rows.push_back(row);
    }
    const Canvas c = pixels(rows);
    const auto r = lint::lintCanvas(c);
    CHECK(r.noise > 0.02);
    auto noise = of(r.findings, Kind::Noise);
    REQUIRE(noise.size() == 1);
    CHECK(noise[0].ramp.empty());
    CHECK((noise[0].x2 == 11 && noise[0].y2 == 5));
    lint::LintOptions lax;
    lax.maxNoise = 1.0;
    CHECK(of(lint::lintCanvas(c, lax).findings, Kind::Noise).empty());

    lint::LintOptions bad;
    bad.ramps = {Ramp{"one", {Grey}}};
    CHECK_THROWS_AS(lint::lintCanvas(c, bad), lint::LintError);
    CHECK(lint::lintCanvas(Canvas(0, 0)).findings.empty());
}

TEST_CASE("the funkpower pieces lint in under a second each", "[lint][rules][golden]") {
    lint::LintOptions o;
    o.ramps = namedRamps();
    int pieces = 0;
    for (const auto& dir : {"golden", "brushes"})
        for (const auto& e : std::filesystem::directory_iterator(bwtest::dataDir() / dir)) {
            if (e.path().extension() != ".ans")
                continue;
            const AnsiDocument doc = parseAnsi(bwtest::readFile(e.path()));
            o.ice = doc.ice;
            const auto t0 = std::chrono::steady_clock::now();
            const auto r = lint::lintCanvas(doc.canvas, o);
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            INFO(e.path().filename().string() << ": " << ms << " ms");
            CHECK(ms < 1000);
            CHECK(r.ice == doc.ice);
            ++pieces;
        }
    CHECK(pieces >= 20);
}
