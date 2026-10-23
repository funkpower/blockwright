#pragma once
// What every brush shares (PLAN §10, "Common to all brushes"): a footprint
// stamped at each dab of a stroke, the selection mask and the transparent
// lock that limit where it may paint, pressure mapped to size or strength,
// a live preview while the stroke is drawn and one CellEdit per stroke for
// undo (core/edit.h, the same delta the keyboard edits make).
//
// A brush is a pure function over the cell model: it reads the canvas as it
// stood when the stroke began and writes through the Stroke, which keeps the
// changes. Nothing touches the canvas until the caller applies the delta.

#include "blockwright/canvas.h"
#include "blockwright/edit.h"
#include "blockwright/selection.h"

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace bw {

// Which cells a stroke may paint: a selection. Cells outside the canvas are
// never selected.
class Mask {
public:
    Mask() = default;
    // A mask over a canvas of this size, every cell set or none.
    Mask(int width, int height, bool all = false);
    // Every cell of the rectangle (inclusive corners, any order), clipped to
    // the canvas.
    static Mask rect(int width, int height, int x0, int y0, int x1, int y1);

    int width() const { return w_; }
    int height() const { return h_; }
    bool contains(int x, int y) const {
        return x >= 0 && y >= 0 && x < w_ && y < h_ && bits_[static_cast<size_t>(y) * w_ + x];
    }
    void set(int x, int y, bool on = true);

    bool operator==(const Mask&) const = default;

private:
    int w_ = 0, h_ = 0;
    std::vector<bool> bits_;
};

enum class Tip { Square, Round };
enum class PressureTo { Nothing, Size, Strength };
// The eight compass points, north up the canvas.
enum class Compass { N, NE, E, SE, S, SW, W, NW };
// A unit vector toward the compass point, x right and y down the canvas.
std::pair<double, double> compassVector(Compass c);

// The widest tip, in cells: far more than an 80- or 160-column piece needs.
// A larger size, set or reached through pressure, is taken as this, so a
// stroke's joining lines walk at most about this many points beyond the
// canvas.
constexpr int kMaxBrushSize = 256;

struct BrushSettings {
    int size = 1;            // footprint width in cells, 1 to kMaxBrushSize
    Tip tip = Tip::Square;
    double strength = 1.0;   // 0-1; what it means is up to the brush
    PressureTo pressure = PressureTo::Nothing;
    // Where the light comes from, for the brushes that read one (the rim
    // light brush, brushes/detail.h).
    Compass lightDirection = Compass::NW;

    bool operator==(const BrushSettings&) const = default;
};

// One stamp of the brush, at a cell, with the tablet's pressure (0-1; a
// mouse gives 1). A stroke's symmetry (StrokeOptions::symmetry) stamps
// images of each dab: `image` says which (0 is the dab itself), `flipX`
// and `flipY` whether its tip, and a brush's glyph, are flipped, and
// `turns` how many quarter turns clockwise a brush's glyph is turned after
// the flips (a radial image a quarter turn round; the tip isn't turned).
struct Dab {
    int x = 0, y = 0;
    double pressure = 1.0;
    int image = 0;
    bool flipX = false, flipY = false;
    int turns = 0;
};

// Symmetry (PLAN §10): every dab of a stroke is repeated as these images.
// Mirror X reflects columns (x to width - 1 - x), mirror Y rows, mirror XY
// both (four images); radial turns the dab `folds` times (2-8) about the
// centre: the canvas centre, or the centre of cell `centre`. Mirrors are
// always about the canvas centre.
enum class SymmetryMode { None, MirrorX, MirrorY, MirrorXY, Radial };

struct Symmetry {
    SymmetryMode mode = SymmetryMode::None;
    int folds = 4;
    std::optional<std::pair<int, int>> centre;

    bool operator==(const Symmetry&) const = default;
};

constexpr int kMinFolds = 2, kMaxFolds = 8;

// The images of a dab on a width × height canvas, the dab itself first, in
// a fixed order (mirror XY: itself, X, Y, both). A mirror image is exact:
// the reflected cell with its tip flipped. A radial turn is made on screen,
// where a cell is twice as tall as wide: the dab's centre is turned about
// the centre with rows counted two columns tall and rounded to the nearest
// cell (halves round down and right), so a half turn is exact (both flips)
// and other turns keep the shape's look on screen, not its cells. A turned
// dab keeps its tip as it is (a round tip stays round); a quarter turn
// (four or eight folds) turns its glyph as selection rotation does
// (rotateGlyph), other turns keep it. An image beyond the ends of int is
// left out here (its index stays, so the others keep theirs); a stroke
// keeps its images in 64 bits.
std::vector<Dab> symmetryDabs(const Symmetry& symmetry, const Dab& d, int width, int height);
// The same for a half-block pixel (x, py) of a canvas `width` cells wide and
// `height` rows high (2 × height pixel rows). Pixels are square, so a
// quarter turn about a centre on the pixel grid is exact too. A set centre
// is the centre of its cell, between its two pixels. In 64 bits: an image
// can lie beyond the ends of int.
std::vector<std::pair<long long, long long>> symmetryPixels(const Symmetry& symmetry, int x, int py, int width,
                                                            int height);

// The cells a dab covers, before clipping to the canvas, row-major. A square
// tip is size × size cells. A round tip is the cells whose centres fall in a
// circle `size` columns across, with rows counted as two columns tall (a
// cell is 8×16), so it looks round on screen: about size/2 rows high, and
// never fewer rows than a smaller size. An odd size centres on the dab; an
// even one reaches one column further left, and a multiple of four (an even
// number of rows) one row further up too. The size is taken as 1 to
// kMaxBrushSize. Cells beyond the ends of int are left out: a dab at
// INT_MAX covers only the columns up to INT_MAX.
std::vector<std::pair<int, int>> footprint(int x, int y, int size, Tip tip);
// The same, clipped to a width × height canvas; it visits only the cells on
// the canvas, so a huge size is cheap. `mirrored` flips the tip left to
// right about the dab (an even size reaches one column further right
// instead), so a dab at the mirror column covers the mirror image of the
// cells the unmirrored dab covers. `flippedY` flips it top to bottom the
// same way.
std::vector<std::pair<int, int>> footprint(int x, int y, int size, Tip tip, int width, int height,
                                           bool mirrored = false, bool flippedY = false);

// The cells from one point to another, 4-connected (a diagonal step adds the
// cell beside the corner first), so a fast drag between two dabs leaves no
// gap and a diagonal doesn't dot. Includes both ends. Any ints, the far
// ends included: the arithmetic is 64-bit.
std::vector<std::pair<int, int>> line4(int x0, int y0, int x1, int y1);
// The same line's cells inside the rectangle cx0,cy0-cx1,cy1 (corners
// inclusive), in order: exactly the cells of the line above that fall in
// it, found without walking the rest (the walk starts where the line
// enters, its error term worked out there), so a line from far off is
// cheap. Empty when it misses. The ends are 64-bit (an image beyond int
// keeps its slope); the cells returned are those within int.
std::vector<std::pair<int, int>> line4(long long x0, long long y0, long long x1, long long y1, long long cx0,
                                       long long cy0, long long cx1, long long cy1);

class Stroke;

// A brush. Implementations live in core/brushes/. What a stroke has done so
// far is the Stroke's to keep; a brush may cache what it works out from the
// stroke (the form brush keeps its form), keyed on what it was made from,
// never on where that lives.
class Brush {
public:
    virtual ~Brush() = default;
    virtual std::string_view name() const = 0;
    // Paints one dab: reads through the stroke and writes the cells it
    // changes with Stroke::set. `size` and `strength` already carry the dab's
    // pressure. The default calls paintCell for each covered cell the stroke
    // hasn't painted yet, or has painted only with a weaker dab: a cell
    // changes once, from its value when the stroke began, with the strongest
    // dab that has covered it. So a pass steps a cell once however often the
    // stroke crosses it, and pressing harder deepens it rather than adding a
    // pass. A brush that builds up (smudge) overrides this.
    virtual void dab(Stroke& stroke, const Dab& d, int size, double strength) const;
    // One cell under the dab: its new value, made from the cell as the
    // stroke began (Stroke::originalAt), or empty to leave it.
    virtual std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const;
};

// The selection as a stroke's mask: empty (paint anywhere) when nothing is
// selected, else the selected cells of a width × height canvas.
std::shared_ptr<const Mask> selectionMask(const Selection& sel, int width, int height);

struct StrokeOptions {
    // Empty: the whole canvas. Shared, not copied: a tool copies its options
    // into a stroke or painter on every move, and a mask is canvas-sized.
    std::shared_ptr<const Mask> selection;
    // Leave transparent cells alone: the alpha lock, until layers have their
    // own (PLAN §5).
    bool lockTransparent = false;
    // The document's iCE colours setting. Half-block pixels (halfblock.h)
    // read and write cells by the colours they show with it.
    bool ice = true;
    // The images every dab is repeated as (symmetryDabs). Each image joins
    // its own dabs with 4-connected lines. Where images cover the same
    // cell, the strongest dab paints it, the lower image (the stroke itself
    // first) on a tie, as for one stroke's own dabs.
    Symmetry symmetry;
};

// One stroke, from the first dab to release. Successive dabs are joined by a
// 4-connected line of dabs.
class Stroke {
public:
    Stroke(const Canvas& canvas, const Brush& brush, BrushSettings settings, StrokeOptions options = {});

    // Stamps a dab, joined to the previous one by a 4-connected line unless
    // `join` is false (a new segment of the same stroke). A dab on the last
    // one's cell stamps again, so pressing harder without moving grows the
    // brush or deepens the cells under it; each cell still changes once per
    // stroke, at the strongest dab's strength.
    void addDab(const Dab& d, bool join = true);

    // The canvas as the stroke started.
    const Canvas& canvas() const { return canvas_; }
    const BrushSettings& settings() const { return settings_; }
    const StrokeOptions& options() const { return options_; }
    // A cell as the stroke has left it so far; empty off the canvas.
    std::optional<Cell> cellAt(int x, int y) const;
    // A cell as it was when the stroke began; empty off the canvas.
    std::optional<Cell> originalAt(int x, int y) const;
    // Whether this stroke has painted the cell already (even if it came out
    // the same).
    bool painted(int x, int y) const { return painted_.count({y, x}) != 0; }
    // The strength the cell was painted at; empty if it hasn't been.
    std::optional<double> paintedStrength(int x, int y) const;
    // The cells the stroke has painted so far, (x, y) in row-major order.
    std::vector<std::pair<int, int>> paintedCells() const;
    // The symmetry image that painted it (Dab::image); empty if none has.
    std::optional<int> paintedImage(int x, int y) const;
    // The dab being stamped, while a brush paints it: its image and flips.
    const Dab& currentDab() const { return current_; }
    // Whether the stroke may paint the cell: on the canvas, in the
    // selection, and not transparent under the lock.
    bool paintable(int x, int y) const;
    // Paints a cell, at the strength of the dab that painted it, as the
    // symmetry image `image` (-1: the dab being stamped); refused (false)
    // where it isn't paintable.
    bool set(int x, int y, const Cell& c, double strength = 1.0, int image = -1);

    // The canvas with the stroke so far, for a live preview.
    Canvas preview() const;
    // The stroke's delta, for one undo step: the changed cells in row-major
    // order, each listed once. Cells painted back to their original value
    // are left out.
    CellEdit edit() const;
    // How many dabs the stroke has stamped, the joining lines included.
    // Only the part of a line within reach of the canvas is stamped; it
    // paints what stamping every dab of the line would.
    long long stamps() const { return stamps_; }
    // What the brush keeps over the stroke besides the cells it has painted
    // (the soft brush's rungs and limits): the brush's own, and dropped
    // with the stroke.
    std::shared_ptr<void>& brushState() { return brushState_; }

private:
    void stamp(const Dab& d);

    const Canvas& canvas_;
    const Brush& brush_;
    BrushSettings settings_;
    StrokeOptions options_;
    // One image of a dab, at x, y (64-bit: an image of a dab near the ends
    // of int can lie beyond them), joined to the image's last position.
    void addImage(long long x, long long y, const Dab& d, bool join,
                  std::optional<std::pair<long long, long long>>& last);

    std::vector<std::optional<std::pair<long long, long long>>> lasts_;  // per symmetry image
    Dab current_;
    long long stamps_ = 0;
    struct Painted {
        Cell cell;
        double strength;
        int image;
    };
    std::map<std::pair<int, int>, Painted> painted_;  // (y, x) -> cell, row-major
    std::shared_ptr<void> brushState_;
};

// A whole stroke in one call: the dabs in order, then the delta.
CellEdit paintStroke(const Canvas& canvas, const Brush& brush, const BrushSettings& settings,
                      const std::vector<Dab>& dabs, const StrokeOptions& options = {});

// One pass over a whole region, as one stroke: a dab of size 1 on every
// selected cell (every cell when there is no selection), row by row.
CellEdit paintRegion(const Canvas& canvas, const Brush& brush, const BrushSettings& settings,
                      const StrokeOptions& options = {});

} // namespace bw
