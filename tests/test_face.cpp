// Faces (PLAN §15): the bundled pixel faces, the blank rule for source
// cells, layout and wrapping, missing characters and confusable screening.

#include "blockwright/face.h"

#include "blockwright/layer.h"
#include "blockwright/pc_fonts.h"
#include "util.h"

#include <QCryptographicHash>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace bw;

namespace {

std::u32string widen(std::string_view s) { return {s.begin(), s.end()}; }

// The §15 fixture words, one a line.
std::vector<std::u32string> fixtureWords() {
    std::istringstream in(bwtest::readFile(bwtest::dataDir() / "text" / "words.txt"));
    std::vector<std::u32string> out;
    for (std::string line; std::getline(in, line);)
        out.push_back(widen(line));
    return out;
}

// A mask as rows of '#' (ink) and '.'.
std::vector<std::string> rowsOf(const InkMask& m) {
    std::vector<std::string> out;
    for (int y = 0; y < m.height; ++y) {
        std::string r;
        for (int x = 0; x < m.width; ++x)
            r += m.at(x, y) ? '#' : '.';
        out.push_back(r);
    }
    return out;
}

InkMask maskFrom(const std::vector<std::string>& rows) {
    InkMask m(static_cast<int>(rows.empty() ? 0 : rows[0].size()), static_cast<int>(rows.size()));
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            m.set(x, y, rows[y][x] == '#');
    return m;
}

// Rows of a byte-a-row 8 px glyph.
std::vector<std::string> bitmapRows(std::initializer_list<int> bytes) {
    std::vector<std::string> out;
    for (int b : bytes) {
        std::string r;
        for (int x = 0; x < 8; ++x)
            r += b & (0x80 >> x) ? '#' : '.';
        out.push_back(r);
    }
    return out;
}

std::vector<std::string> glyphRows(const Face& f, char32_t ch) {
    const auto g = f.glyph(ch);
    REQUIRE(g);
    return rowsOf(g->mask);
}

// A face of hand-made glyphs for layout and screening tests.
class TestFace final : public Face {
public:
    TestFace(int height, int spacing) : height_(height), spacing_(spacing) {}
    std::string_view name() const override { return "test"; }
    int height() const override { return height_; }
    int letterSpacing() const override { return spacing_; }
    std::optional<FaceGlyph> glyph(char32_t ch) const override {
        const auto it = glyphs.find(ch);
        return it == glyphs.end() ? std::nullopt : std::optional<FaceGlyph>(it->second);
    }
    std::vector<ConfusablePair> confusables() const override { return screenConfusables(*this); }

    std::map<char32_t, FaceGlyph> glyphs;

private:
    int height_, spacing_;
};

// A test face whose letters are `width` columns of full ink, 2 px high,
// except the space, which is blank.
TestFace blockyFace(int width, int spacing, std::u32string_view letters) {
    TestFace f(2, spacing);
    for (char32_t ch : letters) {
        InkMask m(width, 2);
        std::fill(m.ink.begin(), m.ink.end(), 1);
        f.glyphs[ch] = FaceGlyph{m, std::nullopt};
    }
    f.glyphs[U' '] = FaceGlyph{InkMask(width, 2), std::nullopt};
    return f;
}

std::vector<std::u32string> lineTexts(const TextLayout& l, std::u32string_view text) {
    std::vector<std::u32string> out;
    for (const auto& line : l.lines)
        out.emplace_back(text.substr(line.first, line.count));
    return out;
}

Cell cell(char32_t ch, std::uint8_t bg, std::uint8_t attrs = 0) {
    return Cell{ch, Color::indexed(Grey), Color::indexed(bg), attrs, false};
}

} // namespace

TEST_CASE("the bundled faces are block, vga and mini, by name") {
    const auto faces = bundledFaces();
    REQUIRE(faces.size() == 3);
    CHECK(faces[0]->name() == "block");
    CHECK(faces[1]->name() == "vga");
    CHECK(faces[2]->name() == "mini");
    CHECK(bundledFace("vga") == &vgaFace());
    CHECK(bundledFace("mini") == &miniFace());
    CHECK(bundledFace("block") == &blockFace());
    CHECK(bundledFace("Block") == nullptr);
    CHECK(blockFace().height() == 8);
    CHECK(vgaFace().height() == 16);
    CHECK(miniFace().height() == 8);
    CHECK(blockFace().letterSpacing() == 0);
    CHECK(vgaFace().letterSpacing() == 0);
    CHECK(miniFace().letterSpacing() == 1);
}

