// The bundled text-style presets (library/text-styles, PLAN §15): the
// fixture matrix, each preset on each fixture word in each face, with iCE on
// and (unless the preset needs it) off, held to the ladder lint, the IRC line
// budget and the 80 columns; and each preset's round trips through its
// Library file and the project file.

#include "blockwright/library.h"

#include "blockwright/ansi.h"
#include "blockwright/face.h"
#include "blockwright/font_store.h"
#include "blockwright/irc.h"
#include "blockwright/lint.h"
#include "blockwright/lint_rules.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/style_draw.h"
#include "blockwright/tdf.h"
#include "blockwright/text.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace bw;
using namespace bw::library;
namespace fs = std::filesystem;

namespace {

const fs::path kPresets = fs::path(BW_BUNDLED_LIBRARY) / "text-styles";
constexpr int kColumns = 80;

std::vector<TextStyleEntry> presets() {
    std::vector<TextStyleEntry> out;
    for (const fs::directory_entry& e : fs::directory_iterator(kPresets))
        if (isTextStyleName(e.path()))
            out.push_back(readTextStyleEntry(e.path()));
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return out;
}

// The faces of the matrix: the bundled pixel faces and the three TheDraw
// test fonts (tests/data/tdf), each glyph at most 8 columns. A TheDraw
// face is its file's (`file`), which a project copies in; `ref` names it.
struct MatrixFace {
    std::string name;
    std::unique_ptr<Face> owned;
    const Face* face = nullptr;
    FaceRef ref;
    FontFilePtr file;
};

std::vector<MatrixFace> faces() {
    std::vector<MatrixFace> out;
    for (const char* name : {"block", "vga", "mini"}) {
        const Face* f = bundledFace(name);
        REQUIRE(f);
        FaceRef ref;
        ref.name = name;
        out.push_back({name, nullptr, f, ref, nullptr});
    }
    for (const char* file : {"bw-outline.tdf", "bw-block.tdf", "bw-colour.tdf"}) {
        MatrixFace m{file, nullptr, nullptr};
        m.file = loadFontFile(bwtest::readFile(bwtest::dataDir() / "tdf" / file));
        REQUIRE(m.file);
        REQUIRE(m.file->names.size() == 1);
        m.face = m.file->face(0);
        REQUIRE(m.face);
        m.ref = {FaceRef::Kind::Tdf, m.file->names[0], m.file->sha256, 0};
        out.push_back(std::move(m));
    }
    return out;
}

// The fixture words (tests/data/text/words.txt), one a line; a line with a
// space is the sentence, set at a wrap of 80 columns.
std::vector<TextSource> words() {
    std::vector<TextSource> out;
    const std::string text = bwtest::readFile(bwtest::dataDir() / "text" / "words.txt");
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty())
            continue;
        TextSource s;
        s.text.assign(line.begin(), line.end());
        s.wrapWidth = line.find(' ') == std::string::npos ? 0 : kColumns;
        out.push_back(std::move(s));
    }
    REQUIRE(out.size() == 5);
    return out;
}

// The ramps a preset's effects name: the ramps its drawing is judged on.
std::set<std::string> rampsOf(const TextStyle& style) {
    std::set<std::string> out;
    for (const Effect& e : style.effects) {
        if (!e.enabled)
            continue;
        if (const auto* f = std::get_if<FillEffect>(&e.settings))
            out.insert(f->ramp);
        else if (const auto* g = std::get_if<GlowEffect>(&e.settings))
            out.insert(g->ramp);
        else if (const auto* s = std::get_if<ShadeEffect>(&e.settings))
            out.insert(s->ramp);
        else if (const auto* b = std::get_if<BevelEffect>(&e.settings))
            out.insert(b->ramp);
    }
    return out;
}

// One drawing of the matrix and what is wrong with it, or "".
struct Case {
    const TextStyleEntry* preset;
    const MatrixFace* face;
    const TextSource* word;
    bool ice;
};

std::string utf8(const std::u32string& s) {
    return std::string(s.begin(), s.end());
}

// The column after the last drawn (non-blank) cell of row y.
int extent(const Canvas& c, int y) {
    int end = 0;
    for (int x = 0; x < c.width(); ++x)
        if (!c.at(x, y).transparent && !isBlank(c.at(x, y)))
            end = x + 1;
    return end;
}

