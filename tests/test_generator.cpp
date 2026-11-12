// Generator layers (generator.h): the spec and its input, reading the
// output, and running a program for a layer's cells; the CLI's reading of a
// generator's directory (cli/generators.h).

#include "blockwright/ansi.h"
#include "blockwright/composite.h"
#include "blockwright/generator.h"
#include "blockwright/palette.h"
#include "blockwright/project.h"

#include "generators.h"
#include "read_file.h"
#include "fs_quirks.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QDir>
#include <QLibraryInfo>
#include <QVersionNumber>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace bw;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
namespace fs = std::filesystem;
namespace gen = bw::generator;

namespace {

using Files = std::map<std::string, std::string>;

// A 4x2 project with one cells layer of dots.
project::Project dotsProject() {
    Canvas c(4, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            c.at(x, y) = Cell{U'.', Color::indexed(7), Color::indexed(0), 0, false};
    AnsiDocument doc;
    doc.canvas = c;
    doc.ice = false;
    return project::fromAnsi(doc);
}

// A generator whose gen.sh is `script`, run by sh.
Files shGenerator(const std::string& script, const std::string& extraSpec = "") {
    return {{"generator.json", R"({"command": ["sh", "gen.sh"])" + extraSpec + "}"}, {"gen.sh", script}};
}

const gen::RunOptions quick{std::chrono::milliseconds(10'000)};

QJsonObject jsonObject(const std::string& s) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();
}

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "generator" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bytes;
}

#ifndef _WIN32
// The most memory the process has held, in bytes (Linux reports KiB).
long long peakRss() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return (long long)u.ru_maxrss; // bytes on macOS
#else
    return (long long)u.ru_maxrss * 1024; // KiB on Linux
#endif
}
#endif

} // namespace

TEST_CASE("generator.json is read strictly and written back as it reads", "[generator]") {
    const gen::Spec s = gen::parseSpec(R"({"command": ["python3", "scene.py", "--only", "sky"], "key": 0})");
    CHECK(s.command == std::vector<std::string>{"python3", "scene.py", "--only", "sky"});
    CHECK(s.key == std::uint8_t{0});
    CHECK(gen::parseSpec(gen::specJson(s)) == s);

    // no key, a null key and keys it doesn't know
    CHECK_FALSE(gen::parseSpec(R"({"command": ["x"]})").key);
    CHECK_FALSE(gen::parseSpec(R"({"command": ["x"], "key": null, "newer": {"a": 1}})").key);
    const gen::Spec noKey{{"./gen.sh"}, std::nullopt};
    CHECK(gen::parseSpec(gen::specJson(noKey)) == noKey);
    CHECK(gen::parseSpec(R"({"command": ["x"], "key": 15})").key == std::uint8_t{15});

    for (const char* bad : {
             "",
             "[]",
             "not json",
             R"({})",
             R"({"command": "sh gen.sh"})",
             R"({"command": []})",
             R"({"command": [""]})",
             R"({"command": ["sh", 3]})",
             R"({"command": ["sh"], "key": 16})",
             R"({"command": ["sh"], "key": -1})",
             R"({"command": ["sh"], "key": 1.5})",
             R"({"command": ["sh"], "key": "4"})",
             R"({"command": ["sh"], "command": ["rm"]})",
             R"({"command": ["sh"], "x": {"a": 1, "a": 2}})",
         })
        CHECK_THROWS_AS(gen::parseSpec(bad), gen::GeneratorError);
    CHECK_THROWS_AS(gen::parseSpec(std::string(R"({"command": ["sh\u0000x"]})")), gen::GeneratorError);
    // over the JSON limit projects read by
    const std::string big = R"({"command": ["sh"], "pad": ")" + std::string(project::kMaxJsonBytes, 'x') + "\"}";
    CHECK_THROWS_AS(gen::parseSpec(big), gen::GeneratorError);
}

TEST_CASE("a generator's declared params are read strictly, with defaults, and written back", "[generator][params]") {
    const gen::Spec s = gen::parseSpec(R"({"command": ["sh", "gen.sh"], "params": [
        {"name": "count", "type": "integer", "label": "Stars", "min": 1, "max": 9, "default": 3},
        {"name": "density", "type": "number", "min": 0, "max": 1, "step": 0.05, "default": 0.25},
        {"name": "twinkle", "type": "boolean", "default": true},
        {"name": "ink", "type": "colour", "default": 14},
        {"name": "sky", "type": "ramp", "default": "sea"},
        {"name": "title", "type": "text", "default": "night", "newer": [1]},
        {"name": "n", "type": "integer", "min": 2},
        {"name": "neg", "type": "number", "max": -3},
        {"name": "b", "type": "boolean"}, {"name": "c", "type": "colour"},
        {"name": "r", "type": "ramp"}, {"name": "t", "type": "text"}
    ]})");
    REQUIRE(s.params.size() == 12);
    const gen::Param& count = s.params[0];
    CHECK(count.name == "count");
    CHECK(count.type == gen::ParamType::Integer);
    CHECK(count.label == "Stars");
    CHECK(count.min == 1.0);
    CHECK(count.max == 9.0);
    CHECK_FALSE(count.step);
    CHECK(count.defaultValue == gen::ParamValue(3.0));
    CHECK(s.params[1].step == 0.05);
    CHECK(s.params[2].defaultValue == gen::ParamValue(true));
    CHECK(s.params[3].defaultValue == gen::ParamValue(14.0));
    CHECK(s.params[4].defaultValue == gen::ParamValue(std::string("sea")));
    CHECK(s.params[5].defaultValue == gen::ParamValue(std::string("night")));
    // without a default: the minimum (or a maximum below 0) or 0, false,
    // light grey, the first named ramp, ""
    CHECK(s.params[6].defaultValue == gen::ParamValue(2.0));
    CHECK(s.params[7].defaultValue == gen::ParamValue(-3.0));
    CHECK(s.params[8].defaultValue == gen::ParamValue(false));
    CHECK(s.params[9].defaultValue == gen::ParamValue(7.0));
    CHECK(s.params[10].defaultValue == gen::ParamValue(std::string("fire")));
    CHECK(s.params[11].defaultValue == gen::ParamValue(std::string()));
    CHECK(gen::parseSpec(gen::specJson(s)) == s);
    CHECK(gen::parseSpec(R"({"command": ["x"], "params": []})").params.empty());

    for (const char* bad : {
             R"({"command": ["x"], "params": {}})",
             R"({"command": ["x"], "params": [1]})",
             R"({"command": ["x"], "params": [{"type": "text"}]})",
             R"({"command": ["x"], "params": [{"name": "", "type": "text"}]})",
             R"({"command": ["x"], "params": [{"name": 3, "type": "text"}]})",
             R"({"command": ["x"], "params": [{"name": "a"}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "float"}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "text"}, {"name": "a", "type": "text"}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "text", "label": 1}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "integer", "min": 1.5}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "integer", "max": "9"}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "number", "min": 2, "max": 1}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "text", "min": 0}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "integer", "step": 1}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "number", "step": 0}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "integer", "default": 2.5}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "integer", "max": 3, "default": 4}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "number", "min": 0, "default": -1}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "number", "default": "1"}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "boolean", "default": 1}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "colour", "default": 16}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "colour", "default": 2.5}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "ramp", "default": "plaid"}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "text", "default": 3}]})",
             R"({"command": ["x"], "params": [{"name": "a", "type": "text", "type": "integer"}]})",
         })
        CHECK_THROWS_AS(gen::parseSpec(bad), gen::GeneratorError);
    CHECK_THROWS_WITH(gen::parseSpec(R"({"command": ["x"], "params": [{"name": "a", "type": "integer", "max": 3,
                                        "default": 4}]})"),
                      ContainsSubstring("parameter \"a\": \"default\" is above the maximum, 3"));
}

