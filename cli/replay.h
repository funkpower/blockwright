#pragma once
// replay (PLAN §9, #208): a steps file (blockwright/steps.h) applied to its
// start through the core brushes, shapes, selection and layer operations,
// as the editor's tools apply them, and the project kept after each step.
// Writing the captures is the command's (main.cpp); everything here is in
// memory, so a step that fails leaves nothing written.

#include "blockwright/brushes/clone.h"
#include "blockwright/edit.h"
#include "blockwright/project.h"
#include "blockwright/steps.h"
#include "blockwright/text_source.h"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace bwcli::replay {

// The project at one point of the replay, and the name its files take:
// "03-outline" after step 3, "03-outline-before" and "03-outline-after"
// for a before-after step.
struct Capture {
    std::string stem;
    bw::project::Project project;
};

struct Outcome {
    std::vector<Capture> captures;
    // A step that changed nothing, say: worth a warning, not a failure.
    std::vector<std::string> notes;
};

// The start the script names: an .ans, XBIN, BIN or project file (relative
// to `dir`, the steps file's directory), a sample in `samplesDir` by its
// directory or .bwp name (with its variant made), or a blank canvas. Throws
// Failure naming "start" when it can't be read. What reading a project
// changed goes in `notes` when given, "start: <file>: " and the change
// (bwcli::projectReadNotes).
bw::project::Project start(const bw::steps::Script& script, const std::filesystem::path& dir,
                           const std::filesystem::path& samplesDir, std::vector<std::string>* notes = nullptr);

// start() for a command that replays `stepsPath`: what reading the start
// changed is said on `err`, a "<program>: note: <stepsPath>: <note>" line
// each, before it returns or the Failure it throws leaves it (a sample's
// variant can fail after the sample was read), so before any step runs and
// whichever way the replay then ends (#370). blockwright-cli replay and
// blockwright --screenshot both start this way (#383).
bw::project::Project startSaying(const bw::steps::Script& script, const std::filesystem::path& dir,
                                 const std::filesystem::path& samplesDir, std::ostream& err,
                                 const std::string& program, const std::string& stepsPath);

// A capture's canvas as replay writes it (project::toAnsi). A project with
// no SAUCE record gets a fresh one, dated `today` (CCYYMMDD) when given and
// else the local date Sauce::fresh gives, so a replay from such a start
// writes the same bytes on any day only with `today` given: blockwright-cli
// replay gives SOURCE_DATE_EPOCH's day. A record the project carries keeps
// its own date.
bw::AnsiDocument document(const bw::project::Project& project, const std::optional<std::string>& today = {});

// The longest named ramp's top rung: where the gradient's "to" starts, so
// a new gradient runs the whole of any ramp.
int longestTopRung();

// What Tool Options and the palette hold between steps: the defaults
// steps.h lists at the start, and each step's options taken as it is
// applied (they hold for the steps after it).
struct ToolSettings {
    bw::Pen pen;
    bw::BrushSettings brush;
    bw::Ramp ramp = *bw::findRamp("fire");
    bw::Symmetry symmetry;
    bw::FormOptions form;
    double hardness = 0.0;
    bw::OutlineShape outline = bw::OutlineShape::Ramp;
    bool keepLint = false;
    char32_t glyph = bw::kFullBlock;
    bool filled = false;
    bw::Connectivity pencil = bw::Connectivity::Four;
    bw::GradientShape gradientShape = bw::GradientShape::Linear;
    bw::GradientBanding gradientBanding = bw::GradientBanding::Cells;
    int gradientFrom = 0, gradientTo = longestTopRung();
    bw::TextureOptions texture;
    bw::SprayOptions spray;
    bw::PasteMode pasteMode = bw::PasteMode::Normal;
    // The clone stamp's (#418): Tool Options' Aligned (on), and the source
    // the clone steps so far set with the distance their strokes set, which
    // the editor's CloneTool keeps across strokes as these are kept across
    // steps.
    bw::CloneSource clone;
    bool sampleMerged = false;  // fill and the eyedropper (CanvasView::sampleMerged)
    // The Text tool's (#421): the settings a new text layer starts with
    // (TextTool::defaults), its text empty.
    bw::TextSource text;
};

// `settings` with the options a step sets taken into it.
void take(ToolSettings& settings, const bw::steps::Options& options);

// The replay just before or after a step, as the editor would hold it: what
// the window's screenshots (#295) put in the window. `settings` are the Tool
// Options and pen the step is applied with, before it as after it: the
// steps before it left them and the step's own options are taken in, as
// they are set in the editor before the step's drag or click. `view` is
// what the "view" steps so far have set (#398), each field the last one
// set, unset where none has: before a view step it is what the steps before
// it left, after it the step's fields are taken in.
struct Moment {
    const bw::steps::Step& step;
    bool before;
    const bw::project::Project& project;
    bw::LayerId active;
    const bw::Selection& selection;
    const ToolSettings& settings;
    const bw::steps::View& view;
};

// The script's first "view" step, if any (#398): only the window has a
// view, so blockwright-cli replay refuses a script with one.
const bw::steps::Step* viewStep(const bw::steps::Script& script);
using Observer = std::function<void(const Moment&)>;

// The script's steps applied to `project` in order. Throws Failure naming
// the step ("step 3 (outline): ...") for one that can't be applied: a point
// off the canvas, no selection to transform, a layer that can't take the
// edit or isn't there, or any other error the step raises but running out
// of memory (std::bad_alloc goes through). `observe`, if given, sees each
// step before and after it is applied; what it throws stops the replay.
Outcome run(const bw::steps::Script& script, bw::project::Project project, const Observer& observe = {});

// "07": the step's number, as wide as the last step's and at least two.
std::string stepNumber(int number, int steps);

} // namespace bwcli::replay
