// Opening a Python scene as a project (scene_import.h): reading a scene's
// layer list, running a command for its stdout (generator::runCommand), and
// importing tests/data/scenes/lighthouse, whose layers composite to what the
// scene itself writes.

#include "blockwright/composite.h"
#include "blockwright/generator.h"
#include "blockwright/project.h"
#include "blockwright/scene_import.h"

#include "scene_env.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QProcess>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace bw;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;
namespace gen = bw::generator;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "scene" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bytes;
}

// The fixture scene copied into a scratch directory of its own.
fs::path lighthouse(const std::string& name) {
    const fs::path dir = scratch(name);
    fs::copy_file(bwtest::dataDir() / "scenes" / "lighthouse" / "scene.py", dir / "scene.py");
    return dir;
}

// Every file under `dir` with its bytes.
std::set<std::pair<std::string, std::string>> contents(const fs::path& dir) {
    std::set<std::pair<std::string, std::string>> out;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir))
        out.emplace(fs::relative(e.path(), dir).string(), e.is_regular_file() ? bwtest::readFile(e.path()) : "");
    return out;
}

// A cell as it shows: a blank (space, NUL or 0xFF without blink) only its
// background, any other cell all of itself.
bool looksSame(const Cell& a, const Cell& b) {
    const auto blank = [](const Cell& c) {
        return (c.ch == U' ' || c.ch == U'\0' || c.ch == U' ') && !(c.attrs & attr::Blink);
    };
    if (blank(a) && blank(b))
        return a.bg.paletteIndex() == b.bg.paletteIndex();
    return a == b;
}

} // namespace

TEST_CASE("a scene's layer list is read strictly", "[scene]") {
    const scene::Listing l = scene::parseListing(
        R"({"width": 24, "rows": 8, "title": "lighthouse",
            "layers": [{"name": "sky"}, {"name": "sea", "key": 13}, {"name": "beam", "key": 0}]})");
    CHECK(l.width == 24);
    CHECK(l.rows == 8);
    CHECK(l.title == "lighthouse");
    CHECK(l.layers == std::vector<scene::SceneLayer>{{"sky", {}}, {"sea", 13}, {"beam", 0}});

    const auto refused = [](const std::string& json, const std::string& why) {
        INFO(json);
        CHECK_THROWS_WITH(scene::parseListing(json), ContainsSubstring(why));
    };
    const std::string layers = R"("layers": [{"name": "sky"}])";
    refused("not json", "not a JSON object");
    refused("[1]", "not a JSON object");
    refused(R"({"width": 0, "rows": 8, "title": "", )" + layers + "}", "no width");
    refused(R"({"width": 401, "rows": 8, "title": "", )" + layers + "}", "no width");
    refused(R"({"width": 2.5, "rows": 8, "title": "", )" + layers + "}", "no width");
    refused(R"({"width": 24, "rows": 5001, "title": "", )" + layers + "}", "rows 1-5000");
    refused(R"({"width": 24, "rows": 8, )" + layers + "}", "no title");
    refused(R"({"width": 24, "rows": 8, "title": 3, )" + layers + "}", "no title");
    refused(R"({"width": 24, "rows": 8, "title": ""})", "names no layers");
    refused(R"({"width": 24, "rows": 8, "title": "", "layers": []})", "names no layers");
    refused(R"({"width": 24, "rows": 8, "title": "", "layers": [{"name": ""}]})", "has no name");
    refused(R"({"width": 24, "rows": 8, "title": "", "layers": ["sky"]})", "has no name");
    refused(R"({"width": 24, "rows": 8, "title": "", "layers": [{"name": "sky", "key": 1}]})",
            "bottom layer sky has a key");
    refused(R"({"width": 24, "rows": 8, "title": "", "layers": [{"name": "sky"}, {"name": "sea"}]})",
            "layer sea has no key colour 0-15");
    refused(R"({"width": 24, "rows": 8, "title": "", "layers": [{"name": "sky"}, {"name": "sea", "key": 16}]})",
            "layer sea has no key colour 0-15");
    refused(R"({"width": 24, "rows": 8, "title": "", "layers": [{"name": "sky"}, {"name": "sky", "key": 1}]})",
            "names layer sky twice");
}

