// The .ans, XBIN and BIN writers on pairs and clusters (PLAN §16, Writing;
// slice 3). UTF-8 .ans writes a lead's code points and its continuation
// nothing; CP437 .ans, XBIN and BIN write a lead as '?' and its
// continuation as a space, compose a cluster to one CP437 byte where NFC
// has one, else write its base and list the marks dropped. Every writer
// has a case for a pair, a lone wide cell, a cluster that composes, one
// whose base has a byte, one whose base hasn't and a control. Nothing in
// the editor makes a pair or a cluster yet; these tests make them by hand.

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/edit.h"
#include "blockwright/glyph.h"
#include "blockwright/library.h"
#include "blockwright/lint_rules.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QString>

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace bw;

namespace {

const Glyph kCont = Glyph::continuation();

Cell cell(Glyph ch, std::uint8_t fg = 7, std::uint8_t bg = 0) {
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

// An .ans's content: what comes before SAUCE's 0x1A, which no .ans content
// holds (SUB is a control code it can't write).
std::string content(const AnsiOutput& out) { return out.bytes.substr(0, out.bytes.find('\x1a')); }

// The glyph bytes of an uncompressed XBIN's or a BIN's cells, row by row:
// the first of each character/attribute pair after `header` bytes.
std::string cellGlyphs(const AnsiOutput& out, std::size_t header) {
    std::string s;
    const std::string body = out.bytes.substr(header, out.bytes.find("\x1aSAUCE") - header);
    for (std::size_t i = 0; i < body.size(); i += 2)
        s += body[i];
    return s;
}

struct Listed {
    int x;
    Loss loss;
    char32_t written;
    bool operator==(const Listed&) const = default;
};

std::vector<Listed> listed(const AnsiOutput& out) {
    std::vector<Listed> v;
    for (const Unwritable& u : out.unwritable)
        v.push_back({u.x, u.loss, u.written});
    return v;
}

std::vector<lint::Finding> noCp437(const Canvas& cv) {
    std::vector<lint::Finding> found;
    for (const lint::Finding& f : lint::checkRules(cv))
        if (f.kind == lint::Finding::Kind::NoCp437)
            found.push_back(f);
    return found;
}

} // namespace

// ---- UTF-8 .ans ---------------------------------------------------------------

TEST_CASE("UTF-8 .ans: a pair's lead writes its code points and its continuation nothing", "[wide][writers]") {
    // 中 bright white on blue over two cells, then A grey on black: no colour
    // change between the halves, nothing for the continuation
    const AnsiOutput out = writeAnsi(row({cell(U'中', 15, 1), cell(kCont, 15, 1), cell(U'A')}), TextEncoding::Utf8);
    CHECK(content(out) == "\x1b[0m\x1b[0;1;37;44m\xE4\xB8\xAD\x1b[0;37;40mA\x1b[0m");
    CHECK(out.unwritable.empty());
}

TEST_CASE("UTF-8 .ans: a cluster is written as its code points, byte for byte", "[wide][writers]") {
    const Glyph e = cluster({U'e', 0x301});
    const Glyph heart = cluster({U'♥', 0xFE0F});
    const Glyph wide = cluster({U'中', 0x301});
    const AnsiOutput out = writeAnsi(row({cell(e), cell(heart), cell(wide), cell(kCont), cell(U'x')}), TextEncoding::Utf8);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40me\xCC\x81\xE2\x99\xA5\xEF\xB8\x8F\xE4\xB8\xAD\xCC\x81x\x1b[0m");
    CHECK(out.unwritable.empty());
}

TEST_CASE("UTF-8 .ans: a lone wide cell is written as it was", "[wide][writers]") {
    const AnsiOutput out = writeAnsi(row({cell(U'中'), cell(U'A')}), TextEncoding::Utf8);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40m\xE4\xB8\xAD"
                          "A\x1b[0m");
    CHECK(out.unwritable.empty());
}

TEST_CASE("UTF-8 .ans: a pair at the row's end fills it, so no line break follows", "[wide][writers]") {
    // the continuation is black, as a trailing blank is, but the lead's
    // glyph takes its column: the row is full and wraps on its own
    const AnsiOutput out = writeAnsi(row({cell(U'A'), cell(U'中'), cell(kCont)}), TextEncoding::Utf8);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40mA\xE4\xB8\xAD\x1b[0m");
}

TEST_CASE("UTF-8 .ans: a control is written as '?' and listed, a control lead as '?' and a space", "[wide][writers]") {
    const AnsiOutput out =
        writeAnsi(row({cell(char32_t(0x85)), cell(U'A'), cell(char32_t(0x9B)), cell(kCont), cell(U'B')}),
                  TextEncoding::Utf8);
    // ASCII alone: the byte order mark says UTF-8
    CHECK(content(out) == "\xEF\xBB\xBF\x1b[0m\x1b[0;37;40m?A? B\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::Control, U'?'}, {2, Loss::Control, U'?'}});
}

TEST_CASE("UTF-8 .ans: a lead a terminal doesn't draw two columns wide is '?' and a space", "[wide][writers]") {
    // no edit makes one (a lead is a lead whatever it holds), but its
    // glyph alone would put B a column left of where the canvas has it
    const Glyph mark = cluster({0x301, 0x302});
    const AnsiOutput out =
        writeAnsi(row({cell(U'A'), cell(kCont), cell(mark), cell(kCont), cell(U'B')}), TextEncoding::Utf8);
    CHECK(content(out) == "\xEF\xBB\xBF\x1b[0m\x1b[0;37;40m? ? B\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::Wide, U'?'}, {2, Loss::Wide, U'?'}});
}

// ---- CP437 .ans ----------------------------------------------------------------

TEST_CASE("CP437 .ans: a pair is '?' and a space in its colours, the lead listed as wide", "[wide][writers]") {
    const AnsiOutput out = writeAnsi(row({cell(U'中', 15, 1), cell(kCont, 15, 1), cell(U'A')}), TextEncoding::Cp437);
    CHECK(content(out) == "\x1b[0m\x1b[0;1;37;44m? \x1b[0;37;40mA\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::Wide, U'?'}});
    CHECK(out.unwritable[0].ch == U'中');
}

TEST_CASE("CP437 .ans: a lone wide cell has no code", "[wide][writers]") {
    const AnsiOutput out = writeAnsi(row({cell(U'中'), cell(U'A')}), TextEncoding::Cp437);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40m?A\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::NoCode, U'?'}});
}

TEST_CASE("CP437 .ans: a cluster whose NFC form is a CP437 glyph is that byte, and no loss", "[wide][writers]") {
    // e + U+0301 is é (0x82); E + U+0341, a mark NFC reads as U+0301, is É (0x90)
    const AnsiOutput out =
        writeAnsi(row({cell(cluster({U'e', 0x301})), cell(cluster({U'E', 0x341})), cell(U'x')}), TextEncoding::Cp437);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40m\x82\x90x\x1b[0m");
    CHECK(out.unwritable.empty());
}

TEST_CASE("CP437 .ans: a cluster whose base has a byte is the base, listed with its marks", "[wide][writers]") {
    // ♥ U+FE0F: ♥ is 0x03; x U+0301 has no precomposed form
    const Glyph heart = cluster({U'♥', 0xFE0F});
    const AnsiOutput out = writeAnsi(row({cell(heart), cell(cluster({U'x', 0x301})), cell(U'y')}), TextEncoding::Cp437);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40m\x03xy\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::MarksDropped, U'♥'}, {1, Loss::MarksDropped, U'x'}});
    CHECK(out.unwritable[0].ch == heart);
}

