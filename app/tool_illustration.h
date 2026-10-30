#pragma once
// The tools' pictures (#153): a small picture of what each tool does, shown
// under "About this tool" in Tool Options and under the tool's heading in
// Help > Manual. Nothing is drawn by hand or kept as an image: each tool has
// a short script, a starting document and a stroke or two, which is run
// through a new copy of the real tool on a private document and view of
// kIllustrationSize cells, with the mouse and keys the canvas gets, and what
// the document then shows is drawn by the app's canvas renderer. A change to
// a tool changes its picture.
//
// The run doesn't touch the open document, its undo stack, the window's
// tools or their settings: the tool is a new one, made with the settings
// pointed at a scratch file (ScratchSettings), so it starts from its
// defaults and what it writes is dropped. The cells are drawn in the
// default palette and the bundled VGA font, which nothing in the app
// changes, so a picture is made once a run, the first time it is asked
// for, and kept.
//
// A tool that changes no cells (the eyedropper picks a colour) has no
// picture, and toolsWithoutIllustration() says why.

#include "blockwright/canvas.h"

#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

namespace bw::help {

inline constexpr QSize kIllustrationSize{16, 6};  // in cells

struct Illustration {
    Canvas before; // the document's composite before the strokes and typing
    Canvas cells;  // the document's composite after the script
    QImage image;  // the cells as the canvas draws them at 1x
};

// The tool's picture, made the first time; null for a tool with none (on
// toolsWithoutIllustration(), or an id with no script).
const Illustration* toolIllustration(const QString& id);

// Runs the tool's script now, uncached; nothing for an id with no script.
std::optional<Illustration> makeToolIllustration(const QString& id);

// The ids with a script, and the tools with no picture by design and why.
// A tool in the registry is on one of the two.
QStringList illustratedTools();
const QList<std::pair<QString, QString>>& toolsWithoutIllustration();

// The id of the tool the script for `id` makes, which should be `id`; empty
// for an id with no script. Makes and drops one.
QString scriptToolId(const QString& id);

} // namespace bw::help
