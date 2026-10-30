#pragma once
// Opening a Python scene as a project (PLAN §7): a scene.py drawn with
// ansidraw's run() becomes one generator
// layer per scene layer, bottom first, each named after its scene layer.
//
// The scene answers for itself (ansidraw's layer modes):
//
//   python3 -B scene.py --list-layers
//       {"width": 80, "rows": 40, "title": "…",
//        "layers": [{"name": "sky"}, {"name": "figure", "key": 13}, …]}
//   python3 -B scene.py --layer-ans=NAME [--key=K]
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
// python3 and the ansidraw it imports; a project moved elsewhere still
// opens, composites and exports its cached cells.
//
// A key is chosen when the scene is opened. If the scene changes so that a
// layer now has a blank cell on its key, that layer's run fails and says
// where; opening the scene again picks new keys.

#include "blockwright/generator.h"
#include "blockwright/project.h"

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

inline constexpr std::string_view kPython = "python3";
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
    std::vector<SceneLayer> layers;  // bottom first

    bool operator==(const Listing&) const = default;
};

// --list-layers' answer read strictly: a JSON object with "width" (1 to
// kMaxColumns) and "rows" (1 to kMaxRows) whole, "title" a string, and
// "layers" a non-empty list of objects, each with a non-empty "name" no
// other has; the first without a "key", every other with a whole "key"
// 0-15. Throws SceneError.
Listing parseListing(std::string_view json);

// The command a layer's generator runs: python3 -B, the scene, --layer-ans=
// the name and, with a key, --key=K.
std::vector<std::string> layerCommand(const std::string& scene, const SceneLayer& layer);

// Asks the scene (an absolute path) for its layers. Throws SceneError.
Listing list(const std::string& scene, const generator::RunOptions& options = {});

// Called before each layer runs: its index (bottom 0), how many there are,
// its name.
using Progress = std::function<void(std::size_t index, std::size_t count, const std::string& name)>;

// The scene at `path` as a new project: the listing's size, iCE on, its
// title in SAUCE when CP437 can hold it, and a generator layer for each
// scene layer (generator::add), run once for its cells. Throws SceneError
// for a path that isn't a file, and for what list and the runs refuse.
project::Project importScene(const std::string& path, const generator::RunOptions& options = {},
                             const Progress& progress = {});

} // namespace bw::scene
