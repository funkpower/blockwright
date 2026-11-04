// Pairs in the CP437 writers, the IRC encoder and a layer's cells (PLAN §16).
// The CP437 .ans, XBIN and BIN write a lead as '?' and a continuation as a
// space in its own colours, so the columns hold (test_wide_writers.cpp has
// the rest of slice 3's writers). IRC still writes a pair as '?' and a space
// and a cluster as '?' (slice 1's interim rule, until slice 4), and a .cells
// layer can't hold either yet. Nothing in the editor makes a pair or a
// cluster yet; these tests make them by hand.

#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/glyph.h"
#include "blockwright/irc.h"
#include "blockwright/project.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <vector>

using namespace bw;

namespace {

Cell cell(Glyph ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

AnsiDocument row(const std::vector<Cell>& cells) {
    AnsiDocument doc;
    doc.canvas = Canvas(int(cells.size()), 1);
    for (std::size_t x = 0; x < cells.size(); ++x)
        doc.canvas.at(int(x), 0) = cells[x];
    return doc;
}

Glyph cluster(std::vector<char32_t> cps) {
    const std::optional<Glyph> g = clusterPool().intern(cps, PoolCap::Capped);
    REQUIRE(g);
    REQUIRE(g->isCluster());
    return *g;
}

// The four writers' bytes for `doc`.
struct Written {
    AnsiOutput cp437, utf8, xbin, bin;
};
Written writeAll(const AnsiDocument& doc) {
    return {writeAnsi(doc, TextEncoding::Cp437), writeAnsi(doc, TextEncoding::Utf8), writeXbin(doc), writeBin(doc)};
}

std::vector<int> listedColumns(const AnsiOutput& out) {
    std::vector<int> xs;
    for (const Unwritable& u : out.unwritable)
        xs.push_back(u.x);
    return xs;
}

const Glyph kCont = Glyph::continuation();

} // namespace

TEST_CASE("a CP437 writer writes a pair as '?' and a space in their colours, and lists the lead") {
    // 中 on blue over two cells, then A; the continuation in other colours
    const AnsiDocument pair = row({cell(U'中', 15, 1), cell(kCont, 14, 4), cell(U'A', 7, 0)});
    const AnsiDocument plain = row({cell(U'?', 15, 1), cell(U' ', 14, 4), cell(U'A', 7, 0)});
    const Written got = writeAll(pair), want = writeAll(plain);
    // (a UTF-8 .ans writes the pair: test_wide_writers.cpp)
    for (const auto& [name, g, w] : {std::tuple{"cp437 .ans", &got.cp437, &want.cp437},
                                     std::tuple{"xbin", &got.xbin, &want.xbin}, std::tuple{"bin", &got.bin, &want.bin}}) {
        INFO(name);
        CHECK(g->bytes == w->bytes);
        CHECK(w->unwritable.empty());
        REQUIRE(g->unwritable.size() == 1);
        CHECK(g->unwritable[0].x == 0);
        CHECK(g->unwritable[0].y == 0);
        CHECK(g->unwritable[0].ch == U'中');
        CHECK(g->unwritable[0].loss == Loss::Wide);
    }
}

TEST_CASE("a lead is a lead whatever it holds, and a continuation whatever is before it") {
    // a narrow glyph before a continuation, a continuation at the row's
    // start and two in a row: none of them is made by an edit, all are
    // written by the same rule
    const AnsiDocument doc = row({cell(kCont, 7, 0), cell(U'A', 7, 0), cell(kCont, 7, 0), cell(kCont, 7, 0)});
    const AnsiDocument plain = row({cell(U' ', 7, 0), cell(U'?', 7, 0), cell(U' ', 7, 0), cell(U' ', 7, 0)});
    const Written got = writeAll(doc), want = writeAll(plain);
    CHECK(got.cp437.bytes == want.cp437.bytes);
    CHECK(got.utf8.bytes == want.utf8.bytes);
    CHECK(got.xbin.bytes == want.xbin.bytes);
    CHECK(got.bin.bytes == want.bin.bytes);
    CHECK(listedColumns(got.cp437) == std::vector{1});
    CHECK(listedColumns(got.utf8) == std::vector{1});
    CHECK(listedColumns(got.bin) == std::vector{1});
}

TEST_CASE("a wide glyph alone in a cell is written as it was") {
    // what every project saved before §16 holds: an ordinary one-cell glyph
    const AnsiDocument doc = row({cell(U'中', 7, 0), cell(U'A', 7, 0)});
    const AnsiOutput utf8 = writeAnsi(doc, TextEncoding::Utf8);
    CHECK_THAT(utf8.bytes, Catch::Matchers::ContainsSubstring("\xE4\xB8\xAD"
                                                              "A"));
    CHECK(utf8.unwritable.empty());
    const AnsiOutput cp437 = writeAnsi(doc, TextEncoding::Cp437);
    CHECK(listedColumns(cp437) == std::vector{0});
}

TEST_CASE("a transparent cell is written blank, never as a lead") {
    AnsiDocument doc = row({cell(U'中', 7, 0), cell(kCont, 7, 0)});
    doc.canvas.at(0, 0).transparent = true;
    const AnsiOutput utf8 = writeAnsi(doc, TextEncoding::Utf8);
    AnsiDocument plain = row({cell(U'中', 7, 0), cell(U' ', 7, 0)});
    plain.canvas.at(0, 0).transparent = true;
    CHECK(utf8.bytes == writeAnsi(plain, TextEncoding::Utf8).bytes);
}

TEST_CASE("IRC text writes a pair as '?' and a space and a cluster as '?'") {
    const Glyph e = cluster({U'e', 0x301});
    const std::vector<Cell> got{cell(U'中', 15, 1), cell(kCont, 14, 4), cell(e, 7, 0), cell(U'A', 7, 0)};
    const std::vector<Cell> want{cell(U'?', 15, 1), cell(U' ', 14, 4), cell(U'?', 7, 0), cell(U'A', 7, 0)};
    CHECK(irc::encodeRow(got) == irc::encodeRow(want));
    // a lone wide glyph is itself, as before
    const std::string lone = irc::encodeRow(std::vector{cell(U'中', 7, 0)});
    CHECK_THAT(lone, Catch::Matchers::ContainsSubstring("\xE4\xB8\xAD"));
    CHECK(lone.find('?') == std::string::npos);
    CHECK_THAT(irc::encodeRow(std::vector{cell(U'中', 7, 0), cell(U'A', 7, 0)}),
               Catch::Matchers::ContainsSubstring("\xE4\xB8\xAD"));
}

TEST_CASE("a layer's cells can't hold a continuation or a cluster yet") {
    using project::ProjectError;
    Canvas cv(3, 1);
    cv.at(0, 0).ch = U'中';
    CHECK_NOTHROW(project::encodeCells(cv));
    cv.at(1, 0).ch = kCont;
    CHECK_THROWS_AS(project::encodeCells(cv), ProjectError);
    CHECK_THROWS_WITH(project::encodeCells(cv), Catch::Matchers::ContainsSubstring("cell 1,0"));
    cv.at(1, 0).ch = U' ';
    cv.at(2, 0).ch = cluster({U'e', 0x301});
    CHECK_THROWS_AS(project::encodeCells(cv), ProjectError);
    CHECK_THROWS_WITH(project::encodeCells(cv), Catch::Matchers::ContainsSubstring("cell 2,0"));
}
