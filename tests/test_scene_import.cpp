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

#include <QByteArray>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <thread>
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

// PATH set to `value` while it lives.
class PathGuard {
public:
    explicit PathGuard(const std::string& value) : had_(qEnvironmentVariableIsSet("PATH")), old_(qgetenv("PATH")) {
        qputenv("PATH", QByteArray::fromStdString(value));
    }
    PathGuard(const PathGuard&) = delete;
    PathGuard& operator=(const PathGuard&) = delete;
    ~PathGuard() {
        if (had_)
            qputenv("PATH", old_);
        else
            qunsetenv("PATH");
    }

private:
    bool had_;
    QByteArray old_;
};

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

TEST_CASE("a scene layer's command names the Python, the scene, the layer and its key", "[scene]") {
    CHECK(scene::layerCommand({"python3"}, "/art/scene.py", {"sky", {}}) ==
          std::vector<std::string>{"python3", "-B", "/art/scene.py", "--layer-ans=sky"});
    CHECK(scene::layerCommand({"python3"}, "/art/scene.py", {"sea", 13}) ==
          std::vector<std::string>{"python3", "-B", "/art/scene.py", "--layer-ans=sea", "--key=13"});
    CHECK(scene::layerCommand({"py", "-3"}, "C:\\art\\scene.py", {"sea", 13}) ==
          std::vector<std::string>{"py", "-3", "-B", "C:\\art\\scene.py", "--layer-ans=sea", "--key=13"});
}

TEST_CASE("Windows tries py -3, python, python3 for a scene; Linux and macOS python3", "[scene]") {
    CHECK(scene::pythonCandidates(true) == std::vector<scene::PythonCommand>{{"py", "-3"}, {"python"}, {"python3"}});
    CHECK(scene::pythonCandidates(false) == std::vector<scene::PythonCommand>{{"python3"}});
    CHECK(scene::pythonText({"py", "-3"}) == "py -3");
    CHECK(scene::pythonText({"python3"}) == "python3");
#ifdef _WIN32
    // what findPython tries is pythonCandidates(true): "Windows finds ..." below
#else
    // unprobed: found with nothing on PATH, as python3 always was
    const PathGuard path("");
    CHECK(scene::findPython() == scene::PythonCommand{"python3"});
#endif
}

TEST_CASE("choosePython takes the first Python the probe accepts and names each one it passed over", "[scene]") {
    const std::vector<scene::PythonCommand> all = scene::pythonCandidates(true);
    std::vector<scene::PythonCommand> asked;
    // which candidates work, by their text
    const auto probe = [&](std::set<std::string> working) {
        return [&asked, working](const scene::PythonCommand& python) -> std::optional<std::string> {
            asked.push_back(python);
            if (working.contains(scene::pythonText(python)))
                return std::nullopt;
            return scene::pythonText(python) + " --version: failed";
        };
    };
    CHECK(scene::choosePython(all, probe({"py -3", "python", "python3"})) == scene::PythonCommand{"py", "-3"});
    CHECK(asked == std::vector<scene::PythonCommand>{{"py", "-3"}});
    asked.clear();
    CHECK(scene::choosePython(all, probe({"python", "python3"})) == scene::PythonCommand{"python"});
    CHECK(asked == std::vector<scene::PythonCommand>{{"py", "-3"}, {"python"}});
    asked.clear();
    CHECK(scene::choosePython(all, probe({"python3"})) == scene::PythonCommand{"python3"});
    CHECK(asked == all);
    asked.clear();
    CHECK_THROWS_WITH(scene::choosePython(all, probe({})),
                      ContainsSubstring("no Python 3 to run the scene with; tried:\n- py -3 --version: failed\n"
                                        "- python --version: failed\n- python3 --version: failed") &&
                          ContainsSubstring("Install Python 3 from python.org"));
    CHECK(asked == all);
    CHECK_THROWS_AS(scene::choosePython(all, probe({})), scene::SceneError);
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
        CHECK(spec.command == scene::layerCommand(scene::findPython(), scenePath, {seen[i], key}));
    }

    // The layers composite to what the scene writes, run as it is.
    const fs::path flat = lighthouse("flat");
    QProcess py;
    py.setWorkingDirectory(QString::fromStdString(flat.string()));
    py.setStandardOutputFile(QProcess::nullDevice());
    py.setStandardErrorFile(QProcess::nullDevice());
    const scene::PythonCommand python = scene::findPython();
    QStringList args;
    for (std::size_t i = 1; i < python.size(); ++i)
        args << QString::fromStdString(python[i]);
    py.start(QString::fromStdString(python[0]), args << QStringLiteral("scene.py"));
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

