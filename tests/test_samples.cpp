// The sample projects (PLAN §12, samples.h): their notes' format, the
// variants a sample's notes describe, the list of samples in a folder, and
// the bundled samples themselves, held to the bar the Library's stamps are.

#include "blockwright/brush.h"
#include "blockwright/brushes/form.h"
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
#include "util.h"

#include <QByteArray>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#ifndef _WIN32
#include <csignal>
#endif
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
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

// A stack's layers bottom first, each with whether it shows: what a variant
// leaves, its order and its visibility both.
using StackState = std::vector<std::pair<std::string, bool>>;

StackState stackState(const LayerStack& s) {
    StackState out;
    for (const Layer& l : s.layers())
        out.emplace_back(l.props.name, l.props.visible);
    return out;
}

// The layers in `order`, bottom first, all shown but the `hidden` ones.
StackState stackState(const std::vector<std::string>& order, const std::set<std::string>& hidden) {
    StackState out;
    for (const std::string& name : order)
        out.emplace_back(name, hidden.count(name) == 0);
    return out;
}

// A sample's variants are exactly the `expected` ones, each once, so a
// repeated name can't stand in for another and skip its check, and each
// leaves the stack in exactly the order and visibility given for it.
void checkVariants(const project::Project& p, const std::map<std::string, StackState>& expected) {
    const Notes notes = parseNotes(p.notes);
    std::vector<std::string> variantNames;
    for (const Variant& v : notes.variants)
        variantNames.push_back(v.name);
    std::sort(variantNames.begin(), variantNames.end());
    std::vector<std::string> expectedNames;
    for (const auto& [name, state] : expected)
        expectedNames.push_back(name);
    REQUIRE(variantNames == expectedNames);
    for (const Variant& v : notes.variants) {
        INFO(v.name);
        project::Project changed = p;
        REQUIRE_NOTHROW(applyVariant(changed.layers, v));
        CHECK(stackState(changed.layers) == expected.at(v.name));
    }
}

// The snapshots are rounds 1 to 3, each decoded and the canvas of its stage
// (a stage left empty, one this host can't make, is skipped).
void checkSnapshots(const project::Project& p, const std::vector<std::optional<Canvas>>& stages) {
    REQUIRE(stages.size() == 3);
    CHECK(stages[0] != stages[1]);
    CHECK(stages[1] != stages[2]);
    REQUIRE(p.snapshots.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("r" << i + 1);
        CHECK(p.snapshots[i].round == int(i) + 1);
        if (stages[i])
            CHECK(parseAnsi(p.snapshots[i].ans).canvas == *stages[i]);
    }
}

// The narrowest crest or trough of a ridge's top edge (its row in each
// column), in columns: a level run with both neighbouring runs below it or
// both above. A line with no crest or trough has none narrower than itself.
int narrowestTurn(const std::vector<int>& edge) {
    std::vector<std::pair<int, int>> runs;  // each level run's row and width
    for (int row : edge) {
        if (runs.empty() || runs.back().first != row)
            runs.emplace_back(row, 0);
        ++runs.back().second;
    }
    int narrowest = int(edge.size());
    for (std::size_t i = 1; i + 1 < runs.size(); ++i)
        if ((runs[i].first < runs[i - 1].first) == (runs[i].first < runs[i + 1].first))
            narrowest = std::min(narrowest, runs[i].second);
    return narrowest;
}

// The project with the layers named in `hidden` hidden, and nothing else.
project::Project hiding(const project::Project& p, const std::set<std::string>& hidden) {
    project::Project out = p;
    for (std::size_t i = 0; i < out.layers.size(); ++i)
        if (hidden.count(out.layers[i].props.name))
            out.layers[i].props.visible = false;
    return out;
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

// The second sample, a finished piece in fourteen layers (PLAN §12).
TEST_CASE("the DDOS KING sample keeps its layers, a locked backdrop, hidden variant layers and a snapshot a stage") {
    const fs::path dir = kSamples / "02-ddos-king";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    CHECK(p.layers.width() == 80);
    CHECK(p.layers.height() == 64);
    const std::vector<std::string> order{"backdrop", "margins", "throne",  "cables", "cable tips",
                                         "king",     "sceptre", "head",    "lightning", "finials",
                                         "packets",  "title",   "border",  "signature"};
    REQUIRE(names(p.layers) == order);

    // every layer is cells over the whole canvas; the backdrop alone is
    // locked, and only the two variant layers are hidden
    for (const Layer& l : p.layers.layers()) {
        INFO(l.props.name);
        CHECK(l.props.kind == LayerKind::Cells);
        CHECK(l.props.mode == LayerMode::Normal);
        CHECK(l.props.x == 0);
        CHECK(l.props.y == 0);
        CHECK(l.cells.width() == 80);
        CHECK(l.cells.height() == 64);
        CHECK(l.props.locked == (l.props.name == "backdrop"));
        CHECK_FALSE(l.props.alphaLocked);
        CHECK(l.props.visible == (l.props.name != "margins" && l.props.name != "cable tips"));
    }

    // twelve of them are the layers in maker/ddos-king/, cell for cell
    const project::Project maker = project::read(kSamples / "maker" / "ddos-king");
    REQUIRE(maker.layers.size() == 12);
    for (const Layer& b : maker.layers.layers()) {
        INFO(b.props.name);
        CHECK(named(p.layers, b.props.name).cells == b.cells);
    }
    CHECK(composite(p.layers, {p.ice, Cell{}}) == composite(maker.layers, {maker.ice, Cell{}}));

    // the hidden variant layers: black blocks over the frame's rows and last
    // column, and four half-block cable tips
    const Layer& margins = named(p.layers, "margins");
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 80; ++x) {
            const Cell& c = margins.cells.at(x, y);
            if (y < 2 || y >= 62 || x == 79) {
                CHECK(c.ch == U'\u2588');
                CHECK(c.fg.paletteIndex() == Black);
                CHECK(c.bg.paletteIndex() == Black);
            } else {
                CHECK(c.transparent);
            }
        }
    // each tip is the yellow of the cable run in the cell under it, whichever
    // layer holds the run there (the sceptre's cable at 12,36, not the cables
    // layer, which is clear there), as the No lightning variant shows it
    std::set<std::pair<int, int>> tipsAt;
    const Canvas& tipCells = named(p.layers, "cable tips").cells;
    LayerStack noLightning = p.layers;
    for (std::size_t i = 0; i < noLightning.size(); ++i) {
        const std::string& n = noLightning[i].props.name;
        noLightning[i].props.visible = n != "lightning" && n != "packets" && n != "margins";
    }
    const Canvas shown = composite(noLightning, {p.ice, Cell{}});
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 80; ++x)
            if (const Cell& c = tipCells.at(x, y); !c.transparent) {
                INFO("cable tip at " << x << "," << y);
                CHECK(c.ch == U'\u2584');
                CHECK(c.fg.paletteIndex() == Yellow);
                CHECK(c.bg.paletteIndex() == Blue);
                const auto run = halfBlocks(shown.at(x, y + 1), p.ice);
                REQUIRE(run);
                CHECK(run->top == Yellow);
                tipsAt.insert({x, y});
            }
    CHECK(tipsAt == std::set<std::pair<int, int>>{{12, 35}, {9, 41}, {7, 43}, {6, 44}});

    // the finials hold the gold balls and nothing of the bolts: the two cells
    // over the inner balls, where the bolts pass, are clear, so No lightning
    // leaves no white dot above them
    const Layer& finials = named(p.layers, "finials");
    for (const auto& [x, y] : {std::pair{31, 10}, std::pair{50, 10}}) {
        INFO("finials at " << x << "," << y);
        CHECK(finials.cells.at(x, y).transparent);
    }
    // the right bolt's glow runs edge to edge down its low side: the top
    // pixel at 62,18 is light cyan, not the blue showing through
    const Cell& glow = named(p.layers, "lightning").cells.at(62, 18);
    CHECK_FALSE(glow.transparent);
    const auto glowPixels = halfBlocks(glow, p.ice);
    REQUIRE(glowPixels);
    CHECK(glowPixels->top == LightCyan);

    // the variants, each by the order and visibility it leaves
    std::set<std::string> allButTitle(order.begin(), order.end());
    allButTitle.erase("title");
    allButTitle.erase("margins");
    checkVariants(p, {{"No lightning", stackState(order, {"margins", "lightning", "packets"})},
                      {"No border", stackState(order, {"cable tips", "border"})},
                      {"Title only", stackState(order, allButTitle)}});

    // the snapshots: the throne and its cables, the figure, finished
    const auto upTo = [&](const std::string& last) {
        LayerStack st(80, 64);
        for (const Layer& l : p.layers.layers()) {
            st.add(l);
            if (l.props.name == last)
                break;
        }
        return std::optional<Canvas>(composite(st, {p.ice, Cell{}}));
    };
    checkSnapshots(p, {upTo("cable tips"), upTo("head"), upTo("signature")});

    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("ddos-king-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "king.bwp");
    CHECK(project::read(out / "king.bwp") == p);
}

