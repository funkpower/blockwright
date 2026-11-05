#pragma once
// Steps files (PLAN §9, #208): a script of edits, read here and replayed
// by `blockwright-cli replay` through the core brushes, shapes and layer
// operations the editor's tools use, so every tutorial picture can be made
// again. The window's own replay (#295) reads the same files with this
// parser.
//
// A steps file is a JSON object:
//
//   {
//     "version": 1,
//     "start": {"file": "piece.ans"} | {"file": "piece.bwp"}
//              | {"sample": "01-layers-showcase", "variant": "Day"}
//              | {"blank": [80, 25]},
//     "ice": true,                     optional: the iCE setting of a blank start
//     "steps": [ {step}, ... ]
//   }
//
// Each step is {"name": ..., "action": ..., ...}. The name is 1-64 of
// a-z 0-9 - _, starting with a letter or digit: it names the step's output
// files. The action is what the editor calls it: a tool's id (the Tools
// menu's <id>Tool actions) for a stroke, a shape or a click, or a menu
// action's object name (newCellsLayer, flipHorizontal), and three names of
// the replay's own for what the editor does in a dock or by dragging
// (setActiveLayer, layerProperties, moveSelection). A tool step with no
// points only sets options, as choosing the tool does. Any step may also
// set Tool Options (below); what it sets holds for the steps after it, as
// Tool Options do in the editor. "capture": "before-after"
// asks for the canvas before the step as well as after it.
//
// Every key is checked: an unknown key, a key the action doesn't take, a
// value of the wrong kind or out of range is refused, and the error names
// the step by number and name. Where a point, rectangle or radial symmetry
// centre lies is checked when the step is replayed, against the canvas it is
// applied to.

#include "blockwright/brush.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw::steps {

constexpr int kVersion = 1;
// A blank start's size, at most: as large as a canvas the editor makes.
constexpr int kMaxBlankSide = 4096;

class StepsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Start {
    enum class Kind { Blank, File, Sample };
    Kind kind = Kind::Blank;
    int width = 80, height = 25;  // Blank
    std::string file;             // File: as written, relative to the steps file
    std::string sample;           // Sample: its directory or .bwp name in the samples
    std::string variant;          // Sample: a variant its notes list; empty for as saved
};

// Tool Options a step sets. Each one unset keeps the value the steps before
// it left (the defaults below at the start).
struct Options {
    std::optional<std::uint8_t> fg, bg;   // the pen; default 7 on 0
    std::optional<int> size;              // 1-kMaxBrushSize; default 1
    std::optional<Tip> tip;               // default square
    std::optional<double> strength;       // 0-1; default 1
    std::optional<Compass> light;         // default NW
    std::optional<Ramp> ramp;             // default fire
    std::optional<Symmetry> symmetry;     // default none
    std::optional<FormOptions> form;      // the form brush's shape, rim and glint
    std::optional<double> hardness;       // the soft brush's, 0-1; default 0
    std::optional<OutlineShape> outline;  // the outline brush's shape; default ramp
    std::optional<bool> keepLint;         // the rim light brush's; default off
    std::optional<char32_t> glyph;        // the glyph brush's; default █
    std::optional<bool> filled;           // rectangle and ellipse; default outline
    std::optional<Connectivity> pencil;   // the half-block brush's joins; default 4
    std::optional<GradientShape> gradientShape;
    std::optional<GradientBanding> gradientBanding;
    std::optional<int> gradientFrom, gradientTo;  // rungs; default 0 to the ramp's top
    std::optional<TextureOptions> texture;
    std::optional<SprayOptions> spray;
    std::optional<PasteMode> pasteMode;   // paste and moveSelection; default normal
};

// A point of a stroke: a cell for the brushes, a half-block pixel (x, and a
// pixel row, two to a cell row) for the half-block tools.
struct Point {
    int x = 0, y = 0;
    double pressure = 1.0;
    bool operator==(const Point&) const = default;
};

enum class Button { Left, Right };
enum class Capture { After, BeforeAfter };

struct Step {
    int number = 0;  // from 1, as the files and errors name it
    std::string name;
    std::string action;
    Capture capture = Capture::After;
    Options options;
    Button button = Button::Left;
    std::vector<Point> points;    // a stroke's, a shape's two corners, a click
    std::u32string text;          // keyboard, NFC-composed as keyboard mode types it
    std::optional<CellRect> rect; // select
    int dx = 0, dy = 0;           // move (canvas pixels on a reference layer), moveSelection
    std::string layer;            // setActiveLayer: the layer's name
    // layerProperties: what it changes on the active layer
    std::optional<std::string> rename;
    std::optional<bool> visible, locked, alphaLocked;
};

struct Script {
    Start start;
    std::optional<bool> ice;
    std::vector<Step> steps;
};

// The actions a steps file may name, in the order PLAN §9 lists them.
const std::vector<std::string_view>& actions();
// Whether the action works in half-block pixels (its points are pixels).
bool pixelAction(std::string_view action);
// "step 3 (outline-head)": how errors name a step.
std::string stepLabel(const Step& step);

// Reads a steps file's text. Throws StepsError naming the step (or the top
// level) for anything it refuses.
Script parse(std::string_view json);

} // namespace bw::steps
