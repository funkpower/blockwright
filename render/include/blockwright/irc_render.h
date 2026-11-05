#pragma once
// The IRC view (irc_view.h) as an image: the view's cells drawn with the VGA
// font, and a red bar where a line over the budget is cut.
#include "blockwright/irc_view.h"
#include "blockwright/render.h"

#include <QImage>
#include <QRgb>

namespace bw {

inline constexpr QRgb kIrcCutMark = 0xffff3030;

// The view drawn as renderCanvas draws a canvas, with a bar a quarter of a
// glyph wide (at least a pixel) in kIrcCutMark on every cut row: at the left
// edge of the first cell the row loses, or, when the cells it loses all lie
// past the view's width, at the view's right edge.
// Throws as renderCanvas does.
QImage renderIrcView(const irc::View& view, int glyphWidth = 8, int scale = 1);

} // namespace bw