// How a cell looks: the colours of its top and bottom halves, or nothing
// for a glyph of two colours that isn't a half block (a shade, a letter),
// which looks like itself. A transparent cell shows black.
std::optional<std::pair<Color, Color>> halves(const Cell& c) {
    if (c.transparent)
        return std::pair{Color::indexed(Black), Color::indexed(Black)};
    if (c.ch == U' ' || c.ch == U'\0' || c.ch == U'\u00a0')
        return std::pair{c.bg, c.bg};
    if (c.ch == U'\u2588' || c.fg == c.bg)
        return std::pair{c.fg, c.fg};
    if (c.ch == U'\u2580')
        return std::pair{c.fg, c.bg};
    if (c.ch == U'\u2584')
        return std::pair{c.bg, c.fg};
    return std::nullopt;
}

bool looksSame(const Cell& a, const Cell& b) {
    const auto ha = halves(a), hb = halves(b);
    return ha || hb ? ha == hb : a == b;
}

// What the case breaks, a line each; "" when it passes. The lint is the
// lint panel's and `lint`'s default: every named ramp with the craft pairs,
// held to its ladder findings (noise and the craft rules are reports, not
// ladder faults).
std::string faults(const Case& k) {
    std::string out;
    const TextStyle& style = k.preset->style;
    const TextRender render = renderStyledText(*k.word, style, *k.face->face, k.ice);
    const Canvas& drawn = render.cells;

    // the letters: every character drawn, with ink, and the ink showing:
    // the drawing doesn't look like the style drawn round no letters (a
    // plate alone, or black letters on black)
    if (!render.missing.empty())
        out += std::to_string(render.missing.size()) + " character(s) the face doesn't have\n";
    const StyleMargins m = styleMargins(style);
    const StyleInput in = textStyleInput(*k.word, *k.face->face);
    if (in.mask.count() == 0)
        out += "the letters have no ink\n";
    StyleInput none = in;
    none.mask = InkMask(in.mask.width, in.mask.height);
    for (Canvas* c : {&none.pixels, &none.source})
        for (int y = 0; y < c->height(); ++y)
            for (int x = 0; x < c->width(); ++x)
                c->at(x, y).transparent = true;
    const Canvas bare = drawStyle(none, style, k.ice, m);
    bool shows = false;
    for (int y = 0; y < drawn.height() && !shows; ++y)
        for (int x = 0; x < drawn.width() && !shows; ++x)
            shows = !looksSame(drawn.at(x, y), bare.at(x, y));
    if (!shows)
        out += "no letter shows\n";

    // the lint
    lint::LintOptions options;
    options.ramps = namedRamps();
    options.craftPairs = true;
    options.ice = k.ice;
    for (const lint::Finding& f : lint::lintCanvas(drawn, options).findings)
        if (lint::ladderKind(f.kind))
            out += "lint " + f.ramp + " " + std::string(lint::kindName(f.kind)) + " " + std::to_string(f.x) + "," +
                   std::to_string(f.y) + ": " + f.message + "\n";

    // the budget
    const std::vector<std::string> lines = irc::encodeCanvas(drawn);
    for (std::size_t y = 0; y < lines.size(); ++y)
        if (lines[y].size() > irc::kEfnetWeechatBudget)
            out += "row " + std::to_string(y) + " is " + std::to_string(lines[y].size()) + " bytes\n";

    // the columns: each line with the margins, and each drawn row
    const TextLayout layout = layoutText(*k.face->face, k.word->text, {k.word->wrapWidth});
    for (std::size_t i = 0; i < layout.lines.size(); ++i)
        if (const int w = m.left + layout.lines[i].width + m.right; w > kColumns)
            out += "line " + std::to_string(i) + " with its margins is " + std::to_string(w) + " columns\n";
    for (int y = 0; y < drawn.height(); ++y)
        if (const int e = extent(drawn, y); e > kColumns)
            out += "row " + std::to_string(y) + " is drawn to column " + std::to_string(e) + "\n";

    // unclipped: drawn with room to spare, the style draws nothing beyond
    // its margins, and within them what the layer holds
    constexpr int kSpare = 8;
    const StyleMargins wide{m.left + kSpare, m.top + kSpare, m.right + kSpare, m.bottom + kSpare};
    const Canvas roomy = drawStyle(in, style, k.ice, wide);
    if (drawStyle(in, style, k.ice, m) != drawn)
        out += "renderStyledText isn't drawStyle at the style's margins\n";
    bool clipped = false;
    for (int y = 0; y < roomy.height() && !clipped; ++y)
        for (int x = 0; x < roomy.width() && !clipped; ++x) {
            const int ix = x - kSpare, iy = y - kSpare;
            const bool inside = ix >= 0 && iy >= 0 && ix < drawn.width() && iy < drawn.height();
            if (inside ? roomy.at(x, y) != drawn.at(ix, iy) : !roomy.at(x, y).transparent)
                clipped = true;
        }
    if (clipped)
        out += "the margins clip it\n";
    return out;
}

