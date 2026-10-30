#pragma once
// Generator layers (PLAN §7): a layer whose cells are a program's output.
//
// A generator is any program that reads JSON on stdin and writes an .ans on
// stdout. Its files live in the project under scripts/<id>/ (the layer's
// id), kept byte for byte with the project (Project::files):
//
//   scripts/<id>/generator.json  {"command": ["python3", "scene.py", ...],
//                                 "key": 0, "params": [...]}
//                                ("key" and "params" optional; Param)
//   scripts/<id>/params.json     the parameters, a JSON object (optional)
//   scripts/<id>/…               anything else it needs: its source, data
//
// A run copies scripts/<id>/ into a new temporary directory (each file
// readable, writable and executable by the user only) and starts the
// command there: its first word is looked up on PATH, unless it holds a '/',
// when it is taken as it is if absolute and from the copy if not, so
// "./gen.sh" runs the project's own script. stdin is one JSON object, read
// from a file:
//
//   {"canvas": {"width", "height", "ice"},
//    "layer":  {"id", "name", "x", "y", "width", "height"},
//    "params": the contents of params.json, or {}, with each parameter
//              generator.json declares (Param) and params.json lacks set
//              to its default}
//
// stdout is read as an .ans (parseAnsi, at the layer's width, or the
// canvas's for a layer with none, unless its SAUCE gives one) and opened as
// the editor opens a file (openAsEditor); those cells, at the size the file
// has, become the layer's: its cached output, saved in layers/<id>.cells
// like any layer's cells, so a reader without the program still composites
// and exports the layer. Black-on-black space is opaque. With a key colour,
// a cell that shows only its background (a space, NUL or 0xFF without blink)
// in that palette colour (Spec::key) is transparent instead, so a generator
// drawing an element on a plain ground makes a layer that overlays the ones
// below.
//
// The program runs with the user's environment and rights: a project's
// generators run only when asked for (cli run-generators, add-generator;
// in the app, a changed parameter or Run once the project is trusted).
// It leads a process group of its own, and the run ends with the group
// killed whichever way it ends (a success too): a background process it
// leaves behind is stopped, and what that process would have written is
// not read. A process that leaves the group on purpose (setsid, setpgid,
// a daemon) is not followed. The group is signalled only while the program
// is unreaped, when its id can be no other group's: the run kills it a last
// time, retires it, and then reaps the program, and nothing signals that
// number again.
//
// That needs the program to stay unreaped until the run reaps it, so run
// requires of the process that:
// - SIGCHLD is not ignored (neither SIG_IGN nor SA_NOCLDWAIT), when the
//   kernel would reap the program as it exits. run checks this before it
//   starts anything and refuses if it is. An ignored SIGCHLD survives exec,
//   so a parent can hand one down: the CLI puts SIGCHLD back to its default
//   before it runs a generator (bwcli::prepareToRunGenerators).
// - nothing else waits for a child it didn't start (waitpid(-1) or
//   waitpid(0), wait()). This can't be checked; neither the CLI nor the app
//   does it (Qt's QProcess waits only for its own children).

#include "blockwright/canvas.h"
#include "blockwright/layer.h"
#include "blockwright/project.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/types.h>

