// Live text layers (PLAN §15): drawing a source through a face, the
// property-undo steps (typing runs, settings, Bake) and the source in the
// project file.

#include "blockwright/text.h"

#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "util.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// test_project.cpp's allocation hook: while not 0, an allocation on this
// thread of more bytes than this throws std::bad_alloc.
namespace bwtest {
extern thread_local std::size_t largestAllocation;
}

using namespace bw;
using bw::project::Entries;
using bw::project::Project;
using bw::project::ProjectError;
namespace fs = std::filesystem;

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

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "text" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

InkMask mask(const std::vector<std::string>& rows) {
    InkMask m(static_cast<int>(rows.front().size()), static_cast<int>(rows.size()));
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            m.set(x, y, rows[y][x] == '#');
    return m;
}

const Cell kSourceCell{U'X', Color::indexed(Blue), Color::indexed(Red), 0, false};

// A face two cells high: A (two columns), B (one blank column), C (one
// column, inked, with a source cell on top and none below), a space.
// Nothing else, so 'a' is missing.
class TestFace final : public Face {
public:
    std::string_view name() const override { return "test"; }
    int height() const override { return 4; }
    int letterSpacing() const override { return 0; }
    std::optional<FaceGlyph> glyph(char32_t ch) const override {
        switch (ch) {
        case U'A': return FaceGlyph{mask({"#.", ".#", "##", "##"}), std::nullopt};
        case U'B': return FaceGlyph{mask({".", ".", ".", "."}), std::nullopt};
        case U' ': return FaceGlyph{mask({".", ".", ".", "."}), std::nullopt};
        case U'C': {
            CellBlock cells(1, 2);
            cells.at(0, 0) = kSourceCell;
            cells.at(0, 1) = transparentCell();
            return FaceGlyph{mask({"#", "#", "#", "#"}), cells};
        }
        default: return std::nullopt;
        }
    }
    std::vector<ConfusablePair> confusables() const override { return {}; }
};

TextSource source(std::u32string text, std::string face = "block") {
    TextSource s;
    s.text = std::move(text);
    s.face.name = std::move(face);
    return s;
}

Cell ink(char32_t ch, std::uint8_t colour = Grey) { return Cell{ch, Color::indexed(colour), Color::indexed(Black), 0, false}; }

// Each row of cells as characters: the cell's own, '.' for transparent.
std::vector<std::u32string> rowsOf(const Canvas& c) {
    std::vector<std::u32string> out;
    for (int y = 0; y < c.height(); ++y) {
        std::u32string r;
        for (int x = 0; x < c.width(); ++x)
            r += c.at(x, y).transparent ? U'.' : c.at(x, y).ch.codePoint().value();
        out.push_back(r);
    }
    return out;
}

// What the drawing rule gives a cell from its two pixels, independently of
// renderText: █, ▀, ▄ in the colour on black, or transparent.
Cell fromPixels(bool upper, bool lower, std::uint8_t colour) {
    if (!upper && !lower)
        return transparentCell();
    return ink(upper && lower ? kFullBlock : upper ? kUpperHalf : kLowerHalf, colour);
}

// The source drawn left-aligned with no extra spacing, cell by cell from
// layoutText's mask, into a box `width` wide.
Canvas expectedLeft(const TextSource& s, const Face& f, int width) {
    const TextLayout layout = layoutText(f, s.text, {s.wrapWidth});
    Canvas c(width, layout.mask.height / 2);
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < width; ++x)
            c.at(x, y) = fromPixels(layout.mask.at(x, 2 * y), layout.mask.at(x, 2 * y + 1), s.colour);
    return c;
}

Project projectWith(Layer layer) {
    Project p;
    p.layers = LayerStack(80, 25);
    p.layers.add(std::move(layer));
    return p;
}

std::string json(const QJsonObject& o) {
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

QJsonObject object(const std::string& text) {
    return QJsonDocument::fromJson(QByteArray(text.data(), static_cast<qsizetype>(text.size()))).object();
}

// The project's single layer's JSON, changed by `edit`, then decoded.
template <typename Edit>
Project decodeEdited(const Project& p, Edit edit) {
    Entries e = project::encode(p);
    const std::string path = "layers/" + std::to_string(p.layers[0].props.id) + ".json";
    QJsonObject o = object(e.at(path));
    edit(o);
    e[path] = json(o);
    return project::decode(e);
}

// The project through a zip and a directory, each read back.
void checkSaves(const Project& p, const fs::path& dir) {
    project::writeZip(p, dir / "piece.bwp");
    CHECK(project::read(dir / "piece.bwp") == p);
    project::writeDirectory(p, dir / "piece");
    CHECK(project::read(dir / "piece") == p);
}

} // namespace

// ---- drawing ----------------------------------------------------------------

TEST_CASE("the Text tool types no C0, DEL or C1 control, and everything else (#428)", "[text]") {
    for (const char32_t ch : {0x00u, 0x09u, 0x0au, 0x1bu, 0x1fu, 0x7fu, 0x80u, 0x85u, 0x9bu, 0x9fu}) {
        INFO(unsigned(ch));
        CHECK_FALSE(typedInText(ch));
    }
    for (const char32_t ch : {0x20u, 0x41u, 0x7eu, 0xa0u, 0xa1u, 0xe9u, 0x2588u, 0x1f600u}) {
        INFO(unsigned(ch));
        CHECK(typedInText(ch));
    }
}