TEST_CASE("CP437 .ans: a cluster whose base has no byte is '?'", "[wide][writers]") {
    const AnsiOutput out = writeAnsi(row({cell(cluster({U'★', 0x301})), cell(U'y')}), TextEncoding::Cp437);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40m?y\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::NoCode, U'?'}});
}

TEST_CASE("CP437 .ans: a glyph or a cluster base on a control byte is '?', listed as a control", "[wide][writers]") {
    // ○ is 0x09, TAB to the parser; a cluster on it can't keep its base either
    const AnsiOutput out =
        writeAnsi(row({cell(U'○'), cell(cluster({U'○', 0x301})), cell(U'y')}), TextEncoding::Cp437);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40m??y\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::Control, U'?'}, {1, Loss::Control, U'?'}});
    CHECK(ansControlCount(out.unwritable) == 2);
    CHECK(ansControlNames(out.unwritable) == "TAB");
}

TEST_CASE("CP437 .ans: a wide cluster as a lead is '?' and a space, listed as wide", "[wide][writers]") {
    // wide comes first: a composition or a base would lose a column
    const AnsiOutput out =
        writeAnsi(row({cell(cluster({U'中', 0x301})), cell(kCont), cell(cluster({U'e', 0x301})), cell(kCont),
                       cell(U'y')}),
                  TextEncoding::Cp437);
    CHECK(content(out) == "\x1b[0m\x1b[0;37;40m? ? y\x1b[0m");
    CHECK(listed(out) == std::vector<Listed>{{0, Loss::Wide, U'?'}, {2, Loss::Wide, U'?'}});
}

