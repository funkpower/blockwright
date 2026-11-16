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
//              (a path relative to the steps file: pathProblem below)
//     "ice": true,                     optional: the iCE setting of a blank start
//     "steps": [ {step}, ... ]
//   }
//
// Each step is {"name": ..., "action": ..., ...}. The name is 1-64 of
// a-z 0-9 - _, starting with a letter or digit: it names the step's output
// files. The action is what the editor calls it: a tool's id (the Tools
// menu's <id>Tool actions) for a stroke, a shape or a click, or a menu
// action's object name (newCellsLayer, flipHorizontal, canvasSize), and
// three names of the replay's own for what the editor does in a dock or by
// dragging (setActiveLayer, layerProperties, moveSelection). A tool step
// with no points only sets options, as choosing the tool does. Any step but
// a view or canvasSize step may also set Tool Options (below), but for
// "sampleMerged", which only a fill or eyedropper step takes; what it sets
// holds for the steps after it, as Tool Options do in the editor.
// "capture": "before-after" asks for the canvas before the step as well as
// after it.
//
// Two keys are the window's (#295), which `blockwright --screenshot` reads
// and `blockwright-cli replay` passes over: a top-level "window": [w, h],
// the window's size in pixels, and a step's "screenshot":
//
//   "screenshot": {"of": "window" | a dock's or widget's object name,
//                  "crop": {"cells": [x, y, w, h]} | {"pixels": [x, y, w, h]},
//                  "when": "before" | "after" | "both",
//                  "scale": 1 | 2,
//                  "pointer": bool}
//
// each key optional (the window, uncropped, after the step, at scale 1, no
// pointer). "scale": 2 draws the picture at device pixel ratio 2, twice the
// pixels each way, from the same window laid out at the same size.
// "pointer": true draws an arrow mouse pointer with its tip at the step's
// pointer position (pointerAt): only a step the pointer makes may ask.
// A clone step's "source" is a click of the pointer's (Ctrl+click), before
// its stroke.
//
// Format > Canvas Size... is a step too (#420), "action": "canvasSize":
//
//   {"name": ..., "action": "canvasSize", "size": [w, h],
//    "anchor": "top-left" | "top" | "top-right" | "left" | "centre"
//              | "right" | "bottom-left" | "bottom" | "bottom-right"}
//
// "size" is the new canvas in cells, 1 to kMaxColumns by 1 to kMaxRows
// (ansi.h, the dialog's limits), and "anchor" the dialog's anchor, where
// the old canvas sits in the new one (top-left without one, where the
// dialog starts). It is applied as the dialog applies it (resizeCanvas,
// with the selection moved and cut by resizedSelection), and the steps
// after it are checked against the new size. Its "size" is the canvas's,
// so it takes no Tool Options, as a view step takes none.
//
// The window's view has a step of its own (#398), "action": "view":
//
//   {"name": ..., "action": "view", "zoom": 1-8, "scrollTo": [x, y],
//    "ircFit": bool, "ircPad": bool, "ircSwapShades": bool,
//    "guides": "none" | "80x25" | "80x40" | "80x50"}
//
// each key optional, at least one given; one left out keeps what the window
// shows. "zoom" is View > Zoom In / Zoom Out's level, "scrollTo" a cell
// scrolled to the top left of the canvas's viewport (as far as the scroll
// bars go; it must be on the canvas), and the three booleans View > IRC's
// Fit Lines to the Budget, Pad Lines and Swap Shades. "guides" is View >
// Guides (#431): the guide set drawn over the canvas, the guideNone,
// guide80x25, guide80x40 and guide80x50 actions. It takes effect where
// it stands, so later steps' screenshots show it until another view step
// changes it. It changes no cell, so it is no document edit. Only the
// window has a view: `blockwright-cli replay` refuses a file with a view
// step (exit status 1), and only `blockwright --screenshot` replays it.
//
// The Clone Stamp is a step too (#418), "action": "clone":
//
//   {"name": ..., "action": "clone", "source": [x, y], "points": [...],
//    "button": "left" | "right", "aligned": bool}
//
// "source" is the cell Ctrl+click sets (CloneTool::setSource): it is set
// before the step's stroke, and holds for later clone steps, with the
// distance the first stroke after it sets, as the editor keeps both. A
// clone step with a source and no points only sets the source. A stroke
// needs a source, on its own step or an earlier clone step. "aligned" is
// Tool Options' Aligned, on by default; only a clone step takes it, and it
// holds for later steps. Either button copies the same.
//
// The Text tool is a step too (#421), "action": "text":
//
//   {"name": ..., "action": "text", "at": [x, y], "text": "...",
//    "textOptions": {"face": "block" | "vga" | "mini"
//                            | {"tdf": "fonts/x.tdf", "font": "NAME",
//                               "index": 0-255},
//                    "case": "as-typed" | "capitals",
//                    "letterSpacing": 0-64, "lineSpacing": 0-64,
//                    "align": "left" | "centre" | "right",
//                    "wrap": 0-65535, "colour": 0-15,
//                    "bigStyle": "none" | "shadow" | "3d" | "outline"
//                                | "block" | "rainbow",
//                    "bigColours": [0-15, ...]}}
//
// "at" is the Text tool's click, a canvas cell off every text layer it
// would edit (textLayerAt), and "text" what is typed there, "\n" for
// Enter: a new live text layer, "Text N", just above the active layer and
// made active, its letters' top-left cell at "at" (makeTextLayer), as the
// tool makes it from the first character typed. "at" and "text" go
// together; "text" is 1 to TextSource::kMaxLength characters, none of them
// a control character (C0, DEL or C1, typedInText in text.h) but "\n".
// "textOptions" is the tool's Tool Options, each key optional (a bundled
// face's name or a TheDraw font, the rest as the dock shows them); only a
// text step takes it,
// and what it sets holds for later text steps, as Tool Options keep what a
// new text layer starts with. The step's click comes first, so its options
// set the new layer's; a text step with no "at" only sets them, and when
// the active layer is a live text layer that isn't locked it sets them on
// that layer too, as Tool Options with the Text tool chosen do
// (TextTool::setSettings). A TheDraw face (#422) names a .tdf file
// relative to the steps file, as "start"'s "file" is (pathProblem), and the
// font's name in it: the only font of that name, or the one at "index",
// which must have it. The step lists the file (Step::files), so the load
// pass reads it; the replay finds the font when the step is applied.
//
// Three steps edit text layers and add a reference layer (#422):
//
//   {"name": ..., "action": "editText", "text": "..."}
//   {"name": ..., "action": "applyTextStyle",
//    "style": "chrome" | {"file": "styles/x.style.json"}}
//   {"name": ..., "action": "openReferenceImage", "file": "refs/sketch.png"}
//
// editText replaces the active layer's whole text, which must be a live
// text layer the Text tool could click into (visible and unlocked), in one
// edit; "text" is checked as a text step's is. applyTextStyle puts a
// Library text style on the active live text, cells or pixels layer, as
// the Library dock's activation does (MainWindow::applyTextStyle): a
// bundled preset by its name (the file name before .style.json in the
// bundled library's text-styles/), or a .style.json relative to the steps
// file, read strictly (library::parseTextStyleEntry). openReferenceImage is
// File > Open Reference Image...: a PNG or JPEG relative to the steps file,
// kept byte for byte (any other format is refused), as a new top reference
// layer "Reference N", made active, the canvas's width in pixels with its
// aspect kept. The load pass reads each step's file, and the presets.
//
// Two steps use the Library (#431):
//
//   {"name": ..., "action": "placeStamp", "stamp": "Oak tree", "at": [x, y],
//    "library": "bundled" | "user"}
//   {"name": ..., "action": "saveToLibrary", "stamp": "Oak tree",
//    "category": "Nature", "tags": ["tree", ...]}
//
// placeStamp is a double-click or drag from the Library dock
// (MainWindow::placeStamp): the stamp named "stamp" (its SAUCE title, in
// UTF-8 here) floated with its top-left at "at" in transparent paste mode,
// for that float only (Tool Options' Paste Mode is kept), and committed as
// a paste step is; the Select tool is the active tool after it. The name
// is looked up in the bundled library, the user library the replay is
// given and the stamps earlier saveToLibrary steps saved; "library" says
// which of the two libraries to look in when both hold the name. The load
// pass reads the libraries (Script::bundledStamps, Script::userStamps).
// saveToLibrary is Selection > Save to Library...: the selection's cells as
// the canvas shows them, as a stamp named "stamp" in category "category"
// with "tags" (none if left out), into the user library, replacing a stamp
// of that name there as the dialog does when told to. The name and every
// tag hold only characters CP437 has a glyph for.
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
#include "blockwright/canvas_size.h"
#include "blockwright/halfblock.h"
#include "blockwright/library.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/text_source.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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

