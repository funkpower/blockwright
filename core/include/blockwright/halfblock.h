#pragma once
// Half-block pixels (PLAN §3, §4 tools): every cell is two pixels, its top
// and bottom half, so a canvas of h rows is 2h pixels high. A pixel's row is
// `py`: cell row py / 2, the top half when py is even. The half-block
// brush, the line, rectangle and ellipse tools and fill all draw in these
// pixels, and an eyedropper click picks the colour of the half under it.
//
// A cell holds pixels when it is a space (or NUL or 0xFF), a full block or
// an upper or lower half block. Plotting into any other cell (text, a
// shade) replaces it, the other half taking the colour its background
// shows. A cell holding pixels is written one way: one colour is a space
// on a dark (0-7) background or a full block for a bright one; two colours
// are ▀ unless only ▄ keeps the background dark.
//
// Pixels are the colours a viewer sees, under the document's iCE setting
// (`ice`, iceCell in ansi.h): with iCE off a bright background shows as its
// dark colour (blinking), so a cell is read by what it shows, not by what
// it stores. Two different bright colours in one cell need a bright
// background. With iCE on that is what is written. With iCE off no cell can
// show them: a bright colour painted into one half of a cell whose other
// half is a different bright colour comes out as its dark counterpart
// (colour - 8) in the background, and the other half keeps its colour.
// Only the attributes the colours don't use survive a write (Blink with iCE
// off, where it is the cell's own blink; with iCE on it is the bright
// background and the written colours replace it), and a transparent cell
// that is painted becomes opaque.

#include "blockwright/brush.h"
#include "blockwright/canvas.h"
#include "blockwright/edit.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace bw {

struct HalfPair {
    std::uint8_t top = 0, bottom = 0;  // palette colours
    bool operator==(const HalfPair&) const = default;
};

// A pixel cell's two colours as they show with iCE colours `ice` (RGB
// quantised to the palette); empty for a cell that doesn't hold pixels.
std::optional<HalfPair> halfBlocks(const Cell& cell, bool ice = true);
// The cell that shows these two colours with iCE `ice`. With iCE off and
// two different bright colours the top keeps its colour and the bottom
// shows its dark counterpart; setHalfBlock picks which half that is.
Cell halfBlockCell(HalfPair pair, bool ice = true);
// The cell with one half set to `colour`, read and written by the colours
// it shows with iCE `ice`. A pixel cell keeps its other half, and stays as
// it is when the colour is already there (unless it is transparent: it
// becomes opaque); any other cell becomes pixels with its background colour
// in the other half. With iCE off, a bright colour that would share the
// cell with a different bright colour is written as its dark counterpart,
// so the other half keeps the colour it shows. Attributes the colours don't
// use are kept (see above).
Cell setHalfBlock(const Cell& cell, bool bottom, std::uint8_t colour, bool ice = true);
// Both halves set to `colour`, the attributes kept as setHalfBlock keeps
// them.
Cell setHalfBlocks(const Cell& cell, std::uint8_t colour, bool ice = true);

// The colour a viewer sees at pixel (x, py) with iCE `ice`: the half's
// colour in a pixel cell; elsewhere the cell's foreground or, with
// `background`, the background it shows (an eyedropper: the left button
// picks the foreground, the right the background). Either half of a wide
// pair (PLAN §16) gives the pair's colours, its lead's. Empty off the
// canvas.
std::optional<Color> pickColour(const Canvas& canvas, int x, int py, bool background, bool ice = true);

// Plots pixels over a canvas and keeps the result as one CellEdit. Each
// pixel goes through StrokeOptions (selection mask and transparent lock,
// as for a brush stroke, and the iCE setting). Pixels off the canvas are
// dropped. In mirror mode every pixel is plotted at column width - 1 - x
// too, before the selection is checked, so a mirrored pixel lands in its
// own cell (keeping that cell's other half) and never outside the
// selection. The options' symmetry (StrokeOptions::symmetry) repeats each
// pixel the same way, at its images (symmetryPixels); `mirror` is mirror X
// when the options have none. A pixel off the canvas has images too, each
// clipped on its own, so a shape running off the canvas keeps the images
// of its part off it that land on the canvas. A
// cell is worked out from the canvas and the halves plotted into it, so
// plotting both halves of a cell gives the colour in both whatever the iCE
// setting.
class PixelPainter {
public:
    PixelPainter(const Canvas& canvas, std::uint8_t colour, StrokeOptions options = {}, bool mirror = false);