TEST_CASE("bundled faces draw printable ASCII and nothing else") {
    for (const Face* f : bundledFaces()) {
        INFO(f->name());
        for (char32_t ch = 0x20; ch <= 0x7e; ++ch) {
            INFO(static_cast<char>(ch));
            const auto g = f->glyph(ch);
            REQUIRE(g);
            CHECK(g->mask.height == f->height());
            CHECK(!g->cells);
            // Every glyph but the space has ink.
            CHECK((g->mask.count() > 0) == (ch != U' '));
        }
        for (char32_t ch : {U'\t', U'\n', U'\0', char32_t(0x7f), U'é', U'★', U'░'})
            CHECK(!f->glyph(ch));
    }
    for (char32_t ch = 0x20; ch <= 0x7e; ++ch) {
        CHECK(blockFace().glyph(ch)->mask.width == 8);
        CHECK(vgaFace().glyph(ch)->mask.width == 8);
    }
}

TEST_CASE("mini letters are 1 to 5 columns, on 4 cell rows with the bottom pixel row blank") {
    for (char32_t ch = U'A'; ch <= U'z'; ++ch) {
        if (ch > U'Z' && ch < U'a')
            continue;
        INFO(static_cast<char>(ch));
        const InkMask m = miniFace().glyph(ch)->mask;
        CHECK(m.width >= 1);
        CHECK(m.width <= 5);
        for (int x = 0; x < m.width; ++x)
            CHECK(!m.at(x, 7));
    }
    CHECK(miniFace().glyph(U' ')->mask.width == 2);
    CHECK(miniFace().glyph(U'l')->mask.width == 1);
    CHECK(miniFace().glyph(U'M')->mask.width == 5);
}

TEST_CASE("the bundled font tables are the ones in VileR's pack, byte for byte") {
    // SHA-256 of what fon2inc.py extracts from Bm437_IBM_BIOS.FON and
    // Bm437_IBM_VGA_8x16.FON in oldschool_pc_font_pack_v2.2_FULL.zip
    // (third_party/oldschool-pc-fonts/README.md).
    const auto sha = [](std::span<const std::uint8_t> bits) {
        const QByteArray bytes(reinterpret_cast<const char*>(bits.data()), static_cast<qsizetype>(bits.size()));
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
    };
    CHECK(sha(ibmBios8x8()) == "75c79a7e7fa423dda67ec6d6d76cec86b63f85677726368750c75b0920ddf319");
    CHECK(sha(ibmVga8x16()) == "a8bad6fd78475a6bc2a05438c19207a9bb8c0f4f4099f60384e158cdc3eba580");
}

TEST_CASE("block glyphs are the IBM PC BIOS 8x8, with its slashed 0") {
    CHECK(glyphRows(blockFace(), U'0') == bitmapRows({0x7c, 0xc6, 0xce, 0xde, 0xf6, 0xe6, 0x7c, 0x00}));
    CHECK(glyphRows(blockFace(), U'O') == bitmapRows({0x38, 0x6c, 0xc6, 0xc6, 0xc6, 0x6c, 0x38, 0x00}));
    CHECK(glyphRows(blockFace(), U'A') == bitmapRows({0x30, 0x78, 0xcc, 0xcc, 0xfc, 0xcc, 0xcc, 0x00}));
    CHECK(glyphRows(blockFace(), U'_') == bitmapRows({0, 0, 0, 0, 0, 0, 0, 0xff}));
    // The slash: the 0's diagonal runs from bottom left to top right inside
    // the ring, where the O is open.
    const InkMask zero = blockFace().glyph(U'0')->mask;
    for (auto [x, y] : {std::pair{4, 2}, {3, 3}, {2, 4}})
        CHECK(zero.at(x, y));
}