// Paths in a steps file (#422): "start"'s "file", and every file a step
// reads, is named relative to the steps file's directory with `/` between
// directories, so the file and what it names replay the same anywhere.
// `..` may leave that directory (tutorials share a fonts folder). Returns
// why `path` can't be one, to be said after the key's quoted name:
// empty, a NUL in it, a `\` in it (a separator only Windows reads, and the
// start of a UNC path), or absolute (a leading `/`, or a drive letter and
// `:`). The path is quoted in what it says, but for a NUL.
std::optional<std::string> pathProblem(std::string_view path);

// A file a step reads (#422): the key that names it ("tdf", "file"), its
// path as written (pathProblem passes it) and the most bytes it may hold.
// The load pass (bwcli::replay::load) reads each into Script::files before
// the replay runs, so applying a step does no I/O.
struct FileRef {
    std::string key;
    std::string path;
    std::size_t maxBytes = 0;
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
    std::optional<PressureTo> pressure;   // what a point's pressure scales; default none
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
    // The clone stamp's Aligned (CloneSource::aligned); default on. Only a
    // clone step sets it, as only that tool shows it.
    std::optional<bool> aligned;
    // Sample merged: fill floods by, and the eyedropper picks from, the
    // composite instead of the active layer; default off. Only a fill or
    // eyedropper step sets it, as only those tools show it.
    std::optional<bool> sampleMerged;
    // The Text tool's (#421): what a new text layer starts with. Only a
    // text step sets them.
    struct Text {
        std::optional<std::string> face;  // a bundled face's name
        // Or a TheDraw font (#422): `font`, the font's name, in the .tdf file
        // at `path` (relative to the steps file; Step::files names it), at
        // `index` (0 to kMaxTdfFonts - 1) when given, else the only font of
        // that name in the file. Set instead of `face`.
        struct Tdf {
            std::string path;
            std::string font;
            std::optional<int> index;
            bool operator==(const Tdf&) const = default;
        };
        std::optional<Tdf> tdf;
        std::optional<TextCase> textCase;
        std::optional<int> letterSpacing, lineSpacing;  // 0-kMaxSpacing
        std::optional<TextAlign> align;
        std::optional<int> wrapWidth;  // 0-kMaxWrapWidth, 0 for none
        std::optional<std::uint8_t> colour;  // 0-15
        std::optional<BigTextStyle> bigStyle;
        std::optional<std::vector<std::uint8_t>> bigColours;  // mIRC 0-15, at most kMaxBigColours
        bool operator==(const Text&) const = default;
    };
    std::optional<Text> text;
};