TEST_CASE("an integer param stays within what the form's fields hold and is written without narrowing",
          "[generator][params]") {
    // the form's integer fields are 32-bit: a bound, a default or a value past
    // them is refused rather than shown clamped
    const gen::Spec edges = gen::parseSpec(R"({"command": ["x"], "params": [
        {"name": "n", "type": "integer", "min": -2147483648, "max": 2147483647, "default": 2147483647}]})");
    CHECK(edges.params[0].min == -2147483648.0);
    CHECK(edges.params[0].max == 2147483647.0);
    CHECK(gen::parseSpec(gen::specJson(edges)) == edges);
    for (const char* bad : {
             R"({"command": ["x"], "params": [{"name": "n", "type": "integer", "default": 1e20}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "integer", "default": 2147483648}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "integer", "min": -2147483649}]})",
             R"({"command": ["x"], "params": [{"name": "seed", "type": "integer",
                 "min": 3000000000, "max": 3000000010, "default": 3000000000}]})",
         })
        CHECK_THROWS_AS(gen::parseSpec(bad), gen::GeneratorError);
    CHECK_THROWS_WITH(gen::parseSpec(R"({"command": ["x"], "params": [{"name": "seed", "type": "integer",
                                        "min": 3000000000}]})"),
                      ContainsSubstring("\"min\" is not a whole number from -2147483648 to 2147483647"));
    CHECK_THROWS_WITH(gen::parseSpec(R"({"command": ["x"], "params": [{"name": "n", "type": "integer",
                                        "default": 1e20}]})"),
                      ContainsSubstring("\"default\" is outside an integer's range, -2147483648 to 2147483647"));

    const gen::Spec s = gen::parseSpec(R"({"command": ["x"], "params": [{"name": "n", "type": "integer"}]})");
    CHECK_THROWS_AS(gen::withParam(s, "", "n", 3e9), gen::GeneratorError);
    CHECK_THROWS_AS(gen::withParam(s, "", "n", 1e20), gen::GeneratorError);
    CHECK(gen::paramValues(s, R"({"n": 1e20})").at("n") == gen::ParamValue(0.0));
    CHECK(jsonObject(gen::withParam(s, "", "n", -2147483648.0)) == jsonObject(R"({"n": -2147483648})"));

    // a spec built by hand with a value no parse would take is still written
    // as it is, not narrowed
    gen::Spec hand = s;
    hand.params[0].defaultValue = 1e20;
    hand.params[0].max = 1e20;
    const QJsonObject written = jsonObject(gen::specJson(hand))[QStringLiteral("params")][0].toObject();
    CHECK(written[QStringLiteral("default")].toDouble() == 1e20);
    CHECK(written[QStringLiteral("max")].toDouble() == 1e20);
}

TEST_CASE("a number param stays within what the form's fields hold", "[generator][params]") {
    // the form's number fields hold -1e12 to 1e12 to six decimal places: a
    // bound, a step, a default or a value past them is refused rather than
    // shown rounded
    const gen::Spec edges = gen::parseSpec(R"({"command": ["x"], "params": [
        {"name": "x", "type": "number", "min": -1e12, "max": 1e12, "step": 0.000001, "default": 0.123456}]})");
    CHECK(edges.params[0].min == -1e12);
    CHECK(edges.params[0].max == 1e12);
    CHECK(edges.params[0].step == 0.000001);
    CHECK(gen::parseSpec(gen::specJson(edges)) == edges);
    for (const char* bad : {
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "min": 0.00000001,
                 "max": 0.00000002, "step": 0.000000001, "default": 0.000000015}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "default": 1e20}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "max": 1000000000001}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "min": -1e13}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "min": 0.0000001}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "step": 0.0000005}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "step": 1e13}]})",
             R"({"command": ["x"], "params": [{"name": "n", "type": "number", "default": 0.1234567}]})",
         })
        CHECK_THROWS_AS(gen::parseSpec(bad), gen::GeneratorError);
    CHECK_THROWS_WITH(gen::parseSpec(R"({"command": ["x"], "params": [{"name": "n", "type": "number",
                                        "min": 0.00000001}]})"),
                      ContainsSubstring("\"min\" is not a number from -1e+12 to 1e+12 with at most six decimal places"));
    CHECK_THROWS_WITH(gen::parseSpec(R"({"command": ["x"], "params": [{"name": "n", "type": "number",
                                        "step": 0.000000001}]})"),
                      ContainsSubstring("\"step\" is not a number above 0"));
    CHECK_THROWS_WITH(gen::parseSpec(R"({"command": ["x"], "params": [{"name": "n", "type": "number",
                                        "default": 1e20}]})"),
                      ContainsSubstring("\"default\" is outside a number's range, -1e+12 to 1e+12"));

    const gen::Spec s = gen::parseSpec(R"({"command": ["x"], "params": [{"name": "n", "type": "number"}]})");
    CHECK_THROWS_AS(gen::withParam(s, "", "n", 1e20), gen::GeneratorError);
    CHECK_THROWS_WITH(gen::withParam(s, "", "n", 0.1234567), ContainsSubstring("has more than six decimal places"));
    CHECK(gen::paramValues(s, R"({"n": 1e20})").at("n") == gen::ParamValue(0.0));
    CHECK(gen::paramValues(s, R"({"n": 0.1234567})").at("n") == gen::ParamValue(0.0));
    CHECK(gen::paramValues(s, R"({"n": -1e12})").at("n") == gen::ParamValue(-1e12));
    CHECK(jsonObject(gen::withParam(s, "", "n", 0.3)) == jsonObject(R"({"n": 0.3})"));
}