// The Windows lookup run for real: probePython runs each candidate as a
// layer's run does, so stand-ins on PATH answer for py, python and python3.
// On Linux and macOS they are sh scripts; on Windows copies of bw-gen-helper
// (gen_helper_win.cpp), each doing what its <name>.mode file says.
namespace {

// What a stand-in does: a Python 3 (py's wants -3 first), the WindowsApps
// alias with no Store Python (9009 and a message on stderr), a Python 2
// (its version on stderr), or one that hangs (10 s, then nothing).
enum class Stand { Python3, Py3, Alias, Python2, Hang };

#ifdef _WIN32
fs::path helperExe() {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    REQUIRE((n > 0 && n < MAX_PATH));
    return fs::path(self).parent_path() / "bw-gen-helper.exe";
}
#endif

void standIn(const fs::path& dir, const std::string& name, Stand what) {
#ifdef _WIN32
    fs::copy_file(helperExe(), dir / (name + ".exe"), fs::copy_options::overwrite_existing);
    const char* mode = what == Stand::Python3 ? "python3"
                       : what == Stand::Py3   ? "py3"
                       : what == Stand::Alias ? "alias"
                       : what == Stand::Hang  ? "hang"
                                              : "python2";
    writeFile(dir / (name + ".mode"), mode);
#else
    // after -3 for py, the arguments: --version, or a scene's -B <scene>
    // --list-layers or --layer-ans=bg
    const std::string python3 = "case \"$*\" in\n"
                                "  --version) echo 'Python 3.12.1' ;;\n"
                                "  \"-B \"*\" --list-layers\") echo '{\"width\": 2, \"rows\": 1, \"title\": \"t\", "
                                "\"layers\": [{\"name\": \"bg\"}]}' ;;\n"
                                "  \"-B \"*\" --layer-ans=bg\") printf ab ;;\n"
                                "  *) echo \"unexpected: $*\" >&2; exit 64 ;;\n"
                                "esac\n";
    std::string body;
    switch (what) {
    case Stand::Python3:
        body = python3;
        break;
    case Stand::Py3:
        body = "[ \"$1\" = -3 ] || { echo 'no -3' >&2; exit 64; }\nshift\n" + python3;
        break;
    case Stand::Alias:
        body = "echo 'Python was not found; run without arguments to install from the Microsoft Store.' >&2\n"
               "exit 9009\n";
        break;
    case Stand::Python2:
        body = "echo 'Python 2.7.18' >&2\n";
        break;
    case Stand::Hang: {
        // by its path: the test's PATH names only the stand-ins
        const QString sleep = QStandardPaths::findExecutable(QStringLiteral("sleep"), {QStringLiteral("/usr/bin"),
                                                                                       QStringLiteral("/bin")});
        REQUIRE_FALSE(sleep.isEmpty());
        body = "exec '" + sleep.toStdString() + "' 10\n";
        break;
    }
    }
    writeFile(dir / name, "#!/bin/sh\n" + body);
    fs::permissions(dir / name, fs::perms::owner_all);
#endif
}

// PATH naming only `dir`, by the absolute path the platform looks programs
// up by (under wine a drive's: Z:\home\...).
std::string pathOf(const fs::path& dir) { return fs::absolute(dir).make_preferred().string(); }

std::optional<std::string> probe(const scene::PythonCommand& python) { return scene::probePython(python); }

scene::PythonCommand windowsChoice() { return scene::choosePython(scene::pythonCandidates(true), probe); }

} // namespace

TEST_CASE("probePython takes a Python 3 and refuses a missing one, the WindowsApps alias and a Python 2", "[scene]") {
    const fs::path dir = scratch("probe");
    const PathGuard path(pathOf(dir));
    standIn(dir, "py", Stand::Py3);
    standIn(dir, "python", Stand::Alias);
    standIn(dir, "python3", Stand::Python2);
    standIn(dir, "pythonw", Stand::Python3);
    CHECK(probe({"py", "-3"}) == std::nullopt);
    CHECK(probe({"pythonw"}) == std::nullopt);
    // py without -3 isn't asked for Python 3
    CHECK_THAT(probe({"py"}).value_or(""), ContainsSubstring("py --version") && ContainsSubstring("exited with status"));
    CHECK_THAT(probe({"python"}).value_or(""),
               ContainsSubstring("python --version") && ContainsSubstring("exited with status") &&
                   ContainsSubstring("Microsoft Store"));
    // Python 2 and 3.0-3.3 print their version on stderr: not called "not Python 3"
    CHECK(probe({"python3"}).value_or("") ==
          "python3 --version: printed nothing on stdout, where a Python 3.4 or later prints \"Python 3.\" and its "
          "version");
    CHECK_THAT(probe({"python9"}).value_or(""), ContainsSubstring("python9 --version: could not run python9"));
}