// `source`'s settings after Tool Options take those `text` sets, its text
// kept, as TextOptions::apply sets them: a big style that changes resets
// the big colours to the style's own (bigTextDefaultColours), no big style
// clears them, and under a big style the face is block and the colours are
// set, the style's own when none are. Returns why Tool Options can't be set
// so, leaving `source` as it was: a face other than block or a colour under
// a big style (Tool Options turns the face picker and the colour off then),
// or big colours without one (their field is off).
// `tdf` is the face text.tdf names, as the replay found it in the file
// (kind Tdf: the file's sha256, the font's index and name); with text.tdf
// set and no `tdf`, nothing is changed and the problem is said.
std::optional<std::string> applyTextOptions(TextSource& source, const Options::Text& text,
                                            const FaceRef* tdf = nullptr);

// A point of a stroke: a cell for the brushes, a half-block pixel (x, and a
// pixel row, two to a cell row) for the half-block tools.
struct Point {
    int x = 0, y = 0;
    double pressure = 1.0;
    bool operator==(const Point&) const = default;
};

enum class Button { Left, Right };
enum class Capture { After, BeforeAfter };

// A picture of the window a step asks for (#295).
struct Screenshot {
    enum class When { Before, After, Both };
    enum class Crop { None, Cells, Pixels };
    std::string of = "window";  // "window", or a dock's or widget's object name
    Crop crop = Crop::None;
    int x = 0, y = 0, width = 0, height = 0;  // the crop: canvas cells, or pixels of `of`
    When when = When::After;
    int scale = 1;  // the picture's device pixel ratio, 1 or 2
    bool pointer = false;  // an arrow drawn at the step's pointer (pointerAt)
};