TEST_CASE("vga glyphs are the IBM VGA 8x16, its own dotted 0 kept") {
    CHECK(glyphRows(vgaFace(), U'0') ==
          bitmapRows({0, 0, 0x38, 0x6c, 0xc6, 0xc6, 0xd6, 0xd6, 0xc6, 0xc6, 0x6c, 0x38, 0, 0, 0, 0}));
    CHECK(glyphRows(vgaFace(), U'A') ==
          bitmapRows({0, 0, 0x10, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0, 0, 0, 0}));
}

TEST_CASE("mini glyphs are ascii.py's, pixel by pixel") {
    using R = std::vector<std::string>;
    CHECK(glyphRows(miniFace(), U'0') == R{".##.", "#..#", "#.##", "##.#", ".##.", "....", "....", "...."});
    CHECK(glyphRows(miniFace(), U'O') == R{".##.", "#..#", "#..#", "#..#", ".##.", "....", "....", "...."});
    CHECK(glyphRows(miniFace(), U'o') == R{"...", ".#.", "#.#", "#.#", ".#.", "...", "...", "..."});
    CHECK(glyphRows(miniFace(), U'I') == R{"###", ".#.", ".#.", ".#.", "###", "...", "...", "..."});
    CHECK(glyphRows(miniFace(), U'1') == R{".#.", "##.", ".#.", ".#.", "###", "...", "...", "..."});
    CHECK(glyphRows(miniFace(), U'l') == R{"#", "#", "#", "#", "#", ".", ".", "."});
    CHECK(glyphRows(miniFace(), U'_') == R{"..", "..", "..", "..", "..", "..", "##", ".."});
    CHECK(glyphRows(miniFace(), U'g') == R{"...", ".##", "#.#", "#.#", ".##", "..#", "##.", "..."});
    CHECK(glyphRows(miniFace(), U'M') == R{"#...#", "##.##", "#.#.#", "#...#", "#...#", ".....", ".....", "....."});
    CHECK(glyphRows(miniFace(), U'[') == R{"##", "#.", "#.", "#.", "#.", "#.", "##", ".."});
}

TEST_CASE("every fixture word renders in every bundled face within 80 columns") {
    const auto words = fixtureWords();
    REQUIRE(words.size() == 5);
    REQUIRE(words.back() == U"the quick brown fox jumps");
    for (const Face* f : bundledFaces()) {
        for (const auto& word : words) {
            INFO(f->name() << ": " << std::string(word.begin(), word.end()));
            const TextLayout l = layoutText(*f, word, {80});
            CHECK(l.missing.empty());
            CHECK(!l.cells);
            CHECK(l.mask.width <= 80);
            CHECK(l.mask.height == static_cast<int>(l.lines.size()) * f->height());
            // Nothing clipped: the mask holds every glyph's ink.
            int ink = 0;
            for (const auto& line : l.lines)
                for (size_t k = line.first; k < line.first + line.count; ++k)
                    ink += f->glyph(word[k])->mask.count();
            CHECK(l.mask.count() == ink);
            if (word.find(U' ') == word.npos)
                CHECK(l.lines.size() == 1);
        }
    }
}

TEST_CASE("fixture words lay out at the faces' own widths and wrap at spaces") {
    const auto words = fixtureWords();
    // block and vga: 8 columns a character, no spacing.
    for (const Face* f : {&blockFace(), &vgaFace()}) {
        for (size_t i = 0; i < 4; ++i)
            CHECK(layoutText(*f, words[i], {80}).mask.width == 8 * static_cast<int>(words[i].size()));
        const auto l = layoutText(*f, words[4], {80});
        CHECK(lineTexts(l, words[4]) == std::vector<std::u32string>{U"the quick", U"brown fox", U"jumps"});
        CHECK(l.lines[0].width == 72);
        CHECK(l.lines[2].width == 40);
    }
    // mini: each glyph's width plus 1 between glyphs.
    CHECK(layoutText(miniFace(), words[0], {80}).mask.width == 37);
    CHECK(layoutText(miniFace(), words[1], {80}).mask.width == 35);
    CHECK(layoutText(miniFace(), words[2], {80}).mask.width == 24);
    CHECK(layoutText(miniFace(), words[3], {80}).mask.width == 36);
    const auto l = layoutText(miniFace(), words[4], {80});
    CHECK(lineTexts(l, words[4]) == std::vector<std::u32string>{U"the quick brown fox", U"jumps"});
    CHECK(l.lines[0].width == 71);
    CHECK(l.lines[1].width == 20);
    CHECK(layoutText(miniFace(), words[4]).lines.size() == 1);
    CHECK(layoutText(miniFace(), words[4]).mask.width == 95);
}

