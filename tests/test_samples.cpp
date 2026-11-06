// The sample projects (PLAN §12, samples.h): their notes' format, the
// variants a sample's notes describe, the list of samples in a folder, and
// the bundled samples themselves, held to the bar the Library's stamps are.

#include "blockwright/composite.h"
#include "blockwright/generator.h"
#include "blockwright/halfblock.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/library.h"
#include "blockwright/palette.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/samples.h"
#include "blockwright/text_source.h"
#include "blockwright/text_style.h"

#include "../cli/lint.h"

#include <QByteArray>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#ifndef _WIN32
#include <csignal>
#endif
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace bw;
using namespace bw::samples;
namespace fs = std::filesystem;

namespace {

const fs::path kSamples = BW_SAMPLES_SOURCE;

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "samples" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

struct EnvGuard {
    const char* name;
    explicit EnvGuard(const char* n, const std::string& value) : name(n) { qputenv(n, QByteArray::fromStdString(value)); }
    ~EnvGuard() { qunsetenv(name); }
};

LayerStack namedStack(const std::vector<std::string>& names) {
    LayerStack s(4, 2);
    for (const std::string& n : names)
        s.add(makeLayer(LayerKind::Cells, 4, 2, n));
    return s;
}

std::vector<std::string> names(const LayerStack& s) {
    std::vector<std::string> out;
    for (const Layer& l : s.layers())
        out.push_back(l.props.name);
    return out;
}

const Layer& named(const LayerStack& s, const std::string& name) {
    for (const Layer& l : s.layers())
        if (l.props.name == name)
            return l;
    FAIL("no layer " << name);
    throw;
}

// How light a cell looks from a distance: the luma of its foreground and
// background, weighted by how much of the cell each covers.
double value(const Cell& c) {
    const auto luma = [](const Color& col) {
        const Rgb& rgb = kPalette[col.paletteIndex()];
        return 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
    };
    int quarters = 0;
    switch (c.ch.codePointOr(kNoCodePoint)) {
    case U'░': quarters = 1; break;
    case U'▒': quarters = 2; break;
    case U'▓': quarters = 3; break;
    case U'█': quarters = 4; break;
    default: break;
    }
    return (quarters * luma(c.fg) + (4 - quarters) * luma(c.bg)) / 4;
}

std::string locks(const LayerProperties& p) {
    std::string out;
    if (p.locked)
        out = "locked";
    if (p.alphaLocked)
        out += out.empty() ? "alpha" : ", alpha";
    return out.empty() ? "-" : out;
}

project::Project small(const std::string& notes) {
    project::Project p;
    p.layers = LayerStack(4, 2);
    Layer l = makeLayer(LayerKind::Cells, 4, 2, "ground");
    l.cells.at(0, 0).ch = U'#';
    l.cells.at(0, 0).transparent = false;
    p.layers.add(std::move(l));
    p.notes = notes;
    return p;
}

} // namespace

TEST_CASE("sample notes give their title, summary, layer table and variants") {
    const Notes n = parseNotes("Some preamble\n"
                               "# Harbour at night  \n"
                               "\n"
                               "A city on the water,\n"
                               "under a moon.\n"
                               "\n"
                               "More text that isn't the summary.\n"
                               "\n"
                               "## Layers\n"
                               "\n"
                               "| Layer | Kind | Mode | Locks | What it does |\n"
                               "|---|:--|---|---|---|\n"
                               "| title | text | normal | - | The name \\| in gold. |\n"
                               "| sky | cells | normal | locked |\n"
                               "\n"
                               "## Variants\n"
                               "| Variant | Changes |\n"
                               "|---|---|\n"
                               "| Day | show day sky;  hide stars ; |\n"
                               "| Moon set | lower moon below sky |\n"
                               "## Try this\n"
                               "| not | a table we read |\n");
    CHECK(n.title == "Harbour at night");
    CHECK(n.summary == "A city on the water, under a moon.");
    REQUIRE(n.layers.size() == 2);
    CHECK(n.layers[0] == LayerRow{"title", "text", "normal", "-", "The name | in gold."});
    CHECK(n.layers[1] == LayerRow{"sky", "cells", "normal", "locked", ""});
    REQUIRE(n.variants.size() == 2);
    CHECK(n.variants[0] == Variant{"Day", {"show day sky", "hide stars"}});
    CHECK(n.variants[1] == Variant{"Moon set", {"lower moon below sky"}});
}

TEST_CASE("sample notes with no heading or tables give empty fields") {
    CHECK(parseNotes("") == Notes{});
    CHECK(parseNotes("just text\n| a | b |\n") == Notes{});
    const Notes n = parseNotes("# Only a title\n## Layers\n");
    CHECK(n.title == "Only a title");
    CHECK(n.summary.empty());
    CHECK(n.layers.empty());
    // the summary stops at a table or heading right under the title
    CHECK(parseNotes("# T\n| a |\n").summary.empty());
    CHECK(parseNotes("# T\r\n\r\nLine one\r\nline two\r\n").summary == "Line one line two");
}

TEST_CASE("sample notes whose table header is narrower than the format are refused, not read past") {
    // a header with fewer columns than the rows are read by: once an abort
    CHECK_THROWS_AS(parseNotes("# T\n\nS.\n\n## Variants\n| Variant |\n|---|\n| Day |\n"), SampleError);
    CHECK_THROWS_AS(parseNotes("## Layers\n| Layer | Kind | Mode |\n|---|---|---|\n| sky | cells | normal |\n"), SampleError);
    CHECK_THROWS_AS(parseNotes("## Layers\n|\n|---|\n|\n"), SampleError);
    CHECK_THROWS_WITH(parseNotes("## Variants\n| Variant |\n|---|\n"), "the Variants table has 1 column, not 2");
    // a full-width header with short rows under it still reads
    const Notes n = parseNotes("## Layers\n| Layer | Kind | Mode | Locks | What it does |\n|---|---|---|---|---|\n"
                               "| sky |\n## Variants\n| Variant | Changes |\n|---|---|\n| Day |\n");
    CHECK(n.layers == std::vector<LayerRow>{{"sky", "", "", "", ""}});
    CHECK(n.variants == std::vector<Variant>{{"Day", {}}});
}

TEST_CASE("a folder's sample whose notes can't be read is listed with the problem") {
    const fs::path dir = scratch("short-table");
    project::writeDirectory(small("# Broken notes\n\nOne column.\n\n## Variants\n| Variant |\n|---|\n| Day |\n"),
                            dir / "01-short");
    const std::vector<Sample> got = list(dir);
    REQUIRE(got.size() == 1);
    CHECK(got[0].title == "01-short");
    CHECK(got[0].problem == "the Variants table has 1 column, not 2");
}