TEST_CASE("the Windows lookup picks py -3, then python, then python3, as they run", "[scene]") {
    const fs::path dir = scratch("choose");
    const PathGuard path(pathOf(dir));
    SECTION("py -3 first") {
        standIn(dir, "py", Stand::Py3);
        standIn(dir, "python", Stand::Python3);
        standIn(dir, "python3", Stand::Python3);
        CHECK(windowsChoice() == scene::PythonCommand{"py", "-3"});
    }
    SECTION("python.org without the launcher: python") {
        standIn(dir, "python", Stand::Python3);
        standIn(dir, "python3", Stand::Alias);
        CHECK(windowsChoice() == scene::PythonCommand{"python"});
    }
    SECTION("a Store Python: python3 past the alias python") {
        standIn(dir, "python", Stand::Alias);
        standIn(dir, "python3", Stand::Python3);
        CHECK(windowsChoice() == scene::PythonCommand{"python3"});
    }
    SECTION("a Python 2 python is passed over") {
        standIn(dir, "python", Stand::Python2);
        standIn(dir, "python3", Stand::Python3);
        CHECK(windowsChoice() == scene::PythonCommand{"python3"});
    }
    SECTION("only the aliases: none, and the message says what was tried and what to install") {
        standIn(dir, "python", Stand::Alias);
        standIn(dir, "python3", Stand::Alias);
        CHECK_THROWS_WITH(windowsChoice(),
                          ContainsSubstring("no Python 3 to run the scene with; tried:") &&
                              ContainsSubstring("- py -3 --version: could not run py") &&
                              ContainsSubstring("- python --version:") && ContainsSubstring("- python3 --version:") &&
                              ContainsSubstring("Microsoft Store") && ContainsSubstring("Install Python 3 from python.org"));
    }
}

TEST_CASE("a scene opened with the Python found stores it in each layer's generator.json", "[scene]") {
    const fs::path dir = scratch("stored");
    const PathGuard path(pathOf(dir));
    standIn(dir, "py", Stand::Py3);
    standIn(dir, "python", Stand::Alias);
    writeFile(dir / "scene.py", "# answered by the stand-in\n");
    const scene::PythonCommand python = windowsChoice();
    REQUIRE(python == scene::PythonCommand{"py", "-3"});
    project::Project p = scene::importScene((dir / "scene.py").string(), {}, {}, python);
    REQUIRE(p.layers.size() == 1);
    const std::string scenePath = fs::canonical(dir / "scene.py").string();
    CHECK(gen::layerSpec(p, p.layers[0].props.id).command ==
          std::vector<std::string>{"py", "-3", "-B", scenePath, "--layer-ans=bg"});
    CHECK(p.layers[0].cells.at(0, 0).ch == U'a');
    CHECK(p.layers[0].cells.at(1, 0).ch == U'b');
    // and the layer runs again the same way
    p.layers[0].cells.at(0, 0).ch = U'x';
    gen::refresh(p, p.layers[0].props.id);
    CHECK(p.layers[0].cells.at(0, 0).ch == U'a');
}