TEST_CASE("layout places glyphs with the face's letter spacing, lines a face height apart") {
    TestFace f(2, 1);
    f.glyphs[U'a'] = FaceGlyph{maskFrom({"#.", ".#"}), std::nullopt};
    f.glyphs[U'b'] = FaceGlyph{maskFrom({"###", "#.."}), std::nullopt};
    f.glyphs[U' '] = FaceGlyph{InkMask(2, 2), std::nullopt};
    const auto l = layoutText(f, U"ab\nb a");
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].width == 6);
    CHECK(l.lines[1].width == 9);
    CHECK(rowsOf(l.mask) == std::vector<std::string>{
                                "#..###...",
                                ".#.#.....",
                                "###....#.",
                                "#.......#",
                            });
    // An empty line keeps its height; leading spaces are kept.
    const auto e = layoutText(f, U"a\n\n  a");
    CHECK(lineTexts(e, U"a\n\n  a") == std::vector<std::u32string>{U"a", U"", U"  a"});
    CHECK(e.mask.height == 6);
    CHECK(e.lines[2].width == 8);
    CHECK(e.mask.at(6, 4));
    CHECK(layoutText(f, U"").lines.size() == 1);
}

TEST_CASE("wrapping breaks at spaces, and inside a word only when it is too long for a line") {
    // Letters 2 columns wide with 1 of spacing: n letters take 3n - 1.
    const TestFace f = blockyFace(2, 1, U"abcdefgh");
    using T = std::vector<std::u32string>;
    CHECK(lineTexts(layoutText(f, U"ab cd", {}), U"ab cd") == T{U"ab cd"});
    CHECK(lineTexts(layoutText(f, U"ab cd", {14}), U"ab cd") == T{U"ab cd"});
    CHECK(lineTexts(layoutText(f, U"ab cd", {13}), U"ab cd") == T{U"ab", U"cd"});
    // The spaces at a wrap are dropped, however many.
    CHECK(lineTexts(layoutText(f, U"ab   cd", {8}), U"ab   cd") == T{U"ab", U"cd"});
    // Spaces that end a paragraph without wrapping are kept.
    CHECK(lineTexts(layoutText(f, U"ab cd ", {8}), U"ab cd ") == T{U"ab", U"cd "});
    CHECK(layoutText(f, U"ab cd ", {8}).lines[1].width == 8);
    // A word longer than the width breaks at the character that would cross it.
    CHECK(lineTexts(layoutText(f, U"abcdefg", {8}), U"abcdefg") == T{U"abc", U"def", U"g"});
    CHECK(lineTexts(layoutText(f, U"a bcdefgh", {8}), U"a bcdefgh") == T{U"a", U"bcd", U"efg", U"h"});
    // Spaces that start a line are kept and are no place to wrap.
    CHECK(lineTexts(layoutText(f, U"  abc", {8}), U"  abc") == T{U"  a", U"bc"});
    // A line holds at least one character, even one wider than the width.
    CHECK(lineTexts(layoutText(f, U"abc", {1}), U"abc") == T{U"a", U"b", U"c"});
    const auto l = layoutText(f, U"abcdefg", {8});
    CHECK(l.mask.width == 8);
    CHECK(l.lines[2].width == 2);
    CHECK(l.mask.count() == 7 * 4);
}