TEST_CASE("a source draws its face's pixels as half blocks in its colour", "[text][render]") {
    const TestFace face;
    TextSource s = source(U"AB");
    s.colour = LightCyan;
    const TextRender r = renderText(s, face);
    CHECK(r.missing.empty());
    REQUIRE(r.cells.width() == 3);
    REQUIRE(r.cells.height() == 2);
    // A: "#." over ".#" is ▀▄; "##" over "##" is ██. B is blank: transparent.
    CHECK(r.cells.at(0, 0) == ink(kUpperHalf, LightCyan));
    CHECK(r.cells.at(1, 0) == ink(kLowerHalf, LightCyan));
    CHECK(r.cells.at(0, 1) == ink(kFullBlock, LightCyan));
    CHECK(r.cells.at(1, 1) == ink(kFullBlock, LightCyan));
    CHECK(r.cells.at(2, 0) == transparentCell());
    CHECK(r.cells.at(2, 1) == transparentCell());
    CHECK(r.cells.at(2, 0).transparent);
}

TEST_CASE("a face's source cells win where it has them", "[text][render]") {
    const TestFace face;
    const TextRender r = renderText(source(U"C"), face);
    REQUIRE(r.cells.width() == 1);
    REQUIRE(r.cells.height() == 2);
    CHECK(r.cells.at(0, 0) == kSourceCell);
    // no source cell below: the mask's pixels
    CHECK(r.cells.at(0, 1) == ink(kFullBlock));
}

TEST_CASE("capitals, and characters the face lacks", "[text][render]") {
    const TestFace face;
    TextSource s = source(U"aA");
    const TextRender typed = renderText(s, face);
    CHECK(typed.missing == std::vector<std::size_t>{0});
    // a missing character is laid out blank, a space wide
    CHECK(rowsOf(typed.cells) == std::vector<std::u32string>{U".▀▄", U".██"});

    s.textCase = TextCase::Capitals;
    const TextRender caps = renderText(s, face);
    CHECK(caps.missing.empty());
    CHECK(rowsOf(caps.cells) == std::vector<std::u32string>{U"▀▄▀▄", U"████"});
    // only a-z change
    s.text = U"é";
    CHECK(renderText(s, face).missing == std::vector<std::size_t>{0});
}

TEST_CASE("letter spacing adds blank columns, line spacing blank rows", "[text][render]") {
    const TestFace face;
    TextSource s = source(U"AA\nA");
    CHECK(rowsOf(renderText(s, face).cells) == std::vector<std::u32string>{U"▀▄▀▄", U"████", U"▀▄..", U"██.."});

    s.letterSpacing = 2;
    s.lineSpacing = 3;
    CHECK(rowsOf(renderText(s, face).cells) ==
          std::vector<std::u32string>{U"▀▄..▀▄", U"██..██", U"......", U"......", U"......", U"▀▄....", U"██...."});
    // a single line has no line spacing
    s.text = U"A";
    CHECK(renderText(s, face).cells.height() == 2);
}

TEST_CASE("each line takes its place in the box by the alignment", "[text][render]") {
    const TestFace face;
    // the second line is one column (C) in a box four wide: three left over
    TextSource s = source(U"AA\nC");
    s.align = TextAlign::Left;
    CHECK(rowsOf(renderText(s, face).cells)[2] == U"X...");
    s.align = TextAlign::Centre;  // the odd column goes on the right
    CHECK(rowsOf(renderText(s, face).cells)[2] == U".X..");
    s.align = TextAlign::Right;
    CHECK(rowsOf(renderText(s, face).cells)[2] == U"...X");
    // the widest line fills the box whatever the alignment
    CHECK(rowsOf(renderText(s, face).cells)[0] == U"▀▄▀▄");

    // the wrap width widens the box
    s.wrapWidth = 8;
    const Canvas c = renderText(s, face).cells;
    CHECK(c.width() == 8);
    CHECK(rowsOf(c)[0] == U"....▀▄▀▄");
    CHECK(rowsOf(c)[2] == U".......X");
}

TEST_CASE("the fixture words draw in every bundled face from its letter masks", "[text][render][fixture]") {
    for (const Face* face : bundledFaces()) {
        for (const std::u32string& word : fixtureWords()) {
            INFO(face->name() << " " << std::string(word.begin(), word.end()));
            TextSource s = source(word, std::string(face->name()));
            const TextRender r = renderText(s, *face);
            CHECK(r.missing.empty());
            const TextLayout layout = layoutText(*face, word);
            CHECK(r.cells.width() == layout.mask.width);
            CHECK(r.cells == expectedLeft(s, *face, layout.mask.width));
            CHECK(r.cells.height() == face->height() / 2);
        }
    }
}