TEST_CASE("CP437 .ans: columns hold across pairs and clusters", "[wide][writers]") {
    const Glyph e = cluster({U'e', 0x301}), heart = cluster({U'♥', 0xFE0F}), star = cluster({U'★', 0x301});
    const AnsiDocument doc = row({cell(U'中'), cell(kCont), cell(e), cell(heart), cell(star), cell(U'○'),
                                  cell(cluster({U'日', 0x301})), cell(kCont), cell(U'中'), cell(U'Z')});
    const AnsiOutput out = writeAnsi(doc, TextEncoding::Cp437);
    const Canvas back = parseAnsi(out.bytes, {.encoding = TextEncoding::Cp437}).canvas;
    REQUIRE(back.width() == 10);
    CHECK(back.at(9, 0).ch == U'Z');
    CHECK(back.at(2, 0).ch == U'é');
    CHECK(back.at(3, 0).ch == U'♥');
    std::vector<int> xs;
    for (const Unwritable& u : out.unwritable)
        xs.push_back(u.x);
    CHECK(xs == std::vector{0, 3, 4, 5, 6, 8});
}

// ---- XBIN and BIN --------------------------------------------------------------

TEST_CASE("XBIN and BIN: pairs and clusters as CP437 .ans writes them, control bytes kept", "[wide][writers]") {
    const Glyph heart = cluster({U'♥', 0xFE0F}), ring = cluster({U'○', 0x301});
    const AnsiDocument doc = row({cell(U'中', 15, 1), cell(kCont, 15, 1), cell(U'中'), cell(cluster({U'e', 0x301})),
                                  cell(heart), cell(cluster({U'★', 0x301})), cell(U'○'), cell(ring), cell(U'Z')});
    const std::vector<Listed> want{{0, Loss::Wide, U'?'},
                                   {2, Loss::NoCode, U'?'},
                                   {4, Loss::MarksDropped, U'♥'},
                                   {5, Loss::NoCode, U'?'},
                                   {7, Loss::MarksDropped, U'○'}};
    // an XBIN's header is 11 bytes without a palette or font
    for (const auto& [name, out, header] : {std::tuple{"xbin", writeXbin(doc, {.compress = false}), std::size_t(11)},
                                            std::tuple{"bin", writeBin(doc), std::size_t(0)}}) {
        INFO(name);
        CHECK(cellGlyphs(out, header).substr(0, 9) == std::string("? ?\x82\x03?\x09\x09Z", 9));
        CHECK(listed(out) == want);
    }
    // the continuation's attribute byte is its own: the lead's colours
    const AnsiOutput bin = writeBin(doc);
    CHECK(bin.bytes[1] == bin.bytes[3]);
}

// ---- lint and writesAlike agree with the writers ----------------------------------