// The seventh sample, a finished piece whose light is a colours-only layer
// (PLAN §12).
TEST_CASE("the battlestation sample keeps its layers, its light on a colours-only layer and an alpha-locked screen") {
    const fs::path dir = kSamples / "07-battlestation";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    CHECK(p.layers.width() == 80);
    CHECK(p.layers.height() == 40);
    const std::vector<std::string> order{"wall",  "margins",       "desk", "monitor frame", "screen",   "keyboard",
                                         "mug and mouse", "rack", "glow", "chair", "signature"};
    REQUIRE(names(p.layers) == order);

    // every layer is cells over the whole canvas; the wall alone is locked,
    // the screen alone alpha-locked, the glow alone colours only, and only
    // the margins hidden
    for (const Layer& l : p.layers.layers()) {
        INFO(l.props.name);
        CHECK(l.props.kind == LayerKind::Cells);
        CHECK(l.props.mode == (l.props.name == "glow" ? LayerMode::ColoursOnly : LayerMode::Normal));
        CHECK(l.props.x == 0);
        CHECK(l.props.y == 0);
        CHECK(l.cells.width() == 80);
        CHECK(l.cells.height() == 40);
        CHECK(l.props.locked == (l.props.name == "wall"));
        CHECK(l.props.alphaLocked == (l.props.name == "screen"));
        CHECK(l.props.visible == (l.props.name != "margins"));
    }

    // six of the layers in maker/battlestation/ are here cell for cell; the
    // other three are here with the screen's light taken off them, glyph
    // for glyph
    const project::Project maker = project::read(kSamples / "maker" / "battlestation");
    REQUIRE(maker.layers.size() == 9);
    for (const char* name : {"monitor frame", "screen", "keyboard", "mug and mouse", "chair", "signature"}) {
        INFO(name);
        CHECK(named(p.layers, name).cells == named(maker.layers, name).cells);
    }
    // the other three are the maker's with the screen's light taken off:
    // on the wall and the desk every blue and light cyan is black, and on
    // the rack's side and top edge toward the screen (row 11 from x63 to
    // x71, and x63 to x65 below it) it is the rack's dark grey down to row
    // 29 and black from row 30; a space left on black has the grey a .ans
    // gives it; every other cell is the maker's as it is
    const auto isLight = [](const Color& c) {
        const std::uint8_t i = c.paletteIndex();
        return i == Blue || i == LightBlue || i == LightCyan;
    };
    const auto darkOnWallAndDesk = [](int, int) -> std::optional<std::uint8_t> { return Black; };
    const auto darkOnRack = [](int x, int y) -> std::optional<std::uint8_t> {
        if ((y == 11 && x >= 63 && x <= 71) || (y >= 12 && x >= 63 && x <= 65))
            return y < 30 ? DarkGrey : Black;
        return std::nullopt;
    };
    struct Unlit {
        const char *here, *there;
        std::function<std::optional<std::uint8_t>(int, int)> darkAt;
        int changed;
    };
    const Unlit unlit[] = {{"wall", "wall glow", darkOnWallAndDesk, 455},
                           {"desk", "desk", darkOnWallAndDesk, 549},
                           {"rack", "rack", darkOnRack, 92}};
    for (const Unlit& u : unlit) {
        const Canvas& dark = named(p.layers, u.here).cells;
        const Canvas& lit = named(maker.layers, u.there).cells;
        int changed = 0;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 80; ++x) {
                INFO(u.here << " at " << x << "," << y);
                Cell want = lit.at(x, y);
                if (const auto d = u.darkAt(x, y); d && !want.transparent) {
                    bool took = false;
                    for (Color* c : {&want.fg, &want.bg})
                        if (isLight(*c)) {
                            *c = Color::indexed(*d);
                            took = true;
                        }
                    if (took && want.ch == Glyph(U' ') && want.bg.paletteIndex() == Black)
                        want.fg = Color::indexed(Grey);
                }
                CHECK(dark.at(x, y) == want);
                changed += dark.at(x, y) != lit.at(x, y);
            }
        INFO(u.here);
        CHECK(changed == u.changed);
    }
    CHECK(named(p.layers, "rack").cells.at(65, 20).bg.paletteIndex() == DarkGrey);
    CHECK(named(p.layers, "rack").cells.at(65, 35).bg.paletteIndex() == Black);
    CHECK(named(maker.layers, "rack").cells.at(65, 35).bg.paletteIndex() == Blue);

    // shown together they are the piece, cell for cell
    const Canvas whole = composite(p.layers, {p.ice, Cell{}});
    CHECK(whole == composite(maker.layers, {maker.ice, Cell{}}));

    // the glow moves no glyph: with it hidden every cell has the glyph it
    // has with it shown; and under the chair and the signature it has a cell
    // exactly where it changes what shows
    const auto without = [&](std::set<std::string> hide) {
        LayerStack st = p.layers;
        for (std::size_t i = 0; i < st.size(); ++i)
            if (hide.count(st[i].props.name))
                st[i].props.visible = false;
        return composite(st, {p.ice, Cell{}});
    };
    // the room it lights, with the chair and the signature out of the way,
    // is the maker's room with those two hidden, so what the chair covers
    // is pinned as well as what shows
    const auto makerWithout = [&](std::set<std::string> hide) {
        LayerStack st = maker.layers;
        for (std::size_t i = 0; i < st.size(); ++i)
            if (hide.count(st[i].props.name))
                st[i].props.visible = false;
        return composite(st, {maker.ice, Cell{}});
    };
    const Canvas dark = without({"glow"});
    const Canvas litRoom = without({"chair", "signature"});
    const Canvas makerRoom = makerWithout({"chair", "signature"});
    const Canvas darkRoom = without({"glow", "chair", "signature"});
    const Canvas& glow = named(p.layers, "glow").cells;
    int lit = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 80; ++x) {
            INFO("cell " << x << "," << y);
            CHECK(dark.at(x, y).ch == whole.at(x, y).ch);
            CHECK(litRoom.at(x, y) == makerRoom.at(x, y));
            const bool lights = darkRoom.at(x, y) != makerRoom.at(x, y);
            CHECK(lights == !glow.at(x, y).transparent);
            if (lights) {
                CHECK(glow.at(x, y).fg == makerRoom.at(x, y).fg);
                CHECK(glow.at(x, y).bg == makerRoom.at(x, y).bg);
            }
            lit += !glow.at(x, y).transparent;
        }
    CHECK(lit == 746);
    // the cells it lights are on the wall, the desk and the rack's near
    // side: under the chair and the signature, over the rest
    CHECK_FALSE(glow.at(30, 2).transparent);   // the wall's top ring
    CHECK_FALSE(glow.at(5, 21).transparent);   // the desk top
    CHECK_FALSE(glow.at(64, 25).transparent);  // the rack's side
    CHECK(glow.at(30, 10).transparent);        // the screen, which is its own light
    CHECK(glow.at(70, 25).transparent);        // the rack's front

    // the hidden margin: one black block in the bottom left corner
    const Layer& margins = named(p.layers, "margins");
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 80; ++x) {
            const Cell& c = margins.cells.at(x, y);
            if (x == 0 && y == 39) {
                CHECK(c.ch == U'█');
                CHECK(c.fg.paletteIndex() == Black);
                CHECK(c.bg.paletteIndex() == Black);
            } else {
                CHECK(c.transparent);
            }
        }

    // the variants, each by the order and visibility it leaves
    checkVariants(p, {{"No glow", stackState(order, {"margins", "glow"})},
                      {"Lights off", stackState(order, {"margins", "glow", "screen"})},
                      {"Screen only", stackState(order, {"desk", "keyboard", "mug and mouse", "rack", "glow",
                                                         "chair", "signature"})}});

    // the snapshots: the dark room with the monitor on, the desk's
    // hardware, finished
    const auto upTo = [&](const std::string& last) {
        LayerStack st(80, 40);
        for (const Layer& l : p.layers.layers()) {
            st.add(l);
            if (l.props.name == last)
                break;
        }
        return std::optional<Canvas>(composite(st, {p.ice, Cell{}}));
    };
    checkSnapshots(p, {upTo("screen"), upTo("rack"), upTo("signature")});
    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("battlestation-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "battlestation.bwp");
    CHECK(project::read(out / "battlestation.bwp") == p);
}

// FNV-1a over a canvas's cells, row by row, every field == compares, the
// transparent cells' glyph and colours included: on a pixel layer a
// transparent ▀ or ▄ is a painted half.
std::uint64_t fingerprint(const Canvas& c) {
    std::uint64_t h = 14695981039346656037ull;
    const auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    const auto colour = [&](const Color& k) {
        mix(std::uint64_t(k.kind));
        mix(k.isIndexed() ? k.index : (std::uint64_t(k.rgb.r) << 16 | std::uint64_t(k.rgb.g) << 8 | k.rgb.b));
    };
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const Cell& cell = c.at(x, y);
            mix(cell.transparent);
            mix(cell.ch.codePoint().value_or(0));
            colour(cell.fg);
            colour(cell.bg);
            mix(cell.attrs);
        }
    return h;
}

// `flat` after one pass of the Form brush that dabs every cell row by row,
// left to right, at size 1, which in the app is the brain on IRC sample's
// one drag along each row and back along it, then down a row: Select All,
// the shape, the ramp and the strength, the light from the north-west, Glint
// on and Form rim light off.
Canvas formPass(const Layer& flat, const char* rampName, FormShape shape, double strength, bool ice) {
    StrokeOptions o = layerStrokeOptions(flat);
    o.selection = std::make_shared<const Mask>(flat.cells.width(), flat.cells.height(), true);
    o.ice = ice;
    BrushSettings s;
    s.lightDirection = Compass::NW;
    s.strength = strength;
    Canvas out = flat.cells;
    const std::optional<Ramp> r = findRamp(rampName);
    REQUIRE(r);
    paintRegion(out, FormShadeBrush(*r, FormOptions{shape, false, true}), s, o).apply(out);
    return out;
}

