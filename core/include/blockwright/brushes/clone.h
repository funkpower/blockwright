#pragma once
// Clone stamp (PLAN §10, "Detail"): copies cells from a source point to
// wherever the stroke goes, keeping the distance between them.

#include "blockwright/brush.h"

#include <map>
#include <optional>
#include <utility>

namespace bw {

// Clone brush: each cell under the tip becomes the cell at the same place
// relative to `source` as the cell is to `anchor` (the stroke's first dab,
// or an earlier stroke's when aligned; CloneSource), whole: glyph, colours
// and flags, transparency included. It reads the canvas as the stroke
// began (Stroke::originalAt), so a source that overlaps the strokes copies
// what was there, never what the stroke has just painted. A source cell off
// the canvas copies nothing, and the cell is left. Strength doesn't matter:
// a cell is copied once per stroke, on Stroke's default dab, with the
// selection and the alpha lock limiting where it lands (not where it
// reads from).
//
// Under symmetry each image copies from the image of the source: image k
// paints its cell from the cell at image_k(source) - image_k(anchor) from
// it, as it is (not flipped). So on a canvas symmetric about the mirror the
// result stays symmetric, the mirror half copied from the source's mirror;
// on a canvas that isn't, each image copies whatever lies at the source's
// image. Under a mirror (X, Y or both) a dab's images are copied together,
// from the stroke's own: a cell several of them cover goes to the image
// whose dab is nearest it on screen (a row two columns tall); on a tie (a
// cell on an axis, or the images' dabs meeting on the axis of an odd
// canvas) to the image whose frame has the cell in the canvas's first half,
// across, then down, then to the lower image. A cell keeps the first copy
// the stroke lays on it. So a cell and its mirror cell take the copies of
// two images that mirror each other, and a symmetric canvas stays
// symmetric, the axes included, except that a cell on an axis copies a
// cell off it as it is: a half block there is not turned to its own mirror.
// A radial image is stamped on its own, as on Stroke's default dab. An
// image of the source or anchor beyond the ends of int copies nothing.
class CloneBrush : public Brush {
public:
    CloneBrush(std::pair<int, int> source, std::pair<int, int> anchor) : source_(source), anchor_(anchor) {}

    std::string_view name() const override { return "clone"; }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

    std::pair<int, int> source() const { return source_; }
    std::pair<int, int> anchor() const { return anchor_; }
    // The cell the stroke's own image copies to cell x, y (off the canvas
    // or not).
    std::pair<long long, long long> sampleAt(int x, int y) const;
    // How far image `image` reads from the cell it paints, on a canvas of
    // this size; empty where that image of the source or anchor is left out.
    std::optional<std::pair<long long, long long>> offset(const Symmetry& symmetry, int image, int width,
                                                          int height) const;

private:
    // What image `image` copies to cell x, y.
    std::optional<Cell> copy(const Stroke& stroke, int x, int y, int image) const;

    std::pair<int, int> source_, anchor_;
    // The images' offsets as they are needed, for the canvas size and
    // symmetry they were worked out on.
    struct Cached {
        Symmetry symmetry;
        int width, height;
        std::map<int, std::optional<std::pair<long long, long long>>> offsets;
    };
    mutable std::optional<Cached> cached_;
};

// Where a clone stamp copies from, across strokes. Setting a source starts
// over: the next stroke copies from the source itself. After that, aligned
// (the default) keeps the distance the first stroke set, so a stroke
// started anywhere copies the cells that distance away and strokes piece
// one copy together; not aligned, every stroke starts copying at the
// source again.
struct CloneSource {
    std::optional<std::pair<int, int>> source;
    bool aligned = true;
    // The first dab of the stroke that set the distance; empty until a
    // stroke after setting the source.
    std::optional<std::pair<int, int>> anchor;

    void setSource(int x, int y) {
        source = {x, y};
        anchor.reset();
    }
    // The brush for a stroke whose first dab is at x, y; empty without a
    // source.
    std::optional<CloneBrush> beginStroke(int x, int y);
    // Where a stroke whose first dab is at x, y starts copying from (the
    // dab plus the distance); empty without a source.
    std::optional<std::pair<long long, long long>> sampleFor(int x, int y) const;
};

} // namespace bw