TEST_CASE("params.json gives the declared params' values and is written with every one", "[generator][params]") {
    const gen::Spec s = gen::parseSpec(R"({"command": ["x"], "params": [
        {"name": "count", "type": "integer", "min": 1, "max": 9, "default": 3},
        {"name": "ink", "type": "colour"}, {"name": "sky", "type": "ramp"}]})");
    using V = std::map<std::string, gen::ParamValue>;
    CHECK(gen::paramValues(s, "") == V{{"count", 3.0}, {"ink", 7.0}, {"sky", std::string("fire")}});
    // a value the param can't take shows its default
    CHECK(gen::paramValues(s, R"({"count": 5, "ink": "red", "sky": "plaid", "other": 1})") ==
          V{{"count", 5.0}, {"ink", 7.0}, {"sky", std::string("fire")}});
    CHECK_THROWS_AS(gen::paramValues(s, "[]"), gen::GeneratorError);
    CHECK_THROWS_AS(gen::paramValues(s, R"({"count": 1, "count": 2})"), gen::GeneratorError);

    const std::string json = gen::withParam(s, R"({"other": [1, 2], "count": 4})", "ink", 12.0);
    CHECK(jsonObject(json) == jsonObject(R"({"other": [1, 2], "count": 4, "ink": 12, "sky": "fire"})"));
    CHECK(gen::paramValues(s, json) == V{{"count", 4.0}, {"ink", 12.0}, {"sky", std::string("fire")}});
    CHECK_THROWS_WITH(gen::withParam(s, "", "nope", 1.0), ContainsSubstring("no parameter \"nope\""));
    CHECK_THROWS_WITH(gen::withParam(s, "", "count", 10.0), ContainsSubstring("above the maximum, 9"));
    CHECK_THROWS_AS(gen::withParam(s, "", "count", 2.5), gen::GeneratorError);
    CHECK_THROWS_AS(gen::withParam(s, "", "sky", 1.0), gen::GeneratorError);
    CHECK_THROWS_AS(gen::withParam(s, "", "ink", true), gen::GeneratorError);
}

TEST_CASE("a generator's files are under scripts/<id>/; the stack lists its generators bottom first",
          "[generator]") {
    CHECK(gen::directory(7) == "scripts/7/");
    project::Project p = dotsProject();
    const LayerId a = p.layers.add(makeLayer(LayerKind::Generator, 4, 2, "a"));
    p.layers.add(makeLayer(LayerKind::Cells, 4, 2, "c"));
    const LayerId b = p.layers.add(makeLayer(LayerKind::Generator, 4, 2, "b"));
    CHECK(gen::generatorLayers(p.layers) == std::vector<LayerId>{a, b});

    // a layer's spec comes from its files
    CHECK_THROWS_WITH(gen::layerSpec(p, a), ContainsSubstring("has no scripts/" + std::to_string(a) +
                                                              "/generator.json"));
    p.files[gen::directory(a) + "generator.json"] = R"({"command": ["sh"], "key": 3})";
    CHECK(gen::layerSpec(p, a) == gen::Spec{{"sh"}, std::uint8_t{3}});
    p.files[gen::directory(a) + "generator.json"] = R"({"command": 1})";
    CHECK_THROWS_WITH(gen::layerSpec(p, a), ContainsSubstring("scripts/" + std::to_string(a) + "/generator.json"));
    CHECK_THROWS_WITH(gen::layerSpec(p, 99), ContainsSubstring("no layer 99"));
    CHECK_THROWS_WITH(gen::layerSpec(p, p.layers.layers().front().props.id), ContainsSubstring("not a generator"));
}

TEST_CASE("a generator reads the canvas, its layer and its params on stdin", "[generator]") {
    project::Project p = dotsProject();
    p.ice = false;
    Layer l = makeLayer(LayerKind::Generator, 3, 1, "stars \"x\"", -2, 1);
    const LayerId id = p.layers.add(l);
    const Layer& added = *p.layers.find(id);

    std::string in = gen::input(p, added);
    REQUIRE(in.ends_with("\n"));
    CHECK(in.find('\n') == in.size() - 1);  // one line
    QJsonObject o = jsonObject(in);
    CHECK(o.value("canvas").toObject() == QJsonObject{{"width", 4}, {"height", 2}, {"ice", false}});
    CHECK(o.value("layer").toObject() == QJsonObject{{"id", qint64(id)},
                                                     {"name", "stars \"x\""},
                                                     {"x", -2},
                                                     {"y", 1},
                                                     {"width", 3},
                                                     {"height", 1}});
    CHECK(o.value("params").toObject().isEmpty());

    p.files[gen::directory(id) + "params.json"] = R"({"density": 0.25, "colours": [9, 11], "seed": "abc"})";
    o = jsonObject(gen::input(p, added));
    CHECK(o.value("params").toObject() == jsonObject(R"({"density": 0.25, "colours": [9, 11], "seed": "abc"})"));

    for (const char* bad : {"[1, 2]", "nope", R"({"a": 1, "a": 2})"}) {
        p.files[gen::directory(id) + "params.json"] = bad;
        CHECK_THROWS_WITH(gen::input(p, added), ContainsSubstring("params.json"));
    }

    // declared params: a missing one gets its default, a bad one is refused
    p.files[gen::directory(id) + "generator.json"] = R"({"command": ["x"], "params": [
        {"name": "count", "type": "integer", "max": 5, "default": 2}, {"name": "on", "type": "boolean"}]})";
    p.files[gen::directory(id) + "params.json"] = R"({"count": 4, "extra": "kept"})";
    o = jsonObject(gen::input(p, added));
    CHECK(o.value("params").toObject() == jsonObject(R"({"count": 4, "on": false, "extra": "kept"})"));
    for (const char* bad : {R"({"count": 6})", R"({"count": 1.5})", R"({"on": 1})"}) {
        p.files[gen::directory(id) + "params.json"] = bad;
        CHECK_THROWS_WITH(gen::input(p, added), ContainsSubstring("params.json"));
    }
    p.files[gen::directory(id) + "params.json"] = "{}";
    p.files[gen::directory(id) + "generator.json"] = R"({"command": ["x"], "params": 1})";
    CHECK_THROWS_WITH(gen::input(p, added), ContainsSubstring("generator.json"));
}