// The eighth sample: the yolk and the pan are Form brush output, with the
// # and the highlight over them.
TEST_CASE("the brain on IRC sample keeps the Form brush's yolk and pan, with the # and the highlight over them") {
    const fs::path dir = kSamples / "08-brain-on-irc";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    CHECK(p.layers.width() == 80);
    CHECK(p.layers.height() == 32);
    REQUIRE(p.sauce);
    CHECK(p.sauce->title == "This Is Your Brain on IRC");
    const std::vector<std::string> order{"background", "margins",   "caption",     "tagline",   "signature",
                                         "pan (flat)", "pan",       "egg white",   "yolk (flat)", "yolk",
                                         "#",          "highlight", "sizzle",      "smoke"};
    REQUIRE(names(p.layers) == order);

    // the #, the highlight and the sizzle are pixels, the rest cells, all
    // over the whole canvas in normal mode; the background alone is locked,
    // the pan and the yolk alone alpha-locked, both copies of each, and the
    // margins and the flat copies hidden
    const std::set<std::string> pixels{"#", "highlight", "sizzle"};
    const std::set<std::string> alpha{"pan", "pan (flat)", "yolk", "yolk (flat)"};
    const std::set<std::string> hidden{"margins", "pan (flat)", "yolk (flat)"};
    for (const Layer& l : p.layers.layers()) {
        INFO(l.props.name);
        CHECK(l.props.kind == (pixels.count(l.props.name) ? LayerKind::Pixels : LayerKind::Cells));
        CHECK(l.props.mode == LayerMode::Normal);
        CHECK(l.props.x == 0);
        CHECK(l.props.y == 0);
        CHECK(l.cells.width() == 80);
        CHECK(l.cells.height() == 32);
        CHECK(l.props.locked == (l.props.name == "background"));
        CHECK(l.props.alphaLocked == (alpha.count(l.props.name) == 1));
        CHECK(l.props.visible == (hidden.count(l.props.name) == 0));
    }

    // each layer, cell for cell, by a fingerprint taken when the picture
    // was settled: a cell changed anywhere, covered or not, changes it
    const std::map<std::string, std::uint64_t> prints{
        {"background", 0x3af6ca07566ec325},
        {"margins", 0xd4e700f939140c80},
        {"caption", 0x627cebcdea88e842},
        {"tagline", 0x1d4ae6871258e448},
        {"signature", 0x4e38d7d6c6187f22},
        {"pan (flat)", 0xb863861980eadf34},
        {"pan", 0x20e4237bc56f7773},
        {"egg white", 0xbe73f7f411ee2aba},
        {"yolk (flat)", 0xbcd26ad189827ff9},
        {"yolk", 0xd5f3c32e58ff9b72},
        {"#", 0xb9f9e07c9f7adcb5},
        {"highlight", 0xe3211fcd9a57b925},
        {"sizzle", 0x63b59dc700456abf},
        {"smoke", 0x1a6365d30de3646a},
    };
    for (const Layer& l : p.layers.layers()) {
        INFO(l.props.name);
        CHECK(fingerprint(l.cells) == prints.at(l.props.name));
    }

    // the layers the maker takes from maker/brain-on-irc/ as they are, and
    // the outlines it fills: the flat pan is dark grey and the flat yolk
    // yellow, solid blocks in exactly the outline's cells
    const project::Project maker = project::read(kSamples / "maker" / "brain-on-irc");
    for (const char* name : {"background", "caption", "tagline", "signature", "egg white", "smoke"}) {
        INFO(name);
        CHECK(named(p.layers, name).cells == named(maker.layers, name).cells);
    }
    for (const auto& [flat, outline, colour] :
         {std::tuple{"pan (flat)", "pan", DarkGrey}, std::tuple{"yolk (flat)", "yolk", Yellow}}) {
        INFO(flat);
        int cells = 0;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 80; ++x) {
                const Cell& c = named(p.layers, flat).cells.at(x, y);
                CHECK(c.transparent == named(maker.layers, outline).cells.at(x, y).transparent);
                if (c.transparent)
                    continue;
                ++cells;
                CHECK(c.ch == Glyph(U'█'));
                CHECK(int(c.fg.paletteIndex()) == colour);
                CHECK(int(c.bg.paletteIndex()) == Black);
            }
        CHECK(cells > 0);
    }

    // the pan and the yolk are one pass of the Form brush over the flat
    // copies, row by row as the notes' one drag goes, but for one cell each,
    // set to the shade of a neighbour
    const Canvas panPass = formPass(named(p.layers, "pan (flat)"), "steel", FormShape::Cylinder, 1.0, p.ice);
    const Canvas yolkPass = formPass(named(p.layers, "yolk (flat)"), "skin", FormShape::Dome, 0.65, p.ice);
    const auto passed = [&](const char* layer, const Canvas& pass, int tx, int ty) {
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 80; ++x)
                if (x != tx || y != ty) {
                    INFO(layer << " at " << x << "," << y);
                    CHECK(named(p.layers, layer).cells.at(x, y) == pass.at(x, y));
                }
        CHECK(named(p.layers, layer).cells.at(tx, ty) != pass.at(tx, ty));
    };
    passed("pan", panPass, 11, 22);
    passed("yolk", yolkPass, 24, 22);
    CHECK(named(p.layers, "pan").cells.at(11, 22) == named(p.layers, "pan").cells.at(10, 22));
    CHECK(named(p.layers, "yolk").cells.at(24, 22) == named(p.layers, "yolk").cells.at(24, 21));

    // cells pinned on each layer, the covered ones included
    const auto is = [&](const char* layer, int x, int y, char32_t ch, int fg, int bg) {
        INFO(layer << " at " << x << "," << y);
        const Cell& c = named(p.layers, layer).cells.at(x, y);
        CHECK_FALSE(c.transparent);
        CHECK(c.ch == Glyph(ch));
        CHECK(int(c.fg.paletteIndex()) == fg);
        CHECK(int(c.bg.paletteIndex()) == bg);
    };
    const auto clear = [&](const char* layer, int x, int y) {
        INFO(layer << " at " << x << "," << y);
        CHECK(named(p.layers, layer).cells.at(x, y).transparent);
    };
    const auto pixel = [&](const char* layer, int x, int py, int colour) {
        INFO(layer << " at " << x << ", pixel row " << py);
        const LayerPixels h = layerPixels(named(p.layers, layer).cells.at(x, py / 2));
        CHECK((py % 2 ? h.bottom : h.top) == std::optional<std::uint8_t>(colour));
    };
    is("background", 40, 16, U' ', Grey, Black);
    is("caption", 4, 0, U'█', White, Black);
    is("caption", 64, 4, U'█', Yellow, Black);
    is("caption", 64, 7, U'▀', Brown, Black);
    is("tagline", 28, 30, U'A', White, Black);
    is("tagline", 43, 30, U'/', Yellow, Black);
    is("signature", 70, 31, U'f', DarkGrey, Black);
    is("signature", 78, 31, U'r', DarkGrey, Black);
    clear("signature", 79, 31);
    // the pan: white along most of the upper edge of each band, black along
    // most of the lower, a few edge cells left the flat dark grey, the hole
    // in the handle, nothing under the egg
    is("pan", 30, 11, U'█', White, Black);
    is("pan", 30, 14, U' ', Black, Black);
    is("pan", 30, 26, U'█', White, Black);
    is("pan", 30, 29, U' ', Black, Black);
    is("pan", 70, 18, U'█', White, Black);
    is("pan", 72, 21, U' ', Black, Black);
    is("pan", 37, 14, U'█', DarkGrey, Black);
    is("pan", 70, 21, U'█', DarkGrey, Black);
    is("pan", 60, 17, U'█', DarkGrey, Black);
    clear("pan", 74, 20);
    clear("pan", 75, 20);
    clear("pan", 30, 20);
    // the notes give the number of cells along the upper and lower edges that
    // keep the flat dark grey: dark grey with no pan above or below
    {
        const Canvas& pan = named(p.layers, "pan").cells;
        const auto open = [&](int x, int y) { return y < 0 || y >= pan.height() || pan.at(x, y).transparent; };
        int kept = 0;
        for (int y = 0; y < pan.height(); ++y)
            for (int x = 0; x < pan.width(); ++x) {
                const Cell& c = pan.at(x, y);
                if (!c.transparent && c.ch == Glyph(U'█') && int(c.fg.paletteIndex()) == DarkGrey &&
                    (open(x, y - 1) || open(x, y + 1)))
                    ++kept;
            }
        const std::vector<std::string> words{"No",      "One",      "Two",       "Three",    "Four",     "Five",
                                             "Six",     "Seven",    "Eight",     "Nine",     "Ten",      "Eleven",
                                             "Twelve",  "Thirteen", "Fourteen",  "Fifteen",  "Sixteen",  "Seventeen",
                                             "Eighteen", "Nineteen", "Twenty"};
        std::smatch m;
        REQUIRE(std::regex_search(p.notes, m,
                                  std::regex("([A-Z][a-z]+) cells along the upper and lower edges keep the flat dark grey")));
        const auto said = std::find(words.begin(), words.end(), m[1].str());
        INFO("the notes say " << m[1].str());
        REQUIRE(said != words.end());
        CHECK(said - words.begin() == kept);
        CHECK(kept == 11);
    }
    is("egg white", 30, 18, U'█', White, Black);  // under the yolk
    is("egg white", 40, 19, U'█', White, Black);
    is("egg white", 45, 21, U'█', White, Black);
    is("egg white", 20, 24, U'▀', Brown, Black);
    // the yolk: no white on it, yellow over most of it, brown over red down
    // its right side
    is("yolk", 29, 18, U'█', Yellow, Black);
    is("yolk", 39, 20, U'▒', Brown, Red);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 80; ++x) {
            const Cell& c = named(p.layers, "yolk").cells.at(x, y);
            INFO("yolk at " << x << "," << y);
            CHECK((c.transparent || (c.fg.paletteIndex() != White && c.bg.paletteIndex() != White)));
        }
    pixel("#", 31, 36, Brown);
    pixel("#", 35, 40, Brown);
    pixel("highlight", 27, 36, White);
    pixel("highlight", 38, 36, White);
    // the highlight covers no pixel of the #
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 80; ++x) {
            const LayerPixels hash = layerPixels(named(p.layers, "#").cells.at(x, y));
            const LayerPixels light = layerPixels(named(p.layers, "highlight").cells.at(x, y));
            INFO("# and highlight at " << x << "," << y);
            CHECK_FALSE((hash.top && light.top));
            CHECK_FALSE((hash.bottom && light.bottom));
        }
    pixel("sizzle", 14, 38, Yellow);
    pixel("sizzle", 56, 43, Yellow);  // a step in the arc, joined
    // the smoke: grey at the heart of each wisp, dark grey about it
    is("smoke", 11, 10, U'░', Grey, Black);
    is("smoke", 37, 9, U'░', Grey, Black);
    is("smoke", 54, 12, U'░', Grey, Black);
    is("smoke", 39, 8, U'░', DarkGrey, Black);
    is("smoke", 60, 8, U'░', DarkGrey, Black);

    // the hidden margin: one black block in the top left corner
    const Layer& margins = named(p.layers, "margins");
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 80; ++x) {
            const Cell& c = margins.cells.at(x, y);
            if (x == 0 && y == 0) {
                CHECK(c.ch == U'█');
                CHECK(c.fg.paletteIndex() == Black);
                CHECK(c.bg.paletteIndex() == Black);
            } else {
                CHECK(c.transparent);
            }
        }

    // the lettering shares no cell with any shown layer over it, so it can
    // sit under the pan
    for (const char* lettering : {"caption", "tagline", "signature"}) {
        int cells = 0;
        const std::size_t at = *p.layers.indexOf(named(p.layers, lettering).props.id);
        for (std::size_t i = at + 1; i < p.layers.size(); ++i) {
            const Layer& l = p.layers[i];
            if (l.props.name == "caption" || l.props.name == "tagline" || l.props.name == "signature")
                continue;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 80; ++x)
                    if (!named(p.layers, lettering).cells.at(x, y).transparent) {
                        INFO(lettering << " under " << l.props.name << " at " << x << "," << y);
                        CHECK(l.cells.at(x, y).transparent);
                        ++cells;
                    }
        }
        CHECK(cells > 0);
    }

    // the longest IRC line, row 21: its length as exported and as sent
    const AnsiDocument doc = project::toAnsi(p);
    const std::vector<std::string> lines = irc::encodeCanvas(doc.canvas);
    REQUIRE(lines.size() == 32);
    std::size_t longest = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (irc::txtSentBytes(lines[i], p.ircBudget.bytes) > irc::txtSentBytes(lines[longest], p.ircBudget.bytes))
            longest = i;
    CHECK(longest == 20);
    CHECK(lines[20].size() == 273);
    CHECK(irc::txtSentBytes(lines[20], p.ircBudget.bytes) == 301);
    CHECK(p.ircBudget.bytes == 407);

    // the variants, each by the order and visibility it leaves
    checkVariants(p, {{"Raw egg", stackState(order, {"margins", "pan", "yolk", "#", "highlight", "sizzle", "smoke"})},
                      {"No caption", stackState(order, {"caption", "tagline", "pan (flat)", "yolk (flat)"})}});

    // the snapshots: the flat pan and yolk, which is the Raw egg variant;
    // the Form brush's own pan and yolk, before the two touches; finished
    const auto at = [&](project::Project& q, const char* name) -> Layer& {
        return q.layers[*q.layers.indexOf(named(q.layers, name).props.id)];
    };
    project::Project raw = hiding(p, {"pan", "yolk", "#", "highlight", "sizzle", "smoke"});
    at(raw, "pan (flat)").props.visible = true;
    at(raw, "yolk (flat)").props.visible = true;
    project::Project brushed = hiding(p, {"#", "highlight", "sizzle", "smoke"});
    at(brushed, "pan").cells = panPass;
    at(brushed, "yolk").cells = yolkPass;
    // each as an .ans written and read back, as a snapshot is: a space
    // keeps no colour of its own there
    const auto asSaved = [](const project::Project& q) {
        return parseAnsi(writeAnsi(project::toAnsi(q), q.ansEncoding).bytes).canvas;
    };
    checkSnapshots(p, {asSaved(raw), asSaved(brushed), asSaved(p)});
    const std::map<int, std::uint64_t> snapshotPrints{
        {1, 0x2ebd646080936a11}, {2, 0x1bd504253cdde05e}, {3, 0x1fa645739969f970}};
    for (const project::Snapshot& s : p.snapshots) {
        INFO("r" << s.round);
        CHECK(fingerprint(parseAnsi(s.ans).canvas) == snapshotPrints.at(s.round));
    }

    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("brain-on-irc-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "brain.bwp");
    CHECK(project::read(out / "brain.bwp") == p);
}