TEST_CASE("wrapped at 80 columns, every line fits and the box is 80 wide", "[text][render][fixture]") {
    const std::u32string fox = U"the quick brown fox jumps";
    for (const Face* face : bundledFaces()) {
        INFO(face->name());
        TextSource s = source(fox + U" " + fox + U" " + fox, std::string(face->name()));
        s.wrapWidth = 80;
        const TextLayout layout = layoutText(*face, s.text, {80});
        CHECK(layout.lines.size() > 1);
        for (const auto& line : layout.lines)
            CHECK(line.width <= 80);
        const Canvas c = renderText(s, *face).cells;
        CHECK(c.width() == 80);
        CHECK(c.height() == static_cast<int>(layout.lines.size()) * face->height() / 2);
        CHECK(c == expectedLeft(s, *face, 80));
    }
}

// ---- sources, faces and layers ------------------------------------------------

TEST_CASE("measuring text gives layoutText's size without drawing it", "[text][face]") {
    const TestFace test;
    std::vector<const Face*> faces(bundledFaces().begin(), bundledFaces().end());
    faces.push_back(&test);
    std::vector<std::u32string> texts = fixtureWords();
    texts.insert(texts.end(), {U"", U"\n", U"ABC A\nC", U"  a  b  ", U"A\n\nAB"});
    for (const Face* face : faces)
        for (const std::u32string& text : texts)
            for (const int wrap : {0, 1, 3, 80}) {
                INFO(face->name() << " " << std::string(text.begin(), text.end()) << " wrap " << wrap);
                const TextLayout layout = layoutText(*face, text, {wrap});
                const TextExtent e = measureText(*face, text, {wrap});
                CHECK(e.width == layout.mask.width);
                CHECK(e.height == layout.mask.height);
                CHECK(e.lines == static_cast<std::int64_t>(layout.lines.size()));
            }
}

TEST_CASE("a face reference finds the bundled pixel faces", "[text]") {
    for (const Face* face : bundledFaces()) {
        FaceRef ref;
        ref.name = std::string(face->name());
        CHECK(findFace(ref) == face);
    }
    FaceRef ref;
    CHECK(findFace(ref) == &blockFace());  // block by default
    ref.name = "nope";
    CHECK(findFace(ref) == nullptr);
    CHECK(faceKindName(FaceRef::Kind::Pixel) == "pixel");
    CHECK(faceKindFromName("pixel") == FaceRef::Kind::Pixel);
    CHECK(faceKindName(FaceRef::Kind::Tdf) == "tdf");
    CHECK(faceKindFromName("tdf") == FaceRef::Kind::Tdf);
    CHECK_FALSE(faceKindFromName("truetype"));
    CHECK(textCaseFromName(textCaseName(TextCase::Capitals)) == TextCase::Capitals);
    CHECK(textAlignFromName(textAlignName(TextAlign::Centre)) == TextAlign::Centre);
    CHECK_FALSE(textAlignFromName("center"));
}

TEST_CASE("a source that can't be drawn says why", "[text]") {
    TextSource s = source(U"hi");
    CHECK_FALSE(textSourceProblem(s));
    s.letterSpacing = s.lineSpacing = TextSource::kMaxSpacing;
    s.wrapWidth = TextSource::kMaxWrapWidth;
    s.colour = 15;
    CHECK_FALSE(textSourceProblem(s));

    const auto problem = [](auto change) {
        TextSource t = source(U"hi");
        change(t);
        return textSourceProblem(t).value_or("");
    };
    CHECK(problem([](TextSource& t) { t.face.name = "nope"; }) == "no pixel face \"nope\"");
    CHECK(problem([](TextSource& t) { t.letterSpacing = -1; }).starts_with("letter spacing -1"));
    CHECK(problem([](TextSource& t) { t.letterSpacing = 65; }).starts_with("letter spacing 65"));
    CHECK(problem([](TextSource& t) { t.lineSpacing = 65; }).starts_with("line spacing 65"));
    CHECK(problem([](TextSource& t) { t.wrapWidth = -1; }).starts_with("wrap width -1"));
    CHECK(problem([](TextSource& t) { t.wrapWidth = 65536; }).starts_with("wrap width 65536"));
    CHECK(problem([](TextSource& t) { t.colour = 16; }).starts_with("colour 16"));

    // the text's length, and its box within a layer the project file holds
    CHECK(problem([](TextSource& t) { t.text = std::u32string(TextSource::kMaxLength, U'W'); }).empty());
    CHECK(problem([](TextSource& t) { t.text = std::u32string(TextSource::kMaxLength + 1, U'W'); }) ==
          "the text is 4097 characters, over 4096");
    // 4096 letters 8 + 64 columns apart: wider than 65,535
    CHECK(problem([](TextSource& t) {
              t.text = std::u32string(TextSource::kMaxLength, U'W');
              t.letterSpacing = 64;
          }).starts_with("the text draws 294848x4 cells"));
    // 65,535 wide by 32 rows: more cells than a layer holds
    CHECK(problem([](TextSource& t) {
              t.text = U"a\na\na\na";
              t.face.name = "vga";
              t.wrapWidth = TextSource::kMaxWrapWidth;
          }).starts_with("the text draws 65535x32 cells"));
    CHECK(problem([](TextSource& t) {
              t.text = U"a\na\na";
              t.face.name = "vga";
              t.wrapWidth = TextSource::kMaxWrapWidth;
          }).empty());
}