TEST_CASE("a generator's output is read as an .ans; the key colour's blank cells are transparent",
          "[generator]") {
    // blue spaces, then red "AB" on blue, black on black, at 6 columns; the
    // key is a palette index as cells hold them (blue is 1, as in SGR 44)
    const std::string ans = "\x1b[0;44m  \x1b[31mAB\x1b[0m  \r\n";

    const Canvas opaque = gen::cellsFromOutput(ans, 6, std::nullopt);
    CHECK(opaque.width() == 6);
    CHECK(opaque.height() == 1);
    for (int x = 0; x < 6; ++x)
        CHECK_FALSE(opaque.at(x, 0).transparent);  // black-on-black space is opaque
    CHECK(opaque.at(2, 0).ch == U'A');

    const Canvas keyed = gen::cellsFromOutput(ans, 6, std::uint8_t{Blue});
    CHECK(keyed.at(0, 0).transparent);
    CHECK(keyed.at(1, 0).transparent);
    CHECK(keyed.at(2, 0) == opaque.at(2, 0));  // a glyph on the key colour stays
    CHECK(keyed.at(3, 0) == opaque.at(3, 0));
    CHECK(keyed.at(4, 0) == opaque.at(4, 0));  // black isn't the key
    const Canvas blackKey = gen::cellsFromOutput(ans, 6, std::uint8_t{0});
    CHECK(blackKey.at(4, 0).transparent);
    CHECK(blackKey.at(5, 0).transparent);
    CHECK_FALSE(blackKey.at(0, 0).transparent);

    // NUL and 0xFF show as blank too; a blinking blank doesn't
    const Canvas blanks =
        gen::cellsFromOutput(std::string("\x1b[44m\0\xff", 7) + "\x1b[5m \r\n", 3, std::uint8_t{Blue});
    CHECK(blanks.at(0, 0).transparent);
    CHECK(blanks.at(1, 0).transparent);
    CHECK_FALSE(blanks.at(2, 0).transparent);

    // with iCE on (its SAUCE says so) blink is a bright background: the key
    // is the bright colour
    AnsiDocument doc;
    doc.canvas = Canvas(3, 1);
    doc.canvas.at(0, 0) = Cell{U' ', Color::indexed(7), Color::indexed(12), 0, false};
    doc.canvas.at(1, 0) = Cell{U'x', Color::indexed(7), Color::indexed(12), 0, false};
    doc.canvas.at(2, 0) = Cell{U' ', Color::indexed(7), Color::indexed(4), 0, false};
    doc.ice = true;
    const std::string iceAns = writeAnsi(doc).bytes;
    const Canvas ice = gen::cellsFromOutput(iceAns, 80, std::uint8_t{12});
    CHECK(ice.width() == 3);  // the SAUCE width, not the one asked for
    CHECK(ice.at(0, 0).transparent);
    CHECK_FALSE(ice.at(1, 0).transparent);
    CHECK_FALSE(ice.at(2, 0).transparent);

    // the width is asked for when there's no SAUCE, and wraps there
    const Canvas narrow = gen::cellsFromOutput("ABCDE", 2, std::nullopt);
    CHECK(narrow.width() == 2);
    CHECK(narrow.height() == 3);
    CHECK(gen::cellsFromOutput("A", 0, std::nullopt).width() == 1);
}

// The runs below use sh; test_generator_win.cpp runs generators on Windows.
#ifndef _WIN32
TEST_CASE("running a generator makes the layer's cells from its stdout", "[generator][run]") {
    project::Project p = dotsProject();
    const project::Project before = p;
    const LayerId id = gen::add(p, shGenerator("printf '\\033[0;44m  \\033[31mAB'\n", R"(, "key": 1)"), "sky", quick);
    REQUIRE(p.layers.size() == 2);
    const Layer& l = *p.layers.find(id);
    CHECK(l.props.kind == LayerKind::Generator);
    CHECK(l.props.name == "sky");
    CHECK(p.layers.layers().back().props.id == id);  // on top
    CHECK(l.width() == 4);
    CHECK(l.height() == 1);
    CHECK(l.cells.at(0, 0).transparent);
    CHECK(l.cells.at(2, 0).ch == U'A');
    CHECK(p.files.at(gen::directory(id) + "gen.sh") == "printf '\\033[0;44m  \\033[31mAB'\n");
    CHECK(p.files.size() == before.files.size() + 2);

    // the composite shows the dots through the keyed cells
    const Canvas shown = composite(p.layers);
    CHECK(shown.at(0, 0).ch == U'.');
    CHECK(shown.at(2, 0).ch == U'A');
    CHECK(shown.at(2, 1).ch == U'.');

    // run returns the cells and leaves the project alone; refresh keeps the
    // layer's place and properties
    p.layers.find(id)->props.x = 1;
    p.layers.find(id)->props.locked = true;
    const project::Project moved = p;
    CHECK(gen::run(p, id, quick) == moved.layers.find(id)->cells);
    CHECK(p == moved);
    p.files[gen::directory(id) + "gen.sh"] = "printf 'XYZ\\r\\nQ'\n";
    gen::refresh(p, id, quick);
    const Layer& r = *p.layers.find(id);
    CHECK(r.props.x == 1);
    CHECK(r.props.locked);
    CHECK(r.width() == 4);  // read at the layer's width
    CHECK(r.height() == 2);
    CHECK(r.cells.at(2, 0).ch == U'Z');
    CHECK(r.cells.at(0, 1).ch == U'Q');
}

TEST_CASE("a generator gets its params, runs in a copy of its files and leaves the project's bytes alone",
          "[generator][run]") {
    project::Project p = dotsProject();
    Files files = shGenerator(
        // the params reach it, its other files are beside it, and what it
        // writes there stays in the run's copy
        "in=$(cat)\n"
        "case \"$in\" in *'\"word\":\"ok\"'*) ;; *) echo \"bad input: $in\" >&2; exit 3;; esac\n"
        "case \"$in\" in *'\"canvas\":{\"height\":2,\"ice\":false,\"width\":4}'*) ;; *) exit 4;; esac\n"
        "echo changed > gen.sh\n"
        "cat data/word.txt\n");
    files["params.json"] = R"({"word": "ok"})";
    files["data/word.txt"] = "WORD";
    p.ice = false;
    const LayerId id = gen::add(p, files, "word", quick);
    const Layer& l = *p.layers.find(id);
    CHECK(l.cells.at(0, 0).ch == U'W');
    CHECK(l.cells.at(3, 0).ch == U'D');
    CHECK(p.files.at(gen::directory(id) + "gen.sh") == files.at("gen.sh"));
    CHECK(p.files.at(gen::directory(id) + "data/word.txt") == "WORD");

    // and it can run again from the project's bytes
    gen::refresh(p, id, quick);
    CHECK(p.layers.find(id)->cells.at(0, 0).ch == U'W');

    p.files[gen::directory(id) + "params.json"] = R"({"word": "no"})";
    CHECK_THROWS_WITH(gen::refresh(p, id, quick), ContainsSubstring("exited with status 3") &&
                                                     ContainsSubstring("bad input:"));
}

TEST_CASE("a generator's program is found on PATH, in its files or by an absolute path", "[generator][run]") {
    project::Project p = dotsProject();
    const std::string script = "#!/bin/sh\nprintf OK\n";
    for (const char* program : {"./gen.sh", "gen.sh/../gen.sh", "bin/gen.sh", "./bin/gen.sh"}) {
        INFO(program);
        project::Project q = p;
        const std::string path = std::string(program).find("bin/") != std::string::npos ? "bin/gen.sh" : "gen.sh";
        const Files files{{"generator.json", std::string(R"({"command": [")") + program + "\"]}"}, {path, script}};
        if (std::string(program) == "gen.sh/../gen.sh") {
            // a path with ".." in it isn't the generator's own file
            CHECK_THROWS_WITH(gen::add(q, files, "g", quick), ContainsSubstring("is not a file in scripts/"));
            CHECK(q == p);
            continue;
        }
        const LayerId id = gen::add(q, files, "g", quick);
        CHECK(q.layers.find(id)->cells.at(1, 0).ch == U'K');
    }
    {
        project::Project q = p;
        const LayerId id = gen::add(q, {{"generator.json", R"({"command": ["/bin/sh", "-c", "printf ABS"]})"}}, "g",
                                    quick);
        CHECK(q.layers.find(id)->cells.at(2, 0).ch == U'S');
    }
    for (const char* bad : {"../gen.sh", "./../gen.sh", "a/.hidden/x"}) {
        INFO(bad);
        project::Project q = p;
        CHECK_THROWS_WITH(
            gen::add(q, {{"generator.json", std::string(R"({"command": [")") + bad + "\"]}"}}, "g", quick),
            ContainsSubstring("is not a file in scripts/"));
        CHECK(q == p);
    }
    project::Project q = p;
    CHECK_THROWS_WITH(gen::add(q, {{"generator.json", R"({"command": ["blockwright-no-such-program"]})"}}, "g",
                               quick),
                      ContainsSubstring("could not run blockwright-no-such-program"));
    CHECK(q == p);
}