std::vector<bool> iceSettings(const TextStyleEntry& preset) {
    return preset.iceRequired ? std::vector<bool>{true} : std::vector<bool>{true, false};
}

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "text_presets" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("the bundled presets are the ten, none needing iCE", "[presets]") {
    const std::map<std::string, bool> expected{{"Gold 3D", false}, {"Shadow", false}, {"Outline", false},
                                               {"Chrome", false},  {"Ice", false},    {"Neon", false},
                                               {"Warning sign", false}, {"Masthead", false}, {"Fire", false},
                                               {"Rainbow", false}};
    std::map<std::string, bool> got;
    for (const TextStyleEntry& p : presets()) {
        INFO(p.path.string());
        CHECK(p.path.filename().string() == slug(p.name) + ".style.json");
        CHECK_FALSE(textStyleProblem(p.style));
        got[p.name] = p.iceRequired;
    }
    CHECK(got == expected);
}

TEST_CASE("the bundled presets name ramps, not colour numbers, where they can", "[presets]") {
    // a gradient's colours come from its ramp; a flat fill, a plate, an
    // outline and a shadow are one colour each, which a ramp can't name
    for (const TextStyleEntry& p : presets()) {
        INFO(p.name);
        CHECK_FALSE(rampsOf(p.style).empty());
        // a ramp fill under Shade or Bevel stacks two gradients: a preset
        // picks one
        const FillEffect* fill = p.style.get<FillEffect>();
        if (fill && fill->mode != FillMode::Flat) {
            CHECK_FALSE(p.style.get<ShadeEffect>());
            CHECK_FALSE(p.style.get<BevelEffect>());
        }
    }
}

TEST_CASE("every preset on every fixture word and face lints clean and fits 80 columns and the budget",
          "[presets][matrix]") {
    const std::vector<TextStyleEntry> all = presets();
    const std::vector<MatrixFace> fs = faces();
    const std::vector<TextSource> ws = words();
    REQUIRE(all.size() == 10);
    int cases = 0;
    for (const TextStyleEntry& p : all)
        for (const MatrixFace& f : fs)
            for (const TextSource& w : ws)
                for (const bool ice : iceSettings(p)) {
                    INFO(p.name << " / " << f.name << " / " << utf8(w.text) << " / iCE " << (ice ? "on" : "off"));
                    CHECK(faults({&p, &f, &w, ice}) == "");
                    ++cases;
                }
    // 10 presets, each with iCE on and off, over 6 faces and 5 words
    CHECK(cases == 10 * 2 * 6 * 5);
}

TEST_CASE("the TheDraw test fonts unstyled lint clean and fit 80 columns and the budget", "[presets][matrix]") {
    // The baseline row: what each preset starts from. A preset's finding on
    // a test font is its own, not the font's. The bundled pixel faces aren't
    // held to it: their grey specks lint (steel speckle) with no style at all.
    TextStyleEntry none;
    none.name = "unstyled";
    const std::vector<MatrixFace> fs = faces();
    const std::vector<TextSource> ws = words();
    int cases = 0;
    for (const MatrixFace& f : fs) {
        if (!f.file)
            continue;
        for (const TextSource& w : ws)
            for (const bool ice : {true, false}) {
                INFO(f.name << " / " << utf8(w.text) << " / iCE " << (ice ? "on" : "off"));
                CHECK(faults({&none, &f, &w, ice}) == "");
                ++cases;
            }
    }
    // 3 test fonts, 5 words, iCE on and off
    CHECK(cases == 3 * 5 * 2);
}