TEST_CASE("spaces at a paragraph's edges wrap like any character; only the ones between words are dropped") {
    // block: every character 8 columns, no spacing, so a width of 8 holds one.
    const Face& f = blockFace();
    using T = std::vector<std::u32string>;
    CHECK(lineTexts(layoutText(f, U" A", {8}), U" A") == T{U" ", U"A"});
    CHECK(lineTexts(layoutText(f, U"A ", {8}), U"A ") == T{U"A", U" "});
    CHECK(lineTexts(layoutText(f, U"   ", {8}), U"   ") == T{U" ", U" ", U" "});
    CHECK(lineTexts(layoutText(f, U" A B ", {8}), U" A B ") == T{U" ", U"A", U"B", U" "});
    const auto l = layoutText(f, U" A", {8});
    CHECK(l.lines[0].count == 1);
    CHECK(l.lines[0].width == 8);
    // The spaces between two words at a wrap are still dropped, whether the
    // wrap falls on the first of them or a later one.
    CHECK(lineTexts(layoutText(f, U"A B", {8}), U"A B") == T{U"A", U"B"});
    CHECK(lineTexts(layoutText(f, U"A  B", {8}), U"A  B") == T{U"A", U"B"});
    CHECK(lineTexts(layoutText(f, U"A  B", {16}), U"A  B") == T{U"A", U"B"});
    CHECK(lineTexts(layoutText(f, U" A  B ", {16}), U" A  B ") == T{U" A", U"B "});
    const auto e = layoutText(f, U"A ", {8});
    REQUIRE(e.lines.size() == 2);
    CHECK(e.lines[1].width == 8);
}

TEST_CASE("a character the face can't draw is blank at the space's width and listed") {
    const std::u32string text = U"A★B\tC";
    const auto l = layoutText(blockFace(), text);
    CHECK(l.missing == std::vector<size_t>{1, 3});
    CHECK(l.mask.width == 5 * 8);
    const InkMask a = blockFace().glyph(U'A')->mask, b = blockFace().glyph(U'B')->mask,
                  c = blockFace().glyph(U'C')->mask;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            CHECK(l.mask.at(x, y) == a.at(x, y));
            CHECK(!l.mask.at(8 + x, y));
            CHECK(l.mask.at(16 + x, y) == b.at(x, y));
            CHECK(!l.mask.at(24 + x, y));
            CHECK(l.mask.at(32 + x, y) == c.at(x, y));
        }
    // In mini a missing character is the space's 2 columns plus spacing.
    const auto m = layoutText(miniFace(), U"IéI");
    CHECK(m.missing == std::vector<size_t>{1});
    CHECK(m.mask.width == 3 + 1 + 2 + 1 + 3);
    CHECK(layoutText(miniFace(), U"funkpower").missing.empty());
}

TEST_CASE("a source cell is ink unless transparent paste reads it as blank") {
    CHECK(!sourceCellInk(Cell{}));
    CHECK(!sourceCellInk(cell(U' ', Black)));
    CHECK(!sourceCellInk(cell(U'\0', Black)));
    CHECK(!sourceCellInk(cell(U' ', Black)));
    CHECK(!sourceCellInk(transparentCell()));
    Cell rgbBlack = cell(U' ', Black);
    rgbBlack.bg = Color::fromRgb({0, 0, 0});
    CHECK(!sourceCellInk(rgbBlack));
    // A space on any other background is ink, and so is blink on black.
    CHECK(sourceCellInk(cell(U' ', Blue)));
    CHECK(sourceCellInk(cell(U'\0', Red)));
    CHECK(sourceCellInk(cell(U' ', Black, attr::Blink)));
    Cell rgbGrey = rgbBlack;
    rgbGrey.bg = Color::fromRgb({1, 1, 1});
    CHECK(sourceCellInk(rgbGrey));
    CHECK(sourceCellInk(cell(U'A', Black)));
    CHECK(sourceCellInk(cell(U'░', Black)));

    // The mask of a source cell is the whole cell: both its pixels.
    CellBlock block(3, 2);
    block.at(0, 0) = cell(U'█', Black);
    block.at(1, 0) = cell(U' ', Black);
    block.at(2, 0) = cell(U' ', Blue);
    block.at(0, 1) = transparentCell();
    block.at(1, 1) = cell(U'▀', Black);
    block.at(2, 1) = cell(U' ', Black, attr::Blink);
    CHECK(rowsOf(maskOfCells(block)) == std::vector<std::string>{"#.#", "#.#", ".##", ".##"});
}

