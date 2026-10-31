#include "blockwright/scene_import.h"

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/sauce.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <cmath>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>

namespace fs = std::filesystem;

namespace bw::scene {
namespace {

[[noreturn]] void fail(const std::string& what) { throw SceneError(what); }

// A whole number from lo to hi, or nothing.
std::optional<int> whole(const QJsonValue& v, int lo, int hi) {
    if (!v.isDouble())
        return {};
    const double d = v.toDouble();
    if (!std::isfinite(d) || d != std::floor(d) || d < lo || d > hi)
        return {};
    return int(d);
}

// The title as SAUCE bytes, or nothing when CP437 can't hold it or it is
// too long for the field.
std::optional<std::string> sauceTitle(const std::string& title) {
    std::string out;
    for (const char32_t cp : QString::fromStdString(title).toUcs4()) {
        const std::optional<std::uint8_t> b = unicodeToCp437(cp);
        if (!b || *b < 0x20 || *b == 0x7f)
            return {};
        out += char(*b);
    }
    if (out.empty() || out.size() > Sauce::kTitleLen)
        return {};
    return out;
}

} // namespace

Listing parseListing(std::string_view json) {
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray(json.data(), qsizetype(json.size())), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        fail("the scene's layer list is not a JSON object");
    const QJsonObject o = doc.object();
    Listing l;
    const std::optional<int> width = whole(o.value(QStringLiteral("width")), 1, kMaxColumns);
    const std::optional<int> rows = whole(o.value(QStringLiteral("rows")), 1, kMaxRows);
    if (!width || !rows)
        fail("the scene's layer list has no width 1-" + std::to_string(kMaxColumns) + " and rows 1-" +
             std::to_string(kMaxRows));
    l.width = *width;
    l.rows = *rows;
    const QJsonValue title = o.value(QStringLiteral("title"));
    if (!title.isString())
        fail("the scene's layer list has no title");
    l.title = title.toString().toStdString();
    const QJsonValue layers = o.value(QStringLiteral("layers"));
    if (!layers.isArray() || layers.toArray().isEmpty())
        fail("the scene's layer list names no layers");
    std::set<std::string> names;
    for (const QJsonValue& v : layers.toArray()) {
        const std::size_t i = l.layers.size();
        const QJsonObject lo = v.toObject();
        const QJsonValue name = lo.value(QStringLiteral("name"));
        if (!v.isObject() || !name.isString() || name.toString().isEmpty())
            fail("layer " + std::to_string(i) + " in the scene's layer list has no name");
        SceneLayer layer{name.toString().toStdString(), {}};
        if (!names.insert(layer.name).second)
            fail("the scene names layer " + layer.name + " twice");
        const QJsonValue key = lo.value(QStringLiteral("key"));
        if (i == 0 && !key.isUndefined())
            fail("the scene's bottom layer " + layer.name + " has a key; it is drawn whole");
        if (i > 0) {
            const std::optional<int> k = whole(key, 0, 15);
            if (!k)
                fail("the scene's layer " + layer.name + " has no key colour 0-15");
            layer.key = std::uint8_t(*k);
        }
        l.layers.push_back(std::move(layer));
    }
    return l;
}

std::vector<PythonCommand> pythonCandidates(bool windows) {
    if (windows)
        return {{"py", "-3"}, {"python"}, {"python3"}};
    return {{"python3"}};
}

std::string pythonText(const PythonCommand& python) {
    std::string out;
    for (const std::string& word : python)
        out += (out.empty() ? "" : " ") + word;
    return out;
}

std::optional<std::string> probePython(const PythonCommand& python, const generator::RunOptions& options) {
    PythonCommand command = python;
    command.push_back("--version");
    std::string out;
    try {
        out = generator::probeCommand(command, pythonText(command), options);
    } catch (const generator::GeneratorError& e) {
        return std::string(e.what());
    }
    if (!out.starts_with("Python 3.")) {
        const std::string line = out.substr(0, out.find_first_of("\r\n"));
        return pythonText(command) + ": " +
               (line.empty() ? std::string("printed nothing on stdout")
                             : "printed \"" + line.substr(0, 80) + "\" on stdout") +
               ", where a Python 3.4 or later prints \"Python 3.\" and its version";
    }
    return {};
}

PythonCommand choosePython(const std::vector<PythonCommand>& candidates, const PythonProbe& probe) {
    std::string tried;
    for (const PythonCommand& python : candidates) {
        const std::optional<std::string> why = probe(python);
        if (!why)
            return python;
        tried += "\n- " + *why;
    }
    fail("no Python 3 to run the scene with; tried:" + tried +
         "\nInstall Python 3 from python.org, which puts py and python on PATH.");
}

namespace {

std::mutex foundLock;
std::optional<PythonCommand> found;  // findPython's, once it has found one

} // namespace

PythonCommand findPython() {
#ifdef _WIN32
    const std::lock_guard lock(foundLock);
    if (!found)
        found = choosePython(pythonCandidates(true), [](const PythonCommand& python) { return probePython(python); });
    return *found;
#else
    return pythonCandidates(false).front();
#endif
}

void testing::forgetPython() {
    const std::lock_guard lock(foundLock);
    found.reset();
}

std::vector<std::string> layerCommand(const PythonCommand& python, const std::string& scene,
                                      const SceneLayer& layer) {
    std::vector<std::string> command = python;
    command.insert(command.end(), {std::string(kNoBytecode), scene, "--layer-ans=" + layer.name});
    if (layer.key)
        command.push_back("--key=" + std::to_string(*layer.key));
    return command;
}

Listing list(const PythonCommand& python, const std::string& scene, const generator::RunOptions& options) {
    std::vector<std::string> command = python;
    command.insert(command.end(), {std::string(kNoBytecode), scene, "--list-layers"});
    std::string out;
    try {
        out = generator::runCommand(command, {}, "scene " + scene + " (listing its layers)", options);
    } catch (const generator::GeneratorError& e) {
        fail(e.what());
    }
    return parseListing(out);
}

project::Project importScene(const std::string& path, const generator::RunOptions& options,
                             const Progress& progress, const PythonCommand& given) {
    std::error_code ec;
    const fs::path scene = fs::weakly_canonical(fs::absolute(path, ec), ec);
    if (ec || !fs::is_regular_file(scene, ec))
        fail(path + " is not a file");
    const PythonCommand python = given.empty() ? findPython() : given;
    const Listing listing = list(python, scene.string(), options);
    project::Project p;
    p.layers = LayerStack(listing.width, listing.rows);
    p.ice = true;
    if (const std::optional<std::string> title = sauceTitle(listing.title)) {
        p.sauce = Sauce::fresh(listing.width, listing.rows);
        p.sauce->title = *title;
    }
    for (std::size_t i = 0; i < listing.layers.size(); ++i) {
        const SceneLayer& layer = listing.layers[i];
        if (progress)
            progress(i, listing.layers.size(), layer.name);
        generator::Spec spec;
        spec.command = layerCommand(python, scene.string(), layer);
        spec.key = layer.key;
        try {
            generator::add(p, {{std::string(generator::kSpecFile), generator::specJson(spec)}}, layer.name, options);
        } catch (const generator::GeneratorError& e) {
            fail(e.what());
        }
    }
    return p;
}

} // namespace bw::scene