TEST_CASE("a variant shows, hides, raises and lowers layers by name") {
    LayerStack s = namedStack({"a", "b", "c", "d"});  // bottom first
    applyVariant(s, {"v", {"hide b", "show b", "hide c"}});
    CHECK(named(s, "b").props.visible);
    CHECK_FALSE(named(s, "c").props.visible);

    applyVariant(s, {"v", {"raise a above c"}});
    CHECK(names(s) == std::vector<std::string>{"b", "c", "a", "d"});
    applyVariant(s, {"v", {"raise b above d"}});
    CHECK(names(s) == std::vector<std::string>{"c", "a", "d", "b"});
    applyVariant(s, {"v", {"lower b below c"}});
    CHECK(names(s) == std::vector<std::string>{"b", "c", "a", "d"});
    applyVariant(s, {"v", {"lower d below a"}});
    CHECK(names(s) == std::vector<std::string>{"b", "c", "d", "a"});
    // already there: no change
    applyVariant(s, {"v", {"raise a above d", "lower b below c"}});
    CHECK(names(s) == std::vector<std::string>{"b", "c", "d", "a"});
}

TEST_CASE("a variant's names may hold spaces and the words it splits at") {
    LayerStack s = namedStack({"day sky", "title (baked)", "fog above"});
    applyVariant(s, {"v", {"hide title (baked)", "lower fog above below day sky"}});
    CHECK_FALSE(named(s, "title (baked)").props.visible);
    CHECK(names(s) == std::vector<std::string>{"fog above", "day sky", "title (baked)"});

    // the word in the first name: the split whose sides both name layers
    applyVariant(s, {"v", {"raise fog above above day sky"}});
    CHECK(names(s) == std::vector<std::string>{"day sky", "fog above", "title (baked)"});
    LayerStack below = namedStack({"a below", "b", "c below d"});
    applyVariant(below, {"v", {"lower c below d below a below"}});
    CHECK(names(below) == std::vector<std::string>{"c below d", "a below", "b"});
    // one that splits two ways is refused
    LayerStack two = namedStack({"a", "a above b", "b above c", "c"});
    // and the message names both readings, quoting each name
    CHECK_THROWS_WITH(applyVariant(two, {"v", {"raise a above b above c"}}),
                      "\"raise a above b above c\": could be raise \"a\" above \"b above c\" or raise \"a above "
                      "b\" above \"c\"");
    CHECK(names(two) == std::vector<std::string>{"a", "a above b", "b above c", "c"});
    // one whose sides name no layer either way says which isn't there
    CHECK_THROWS_WITH(applyVariant(two, {"v", {"raise x above y above z"}}),
                      "\"raise x above y above z\": no layer is named x");
}

TEST_CASE("a variant it can't apply is refused, leaving the changes before it made") {
    LayerStack s = namedStack({"a", "b", "a2"});
    CHECK_THROWS_AS(applyVariant(s, {"v", {"hide a", "hide nothing"}}), SampleError);
    CHECK_FALSE(named(s, "a").props.visible);
    CHECK_THROWS_AS(applyVariant(s, {"v", {"spin a"}}), SampleError);
    CHECK_THROWS_AS(applyVariant(s, {"v", {"show"}}), SampleError);
    CHECK_THROWS_AS(applyVariant(s, {"v", {"raise a"}}), SampleError);
    CHECK_THROWS_AS(applyVariant(s, {"v", {"raise a below b"}}), SampleError);
    CHECK_THROWS_AS(applyVariant(s, {"v", {"raise a above a"}}), SampleError);
    CHECK_THROWS_AS(applyVariant(s, {"v", {"lower a below"}}), SampleError);
    CHECK(names(s) == std::vector<std::string>{"a", "b", "a2"});

    LayerStack twice = namedStack({"x", "x"});
    CHECK_THROWS_AS(applyVariant(twice, {"v", {"hide x"}}), SampleError);
}

TEST_CASE("the samples in a folder are listed by file name with their notes' titles") {
    const fs::path dir = scratch("list");
    project::writeZip(small("# Second\n\nThe other one.\n"), dir / "02-b.bwp");
    project::writeDirectory(small("# First\n\nThe first one.\n"), dir / "01-a");
    project::writeZip(small("no title here\n"), dir / "03-untitled.BWP");
    project::writeZip(small("# Hidden\n"), dir / ".04-hidden.bwp");
    std::ofstream(dir / "05-broken.bwp", std::ios::binary) << "not a zip";
    std::ofstream(dir / "readme.txt") << "not a sample";
    fs::create_directories(dir / "empty-folder");

    const std::vector<Sample> got = list(dir);
    REQUIRE(got.size() == 4);
    CHECK(got[0].path == dir / "01-a");
    CHECK(got[0].title == "First");
    CHECK(got[0].summary == "The first one.");
    CHECK(got[0].problem.empty());
    CHECK(got[1].title == "Second");
    CHECK(got[2].title == "03-untitled");
    CHECK(got[2].problem.empty());
    CHECK(got[3].title == "05-broken");
    CHECK_FALSE(got[3].problem.empty());

    CHECK(list(dir / "not-there").empty());
}

TEST_CASE("the bundled samples are found from the binary's folder or the environment") {
    qunsetenv("BLOCKWRIGHT_SAMPLES");
    // the install layout's own path from the binaries to the samples
    // (core/CMakeLists.txt): ../share/... by default, ../Resources/... in
    // the macOS bundle
    const fs::path installed = (fs::path("/opt/bw/bin") / BW_SAMPLES_FROM_BINDIR).lexically_normal();
    CHECK(installed.filename() == "samples");
    CHECK(installed.parent_path().filename() == "blockwright");
    CHECK(bundledDir("/opt/bw/bin") == installed);
    {
        EnvGuard env("BLOCKWRIGHT_SAMPLES", "/elsewhere/samples");
        CHECK(bundledDir("/opt/bw/bin") == fs::path("/elsewhere/samples"));
    }
    EnvGuard empty("BLOCKWRIGHT_SAMPLES", "");
    CHECK(bundledDir("/opt/bw/bin") == installed);
}

// ---- the bundled samples ----------------------------------------------------

TEST_CASE("every bundled sample reads, and its notes describe its layers top first") {
    const std::vector<Sample> all = list(kSamples);
    REQUIRE(all.size() >= 1);
    const std::regex defaultName("Layer [0-9]+");
    for (const Sample& s : all) {
        INFO(s.path.filename().string());
        REQUIRE(s.problem.empty());
        // "NN-": the order they're listed in
        CHECK(std::regex_search(s.path.filename().string(), std::regex("^[0-9][0-9]-")));
        const project::Project p = project::read(s.path);
        const Notes n = parseNotes(p.notes);
        CHECK_FALSE(n.title.empty());
        CHECK_FALSE(n.summary.empty());
        CHECK(s.title == n.title);
        REQUIRE(p.sauce.has_value());
        CHECK(p.sauce->author == "funkpower");
        CHECK(p.sauce->title == n.title);
        CHECK_FALSE(p.snapshots.empty());

        const auto& layers = p.layers.layers();
        REQUIRE(n.layers.size() == layers.size());
        std::set<std::string> seen;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const LayerProperties& lp = layers[layers.size() - 1 - i].props;
            const LayerRow& row = n.layers[i];
            INFO(lp.name);
            CHECK_FALSE(std::regex_match(lp.name, defaultName));
            CHECK(seen.insert(lp.name).second);
            CHECK(row.name == lp.name);
            CHECK(row.kind == layerKindName(lp.kind));
            CHECK(row.mode == layerModeName(lp.mode));
            CHECK(row.locks == locks(lp));
            CHECK_FALSE(row.does.empty());
        }
    }
}