TEST_CASE("lint no-cp437 lists what a CP437 .ans lists, and words each kind", "[wide][writers][lint]") {
    // review of #337, finding 1: a lead holding A is written '?' and listed,
    // so lint finds it too
    Canvas cv(12, 1);
    cv.at(0, 0).ch = U'A';
    cv.at(1, 0).ch = kCont;
    cv.at(2, 0).ch = U'B';
    cv.at(3, 0).ch = cluster({U'e', 0x301});
    cv.at(4, 0).ch = cluster({U'♥', 0xFE0F});
    cv.at(5, 0).ch = cluster({U'★', 0x301});
    cv.at(6, 0).ch = U'中';
    cv.at(7, 0).ch = kCont;
    cv.at(8, 0).ch = U'○';
    cv.at(9, 0).ch = cluster({U'○', 0x301});
    AnsiDocument doc;
    doc.canvas = cv;
    std::vector<int> written, linted;
    for (const Unwritable& u : writeAnsi(doc).unwritable)
        written.push_back(u.x);
    const std::vector<lint::Finding> found = noCp437(cv);
    for (const lint::Finding& f : found)
        linted.push_back(f.x);
    CHECK(written == std::vector{0, 4, 5, 6, 8, 9});
    CHECK(linted == written);
    REQUIRE(found.size() == 6);
    CHECK(found[0].message == "U+0041 A takes two cells: an .ans saves it as '?' and a space");
    CHECK(found[1].message == "U+2665 U+FE0F ♥ U+FE0F has no CP437 code: an .ans saves it as ♥, dropping U+FE0F");
    CHECK(found[2].message == "U+2605 U+0301 ★ U+0301 has no CP437 code: an .ans saves it as '?'");
    CHECK(found[3].message == "U+4E2D 中 takes two cells: an .ans saves it as '?' and a space");
    CHECK(found[4].message ==
          "U+25CB ○ is CP437 0x09, which .ans reads as TAB: an .ans saves it as '?' (XBIN and BIN keep it)");
    CHECK(found[5].message == "U+25CB U+0301 ○ U+0301 has a base whose CP437 byte 0x09 .ans reads as TAB: an .ans "
                              "saves it as '?' (XBIN and BIN keep its base)");
}

TEST_CASE("writesAlike judges a lead as the writers write it", "[wide][writers]") {
    // review of #337, finding 1: A before a continuation writes '?', so it
    // isn't A alike; with nothing said of the right neighbour it is A
    const Cell a = cell(U'A');
    CHECK_FALSE(writesAlike(a, kCont, a, U' '));
    CHECK(writesAlike(a, kCont, cell(U'?'), U' '));
    CHECK(writesAlike(a, kCont, cell(U'中'), kCont));
    CHECK(writesAlike(a, a));
    // a cluster that composes writes its byte; one with a base writes the base
    CHECK(writesAlike(cell(cluster({U'e', 0x301})), cell(U'é')));
    CHECK(writesAlike(cell(cluster({U'♥', 0xFE0F})), cell(U'♥')));
    CHECK(writesAlike(cell(cluster({U'★', 0x301})), cell(U'?')));
    CHECK(writesAlike(cell(char32_t(0)), cell(U' ')));
}

TEST_CASE("the lost-blink mark stays on a cell drawn over a lead with what it writes", "[wide][writers]") {
    // A on a lead writes '?'; drawing '?' and a space over the pair keeps
    // the mark, as drawing the same glyph over any cell does
    Cell lead = cell(U'A'), cont = cell(kCont);
    lead.lostBlink = cont.lostBlink = true;
    CellEdit e;
    e.changes.push_back({0, 0, lead, cell(U'?')});
    e.changes.push_back({1, 0, cont, cell(U' ')});
    const CellEdit s = settleMarks(e);
    CHECK(s.changes[0].after.lostBlink);
    CHECK(s.changes[1].after.lostBlink);
    CHECK(writtenOver(lead, cell(U'?'), kCont, U' ').lostBlink);
    CHECK_FALSE(writtenOver(lead, cell(U'A'), kCont, U' ').lostBlink);
}

// ---- stamps ---------------------------------------------------------------------

