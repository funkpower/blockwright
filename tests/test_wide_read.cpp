// Reading a UTF-8 .ans into pairs and clusters (#334 slice 6, PLAN §16,
// Reading), what the read reports it dropped, and the lint rules for glyphs
// whose width or order a client may not agree on.
#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/glyph.h"
#include "blockwright/lint_rules.h"
#include "blockwright/width.h"

#include "../cli/loss.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace bw;
using Kind = lint::Finding::Kind;

namespace {

AnsiDocument utf8(std::string_view bytes, int width = 80, std::size_t poolCap = ClusterPool::kLossyCap) {
    ParseOptions o;
    o.defaultWidth = width;
    o.encoding = TextEncoding::Utf8;
    o.poolCap = poolCap;
    return parseAnsi(bytes, o);
}

// A cell's code points, base first; none for the continuation.
std::u32string cps(const AnsiDocument& d, int x, int y) {
    const GlyphCodePoints g = codePoints(d.canvas.at(x, y).ch);
    return {g.begin(), g.end()};
}

bool continuation(const AnsiDocument& d, int x, int y) { return d.canvas.at(x, y).ch.isContinuation(); }

std::vector<lint::Finding> found(const Canvas& c, Kind k) {
    std::vector<lint::Finding> out;
    for (const lint::Finding& f : lint::checkRules(c))
        if (f.kind == k)
            out.push_back(f);
    return out;
}

bool has(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

} // namespace

TEST_CASE("a wide cluster reads as a lead and its continuation", "[utf8][wide-read]") {
    SECTION("a family emoji at column 0: leads at 0, 2 and 4") {
        // U+1F468 ZWJ U+1F469 ZWJ U+1F467: three wide code points, each
        // ZWJ joining the one before it
        const AnsiDocument d = utf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7!");
        CHECK(cps(d, 0, 0) == U"\U0001F468\u200D");
        CHECK(continuation(d, 1, 0));
        CHECK(cps(d, 2, 0) == U"\U0001F469\u200D");
        CHECK(continuation(d, 3, 0));
        CHECK(cps(d, 4, 0) == U"\U0001F467");
        CHECK(continuation(d, 5, 0));
        CHECK(cps(d, 6, 0) == U"!");
        CHECK_FALSE(d.readLoss.lossy());
    }
    SECTION("the continuation takes the lead's colours") {
        const AnsiDocument d = utf8("\x1b[1;33;44m\xE4\xB8\xAD");
        CHECK(d.canvas.at(1, 0).fg == d.canvas.at(0, 0).fg);
        CHECK(d.canvas.at(1, 0).bg == d.canvas.at(0, 0).bg);
    }
    SECTION("at the last column it wraps first, as xterm does") {
        const AnsiDocument d = utf8("abcd\xE4\xB8\xAD" "e", 5);
        CHECK(cps(d, 3, 0) == U"d");
        CHECK(d.canvas.at(4, 0).ch == Glyph(U' '));  // left as it was
        CHECK(cps(d, 0, 1) == U"中");
        CHECK(continuation(d, 1, 1));
        CHECK(cps(d, 2, 1) == U"e");
    }
    SECTION("filling the row exactly, it doesn't wrap early") {
        const AnsiDocument d = utf8("abc\xE4\xB8\xAD" "e", 5);
        CHECK(cps(d, 3, 0) == U"中");
        CHECK(continuation(d, 4, 0));
        CHECK(cps(d, 0, 1) == U"e");
    }
    SECTION("on a one-column canvas: U+FFFD, which takes the marks after it") {
        const AnsiDocument d = utf8("\xE4\xB8\xAD\xCC\x81" "a", 1);
        CHECK(cps(d, 0, 0) == U"\uFFFD\u0301");
        CHECK(cps(d, 0, 1) == U"a");
    }
    SECTION("writing over half a pair breaks it into spaces") {
        const AnsiDocument right = utf8("\xE4\xB8\xAD\x1b[1Dx");
        CHECK(right.canvas.at(0, 0).ch == Glyph(U' '));
        CHECK(cps(right, 1, 0) == U"x");
        const AnsiDocument left = utf8("\xE4\xB8\xAD\x1b[2Dx");
        CHECK(cps(left, 0, 0) == U"x");
        CHECK(left.canvas.at(1, 0).ch == Glyph(U' '));
    }
}

TEST_CASE("a zero-width code point joins the last cluster unless the cursor moved since", "[utf8][wide-read]") {
    SECTION("at the start of the file: a cell of its own") {
        const AnsiDocument d = utf8("\xCC\x81" "a");
        CHECK(cps(d, 0, 0) == U"\u0301");
        CHECK(cps(d, 1, 0) == U"a");
    }
    SECTION("after a cursor move: a cell of its own") {
        const AnsiDocument d = utf8("a\x1b[C\xCC\x81");
        CHECK(cps(d, 0, 0) == U"a");
        CHECK(cps(d, 2, 0) == U"\u0301");
    }
    SECTION("after CR or LF: a cell of its own") {
        const AnsiDocument lf = utf8("a\r\n\xCC\x81");
        CHECK(cps(lf, 0, 0) == U"a");
        CHECK(cps(lf, 0, 1) == U"\u0301");
        // CR leaves the cursor where it is, as libansilove does, but ends
        // the cluster as a terminal's CR would
        const AnsiDocument cr = utf8("ab\r\xCC\x81");
        CHECK(cps(cr, 1, 0) == U"b");
        CHECK(cps(cr, 2, 0) == U"\u0301");
    }
    SECTION("after a C1 control, U+2028 or U+2029: a cell of its own") {
        for (const std::string sep : {"\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9"}) {
            CAPTURE(sep);
            const AnsiDocument d = utf8("a" + sep + "\xCC\x81");
            CHECK(cps(d, 0, 0) == U"a");
            CHECK(cps(d, 2, 0) == U"\u0301");
        }
    }
    SECTION("after SGR: it joins") {
        const AnsiDocument d = utf8("a\x1b[31m\xCC\x81\x1b[0;1m\xCC\x82" "b");
        CHECK(cps(d, 0, 0) == U"a\u0301\u0302");
        CHECK(cps(d, 1, 0) == U"b");
    }
    SECTION("a ZWJ joins like a mark") {
        const AnsiDocument d = utf8("e\xCC\x81\xE2\x80\x8D");
        CHECK(cps(d, 0, 0) == U"e\u0301\u200D");
    }
}

TEST_CASE("a cluster keeps 16 code points and reports the rest", "[utf8][wide-read]") {
    std::string bytes = "    a";
    for (int i = 0; i < 18; ++i)
        bytes += "\xCC\x81";  // U+0301
    const AnsiDocument d = utf8(bytes + "b");
    const std::u32string kept = cps(d, 4, 0);
    REQUIRE(kept.size() == kMaxClusterCodePoints);
    CHECK(kept == U"a" + std::u32string(kMaxClusterCodePoints - 1, U'\u0301'));
    CHECK(cps(d, 5, 0) == U"b");
    CHECK(d.readLoss.marksDropped == 3);
    CHECK(d.readLoss.markClusters == 1);
    CHECK(d.readLoss.markX == 4);
    CHECK(d.readLoss.markY == 0);
    CHECK(bwcli::readWarnings(d.readLoss) == std::vector<std::string>{"3 combining code points past 16 dropped at 4,0"});
    SECTION("in two clusters: counted together, the first named") {
        const AnsiDocument two = utf8(bytes + "\r\n x" + bytes.substr(5, 2 * 17));
        CHECK(two.readLoss.marksDropped == 5);
        CHECK(two.readLoss.markClusters == 2);
        CHECK(bwcli::readWarnings(two.readLoss) ==
              std::vector<std::string>{"5 combining code points past 16 dropped in 2 clusters, the first at 4,0"});
    }
}

TEST_CASE("past the pool's cap a cluster keeps its base and is reported", "[utf8][wide-read]") {
    // clusters no other test makes: a Coptic letter and two marks
    auto cluster = [](int i) {
        std::string s;
        const char32_t base = 0x2C80 + char32_t(i);
        s += char(0xE0 | (base >> 12));
        s += char(0x80 | ((base >> 6) & 0x3F));
        s += char(0x80 | (base & 0x3F));
        return s + "\xCD\x90\xCD\x91";  // U+0350 U+0351
    };
    std::string bytes;
    for (int i = 0; i < 5; ++i)
        bytes += cluster(i);
    bytes += cluster(0);  // already in the pool: never refused
    const std::size_t cap = clusterPool().size() + 2;
    const AnsiDocument d = utf8(bytes, 80, cap);
    CHECK(cps(d, 0, 0) == U"\u2C80\u0350\u0351");
    CHECK(cps(d, 1, 0) == U"\u2C81\u0350\u0351");
    for (int x = 2; x < 5; ++x)
        CHECK(cps(d, x, 0) == std::u32string(1, char32_t(0x2C80 + x)));
    CHECK(cps(d, 5, 0) == U"\u2C80\u0350\u0351");
    CHECK(d.readLoss.poolRefused == 3);
    CHECK(d.readLoss.marksDropped == 0);
    CHECK(bwcli::readWarnings(d.readLoss) ==
          std::vector<std::string>{"3 clusters read as their base: too many distinct clusters"});
    ReadLoss one;
    one.poolRefused = 1;
    CHECK(bwcli::readWarnings(one) == std::vector<std::string>{"1 cluster read as its base: too many distinct clusters"});
}

TEST_CASE("a UTF-8 .ans with pairs and clusters round-trips byte for byte", "[utf8][wide-read]") {
    // pairs, clusters on narrow and wide bases, a wide glyph at the last
    // column's wrap, colours on all of them
    const std::string art = "\x1b[1;33;44m\xE4\xB8\xAD" "e\xCC\x81\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
                            "\x1b[0;31m\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x96\x91\xE2\x96\x88xyzvw\xE4\xB8\xAD\r\n"
                            "\x1b[0;32m\xCC\x81\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 a\xCC\x81\xCC\x82\xCC\x83\r\n";
    const AnsiDocument first = utf8(art, 16);
    const AnsiOutput out = writeAnsi(first, TextEncoding::Utf8);
    REQUIRE(out.unwritable.empty());
    const AnsiDocument second = parseAnsi(out.bytes);
    CHECK(second.encoding == TextEncoding::Utf8);
    CHECK(second.canvas == first.canvas);
    CHECK_FALSE(second.readLoss.lossy());
    CHECK(writeAnsi(second, TextEncoding::Utf8).bytes == out.bytes);
    // every glyph's bytes are written as they were read
    for (const std::string_view g : {"\xE4\xB8\xAD", "e\xCC\x81", "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9",
                                     "\xE2\x9D\xA4\xEF\xB8\x8F", "a\xCC\x81\xCC\x82\xCC\x83"})
        CHECK(has(out.bytes, g));
    SECTION("except the marks past 16") {
        std::string many = "a";
        for (int i = 0; i < 20; ++i)
            many += "\xCC\x81";
        const AnsiDocument d = utf8(many);
        const AnsiOutput lossy = writeAnsi(d, TextEncoding::Utf8);
        std::string kept = "a";
        for (int i = 0; i < 15; ++i)
            kept += "\xCC\x81";
        CHECK(has(lossy.bytes, kept + "\x1b"));
        CHECK_FALSE(has(lossy.bytes, kept + "\xCC\x81"));
        CHECK(parseAnsi(lossy.bytes).canvas == d.canvas);
    }
}

namespace {

// The cells of row `y` as their code points, a continuation as "|".
std::vector<std::u32string> rowCells(const AnsiDocument& d, int y) {
    std::vector<std::u32string> out;
    for (int x = 0; x < d.canvas.width(); ++x)
        out.push_back(continuation(d, x, y) ? U"|" : cps(d, x, y));
    return out;
}

} // namespace

TEST_CASE("a UTF-8 .ans writes a lone mark after a cell as '?', so the row keeps its columns",
          "[utf8][wide-read]") {
    // review of #348, M1: █ b, CUB, CUF, U+0301, c. The cursor move gives the
    // mark a cell of its own at 2,0; written straight after the b, it would
    // join it and the c would move from column 3 to 2
    const AnsiDocument d = utf8("\xe2\x96\x88" "b\x1b[D\x1b[C\xcc\x81" "c\r\n", 8);
    REQUIRE(cps(d, 2, 0) == U"́");
    REQUIRE(cps(d, 3, 0) == U"c");
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    REQUIRE(out.unwritable.size() == 1);
    CHECK(out.unwritable[0].x == 2);
    CHECK(out.unwritable[0].y == 0);
    CHECK(out.unwritable[0].ch == d.canvas.at(2, 0).ch);
    CHECK(out.unwritable[0].loss == Loss::LoneMark);
    CHECK(out.unwritable[0].written == U'?');
    CHECK_FALSE(out.unwritable[0].lead);
    const AnsiDocument back = parseAnsi(out.bytes);
    CHECK(back.canvas.width() == d.canvas.width());
    CHECK(back.canvas.height() == d.canvas.height());
    CHECK(rowCells(back, 0) == std::vector<std::u32string>{U"█", U"b", U"?", U"c", U" ", U" ", U" ", U" "});
    CHECK(bwcli::unwritableWarnings(out) ==
          std::vector<std::string>{"1 cell holds a zero-width glyph a terminal would draw on the cell before it and "
                                   "is written as '?', so the row keeps its columns (first: U+0301 at column 3, "
                                   "row 1)"});
    // a CP437 .ans has no code for it either way
    CHECK(writeAnsi(d).unwritable.at(0).loss == Loss::NoCode);
}

TEST_CASE("a lone mark after a C1 control is written as '?' as well as the control", "[utf8][wide-read]") {
    // review of #348, M1: the C1 is written '?', which the mark would join
    const AnsiDocument d = utf8("\xe2\x96\x88" "a\xc2\x85\xcc\x81" "b\r\n", 6);
    REQUIRE(cps(d, 2, 0) == U"\u0085");
    REQUIRE(cps(d, 3, 0) == U"́");
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    REQUIRE(out.unwritable.size() == 2);
    CHECK(out.unwritable[0].loss == Loss::Control);
    CHECK(out.unwritable[0].x == 2);
    CHECK(out.unwritable[1].loss == Loss::LoneMark);
    CHECK(out.unwritable[1].x == 3);
    CHECK(rowCells(parseAnsi(out.bytes), 0) == std::vector<std::u32string>{U"█", U"a", U"?", U"?", U"b", U" "});
}

TEST_CASE("a lone mark at the start of a row after a line break is written as it is", "[utf8][wide-read]") {
    // At column 0 after the start of the file or a line break the reader
    // gives it a cell of its own again, so it is no loss and the bytes come
    // back the same. A terminal draws it on nothing (lint's lone-mark says
    // so), so the row's columns hold there too.
    const AnsiDocument d = utf8("\xcc\x81" "a\r\n\xcc\x81\xcc\x82" "b\xe2\x96\x88\r\n", 4);
    REQUIRE(cps(d, 0, 0) == U"́");
    REQUIRE(cps(d, 0, 1) == U"́̂");
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    CHECK(out.unwritable.empty());
    const AnsiDocument back = parseAnsi(out.bytes);
    CHECK(back.canvas == d.canvas);
    CHECK(writeAnsi(back, TextEncoding::Utf8).bytes == out.bytes);
    SECTION("but not after a row written full width, whose wrap is still pending") {
        // the row before ends in its last column with no line break, so the
        // reader would join the mark to that row's last cell
        AnsiDocument full;
        full.canvas = Canvas(3, 2);
        full.canvas.at(0, 0).ch = U'a';
        full.canvas.at(1, 0).ch = U'b';
        full.canvas.at(2, 0).ch = U'c';
        full.canvas.at(0, 1).ch = U'́';
        full.canvas.at(1, 1).ch = U'd';
        const AnsiOutput lossy = writeAnsi(full, TextEncoding::Utf8);
        REQUIRE(lossy.unwritable.size() == 1);
        CHECK(lossy.unwritable[0].loss == Loss::LoneMark);
        CHECK(lossy.unwritable[0].x == 0);
        CHECK(lossy.unwritable[0].y == 1);
        const AnsiDocument r = parseAnsi(lossy.bytes);
        CHECK(rowCells(r, 0) == std::vector<std::u32string>{U"a", U"b", U"c"});
        CHECK(rowCells(r, 1) == std::vector<std::u32string>{U"?", U"d", U" "});
        // as it was: the mark joins the c, and the d moves to column 0
        const std::string joined = "\x1b[0m\x1b[0;37;40mabc\x1b[0m\x1b[0;37;40m\xcc\x81" "d\x1b[0m\r\n";
        const AnsiDocument j = parseAnsi(joined, {.defaultWidth = 3, .encoding = TextEncoding::Utf8});
        CHECK(cps(j, 2, 0) == U"ć");
        CHECK(cps(j, 0, 1) == U"d");
    }
}

TEST_CASE("a UTF-8 .ans writes a lone wide cell as '?', so the row neither grows nor wraps", "[utf8][wide-read]") {
    // review of #348, M2: a wide glyph in one cell with no continuation (a
    // project saved before slice 6, or typed until slice 8) would read back
    // as a pair, push the row right and wrap its last cell
    AnsiDocument d;
    d.canvas = Canvas(4, 1);
    const char32_t row[] = {U'a', U'中', U'b', U'c'};
    for (int x = 0; x < 4; ++x)
        d.canvas.at(x, 0).ch = row[x];
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    REQUIRE(out.unwritable.size() == 1);
    CHECK(out.unwritable[0].x == 1);
    CHECK(out.unwritable[0].ch == Glyph(U'中'));
    CHECK(out.unwritable[0].loss == Loss::LoneWide);
    CHECK(out.unwritable[0].written == U'?');
    CHECK_FALSE(out.unwritable[0].lead);
    const AnsiDocument back = parseAnsi(out.bytes);
    CHECK(back.canvas.width() == 4);
    CHECK(back.canvas.height() == 1);
    CHECK(rowCells(back, 0) == std::vector<std::u32string>{U"a", U"?", U"b", U"c"});
    CHECK(bwcli::unwritableWarnings(out) ==
          std::vector<std::string>{"1 cell holds a glyph two columns wide with no continuation after it and is "
                                   "written as '?', so the row keeps its columns (first: U+4E2D '中' at column 2, "
                                   "row 1)"});
    // the same glyph as a pair is written whole
    d.canvas.at(2, 0).ch = Glyph::continuation();
    CHECK(writeAnsi(d, TextEncoding::Utf8).unwritable.empty());
}

TEST_CASE("saving lone wide cells and lone marks as UTF-8 then reopening keeps every row's columns",
          "[utf8][wide-read]") {
    // lone wide cells mid-row, in the last column and as a cluster's base,
    // lone marks mid-row and after a pair, next to pairs and clusters the
    // writer keeps; nothing grows, wraps or moves
    AnsiDocument d;
    d.canvas = Canvas(8, 3);
    const auto put = [&](int x, int y, Glyph g) { d.canvas.at(x, y).ch = g; };
    const char32_t heartCps[] = {U'❤', U'️'}, thumbCps[] = {U'\U0001F44D', U'\U0001F3FD'},
                   marksCps[] = {U'́', U'̂'};
    const Glyph heart = *clusterPool().intern(heartCps, PoolCap::Uncapped);
    const Glyph thumb = *clusterPool().intern(thumbCps, PoolCap::Uncapped);
    const Glyph marks = *clusterPool().intern(marksCps, PoolCap::Uncapped);
    // row 0: a 中 b 😀 c d e 한   (lone wide at 1, 3 and the last column)
    const char32_t row0[] = {U'a', U'中', U'b', U'\U0001F600', U'c', U'd', U'e', U'한'};
    for (int x = 0; x < 8; ++x)
        put(x, 0, row0[x]);
    // row 1: 中 | U+0301 x 👍🏽(lone) ❤️ U+0301+U+0302, then a blank the trim drops
    put(0, 1, U'中');
    put(1, 1, Glyph::continuation());
    put(2, 1, U'́');
    put(3, 1, U'x');
    put(4, 1, thumb);
    put(5, 1, heart);
    put(6, 1, marks);
    // row 2: U+0301 at column 0 after a line break, then a pair and a lone wide
    put(0, 2, U'́');
    put(1, 2, U'ｗ');
    put(2, 2, Glyph::continuation());
    put(3, 2, U'ｗ');
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    std::vector<std::pair<int, int>> lost;
    for (const Unwritable& u : out.unwritable) {
        CHECK((u.loss == Loss::LoneWide || u.loss == Loss::LoneMark));
        lost.emplace_back(u.x, u.y);
    }
    CHECK(lost == std::vector<std::pair<int, int>>{{1, 0}, {3, 0}, {7, 0}, {2, 1}, {4, 1}, {6, 1}, {3, 2}});
    const AnsiDocument back = parseAnsi(out.bytes);
    REQUIRE(back.encoding == TextEncoding::Utf8);
    CHECK(back.canvas.width() == 8);
    CHECK(back.canvas.height() == 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 8; ++x) {
            INFO(x << "," << y);
            const bool wasLost = std::find(lost.begin(), lost.end(), std::pair{x, y}) != lost.end();
            CHECK(back.canvas.at(x, y).ch == (wasLost ? Glyph(U'?') : d.canvas.at(x, y).ch));
        }
    // and a second save loses nothing more
    CHECK(writeAnsi(back, TextEncoding::Utf8).unwritable.empty());
}

TEST_CASE("CP437, XBIN and BIN don't make pairs or clusters", "[utf8][wide-read]") {
    // the bytes of 中 and U+0301 read as CP437 are four glyphs of their own
    ParseOptions o;
    o.encoding = TextEncoding::Cp437;
    const AnsiDocument d = parseAnsi("\xE4\xB8\xAD\xCC\x81", o);
    for (int x = 0; x < 5; ++x)
        CHECK(d.canvas.at(x, 0).ch.isCodePoint());
    CHECK(cps(d, 0, 0) == U"Σ");
    CHECK(cps(d, 3, 0) == U"╠");
    CHECK_FALSE(d.readLoss.lossy());
}

TEST_CASE("lint: width-varies names a glyph clients may draw at another width", "[lint][wide-read]") {
    SECTION("wide only since a later Unicode version") {
        // U+1F970 (Unicode 11) is wide; U+1F600 has been since 9.0.0
        const AnsiDocument d = utf8("\xF0\x9F\xA5\xB0\xF0\x9F\x98\x80");
        CHECK(widthVaries(U'\U0001F970'));
        CHECK_FALSE(widthVaries(U'\U0001F600'));
        CHECK(std::string(widthVariesFloor()) == "9.0.0");
        const auto f = found(d.canvas, Kind::WidthVaries);
        REQUIRE(f.size() == 1);
        CHECK(f[0].x == 0);
        CHECK(f[0].message == "U+1F970 🥰: it takes two columns here, but only since a Unicode version after 9.0.0: "
                              "a client on an older table may draw it in one");
        CHECK(lint::kindName(Kind::WidthVaries) == "width-varies");
    }
    SECTION("ZWJ, VS16 on a narrow base, and a skin tone") {
        const AnsiDocument d = utf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"  // 👨 ZWJ 👩
                                    "\xE2\x9D\xA4\xEF\xB8\x8F"                          // ❤ VS16
                                    "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD");                // 👍 🏽
        const auto f = found(d.canvas, Kind::WidthVaries);
        std::vector<int> xs;
        for (const auto& one : f)
            xs.push_back(one.x);
        // the modifier is wide, so not joined: a pair of its own after the 👍
        CHECK(xs == std::vector<int>{0, 4, 7});
        REQUIRE(f.size() == 3);
        CHECK(has(f[0].message, "U+200D joins it to the glyph after it in a terminal that joins graphemes"));
        CHECK(f[1].message == "U+2764 U+FE0F ❤ U+FE0F: U+FE0F makes it two columns wide in a terminal that joins "
                              "graphemes, one more than its cell");
        CHECK(has(f[2].message, "draws a skin tone modifier inside the emoji before it, narrower than their cells"));
    }
    SECTION("a flag: two regional indicators, one finding over both cells") {
        const AnsiDocument d = utf8("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
        const auto f = found(d.canvas, Kind::WidthVaries);
        REQUIRE(f.size() == 1);
        CHECK(f[0].x == 0);
        CHECK(f[0].x2 > f[0].x);
        CHECK(has(f[0].message, "make a flag: a terminal that joins graphemes draws one glyph over both cells"));
    }
    SECTION("CP437 glyphs and a plain accent are found by none of the new rules") {
        const AnsiDocument d = utf8("\xE2\x96\x91\xE2\x96\x88" "e\xCC\x81\xE4\xB8\xAD");
        for (const Kind k : {Kind::WidthVaries, Kind::LoneMark, Kind::BidiControl})
            CHECK(found(d.canvas, k).empty());
    }
}

TEST_CASE("lint: lone-mark finds a mark with no base and a wide glyph with no continuation", "[lint][wide-read]") {
    const AnsiDocument d = utf8("\xCC\x81" "a");
    auto f = found(d.canvas, Kind::LoneMark);
    REQUIRE(f.size() == 1);
    CHECK(f[0].x == 0);
    CHECK(f[0].message ==
          "U+0301 is zero-width in a cell of its own: a terminal draws it on the cell before it, or not at all");
    CHECK(lint::kindName(Kind::LoneMark) == "lone-mark");
    Canvas c(4, 1);
    c.at(1, 0).ch = Glyph(U'中');
    f = found(c, Kind::LoneMark);
    REQUIRE(f.size() == 1);
    CHECK(f[0].x == 1);
    CHECK(f[0].message ==
          "U+4E2D 中 is two columns wide with no continuation after it: a terminal draws it over the next cell too");
    c.at(2, 0).ch = Glyph::continuation();
    CHECK(found(c, Kind::LoneMark).empty());
}

TEST_CASE("lint: bidi-control finds a bidi control alone or in a cluster", "[lint][wide-read]") {
    const AnsiDocument d = utf8("\xE2\x80\xAE" "J\xE2\x80\xAE");
    const auto f = found(d.canvas, Kind::BidiControl);
    REQUIRE(f.size() == 2);
    CHECK(f[0].x == 0);
    CHECK(f[0].message == "U+202E is a bidi control: a UTF-8 .ans or IRC text writes it as it is, and a client that "
                          "applies bidi (HexChat, mIRC) reorders the rest of the line");
    CHECK(f[1].x == 1);
    CHECK(has(f[1].message, "holds the bidi control U+202E: a UTF-8 .ans or IRC text writes it as it is"));
    CHECK(lint::kindName(Kind::BidiControl) == "bidi-control");
    for (const auto& one : f)
        CHECK_FALSE(has(one.message, "\xE2\x80\xAE"));
}