// The ninth sample, a finished piece a hundred columns wide, its layers as
// they are in maker/wtc-0903/, with their locks and modes set (PLAN §12).
TEST_CASE("the wtc-0903 sample keeps its layers, its sunlight apart from the towers and colours-only reflections") {
    constexpr int W = 100, H = 48;
    const fs::path dir = kSamples / "09-wtc-0903";
    const project::Project p = project::read(dir);
    CHECK(p.ice);
    CHECK(p.layers.width() == W);
    CHECK(p.layers.height() == H);
    const std::vector<std::string> order{"sky",      "North Tower", "plume", "fire",  "South Tower", "sunlight",
                                         "fireball", "plane",       "skyline", "water", "reflections", "tag"};
    REQUIRE(names(p.layers) == order);

    // every layer is cells over the whole canvas and shown; the sky alone is
    // locked, the two towers alone alpha-locked, the reflections alone
    // colours only
    for (const Layer& l : p.layers.layers()) {
        INFO(l.props.name);
        CHECK(l.props.kind == LayerKind::Cells);
        CHECK(l.props.mode == (l.props.name == "reflections" ? LayerMode::ColoursOnly : LayerMode::Normal));
        CHECK(l.props.x == 0);
        CHECK(l.props.y == 0);
        CHECK(l.cells.width() == W);
        CHECK(l.cells.height() == H);
        CHECK(l.props.locked == (l.props.name == "sky"));
        CHECK(l.props.alphaLocked == (l.props.name == "North Tower" || l.props.name == "South Tower"));
        CHECK(l.props.visible);
    }

    // the layers in maker/wtc-0903/ are here cell for cell, and shown
    // together they are the piece
    const project::Project maker = project::read(kSamples / "maker" / "wtc-0903");
    REQUIRE(names(maker.layers) == order);
    for (const std::string& name : order) {
        INFO(name);
        CHECK(named(p.layers, name).cells == named(maker.layers, name).cells);
    }
    const Canvas whole = composite(p.layers, {p.ice, Cell{}});
    CHECK(whole == composite(maker.layers, {maker.ice, Cell{}}));

    const auto says = [&](const std::string& text) {
        INFO("the notes say: " << text);
        CHECK(p.notes.find(text) != std::string::npos);
    };
    const auto str = [](auto v) { return std::to_string(v); };
    // "a, b and c", as the notes list layers
    const auto listOf = [](const std::vector<std::string>& items) {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i)
            out += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
        return out;
    };
    const auto without = [&](const std::set<std::string>& hide) {
        return composite(hiding(p, hide).layers, {p.ice, Cell{}});
    };
    const auto changedCells = [&](const Canvas& a, const Canvas& b) {
        std::vector<std::pair<int, int>> out;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (a.at(x, y) != b.at(x, y))
                    out.emplace_back(x, y);
        return out;
    };
    const auto isEastFace = [](int x) { return (x >= 46 && x <= 55) || (x >= 64 && x <= 73); };

    // the reflections hold cells on row 43 alone and move no glyph: hidden,
    // every cell keeps its glyph and row 43 is the water's light blue
    const Canvas& reflections = named(p.layers, "reflections").cells;
    const Canvas noReflections = without({"reflections"});
    int reflected = 0;
    std::set<int> reflectionRows;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            INFO("cell " << x << "," << y);
            if (!reflections.at(x, y).transparent) {
                CHECK(y == 43);
                reflectionRows.insert(y);
                ++reflected;
            }
            CHECK(noReflections.at(x, y).ch == whole.at(x, y).ch);
        }
    CHECK(reflected > 0);
    for (int x = 0; x < W; ++x) {
        INFO("column " << x);
        CHECK(noReflections.at(x, 43).ch == U'█');
        CHECK(noReflections.at(x, 43).fg.paletteIndex() == LightBlue);
    }

    // every reflection is dark grey on black under a building of the
    // skyline, and two columns of every five are left out
    const Canvas& skyline = named(p.layers, "skyline").cells;
    for (int x = 0; x < W; ++x) {
        const Cell& r = reflections.at(x, 43);
        if (r.transparent)
            continue;
        INFO("reflection " << x);
        CHECK(r.ch == U'█');
        CHECK(r.fg.paletteIndex() == DarkGrey);
        CHECK(r.bg.paletteIndex() == Black);
        CHECK(!skyline.at(x, 41).transparent);
        CHECK(x % 5 >= 2);
    }
    CHECK(reflected == 20);
    REQUIRE(reflectionRows.size() == 1);
    const int reflectionRow = *reflectionRows.begin();
    says("along row " + str(reflectionRow) + ", all in dark grey, with two columns of every five left out");
    says("Select reflections and paint along row " + str(reflectionRow) + " in light cyan.");
    says("in light cyan. The layer is colours only, so the water's full blocks stay and take the new colour. Undo the "
         "paint before the next step.");

    // the sunlight: hidden, 286 cells change, every one on an east face
    const auto sunOff = changedCells(whole, without({"sunlight"}));
    CHECK(sunOff.size() == 286);
    for (const auto& [x, y] : sunOff) {
        INFO("cell " << x << "," << y);
        CHECK(isEastFace(x));
    }
    const Canvas& sunlight = named(p.layers, "sunlight").cells;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (!sunlight.at(x, y).transparent) {
                INFO("cell " << x << "," << y);
                CHECK(isEastFace(x));
            }

    // it lights only the cells the plume and the fire left showing: under
    // each of its cells the nearest layer that has one is a tower
    const std::size_t sunAt = *p.layers.indexOf(named(p.layers, "sunlight").props.id);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (sunlight.at(x, y).transparent)
                continue;
            std::string under;
            for (std::size_t i = sunAt; i-- > 0;)
                if (!p.layers[i].cells.at(x, y).transparent) {
                    under = p.layers[i].props.name;
                    break;
                }
            INFO("cell " << x << "," << y);
            CHECK((under == "South Tower" || under == "North Tower"));
        }

    // the North Tower carries the 41 east-face cells the plume and the fire
    // cover in the sun itself: with those, the fireball, the plane and the
    // sunlight hidden, they are the white left on the North Tower, rows 11
    // to 15
    const Canvas bare = without({"plume", "fire", "fireball", "plane", "sunlight"});
    std::vector<std::pair<int, int>> band;
    for (int y = 0; y < H; ++y)
        for (int x = 60; x <= 73; ++x) {
            const Cell& c = bare.at(x, y);
            if (c.fg.paletteIndex() == White || c.bg.paletteIndex() == White) {
                INFO("cell " << x << "," << y);
                CHECK((x >= 64 && y >= 11 && y <= 15));
                band.emplace_back(x, y);
            }
        }
    const int litBand = int(band.size());
    CHECK(litBand == 41);
    int bandTop = H, bandBottom = -1;
    for (const auto& [x, y] : band) {
        bandTop = std::min(bandTop, y);
        bandBottom = std::max(bandBottom, y);
    }
    const std::string bandRows = "rows " + str(bandTop) + " to " + str(bandBottom);
    says("The North Tower's east face is lit all the way down, " + bandRows + " included");
    says("a lit band across " + bandRows + " of the North Tower");

    // the plume behind the South Tower: raised above the sunlight, it
    // crosses the tower's top, 79 cells in columns 42 to 62, rows 9 to 14
    project::Project smoke = p;
    REQUIRE_NOTHROW(applyVariant(smoke.layers, {"Smoke in front", {"raise plume above sunlight"}}));
    const auto smokeCells = changedCells(whole, composite(smoke.layers, {p.ice, Cell{}}));
    CHECK(smokeCells.size() == 79);
    int left = W, top = H, right = -1, bottom = -1;
    for (const auto& [x, y] : smokeCells) {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x);
        bottom = std::max(bottom, y);
    }
    CHECK(left == 42);
    CHECK(right == 62);
    CHECK(top == 9);
    CHECK(bottom == 14);

    // a hundred columns and every row one IRC line: the longest, row 24,
    // as exported and as sent, and 68 shade glyphs of three bytes each in it
    const AnsiDocument doc = project::toAnsi(p);
    const std::vector<std::string> lines = irc::encodeCanvas(doc.canvas);
    REQUIRE(lines.size() == std::size_t(H));
    std::size_t longest = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (irc::txtSentBytes(lines[i], p.ircBudget.bytes) > irc::txtSentBytes(lines[longest], p.ircBudget.bytes))
            longest = i;
    CHECK(longest == 24);
    CHECK(lines[24].size() == 344);
    CHECK(irc::txtSentBytes(lines[24], p.ircBudget.bytes) == 347);
    CHECK(p.ircBudget.bytes == 407);
    int shades = 0;
    for (std::size_t at = lines[24].find("░"); at != std::string::npos; at = lines[24].find("░", at + 1))
        ++shades;
    CHECK(shades == 68);

    // the three dearest rows as sent, together, where the sky shades from
    // light blue into light cyan
    std::vector<std::size_t> byCost(lines.size());
    for (std::size_t i = 0; i < byCost.size(); ++i)
        byCost[i] = i;
    std::stable_sort(byCost.begin(), byCost.end(), [&](std::size_t a, std::size_t b) {
        return irc::txtSentBytes(lines[a], p.ircBudget.bytes) > irc::txtSentBytes(lines[b], p.ircBudget.bytes);
    });
    std::vector<std::size_t> dearest(byCost.begin(), byCost.begin() + 3);
    std::sort(dearest.begin(), dearest.end());
    CHECK(dearest == std::vector<std::size_t>{24, 25, 26});
    std::vector<std::size_t> dearestSent;
    for (std::size_t i : dearest)
        dearestSent.push_back(irc::txtSentBytes(lines[i], p.ircBudget.bytes));
    CHECK(dearestSent == std::vector<std::size_t>{347, 342, 342});
    CHECK(irc::txtSentBytes(lines[byCost[3]], p.ircBudget.bytes) < dearestSent.back());
    CHECK(dearest.back() - dearest.front() == 2);
    says("The dearest rows are " + str(dearest.front()) + " to " + str(dearest.back()) +
         ", where the sky shades from light blue into light cyan");

    // the numbers the notes give are the piece's
    const auto boundsOf = [&](const Canvas& c) {
        std::array<int, 4> b{W, H, -1, -1};  // left, top, right, bottom
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (!c.at(x, y).transparent)
                    b = {std::min(b[0], x), std::min(b[1], y), std::max(b[2], x), std::max(b[3], y)};
        return b;
    };
    says(str(sunOff.size()) + " cells change, all of them on the two towers");
    says("The North Tower layer carries those " + str(litBand) + " cells in the sun itself.");
    says(str(smokeCells.size()) + " cells change, in columns " + str(left) + " to " + str(right) + " and rows " +
         str(top) + " to " + str(bottom));
    says("row " + str(longest) + " exports at " + str(lines[longest].size()) + " bytes, " +
         str(irc::txtSentBytes(lines[longest], p.ircBudget.bytes)) + " as sent, against the project's budget of " +
         str(p.ircBudget.bytes) + ", and " + str(3 * shades) + " of those bytes are its " + str(shades) +
         " shade glyphs");

    // the fire: black, red, brown and yellow, dark grey at the end of the
    // gash and grey where a half block leaves the tower's face showing, in
    // rows 13 to 15 across columns 60 to 73, and its rows as sent
    const Canvas& fire = named(p.layers, "fire").cells;
    const std::set<int> fireColours{Black, Red, Brown, Yellow, DarkGrey, Grey};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (!fire.at(x, y).transparent) {
                INFO("fire " << x << "," << y);
                CHECK(fireColours.count(fire.at(x, y).fg.paletteIndex()));
                CHECK(fireColours.count(fire.at(x, y).bg.paletteIndex()));
            }
    const auto fb = boundsOf(fire);
    CHECK(fb == std::array<int, 4>{60, 13, 73, 15});
    says("rows " + str(fb[1]) + " to " + str(fb[3]) + " across columns " + str(fb[0]) + " to " + str(fb[2]));
    says("columns " + str(fb[0]) + " to " + str(fb[2]) + ", rows " + str(fb[1]) + " to " + str(fb[3]));
    std::vector<std::size_t> fireRows;
    for (int y = fb[1]; y <= fb[3]; ++y)
        fireRows.push_back(irc::txtSentBytes(lines[std::size_t(y)], p.ircBudget.bytes));
    CHECK(fireRows == std::vector<std::size_t>{206, 241, 280});
    says("The fire's rows, " + str(fb[1]) + " to " + str(fb[3]) + ", come to " + str(fireRows[0]) + ", " +
         str(fireRows[1]) + " and " + str(fireRows[2]) + " bytes as sent.");

    // the fire covers the plume at one cell, and raising the plume covers it
    const Canvas& plume = named(p.layers, "plume").cells;
    std::vector<std::pair<int, int>> overlap;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (!plume.at(x, y).transparent && !fire.at(x, y).transparent)
                overlap.emplace_back(x, y);
    REQUIRE(overlap.size() == 1);
    CHECK(overlap[0] == std::pair{62, 13});
    CHECK(std::count(smokeCells.begin(), smokeCells.end(), overlap[0]) == 1);
    const std::string overlapAt = "column " + str(overlap[0].first) + " on row " + str(overlap[0].second);
    says("The fire covers it at one cell, " + overlapAt + ".");
    says("covers the one cell where the fire sat over it, " + overlapAt + ".");

    // the fireball: below the fire, rows 15 to 24, and further left, to
    // column 46 on the South Tower's east face
    const auto bb = boundsOf(named(p.layers, "fireball").cells);
    CHECK(bb == std::array<int, 4>{46, 15, 72, 24});
    CHECK(bb[3] > fb[3]);
    CHECK(bb[0] < fb[0]);
    CHECK(isEastFace(bb[0]));
    CHECK(bb[0] <= 55);
    says("the fireball a larger shape below it, rows " + str(bb[1]) + " to " + str(bb[3]) +
         ", that reaches further left, to column " + str(bb[0]) + " on the South Tower's east face");

    // the skyline from row 25 down, where 40 Wall Street's spire stands
    // alone, dark grey, in column 79 down to row 27
    const auto kb = boundsOf(skyline);
    CHECK(kb[1] == 25);
    int spireEnd = kb[1];
    const auto spireOnly = [&](int y) {
        for (int x = 0; x < W; ++x)
            if (!skyline.at(x, y).transparent && x != 79)
                return false;
        return !skyline.at(79, y).transparent && skyline.at(79, y).fg.paletteIndex() == DarkGrey;
    };
    REQUIRE(spireOnly(kb[1]));
    while (spireOnly(spireEnd + 1))
        ++spireEnd;
    CHECK(spireEnd == 27);
    says("Downtown in front of the towers' feet, from row " + str(kb[1]) + " down:");
    says("a dark grey spire at column 79, rows " + str(kb[1]) + " to " + str(spireEnd) + ", alone above the other roofs");

    // the shore, the skyline's last row: black and dark grey only, the
    // water from the row under it, where the reflections are
    CHECK(kb[3] == 42);
    for (int x = 0; x < W; ++x) {
        INFO("shore " << x);
        const Cell& c = skyline.at(x, kb[3]);
        REQUIRE_FALSE(c.transparent);
        CHECK((c.ch == U' ' || c.fg.paletteIndex() == DarkGrey));
        CHECK(c.bg.paletteIndex() == Black);
    }
    says("the shore along row " + str(kb[3]) + ", a black drive with dark grey piers");
    const auto wb = boundsOf(named(p.layers, "water").cells);
    CHECK(wb[1] == kb[3] + 1);
    CHECK(wb[1] == reflectionRow);
    CHECK(wb[3] == H - 1);
    says("The river from row " + str(wb[1]) + " down");

    // the tag: one row, the last, in the right half
    const auto tb = boundsOf(named(p.layers, "tag").cells);
    CHECK(tb[1] == tb[3]);
    CHECK(tb[3] == H - 1);
    CHECK(tb[0] >= W / 2);
    says("funkpower in grey in the lower right corner, row " + str(tb[1]) + ".");

    // the plane's nose inside the South Tower: a hole of black spaces cut
    // into the south face, column 44, rows 18 and 19
    const Canvas& plane = named(p.layers, "plane").cells;
    std::vector<std::pair<int, int>> hole;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Cell& c = plane.at(x, y);
            if (!c.transparent && c.ch == U' ' && c.bg.paletteIndex() == Black) {
                INFO("hole " << x << "," << y);
                CHECK(!named(p.layers, "South Tower").cells.at(x, y).transparent);
                CHECK((x >= 42 && x <= 45));
                hole.emplace_back(x, y);
            }
        }
    CHECK(hole == std::vector<std::pair<int, int>>{{44, 18}, {44, 19}});
    REQUIRE(hole.size() == 2);
    says("where a black hole is cut at column " + str(hole[0].first) + ", rows " + str(hole[0].second) + " and " +
         str(hole[1].second) + ".");

    // each tower from the first row its cells span the whole width, and
    // the North Tower's mast above that
    const auto fullFrom = [&](const Canvas& c, int x0, int x1) {
        for (int y = 0; y < H; ++y) {
            bool full = true;
            for (int x = x0; x <= x1; ++x)
                full = full && !c.at(x, y).transparent;
            if (full)
                return y;
        }
        return -1;
    };
    const Canvas& southCells = named(p.layers, "South Tower").cells;
    const Canvas& northCells = named(p.layers, "North Tower").cells;
    const auto sTower = boundsOf(southCells);
    const auto nTower = boundsOf(northCells);
    CHECK(sTower == std::array<int, 4>{42, 9, 55, 44});
    CHECK(fullFrom(southCells, sTower[0], sTower[2]) == sTower[1]);
    const int northFrom = fullFrom(northCells, 60, 73);
    CHECK(northFrom == 10);
    CHECK(nTower[1] == 2);
    says("The South Tower in shade, columns " + str(sTower[0]) + " to " + str(sTower[2]) + " from row " +
         str(sTower[1]) + " down");
    says("The North Tower in shade, columns 60 to 73 from row " + str(northFrom) + " down");
    says("the mast on its roof in grey up to row " + str(nTower[1]));

    // the sky in bands: blue, light blue and light cyan, each row one
    // colour all the way across or a row of shade between two of them
    const Canvas& sky = named(p.layers, "sky").cells;
    const auto solidRow = [&](int y) -> int {  // the row's colour, or -1 for shade
        int colour = -1;
        for (int x = 0; x < W; ++x) {
            const Cell& c = sky.at(x, y);
            int here = -1;
            if (c.ch == U' ')
                here = c.bg.paletteIndex();
            else if (c.ch == U'█')
                here = c.fg.paletteIndex();
            if (here < 0 || (x > 0 && here != colour))
                return -1;
            colour = here;
        }
        return colour;
    };
    std::map<int, int> firstSolid;
    std::vector<int> shadeRows;
    for (int y = 0; y < H; ++y) {
        const int c = solidRow(y);
        if (c < 0)
            shadeRows.push_back(y);
        else if (!firstSolid.count(c))
            firstSolid[c] = y;
    }
    REQUIRE(firstSolid.count(Blue));
    REQUIRE(firstSolid.count(LightBlue));
    REQUIRE(firstSolid.count(LightCyan));
    CHECK(firstSolid.at(Blue) == 0);
    CHECK(shadeRows == std::vector<int>{10, 11, 12, 24, 25, 26});
    CHECK(firstSolid.at(LightBlue) == shadeRows[2] + 1);
    CHECK(firstSolid.at(LightCyan) == shadeRows[5] + 1);
    says("blue at the top, light blue from row " + str(firstSolid.at(LightBlue)) + ", light cyan from row " +
         str(firstSolid.at(LightCyan)) + ", with three rows of shade at each change, rows " + str(shadeRows[0]) +
         " to " + str(shadeRows[2]) + " and " + str(shadeRows[3]) + " to " + str(shadeRows[5]) + ".");
    // the dearest rows are the second change
    CHECK(int(dearest.front()) == shadeRows[3]);
    CHECK(int(dearest.back()) == shadeRows[5]);

    // Try this, step by step, each from where the one before left the piece
    {
        LayerStack st = p.layers;
        const auto show = [&](std::initializer_list<const char*> layers, bool on) {
            for (const char* n : layers)
                st.find(named(st, n).props.id)->props.visible = on;
        };
        const auto now = [&] { return composite(st, {p.ice, Cell{}}); };
        const auto showAll = [&](const std::vector<std::string>& layers, bool on) {
            for (const std::string& n : layers)
                st.find(named(st, n).props.id)->props.visible = on;
        };
        // 1: sunlight hidden
        show({"sunlight"}, false);
        CHECK(changedCells(whole, now()) == sunOff);
        // 2: sunlight back, the plume, the fire, the fireball and the plane
        // hidden, as Before 8:46 hides them; the North Tower's east face lit
        // rows 11 to 15, the band by the tower's own cells
        const std::vector<std::string> before846{"plume", "fire", "fireball", "plane"};
        says("Show sunlight again and hide " + listOf(before846) + ", as the Before 8:46 variant does.");
        show({"sunlight"}, true);
        showAll(before846, false);
        const Canvas lit = now();
        CHECK(lit == without({"plume", "fire", "fireball", "plane"}));
        for (int y = 11; y <= 15; ++y)
            for (int x = 64; x <= 73; ++x) {
                INFO("cell " << x << "," << y);
                CHECK((lit.at(x, y).fg.paletteIndex() == White || lit.at(x, y).bg.paletteIndex() == White));
            }
        for (const auto& [x, y] : band) {
            INFO("cell " << x << "," << y);
            CHECK(sunlight.at(x, y).transparent);
        }
        // 3: sunlight hidden too, then everything shown again
        show({"sunlight"}, false);
        CHECK(now() == bare);
        std::vector<std::string> hidden{"sunlight"};
        hidden.insert(hidden.end(), before846.begin(), before846.end());
        std::vector<std::string> stillHidden;
        for (const Layer& l : st.layers())
            if (!l.props.visible)
                stillHidden.push_back(l.props.name);
        CHECK(std::set<std::string>(hidden.begin(), hidden.end()) ==
              std::set<std::string>(stillHidden.begin(), stillHidden.end()));
        says("Show " + listOf(hidden) + " again before the next step.");
        showAll(hidden, true);
        CHECK(now() == whole);
        // 4: the plume raised above the sunlight, then lowered back to just
        // above the layer it sat on, one step at a time as the Layers dock
        // moves it
        REQUIRE_NOTHROW(applyVariant(st, {"step 4", {"raise plume above sunlight"}}));
        CHECK(changedCells(whole, now()) == smokeCells);
        const std::string plumeOn = *(std::find(order.begin(), order.end(), "plume") - 1);
        says("Lower plume back to just above " + plumeOn + " before the next step.");
        for (int i = 0; i < int(order.size()); ++i) {
            const std::vector<std::string> at = names(st);
            const auto it = std::find(at.begin(), at.end(), "plume");
            if (it == at.begin() || *(it - 1) == plumeOn)
                break;
            REQUIRE_NOTHROW(applyVariant(st, {"step 4", {"lower plume below " + *(it - 1)}}));
        }
        CHECK(names(st) == order);
        CHECK(now() == whole);
        // 5: everything but the sky, the fire and the fireball hidden, then
        // shown again
        show({"North Tower", "plume", "South Tower", "sunlight", "plane", "skyline", "water", "reflections", "tag"},
             false);
        std::vector<std::string> fireShown;
        for (const Layer& l : st.layers())
            if (l.props.visible)
                fireShown.push_back(l.props.name);
        says("Hide every layer but " + listOf(fireShown) + ", as the Fire only variant does.");
        LayerStack fireOnly = p.layers;
        REQUIRE_NOTHROW(applyVariant(fireOnly, {"Fire only",
                                                {"hide North Tower", "hide plume", "hide South Tower",
                                                 "hide sunlight", "hide plane", "hide skyline", "hide water",
                                                 "hide reflections", "hide tag"}}));
        CHECK(now() == composite(fireOnly, {p.ice, Cell{}}));
        show({"North Tower", "plume", "South Tower", "sunlight", "plane", "skyline", "water", "reflections", "tag"},
             true);
        for (const Layer& l : st.layers())
            CHECK(l.props.visible);
        CHECK(now() == whole);
        says("can be recoloured or moved without repainting a tower. Show every layer again before the next step.");
        // 6: the South Tower's cells are in columns 42 to 55, and the
        // sunlight is over every one of them on the east face
        const Canvas& south = named(st, "South Tower").cells;
        const auto sb = boundsOf(south);
        CHECK(sb[0] == 42);
        CHECK(sb[2] == 55);
        for (int y = 0; y < H; ++y)
            for (int x = 46; x <= 55; ++x)
                if (!south.at(x, y).transparent) {
                    INFO("cell " << x << "," << y);
                    CHECK(!sunlight.at(x, y).transparent);
                }
        says("keeps the paint on the tower's own cells, columns " + str(sb[0]) + " to " + str(sb[2]));
        // paint on the east face shows only with the sunlight hidden
        LayerStack painted = st;
        Layer& paintedSouth = *painted.find(named(painted, "South Tower").props.id);
        for (int x = 46; x <= 55; ++x)
            paintedSouth.cells.at(x, 12) = Cell{U'█', Color::indexed(LightRed), Color::indexed(Black)};
        CHECK(composite(painted, {p.ice, Cell{}}) == whole);
        painted.find(named(painted, "sunlight").props.id)->props.visible = false;
        LayerStack sunHidden = st;
        sunHidden.find(named(sunHidden, "sunlight").props.id)->props.visible = false;
        CHECK(composite(painted, {p.ice, Cell{}}) != composite(sunHidden, {p.ice, Cell{}}));
        says("On the east face the paint goes under the sunlight and shows only with sunlight hidden. Undo the paint "
             "before the next step.");
    }

    // the variants, each by the order and visibility it leaves
    std::vector<std::string> smokeOrder = order;
    smokeOrder.erase(std::find(smokeOrder.begin(), smokeOrder.end(), "plume"));
    smokeOrder.insert(std::find(smokeOrder.begin(), smokeOrder.end(), "sunlight") + 1, "plume");
    checkVariants(p, {{"Before 8:46", stackState(order, {"plume", "fire", "fireball", "plane"})},
                      {"Sunlight off", stackState(order, {"sunlight"})},
                      {"Fire only", stackState(order, {"North Tower", "plume", "South Tower", "sunlight", "plane",
                                                       "skyline", "water", "reflections", "tag"})},
                      {"Smoke in front", stackState(smokeOrder, {})}});

    // the snapshots: the sky and the towers in shade with the plume and the
    // fire; the sunlight, the fireball and the plane; finished
    const auto upTo = [&](const std::string& last) {
        LayerStack st(W, H);
        for (const Layer& l : p.layers.layers()) {
            st.add(l);
            if (l.props.name == last)
                break;
        }
        return std::optional<Canvas>(composite(st, {p.ice, Cell{}}));
    };
    checkSnapshots(p, {upTo("South Tower"), upTo("plane"), upTo("tag")});

    // it saves back as it reads, as a directory and as a zip
    const fs::path out = scratch("wtc-0903-saved");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "wtc-0903.bwp");
    CHECK(project::read(out / "wtc-0903.bwp") == p);
}