TEST_CASE("layout carries a cell face's source cells, holes elsewhere") {
    TestFace f(2, 1);
    CellBlock x(2, 1);
    x.at(0, 0) = cell(U'▓', Red);
    x.at(1, 0) = cell(U' ', Black);
    f.glyphs[U'x'] = FaceGlyph{maskOfCells(x), x};
    f.glyphs[U' '] = FaceGlyph{InkMask(1, 2), std::nullopt};
    const auto l = layoutText(f, U"x x\nx", {});
    REQUIRE(l.cells);
    CHECK(l.cells->width == l.mask.width);
    CHECK(l.cells->width == 7);
    CHECK(l.cells->height == 2);
    CHECK(rowsOf(l.mask) == std::vector<std::string>{"#....#.", "#....#.", "#......", "#......"});
    CHECK(l.cells->at(0, 0) == x.at(0, 0));
    CHECK(l.cells->at(1, 0) == x.at(1, 0));
    CHECK(l.cells->at(5, 0) == x.at(0, 0));
    CHECK(l.cells->at(6, 0) == x.at(1, 0));
    CHECK(l.cells->at(0, 1) == x.at(0, 0));
    CHECK(l.cells->at(1, 1) == x.at(1, 0));
    for (auto [cx, cy] : {std::pair{2, 0}, {3, 0}, {4, 0}, {2, 1}, {6, 1}})
        CHECK(l.cells->at(cx, cy) == transparentCell());
    // Pixel faces have no source cells.
    CHECK(!layoutText(blockFace(), U"x").cells);
}

TEST_CASE("the confusable rule: at most 2 pixels apart, or a one-pixel shift") {
    const InkMask base = maskFrom({"###", "#.#", "###"});
    CHECK(confusableMasks(base, base));
    CHECK(confusableMasks(base, maskFrom({"###", "###", "###"})));
    CHECK(confusableMasks(base, maskFrom({"##.", "###", "###"})));
    CHECK(!confusableMasks(base, maskFrom({"#..", "###", "###"})));
    // A one-pixel shift any way along a row or column, however many pixels
    // that moves.
    const InkMask bar = maskFrom({"#...", "#...", "#...", "#..."});
    CHECK(confusableMasks(bar, maskFrom({".#..", ".#..", ".#..", ".#.."})));
    CHECK(confusableMasks(maskFrom({".#..", ".#..", ".#..", ".#.."}), bar));
    CHECK(confusableMasks(maskFrom({"###.", "#...", "....", "...."}), maskFrom({"....", "###.", "#...", "...."})));
    CHECK(!confusableMasks(bar, maskFrom({"..#.", "..#.", "..#.", "..#."})));
    // A diagonal step is two shifts, not one.
    const InkMask square = maskFrom({"....", ".##.", ".##.", "...."});
    for (const auto& moved : {std::vector<std::string>{"##..", "##..", "....", "...."},
                              {"..##", "..##", "....", "...."},
                              {"....", "....", "##..", "##.."},
                              {"....", "....", "..##", "..##"}}) {
        CHECK(!confusableMasks(square, maskFrom(moved)));
        CHECK(!confusableMasks(maskFrom(moved), square));
    }
    // Masks are aligned left and padded to the wider...
    CHECK(confusableMasks(maskFrom({"#.#", "###"}), maskFrom({"#.#..", "###.."})));
    CHECK(!confusableMasks(maskFrom({"#.#", "###"}), maskFrom({"..#.#", "..###"})));
    // ...and on the baseline, their bottom rows.
    CHECK(confusableMasks(maskFrom({"#.#", "###"}), maskFrom({"...", "...", "#.#", "###"})));
    CHECK(!confusableMasks(maskFrom({"###", "#.#"}), maskFrom({"###", "#.#", "...", "...", "..."})));
}