TEST_CASE("a ramp Fill on every named ramp lints clean on the TheDraw test fonts", "[presets][matrix]") {
    // #184: a ramp Fill maps a TheDraw face's colours onto consecutive ramp
    // steps, so the font's own ladder stays one on the ramp: no ramp, however
    // many steps it has, skips a rung between colours the font shades between.
    const std::vector<MatrixFace> fs = faces();
    const std::vector<TextSource> ws = words();
    int cases = 0;
    for (const Ramp& r : namedRamps())
        for (const FillMode mode : {FillMode::Rows, FillMode::Columns, FillMode::Rainbow}) {
            TextStyleEntry fill;
            fill.name = r.name + " fill";
            fill.style.effects.push_back(Effect{true, FillEffect{mode, Grey, r.name}});
            for (const MatrixFace& f : fs) {
                if (!f.file)
                    continue;
                for (const TextSource& w : ws)
                    for (const bool ice : {true, false}) {
                        INFO(r.name << " / mode " << int(mode) << " / " << f.name << " / " << utf8(w.text)
                                    << " / iCE " << (ice ? "on" : "off"));
                        CHECK(faults({&fill, &f, &w, ice}) == "");
                        // a source cell of two colours keeps two
                        const StyleInput in = textStyleInput(w, *f.face);
                        const StyleMargins m = styleMargins(fill.style);
                        const Canvas drawn = renderStyledText(w, fill.style, *f.face, ice).cells;
                        int merged = 0;
                        for (int y = 0; y < in.source.height(); ++y)
                            for (int x = 0; x < in.source.width(); ++x) {
                                const Cell& c = in.source.at(x, y);
                                const Cell& d = drawn.at(x + m.left, y + m.top);
                                if (!c.transparent && c.fg != c.bg && d.fg == d.bg)
                                    ++merged;
                            }
                        CHECK(merged == 0);
                        ++cases;
                    }
            }
        }
    // 10 ramps, 3 modes, 3 test fonts, 5 words, iCE on and off
    CHECK(cases == 10 * 3 * 3 * 5 * 2);
}

// The comparison sheets: for each face and iCE setting, each fixture word
// unstyled and then in each preset, each drawing under its name, written as
// <face>-ice-<on|off>.ans in the directory BW_PRESET_SHEETS names. Run with
// "[.sheets]"; `blockwright-cli render` turns a sheet into a PNG.
TEST_CASE("comparison sheets of the presets", "[.sheets]") {
    const char* dir = std::getenv("BW_PRESET_SHEETS");
    REQUIRE(dir);
    fs::create_directories(dir);
    TextStyleEntry none;
    none.name = "unstyled";
    std::vector<TextStyleEntry> looks{none};
    for (TextStyleEntry& p : presets())
        looks.push_back(std::move(p));
    const std::vector<TextSource> ws = words();
    for (const MatrixFace& f : faces())
        for (const bool ice : {true, false}) {
            std::vector<Canvas> parts;
            for (const TextSource& w : ws)
                for (const TextStyleEntry& p : looks) {
                    Canvas label(kColumns, 1);
                    const std::string name = p.name + " / " + utf8(w.text);
                    for (std::size_t i = 0; i < name.size() && int(i) < kColumns; ++i)
                        label.at(int(i), 0).ch = char32_t(name[i]);
                    parts.push_back(label);
                    parts.push_back(renderStyledText(w, p.style, *f.face, ice).cells);
                    parts.push_back(Canvas(kColumns, 1));
                }
            int height = 0;
            for (const Canvas& c : parts)
                height += c.height();
            AnsiDocument doc;
            doc.ice = ice;
            doc.canvas = Canvas(kColumns, height);
            int y = 0;
            for (const Canvas& c : parts) {
                for (int cy = 0; cy < c.height(); ++cy)
                    for (int cx = 0; cx < c.width() && cx < kColumns; ++cx)
                        if (!c.at(cx, cy).transparent)
                            doc.canvas.at(cx, y + cy) = c.at(cx, cy);
                y += c.height();
            }
            std::ofstream out(fs::path(dir) / (slug(f.name) + "-ice-" + (ice ? "on" : "off") + ".ans"), std::ios::binary);
            out << writeAnsi(doc).bytes;
        }
}