// The finished pieces among the samples (PLAN §12): with the layers they
// show, each is its piece's .ans cell for cell, the piece as it is in
// tests/data/pieces/. A space is compared by its background alone, since
// its foreground shows nowhere.
TEST_CASE("the finished samples are their pieces' .ans cell for cell") {
    const std::vector<std::pair<std::string, std::string>> pieces{
        {"02-ddos-king", "ddos-king.ans"}, {"07-battlestation", "battlestation.ans"}, {"09-wtc-0903", "wtc-0903.ans"}};
    for (const auto& [sample, file] : pieces) {
        INFO(sample);
        const project::Project p = project::read(kSamples / sample);
        const Canvas shown = parseAnsi(writeAnsi(project::toAnsi(p), p.ansEncoding).bytes).canvas;
        const AnsiDocument piece = parseAnsi(bwtest::readFile(bwtest::dataDir() / "pieces" / file));
        REQUIRE(shown.width() == piece.canvas.width());
        REQUIRE(shown.height() == piece.canvas.height());
        const auto same = [](const Cell& a, const Cell& b) {
            if (a.ch == U' ' && b.ch == U' ')
                return a.bg == b.bg && a.attrs == b.attrs && a.transparent == b.transparent;
            return a == b;
        };
        int differ = 0;
        for (int y = 0; y < shown.height(); ++y)
            for (int x = 0; x < shown.width(); ++x)
                if (!same(shown.at(x, y), piece.canvas.at(x, y))) {
                    if (differ < 5)
                        UNSCOPED_INFO("cell " << x << "," << y << " differs");
                    ++differ;
                }
        CHECK(differ == 0);
    }
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

    // the variants: the rim light, the shadows, or both, hidden
    const std::vector<std::string> order{"wall", "table", "shadows", "cylinder", "box", "sphere", "rim light"};
    checkVariants(p, {
                         {"No rim light", stackState(order, {"rim light"})},
                         {"No shadows", stackState(order, {"shadows"})},
                         {"Form only", stackState(order, {"rim light", "shadows"})},
                     });

    // three rounds, each decoded and held to its stage: the objects blocked
    // in flat, each its own shape in one colour, the middle solid colour of
    // its ramp (the cylinder magenta on violet, the box brown on wood
    // without its black, the ball blue on sea), over the wall and the table;
    // each shaded inside its shape, before the shadows and the rim light;
    // the finished piece
    project::Project blockedIn = hiding(p, {"shadows", "rim light"});
    const project::Project shaded = blockedIn;
    const std::map<std::string, std::uint8_t> flat{{"cylinder", Magenta}, {"box", Brown}, {"sphere", Blue}};
    for (std::size_t i = 0; i < blockedIn.layers.size(); ++i) {
        Layer& l = blockedIn.layers[i];
        if (!flat.count(l.props.name))
            continue;
        int filled = 0;
        for (int y = 0; y < l.cells.height(); ++y)
            for (int x = 0; x < l.cells.width(); ++x) {
                Cell& c = l.cells.at(x, y);
                if (c.transparent)
                    continue;
                c.ch = U' ';
                c.fg = Color::indexed(Black);
                c.bg = Color::indexed(flat.at(l.props.name));
                ++filled;
            }
        CHECK(filled > 0);
    }
    checkSnapshots(p, {project::toAnsi(blockedIn).canvas, project::toAnsi(shaded).canvas, whole});

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

    // The first round as the notes give it, on every host and running
    // nothing: the sky and the stars as the piece keeps them, and under
    // them two smooth ridges and nothing else. Down each column the cells
    // are the sky's to the far ridge's edge, then two bands, each in one
    // look all along the ridge: the far one, the light blue the far ridge
    // always is, two or three rows deep, then the near one, darker, to the
    // bottom. Each edge climbs or drops a row a column at most, the far one
    // between the highest row the piece keeps (18) and the seven under it.
    // With no roughness each crest and trough of an edge is a rolling one,
    // four columns across or more, where roughness puts jags a column or
    // two across on them.
    {
        const Canvas r1 = parseAnsi(p.snapshots.at(0).ans).canvas;
        const Canvas skyAndStars = project::toAnsi(hiding(p, {"ridges", "lake", "shore", "pines", "cabin"})).canvas;
        REQUIRE(r1.width() == skyAndStars.width());
        REQUIRE(r1.height() == skyAndStars.height());
        std::vector<int> farEdge, nearEdge;
        std::vector<Cell> farLooks, nearLooks;
        for (int x = 0; x < r1.width(); ++x) {
            INFO("r1, column " << x);
            int y = 0;
            while (y < r1.height() && r1.at(x, y) == skyAndStars.at(x, y))
                ++y;
            std::vector<std::pair<int, Cell>> bands;  // under the sky, each run of one look: its top row and look
            for (; y < r1.height(); ++y)
                if (bands.empty() || !(r1.at(x, y) == bands.back().second))
                    bands.emplace_back(y, r1.at(x, y));
            CHECK(bands.size() == 2);
            if (bands.size() != 2)
                continue;
            farEdge.push_back(bands[0].first);
            nearEdge.push_back(bands[1].first);
            farLooks.push_back(bands[0].second);
            nearLooks.push_back(bands[1].second);
        }
        REQUIRE(int(farEdge.size()) == r1.width());
        for (const Cell& c : farLooks)
            CHECK(c == farLooks[0]);
        for (const Cell& c : nearLooks)
            CHECK(c == nearLooks[0]);
        const Cell& far = farLooks[0];
        const Cell& near = nearLooks[0];
        CHECK(far.ch.codePointOr(kNoCodePoint) == U'█');
        CHECK(far.fg.paletteIndex() == LightBlue);
        CHECK(value(near) < value(far));
        CHECK(value(far) < glow);
        for (const Cell& c : {far, near})
            CHECK(skyLooks.count({c.ch.bits(), c.fg.paletteIndex(), c.bg.paletteIndex()}) == 0);
        for (int x = 0; x < r1.width(); ++x) {
            INFO("r1, column " << x << ": far edge at row " << farEdge[x] << ", near at " << nearEdge[x]);
            CHECK(farEdge[x] >= 18);
            CHECK(farEdge[x] <= 25);
            CHECK((nearEdge[x] - farEdge[x] == 2 || nearEdge[x] - farEdge[x] == 3));
            if (x > 0) {
                CHECK(std::abs(farEdge[x] - farEdge[x - 1]) <= 1);
                CHECK(std::abs(nearEdge[x] - nearEdge[x - 1]) <= 1);
            }
        }
        CHECK(*std::min_element(farEdge.begin(), farEdge.end()) < *std::max_element(farEdge.begin(), farEdge.end()));
        CHECK(narrowestTurn(farEdge) >= 4);
        CHECK(narrowestTurn(nearEdge) >= 4);
    }

    // where the generator can run, the first round is also the ridges
    // generator run again at its parameters
    std::optional<Canvas> twoSmooth;
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
    // the first round's ridges: two of them, smooth, the seed and the rest
    // as the piece keeps them
    {
        project::Project first = hiding(p, {"lake", "shore", "pines", "cabin"});
        const LayerId id = named(first.layers, "ridges").props.id;
        first.files[generator::directory(id) + std::string(generator::kParamsFile)] =
            R"({"ridges": 2, "roughness": 0, "seed": 3, "sharp": false, "top": 18})";
        const Canvas cells = generator::run(first, id);
        for (std::size_t i = 0; i < first.layers.size(); ++i)
            if (first.layers[i].props.id == id)
                first.layers[i].cells = cells;
        twoSmooth = project::toAnsi(first).canvas;
    }
#endif

    // the variants, each by the order and visibility it leaves; the baked
    // copy looks as the ridges do, the others change the picture
    const std::vector<std::string> order{"sky",  "stars", "ridges", "ridges (seed 12)", "ridges (baked)",
                                         "lake", "shore", "pines",  "cabin"};
    checkVariants(p, {
                         {"Another seed", stackState(order, {"ridges", "ridges (baked)"})},
                         {"Baked", stackState(order, {"ridges", "ridges (seed 12)"})},
                         {"No stars", stackState(order, {"stars", "ridges (seed 12)", "ridges (baked)"})},
                         {"Generated only", stackState(order, {"ridges (seed 12)", "ridges (baked)", "lake", "shore",
                                                               "pines", "cabin"})},
                     });
    const Canvas whole = project::toAnsi(p).canvas;
    const Notes notes = parseNotes(p.notes);
    for (const Variant& v : notes.variants) {
        INFO(v.name);
        project::Project changed = p;
        REQUIRE_NOTHROW(applyVariant(changed.layers, v));
        CHECK((project::toAnsi(changed).canvas == whole) == (v.name == "Baked"));
    }

    // three rounds, each decoded and held to its stage, as the notes give
    // them: the sky, the stars and the first run of the ridges, two smooth
    // ones; the same with the parameters the piece keeps; the finished
    // piece with the lake, shore, pines and cabin. The first is held to the notes
    // above on every host, and here also to the ridges generator run again
    // at its parameters, where it can run.
    const project::Project generated = hiding(p, {"lake", "shore", "pines", "cabin"});
    checkSnapshots(p, {twoSmooth, project::toAnsi(generated).canvas, whole});

    const fs::path out = scratch("generator-round-trip");
    project::writeDirectory(p, out / "dir");
    CHECK(project::read(out / "dir") == p);
    project::writeZip(p, out / "ridges.bwp");
    CHECK(project::read(out / "ridges.bwp") == p);
}