    const Canvas& canvas() const { return canvas_; }
    // Whether the cell may be painted: on the canvas, in the selection and
    // not transparent under the lock (on a pixel layer, StrokeOptions::
    // pixelLayer: with a pixel set).
    bool paintable(int x, int y) const;
    // Whether pixel (x, py) may be painted: its cell is paintable, and on a
    // pixel layer under the lock the pixel itself is set.
    bool pixelPaintable(int x, int py) const;
    // Plots one pixel in the painter's colour, and its images under the
    // symmetry; false where none is paintable.
    bool plot(int x, int py);
    // Plots one pixel alone, without its images.
    bool plotOne(int x, int py);
    // The symmetry plot() repeats pixels with.
    const Symmetry& symmetry() const { return symmetry_; }
    // The cell as plotted so far; empty off the canvas.
    std::optional<Cell> cellAt(int x, int y) const;
    // The canvas with the pixels plotted, for a live preview.
    Canvas preview() const;
    // The changed cells, row-major, each once (cells plotted back to their
    // original value are left out).
    CellEdit edit() const;
    // How many cells the painter keeps halves for: a window around the
    // cells plotted, so its work follows what is painted, not the canvas
    // size. The window is the plotted cells' bounding box with a margin of
    // half its span on each side, so at most twice their span each way
    // (storedWidth, storedHeight), and never past the canvas.
    std::size_t storedCells() const { return halves_.size(); }
    int storedWidth() const { return ww_; }
    int storedHeight() const { return wh_; }
    // The selection the painter checks: the options' own mask, shared, not
    // a copy of it.
    const Mask* selection() const { return options_.selection.get(); }

private:
    // Grows the window to hold cell (x, y), which is paintable.
    void cover(int x, int y);
    Cell painted(int x, int y, std::uint8_t halves) const;

    const Canvas& canvas_;
    std::uint8_t colour_;
    StrokeOptions options_;
    Symmetry symmetry_;
    // The halves plotted per cell (bit 0 top, bit 1 bottom), row-major over
    // the window of ww_ x wh_ cells at (wx_, wy_); empty until the first
    // plot.
    std::vector<std::uint8_t> halves_;
    int wx_ = 0, wy_ = 0, ww_ = 0, wh_ = 0;
    // The bounding box of the cells plotted, inclusive.
    int bx0_ = 0, by0_ = 0, bx1_ = -1, by1_ = -1;
};

// Plots `pixels` (x, py) in `colour` as one edit, mirrored in mirror mode.
CellEdit plotPixels(const Canvas& canvas, const std::vector<std::pair<int, int>>& pixels, std::uint8_t colour,
                    const StrokeOptions& options = {}, bool mirror = false);

// How the pencil joins one point to the next: Four steps only sideways or
// up and down (line4), so a diagonal is a staircase of pixels touching by
// their sides and never dots; Eight steps diagonally too (line8), the
// thinnest line, its pixels touching at the corners.
enum class Connectivity { Four, Eight };

// The 8-connected line between two points, both ends included: one point
// per step along the longer axis, the other axis rounded to the nearest
// (halves away from the start). Only its points inside the box
// (cx0, cy0)-(cx1, cy1) are returned, and only those are walked.
// The ends are 64-bit (images of int points, within 2^34 of the canvas).
std::vector<std::pair<int, int>> line8(long long x0, long long y0, long long x1, long long y1, long long cx0,
                                       long long cy0, long long cx1, long long cy1);

// The half-block pencil: a stroke of single pixels, each joined to the last
// by a 4-connected line (line4) by default, so a fast drag leaves no gap
// and a diagonal doesn't dot, or an 8-connected one (line8). One edit per
// stroke. Under the options' symmetry (or `mirror`, mirror X) each image of
// the stroke is a track of its own: the images of the points, each joined
// to that image's last point, so a rotated stroke is as joined as the
// stroke.
class HalfBlockStroke {
public:
    HalfBlockStroke(const Canvas& canvas, std::uint8_t colour, StrokeOptions options = {}, bool mirror = false,
                    Connectivity connectivity = Connectivity::Four);

    // Plots (x, py), joined to the last point unless `join` is false. Only
    // the part of a joining line on the canvas is walked.
    void addPoint(int x, int py, bool join = true);
    Canvas preview() const { return painter_.preview(); }
    CellEdit edit() const { return painter_.edit(); }

private:
    void addImage(long long x, long long py, bool join, std::optional<std::pair<long long, long long>>& last);

    PixelPainter painter_;
    Symmetry symmetry_;
    Connectivity connectivity_;
    // each image's last point, in 64 bits: an image can lie beyond int
    std::vector<std::optional<std::pair<long long, long long>>> lasts_;
};

// Fill: the 4-connected pixels of the same colour as (x, py), as they show
// with the options' iCE setting, through pixel cells only (text and shades
// bound a region) and paintable pixels only (the selection bounds it too),
// set to `colour`, as one edit. On a pixel layer (StrokeOptions::pixelLayer)
// transparent pixels are a region of their own. Empty when (x, py) is off
// the canvas, not in a pixel cell or not paintable. In mirror mode the
// filled pixels are plotted mirrored too (the region's mirror image, not a
// second fill).
std::vector<std::pair<int, int>> floodPixels(const Canvas& canvas, int x, int py, const StrokeOptions& options = {});
CellEdit fillPixels(const Canvas& canvas, int x, int py, std::uint8_t colour, const StrokeOptions& options = {},
                    bool mirror = false);

} // namespace bw
