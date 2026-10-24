#pragma once
// Texture stamps (PLAN §10, "Detail"). A
// texture is many small lit forms, each with a lit top, a dark seam and,
// on some, a glint, so a surface reads as many forms: scales, foliage and
// a coin hoard; brick and mottled stone (noise) besides. It is a field over
// the whole canvas, fixed by the seed:
// the brush uncovers it where it passes, so dabs that overlap, a drag and
// a second stroke over the first all paint the one texture, with no seam
// where they meet. The cells under the brush are stamped whatever they
// held: off the ramp, transparent (unless the alpha lock is on), anything.
//
// The forms are ellipses in cells, rows counted two columns tall on
// screen, squashed (a lump's height on screen over its width), centred on
// a jittered grid whose spacing sets how many cover a stretch: density is
// lumps()' one, the lumps' area over the area, so 1.6 overlaps them and 0.5
// leaves gaps. Where lumps overlap the one lower down (then further right)
// is on top, as lumps() draws them. The seam is the ramp's first colour
// (rung 0, black on every named ramp): the gaps between lumps, which read
// as shadowed depth; a lump's cells beside one of a lump on top of it (left,
// right, above or below), its shadow; and its rim (past 0.75 of the way
// out) on the side away from the light, as lumps()' rim. Inside the seam a
// lump is a ladder: its peak, two cells side by side at the lit point
// (offset toward the light, 0.45 of the way to the rim, where lumps() puts
// its glint), and one rung darker for each cell further away (eight ways),
// down to the ramp's second colour. Two lumps' lit cells never meet along a
// row or a column, so the ladder has no skips: neighbouring cells are a
// rung apart or one is black. A cell lighter than all its neighbours on the
// canvas (three or more), black and the top rung aside, takes its lightest
// neighbour's rung; then a cell darker than all of them takes its darkest
// neighbour's, so the texture leaves no speck (lint::checkLadder) where
// lumps hide one another's peaks or a mirror's fold reflects a slope. Packed lumps are mostly shadow:
// about 55% of the cells are seam at density 1, 70% at 2.5.
//
// Kinds (the radius in columns, jittered 0.75 to 1.2 a lump, as lumps()):
//  - Scales: r 5, squash 0.7, every peak the top rung.
//  - Foliage: r 4, squash 0.9, leaves lit one by one: a leaf peaks at the
//    top rung, one under it or two under it.
//  - Hoard: r 6, squash 0.6, coins peaking two rungs under the top, about
//    one in three with a glint: peaking at the top rung.
// A cell is twice as tall as wide, so these radii are about twice lumps()'
// in pixels: a lump needs a few rows for a lit top over its shadow.
//
// Two kinds aren't lumps:
//  - Brick: running-bond courses of bricks on black mortar (rung 0, the
//    seam): a mortar row over each course, a mortar column between bricks,
//    every other course shifted half a brick. Density sets the brick's size
//    (its rows are 3.6 over the root of the density, at least two, and it
//    is 5.2 columns long a row: 16 x 3 at 1.6, 10 x 2 at 2.5, 36 x 7 at
//    0.3). The seed moves each joint a column either way or not, and each
//    brick's face a rung either way or not, about 0.62 of the way up. A
//    brick is a plane: its two columns and its row on the light's side a
//    rung lighter, the lit corner two, and its far edges falling a rung a
//    cell to shadow (three columns, two rows; fewer on a small brick). No
//    two bricks touch, so neighbours in a brick are a rung apart and no
//    brick cell is lighter than both its neighbours along a row; where a
//    fold leaves one darker than all its neighbours, the speck rule raises
//    it.
//  - Noise: mottled stone, a smooth seeded field (value noise at three
//    scales, a drift, a mottle and a grain, and the mottle's relief lit
//    from the light) about the middle of the ramp, from rung 2 to the top,
//    rounded to rungs. Density sets its grain: the mottle's lattice is 24
//    columns over the density apart, the drift's three times that, the
//    grain's 0.4 of it. The field climbs at most 0.95 of a rung between
//    neighbouring blocks of two cells side by side (square on screen),
//    which caps its contrast on a fine grain, so neighbouring cells are at
//    most a rung apart; and it is read a block at a time, so every cell has
//    a neighbour of its own rung and none is a speck (a block's left cell
//    with no right one beside it, at an odd canvas's last column or, under a
//    fold, its centre column, joins the block to its left). There is no seam.
//
// With iCE off a rung that needs a bright background can't be written, so
// the texture's top is the highest rung with every rung under it writable
// (fire stops at light red). The light is BrushSettings::lightDirection, as
// the rim light's. With a mirror symmetry on, the field is folded about
// the mirror axes (the canvas's centre column, row or both): every cell
// reads the field at its fold in the left (top) half, whichever image
// paints it, so a stroke and its images paint the exact mirror image, its
// light mirrored too, even where they overlap, and strokes with the same
// mirror agree where they overlap. Off the fold's own column and row the
// left (top) half is the plain field; the speck rule judges the folded
// picture. Radial images can't be exact on the cell grid, so a radial image
// reads the plain field at its own cells: overlapping images agree, but the
// forms aren't turned with them.
// It ignores strength; pressure scales the size. Like the rim light, it is
// an accent: where the stamp meets what was on the canvas the lint judges
// the join.

#include "blockwright/brush.h"
#include "blockwright/ramp.h"

#include <cstdint>
#include <optional>

namespace bw {

// New kinds go on the end: a kind's number is what Tool Options holds.
enum class TextureKind { Scales, Foliage, Hoard, Brick, Noise };

constexpr double kMinTextureDensity = 0.3, kMaxTextureDensity = 2.5;
constexpr int kMaxTextureSeed = 99999;

struct TextureOptions {
    TextureKind kind = TextureKind::Scales;
    double density = 1.6;  // kMinTextureDensity to kMaxTextureDensity
    int seed = 1;          // 0 to kMaxTextureSeed

    bool operator==(const TextureOptions&) const = default;
};

// The texture's rung at cell (x, y) of a canvas `width` x `height` (0: no
// edge that way): rung 0 for a seam, a gap or mortar. Density and seed are taken
// within their ranges; `ice` as above. `foldX` (`foldY`) folds the field
// across the canvas's centre column (row), as a mirror symmetry paints it;
// a fold needs that edge (no width, no fold across).
int textureRung(const Ramp& ramp, const TextureOptions& options, Compass light, int x, int y, bool ice = true,
                int width = 0, int height = 0, bool foldX = false, bool foldY = false);

class TextureBrush : public Brush {
public:
    TextureBrush(Ramp ramp, TextureOptions options, Compass light);

    std::string_view name() const override { return "texture"; }
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

private:
    Ramp ramp_;
    TextureOptions options_;
    Compass light_;
};

} // namespace bw