namespace {

// A program that runs 10 s unless killed, by an absolute path (found
// before a test's PATH names only its stand-ins).
std::vector<std::string> slowCommand() {
#ifdef _WIN32
    return {helperExe().string(), "sleep", "10000"};
#else
    const QString sleep = QStandardPaths::findExecutable(QStringLiteral("sleep"));
    REQUIRE_FALSE(sleep.isEmpty());
    return {sleep.toStdString(), "10"};
#endif
}

// Counts the programs runs start (generator::testing::started), and puts
// the hook back and lets runs start again after killRunning, however the
// test ends.
struct CountStarts {
    std::atomic<int> starts{0};
    CountStarts() {
        gen::testing::started = [this](pid_t) { ++starts; };
    }
    CountStarts(const CountStarts&) = delete;
    CountStarts& operator=(const CountStarts&) = delete;
    ~CountStarts() {
        gen::testing::started = nullptr;
        gen::testing::allowRuns();
    }
    bool waitFor(int n) const {
        for (int i = 0; i < 1000 && starts.load() < n; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return starts.load() >= n;
    }
};

// Runs `command` on a thread of its own (generator::runCommand, or
// probeCommand with `probe`), keeping how long it took and whether it
// threw.
struct Background {
    std::chrono::steady_clock::duration took{};
    std::string error;
    std::thread thread;
    Background(std::vector<std::string> command, bool probe) {
        thread = std::thread([this, command = std::move(command), probe] {
            const auto t0 = std::chrono::steady_clock::now();
            const gen::RunOptions slow{std::chrono::seconds(20)};
            try {
                if (probe)
                    gen::probeCommand(command, "probe", slow);
                else
                    gen::runCommand(command, {}, "generator", slow);
            } catch (const gen::GeneratorError& e) {
                error = e.what();
            }
            took = std::chrono::steady_clock::now() - t0;
        });
    }
    Background(const Background&) = delete;
    Background& operator=(const Background&) = delete;
    ~Background() {
        if (thread.joinable())
            thread.join();
    }
};

} // namespace

TEST_CASE("a Python probe while a generator runs leaves killRunning able to kill the generator",
          "[scene][generator][process]") {
    // The app probes for a Python on its UI thread while a generator may run
    // on another (another window's, or one that outlived its window): the
    // probe must not take the generator's place as the run killRunning
    // kills, or quitting waits out the generator.
    const std::vector<std::string> slow = slowCommand();
    const fs::path dir = scratch("probe-while-running");
    const PathGuard path(pathOf(dir));
    standIn(dir, "py", Stand::Py3);
    CountStarts count;
    Background generator(slow, false);
    REQUIRE(count.waitFor(1));
    // a probe that starts, answers and ends while the generator runs
    CHECK(probe({"py", "-3"}) == std::nullopt);
    REQUIRE(count.starts.load() == 2);
    const auto t0 = std::chrono::steady_clock::now();
    gen::killRunning();
    generator.thread.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    CHECK(generator.took < std::chrono::seconds(5));
    CHECK_FALSE(generator.error.empty());
}

TEST_CASE("a Python probe that hangs is stopped at its timeout, and killRunning kills it with a running generator",
          "[scene][generator][process]") {
    const std::vector<std::string> slow = slowCommand();
    const fs::path dir = scratch("probe-hangs");
    const PathGuard path(pathOf(dir));
    standIn(dir, "python", Stand::Hang);
    CountStarts count;
    SECTION("the timeout") {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_THAT(scene::probePython({"python"}, {std::chrono::milliseconds(300)}).value_or(""),
                   ContainsSubstring("python --version: python took longer than 300 ms; stopped"));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    }
    SECTION("killRunning, a generator running too") {
        Background generator(slow, false);
        REQUIRE(count.waitFor(1));
        Background hung({"python", "--version"}, true);
        REQUIRE(count.waitFor(2));
        gen::killRunning();
        generator.thread.join();
        hung.thread.join();
        CHECK(generator.took < std::chrono::seconds(5));
        CHECK_FALSE(generator.error.empty());
        CHECK(hung.took < std::chrono::seconds(5));
        CHECK_FALSE(hung.error.empty());
        // and none starts after
        CHECK_THAT(probe({"python"}).value_or(""), ContainsSubstring("not run: generators are being stopped"));
    }
}

#ifdef _WIN32
TEST_CASE("Windows finds py -3 on PATH for a scene and opens it with that", "[scene]") {
    const fs::path dir = scratch("windows");
    const PathGuard path(pathOf(dir));
    struct Forget {
        Forget() { scene::testing::forgetPython(); }
        ~Forget() { scene::testing::forgetPython(); }
    } forget;
    standIn(dir, "python3", Stand::Alias);
    CHECK_THROWS_WITH(scene::findPython(), ContainsSubstring("exited with status 9009") &&
                                               ContainsSubstring("Install Python 3 from python.org"));
    // a failure isn't kept: the next call looks again
    standIn(dir, "python", Stand::Python3);
    CHECK(scene::findPython() == scene::PythonCommand{"python"});
    // what was found is kept, unprobed, for the rest of the process
    standIn(dir, "py", Stand::Py3);
    CountStarts count;
    CHECK(scene::findPython() == scene::PythonCommand{"python"});
    CHECK(count.starts.load() == 0);
    scene::testing::forgetPython();
    CHECK(scene::findPython() == scene::PythonCommand{"py", "-3"});
    writeFile(dir / "scene.py", "# answered by the stand-in\n");
    const project::Project p = scene::importScene((dir / "scene.py").string());
    REQUIRE(p.layers.size() == 1);
    const std::vector<std::string> command = gen::layerSpec(p, p.layers[0].props.id).command;
    REQUIRE(command.size() == 5);
    CHECK(std::vector<std::string>(command.begin(), command.begin() + 3) == std::vector<std::string>{"py", "-3", "-B"});
    CHECK(p.layers[0].cells.at(0, 0).ch == U'a');
}
#endif