// Codex round 1: 2048 W then 2048 newlines in vga, 64 columns between
// letters. Every setting is within its cap, but its mask would be 147,392 x
// 32,784 pixels (4.8 GB) and its box is 147,392 x 16,392 cells.
TEST_CASE("a source too large to draw is refused before anything its size is allocated", "[text][limits]") {
    TextSource big = source(std::u32string(2048, U'W') + std::u32string(2048, U'\n'), "vga");
    big.letterSpacing = 64;
    const std::string why = "the text draws 147392x16392 cells";
    const Project fits = projectWith(makeTextLayer(source(U"x", "vga"), "t"));
    const Layer layer = fits.layers[0];
    const auto bigInFile = [&](QJsonObject& o) {
        QJsonObject s = o.value("source").toObject();
        s["text"] = QString::fromStdU32String(big.text);
        s["letterSpacing"] = big.letterSpacing;
        o["source"] = s;
    };

    struct Cap {
        Cap() { bwtest::largestAllocation = std::size_t{64} << 20; }
        ~Cap() { bwtest::largestAllocation = 0; }
    } cap;
    // the check, and each way a source is drawn: the core API, a new layer,
    // new properties, an undo step and the project reader
    std::optional<std::string> problem;
    REQUIRE_NOTHROW(problem = textSourceProblem(big));
    CHECK(problem.value_or("").starts_with(why));
    CHECK_THROWS_AS(renderText(big, *findFace(big.face)), TextError);
    CHECK_THROWS_AS(makeTextLayer(big), TextError);
    Layer l = layer;
    LayerProperties props = l.props;
    props.text = big;
    CHECK_THROWS_AS(applyLayerProperties(l, props), TextError);
    CHECK(l == layer);
    CHECK_THROWS_AS(applyEdit(l, textEdit(l, big)), TextError);
    CHECK(l == layer);
    std::string read = "(read)";
    try {
        decodeEdited(fits, bigInFile);
    } catch (const ProjectError& e) {
        read = e.what();
    } catch (const std::bad_alloc&) {
        read = "std::bad_alloc";
    }
    CHECK(read.find(why) != std::string::npos);
}

TEST_CASE("a live text layer is its drawn source and isn't painted on", "[text][layer]") {
    const TextSource s = source(U"funkpower", "mini");
    const Layer l = makeTextLayer(s, "Title", 3, 4);
    CHECK(l.props.kind == LayerKind::Text);
    CHECK(l.props.name == "Title");
    CHECK(l.props.x == 3);
    CHECK(l.props.y == 4);
    REQUIRE(l.props.text);
    CHECK(*l.props.text == s);
    CHECK(l.cells == renderText(s, miniFace()).cells);
    for (int y = 0; y < l.height(); ++y)
        for (int x = 0; x < l.width(); ++x)
            CHECK_FALSE(layerPaintable(l, 3 + x, 4 + y));

    // a text layer without a source is cells read as text, painted as before
    Layer plain = makeLayer(LayerKind::Text, 4, 2);
    CHECK(layerPaintable(plain, 0, 0));

    CHECK_THROWS_AS(makeTextLayer(source(U"x", "nope")), TextError);
    TextSource bad = s;
    bad.wrapWidth = -1;
    CHECK_THROWS_AS(makeTextLayer(bad), TextError);
}

TEST_CASE("new properties redraw a live text layer, and a bad source changes nothing", "[text][layer]") {
    Layer l = makeTextLayer(source(U"A"), "t");
    l.props.id = 9;
    const Layer was = l;

    LayerProperties props = l.props;
    props.id = 42;  // the layer keeps its own
    props.text->text = U"AB";
    props.text->colour = Yellow;
    applyLayerProperties(l, props);
    CHECK(l.props.id == 9);
    CHECK(l.props.text->text == U"AB");
    CHECK(l.cells == renderText(*props.text, blockFace()).cells);
    CHECK(l.width() > was.width());

    // a source no face draws, or on a layer that isn't text: thrown before
    // the layer changes
    const Layer now = l;
    props.text->face.name = "nope";
    CHECK_THROWS_AS(applyLayerProperties(l, props), TextError);
    CHECK(l == now);
    props = l.props;
    props.kind = LayerKind::Cells;
    CHECK_THROWS_AS(applyLayerProperties(l, props), TextError);
    CHECK(l == now);

    // properties without a source keep the cells
    Layer cells = makeLayer(LayerKind::Cells, 2, 2, "c");
    cells.cells.at(1, 1) = ink(U'#');
    const Canvas kept = cells.cells;
    LayerProperties hidden = cells.props;
    hidden.visible = false;
    applyLayerProperties(cells, hidden);
    CHECK_FALSE(cells.props.visible);
    CHECK(cells.cells == kept);
}

// ---- undo -----------------------------------------------------------------------

