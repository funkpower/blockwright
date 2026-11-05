#pragma once
// The window's replay (PLAN §9, #295): `blockwright --screenshot` plays a
// steps file (blockwright/steps.h) through the same replay as
// `blockwright-cli replay` (cli/replay.h), puts the replay into the real
// window at each moment a step's "screenshot" asks for (the project, its
// active layer, the selection and the step's tool), and grabs the window, a
// dock or a crop of it as a PNG.

#include "blockwright/project.h"
#include "blockwright/steps.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace bw {

class MainWindow;

namespace screenshot {

// The window's size when the steps file gives none.
inline constexpr QSize kDefaultWindowSize{1280, 800};

struct Shot {
    // "03-stroke": the replay's name for the step's capture, with "-before"
    // for the picture before it, and "-after" after it when there is a
    // picture before it too or the step is marked "capture": "before-after".
    std::string stem;
    QImage image;
};

// The script replayed from `start` with `window` showing each screenshot's
// moment, every picture in memory. The window is resized to the script's
// size (kDefaultWindowSize without one) and its cell cursor hidden. A dock
// asked for is brought to the front of its tabs, and each shown dock in the
// picture that follows the document (Lint, IRC View) is waited for until it
// shows the moment's document. Throws bwcli::Failure naming the step for a
// step that can't be applied, a dock or widget the window doesn't have, or
// a crop off it.
std::vector<Shot> take(MainWindow& window, const steps::Script& script, project::Project start);

// Writes each shot as <dir>/<stem>.png, all of them or none, as
// `blockwright-cli replay` writes its captures (bwcli::saveAll): every PNG
// written and flushed beside its place first, then each moved into place,
// and a failure removes the ones it moved in that weren't there before;
// <dir> is made if it isn't there. `beforeCommit`, for tests, runs once
// every picture is written and before any is moved into place. Throws
// bwcli::Failure.
void write(const std::vector<Shot>& shots, const std::filesystem::path& dir,
           const std::function<void()>& beforeCommit = {});

// The command: the steps file at `stepsPath` replayed in a new window
// under scratch settings, its shots written to `outDir`. The exit status;
// what failed is on stderr.
int run(const QString& stepsPath, const QString& outDir);

} // namespace screenshot
} // namespace bw