#ifndef _WIN32
TEST_CASE("the generator sample's ridges stay on a canvas shorter than the highest row the form allows") {
    // Highest row goes to 33, which fits the sample's 34 rows; on a shorter
    // canvas no peak still rises above it, and the far ridge lies along the
    // last rows, so it shows in every column (#442)
    const project::Project sample = project::read(kSamples / "04-generator-ridges");
    const std::string from = generator::directory(named(sample.layers, "ridges").props.id);
    std::map<std::string, std::string> files;
    for (const auto& [path, bytes] : sample.files)
        if (path.starts_with(from))
            files[path.substr(from.size())] = bytes;
    REQUIRE(files.count(std::string(generator::kSpecFile)) == 1);
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
    for (const auto& [height, top, roughness] :
         std::vector<std::tuple<int, int, double>>{{25, 33, 0.3}, {25, 33, 1}, {25, 24, 0.3}, {34, 33, 1}, {6, 33, 0.3}}) {
        INFO(height << " rows, top " << top << ", roughness " << roughness);
        AnsiDocument doc;
        doc.canvas = Canvas(80, height);
        project::Project p = project::fromAnsi(doc);
        files[std::string(generator::kParamsFile)] = R"({"ridges": 3, "roughness": )" + std::to_string(roughness) +
                                                     R"(, "seed": 3, "sharp": false, "top": )" + std::to_string(top) +
                                                     "}";
        const LayerId id = generator::add(p, files, "ridges");
        const Canvas& cells = p.layers.find(id)->cells;
        REQUIRE(cells.height() == height);
        for (int x = 0; x < cells.width(); ++x) {
            INFO("column " << x);
            CHECK_FALSE(cells.at(x, height - 1).transparent);
            for (int y = 0; y < std::min(top, height - 1); ++y)
                CHECK(cells.at(x, y).transparent);
        }
    }
}
#endif

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

    // the variants, each by the order and visibility it leaves; the merged
    // copy looks as the parts do, the others change the picture
    const std::vector<std::string> order{"sky",  "hills",       "meadow",     "shadow",         "outline",
                                         "body", "face detail", "highlights", "figure (merged)"};
    checkVariants(p, {
                         {"No outline", stackState(order, {"figure (merged)", "outline"})},
                         {"No highlights", stackState(order, {"figure (merged)", "highlights"})},
                         {"Silhouette", stackState({"sky", "hills", "meadow", "shadow", "body", "face detail",
                                                    "highlights", "outline", "figure (merged)"},
                                                   {"figure (merged)"})},
                         {"One layer", stackState(order, {"shadow", "outline", "body", "face detail", "highlights"})},
                         {"Scene only", stackState(order, {"shadow", "outline", "body", "face detail", "highlights",
                                                           "figure (merged)"})},
                     });
    const Canvas whole = project::toAnsi(p).canvas;
    const Notes notes = parseNotes(p.notes);
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

    // three rounds, each decoded and held to its stage, as the notes give
    // them: the scene; the figure with its outline and its body in flat
    // colours, no lit side (the coat's light red red, the pack's
    // light green green); the finished piece
    const project::Project sceneOnly =
        hiding(p, {"shadow", "outline", "body", "face detail", "highlights", "figure (merged)"});
    project::Project blockedIn = hiding(p, {"shadow", "face detail", "highlights", "figure (merged)"});
    int flattened = 0;
    for (std::size_t i = 0; i < blockedIn.layers.size(); ++i) {
        Layer& l = blockedIn.layers[i];
        if (l.props.name != "body")
            continue;
        for (int py = 0; py < 2 * l.cells.height(); ++py)
            for (int x = 0; x < l.cells.width(); ++x) {
                const auto c = pixel(l, x, py);
                if (c == std::optional<std::uint8_t>(LightRed) || c == std::optional<std::uint8_t>(LightGreen)) {
                    l.cells.at(x, py / 2) =
                        setLayerPixel(l.cells.at(x, py / 2), py % 2 == 1, *c == LightRed ? Red : Green);
                    ++flattened;
                }
            }
    }
    CHECK(flattened > 0);
    checkSnapshots(p, {project::toAnsi(sceneOnly).canvas, project::toAnsi(blockedIn).canvas, whole});

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