TEST_CASE("a change of text, a setting or the face is one step that undoes and redoes", "[text][undo]") {
    LayerStack stack(80, 25);
    const LayerId id = stack.add(makeTextLayer(source(U"fun"), "t", 2, 1));
    const Layer original = *stack.find(id);

    const auto step = [&](auto change) {
        TextSource next = *stack.find(id)->props.text;
        change(next);
        const Layer before = *stack.find(id);
        const LayerPropsEdit e = textEdit(before, next);
        CHECK(e.id == id);
        CHECK(e.typingRun == 0);
        const CellRect rect = applyEdit(stack, e);
        const Layer after = *stack.find(id);
        CHECK(*after.props.text == next);
        CHECK(after.cells == renderText(next, *findFace(next.face)).cells);
        // the cells to recomposite cover the layer before and after
        CHECK(rect == unite(before.bounds(), after.bounds(), stack.canvasRect()));
        applyEdit(stack, e, true);
        CHECK(*stack.find(id) == before);
        applyEdit(stack, e);
        CHECK(*stack.find(id) == after);
    };
    step([](TextSource& t) { t.text = U"funk"; });
    step([](TextSource& t) { t.face.name = "vga"; });
    step([](TextSource& t) { t.textCase = TextCase::Capitals; });
    step([](TextSource& t) { t.letterSpacing = 3; });
    step([](TextSource& t) { t.text += U"\npower"; t.lineSpacing = 2; });
    step([](TextSource& t) { t.align = TextAlign::Right; });
    step([](TextSource& t) { t.wrapWidth = 80; });
    step([](TextSource& t) { t.colour = LightRed; });
    CHECK(*stack.find(id) != original);

    LayerPropsEdit gone = textEdit(original, source(U"x"));
    gone.id = 999;
    CHECK_THROWS_AS(applyEdit(stack, gone), LayerError);
}

TEST_CASE("a run of typing coalesces into one step", "[text][undo]") {
    LayerStack stack(80, 25);
    const LayerId id = stack.add(makeTextLayer(source(U""), "t"));
    const Layer original = *stack.find(id);

    std::optional<LayerPropsEdit> step;
    std::u32string typed;
    for (char32_t ch : std::u32string(U"funkpower")) {
        typed += ch;
        TextSource next = *stack.find(id)->props.text;
        next.text = typed;
        const LayerPropsEdit e = textEdit(*stack.find(id), next, 7);
        applyEdit(stack, e);
        if (!step)
            step = e;
        else
            CHECK(mergeEdits(*step, e));
    }
    const Layer typedLayer = *stack.find(id);
    CHECK(typedLayer.props.text->text == U"funkpower");
    CHECK(step->before == original.props);
    CHECK(step->after == typedLayer.props);
    applyEdit(stack, *step, true);
    CHECK(*stack.find(id) == original);
    applyEdit(stack, *step);
    CHECK(*stack.find(id) == typedLayer);
}

TEST_CASE("a step changing the source keeps the cells it replaces; undo gives them back", "[text][undo]") {
    LayerStack stack(80, 25);
    Layer plain = makeLayer(LayerKind::Text, 6, 3, "t", 2, 1);
    plain.cells.at(4, 2) = ink(U'#', Red);
    const LayerId id = stack.add(plain);
    plain = *stack.find(id);

    // made live by one edit: the edit keeps the cells it replaces
    const LayerPropsEdit e = textEdit(plain, source(U"A"));
    REQUIRE(e.cellsBefore);
    CHECK(*e.cellsBefore == plain.cells);
    applyEdit(stack, e);
    CHECK(stack.find(id)->cells == renderText(source(U"A"), blockFace()).cells);
    applyEdit(stack, e, true);
    CHECK(*stack.find(id) == plain);

    // and by a run of typing, merged: the first keystroke's cells
    std::optional<LayerPropsEdit> run;
    for (const std::u32string text : {U"f", U"fu", U"fun"}) {
        const LayerPropsEdit key = textEdit(*stack.find(id), source(text), 3);
        REQUIRE(key.cellsBefore);
        CHECK(*key.cellsBefore == stack.find(id)->cells);
        applyEdit(stack, key);
        if (!run)
            run = key;
        else
            CHECK(mergeEdits(*run, key));
    }
    applyEdit(stack, *run, true);
    CHECK(*stack.find(id) == plain);
    applyEdit(stack, *run);
    CHECK(stack.find(id)->props.text->text == U"fun");

    // so does any edit changing a live layer's source, Bake among them,
    // but not one leaving it as it is
    CHECK(textEdit(*stack.find(id), source(U"funk")).cellsBefore == stack.find(id)->cells);
    CHECK(bakeEdit(*stack.find(id)).cellsBefore == stack.find(id)->cells);
    LayerProperties renamed = stack.find(id)->props;
    renamed.name = "renamed";
    CHECK_FALSE(propsEdit(*stack.find(id), renamed).cellsBefore);
    // dropping the source keeps them too, and changes none: the drawn cells
    // stay as cells read as text
    LayerProperties dropped = stack.find(id)->props;
    dropped.text.reset();
    const Layer live = *stack.find(id);
    const LayerPropsEdit drop = propsEdit(live, dropped);
    CHECK(drop.cellsBefore == live.cells);
    applyEdit(stack, drop);
    CHECK_FALSE(stack.find(id)->props.text);
    CHECK(stack.find(id)->cells == live.cells);
    applyEdit(stack, drop, true);
    CHECK(*stack.find(id) == live);
    // a step that leaves the layer without a source doesn't merge into a run
    LayerPropsEdit typed = textEdit(live, source(U"funky"), 3);
    LayerProperties after = typed.after;
    Layer typedLayer = live;
    applyEdit(typedLayer, typed);
    after.text.reset();
    CHECK_FALSE(mergeEdits(typed, propsEdit(typedLayer, after, 3)));
}