namespace bw::generator {

inline constexpr std::string_view kSpecFile = "generator.json";
inline constexpr std::string_view kParamsFile = "params.json";
// How long a run may take, and how much it may write to stdout, before it
// is stopped and refused.
inline constexpr std::chrono::milliseconds kDefaultTimeout{30'000};
inline constexpr std::size_t kMaxOutputBytes = std::size_t{32} << 20;

// A layer that isn't a generator, a spec or params it can't read, a program
// that can't start, fails, runs too long or writes something that isn't an
// .ans. The project is left as it was.
class GeneratorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The kinds of parameter a generator can declare (Param), by their names
// in generator.json: "integer", "number", "boolean", "colour" (a palette
// colour 0-15, numbered as Spec::key is), "ramp" (a named ramp, ramp.h)
// and "text".
enum class ParamType { Integer, Number, Boolean, Colour, Ramp, Text };
std::string_view paramTypeName(ParamType type);

// A parameter's value: a boolean's, a number's (an integer's and a
// colour's are whole), or a ramp's name or a text's.
using ParamValue = std::variant<bool, double, std::string>;

// A parameter the generator declares in generator.json's "params" list,
// for the editor to show as a form; its value is params.json's key `name`.
//
//   {"name": "density", "type": "number", "label": "Density",
//    "min": 0, "max": 1, "step": 0.05, "default": 0.25}
//
// "name" and "type" are needed, the rest optional. An integer's values,
// bounds and default are whole numbers from -2147483648 to 2147483647, and
// a number's values, bounds, step and default are from -1e12 to 1e12 with
// at most six decimal places (what the form's fields hold). "min" and "max"
// bound an integer or a number, "step" is a number's step in the form
// (above 0). "default" is the value
// a params.json without the key gives; without one it is the minimum or 0,
// false, 7 (light grey), the first named ramp or "".
struct Param {
    std::string name;
    ParamType type = ParamType::Number;
    std::string label;  // empty: the name
    std::optional<double> min, max, step;
    ParamValue defaultValue;

    bool operator==(const Param&) const = default;
};

// Why `value` can't be the parameter's (of another kind, out of its range
// or an integer's, not whole for an integer or a colour, not a named ramp),
// or empty.
std::string paramProblem(const Param& param, const ParamValue& value);

// What generator.json says.
struct Spec {
    // The program and its arguments; at least the program, none empty or
    // holding a NUL.
    std::vector<std::string> command;
    // The palette colour (0-15, numbered as cells and ramps number them: 1
    // blue, 4 red, the order of the PC's palette rather than SGR's) whose
    // blank cells are transparent; none: every cell is opaque.
    std::optional<std::uint8_t> key;
    // The parameters it declares, in the form's order; none: params.json is
    // passed on as it is.
    std::vector<Param> params;

    bool operator==(const Spec&) const = default;
};

// "scripts/<id>/": where the layer's generator files are in the project.
std::string directory(LayerId id);

// generator.json read strictly: a JSON object, no key twice, "command" a
// list of strings as Spec says, "key" absent, null or a whole number 0-15,
// "params" absent or a list of objects as Param says, no name twice, min
// no more than max, a default the parameter can take. Keys it doesn't know
// are skipped, as a newer minor version's, in a param too. Throws
// GeneratorError.
Spec parseSpec(std::string_view json);
// generator.json as parseSpec reads it back.
std::string specJson(const Spec& spec);

// The generator layers of the stack, bottom first.
std::vector<LayerId> generatorLayers(const LayerStack& stack);

// The layer's spec, from the project's files. Throws GeneratorError for a
// layer the stack doesn't hold, one that isn't a generator, or one without
// a readable generator.json.
Spec layerSpec(const project::Project& project, LayerId id);

// The declared parameters' values in `paramsJson` (params.json, or empty
// for none), by name: each key's value, or the parameter's default for a
// key that is missing or holds a value the parameter can't take (so a form
// always has one to show). Throws GeneratorError for params.json that isn't
// a JSON object, or that has a key twice.
std::map<std::string, ParamValue> paramValues(const Spec& spec, std::string_view paramsJson);

// params.json with the declared parameter `name` set to `value` and every
// other declared one to its value in `paramsJson` (paramValues), keeping
// the keys nothing declares. Throws GeneratorError for a name the spec
// doesn't declare, a value it can't take (paramProblem), and what
// paramValues refuses.
std::string withParam(const Spec& spec, std::string_view paramsJson, const std::string& name,
                      const ParamValue& value);

// What the program reads on stdin for the layer (above), its params with
// each declared parameter params.json lacks set to its default. Throws
// GeneratorError for a params.json that isn't a JSON object, that has a
// key twice, or that holds a value a declared parameter can't take, and for
// a generator.json that is there and can't be read (parseSpec).
std::string input(const project::Project& project, const Layer& layer);

// The program's stdout as the layer's cells (above): read at `width`
// columns unless its SAUCE says otherwise, blank cells of `key` made
// transparent. Throws GeneratorError for output that isn't an .ans.
Canvas cellsFromOutput(std::string_view ans, int width, std::optional<std::uint8_t> key);

struct RunOptions {
    std::chrono::milliseconds timeout = kDefaultTimeout;
};

// Runs the layer's generator and returns the cells it made, leaving the
// project as it is. Throws GeneratorError, starting nothing, if SIGCHLD is
// ignored (above) or killRunning has been called; and if the program can't
// start, exits with a status other than 0 or crashes (the end of what it
// wrote to stderr in the message), takes longer than the timeout or writes
// more than kMaxOutputBytes (its process group is killed as soon as either
// happens, while it is still writing), and for everything layerSpec, input
// and cellsFromOutput refuse.
Canvas run(const project::Project& project, LayerId id, const RunOptions& options = {});

// Runs `command` as run runs a generator's program, in a new empty
// temporary directory with `stdinData` on stdin, and returns what it wrote
// to stdout: for a program that answers with something other than a
// layer's cells (a scene listing its layers, scene_import.h). Its first
// word is looked up on PATH unless it is an absolute path. `name` leads
// each message. Throws GeneratorError for an empty command, an argument
// holding a NUL, a relative program path, and whatever run refuses about
// starting, running and ending the program.
std::string runCommand(const std::vector<std::string>& command, std::string_view stdinData, const std::string& name,
                       const RunOptions& options = {});

// Kills the process group of the generator running now, if one is, and
// stops any run from starting after: from the first call on, run refuses
// before it starts its program ("not run: generators are being stopped").
// Safe in a signal handler on any thread, for a program that is ending on
// a signal. A signal that comes while a run is starting its program is
// handled once the group is known: the running thread blocks every signal
// until it has published the group, and a call on another thread waits for
// it. A call that found nothing running, and a run that was about to start
// its program, can't both go on: either the call sees the run starting and
// kills its group, or the run sees the call and starts nothing. A group the
// run has retired is never signalled. (One run at a time: it knows the
// latest run's group.)
void killRunning() noexcept;

namespace testing {
// Tests only. `started` is called on the running thread just after the
// program has been started, before its group is published; `killing` with
// every group a run or killRunning signals, just before the signal (from a
// signal handler too); `reaped` just after the run has reaped the program
// that led `group`.
inline std::function<void(pid_t group)> started;
inline std::function<void(pid_t group)> killing;
inline std::function<void(pid_t group)> reaped;
// Lets runs start again after killRunning.
void allowRuns() noexcept;
} // namespace testing

// Runs the layer's generator (run) and makes its output the layer's cells,
// keeping the layer's offset and properties. On a throw the project is as
// it was.
void refresh(project::Project& project, LayerId id, const RunOptions& options = {});

// Adds a generator layer named `name` on top of the stack, the canvas's
// size at (0, 0), with `files` (paths relative to the generator's
// directory, '/'-separated, to bytes) under scripts/<id>/, and runs it
// once for its first cells. Returns its id. Throws GeneratorError, leaving
// the project as it was, when the files have no generator.json, a path in
// them isn't one a project can hold (project::safeEntryPath), or the run
// fails.
LayerId add(project::Project& project, const std::map<std::string, std::string>& files, std::string name,
            const RunOptions& options = {});

} // namespace bw::generator
