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
#include <QTemporaryFile>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace bw {

class MainWindow;

namespace screenshot {

// The window's size when the steps file gives none, in the window's own
// pixels at every scale: a "scale": 2 picture of it is 2560 x 1600.
inline constexpr QSize kDefaultWindowSize{1280, 800};

// The screens `blockwright --screenshot` runs on, whatever the machine has:
// offscreen, two side by side, each 4096 pixels a side (the largest window a
// steps file may ask for) at 96 dpi, the first at device pixel ratio 1 and
// the primary screen, the second at 2 for "scale": 2. Made before the
// QApplication: it writes the offscreen platform's configuration file,
// points QT_QPA_PLATFORM at it and clears what would scale the screens or
// their fonts from the environment (QT_SCALE_FACTOR, QT_SCREEN_SCALE_FACTORS,
// QT_FONT_DPI and the like). The file goes in the temporary folder, or (on Unix) in
// /tmp when that folder can't be written or has a ':' or ';' in its path
// (the platform's arguments are split at ':', its list of platforms at ';').
// The platform reads it once, as the QApplication is made: release() then
// removes it, and so does going.
class FixedScreens {
public:
    FixedScreens();
    // empty when it worked, else why not
    const std::string& error() const { return error_; }
    // The configuration file, as QT_QPA_PLATFORM names it.
    const QString& path() const { return path_; }
    // Remove the file, once the QApplication has read it.
    void release() { file_.remove(); }
    // The configuration file's contents.
    static QByteArray configuration();

private:
    QTemporaryFile file_;
    QString path_;
    std::string error_;
};

struct Shot {
    // "03-stroke": the replay's name for the step's capture, with "-before"
    // for the picture before it, and "-after" after it when there is a
    // picture before it too or the step is marked "capture": "before-after".
    std::string stem;
    QImage image;
};

// The script replayed from `start` with `window` showing each screenshot's
// moment, every picture in memory. A picture at "scale": 1 is taken with the
// window on the primary screen, one at "scale": 2 with the window moved to
// a screen at device pixel ratio 2 (FixedScreens has one), the same size in
// its own pixels; no such screen fails at the step. A moment is the replay's project, active
// layer and selection, the step's tool, and the Tool Options and pen the
// step is applied with (bwcli::replay::Moment::settings), which Tool Options
// and the palette show. The window is resized to the script's
// size (kDefaultWindowSize without one) and its cell cursor hidden. A dock
// asked for is brought to the front of its tabs, and each shown dock in the
// picture that follows the document (Lint, IRC View) is waited for until it
// shows the moment's document. Throws bwcli::Failure naming the step for a
// step that can't be applied, a dock or widget the window doesn't have, or
// a crop off it. `taken`, for tests, sees each picture as it is taken,
// while the window still shows its moment.
std::vector<Shot> take(MainWindow& window, const steps::Script& script, project::Project start,
                       const std::function<void(const Shot&)>& taken = {});

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