// A live text layer whose cells aren't what its source draws: a project
// saved with `A` whose source was then edited to `B` in its layer file opens
// with the cells it saved (PLAN §15). Codex round 1.
namespace {
Layer openedStale() {
    Layer l = makeTextLayer(source(U"A"), "t", 2, 1);
    l.props.text->text = U"B";
    return l;
}
} // namespace

TEST_CASE("a change that leaves the source as it is keeps the layer's cells", "[text][undo]") {
    const Layer opened = openedStale();
    const Canvas cached = makeTextLayer(source(U"A")).cells;
    REQUIRE(opened.cells == cached);
    REQUIRE(opened.cells != makeTextLayer(source(U"B")).cells);
    const std::vector<std::pair<const char*, void (*)(LayerProperties&)>> changes = {
        {"rename", [](LayerProperties& p) { p.name = "renamed"; }},
        {"lock", [](LayerProperties& p) { p.locked = true; }},
        {"alpha lock", [](LayerProperties& p) { p.alphaLocked = true; }},
        {"hide", [](LayerProperties& p) { p.visible = false; }},
        {"mode", [](LayerProperties& p) { p.mode = LayerMode::ColoursOnly; }},
        {"move", [](LayerProperties& p) { p.x = 7, p.y = -3; }},
    };
    for (const auto& [what, change] : changes) {
        INFO(what);
        Layer l = opened;
        LayerProperties after = l.props;
        change(after);
        const LayerPropsEdit e = propsEdit(l, after);
        CHECK_FALSE(e.cellsBefore);
        applyEdit(l, e);
        CHECK(l.props == after);
        CHECK(l.cells == cached);
        applyEdit(l, e, true);
        CHECK(l == opened);
        applyEdit(l, e);
        CHECK(l.cells == cached);
        // and set directly
        Layer m = opened;
        applyLayerProperties(m, after);
        CHECK(m.cells == cached);
    }
}

TEST_CASE("undoing any step gives back the exact cells before it", "[text][undo]") {
    const Layer opened = openedStale();
    const Canvas drawnB = makeTextLayer(source(U"B")).cells, drawnC = makeTextLayer(source(U"C")).cells;

    // a new text redraws; its undo puts the opened cells back, not B drawn
    Layer l = opened;
    TextSource c = *l.props.text;
    c.text = U"C";
    const LayerPropsEdit edit = textEdit(l, c);
    applyEdit(l, edit);
    CHECK(l.cells == drawnC);
    applyEdit(l, edit, true);
    CHECK(l == opened);
    applyEdit(l, edit);
    CHECK(l.cells == drawnC);

    // a run of typing that ends on the text it started from redraws it, and
    // its undo gives the opened cells back
    l = opened;
    TextSource bc = *l.props.text;
    bc.text = U"BC";
    LayerPropsEdit run = textEdit(l, bc, 7);
    applyEdit(l, run);
    const LayerPropsEdit back = textEdit(l, *opened.props.text, 7);
    applyEdit(l, back);
    REQUIRE(mergeEdits(run, back));
    CHECK(l.cells == drawnB);
    const Layer typed = l;
    applyEdit(l, run, true);
    CHECK(l == opened);
    applyEdit(l, run);
    CHECK(l == typed);
    // a run whose first step changed nothing takes the next one's cells
    l = opened;
    LayerPropsEdit idle = textEdit(l, *opened.props.text, 8);
    CHECK_FALSE(idle.cellsBefore);
    applyEdit(l, idle);
    const LayerPropsEdit typedC = textEdit(l, c, 8);
    applyEdit(l, typedC);
    REQUIRE(mergeEdits(idle, typedC));
    applyEdit(l, idle, true);
    CHECK(l == opened);

    // Bake keeps the opened cells; its undo gives the live layer back as it was
    l = opened;
    const LayerPropsEdit baking = bakeEdit(l);
    applyEdit(l, baking);
    CHECK(l.props.kind == LayerKind::Cells);
    CHECK(l.cells == opened.cells);
    applyEdit(l, baking, true);
    CHECK(l == opened);
    applyEdit(l, baking);
    CHECK(l.cells == opened.cells);
}