TEST_CASE("an entry refuses a key given twice at any depth, and a flat Fill's unknown ramp", "[presets]") {
    const auto refusal = [](const std::string& json) -> std::string {
        try {
            parseTextStyleEntry(json);
        } catch (const LibraryError& e) {
            return e.what();
        }
        return "";
    };
    const std::string head = R"({"kind": "text-style", "name": "Test", "category": "test", "tags": [], "effects": [)";
    CHECK(refusal(head + R"({"kind": "fill", "enabled": true, "mode": "flat", "colour": 4, "colour": 7, "ramp": "steel"}]})") ==
          "text style has the key \"colour\" more than once");
    CHECK(refusal(head + R"({"kind": "fill", "enabled": true, "mode": "flat", "colour": 4, "\u0063olour": 7, "ramp": "steel"}]})") ==
          "text style has the key \"colour\" more than once");
    CHECK(refusal(head + R"({"kind": "fill", "enabled": true, "mode": "flat", "colour": 7, "ramp": "does-not-exist"}]})") ==
          "text style: fill ramp \"does-not-exist\" is not a named ramp");
    // switched off too, as textStyleProblem checks switched-off effects
    CHECK(refusal(head + R"({"kind": "outline", "enabled": true, "form": "pixels", "colour": 15},
        {"kind": "fill", "enabled": false, "mode": "flat", "colour": 7, "ramp": "does-not-exist"}]})") ==
          "text style: fill ramp \"does-not-exist\" is not a named ramp");
    CHECK(refusal(head + R"({"kind": "fill", "enabled": true, "mode": "flat", "colour": 7, "ramp": "steel"}]})") == "");

    // a project file still opens a flat Fill whose ramp this build lacks
    project::Project doc;
    doc.layers = LayerStack(80, 25);
    TextSource s;
    s.text = U"funk";
    Layer l = makeTextLayer(s, "title");
    LayerProperties props = l.props;
    props.style = TextStyle{{Effect{true, FillEffect{FillMode::Flat, 7, "does-not-exist"}}}};
    applyLayerProperties(l, props);
    doc.layers.add(l);
    const fs::path zip = scratch("flat-ramp") / "flat.bwp";
    project::writeZip(doc, zip);
    CHECK(project::read(zip) == doc);
}

TEST_CASE("preset matrix rejects a missing face", "[presets][matrix]") {
    auto p = readTextStyleEntry(kPresets / "outline.style.json");
    TextSource w;
    w.text = U"funkpower";
    MatrixFace f{"missing", tdfFace(TdfFont{}), nullptr};
    f.face = f.owned.get();

    const std::string why = faults({&p, &f, &w, true});
    CHECK_FALSE(why.empty());
    CHECK(why.find("9 character(s) the face doesn't have") != std::string::npos);
    CHECK(why.find("the letters have no ink") != std::string::npos);
}

TEST_CASE("preset matrix rejects a preset whose letters don't show", "[presets][matrix]") {
    // Outline made a flat black fill with no outline: black on black
    TextStyleEntry p = readTextStyleEntry(kPresets / "outline.style.json");
    p.style.effects = {Effect{true, FillEffect{FillMode::Flat, 0, "sea"}}};
    REQUIRE_FALSE(textStyleProblem(p.style));
    // bw-colour's ▓▒░ and space source cells keep their blue background
    // under a flat Fill, which sets only the foreground, so its letters show
    const std::vector<MatrixFace> faceList = faces();
    for (const MatrixFace& f : faceList)
        for (const TextSource& w : words())
            for (const bool ice : {true, false}) {
                if (f.name == "bw-colour.tdf")
                    continue;
                INFO(f.name << " / " << utf8(w.text) << " / iCE " << (ice ? "on" : "off"));
                CHECK(faults({&p, &f, &w, ice}).find("no letter shows") != std::string::npos);
            }
    // on a plate the same letters show
    p.style.effects.push_back(Effect{true, PlateEffect{6, 1}});
    TextSource w;
    w.text = U"funkpower";
    CHECK(faults({&p, &faceList[0], &w, true}).find("no letter shows") == std::string::npos);
}

TEST_CASE("a preset read from its Library file is the same stack, and writes back to the same bytes",
          "[presets][roundtrip]") {
    const fs::path dir = scratch("roundtrip");
    for (const TextStyleEntry& p : presets()) {
        INFO(p.path.string());
        const std::string bytes = bwtest::readFile(p.path);
        TextStyleEntry parsed = parseTextStyleEntry(bytes);
        parsed.path = p.path;
        CHECK(parsed == p);
        CHECK(textStyleEntryJson(p) == bytes);

        // written to another Library and scanned, it is the same entry
        const fs::path copy = dir / p.path.filename();
        {
            std::ofstream out(copy, std::ios::binary);
            out << textStyleEntryJson(p);
        }
        TextStyleEntry reread = readTextStyleEntry(copy);
        CHECK(reread.path == copy);
        reread.path = p.path;
        CHECK(reread == p);
    }
    const ScanResult r = scan(dir);
    CHECK(r.problems.empty());
    CHECK(r.stamps.empty());
    CHECK(r.styles.size() == 10);
}