TEST_CASE("every bundled sample, in every variant, lints clean and fits one IRC line a row") {
    for (const Sample& s : list(kSamples)) {
        const project::Project saved = project::read(s.path);
        std::vector<Variant> variants{{"As saved", {}}};
        for (const Variant& v : parseNotes(saved.notes).variants)
            variants.push_back(v);
        for (const Variant& v : variants) {
            INFO(s.path.filename().string() << ": " << v.name);
            project::Project p = saved;
            REQUIRE_NOTHROW(applyVariant(p.layers, v));
            const AnsiDocument doc = project::toAnsi(p);
            const lint::LintReport r = bwcli::lintDocument(doc, {});
            INFO(bwcli::lintText(r));
            CHECK(r.findings.empty());
            const std::vector<std::string> lines = irc::encodeCanvas(doc.canvas);
            CHECK(irc::txtOverBudget(lines, saved.ircBudget.bytes).empty());
        }
    }
}

namespace {

// Whether, with iCE off, a pixel layer's bright pixel keeps its colour in a
// cell whose other half shows a different bright colour under it, the half
// under coming out as its dark counterpart (PLAN §5, composite.h): the upper
// layer wins.
bool upperLayerWins(const project::Project& p) {
    if (p.ice)
        return false;
    const CompositeOptions off{false, Cell{}};
    const LayerStack& stack = p.layers;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const Layer& l = stack[i];
        if (l.props.kind != LayerKind::Pixels || !l.props.visible)
            continue;
        for (int y = 0; y < l.cells.height(); ++y)
            for (int x = 0; x < l.cells.width(); ++x) {
                const int cx = l.props.x + x, cy = l.props.y + y;
                if (cx < 0 || cy < 0 || cx >= stack.width() || cy >= stack.height())
                    continue;
                const LayerPixels px = layerPixels(l.cells.at(x, y));
                if (px.top.has_value() == px.bottom.has_value())
                    continue;
                const bool top = px.top.has_value();
                const std::uint8_t mine = top ? *px.top : *px.bottom;
                const HalfPair under = halvesUnder(compositeCellBelow(stack, i, cx, cy, off), false);
                const std::uint8_t other = top ? under.bottom : under.top;
                if (mine < 8 || other < 8 || other == mine)
                    continue;
                const auto shown = halfBlocks(compositeCell(stack, cx, cy, off), false);
                if (shown && (top ? shown->top : shown->bottom) == mine && (top ? shown->bottom : shown->top) == other - 8)
                    return true;
            }
    }
    return false;
}

bool insideCanvas(const Layer& l, const LayerStack& s) {
    return l.props.x >= 0 && l.props.y >= 0 && l.props.x + l.cells.width() <= s.width() &&
           l.props.y + l.cells.height() <= s.height();
}

} // namespace

TEST_CASE("the bundled samples hold nothing a library version could encode differently") {
    const auto bytes = [](const fs::path& path) {
        std::ifstream f(path, std::ios::binary);
        REQUIRE(f);
        return std::string(std::istreambuf_iterator<char>(f), {});
    };
    // every layer's cells in a raw frame, which no zstd version changes
    int cellsFiles = 0;
    for (const Sample& s : list(kSamples))
        for (const auto& e : fs::directory_iterator(s.path / "layers"))
            if (e.path().extension() == ".cells") {
                INFO(e.path().string());
                const std::string data = bytes(e.path());
                CHECK(data == project::encodeCells(project::decodeCells(data), project::CellsFrame::Raw));
                ++cellsFiles;
            }
    CHECK(cellsFiles > 0);
    // the showcase's reference sketch is maker/sketch.png as it is, not a
    // picture encoded on the machine that made the samples
    const project::Project showcase = project::read(kSamples / "01-layers-showcase");
    int references = 0;
    for (std::size_t i = 0; i < showcase.layers.size(); ++i)
        if (const auto& ref = showcase.layers[i].props.reference) {
            CHECK(*ref->bytes == bytes(kSamples / "maker" / "sketch.png"));
            ++references;
        }
    CHECK(references == 1);
}

// #207's list of the layer capabilities the set must cover between them.
TEST_CASE("the bundled samples show every layer capability between them") {
    std::set<LayerKind> kinds;
    std::set<LayerMode> modes;
    bool locked = false, alphaLocked = false, hidden = false, offCanvas = false, variant = false;
    bool smaller = false, offset = false, baked = false, orderMatters = false, upperWins = false;
    for (const Sample& s : list(kSamples)) {
        const project::Project p = project::read(s.path);
        const Notes notes = parseNotes(p.notes);
        variant = variant || !notes.variants.empty();
        const auto& layers = p.layers.layers();
        for (const Layer& l : layers) {
            kinds.insert(l.props.kind);
            modes.insert(l.props.mode);
            locked = locked || l.props.locked;
            alphaLocked = alphaLocked || l.props.alphaLocked;
            hidden = hidden || !l.props.visible;
            offCanvas = offCanvas || l.props.x < 0 || l.props.x + l.cells.width() > p.layers.width();
            if (l.props.kind == LayerKind::Reference || l.cells.width() == 0)
                continue;
            // inside the canvas and smaller than it both ways
            smaller = smaller || (insideCanvas(l, p.layers) && l.cells.width() < p.layers.width() &&
                                  l.cells.height() < p.layers.height());
            // placed by its offset, not at the corner, and all of it on the canvas
            offset = offset || (insideCanvas(l, p.layers) && l.props.x > 0 && l.props.y > 0);
            // a text or generator layer with a cells layer beside it that is its bake
            if (l.props.kind == LayerKind::Text || l.props.kind == LayerKind::Generator) {
                const Layer b = bake(l, p.ice);
                for (const Layer& other : layers)
                    baked = baked || (other.props.id != l.props.id && other.props.kind == LayerKind::Cells &&
                                      other.props.x == l.props.x && other.props.y == l.props.y && other.cells == b.cells);
            }
        }
        // a variant that only reorders layers, and so changes the piece
        const Canvas saved = composite(p.layers, {p.ice, Cell{}});
        for (const Variant& v : notes.variants) {
            const bool reorders = !v.changes.empty() && std::all_of(v.changes.begin(), v.changes.end(), [](const std::string& c) {
                return c.rfind("raise ", 0) == 0 || c.rfind("lower ", 0) == 0;
            });
            if (!reorders)
                continue;
            LayerStack moved = p.layers;
            applyVariant(moved, v);
            orderMatters = orderMatters || composite(moved, {p.ice, Cell{}}) != saved;
        }
        upperWins = upperWins || upperLayerWins(p);
    }
    CHECK(kinds == std::set<LayerKind>{LayerKind::Cells, LayerKind::Pixels, LayerKind::Text, LayerKind::Generator,
                                       LayerKind::Reference});
    CHECK(modes == std::set<LayerMode>{LayerMode::Normal, LayerMode::CharsOnly, LayerMode::ColoursOnly});
    CHECK(locked);
    CHECK(alphaLocked);
    CHECK(hidden);
    CHECK(offCanvas);
    CHECK(smaller);
    CHECK(offset);
    CHECK(baked);
    CHECK(variant);
    CHECK(orderMatters);
    CHECK(upperWins);
}