TEST_CASE("edits merge only within one run on one layer, end to start", "[text][undo]") {
    const Layer l = makeTextLayer(source(U"a"), "t");
    TextSource ab = *l.props.text;
    ab.text = U"ab";
    LayerPropsEdit first = textEdit(l, ab, 5);
    Layer after = l;
    applyLayerProperties(after, first.after);
    TextSource abc = ab;
    abc.text = U"abc";
    const LayerPropsEdit second = textEdit(after, abc, 5);

    const auto refused = [&](LayerPropsEdit into, const LayerPropsEdit& next) {
        const LayerPropsEdit kept = into;
        const bool merged = mergeEdits(into, next);
        CHECK(into == kept);
        return !merged;
    };
    // not a typing run
    LayerPropsEdit setting = first;
    setting.typingRun = 0;
    LayerPropsEdit next0 = second;
    next0.typingRun = 0;
    CHECK(refused(setting, next0));
    // another run
    LayerPropsEdit other = second;
    other.typingRun = 6;
    CHECK(refused(first, other));
    // another layer
    LayerPropsEdit elsewhere = second;
    elsewhere.id = l.props.id + 1;
    CHECK(refused(first, elsewhere));
    // not starting where the first ends
    LayerPropsEdit gap = textEdit(l, abc, 5);
    CHECK(refused(first, gap));

    LayerPropsEdit merged = first;
    CHECK(mergeEdits(merged, second));
    CHECK(merged.before == l.props);
    CHECK(merged.after.text->text == U"abc");
    CHECK(merged.typingRun == 5);
}

TEST_CASE("Bake keeps the cells and drops the source; undo gives the live layer back", "[text][undo][bake]") {
    LayerStack stack(80, 25);
    TextSource s = source(U"funkpower", "vga");
    s.colour = LightMagenta;
    const LayerId id = stack.add(makeTextLayer(s, "Title", 5, 2));
    const Layer live = *stack.find(id);

    const LayerPropsEdit e = bakeEdit(live);
    CHECK(e.id == id);
    CHECK(e.before == live.props);
    applyEdit(stack, e);
    const Layer baked = *stack.find(id);
    CHECK(baked.props.kind == LayerKind::Cells);
    CHECK_FALSE(baked.props.text);
    CHECK(baked.props.name == "Title");
    CHECK(baked.props.x == 5);
    CHECK(baked.props.y == 2);
    CHECK(baked.cells == live.cells);
    CHECK(layerPaintable(baked, 5, 2));
    CHECK(bake(live) == baked);

    applyEdit(stack, e, true);
    CHECK(*stack.find(id) == live);
    applyEdit(stack, e);
    CHECK(*stack.find(id) == baked);
}

// ---- the project file -------------------------------------------------------------

TEST_CASE("a live text layer's source is saved in its layer file", "[text][project]") {
    TextSource s = source(U"funk\npower é", "mini");
    s.textCase = TextCase::Capitals;
    s.letterSpacing = 2;
    s.lineSpacing = 1;
    s.align = TextAlign::Centre;
    s.wrapWidth = 40;
    s.colour = Yellow;
    const Project p = projectWith(makeTextLayer(s, "Title", 4, 3));
    const Entries e = project::encode(p);
    const QJsonObject layer = object(e.at("layers/" + std::to_string(p.layers[0].props.id) + ".json"));
    CHECK(layer.value("kind").toString() == "text");
    const QJsonObject src = layer.value("source").toObject();
    CHECK(src.value("text").toString() == QString::fromUtf8("funk\npower é"));
    const QJsonObject face = src.value("face").toObject();
    CHECK(face.value("kind").toString() == "pixel");
    CHECK(face.value("name").toString() == "mini");
    CHECK(src.value("case").toString() == "capitals");
    CHECK(src.value("letterSpacing").toInt() == 2);
    CHECK(src.value("lineSpacing").toInt() == 1);
    CHECK(src.value("align").toString() == "centre");
    CHECK(src.value("wrapWidth").toInt() == 40);
    CHECK(src.value("colour").toInt() == Yellow);

    const Project back = project::decode(e);
    CHECK(back == p);
    REQUIRE(back.layers[0].props.text);
    CHECK(*back.layers[0].props.text == s);
    checkSaves(p, scratch("source"));

    // only a live text layer has the key
    Project plain = projectWith(makeLayer(LayerKind::Text, 3, 1, "plain"));
    CHECK_FALSE(object(project::encode(plain).at("layers/" + std::to_string(plain.layers[0].props.id) + ".json")).contains("source"));
}

TEST_CASE("a text layer without a source still opens", "[text][project]") {
    const Project live = projectWith(makeTextLayer(source(U"old"), "t"));
    const Project old = decodeEdited(live, [](QJsonObject& o) { o.remove("source"); });
    const Layer& l = old.layers[0];
    CHECK(l.props.kind == LayerKind::Text);
    CHECK_FALSE(l.props.text);
    CHECK(l.cells == live.layers[0].cells);
    CHECK(project::decode(project::encode(old)) == old);
}

TEST_CASE("a project whose cells aren't what its source draws opens with its cells", "[text][project]") {
    // what a 1.0 reader shows (PLAN §6, §15): the source is only redrawn when it changes
    const Project saved = projectWith(makeTextLayer(source(U"A"), "t", 2, 1));
    const Project opened = decodeEdited(saved, [](QJsonObject& o) {
        QJsonObject s = o.value("source").toObject();
        s["text"] = "B";
        o["source"] = s;
    });
    const Layer& l = opened.layers[0];
    CHECK(l.cells == saved.layers[0].cells);
    REQUIRE(l.props.text);
    CHECK(l.props.text->text == U"B");
    CHECK(project::decode(project::encode(opened)) == opened);
}

