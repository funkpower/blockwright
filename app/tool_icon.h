#pragma once
// The tools' icons (PLAN §11, #224). The generic tools use Tabler Icons
// (MIT, third_party/tabler-icons/), the ANSI-specific ones are drawn here on
// the same 24-unit grid and 2-unit stroke. Each icon is drawn when it is
// asked for, in the palette's colours of the moment and at the size and
// device pixel ratio asked for, so it stays sharp at any scale and legible on
// a light or a dark colour scheme.

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QString>

class QWidget;

namespace bw {

// The icon the toolbox and the Tools menu show for a tool, drawn in
// `paletteFrom`'s palette while that widget lives, else the application's.
QIcon toolIcon(const QString& toolId, const QWidget* paletteFrom = nullptr);

// The colours an icon is drawn in for a mode: `ink`, the foreground, and
// `paper`, the background it is drawn for, which the tones of a ramp or a
// fill are mixed towards. Normal, Active and On use ButtonText on Button;
// Disabled the disabled group's; Selected HighlightedText on Highlight.
struct ToolIconColours {
    QColor ink;
    QColor paper;
};
ToolIconColours toolIconColours(const QPalette& palette, QIcon::Mode mode);

// The Tabler icon a tool's comes from ("arrows-move"), or empty for one
// drawn here.
QString toolIconSource(const QString& toolId);

} // namespace bw