TEST_CASE("a scene layer's command names the scene, the layer and its key", "[scene]") {
    CHECK(scene::layerCommand("/art/scene.py", {"sky", {}}) ==
          std::vector<std::string>{"python3", "-B", "/art/scene.py", "--layer-ans=sky"});
    CHECK(scene::layerCommand("/art/scene.py", {"sea", 13}) ==
          std::vector<std::string>{"python3", "-B", "/art/scene.py", "--layer-ans=sea", "--key=13"});
}

TEST_CASE("runCommand returns a program's stdout and refuses as run does", "[scene]") {
#ifdef _WIN32
    SKIP("runs sh and cat; test_generator_win.cpp runs programs on Windows");
#endif
    CHECK(gen::runCommand({"cat"}, "hello", "echo") == "hello");
    // a new empty directory to run in
    CHECK(gen::runCommand({"sh", "-c", "ls -A | wc -l | tr -d ' '"}, {}, "ls") == "0\n");
    CHECK_THROWS_WITH(gen::runCommand({}, {}, "none"), ContainsSubstring("none"));
    CHECK_THROWS_AS(gen::runCommand({"sh", std::string("a\0b", 3)}, {}, "nul"), gen::GeneratorError);
    CHECK_THROWS_AS(gen::runCommand({"./gen.sh"}, {}, "relative"), gen::GeneratorError);
    CHECK_THROWS_WITH(gen::runCommand({"sh", "-c", "echo broken >&2; exit 3"}, {}, "failing"),
                      ContainsSubstring("failing") && ContainsSubstring("broken"));
    gen::RunOptions quick;
    quick.timeout = std::chrono::milliseconds(200);
    CHECK_THROWS_AS(gen::runCommand({"sleep", "5"}, {}, "slow", quick), gen::GeneratorError);
}

TEST_CASE("a scene that can't list its layers is refused", "[scene]") {
    const fs::path dir = scratch("refused");
    CHECK_THROWS_WITH(scene::importScene((dir / "missing.py").string()), ContainsSubstring("is not a file"));
    CHECK_THROWS_WITH(scene::importScene(dir.string()), ContainsSubstring("is not a file"));
    if (const std::string why = bwtest::sceneSkipReason(); !why.empty())
        SKIP(why);
    writeFile(dir / "broken.py", "import sys\nsys.exit('no layers here')\n");
    CHECK_THROWS_WITH(scene::importScene((dir / "broken.py").string()),
                      ContainsSubstring("listing its layers") && ContainsSubstring("no layers here"));
    writeFile(dir / "chatty.py", "print('not json')\n");
    CHECK_THROWS_WITH(scene::importScene((dir / "chatty.py").string()), ContainsSubstring("not a JSON object"));
    // a layer that fails names it
    writeFile(dir / "half.py", "import sys\n"
                               "if '--list-layers' in sys.argv:\n"
                               "    print('{\"width\": 4, \"rows\": 1, \"title\": \"\", \"layers\": "
                               "[{\"name\": \"ground\"}]}')\n"
                               "else:\n"
                               "    sys.exit('cannot draw ' + sys.argv[1])\n");
    CHECK_THROWS_WITH(scene::importScene((dir / "half.py").string()), ContainsSubstring("--layer-ans=ground"));
}