// #207's second sample: a logo built on the text-style system (PLAN §15).
TEST_CASE("the text-style logo sample holds big styled text, its saved stacks and a baked copy") {
    const fs::path dir = kSamples / "02-text-style-logo";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    CHECK(names(p.layers) == std::vector<std::string>{"backdrop", "sparks", "EMBER (baked)", "EMBER (quenched)",
                                                      "KILN (quenched)", "EMBER", "KILN", "tagline"});
    CHECK(p.snapshots.size() == 2);

    // the saved stacks ship with it, as library text styles
    const auto stack = [&](const std::string& file) {
        const std::string path = "styles/" + file + ".style.json";
        REQUIRE(p.files.count(path) == 1);
        const library::TextStyleEntry entry = library::parseTextStyleEntry(p.files.at(path));
        // in the form Save as Preset… writes, so one saved back is the same bytes
        CHECK(library::textStyleEntryJson(entry) == p.files.at(path));
        // marked as needing iCE exactly when its stack does (#310)
        CHECK(styleNeedsIce(entry.style) == entry.iceRequired);
        return entry.style;
    };
    const TextStyle molten = stack("molten"), quenched = stack("quenched"), tag = stack("kiln-tag");

    // every text layer has its style; the words are big text
    for (const Layer& l : p.layers.layers()) {
        if (l.props.kind != LayerKind::Text)
            continue;
        INFO(l.props.name);
        REQUIRE(l.props.text.has_value());
        REQUIRE(l.props.style.has_value());
        CHECK_FALSE(l.props.style->effects.empty());
        if (l.props.name != "tagline")
            CHECK(l.props.text->bigStyle == BigTextStyle::Block);
    }
    // one stack on two layers, and a second on the hidden copies
    CHECK(named(p.layers, "EMBER").props.style == molten);
    CHECK(named(p.layers, "KILN").props.style == molten);
    CHECK(named(p.layers, "EMBER (quenched)").props.style == quenched);
    CHECK(named(p.layers, "KILN (quenched)").props.style == quenched);
    CHECK(named(p.layers, "tagline").props.style == tag);
    CHECK_FALSE(named(p.layers, "EMBER (quenched)").props.visible);
    CHECK_FALSE(named(p.layers, "KILN (quenched)").props.visible);
    // what the effects a logo uses come to between them
    REQUIRE(molten.get<ShadowEffect>() != nullptr);
    CHECK(molten.get<ShadowEffect>()->form == ShadowForm::Extrude);
    CHECK(molten.get<FillEffect>() != nullptr);
    CHECK(molten.get<ShadeEffect>() != nullptr);
    CHECK(quenched.get<ShadeEffect>() != nullptr);
    CHECK(tag.get<PlateEffect>() != nullptr);

    // the baked copy is EMBER's bake, hidden, in the same place
    const Layer& ember = named(p.layers, "EMBER");
    const Layer& baked = named(p.layers, "EMBER (baked)");
    const Layer b = bake(ember, p.ice);
    CHECK(baked.props.kind == LayerKind::Cells);
    CHECK_FALSE(baked.props.visible);
    CHECK(baked.props.x == ember.props.x);
    CHECK(baked.props.y == ember.props.y);
    CHECK(baked.cells == b.cells);

    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("logo-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "logo.bwp");
    CHECK(project::read(out / "logo.bwp") == p);
}

TEST_CASE("the still life sample keeps each object, its shadows and its rim light on layers of their own") {
    const fs::path dir = kSamples / "03-still-life";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    REQUIRE(names(p.layers) ==
            std::vector<std::string>{"wall", "table", "shadows", "cylinder", "box", "sphere", "rim light"});
    for (const Layer& l : p.layers.layers()) {
        INFO(l.props.name);
        CHECK(l.props.kind == LayerKind::Cells);
        CHECK(l.props.visible);
    }
    CHECK(named(p.layers, "wall").props.locked);
    CHECK(named(p.layers, "table").props.locked);

    // each object shaded inside its own shape: alpha-locked, on a layer
    // smaller than the canvas, placed by its offset
    for (const char* name : {"cylinder", "box", "sphere"}) {
        INFO(name);
        const Layer& l = named(p.layers, name);
        CHECK(l.props.alphaLocked);
        CHECK_FALSE(l.props.locked);
        CHECK(l.cells.width() < p.layers.width());
        CHECK(l.cells.height() < p.layers.height());
        CHECK((l.props.x > 0 && l.props.y > 0));
    }

    // a cell of a layer at canvas cell x,y, if it holds one there
    const auto opaqueAt = [](const Layer& l, int x, int y) {
        const int lx = x - l.props.x, ly = y - l.props.y;
        return lx >= 0 && ly >= 0 && lx < l.cells.width() && ly < l.cells.height() && !l.cells.at(lx, ly).transparent;
    };
    // the rim light lies over the objects only, and the shadows on the table
    const Layer& rim = named(p.layers, "rim light");
    const Layer& table = named(p.layers, "table");
    int rimCells = 0, shadowCells = 0;
    for (int y = 0; y < p.layers.height(); ++y)
        for (int x = 0; x < p.layers.width(); ++x) {
            INFO(x << "," << y);
            if (opaqueAt(rim, x, y)) {
                ++rimCells;
                CHECK((opaqueAt(named(p.layers, "cylinder"), x, y) || opaqueAt(named(p.layers, "box"), x, y) ||
                       opaqueAt(named(p.layers, "sphere"), x, y)));
            }
            if (opaqueAt(named(p.layers, "shadows"), x, y)) {
                ++shadowCells;
                CHECK(opaqueAt(table, x, y));
            }
        }
    CHECK(rimCells > 0);
    CHECK(shadowCells > 0);

    // turning either off changes the picture
    const Canvas whole = project::toAnsi(p).canvas;
    for (const char* name : {"rim light", "shadows"}) {
        INFO(name);
        project::Project off = p;
        REQUIRE_NOTHROW(applyVariant(off.layers, {"off", {"hide " + std::string(name)}}));
        CHECK_FALSE(project::toAnsi(off).canvas == whole);
    }

    // three rounds: flat, shaded, lit
    REQUIRE(p.snapshots.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(p.snapshots[std::size_t(i)].round == i + 1);
    CHECK(p.snapshots[0].ans != p.snapshots[1].ans);
    CHECK(p.snapshots[1].ans != p.snapshots[2].ans);

    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("still-life-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "still-life.bwp");
    CHECK(project::read(out / "still-life.bwp") == p);
}

TEST_CASE("the generator sample keeps its generators, their parameters and a baked copy under cells layers") {
    const fs::path dir = kSamples / "04-generator-ridges";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    REQUIRE(names(p.layers) == std::vector<std::string>{"sky", "stars", "ridges", "ridges (seed 12)", "ridges (baked)",
                                                        "lake", "shore", "pines", "cabin"});
    const auto kindOf = [&](const char* name) { return named(p.layers, name).props.kind; };
    CHECK(kindOf("sky") == LayerKind::Cells);
    CHECK(kindOf("stars") == LayerKind::Generator);
    CHECK(kindOf("ridges") == LayerKind::Generator);
    CHECK(kindOf("ridges (seed 12)") == LayerKind::Generator);
    CHECK(kindOf("ridges (baked)") == LayerKind::Cells);
    CHECK(kindOf("lake") == LayerKind::Cells);
    CHECK(kindOf("shore") == LayerKind::Cells);
    CHECK(kindOf("pines") == LayerKind::Pixels);
    CHECK(kindOf("cabin") == LayerKind::Pixels);
    CHECK(named(p.layers, "stars").props.mode == LayerMode::CharsOnly);
    for (const char* name : {"sky", "lake", "shore"})
        CHECK(named(p.layers, name).props.locked);
    for (const Layer& l : p.layers.layers()) {
        INFO(l.props.name);
        const bool hidden = l.props.name == "ridges (seed 12)" || l.props.name == "ridges (baked)";
        CHECK(l.props.visible != hidden);
    }

    // each generator ships inside the project, run by a relative command,
    // with the parameters it was left at
    const auto params = [&](const char* name) {
        const LayerId id = named(p.layers, name).props.id;
        const std::string path = generator::directory(id) + std::string(generator::kParamsFile);
        return generator::paramValues(generator::layerSpec(p, id), p.files.count(path) ? p.files.at(path) : "");
    };
    for (const char* name : {"stars", "ridges", "ridges (seed 12)"}) {
        INFO(name);
        const generator::Spec spec = generator::layerSpec(p, named(p.layers, name).props.id);
        REQUIRE(spec.command.size() == 2);
        CHECK(spec.command[0] == "sh");
        CHECK(spec.command[1].find('/') == std::string::npos);
        CHECK(p.files.count(generator::directory(named(p.layers, name).props.id) + spec.command[1]) == 1);
        CHECK(spec.key == std::optional<std::uint8_t>(0));
        CHECK_FALSE(spec.params.empty());
    }
    auto ridges = params("ridges");
    CHECK(ridges.at("seed") == generator::ParamValue(3.0));
    CHECK(ridges.at("ridges") == generator::ParamValue(3.0));
    CHECK(ridges.at("roughness") == generator::ParamValue(0.3));
    CHECK(ridges.at("sharp") == generator::ParamValue(false));
    CHECK(ridges.at("top") == generator::ParamValue(18.0));
    auto other = params("ridges (seed 12)");
    CHECK(other.at("seed") == generator::ParamValue(12.0));
    CHECK(other.at("ridges") == generator::ParamValue(4.0));
    CHECK(other.at("sharp") == generator::ParamValue(true));

    // the generated ridges sit on the glow and are darker than it, in no
    // colour the sky has, and each nearer ridge is darker than the one
    // behind it, as haze makes them: down every column a cell is as dark as
    // the one above it, or darker where a nearer ridge starts
    const Layer& sky = named(p.layers, "sky");
    const double glow = value(Cell{U'\u2588', Color::indexed(LightRed), Color::indexed(Black)});
    CHECK(value(sky.cells.at(0, sky.cells.height() - 1)) == glow);
    std::set<std::tuple<std::uint32_t, int, int>> skyLooks;
    for (int y = 0; y < sky.cells.height(); ++y)
        for (int x = 0; x < sky.cells.width(); ++x) {
            const Cell& s = sky.cells.at(x, y);
            skyLooks.insert({s.ch.bits(), s.fg.paletteIndex(), s.bg.paletteIndex()});
        }
    const Layer& ridgeLayer = named(p.layers, "ridges");
    for (const char* name : {"ridges", "ridges (seed 12)"}) {
        INFO(name);
        const Layer& l = named(p.layers, name);
        int ridgeCells = 0;
        for (int x = 0; x < l.cells.width(); ++x) {
            const Cell* above = nullptr;
            for (int y = 0; y < l.cells.height(); ++y) {
                const Cell& c = l.cells.at(x, y);
                if (c.transparent)
                    continue;
                INFO(x << "," << y);
                ++ridgeCells;
                CHECK(y >= 18);
                CHECK(value(c) < glow);
                CHECK(skyLooks.count({c.ch.bits(), c.fg.paletteIndex(), c.bg.paletteIndex()}) == 0);
                if (above)
                    CHECK((*above == c ? value(c) == value(*above) : value(c) < value(*above)));
                above = &c;
            }
        }
        CHECK(ridgeCells > 0);
    }

    // the baked copy is the ridges layer's cells, with no generator behind it
    const Layer& baked = named(p.layers, "ridges (baked)");
    CHECK(baked.cells == bake(ridgeLayer, p.ice).cells);
    CHECK_FALSE(named(p.layers, "ridges (seed 12)").cells == ridgeLayer.cells);

#ifndef _WIN32
    // run again, each generator draws the cells it saved
    struct sigaction old {};
    REQUIRE(::sigaction(SIGCHLD, nullptr, &old) == 0);
    struct Restore {
        struct sigaction old;
        ~Restore() { ::sigaction(SIGCHLD, &old, nullptr); }
    } restore{old};
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    REQUIRE(::sigaction(SIGCHLD, &dfl, nullptr) == 0);
    for (const char* name : {"stars", "ridges", "ridges (seed 12)"}) {
        INFO(name);
        const Layer& l = named(p.layers, name);
        CHECK(generator::run(p, l.props.id) == l.cells);
    }
    // gawk looks for `-f` scripts on AWKPATH before the working directory;
    // the scripts name theirs as ./, so a decoy of the same name there is
    // never run
    {
        const fs::path decoys = scratch("generator-awkpath");
        for (const char* file : {"ridges.awk", "stars.awk"})
            std::ofstream(decoys / file) << "BEGIN { print \"decoy\"; exit }\n";
        const EnvGuard awkpath("AWKPATH", decoys.string());
        for (const char* name : {"stars", "ridges", "ridges (seed 12)"}) {
            INFO(name << ", AWKPATH holding decoys");
            const Layer& l = named(p.layers, name);
            CHECK(generator::run(p, l.props.id) == l.cells);
        }
    }
#endif

    // the variants: the baked copy looks as the ridges do, the others change
    // the picture
    const Canvas whole = project::toAnsi(p).canvas;
    const Notes notes = parseNotes(p.notes);
    REQUIRE(notes.variants.size() == 4);
    for (const Variant& v : notes.variants) {
        INFO(v.name);
        project::Project changed = p;
        REQUIRE_NOTHROW(applyVariant(changed.layers, v));
        CHECK((project::toAnsi(changed).canvas == whole) == (v.name == "Baked"));
    }

    // three rounds: two smooth ridges, the parameters kept, finished
    REQUIRE(p.snapshots.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(p.snapshots[std::size_t(i)].round == i + 1);
    CHECK(p.snapshots[0].ans != p.snapshots[1].ans);
    CHECK(p.snapshots[1].ans != p.snapshots[2].ans);

    const fs::path out = scratch("generator-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "ridges.bwp");
    CHECK(project::read(out / "ridges.bwp") == p);
}

TEST_CASE("the generator sample's pines run unbroken from tip to crown") {
    const project::Project p = project::read(kSamples / "04-generator-ridges");
    const Layer& pines = named(p.layers, "pines");
    const int w = pines.cells.width(), h = 2 * pines.cells.height();
    const auto pixel = [&](int x, int py) -> std::optional<std::uint8_t> {
        if (x < 0 || x >= w || py < 0 || py >= h)
            return std::nullopt;
        const LayerPixels px = layerPixels(pines.cells.at(x, py / 2));
        return py % 2 == 1 ? px.bottom : px.top;
    };
    // a tip is the top pixel of its column, with nothing beside it; down
    // from it, until the crown widens, every pixel is the tree's green
    int tips = 0;
    for (int x = 0; x < w; ++x) {
        int py = 0;
        while (py < h && !pixel(x, py))
            ++py;
        if (py == h || pixel(x - 1, py) || pixel(x + 1, py))
            continue;
        ++tips;
        for (; py < h && pixel(x, py) && !pixel(x - 1, py) && !pixel(x + 1, py); ++py) {
            INFO("pixel " << x << "," << py << " (cell " << x << "," << pines.props.y + py / 2 << ")");
            CHECK(pixel(x, py) == std::optional<std::uint8_t>(Green));
        }
        INFO("the crown under the tip at column " << x);
        CHECK(pixel(x, py).has_value());
    }
    CHECK(tips == 6);
}

TEST_CASE("the generator sample's overlapping pines keep a lit edge between them") {
    // A pine in front of another hides its lit side with its own shaded
    // one; the one behind keeps a lit edge against it, so the two shades
    // don't run together (#363's review: at cell 74,29 they did). One pine's
    // shaded side is at most six pixels wide, so no run of black in the
    // boughs is wider, but in the lowest two rows of them, where the edge
    // stops.
    const project::Project p = project::read(kSamples / "04-generator-ridges");
    const Layer& pines = named(p.layers, "pines");
    const int w = pines.cells.width(), h = 2 * pines.cells.height();
    const auto pixel = [&](int x, int py) {
        const LayerPixels px = layerPixels(pines.cells.at(x, py / 2));
        return py % 2 == 1 ? px.bottom : px.top;
    };
    int lowest = -1;
    for (int py = 0; py < h; ++py)
        for (int x = 0; x < w; ++x)
            if (pixel(x, py) == std::optional<std::uint8_t>(Black))
                lowest = py;
    REQUIRE(lowest > 0);
    for (int py = 0; py < lowest - 1; ++py) {
        int run = 0;
        for (int x = 0; x < w; ++x) {
            run = pixel(x, py) == std::optional<std::uint8_t>(Black) ? run + 1 : 0;
            INFO("pixel " << x << "," << py << " (cell " << x << "," << pines.props.y + py / 2 << ")");
            CHECK(run <= 6);
        }
    }
    // the edge the review's cell lost: pine 72's, beside pine 77's shade
    CHECK(layerPixels(pines.cells.at(72, 29 - pines.props.y)).top == std::optional<std::uint8_t>(Green));
}

TEST_CASE("the character sample keeps its figure on layers of its own, one size and in one place, over a locked scene") {
    const fs::path dir = kSamples / "05-character";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    const std::vector<std::string> scene{"sky", "hills", "meadow"};
    const std::vector<std::string> parts{"shadow", "outline", "body", "face detail", "highlights"};
    REQUIRE(names(p.layers) == std::vector<std::string>{"sky", "hills", "meadow", "shadow", "outline", "body",
                                                        "face detail", "highlights", "figure (merged)"});
    for (const std::string& name : scene) {
        INFO(name);
        const Layer& l = named(p.layers, name);
        CHECK(l.props.kind == LayerKind::Cells);
        CHECK(l.props.locked);
        CHECK(l.props.visible);
    }

    // the figure's layers: pixels, one size and in one place, smaller than
    // the canvas and placed by their offset, so moving each by the same
    // distance moves the figure whole
    const Layer& merged = named(p.layers, "figure (merged)");
    CHECK_FALSE(merged.props.visible);
    CHECK(merged.cells.width() < p.layers.width());
    CHECK(merged.cells.height() < p.layers.height());
    CHECK((merged.props.x > 0 && merged.props.y > 0));
    CHECK(insideCanvas(merged, p.layers));
    for (const std::string& name : parts) {
        INFO(name);
        const Layer& l = named(p.layers, name);
        CHECK(l.props.kind == LayerKind::Pixels);
        CHECK(l.props.visible);
        CHECK_FALSE(l.props.locked);
        CHECK(l.props.alphaLocked == (name == "body"));
        CHECK(l.props.x == merged.props.x);
        CHECK(l.props.y == merged.props.y);
        CHECK(l.cells.width() == merged.cells.width());
        CHECK(l.cells.height() == merged.cells.height());
    }

    // pixel x,py of a figure layer, in the layer
    const auto pixel = [](const Layer& l, int x, int py) {
        const LayerPixels px = layerPixels(l.cells.at(x, py / 2));
        return py % 2 == 1 ? px.bottom : px.top;
    };
    const int w = merged.cells.width(), h = 2 * merged.cells.height();
    const Layer& outline = named(p.layers, "outline");
    const Layer& body = named(p.layers, "body");
    int ring = 0, detail = 0, lights = 0;
    for (int py = 0; py < h; ++py)
        for (int x = 0; x < w; ++x) {
            INFO("pixel " << x << "," << py);
            // the merged copy is the five parts, each over the ones under it
            std::optional<std::uint8_t> top;
            for (const std::string& name : parts)
                if (const auto c = pixel(named(p.layers, name), x, py))
                    top = c;
            CHECK(pixel(merged, x, py) == top);
            // the outline is black, under the whole figure and a pixel round it
            const auto o = pixel(outline, x, py);
            CHECK((!o || *o == Black));
            if (pixel(body, x, py))
                CHECK(o.has_value());
            else if (o)
                ++ring;
            // the face and the highlights lie on the body
            if (pixel(named(p.layers, "face detail"), x, py)) {
                ++detail;
                CHECK(pixel(body, x, py).has_value());
            }
            if (pixel(named(p.layers, "highlights"), x, py)) {
                ++lights;
                CHECK(pixel(body, x, py).has_value());
            }
        }
    CHECK(ring > 0);
    CHECK(detail > 0);
    CHECK(lights > 0);

    // the variants: the merged copy looks as the parts do, the others change
    // the picture
    const Canvas whole = project::toAnsi(p).canvas;
    const Notes notes = parseNotes(p.notes);
    REQUIRE(notes.variants.size() == 5);
    for (const Variant& v : notes.variants) {
        INFO(v.name);
        project::Project changed = p;
        REQUIRE_NOTHROW(applyVariant(changed.layers, v));
        CHECK((project::toAnsi(changed).canvas == whole) == (v.name == "One layer"));
    }

    // moved as Alt+drag moves a layer, the merged copy alone or each part by
    // the same distance, to anywhere on the canvas the figure's layers fit
    // and its boots stay on the meadow: the same picture either way, and
    // the whole piece lints clean wherever the figure stands, the figure
    // and the scene round it (a tuft in the grass a cell past the tip of
    // the shadow included).
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -merged.props.x; merged.props.x + dx + merged.cells.width() <= p.layers.width(); ++dx) {
            INFO("moved by " << dx << "," << dy);
            project::Project one = p;
            REQUIRE_NOTHROW(applyVariant(one.layers, {"One layer", {"show figure (merged)", "hide highlights",
                                                                    "hide face detail", "hide body", "hide outline",
                                                                    "hide shadow"}}));
            project::Project each = p;
            for (std::size_t i = 0; i < p.layers.size(); ++i) {
                const std::string& name = p.layers[i].props.name;
                if (name == "figure (merged)")
                    one.layers[i].props.x += dx, one.layers[i].props.y += dy;
                else if (std::find(parts.begin(), parts.end(), name) != parts.end())
                    each.layers[i].props.x += dx, each.layers[i].props.y += dy;
            }
            const Layer& moved = named(each.layers, "body");
            REQUIRE(insideCanvas(moved, each.layers));
            const AnsiDocument doc = project::toAnsi(each);
            CHECK(project::toAnsi(one).canvas == doc.canvas);
            CHECK((doc.canvas == whole) == (dx == 0 && dy == 0));
            const lint::LintReport r = bwcli::lintDocument(doc, {});
            INFO(bwcli::lintText(r));
            CHECK(r.findings.empty());
            CHECK(irc::overBudget(irc::encodeCanvas(doc.canvas), p.ircBudget.bytes).empty());
        }

    // three rounds: the scene, the figure in flat colours, finished
    REQUIRE(p.snapshots.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(p.snapshots[std::size_t(i)].round == i + 1);
    CHECK(p.snapshots[0].ans != p.snapshots[1].ans);
    CHECK(p.snapshots[1].ans != p.snapshots[2].ans);

    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("character-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "character.bwp");
    CHECK(project::read(out / "character.bwp") == p);
}

TEST_CASE("the logo sample takes the program's logo apart into the layers a block-letter logo is made of") {
    const fs::path dir = kSamples / "06-blockwright-logo";
    const project::Project p = project::read(dir);
    CHECK_FALSE(p.ice);
    REQUIRE(names(p.layers) ==
            std::vector<std::string>{"backdrop", "white blocks", "gold block", "drop", "letters", "shading"});
    for (std::size_t i = 0; i < p.layers.size(); ++i)
        CHECK(p.layers[i].props.visible);

    const Layer& backdrop = named(p.layers, "backdrop");
    CHECK(backdrop.props.kind == LayerKind::Cells);
    CHECK(backdrop.props.locked);
    CHECK(backdrop.cells.width() == p.layers.width());
    CHECK(backdrop.cells.height() == p.layers.height());

    // the mark's blocks: cells layers smaller than the canvas, placed by
    // their offset
    for (const char* name : {"white blocks", "gold block"}) {
        INFO(name);
        const Layer& l = named(p.layers, name);
        CHECK(l.props.kind == LayerKind::Cells);
        CHECK_FALSE(l.props.locked);
        CHECK(l.cells.width() < p.layers.width());
        CHECK(l.cells.height() < p.layers.height());
        CHECK((l.props.x > 0 && l.props.y > 0));
        CHECK(insideCanvas(l, p.layers));
    }

    // the wordmark's three layers, one size and in one place
    const Layer& letters = named(p.layers, "letters");
    const Layer& drop = named(p.layers, "drop");
    const Layer& shading = named(p.layers, "shading");
    CHECK(letters.props.kind == LayerKind::Pixels);
    CHECK(drop.props.kind == LayerKind::Pixels);
    CHECK(shading.props.kind == LayerKind::Cells);
    CHECK(letters.props.alphaLocked);
    CHECK_FALSE(drop.props.alphaLocked);
    CHECK_FALSE(shading.props.alphaLocked);
    CHECK(insideCanvas(letters, p.layers));
    for (const Layer* l : {&drop, &shading}) {
        INFO(l->props.name);
        CHECK_FALSE(l->props.locked);
        CHECK(l->props.x == letters.props.x);
        CHECK(l->props.y == letters.props.y);
        CHECK(l->cells.width() == letters.cells.width());
        CHECK(l->cells.height() == letters.cells.height());
    }

    const auto pixel = [](const Layer& l, int x, int py) -> std::optional<std::uint8_t> {
        if (x < 0 || py < 0 || x >= l.cells.width() || py >= 2 * l.cells.height())
            return std::nullopt;
        const LayerPixels px = layerPixels(l.cells.at(x, py / 2));
        return py % 2 == 1 ? px.bottom : px.top;
    };
    // Each letter pixel's colour as the design gives it, not as the layer
    // has it: under a shade cell, the colour the shade mostly shows, ▓ and
    // ▒ light cyan and ░ cyan (the letter goes dark at the ░ rung, so with
    // the shading hidden it still steps from light to dark); elsewhere
    // light cyan, and cyan at the foot, the last cell row of each line of
    // lettering (BLOCK in rows 0 to 5, WRIGHT in rows 8 to 13). The shade
    // cells themselves are held to the shipped logo below, so this pins
    // what r2 shows under them.
    const auto designColour = [&shading](int x, int py) -> std::uint8_t {
        const Cell& s = shading.cells.at(x, py / 2);
        if (!s.transparent)
            return s.ch == U'░' ? Cyan : LightCyan;
        return py / 2 == 5 || py / 2 == 13 ? Cyan : LightCyan;
    };
    int rim = 0, shades = 0, under[2] = {0, 0};
    for (int py = 0; py < 2 * letters.cells.height(); ++py)
        for (int x = 0; x < letters.cells.width(); ++x) {
            INFO("pixel " << x << "," << py);
            // the letters in two flat colours, each where the design puts it
            if (const auto c = pixel(letters, x, py)) {
                CHECK(int(*c) == int(designColour(x, py)));
                if (!shading.cells.at(x, py / 2).transparent)
                    ++under[*c == Cyan];
            }
            // the drop is blue and the letters moved one column right and
            // one pixel down; where the letters don't cover it, it shows
            if (const auto d = pixel(drop, x, py)) {
                CHECK(*d == Blue);
                CHECK(pixel(letters, x - 1, py - 1).has_value());
                if (!pixel(letters, x, py))
                    ++rim;
            }
        }
    for (int y = 0; y < shading.cells.height(); ++y)
        for (int x = 0; x < shading.cells.width(); ++x) {
            const Cell& c = shading.cells.at(x, y);
            if (c.transparent)
                continue;
            INFO("cell " << x << "," << y);
            // the shade cells, ▓ ▒ ░ in light cyan on cyan, lie on whole cells
            // of the letters
            ++shades;
            CHECK((c.ch == U'▓' || c.ch == U'▒' || c.ch == U'░'));
            CHECK(c.fg == Color::indexed(LightCyan));
            CHECK(c.bg == Color::indexed(Cyan));
            CHECK(pixel(letters, x, 2 * y).has_value());
            CHECK(pixel(letters, x, 2 * y + 1).has_value());
        }
    CHECK(rim > 0);
    CHECK(shades > 0);
    // letters under the shading in both colours, so neither rule is empty
    CHECK(under[0] > 0);
    CHECK(under[1] > 0);

    // shown together they are the program's own logo, colour for colour: a
    // half-block, block or blank cell by the two colours it shows, every
    // other cell as it is
    const Canvas whole = project::toAnsi(p).canvas;
    const project::Project brand = project::read(fs::path(BW_BRANDING_DIR) / "logo-large.bwp");
    const Canvas logo = project::toAnsi(brand).canvas;
    REQUIRE(whole.width() == logo.width());
    REQUIRE(whole.height() == logo.height());
    REQUIRE(p.sauce);
    REQUIRE(brand.sauce);
    CHECK(p.sauce->title == brand.sauce->title);
    CHECK(p.ice == brand.ice);
    for (int y = 0; y < logo.height(); ++y)
        for (int x = 0; x < logo.width(); ++x) {
            INFO("cell " << x << "," << y);
            const auto a = halfBlocks(whole.at(x, y), false), b = halfBlocks(logo.at(x, y), false);
            if (a && b)
                CHECK(*a == *b);
            else
                CHECK(whole.at(x, y) == logo.at(x, y));
        }

    // the variants, each by the layers it shows, all six of them: the mark
    // and the wordmark each stay as they are in the variant that keeps them,
    // and the side it drops shows only the backdrop
    const auto shown = [](const LayerStack& s) {
        std::set<std::string> out;
        for (const Layer& l : s.layers())
            if (l.props.visible)
                out.insert(l.props.name);
        return out;
    };
    const std::map<std::string, std::set<std::string>> variantShows = {
        {"No shading", {"backdrop", "white blocks", "gold block", "drop", "letters"}},
        {"No drop", {"backdrop", "white blocks", "gold block", "letters", "shading"}},
        {"Wordmark only", {"backdrop", "drop", "letters", "shading"}},
        {"Mark only", {"backdrop", "white blocks", "gold block"}},
    };
    const Canvas bare = [&] {
        project::Project b = p;
        for (std::size_t i = 0; i < b.layers.size(); ++i)
            b.layers[i].props.visible = b.layers[i].props.name == "backdrop";
        return project::toAnsi(b).canvas;
    }();
    const Notes notes = parseNotes(p.notes);
    // exactly the four, each once, so a repeated name can't stand in for
    // one of the "only" variants and skip its check
    std::vector<std::string> variantNames;
    for (const Variant& v : notes.variants)
        variantNames.push_back(v.name);
    std::sort(variantNames.begin(), variantNames.end());
    std::vector<std::string> expectedNames;
    for (const auto& [name, layers] : variantShows)
        expectedNames.push_back(name);
    REQUIRE(variantNames == expectedNames);
    const int split = letters.props.x - 1;  // the mark is left of it, the wordmark from it on
    for (const Variant& v : notes.variants) {
        INFO(v.name);
        REQUIRE(variantShows.count(v.name) == 1);
        project::Project changed = p;
        REQUIRE_NOTHROW(applyVariant(changed.layers, v));
        CHECK(shown(changed.layers) == variantShows.at(v.name));
        const Canvas c = project::toAnsi(changed).canvas;
        CHECK(c != whole);
        if (v.name != "Mark only" && v.name != "Wordmark only")
            continue;
        const bool markKept = v.name == "Mark only";
        int kept = 0, cleared = 0;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x) {
                INFO("cell " << x << "," << y);
                if ((x < split) == markKept) {
                    CHECK(c.at(x, y) == whole.at(x, y));
                    kept += whole.at(x, y) != bare.at(x, y);
                } else {
                    CHECK(c.at(x, y) == bare.at(x, y));
                    cleared += whole.at(x, y) != bare.at(x, y);
                }
            }
        // both sides hold something over the backdrop, so neither check is
        // empty
        CHECK(kept > 0);
        CHECK(cleared > 0);
    }

    // three rounds, each decoded and held to its stage as the layers make
    // it: the mark and the letters in flat light cyan; the letters
    // going dark at the foot with the drop under them; the finished logo
    REQUIRE(p.snapshots.size() == 3);
    for (int i = 0; i < 3; ++i)
        CHECK(p.snapshots[std::size_t(i)].round == i + 1);
    project::Project blockedIn = p, darkFoot = p;
    for (project::Project* stage : {&blockedIn, &darkFoot})
        for (std::size_t i = 0; i < stage->layers.size(); ++i) {
            Layer& l = stage->layers[i];
            if (l.props.name == "shading" || (stage == &blockedIn && l.props.name == "drop"))
                l.props.visible = false;
            if (stage == &blockedIn && l.props.name == "letters")
                for (int py = 0; py < 2 * l.cells.height(); ++py)
                    for (int x = 0; x < l.cells.width(); ++x)
                        if (pixel(l, x, py))
                            l.cells.at(x, py / 2) = setLayerPixel(l.cells.at(x, py / 2), py % 2 == 1, LightCyan);
        }
    const std::vector<Canvas> stages = {project::toAnsi(blockedIn).canvas, project::toAnsi(darkFoot).canvas, whole};
    CHECK(stages[0] != stages[1]);
    CHECK(stages[1] != stages[2]);
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("r" << i + 1);
        const AnsiDocument snap = parseAnsi(p.snapshots[i].ans);
        CHECK(snap.canvas == stages[i]);
    }

    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("brand-logo-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "brand-logo.bwp");
    CHECK(project::read(out / "brand-logo.bwp") == p);
}
