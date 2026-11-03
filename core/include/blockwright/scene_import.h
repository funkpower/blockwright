#pragma once
// Opening a Python scene as a project (PLAN §7): a scene.py drawn with
// ansidraw's run() becomes one generator
// layer per scene layer, bottom first, each named after its scene layer.
//
// The scene answers for itself (ansidraw's layer modes):
//
//   <python> -B scene.py --list-layers
//       {"width": 80, "rows": 40, "title": "…", "ice": false,
//        "layers": [{"name": "sky"}, {"name": "figure", "key": 13}, …]}
//       ("ice" optional: true, iCE colours, when it is left out)
//   <python> -B scene.py --layer-ans=NAME [--key=K]
//       the layer's .ans: the layers below it drawn first, the cells it
//       changed kept, every other cell a blank on palette colour K
//
// Each layer's generator.json runs the second with the scene's absolute
// path and the key the listing gave, and holds that key, so the cells the
// layer left alone are transparent. The bottom layer has no key: it is
// drawn whole, opaque. Stacked, the layers show what the scene's own .ans
// shows.
//
// The project names the scene where it is rather than copying it: the
// scene stays the one source, its edits reach every layer on the next run,
// and what it reads beside itself (locked layers, reference pictures) is
// found, because ansidraw finds those from the script's own path. The cost
// is that running the layers again needs the scene at that path, with
// the Python it was opened with and the ansidraw it imports; a project
// moved elsewhere still opens, composites and exports its cached cells.
//
// The Python is python3 on Linux and macOS. Windows has no one name for
// it: python.org's installer puts py.exe (the launcher) and python.exe on
// PATH but no python3.exe, and stock Windows has App Execution Aliases,
// python.exe and python3.exe in WindowsApps, that stand in for a Store
// Python and, with none installed, exit 9009. So on Windows the first of
// py -3, python and python3 that answers --version with "Python 3." is
// used, and the layers' generator.json run it the same way.
//
// A key is chosen when the scene is opened. If the scene changes so that a
// layer now has a blank cell on its key, that layer's run fails and says
// where; opening the scene again picks new keys.

#include "blockwright/generator.h"
#include "blockwright/project.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw::scene {

// A listing that isn't one, a scene that can't be run or whose layer fails.
class SceneError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A command that runs Python 3: the program and the arguments before the
// script's, e.g. {"python3"} or {"py", "-3"}.
using PythonCommand = std::vector<std::string>;

// The commands to try, in order: on Windows py -3, python, python3; on
// Linux and macOS python3 alone.
std::vector<PythonCommand> pythonCandidates(bool windows);

// Runs `python --version` (generator::probeCommand, so the program is
// looked up on PATH as a layer's run looks it up, and a generator running
// on another thread stays killable) and returns why it won't do: it
// couldn't start, exited with a status other than 0 (the WindowsApps
// alias's 9009), took longer than `options` allow, or printed no
// "Python 3." on stdout (Python 2 and 3.0-3.3 print their version on
// stderr). Nothing when it is a Python 3.
std::optional<std::string> probePython(const PythonCommand& python,
                                       const generator::RunOptions& options = {std::chrono::seconds(5)});

// Why a command isn't a Python 3, or nothing when it is (probePython).
using PythonProbe = std::function<std::optional<std::string>(const PythonCommand&)>;

// The first candidate `probe` accepts. Throws SceneError naming each one
// tried and why it was passed over, and what to install.
PythonCommand choosePython(const std::vector<PythonCommand>& candidates, const PythonProbe& probe);

// The Python scenes run with here: on Windows choosePython over
// pythonCandidates(true) with probePython, the one found kept for the
// rest of the process (a failure is not kept: the next call looks again);
// on Linux and macOS python3, unprobed (a missing one fails the run that
// needs it, as before).
PythonCommand findPython();

namespace testing {
// Tests only: forgets the Python findPython found, so it looks again.
void forgetPython();
} // namespace testing

// The command as words joined by spaces, for messages: "py -3".
std::string pythonText(const PythonCommand& python);

// Both commands run Python with -B: the scene's imports (a helper beside
// it, ansidraw itself) write no __pycache__ there. ansidraw's layer modes
// keep the scene's own writes off its files, but Python's import machinery
// writes bytecode without going through what they guard.
inline constexpr std::string_view kNoBytecode = "-B";

struct SceneLayer {
    std::string name;
    std::optional<std::uint8_t> key;  // none for the bottom layer

    bool operator==(const SceneLayer&) const = default;
};

struct Listing {
    int width = 0, rows = 0;
    std::string title;
    bool ice = true;  // the scene's iCE colours ("ice"; true when left out)
    std::vector<SceneLayer> layers;  // bottom first

    bool operator==(const Listing&) const = default;
};

// --list-layers' answer read strictly: a JSON object with "width" (1 to
// kMaxColumns) and "rows" (1 to kMaxRows) whole, "title" a string, "ice"
// true, false or left out (true), and "layers" a non-empty list of objects, each with a non-empty "name" no
// other has; the first without a "key", every other with a whole "key"
// 0-15. Throws SceneError.
Listing parseListing(std::string_view json);

// The command a layer's generator runs: `python`, -B, the scene,
// --layer-ans= the name and, with a key, --key=K.
std::vector<std::string> layerCommand(const PythonCommand& python, const std::string& scene,
                                      const SceneLayer& layer);

// Asks the scene (an absolute path) for its layers, running it with
// `python`. Throws SceneError.
Listing list(const PythonCommand& python, const std::string& scene, const generator::RunOptions& options = {});

// Called before each layer runs: its index (bottom 0), how many there are,
// its name.
using Progress = std::function<void(std::size_t index, std::size_t count, const std::string& name)>;

// The scene at `path` as a new project: the listing's size and iCE, its
// title in SAUCE when CP437 can hold it, and a generator layer for each
// scene layer (generator::add), run once for its cells. The scene runs
// with `python`, or findPython()'s when it is empty. Throws SceneError for
// a path that isn't a file, no Python found, and what list and the runs
// refuse.
project::Project importScene(const std::string& path, const generator::RunOptions& options = {},
                             const Progress& progress = {}, const PythonCommand& python = {});

// `text` (UTF-8) as the bytes of a SAUCE text field at most `length` long,
// or nothing when it is empty, too long, or holds a character CP437 has no
// printable byte for (a control code or DEL included).
std::optional<std::string> sauceText(const std::string& text, std::size_t length);

// For a project that rebuilds byte for byte (#308): what import-scene's
// --bake, --author and --date and SOURCE_DATE_EPOCH change in the project
// a scene imports as.
//
// Every generator layer as a cells layer of the cells it last made
// (bw::bake), its scripts/<id>/ files dropped. The project then no longer
// names the scene or the Python, so the same scene imported from another
// path is the same project; its layers can't be run again.
void bake(project::Project& p);

struct Stamp {
    std::optional<std::string> author;  // SAUCE bytes (sauceText), at most Sauce::kAuthorLen
    std::optional<std::string> date;    // CCYYMMDD (isSauceDate)
    // A SAUCE record is made for them when the scene's title gave none;
    // without this they only change a record the project already has.
    bool record = false;
};

// Sets the author and the date of the project's SAUCE record, as `stamp`
// gives them. A record it makes is Sauce::fresh's for the canvas, with the
// project's iCE.
void stamp(project::Project& p, const Stamp& stamp);

} // namespace bw::scene