TEST_CASE("a scene opens as one generator layer per scene layer", "[scene]") {
    if (const std::string why = bwtest::sceneSkipReason(); !why.empty())
        SKIP(why);
    const fs::path dir = lighthouse("import");
    const auto before = contents(dir);
    std::vector<std::string> seen;
    const project::Project p = scene::importScene(
        (dir / "scene.py").string(), {},
        [&](std::size_t index, std::size_t count, const std::string& name) {
            CHECK(index == seen.size());
            CHECK(count == 4);
            seen.push_back(name);
        });
    CHECK(seen == std::vector<std::string>{"sky", "sea", "tower", "beam"});
    // nothing written beside the scene
    CHECK(contents(dir) == before);

    CHECK(p.layers.width() == 24);
    CHECK(p.layers.height() == 8);
    CHECK(p.ice);
    REQUIRE(p.sauce);
    CHECK(p.sauce->title == "lighthouse");
    REQUIRE(p.layers.size() == 4);
    const std::string scenePath = fs::canonical(dir / "scene.py").string();
    for (std::size_t i = 0; i < 4; ++i) {
        const Layer& l = p.layers[i];
        INFO(l.props.name);
        CHECK(l.props.name == seen[i]);
        CHECK(l.props.kind == LayerKind::Generator);
        const gen::Spec spec = gen::layerSpec(p, l.props.id);
        const std::optional<std::uint8_t> key = i == 0 ? std::nullopt : std::optional<std::uint8_t>(13);
        CHECK(spec.key == key);
        CHECK(spec.command == scene::layerCommand(scenePath, {seen[i], key}));
    }

    // The layers composite to what the scene writes, run as it is.
    const fs::path flat = lighthouse("flat");
    QProcess py;
    py.setWorkingDirectory(QString::fromStdString(flat.string()));
    py.setStandardOutputFile(QProcess::nullDevice());
    py.setStandardErrorFile(QProcess::nullDevice());
    py.start(QStringLiteral("python3"), {QStringLiteral("scene.py")});
    REQUIRE(py.waitForFinished(120'000));
    REQUIRE(py.exitCode() == 0);
    const Canvas want = gen::cellsFromOutput(bwtest::readFile(flat / "lighthouse.ans"), 24, std::nullopt);
    const Canvas got = composite(p.layers, {p.ice, Cell{}});
    REQUIRE(got.width() == 24);
    REQUIRE(got.height() == 8);
    int differ = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 24; ++x) {
            // ansidraw trims a row's trailing black blanks, so a short row
            // is black past its end
            const Cell w = x < want.width() && y < want.height() ? want.at(x, y) : Cell{};
            if (!looksSame(got.at(x, y), w)) {
                UNSCOPED_INFO("cell " << x << "," << y);
                ++differ;
            }
        }
    CHECK(differ == 0);

    // Each layer runs again from the project, as a refresh would.
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(gen::run(p, p.layers[i].props.id) == p.layers[i].cells);
}

TEST_CASE("a scene's imports write no bytecode beside it", "[scene]") {
    if (const std::string why = bwtest::sceneSkipReason(); !why.empty())
        SKIP(why);
    qunsetenv("PYTHONDONTWRITEBYTECODE");
    const fs::path dir = lighthouse("bytecode");
    // the scene draws its beam from a module beside it, imported fresh
    writeFile(dir / "helper.py", "BEAM = '~fp~'\n");
    std::string src = bwtest::readFile(dir / "scene.py");
    const std::string from = "import ansidraw as D  # noqa: E402\n";
    const std::size_t at = src.find(from);
    REQUIRE(at != std::string::npos);
    src.insert(at + from.size(), "import helper  # noqa: E402\n");
    writeFile(dir / "scene.py", src);
    const auto before = contents(dir);
    project::Project p = scene::importScene((dir / "scene.py").string());
    REQUIRE(p.layers.size() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        gen::refresh(p, p.layers[i].props.id);
    CHECK_FALSE(fs::exists(dir / "__pycache__"));
    CHECK(contents(dir) == before);
}

TEST_CASE("a scene layer whose key the scene now uses fails and says so", "[scene]") {
    if (const std::string why = bwtest::sceneSkipReason(); !why.empty())
        SKIP(why);
    const fs::path dir = lighthouse("rekey");
    project::Project p = scene::importScene((dir / "scene.py").string());
    // the beam now writes a blank on its key colour, light magenta
    std::string src = bwtest::readFile(dir / "scene.py");
    const std::string from = R"(c.put_text(1, 1, "~fp~", D.YELLOW))";
    const std::size_t at = src.find(from);
    REQUIRE(at != std::string::npos);
    src.replace(at, from.size(), from + "\n    c.put_text(8, 1, \" \", D.WHITE, D.LMAGENTA)");
    writeFile(dir / "scene.py", src);
    CHECK_THROWS_WITH(gen::refresh(p, p.layers[3].props.id), ContainsSubstring("column 8, row 1") && ContainsSubstring("open the scene again"));
    // opened again, it picks another key
    const project::Project again = scene::importScene((dir / "scene.py").string());
    const gen::Spec spec = gen::layerSpec(again, again.layers[3].props.id);
    REQUIRE(spec.key);
    CHECK(*spec.key != 13);
}
