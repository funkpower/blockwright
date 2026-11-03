// The sample projects (PLAN §12, samples.h): their notes' format, the
// variants a sample's notes describe, the list of samples in a folder, and
// the bundled samples themselves, held to the bar the Library's stamps are.

#include "blockwright/composite.h"
#include "blockwright/halfblock.h"
#include "blockwright/irc.h"
#include "blockwright/library.h"
#include "blockwright/project.h"
#include "blockwright/samples.h"
#include "blockwright/text_source.h"
#include "blockwright/text_style.h"

#include "../cli/lint.h"

#include <QByteArray>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <regex>
#include <set>
#include <string>
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
            CHECK(irc::overBudget(lines, saved.ircBudget.bytes).empty());
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