TEST_CASE("a generator that fails, hangs or floods leaves the project as it was", "[generator][run]") {
    project::Project p = dotsProject();
    const LayerId id = gen::add(p, shGenerator("printf ok\n"), "g", quick);
    const project::Project good = p;
    const std::string script = gen::directory(id) + "gen.sh";
    const auto refreshFails = [&](const std::string& body, const std::string& why,
                                  gen::RunOptions opts = quick) {
        INFO(body);
        project::Project q = good;
        q.files[script] = body;
        const project::Project before = q;
        CHECK_THROWS_WITH(gen::refresh(q, id, opts),
                          ContainsSubstring("layer " + std::to_string(id) + " (g): ") && ContainsSubstring(why));
        CHECK(q == before);
    };
    refreshFails("echo oops >&2; exit 2\n", "sh exited with status 2:\noops");
    refreshFails("exit 1\n", "sh exited with status 1");
    refreshFails("kill -9 $$\n", "sh crashed");
    // a program that names its own file by its full path, as Python's
    // traceback does, names it from the generator's directory, not from the
    // random temporary copy it ran in, written as given or with links
    // resolved; the directory itself is named as the generator's (#441)
    const auto namesNoTempDir = [&] {
        project::Project q = good;
        q.files[script] = "echo \"File $PWD/gen.sh, line 1\" >&2; echo \"in $(pwd -P)\" >&2;"
                          " echo \"File $(pwd -P)/sub/x.py\" >&2; echo \"left $(pwd -P).\" >&2; exit 4\n";
        try {
            gen::refresh(q, id, quick);
            FAIL("no throw");
        } catch (const gen::GeneratorError& e) {
            const std::string what = e.what();
            CHECK_THAT(what, EndsWith("sh exited with status 4:\nFile ./gen.sh, line 1\nin <generator dir>\n"
                                      "File ./sub/x.py\nleft <generator dir>."));
            CHECK(what.find(QDir::tempPath().toStdString()) == std::string::npos);
            CHECK(what.find(QFileInfo(QDir::tempPath()).canonicalFilePath().toStdString()) == std::string::npos);
        }
    };
    namesNoTempDir();
    {
        // under a TMPDIR reached through a link, as macOS's /var is, the
        // program sees the resolved path
        QTemporaryDir real;
        REQUIRE(real.isValid());
        const fs::path base(QFile::encodeName(real.path()).toStdString());
        fs::create_directory(base / "real");
        fs::create_directory_symlink(base / "real", base / "link");
        const QByteArray oldTmp = qgetenv("TMPDIR");
        const bool hadTmp = qEnvironmentVariableIsSet("TMPDIR");
        qputenv("TMPDIR", QFile::encodeName(QString::fromStdString((base / "link").string())));
        struct Restore {
            QByteArray old;
            bool had;
            ~Restore() { had ? qputenv("TMPDIR", old) : qunsetenv("TMPDIR"); }
        } restore{oldTmp, hadTmp};
        // Qt before 6.9.2 resolves TMPDIR's links in QDir::tempPath(), so
        // the program never sees the link there
        if (QLibraryInfo::version() < QVersionNumber(6, 9, 2))
            REQUIRE(QDir::tempPath() == QFileInfo(QDir::tempPath()).canonicalFilePath());
        else
            REQUIRE(QDir::tempPath() != QFileInfo(QDir::tempPath()).canonicalFilePath());
        INFO("TMPDIR through a link");
        namesNoTempDir();
    }
    // the end of a long stderr, from a line start
    refreshFails("i=0; while [ $i -lt 400 ]; do echo \"line $i of the noise\" >&2; i=$((i+1)); done; exit 5\n",
                 "status 5:\nline ");
    {
        project::Project q = good;
        q.files[script] = "i=0; while [ $i -lt 400 ]; do echo \"line $i\" >&2; i=$((i+1)); done; exit 5\n";
        try {
            gen::refresh(q, id, quick);
            FAIL("no throw");
        } catch (const gen::GeneratorError& e) {
            const std::string what = e.what();
            CHECK(what.ends_with("line 399"));
            CHECK(what.find("line 0\n") == std::string::npos);
            CHECK(what.size() < 2200);
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    refreshFails("exec sleep 30\n", "took longer than 300 ms; stopped", gen::RunOptions{std::chrono::milliseconds(300)});
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    refreshFails("exec head -c " + std::to_string(gen::kMaxOutputBytes + 1000) + " /dev/zero\n",
                 "wrote more than " + std::to_string(gen::kMaxOutputBytes) + " bytes");

    // and a layer that isn't a generator, or a spec it can't read
    project::Project q = good;
    const LayerId base = q.layers.layers().front().props.id;
    CHECK_THROWS_WITH(gen::refresh(q, base, quick), ContainsSubstring("not a generator"));
    q.files.erase(gen::directory(id) + "generator.json");
    CHECK_THROWS_WITH(gen::refresh(q, id, quick), ContainsSubstring("has no scripts/"));
    CHECK(q.layers == good.layers);
}

TEST_CASE("adding a generator refuses files it can't hold and a run that fails, adding nothing",
          "[generator][run]") {
    const project::Project p = dotsProject();
    const auto addFails = [&](const Files& files, const std::string& why) {
        project::Project q = p;
        CHECK_THROWS_WITH(gen::add(q, files, "g", quick), ContainsSubstring(why));
        CHECK(q == p);
    };
    addFails({{"gen.sh", "printf x"}}, "needs a generator.json");
    addFails({{"generator.json", R"({"command": 3})"}}, "generator.json");
    addFails({{"generator.json", R"({"command": ["sh", "gen.sh"]})"}, {"gen.sh", "exit 9"}}, "status 9");
    for (const char* path : {".hidden", "a/../b", "a\\b", "c:d", "/abs"}) {
        INFO(path);
        addFails({{"generator.json", R"({"command": ["sh", "-c", "printf x"]})"}, {path, "x"}}, "can't hold the file");
    }

    // nothing is taken: the next layer gets the id the failed one would have
    project::Project q = p;
    const LayerId next = q.layers.nextId();
    CHECK_THROWS(gen::add(q, {{"generator.json", R"({"command": ["sh", "-c", "exit 1"]})"}}, "g", quick));
    CHECK(gen::add(q, {{"generator.json", R"({"command": ["sh", "-c", "printf x"]})"}}, "g", quick) == next);

    // files already in the directory the new layer would use
    project::Project r = p;
    r.files["scripts/" + std::to_string(r.layers.nextId()) + "/old.txt"] = "x";
    const project::Project withOld = r;
    CHECK_THROWS_WITH(gen::add(r, {{"generator.json", R"({"command": ["sh", "-c", "printf x"]})"}}, "g", quick),
                      ContainsSubstring("already has files in scripts/"));
    CHECK(r == withOld);
}

TEST_CASE("a generator layer saves with its files and runs again after reopening", "[generator][project]") {
    project::Project p = dotsProject();
    Files files = shGenerator("cat sub/out.ans\n", R"(, "key": 0)");
    files["sub/out.ans"] = "\x1b[0;33m*\x1b[0m *";
    const LayerId id = gen::add(p, files, "stars", quick);
    const project::Project back = project::decode(project::encode(p));
    CHECK(back == p);
    const Layer& l = *back.layers.find(id);
    CHECK(l.props.kind == LayerKind::Generator);
    CHECK(l.cells.at(0, 0).ch == U'*');
    CHECK(l.cells.at(1, 0).transparent);
    // what a flat export writes includes the generator's cells
    const AnsiDocument flat = project::toAnsi(back);
    CHECK(flat.canvas.at(0, 0).ch == U'*');
    CHECK(flat.canvas.at(1, 0).ch == U'.');
    project::Project again = back;
    gen::refresh(again, id, quick);
    CHECK(again == back);
}

#endif

TEST_CASE("the CLI reads a generator's directory, skipping hidden entries and refusing links", "[generator][cli]") {
    const fs::path dir = scratch("read");
    writeFile(dir / "generator.json", R"({"command": ["sh", "gen.sh"]})");
    writeFile(dir / "gen.sh", "printf x");
    writeFile(dir / "lib" / "util.sh", "u");
    writeFile(dir / ".git" / "config", "secret");
    writeFile(dir / ".env", "secret");
    writeFile(dir / "lib" / ".cache", "c");
    const Files files = bwcli::readGeneratorDir(dir);
    CHECK(files == Files{{"generator.json", R"({"command": ["sh", "gen.sh"]})"},
                         {"gen.sh", "printf x"},
                         {"lib/util.sh", "u"}});

    CHECK_THROWS_AS(bwcli::readGeneratorDir(dir / "gen.sh"), bwcli::Failure);
    CHECK_THROWS_AS(bwcli::readGeneratorDir(dir / "missing"), bwcli::Failure);
    const fs::path link = scratch("link");
    if (!bwtest::symlinksWork())
        SKIP("the symbolic-link tests don't run here: no right to make links (fs_quirks.h)");
    fs::create_directory_symlink(dir, link / "to-dir");
    CHECK_THROWS_AS(bwcli::readGeneratorDir(link / "to-dir"), bwcli::Failure);
    fs::create_symlink(dir / "gen.sh", dir / "lib" / "link.sh");
    try {
        bwcli::readGeneratorDir(dir);
        FAIL("no throw");
    } catch (const bwcli::Failure& f) {
        CHECK_THAT(f.message, ContainsSubstring((fs::path("lib") / "link.sh").string() + ": not a plain file"));
    }
}

TEST_CASE("the CLI runs the generators asked for, bottom first, or every one", "[generator][cli]") {
    LayerStack s(4, 2);
    const LayerId c = s.add(makeLayer(LayerKind::Cells, 4, 2, "cells"));
    const LayerId a = s.add(makeLayer(LayerKind::Generator, 4, 2, "a"));
    const LayerId b = s.add(makeLayer(LayerKind::Generator, 4, 2, "b"));
    CHECK(bwcli::generatorsToRun(s, {}) == std::vector<LayerId>{a, b});
    CHECK(bwcli::generatorsToRun(s, {b, a, b}) == std::vector<LayerId>{a, b});
    CHECK(bwcli::generatorsToRun(s, {b}) == std::vector<LayerId>{b});
    CHECK_THROWS_AS(bwcli::generatorsToRun(s, {c}), bwcli::Failure);
    CHECK_THROWS_AS(bwcli::generatorsToRun(s, {99}), bwcli::Failure);
    CHECK(bwcli::generatorsToRun(LayerStack(4, 2), {}).empty());
}

TEST_CASE("the CLI runs a hidden generator layer only when it is named", "[generator][cli]") {
    LayerStack s(4, 2);
    const LayerId a = s.add(makeLayer(LayerKind::Generator, 4, 2, "a"));
    const LayerId h = s.add(makeLayer(LayerKind::Generator, 4, 2, "hidden"));
    const LayerId b = s.add(makeLayer(LayerKind::Generator, 4, 2, "b"));
    const LayerId h2 = s.add(makeLayer(LayerKind::Generator, 4, 2, "hidden too"));
    s.find(h)->props.visible = false;
    s.find(h2)->props.visible = false;
    std::vector<LayerId> hidden;
    CHECK(bwcli::generatorsToRun(s, {}, &hidden) == std::vector<LayerId>{a, b});
    CHECK(hidden == std::vector<LayerId>{h, h2});
    CHECK(bwcli::generatorsToRun(s, {}) == std::vector<LayerId>{a, b});
    hidden.clear();
    CHECK(bwcli::generatorsToRun(s, {h2, h}, &hidden) == std::vector<LayerId>{h, h2});
    CHECK(bwcli::generatorsToRun(s, {b, h}, &hidden) == std::vector<LayerId>{h, b});
    CHECK(hidden.empty());
    s.find(a)->props.visible = false;
    s.find(b)->props.visible = false;
    CHECK(bwcli::generatorsToRun(s, {}, &hidden).empty());
    CHECK(hidden == std::vector<LayerId>{a, h, b, h2});
}

#ifndef _WIN32
TEST_CASE("a run leaves nothing it started running whichever way it ends", "[generator][run][process]") {
    // Each script starts a background child that writes its marker a second
    // later; by then the run is over, whichever way it ended, and the child
    // must be gone with it.
    const fs::path dir = scratch("descendants");
    const auto late = [&](const std::string& name) {
        return "(sleep 1; printf leaked > '" + (dir / name).string() + "') </dev/null >/dev/null 2>&1 &\n";
    };
    const gen::RunOptions brief{std::chrono::milliseconds(300)};
    project::Project p = dotsProject();
    CHECK_THROWS_WITH(gen::add(p, shGenerator(late("timeout") + "wait\n"), "g", brief),
                      ContainsSubstring("took longer than 300 ms; stopped"));
    CHECK_THROWS_WITH(gen::add(p, shGenerator(late("status") + "exit 3\n"), "g", quick),
                      ContainsSubstring("exited with status 3"));
    CHECK_THROWS_WITH(gen::add(p, shGenerator(late("crash") + "kill -9 $$\n"), "g", quick),
                      ContainsSubstring("crashed"));
    CHECK_THROWS_WITH(gen::add(p, shGenerator(late("flood") + "exec head -c " +
                                                  std::to_string(gen::kMaxOutputBytes + 1000) + " /dev/zero\n"),
                               "g", quick),
                      ContainsSubstring("wrote more than"));
    // a program that succeeds but leaves a child behind, this one still
    // holding stdout: the run takes what was written by the time the program
    // exited
    const std::string holder = "(sleep 1; printf leaked > '" + (dir / "success").string() + "') &\nprintf ok\n";
    const auto t0 = std::chrono::steady_clock::now();
    const LayerId id = gen::add(p, shGenerator(holder), "g", quick);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(900));
    CHECK(p.layers.find(id)->cells.at(0, 0).ch == U'o');

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    for (const char* name : {"timeout", "status", "crash", "flood", "success"}) {
        INFO(name);
        CHECK_FALSE(fs::exists(dir / name));
    }
}

TEST_CASE("a generator flooding stdout and stderr is stopped promptly and in bounded memory",
          "[generator][run][process]") {
    // Four children on each pipe, or eight on stderr, writing as fast as
    // they can: the cap and the deadline must hold while the data keeps
    // coming, and what the run holds stays near the cap. Each case gets a
    // deadline only its own limit can beat: the stdout cap has a minute, far
    // more than reading 32 MiB takes on a loaded machine, so the cap is what
    // stops it (the message says which limit did), and it must do so within
    // a few seconds; the stderr flood never reaches the cap, so the 300 ms
    // deadline stops it. Each stdout writer ends after 64 MiB, eight times
    // the cap between the four, so a cap that fails to hold fails here on
    // the message and the memory check instead of reading without end.
    using std::chrono::milliseconds;
    struct Case {
        std::string script, why;
        milliseconds timeout, within;
    };
    const Case cases[] = {
        {"for i in 1 2 3 4; do head -c " + std::to_string(2 * gen::kMaxOutputBytes) +
             " /dev/zero & cat /dev/zero >&2 & done; wait\n",
         "wrote more than", milliseconds(60'000), milliseconds(5'000)},
        {"for i in 1 2 3 4 5 6 7 8; do cat /dev/zero >&2 & done; wait\n", "took longer than 300 ms; stopped",
         milliseconds(300), milliseconds(1500)},
    };
    const long long rss0 = peakRss();
    for (const Case& c : cases) {
        INFO(c.why);
        project::Project p = dotsProject();
        const project::Project before = p;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_THROWS_WITH(gen::add(p, shGenerator(c.script), "g", gen::RunOptions{c.timeout}),
                          ContainsSubstring(c.why));
        CHECK(std::chrono::steady_clock::now() - t0 < c.within);
        CHECK(p == before);
    }
    // the stdout cap (32 MiB) and the stderr tail are what a run holds
    CHECK(peakRss() - rss0 < (128LL << 20));
}

namespace {

std::atomic<bool> termHandled{false};

// What the CLI's handler does before it ends the program.
void killOnTerm(int) {
    gen::killRunning();
    termHandled = true;
}

// Clears the hooks, lets runs start again after killRunning and puts
// SIGTERM's action back, however the test ends.
struct HooksAndSigterm {
    struct sigaction old {};
    HooksAndSigterm() {
        struct sigaction sa {};
        sa.sa_handler = killOnTerm;
        sigemptyset(&sa.sa_mask);
        ::sigaction(SIGTERM, &sa, &old);
        termHandled = false;
    }
    ~HooksAndSigterm() {
        gen::testing::started = nullptr;
        gen::testing::killing = nullptr;
        gen::testing::reaped = nullptr;
        gen::testing::allowRuns();
        ::sigaction(SIGTERM, &old, nullptr);
    }
};

} // namespace

TEST_CASE("a signal arriving while a generator starts kills its group once the group is known",
          "[generator][run][process]") {
    // The signal comes after the program is running in its own group but
    // before the run has published the group: a handler that runs then
    // (killRunning) must still kill it, so the program ends at once on
    // SIGKILL rather than running to the timeout.
    HooksAndSigterm guard;
    const gen::RunOptions slow{std::chrono::milliseconds(3000)};
    const Files sleeper{{"generator.json", R"({"command": ["sleep", "10"]})"}};
    project::Project p = dotsProject();

    // on the running thread
    gen::testing::started = [](pid_t) { ::raise(SIGTERM); };
    auto t0 = std::chrono::steady_clock::now();
    CHECK_THROWS_WITH(gen::add(p, sleeper, "g", slow), ContainsSubstring("sleep crashed"));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));
    CHECK(termHandled);

    // on another thread, while the running one is still starting
    gen::testing::allowRuns();
    std::thread other;
    gen::testing::started = [&](pid_t) {
        other = std::thread([] { gen::killRunning(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    t0 = std::chrono::steady_clock::now();
    CHECK_THROWS_WITH(gen::add(p, sleeper, "g", slow), ContainsSubstring("sleep crashed"));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));
    other.join();
}

TEST_CASE("a run signals its group only before the program is reaped", "[generator][run][process]") {
    // Once the program that leads the group is reaped, its id may be
    // another process's group: nothing may signal it again, neither the run
    // nor killRunning (a signal handled just after the reap, or later).
    HooksAndSigterm guard;
    std::vector<std::pair<char, pid_t>> log;  // 'k' a group signalled, 'r' the program reaped
    gen::testing::killing = [&](pid_t g) { log.emplace_back('k', g); };
    gen::testing::reaped = [&](pid_t g) {
        log.emplace_back('r', g);
        gen::killRunning();
    };
    const gen::RunOptions brief{std::chrono::milliseconds(300)};
    const std::string flood = "exec head -c " + std::to_string(gen::kMaxOutputBytes + 1000) + " /dev/zero\n";
    for (const auto& [script, options, ends] :
         {std::tuple{std::string("printf ok\n"), quick, std::string("success")},
          std::tuple{std::string("exit 3\n"), quick, std::string("status")},
          std::tuple{std::string("exec sleep 5\n"), brief, std::string("timeout")},
          std::tuple{flood, quick, std::string("cap")}}) {
        INFO(ends);
        log.clear();
        gen::testing::allowRuns();
        project::Project p = dotsProject();
        try {
            gen::add(p, shGenerator(script), "g", options);
        } catch (const gen::GeneratorError&) {
        }
        gen::killRunning();
        const auto reap = std::find_if(log.begin(), log.end(), [](const auto& e) { return e.first == 'r'; });
        REQUIRE(reap != log.end());
        const pid_t group = reap->second;
        CHECK(group > 0);
        // the group was killed, the last time before the reap, and nothing after
        CHECK(std::find(log.begin(), reap, std::pair{'k', group}) != reap);
        CHECK(std::distance(reap, log.end()) == 1);
    }
}

TEST_CASE("no run starts once killRunning has begun though it found no group",
          "[generator][run][process]") {
    // A handler on another thread reads that nothing is running, and before
    // it ends the program a run starts. In a child process: killRunning
    // finds no group, then a generator is added, and the child exits the
    // moment the program has started (as the CLI's handler goes on to end
    // it). The program must not have been started.
    HooksAndSigterm guard;
    int report[2];
    REQUIRE(::pipe(report) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::close(report[0]);
        gen::killRunning();
        gen::testing::started = [&](pid_t pid) {
            (void)!::write(report[1], &pid, sizeof pid);
            ::_exit(0);
        };
        project::Project p = dotsProject();
        try {
            gen::add(p, {{"generator.json", R"({"command": ["sleep", "10"]})"}}, "g", quick);
        } catch (const gen::GeneratorError&) {
        }
        ::_exit(0);
    }
    ::close(report[1]);
    pid_t started = 0;
    const ssize_t n = ::read(report[0], &started, sizeof started);
    ::close(report[0]);
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    if (n == ssize_t(sizeof started) && started > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const bool alive = ::kill(started, 0) == 0;
        if (alive)
            ::kill(started, SIGKILL);
        CHECK_FALSE(alive);
    }
    CHECK(n == 0);

    // and in this process: no run starts after it, as run-generators would
    // start the next layer's
    gen::killRunning();
    project::Project p = dotsProject();
    const project::Project before = p;
    bool ran = false;
    gen::testing::started = [&](pid_t) { ran = true; };
    CHECK_THROWS_WITH(gen::add(p, shGenerator("printf ok\n"), "g", quick),
                      ContainsSubstring("not run: generators are being stopped"));
    CHECK_FALSE(ran);
    CHECK(p == before);
    gen::testing::started = nullptr;
    gen::testing::allowRuns();
    CHECK_NOTHROW(gen::add(p, shGenerator("printf ok\n"), "g", quick));
}

TEST_CASE("failed generator startup does not reap another child", "[generator][run][process]") {
    // The sibling outlives any wait the failed start could do on it, so a
    // run that waited for it would take its whole life; the bound leaves a
    // slow machine room and is still far short of that.
    const pid_t sibling = ::fork();
    if (sibling == 0) {
        ::sleep(60);
        ::_exit(0);
    }
    REQUIRE(sibling > 0);

    auto p = dotsProject();
    const auto before = std::chrono::steady_clock::now();
    CHECK_THROWS_WITH(gen::add(p, {{"generator.json", R"({"command":["/blockwright-no-such-program"]})"}}, "g",
                               quick),
                      ContainsSubstring("could not run"));
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(10'000));

    // still ours to reap, and still running: nothing took it
    int status;
    const pid_t ours = ::waitpid(sibling, &status, WNOHANG);
    CHECK(ours == 0);
    if (ours == 0) {
        ::kill(sibling, SIGKILL);
        CHECK(::waitpid(sibling, &status, 0) == sibling);
    }
}

TEST_CASE("a run refuses to start while SIGCHLD is ignored", "[generator][run][process]") {
    // An ignored SIGCHLD (SIG_IGN, or SA_NOCLDWAIT) has the kernel reap the
    // program as it exits, and with it goes the run's hold on its group's
    // id: the run would have to let what the program left running go on.
    // It refuses before starting anything instead.
    struct sigaction old {};
    REQUIRE(::sigaction(SIGCHLD, nullptr, &old) == 0);
    struct Restore {
        struct sigaction& old;
        ~Restore() { ::sigaction(SIGCHLD, &old, nullptr); }
    } restore{old};
    const fs::path dir = scratch("sigchld");
    const std::string script =
        "(sleep 1; printf leaked > '" + (dir / "leaked").string() + "') </dev/null >/dev/null 2>&1 &\nprintf ok\n";
    for (const bool noCldWait : {false, true}) {
        INFO((noCldWait ? "SA_NOCLDWAIT" : "SIG_IGN"));
        struct sigaction sa {};
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = noCldWait ? SIG_DFL : SIG_IGN;
        sa.sa_flags = noCldWait ? SA_NOCLDWAIT : 0;
        REQUIRE(::sigaction(SIGCHLD, &sa, nullptr) == 0);
        bool ran = false;
        gen::testing::started = [&](pid_t) { ran = true; };
        project::Project p = dotsProject();
        const project::Project before = p;
        CHECK_THROWS_WITH(gen::add(p, shGenerator(script), "g", quick), ContainsSubstring("SIGCHLD is ignored"));
        gen::testing::started = nullptr;
        CHECK_FALSE(ran);
        CHECK(p == before);
    }
    ::sigaction(SIGCHLD, &old, nullptr);
    project::Project p = dotsProject();
    CHECK_NOTHROW(gen::add(p, shGenerator("printf ok\n"), "g", quick));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK_FALSE(fs::exists(dir / "leaked"));
}

TEST_CASE("the CLI refuses a generator directory it can't read all of", "[generator][cli]") {
    if (geteuid() == 0)
        SKIP("root reads a directory without permissions");
    const fs::path dir = scratch("unreadable");
    writeFile(dir / "generator.json", R"({"command": ["sh", "gen.sh"]})");
    writeFile(dir / "assets" / "input.txt", "required asset");
    fs::permissions(dir / "assets", fs::perms::none);
    CHECK_THROWS_AS(bwcli::readGeneratorDir(dir), bwcli::Failure);
    fs::permissions(dir / "assets", fs::perms::owner_all);
    CHECK(bwcli::readGeneratorDir(dir).size() == 2);
}
#endif

// A generator writes CP437 (PLAN §7): its output is read as CP437 even when
// its bytes would pass for UTF-8, so changing cellsFromOutput to detect the
// encoding fails here (#302).
TEST_CASE("a generator's output is read as CP437, never detected as UTF-8", "[generator][utf8]") {
    // α░▒ in CP437 is E0 B0 B1, which is also UTF-8 for one glyph (U+0C31)
    const std::string stamp = "\xE0\xB0\xB1\r\n";
    REQUIRE(detectEncoding(stamp) == TextEncoding::Utf8);
    const Canvas c = gen::cellsFromOutput(stamp, 6, std::nullopt);
    CHECK(c.at(0, 0).ch == U'α');
    CHECK(c.at(1, 0).ch == U'░');
    CHECK(c.at(2, 0).ch == U'▒');
    // and UTF-8 bytes for █ are three CP437 glyphs, not one block
    const Canvas block = gen::cellsFromOutput("\xE2\x96\x88\r\n", 6, std::nullopt);
    CHECK(block.at(0, 0).ch == U'Γ');
    CHECK(block.at(1, 0).ch == U'û');
    CHECK(block.at(2, 0).ch == U'ê');
}