TEST_CASE("a text layer in each preset, face, word and iCE setting saves, reopens, redraws, undoes and bakes",
          "[presets][roundtrip]") {
    const fs::path dir = scratch("project");
    const std::vector<TextStyleEntry> all = presets();
    const std::vector<MatrixFace> faceList = faces();
    const std::vector<TextSource> ws = words();
    int cases = 0;
    for (std::size_t pi = 0; pi < all.size(); ++pi) {
        const TextStyleEntry& p = all[pi];
        // the style it is changed to after reopening: the next preset's
        const TextStyle& next = all[(pi + 1) % all.size()].style;
        for (const MatrixFace& f : faceList)
            for (std::size_t wi = 0; wi < ws.size(); ++wi)
                for (const bool ice : iceSettings(p)) {
                    INFO(p.name << " / " << f.name << " / " << utf8(ws[wi].text) << " / iCE " << (ice ? "on" : "off"));
                    project::Project doc;
                    doc.layers = LayerStack(80, 25);
                    doc.ice = ice;
                    if (f.file)
                        doc.fonts.add(f.file);
                    const FontSources fonts{&doc.fonts};
                    TextSource s = ws[wi];
                    s.face = f.ref;
                    Layer l = makeTextLayer(s, "title", 2, 1, fonts);
                    LayerProperties props = l.props;
                    props.style = p.style;
                    applyLayerProperties(l, props, fonts, ice);
                    const Canvas drawn = renderStyledText(s, p.style, *f.face, ice).cells;
                    REQUIRE(l.cells == drawn);
                    const LayerId id = doc.layers.add(l);

                    const std::string name = slug(p.name) + "-" + std::to_string(cases);
                    project::writeZip(doc, dir / (name + ".bwp"));
                    project::writeDirectory(doc, dir / name);
                    for (const fs::path& path : {dir / (name + ".bwp"), dir / name}) {
                        INFO(path.filename().string());
                        project::Project back = project::read(path);
                        CHECK(back == doc);
                        CHECK(back.ice == ice);
                        REQUIRE(back.layers.size() == 1);
                        Layer& bl = back.layers[0];
                        REQUIRE(bl.props.id == id);
                        CHECK(bl.props.text == s);
                        CHECK(bl.props.style == p.style);
                        CHECK(bl.cells == drawn);
                        const FontSources bf{&back.fonts};

                        // a new style redraws from the text; undo gives the cells back
                        const LayerPropsEdit restyle = styleEdit(bl, next, back.ice);
                        CHECK(applyEdit(bl, restyle, false, bf));
                        CHECK(bl.cells == renderStyledText(s, next, *f.face, ice).cells);
                        CHECK(applyEdit(bl, restyle, true, bf));
                        CHECK(bl.props.style == p.style);
                        CHECK(bl.cells == drawn);

                        // a text edit, its undo and its redo
                        TextSource t = s;
                        t.text = ws[(wi + 1) % ws.size()].text;
                        t.wrapWidth = ws[(wi + 1) % ws.size()].wrapWidth;
                        const Canvas edited = renderStyledText(t, p.style, *f.face, ice).cells;
                        const LayerPropsEdit edit = textEdit(bl, t, 0, back.ice);
                        CHECK(applyEdit(bl, edit, false, bf));
                        CHECK(bl.props.text == t);
                        CHECK(bl.cells == edited);
                        CHECK(applyEdit(bl, edit, true, bf));
                        CHECK(bl.props.text == s);
                        CHECK(bl.cells == drawn);
                        CHECK(applyEdit(bl, edit, false, bf));
                        CHECK(bl.props.text == t);
                        CHECK(bl.cells == edited);
                        CHECK(applyEdit(bl, edit, true, bf));

                        // Bake keeps the picture and drops the text and style;
                        // undo gives back the live layer
                        const LayerProperties live = bl.props;
                        const LayerPropsEdit bake = bakeEdit(bl, back.ice);
                        applyEdit(bl, bake, false, bf);
                        CHECK_FALSE(bl.props.text);
                        CHECK_FALSE(bl.props.style);
                        CHECK(bl.cells == drawn);
                        applyEdit(bl, bake, true, bf);
                        CHECK(bl.props == live);
                        CHECK(bl.cells == drawn);
                    }
                    fs::remove_all(dir / name);
                    fs::remove(dir / (name + ".bwp"));
                    ++cases;
                }
    }
    CHECK(cases == 10 * 2 * 6 * 5);
}