// The view a "view" step sets (#398): each unset keeps the window's.
// Zoom's limits are CanvasView's (kMinZoom-kMaxZoom).
constexpr int kMinZoom = 1, kMaxZoom = 8;
struct View {
    std::optional<int> zoom;
    std::optional<Point> scrollTo;  // a cell
    std::optional<bool> ircFit, ircPad, ircSwapShades;
    // View > Guides (#431): "none", "80x25", "80x40" or "80x50"
    std::optional<std::string> guides;
    bool operator==(const View&) const = default;
};

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
    std::optional<Screenshot> screenshot;
    View view;  // view
    // canvasSize: the new size in cells and where the old canvas sits in it
    int canvasWidth = 0, canvasHeight = 0;
    Anchor anchor = Anchor::TopLeft;
    // clone: the source cell Ctrl+click sets before the stroke (#418)
    std::optional<Point> source;
    // The files the step reads (#422), in the order it names them: a text
    // step's TheDraw face (key "textOptions face tdf"), applyTextStyle's
    // .style.json ("style file") and openReferenceImage's picture ("file").
    std::vector<FileRef> files;
    // applyTextStyle (#422 D2): a bundled Library preset by its name, the
    // file name before .style.json in the bundled library's text-styles/;
    // the load pass reads it into Script::presets. Unset for a style file.
    std::optional<std::string> stylePreset;
    // placeStamp and saveToLibrary (#431): the stamp's name as its SAUCE
    // title, in CP437 bytes; saveToLibrary's category and tags (UTF-8); the
    // library placeStamp looks in, unset for either.
    std::string stamp;
    std::string category;
    std::vector<std::string> tags;
    enum class Library { Bundled, User };
    std::optional<Library> library;
};

// The window's size a steps file may set, in pixels.
constexpr int kMinWindowSide = 320;
constexpr int kMaxWindowSide = 4096;

struct Script {
    Start start;
    std::optional<bool> ice;
    std::optional<int> windowWidth, windowHeight;  // "window": [w, h]
    std::vector<Step> steps;
    // Each step's files' bytes by their paths as written, the load pass's
    // (bwcli::replay::load); empty until it runs.
    std::map<std::string, std::string> files;
    // The bundled text style presets applyTextStyle steps name, their
    // .style.json bytes by name, read by the load pass too.
    std::map<std::string, std::string> presets;
    // The stamps placeStamp steps can name (#431), read by the load pass
    // when a step places one: the bundled library's and the user library's
    // the replay is given (library::scan), each sorted by path.
    std::vector<library::Stamp> bundledStamps, userStamps;
    // For each name a saveToLibrary step saves, the user library's stamp a
    // save of it writes over (library::stampPathFor: one of that title in
    // the library's own folder, never one in a subfolder), read by the load
    // pass; a name the library holds no such stamp of isn't here.
    std::map<std::string, std::filesystem::path> userOverwrites;
};

// The actions a steps file may name, in the order PLAN §9 lists them.
const std::vector<std::string_view>& actions();
// Whether the action works in half-block pixels (its points are pixels).
bool pixelAction(std::string_view action);
// Where the mouse pointer is in a picture of `step` (#295): a point of its
// own (a cell, or a half-block pixel for a pixelAction), the last one after
// the step (a stroke's or drag's end, the click) and the first before it
// (where the press starts); select's rect is dragged from its top-left
// corner to its bottom-right one, and a clone step's "source" is its first
// press (the Ctrl+click), so before the step the pointer is there, and after
// it too when the step has no points. Empty for a step the pointer doesn't make
// (a menu or dock action, canvasSize, a move "by", a tool step with no
// points, a view step), which can't ask for "pointer".
std::optional<Point> pointerAt(const Step& step, bool before);
// "step 3 (outline-head)": how errors name a step.
std::string stepLabel(const Step& step);

// Reads a steps file's text. Throws StepsError naming the step (or the top
// level) for anything it refuses.
Script parse(std::string_view json);

} // namespace bw::steps