TEST_CASE("stampLossMessage counts wide glyphs and names marks dropped", "[wide][writers]") {
    AnsiOutput out;
    out.unwritable = {{0, 0, U'中', Loss::Wide}, {2, 0, U'中', Loss::Wide}, {4, 0, U'日', Loss::Wide}};
    CHECK(library::stampLossMessage(out) == "3 wide glyphs written as ?");
    out.unwritable = {{0, 0, U'中', Loss::Wide}, {2, 0, cluster({U'♥', 0xFE0F}), Loss::MarksDropped, U'♥'},
                      {3, 0, U'★'}};
    CHECK(library::stampLossMessage(out) == "1 cell holds a glyph with no CP437 code and 1 cell holds a glyph whose marks "
                                   "CP437 drops, which .ans can't store; 1 wide glyph written as ?");
}

// ---- the composition table ---------------------------------------------------------

namespace {

std::vector<char32_t> ucs4(const QString& s) {
    const QList<uint> v = s.toUcs4();
    return std::vector<char32_t>(v.begin(), v.end());
}

QString text(char32_t c) { return QString::fromUcs4(&c, 1); }

} // namespace

TEST_CASE("the NFC composition table is every two-code-point cluster Qt composes to a CP437 glyph", "[wide][writers]") {
    // Every CP437 glyph decomposes (NFD) to at most two code points, so only
    // a cluster of two can compose to one: X Y with NFD(X) the glyph's base
    // and NFD(Y) its mark. Each code point whose NFD is one code point,
    // grouped by that code point, gives every such X and Y.
    std::map<char32_t, std::vector<char32_t>> byNfd;
    for (char32_t c = 0; c <= 0x10FFFF; ++c) {
        if (c >= 0xD800 && c <= 0xDFFF)
            continue;
        const std::vector<char32_t> d = ucs4(text(c).normalized(QString::NormalizationForm_D));
        if (d.size() == 1)
            byNfd[d[0]].push_back(c);
    }
    std::vector<std::tuple<char32_t, char32_t, std::uint8_t>> want;
    for (int b = 0; b < 256; ++b) {
        const char32_t c = cp437ToUnicode(std::uint8_t(b));
        INFO("CP437 0x" << std::hex << b);
        CHECK(ucs4(text(c).normalized(QString::NormalizationForm_C)) == std::vector{c});
        const std::vector<char32_t> d = ucs4(text(c).normalized(QString::NormalizationForm_D));
        REQUIRE(d.size() <= 2);
        if (d.size() == 2)
            for (const char32_t x : byNfd[d[0]])
                for (const char32_t y : byNfd[d[1]])
                    if (ucs4((text(x) + text(y)).normalized(QString::NormalizationForm_C)) == std::vector{c})
                        want.emplace_back(x, y, std::uint8_t(b));
    }
    std::sort(want.begin(), want.end());
    std::vector<std::tuple<char32_t, char32_t, std::uint8_t>> got;
    for (const Cp437Composition& c : cp437Compositions())
        got.emplace_back(c.base, c.mark, c.byte);
    // on a difference, the table core/src/cp437_compose.inc should hold
    std::string table;
    for (const auto& [x, y, b] : want) {
        char line[48];
        std::snprintf(line, sizeof line, "    {0x%04X, 0x%04X, 0x%02X},\n", unsigned(x), unsigned(y), unsigned(b));
        table += line;
    }
    INFO(table);
    CHECK(got == want);
    for (const auto& [x, y, b] : want)
        CHECK(cp437Composed(cluster({x, y})) == b);
}

TEST_CASE("cp437Composed: only a two-code-point cluster in the table", "[wide][writers]") {
    CHECK(cp437Composed(cluster({U'e', 0x301})) == std::uint8_t(0x82));
    CHECK(cp437Composed(cluster({U'A', 0x30A})) == std::uint8_t(0x8F));
    CHECK_FALSE(cp437Composed(cluster({U'e', 0x301, 0x302})));
    CHECK_FALSE(cp437Composed(cluster({U'x', 0x301})));
    CHECK_FALSE(cp437Composed(U'é'));
    CHECK_FALSE(cp437Composed(kCont));
}