TEST_CASE("screening finds confusable pairs within each set, once, skipping what the face lacks") {
    TestFace f(4, 1);
    const auto put = [&](char32_t ch, std::vector<std::string> rows) {
        f.glyphs[ch] = FaceGlyph{maskFrom(rows), std::nullopt};
    };
    put(U'Z', {"###", "..#", ".#.", "###"});
    put(U'B', {"##.", "#.#", "###", "#.#"});
    put(U'8', {".#.", "#.#", ".#.", "#.#"});  // 3 pixels off B
    put(U'O', {"###", "#.#", "#.#", "###"});
    put(U'0', {"###", "#.#", "#.#", "##."});  // 1 pixel off O
    put(U'2', {".##", "..#", ".#.", "###"});  // 1 pixel off Z
    put(U'l', {"#..", "#..", "#..", "#.."});
    put(U'_', {".#.", ".#.", ".#.", ".#."});  // l shifted right by one
    put(U'I', {".#.", ".#.", ".#.", "..."});  // 1 pixel off _, but they share no set
    const auto pairs = screenConfusables(f);
    CHECK(pairs == std::vector<ConfusablePair>{{U'O', U'0'}, {U'Z', U'2'}, {U'l', U'_'}});
    // In typed text: both characters have to be there.
    CHECK(confusablesIn(f, U"ZB8S5O0I1") == std::vector<ConfusablePair>{{U'O', U'0'}});
    CHECK(confusablesIn(f, U"Z2zMWKAH") == std::vector<ConfusablePair>{{U'Z', U'2'}});
    CHECK(confusablesIn(f, U"l23456_") == std::vector<ConfusablePair>{{U'l', U'_'}});
    CHECK(confusablesIn(f, U"funkpower").empty());
    CHECK(confusablesIn(f, U"O").empty());
    CHECK(!confusableMasks(f.glyph(U'B')->mask, f.glyph(U'8')->mask));
    CHECK(confusableMasks(f.glyph(U'I')->mask, f.glyph(U'_')->mask));
}

TEST_CASE("the bundled faces' shipped confusables are what screening finds") {
    for (const Face* f : bundledFaces()) {
        INFO(f->name());
        CHECK(f->confusables() == screenConfusables(*f));
    }
    CHECK(blockFace().confusables().empty());
    CHECK(vgaFace().confusables().empty());
    CHECK(miniFace().confusables() ==
          std::vector<ConfusablePair>{{U'S', U'5'}, {U'O', U'0'}, {U'Z', U'2'}, {U'K', U'H'}});
    CHECK(confusablesIn(miniFace(), U"ZB8S5O0I1") ==
          std::vector<ConfusablePair>{{U'S', U'5'}, {U'O', U'0'}});
    CHECK(confusablesIn(miniFace(), U"funkpower").empty());
}

TEST_CASE("no two screened sets share two characters, so screening reports a pair once") {
    for (size_t i = 0; i < kConfusableSets.size(); ++i)
        for (size_t j = i + 1; j < kConfusableSets.size(); ++j) {
            int shared = 0;
            for (char32_t ch : kConfusableSets[i])
                shared += kConfusableSets[j].find(ch) != std::u32string_view::npos;
            CHECK(shared <= 1);
        }
    for (std::u32string_view set : kConfusableSets)
        for (size_t i = 0; i < set.size(); ++i)
            CHECK(set.find(set[i], i + 1) == std::u32string_view::npos);
}

TEST_CASE("confusable marks on the fixture words match the rule") {
    // For every pair of characters of a word that share a screened set, the
    // pair is marked exactly when the rule holds for the face's masks.
    for (const Face* f : bundledFaces()) {
        for (const auto& word : fixtureWords()) {
            INFO(f->name() << ": " << std::string(word.begin(), word.end()));
            const auto marked = confusablesIn(*f, word);
            int checked = 0;
            for (std::u32string_view set : kConfusableSets)
                for (size_t i = 0; i < set.size(); ++i)
                    for (size_t j = i + 1; j < set.size(); ++j) {
                        if (word.find(set[i]) == word.npos || word.find(set[j]) == word.npos)
                            continue;
                        ++checked;
                        const bool rule = confusableMasks(f->glyph(set[i])->mask, f->glyph(set[j])->mask);
                        const bool mark = std::find(marked.begin(), marked.end(), ConfusablePair{set[i], set[j]}) !=
                                          marked.end();
                        CHECK(rule == mark);
                    }
            // The three screened words hold every pair of their own set.
            if (word == kConfusableSets[0])
                CHECK(checked == 36);
            if (word == kConfusableSets[2])
                CHECK(checked == 21);
        }
    }
}
