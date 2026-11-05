#pragma once
// replay (PLAN §9, #208): a steps file (blockwright/steps.h) applied to its
// start through the core brushes, shapes, selection and layer operations,
// as the editor's tools apply them, and the project kept after each step.
// Writing the captures is the command's (main.cpp); everything here is in
// memory, so a step that fails leaves nothing written.

#include "blockwright/project.h"
#include "blockwright/steps.h"

#include <filesystem>
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
// Failure naming "start" when it can't be read.
bw::project::Project start(const bw::steps::Script& script, const std::filesystem::path& dir,
                           const std::filesystem::path& samplesDir);

// The script's steps applied to `project` in order. Throws Failure naming
// the step ("step 3 (outline): ...") for one that can't be applied: a point
// off the canvas, no selection to transform, a layer that can't take the
// edit or isn't there.
Outcome run(const bw::steps::Script& script, bw::project::Project project);

// "07": the step's number, as wide as the last step's and at least two.
std::string stepNumber(int number, int steps);

} // namespace bwcli::replay