TEST_CASE("a reader refuses a source it can't draw or that is misplaced", "[text][project]") {
    const Project p = projectWith(makeTextLayer(source(U"x"), "t"));
    const auto refuses = [&](auto edit) {
        try {
            decodeEdited(p, edit);
        } catch (const ProjectError& e) {
            return std::string(e.what());
        }
        return std::string("(read)");
    };
    const auto inSource = [&](auto change) {
        return refuses([&](QJsonObject& o) {
            QJsonObject s = o.value("source").toObject();
            change(s);
            o["source"] = s;
        });
    };
    const auto inFace = [&](auto change) {
        return inSource([&](QJsonObject& s) {
            QJsonObject f = s.value("face").toObject();
            change(f);
            s["face"] = f;
        });
    };
    CHECK(refuses([](QJsonObject& o) { o["kind"] = "cells"; }).find("cells layer has a text source") !=
          std::string::npos);
    CHECK(inFace([](QJsonObject& f) { f["name"] = "nope"; }).find("no pixel face \"nope\"") != std::string::npos);
    CHECK(inFace([](QJsonObject& f) { f["kind"] = "truetype"; }).find("unknown face kind") != std::string::npos);
    // a TheDraw face goes under "tdfSource" (test_font_store.cpp)
    CHECK(inFace([](QJsonObject& f) { f["kind"] = "tdf"; }).find("a tdf face under \"source\"") != std::string::npos);
    CHECK(inSource([](QJsonObject& s) { s["case"] = "lower"; }).find("unknown case") != std::string::npos);
    CHECK(inSource([](QJsonObject& s) { s["align"] = "center"; }).find("unknown alignment") != std::string::npos);
    CHECK(inSource([](QJsonObject& s) { s["text"] = 3; }).find("\"text\" is not a string") != std::string::npos);
    CHECK(inSource([](QJsonObject& s) { s["letterSpacing"] = 65; }) != "(read)");
    CHECK(inSource([](QJsonObject& s) { s["lineSpacing"] = -1; }) != "(read)");
    CHECK(inSource([](QJsonObject& s) { s["wrapWidth"] = 65536; }) != "(read)");
    CHECK(inSource([](QJsonObject& s) { s["colour"] = 16; }) != "(read)");
    CHECK(inSource([](QJsonObject& s) { s.remove("face"); }) != "(read)");
    CHECK(refuses([](QJsonObject& o) { o["source"] = "x"; }) != "(read)");
    // and the untouched file reads
    CHECK(refuses([](QJsonObject&) {}) == "(read)");
}

TEST_CASE("a writer refuses what the reader refuses", "[text][project]") {
    Layer cells = makeLayer(LayerKind::Cells, 2, 1, "c");
    cells.props.text = source(U"x");
    CHECK_THROWS_AS(project::encode(projectWith(cells)), ProjectError);

    Layer unknown = makeTextLayer(source(U"x"), "t");
    unknown.props.text->face.name = "nope";
    CHECK_THROWS_AS(project::encode(projectWith(unknown)), ProjectError);

    Layer spaced = makeTextLayer(source(U"x"), "t");
    spaced.props.text->letterSpacing = TextSource::kMaxSpacing + 1;
    CHECK_THROWS_AS(project::encode(projectWith(spaced)), ProjectError);
}

TEST_CASE("the fixture words in every bundled face survive save, reopen, undo and Bake",
          "[text][project][fixture]") {
    const fs::path dir = scratch("fixture");
    for (const Face* face : bundledFaces()) {
        for (const std::u32string& word : fixtureWords()) {
            INFO(face->name() << " " << std::string(word.begin(), word.end()));
            TextSource s = source(word, std::string(face->name()));
            if (word.find(U' ') != word.npos)
                s.wrapWidth = 80;
            Project p;
            p.layers = LayerStack(80, 25);
            const LayerId id = p.layers.add(makeTextLayer(s, "word", 1, 1));
            const Project live = p;
            checkSaves(live, dir);

            // an edit (a setting and the text), saved, undone, saved
            TextSource next = s;
            next.textCase = TextCase::Capitals;
            next.text += U"!";
            const LayerPropsEdit edit = textEdit(*p.layers.find(id), next);
            applyEdit(p.layers, edit);
            CHECK(p.layers.find(id)->props.text->text == next.text);
            checkSaves(p, dir);
            applyEdit(p.layers, edit, true);
            CHECK(p == live);
            checkSaves(p, dir);

            // Bake, saved; undone, saved
            const LayerPropsEdit bakeStep = bakeEdit(*p.layers.find(id));
            applyEdit(p.layers, bakeStep);
            const Layer& baked = *p.layers.find(id);
            CHECK(baked.props.kind == LayerKind::Cells);
            CHECK_FALSE(baked.props.text);
            CHECK(baked.cells == live.layers[0].cells);
            checkSaves(p, dir);
            applyEdit(p.layers, bakeStep, true);
            CHECK(p == live);
            checkSaves(p, dir);
            CHECK(project::read(dir / "piece.bwp").layers[0].props.text == s);
        }
    }
}
