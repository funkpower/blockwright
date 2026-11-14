// Writes the sample projects (samples/<name>/) so every layer of
// them can be made again the same way:
//
//   bw-make-samples <samples dir>
//
// It writes each sample as a directory project under <samples dir>,
// replacing what is there. The fog layer's cells are its generator's first
// run (python3 fog.py), as Add Generator makes them; the generator piece's
// are its generators' runs (sh and awk) with the parameters it sets.
//
// Every byte it writes is the same on any machine: nothing it writes is
// compressed or encoded by a library whose output can change with its
// version. The layers' cells go in raw zstd blocks (CellsFrame::Raw), and
// the reference sketch is maker/sketch.png, copied as it is.
#include "blockwright/composite.h"
#include "blockwright/generator.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/palette.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/sauce.h"
#include "blockwright/text.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <csignal>

using namespace bw;
namespace fs = std::filesystem;

namespace {

constexpr int kW = 80, kH = 40;

Color ix(std::uint8_t c) { return Color::indexed(c); }

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = ix(fg);
    c.bg = ix(bg);
    return c;
}

void apply(Canvas& c, const CellEdit& e) {
    for (const CellChange& ch : e.changes)
        c.at(ch.x, ch.y) = ch.after;
}

// A small fixed generator, so the pieces come out the same everywhere.
struct Lcg {
    std::uint32_t s;
    std::uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int below(int n) { return int(next() % std::uint32_t(n)); }
};

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error(p.string() + ": can't be read");
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::map<std::string, std::string> dirFiles(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file())
            out[e.path().filename().string()] = readFile(e.path());
    return out;
}

Sauce sauceFor(const std::string& title) {
    Sauce s;
    s.title = title;
    s.author = "funkpower";
    s.date = "20261102";
    return s;
}

Layer& layerNamed(LayerStack& st, const std::string& name) {
    for (std::size_t i = 0; i < st.size(); ++i)
        if (st[i].props.name == name)
            return st[i];
    throw std::runtime_error("no layer " + name);
}

// ---- the Layers showcase ----------------------------------------------------

const Ramp& ramp(const char* name) {
    static std::map<std::string, Ramp> cache;
    auto it = cache.find(name);
    if (it == cache.end())
        it = cache.emplace(name, *findRamp(name)).first;
    return it->second;
}

// A black cell the IRC export keeps: a margin, or the last column of a black
// row beside a row that reaches the edge.
Cell blackBlock() { return cell(U'█', Black, Black); }

// The night sky, a band a rung: black at the top, where its spaces carry
// the star colours the stars layer takes (chars only), city glow at the
// horizon.
Layer nightSky() {
    Layer l = makeLayer(LayerKind::Cells, kW, 24, "sky");
    Canvas& c = l.cells;
    // rows 0-6 black, then a rung a row up the sunset ramp; the solids wider
    const int rungs[24] = {0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 4, 4, 4, 4, 5, 6, 7, 8, 8, 9, 10, 11, 12};
    Lcg r{11};
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            if (rungs[y] > 0) {
                c.at(x, y) = rungCell(ramp("sunset"), rungs[y]);
                continue;
            }
            const int pick = r.below(5);
            c.at(x, y) = x == kW - 1 ? blackBlock() : cell(U' ', pick == 0 ? White : pick < 3 ? Grey : DarkGrey, Black);
        }
    l.props.locked = true;
    return l;
}

Layer daySky() {
    Layer l = makeLayer(LayerKind::Cells, kW, 24, "day sky");
    Canvas& c = l.cells;
    // the sea ramp: its shades sit on a dark colour, so they need no iCE
    const int rungs[24] = {4, 4, 4, 4, 5, 6, 7, 8, 8, 8, 8, 8, 8, 9, 10, 11, 12, 12, 12, 12, 12, 12, 12, 12};
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            c.at(x, y) = rungCell(ramp("sea"), rungs[y]);
    l.props.visible = false;
    return l;
}

// Star glyphs over the black of the sky; chars only, so each takes the
// colours of the sky cell under it.
Layer stars(const Canvas& sky) {
    Layer l = makeLayer(LayerKind::Cells, kW, 7, "stars");
    Lcg r{5};
    for (int y = 0; y < l.cells.height(); ++y) {
        int placed = 0;
        for (int tries = 0; tries < 400 && placed < 4; ++tries) {
            const int x = r.below(kW - 1);
            const Cell& under = sky.at(x, y);
            if (under.fg.paletteIndex() == DarkGrey || (x > 60 && x < 74))
                continue;
            if (!l.cells.at(x, y).transparent || (x > 0 && !l.cells.at(x - 1, y).transparent) ||
                !l.cells.at(x + 1, y).transparent)
                continue;
            const int pick = r.below(8);
            l.cells.at(x, y) = cell(pick == 0 ? U'*' : pick == 1 ? U'+' : U'·', Grey, Black);
            ++placed;
        }
    }
    l.props.mode = LayerMode::CharsOnly;
    return l;
}

// The moon, in half-block pixels, behind the towers on the right.
Layer moon() {
    const int cw = 10, ch = 5;
    Layer l = makeLayer(LayerKind::Pixels, cw, ch, "moon", 63, 1);
    const double cx = 4.5, cy = 4.5, rad = 4.8;
    for (int py = 0; py < ch * 2; ++py)
        for (int px = 0; px < cw; ++px) {
            const double dx = px - cx, dy = (py - cy);
            if (dx * dx + dy * dy > rad * rad)
                continue;
            // a crater, a cluster of darker pixels
            const bool crater = (px >= 4 && px <= 6 && py >= 2 && py <= 3) || (px >= 2 && px <= 3 && py >= 6 && py <= 7);
            Cell& c = l.cells.at(px, py / 2);
            c = setLayerPixel(c, py % 2 == 1, crater ? Brown : Yellow);
        }
    return l;
}

// A thin cloud across the moon, in pixels over the moon's pixels. With iCE
// off a cell shows one bright colour, and the upper layer keeps its own: the
// cloud's grey stays and the moon's yellow under it shows as its dark
// counterpart, brown. Raise the moon above it and the moon keeps its yellow.
Layer cloud() {
    const int cw = 22, ch = 2;
    Layer l = makeLayer(LayerKind::Pixels, cw, ch, "cloud", 56, 3);
    // pixel rows 1 and 2 of the layer: the bottom of its first row of cells
    // and the top of its second, each half of a cell
    for (int px = 2; px <= 15; ++px)
        l.cells.at(px, 0) = setLayerPixel(l.cells.at(px, 0), true, DarkGrey);
    for (int px = 7; px <= 20; ++px)
        l.cells.at(px, 1) = setLayerPixel(l.cells.at(px, 1), false, DarkGrey);
    return l;
}

struct Tower {
    int x, w, top;
};
// in the skyline layer's cells
const Tower kTowers[] = {{0, 5, 6},  {5, 4, 4},  {9, 6, 1},  {15, 3, 5},  {18, 7, 3},  {25, 4, 0},
                         {29, 5, 4}, {34, 3, 6}, {37, 6, 2}, {43, 4, 5},  {47, 3, 7},  {56, 6, 4},
                         {62, 8, 2}, {70, 4, 5}};

// The city: towers on a strip smaller than the canvas, alpha-locked, so the
// glow's shading (lit from the horizon behind) stays inside the towers.
Layer skyline() {
    const int w = 74, h = 11;
    Layer l = makeLayer(LayerKind::Cells, w, h, "skyline", 3, 12);
    for (const Tower& t : kTowers)
        for (int y = t.top; y < h; ++y)
            for (int x = t.x; x < t.x + t.w && x < w; ++x)
                l.cells.at(x, y) = blackBlock();
    l.props.alphaLocked = true;
    // the rim each tower's roof catches: two rungs of steel over black
    for (const Tower& t : kTowers)
        for (int x = t.x; x < t.x + t.w; ++x) {
            for (int y = t.top, k = 0; y < h && k < 4; ++y, ++k) {
                Cell& c = l.cells.at(x, y);
                if (!c.transparent)
                    c = rungCell(ramp("steel"), 4 - k);
            }
        }
    // lit windows: a glyph, not a pixel, so a window isn't a speck
    Lcg r{3};
    for (const Tower& t : kTowers)
        for (int y = t.top + 4; y < h; y += 2)
            for (int x = t.x + 1; x < t.x + t.w - 1; x += 3) {
                Cell& c = l.cells.at(x, y);
                if (r.below(3) == 0)
                    continue;
                c = cell(U'■', r.below(3) == 0 ? Yellow : Brown, Black);
            }
    return l;
}

// The stone quay along the shore, wider than the canvas at both ends.
Layer quay() {
    const int w = kW + 6, h = 2;
    Layer l = makeLayer(LayerKind::Cells, w, h, "quay", -3, 22);
    for (int x = 0; x < w; ++x) {
        l.cells.at(x, 0) = cell(U'▄', DarkGrey, Black);
        l.cells.at(x, 1) = rungCell(ramp("steel"), 3);
    }
    return l;
}

Layer water() {
    Layer l = makeLayer(LayerKind::Cells, kW, 16, "water", 0, 24);
    Canvas& c = l.cells;
    const int rungs[16] = {4, 4, 4, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Lcg r{9};
    for (int y = 0; y < c.height(); ++y) {
        for (int x = 0; x < c.width(); ++x)
            c.at(x, y) = rungs[y] == 0 ? blackBlock() : rungCell(ramp("sea"), rungs[y]);
        // ripples: dashes a rung up, two or more cells apart
        const int up = rungs[y] == 4 && rungs[y + 1] == 4 ? 5 : rungs[y] == 0 ? 1 : -1;
        if (up < 0)
            continue;
        // on the dark water, fewer and further apart, every other row
        const bool dark = rungs[y] == 0;
        if (dark && y % 2 == 0)
            continue;
        for (int x = r.below(6); x + 2 <= kW;) {
            const int len = std::min(2 + r.below(dark ? 3 : 4), kW - x);
            for (int i = 0; i < len; ++i)
                c.at(x + i, y) = rungCell(ramp("sea"), up);
            x += len + (dark ? 8 + r.below(12) : 3 + r.below(6));
        }
    }
    return l;
}

// The moon's path on the water, colours only: the water's own ripple
// glyphs in moonlight.
Layer reflection(const Canvas& waterCells) {
    Layer l = makeLayer(LayerKind::Cells, kW, 16, "reflection", 0, 24);
    for (int y = 4; y < 16; ++y) {
        const int half = 2 + (y - 4) / 2;
        for (int x = 67 - half; x <= 67 + half; ++x) {
            const Cell& w = waterCells.at(x, y);
            if (w.ch == U'█' || w.ch == U' ' || w.bg.paletteIndex() != Black)
                continue;
            l.cells.at(x, y) = cell(U' ', Yellow, Black);
        }
    }
    l.props.mode = LayerMode::ColoursOnly;
    return l;
}

project::Project showcase(const fs::path& makerDir) {
    project::Project p;
    p.layers = LayerStack(kW, kH);
    p.ice = false;
    p.sauce = sauceFor("Harbour at night");

    Layer sky = nightSky();
    const Canvas skyCells = sky.cells;
    p.layers.add(std::move(sky));
    p.layers.add(daySky());
    p.layers.add(stars(skyCells));
    p.layers.add(moon());
    p.layers.add(cloud());
    p.layers.add(skyline());
    p.layers.add(quay());
    Layer sea = water();
    const Canvas seaCells = sea.cells;
    p.layers.add(std::move(sea));
    p.layers.add(reflection(seaCells));

    // the first round: the scene without its fog and title
    project::takeSnapshot(p);

    generator::add(p, dirFiles(makerDir / "fog"), "fog");

    TextSource src;
    src.text = U"HARBOUR";
    src.colour = Yellow;
    Layer title = makeTextLayer(src, "title", 12, 31);
    LayerProperties props = title.props;
    props.style = library::readTextStyleEntry(makerDir / "title.style.json").style;
    applyLayerProperties(title, props, {}, false);
    Layer baked = bake(title, false);
    baked.props.id = 0;
    baked.props.name = "title (baked)";
    baked.props.visible = false;
    p.layers.add(std::move(baked));
    p.layers.add(std::move(title));
    project::takeSnapshot(p);

    Layer ref = makeLayer(LayerKind::Reference, 0, 0, "reference sketch");
    ReferenceImage img;
    // the composition as drawn before the cells (horizon, towers, moon),
    // copied byte for byte: a PNG encoded here would come out in other
    // bytes from another zlib
    img.bytes = std::make_shared<const std::string>(readFile(makerDir / "sketch.png"));
    img.x = 0, img.y = 0, img.width = kW, img.height = kH;
    img.opacity = 40;
    ref.props.reference = img;
    ref.props.visible = false;
    p.layers.add(std::move(ref));

    p.notes = readFile(makerDir / "notes" / "01-layers-showcase.md");
    project::takeSnapshot(p);
    return p;
}

// ---- DDOS KING --------------------------------------------------------------

// A finished piece. Its layers are in maker/ddos-king/ (PLAN §12); the maker
// takes them as they are.
constexpr int kKingW = 80, kKingH = 64;

// Hidden: black blocks over the frame's rows and its last column, which the
// IRC export keeps, for the variants that hide the border.
Layer kingMargins() {
    Layer l = makeLayer(LayerKind::Cells, kKingW, kKingH, "margins");
    for (int y = 0; y < kKingH; ++y)
        for (int x = 0; x < kKingW; ++x)
            if (y < 2 || y >= kKingH - 2 || x == kKingW - 1)
                l.cells.at(x, y) = blackBlock();
    l.props.visible = false;
    return l;
}

// Hidden: half a cell more cable on top of four cable runs, for the variant
// that hides the lightning. With it gone, the blue cell over each run's top
// and the yellow cell of the run read to the lint as a ramp that skips rungs;
// a half block of the cable's own colour over the blue breaks that. The
// colour is the top half of the run's cell as the layers under the lightning
// show it, whichever of them holds the cable there (the sceptre's at 12,36,
// the cables' elsewhere).
Layer cableTips(const LayerStack& layers, bool ice) {
    std::size_t lightning = 0;
    while (layers[lightning].props.name != "lightning")
        if (++lightning == layers.size())
            throw std::runtime_error("no layer lightning");
    Layer l = makeLayer(LayerKind::Cells, kKingW, kKingH, "cable tips");
    for (const auto& [x, y] : {std::pair{12, 35}, {9, 41}, {7, 43}, {6, 44}}) {
        const auto run = halfBlocks(compositeCellBelow(layers, lightning, x, y + 1, {ice, Cell{}}), ice);
        if (!run)
            throw std::runtime_error("no cable under a cable tip");
        l.cells.at(x, y) = cell(U'▄', run->top, Blue);
    }
    l.props.visible = false;
    return l;
}

project::Project ddosKing(const fs::path& makerDir) {
    project::Project piece = project::read(makerDir / "ddos-king");
    const auto take = [&](const char* name) {
        Layer l = layerNamed(piece.layers, name);
        l.props.id = 0;
        return l;
    };
    project::Project p;
    p.layers = LayerStack(kKingW, kKingH);
    p.ice = piece.ice;  // the light cyan glow and the gold are backgrounds
    p.ircBudget = piece.ircBudget;
    p.sauce = sauceFor("DDOS KING");

    // the first snapshot: the throne and its cables on the locked backdrop
    Layer backdrop = take("backdrop");
    backdrop.props.locked = true;
    p.layers.add(std::move(backdrop));
    p.layers.add(kingMargins());
    p.layers.add(take("throne"));
    p.layers.add(take("cables"));
    p.layers.add(cableTips(piece.layers, piece.ice));
    project::takeSnapshot(p);

    // the second: the figure
    for (const char* name : {"king", "sceptre", "head"})
        p.layers.add(take(name));
    project::takeSnapshot(p);

    // the third: the lightning, the lettering and the frame
    for (const char* name : {"lightning", "finials", "packets", "title", "border", "signature"})
        p.layers.add(take(name));
    p.notes = readFile(makerDir / "notes" / "02-ddos-king.md");
    project::takeSnapshot(p);
    return p;
}

// ---- the battlestation ------------------------------------------------------

// A finished piece. Its layers are in maker/battlestation/; the maker takes
// them as they are, except that the blue the screen throws on the wall, the
// desk and the rack's near side moves off them onto a layer of its own.
constexpr int kStationW = 80, kStationH = 40;

// The layer with the screen's light taken off it: on each cell `darkAt`
// gives a colour for, its blues and light cyan turned that colour, the one
// the surface is without the light. The glyphs stay, so the glow over them
// puts back the cells they were.
Layer unlitLayer(Layer l, const std::function<std::optional<std::uint8_t>(int, int)>& darkAt) {
    for (int y = 0; y < l.cells.height(); ++y)
        for (int x = 0; x < l.cells.width(); ++x)
            if (Cell& c = l.cells.at(x, y); !c.transparent)
                if (const auto dark = darkAt(x, y)) {
                    bool changed = false;
                    for (Color* col : {&c.fg, &c.bg})
                        if (const auto i = col->paletteIndex(); i == Blue || i == LightBlue || i == LightCyan) {
                            *col = ix(*dark);
                            changed = true;
                        }
                    // a space left on black takes the grey a .ans gives it,
                    // so a snapshot reads back the cell it was written from
                    if (changed && c.ch == U' ' && c.bg.paletteIndex() == Black)
                        c.fg = ix(Grey);
                }
    return l;
}

// The wall and the desk are black without the light.
std::optional<std::uint8_t> black(int, int) { return Black; }

// The rack's side and top edge toward the screen: its own dark grey, black
// below row 29, where the light was already fading into it.
std::optional<std::uint8_t> rackNearSide(int x, int y) {
    if ((y == 11 && x >= 63 && x <= 71) || (y >= 12 && x >= 63 && x <= 65))
        return y < 30 ? DarkGrey : Black;
    return std::nullopt;
}

// Colours only: the colours of every cell the screen lights, as the layers
// under it have them lit, so with it shown the piece reads as the layers
// do. `lit` is those layers with the light on, `dark` with it off.
Layer glow(const LayerStack& lit, const LayerStack& dark, bool ice) {
    const Canvas on = composite(lit, {ice, Cell{}}), off = composite(dark, {ice, Cell{}});
    Layer l = makeLayer(LayerKind::Cells, kStationW, kStationH, "glow");
    l.props.mode = LayerMode::ColoursOnly;
    for (int y = 0; y < kStationH; ++y)
        for (int x = 0; x < kStationW; ++x)
            if (on.at(x, y) != off.at(x, y)) {
                Cell c = on.at(x, y);
                c.dithered = false;
                l.cells.at(x, y) = c;
            }
    return l;
}

// Hidden: a black block in the last row's first cell, which the IRC export
// keeps, for the variant that shows the screen alone: without it the rows
// under the monitor are blank and IRC trims them.
Layer stationMargins() {
    Layer l = makeLayer(LayerKind::Cells, kStationW, kStationH, "margins");
    l.cells.at(0, kStationH - 1) = blackBlock();
    l.props.visible = false;
    return l;
}

project::Project battlestation(const fs::path& makerDir) {
    project::Project piece = project::read(makerDir / "battlestation");
    const auto take = [&](const char* name) {
        Layer l = layerNamed(piece.layers, name);
        l.props.id = 0;
        return l;
    };
    project::Project p;
    p.layers = LayerStack(kStationW, kStationH);
    p.ice = piece.ice;
    p.ircBudget = piece.ircBudget;
    p.sauce = sauceFor("Battlestation");

    // the first snapshot: the room in the dark and the screen, its panes
    // held to their own cells
    Layer wall = unlitLayer(take("wall glow"), black);
    wall.props.name = "wall";
    wall.props.locked = true;
    p.layers.add(std::move(wall));
    p.layers.add(stationMargins());
    p.layers.add(unlitLayer(take("desk"), black));
    p.layers.add(take("monitor frame"));
    Layer screen = take("screen");
    screen.props.alphaLocked = true;
    p.layers.add(std::move(screen));
    project::takeSnapshot(p);

    // the second: what stands on the desk and beside it
    for (const char* name : {"keyboard", "mug and mouse"})
        p.layers.add(take(name));
    p.layers.add(unlitLayer(take("rack"), rackNearSide));
    project::takeSnapshot(p);

    // the third: the light the screen throws, and the chair in front of it
    LayerStack lit(kStationW, kStationH);
    for (const char* name : {"wall glow", "desk", "monitor frame", "screen", "keyboard", "mug and mouse", "rack"})
        lit.add(take(name));
    p.layers.add(glow(lit, p.layers, p.ice));
    for (const char* name : {"chair", "signature"})
        p.layers.add(take(name));
    p.notes = readFile(makerDir / "notes" / "07-battlestation.md");
    project::takeSnapshot(p);
    return p;
}

// ---- this is your brain on IRC ----------------------------------------------

// One egg in a pan. The lettering, the pan's outline, the egg white, the
// yolk's outline, the sizzle and the smoke are in maker/brain-on-irc/; the
// pan and the yolk are filled flat and shaded with the Form brush as the
// app paints with it, and the # and the highlight are pixels over the yolk.
constexpr int kBrainW = 80, kBrainH = 32;

// Hidden: a black block in the first row's first cell, which the IRC export
// keeps, for the variant without the caption: without it the rows above
// the smoke are blank and IRC trims them.
Layer brainMargins() {
    Layer l = makeLayer(LayerKind::Cells, kBrainW, kBrainH, "margins");
    l.cells.at(0, 0) = blackBlock();
    l.props.visible = false;
    return l;
}

// `outline`'s opaque cells as one flat colour, alpha-locked.
Layer flatShape(Layer outline, std::uint8_t colour) {
    for (int y = 0; y < outline.cells.height(); ++y)
        for (int x = 0; x < outline.cells.width(); ++x)
            if (!outline.cells.at(x, y).transparent)
                outline.cells.at(x, y) = cell(U'█', colour, Black);
    outline.props.alphaLocked = true;
    return outline;
}

// A copy of `flat` named `name`, with one pass of the Form brush that dabs
// every cell row by row, left to right, at size 1: Select All, the given
// shape, ramp and strength, the light from the north-west, Glint on and
// Form rim light off. In the app that is the notes' one drag along each row
// and back along it, then down a row.
Layer formBrushed(const Layer& flat, const char* name, const char* rampName, FormShape shape, int strengthPercent,
                  bool ice) {
    Layer l = flat;
    l.props.name = name;
    StrokeOptions o = layerStrokeOptions(l);
    o.selection = std::make_shared<const Mask>(l.cells.width(), l.cells.height(), true);
    o.ice = ice;
    BrushSettings s;
    s.lightDirection = Compass::NW;
    s.strength = strengthPercent / 100.0;
    paintRegion(l.cells, FormShadeBrush(ramp(rampName), FormOptions{shape, false, true}), s, o).apply(l.cells);
    return l;
}

// Half-block pixels at (x, pixel row), in one colour, on a pixel layer.
Layer brainPixels(const char* name, std::uint8_t colour, std::initializer_list<std::pair<int, int>> pixels) {
    Layer l = makeLayer(LayerKind::Pixels, kBrainW, kBrainH, name);
    for (const auto& [x, py] : pixels)
        l.cells.at(x, py / 2) = setLayerPixel(l.cells.at(x, py / 2), py % 2 == 1, colour);
    return l;
}

// The yellow and white halves of a cell layer, on a pixel layer of the
// same name: the rest shows whatever is under it.
Layer brainPixelsOf(const Layer& cells) {
    Layer l = makeLayer(LayerKind::Pixels, kBrainW, kBrainH, cells.props.name);
    for (int y = 0; y < kBrainH; ++y)
        for (int x = 0; x < kBrainW; ++x) {
            const Cell& c = cells.cells.at(x, y);
            const auto halves = c.transparent ? std::nullopt : halfBlocks(c);
            if (!halves)
                continue;
            for (const bool bottom : {false, true}) {
                const std::uint8_t colour = bottom ? halves->bottom : halves->top;
                if (colour == Yellow || colour == White)
                    l.cells.at(x, y) = setLayerPixel(l.cells.at(x, y), bottom, colour);
            }
        }
    return l;
}

// The # pressed into the yolk: two bars across and two strokes down,
// leaning right.
Layer brainHash() {
    return brainPixels("#", Brown,
                       {{31, 36}, {31, 37}, {34, 36}, {34, 37},                                            // row 18
                        {29, 38}, {30, 38}, {31, 38}, {32, 38}, {33, 38}, {34, 38}, {35, 38}, {30, 39}, {34, 39},  // 19
                        {29, 40}, {30, 40}, {31, 40}, {32, 40}, {33, 40}, {34, 40}, {35, 40}, {30, 41}, {33, 41}});
}

// The light on the yolk from the upper left: a crescent along the top left,
// two pixels at the lower left and one at the upper right, none of them on
// the #.
Layer brainHighlight() {
    return brainPixels("highlight", White,
                       {{26, 37}, {27, 36}, {27, 37}, {28, 36}, {28, 37}, {29, 36}, {30, 36}, {38, 36},
                        {26, 38}, {27, 38}, {25, 40}, {26, 40}});
}

project::Project brainOnIrc(const fs::path& makerDir) {
    project::Project piece = project::read(makerDir / "brain-on-irc");
    const auto take = [&](const char* name) {
        Layer l = layerNamed(piece.layers, name);
        l.props.id = 0;
        return l;
    };
    project::Project p;
    p.layers = LayerStack(kBrainW, kBrainH);
    p.ice = piece.ice;
    p.ircBudget = piece.ircBudget;
    p.sauce = sauceFor("This Is Your Brain on IRC");

    // the first snapshot: the lettering, the pan, the egg white and the
    // yolk, the pan and the yolk flat and alpha-locked
    Layer background = take("background");
    background.props.locked = true;
    p.layers.add(std::move(background));
    p.layers.add(brainMargins());
    for (const char* name : {"caption", "tagline", "signature"})
        p.layers.add(take(name));
    Layer pan = flatShape(take("pan"), DarkGrey);
    pan.props.name = "pan (flat)";
    p.layers.add(pan);
    p.layers.add(take("egg white"));
    Layer yolk = flatShape(take("yolk"), Yellow);
    yolk.props.name = "yolk (flat)";
    p.layers.add(yolk);
    project::takeSnapshot(p);

    // the second: the Form brush over a copy of each, a cylinder of steel
    // and a dome of skin, the flat ones hidden under them
    layerNamed(p.layers, "pan (flat)").props.visible = false;
    layerNamed(p.layers, "yolk (flat)").props.visible = false;
    const auto above = [&](const char* name) { return *p.layers.indexOf(layerNamed(p.layers, name).props.id) + 1; };
    p.layers.add(formBrushed(pan, "pan", "steel", FormShape::Cylinder, 100, p.ice), above("pan (flat)"));
    p.layers.add(formBrushed(yolk, "yolk", "skin", FormShape::Dome, 65, p.ice), above("yolk (flat)"));
    project::takeSnapshot(p);

    // the third: the #, the highlight, the heat and two touches
    p.layers.add(brainHash());
    p.layers.add(brainHighlight());
    // the sizzle's arcs step diagonally a pixel at a time, which the lint
    // counts as noise once the caption is gone; three pixels join three of
    // the steps
    Layer sizzle = brainPixelsOf(take("sizzle"));
    for (const auto& [x, py] : {std::pair{13, 38}, std::pair{13, 44}, std::pair{56, 43}})
        sizzle.cells.at(x, py / 2) = setLayerPixel(sizzle.cells.at(x, py / 2), py % 2 == 1, Yellow);
    p.layers.add(std::move(sizzle));
    p.layers.add(take("smoke"));
    // two cells the lint calls specks beside the sizzle and the egg white,
    // each set to the shade of a neighbour
    layerNamed(p.layers, "pan").cells.at(11, 22) = layerNamed(p.layers, "pan").cells.at(10, 22);
    layerNamed(p.layers, "yolk").cells.at(24, 22) = layerNamed(p.layers, "yolk").cells.at(24, 21);
    p.notes = readFile(makerDir / "notes" / "08-brain-on-irc.md");
    project::takeSnapshot(p);
    return p;
}

// ---- the still life ---------------------------------------------------------

constexpr int kLifeW = 80, kLifeH = 30;
// the row the table's back edge is on
constexpr int kTableTop = 17;

// A mask of the canvas from a test on each cell.
template <class F> Mask maskOf(F inside) {
    Mask m(kLifeW, kLifeH);
    for (int y = 0; y < kLifeH; ++y)
        for (int x = 0; x < kLifeW; ++x)
            if (inside(x, y))
                m.set(x, y);
    return m;
}

// An ellipse of cells, its centre and radii in cells (a cell is twice as tall
// as wide, so rx = 2 ry looks round).
bool inEllipse(double cx, double cy, double rx, double ry, int x, int y) {
    const double dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
    return dx * dx + dy * dy <= 1.0;
}

const Mask& sphereShape() {
    static const Mask m = maskOf([](int x, int y) { return inEllipse(62, 20, 11, 5.5, x, y); });
    return m;
}
// a pillar on a round foot
const Mask& cylinderShape() {
    static const Mask m = maskOf([](int x, int y) {
        return (x >= 9 && x <= 22 && y >= 5 && y <= 20) || inEllipse(16, 20.5, 7, 1.5, x, y);
    });
    return m;
}
// the box: its front face, its top leaning back to the right, and its
// right side
bool boxFront(int x, int y) { return x >= 27 && x <= 44 && y >= 13 && y <= 23; }
bool boxTop(int x, int y) {
    if (y < 10 || y > 12)
        return false;
    const int k = 13 - y;  // 1 to 3 rows back
    return x >= 27 + 2 * k && x <= 44 + 2 * k;
}
bool boxSide(int x, int y) {
    if (x < 45 || x > 50)
        return false;
    const int k = (x - 45) / 2 + 1;  // 1 to 3 columns back
    return y >= 13 - k && y <= 23 - k;
}

// Keeps a field of rungs a ladder: no cell more than a rung below a
// neighbour on the field (-1 is off it), lifting the darker one, so an
// edge between two tones steps a rung at a time.
void keepLadder(std::vector<int>& rung, int w, int h) {
    for (bool changed = true; changed;) {
        changed = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int& r = rung[std::size_t(y * w + x)];
                if (r < 0)
                    continue;
                for (const auto& [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    const int n = rung[std::size_t(ny * w + nx)];
                    if (r < n - 1) {
                        r = n - 1;
                        changed = true;
                    }
                }
            }
    }
}

// A layer just big enough for the rungs set in a canvas-sized field, placed
// by its offset.
Layer rungLayer(const std::string& name, const std::vector<int>& rung, const Ramp& r) {
    int x0 = kLifeW, y0 = kLifeH, x1 = -1, y1 = -1;
    for (int y = 0; y < kLifeH; ++y)
        for (int x = 0; x < kLifeW; ++x)
            if (rung[std::size_t(y * kLifeW + x)] >= 0)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    Layer l = makeLayer(LayerKind::Cells, x1 - x0 + 1, y1 - y0 + 1, name, x0, y0);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (const int t = rung[std::size_t(y * kLifeW + x)]; t >= 0)
                l.cells.at(x - x0, y - y0) = rungCell(r, t);
    return l;
}

std::vector<int> maskRungs(const Mask& m, int rung) {
    std::vector<int> out(std::size_t(kLifeW * kLifeH), -1);
    for (int y = 0; y < kLifeH; ++y)
        for (int x = 0; x < kLifeW; ++x)
            if (m.contains(x, y))
                out[std::size_t(y * kLifeW + x)] = rung;
    return out;
}

// How many cells in from the east edge of a shape each cell is, stepping
// through the shape a cell at a time (a cell whose east neighbour is
// outside it is 0), so the band it gives falls a rung a cell every way.
std::vector<int> rimDistances(const Mask& shape) {
    std::vector<int> d(std::size_t(kLifeW * kLifeH), 1 << 20);
    std::deque<std::pair<int, int>> todo;
    for (int cy = 0; cy < kLifeH; ++cy)
        for (int cx = 0; cx < kLifeW; ++cx)
            if (shape.contains(cx, cy) && (cx + 1 >= kLifeW || !shape.contains(cx + 1, cy))) {
                d[std::size_t(cy * kLifeW + cx)] = 0;
                todo.emplace_back(cx, cy);
            }
    while (!todo.empty()) {
        const auto [cx, cy] = todo.front();
        todo.pop_front();
        for (const auto& [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= kLifeW || ny >= kLifeH || !shape.contains(nx, ny))
                continue;
            int& n = d[std::size_t(ny * kLifeW + nx)];
            if (n > d[std::size_t(cy * kLifeW + cx)] + 1) {
                n = d[std::size_t(cy * kLifeW + cx)] + 1;
                todo.emplace_back(nx, ny);
            }
        }
    }
    return d;
}

// The box's tones on the wood ramp: solid faces, the top yellow where the
// light falls, the front brown at a slant to it and the side red, facing
// away. Where two faces meet, the front steps up toward the top a rung a
// row, and the lighter face steps down toward the side a rung a column, so
// the edges stay a ladder.
std::vector<int> boxRungs() {
    std::vector<int> out(std::size_t(kLifeW * kLifeH), -1);
    const auto at = [&](int x, int y) -> int& { return out[std::size_t(y * kLifeW + x)]; };
    for (int y = 0; y < kLifeH; ++y)
        for (int x = 0; x < kLifeW; ++x)
            at(x, y) = boxTop(x, y) ? 8 : boxFront(x, y) ? 4 : boxSide(x, y) ? 0 : -1;
    for (int y = 0; y < kLifeH; ++y)
        for (int x = 0; x < kLifeW; ++x)
            if (boxFront(x, y))
                at(x, y) = std::max(at(x, y), 8 - (y - 12));
    // lighter cells come down to a rung over their darkest neighbour
    for (bool changed = true; changed;) {
        changed = false;
        for (int y = 1; y < kLifeH - 1; ++y)
            for (int x = 1; x < kLifeW - 1; ++x) {
                int& r = at(x, y);
                if (r < 0)
                    continue;
                for (const auto& [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    const int n = at(x + dx, y + dy);
                    if (n >= 0 && r > n + 1) {
                        r = n + 1;
                        changed = true;
                    }
                }
            }
    }
    return out;
}

// The back wall, a flat dark grey, locked: dark enough to set the lit
// sides off, light enough that the black on each shadow side still reads.
Layer wall() {
    Layer l = makeLayer(LayerKind::Cells, kLifeW, kTableTop, "wall");
    for (int y = 0; y < kTableTop; ++y)
        for (int x = 0; x < kLifeW; ++x)
            l.cells.at(x, y) = rungCell(ramp("steel"), 4);
    l.props.locked = true;
    return l;
}

// The table top on the steel ramp, lighter toward the front where the
// light falls.
int tableRung(int y) {
    static const int rungs[] = {5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8};
    return rungs[std::min<int>(y - kTableTop, int(std::size(rungs)) - 1)];
}

Layer table() {
    Layer l = makeLayer(LayerKind::Cells, kLifeW, kLifeH - kTableTop, "table", 0, kTableTop);
    for (int y = kTableTop; y < kLifeH; ++y)
        for (int x = 0; x < kLifeW; ++x)
            l.cells.at(x, y - kTableTop) = rungCell(ramp("steel"), tableRung(y));
    l.props.locked = true;
    return l;
}

// The shadows the objects cast on the table, away from the light: the
// table's own rungs taken down, deepest at each object's foot and lifting
// back to the table a rung at a time.
Layer shadows() {
    struct Fall {
        double cx, cy, rx, ry;
    };
    // an ellipse each, east of the object's foot
    const Fall falls[] = {{24, 21.0, 9, 1.6}, {50, 23.0, 9, 1.6}, {72, 25.5, 10, 1.6}};
    std::vector<int> rung(std::size_t(kLifeW * kLifeH), -1);
    std::vector<int> table(rung.size(), -1);
    for (int y = kTableTop; y < kLifeH; ++y)
        for (int x = 0; x < kLifeW; ++x) {
            double depth = 0;
            for (const Fall& f : falls) {
                const double dx = (x + 0.5 - f.cx) / f.rx, dy = (y + 0.5 - f.cy) / f.ry;
                depth = std::max(depth, 1.0 - std::sqrt(dx * dx + dy * dy));
            }
            const int down = depth <= 0 ? 0 : std::min(tableRung(y) - 1, 1 + int(depth * 4));
            rung[std::size_t(y * kLifeW + x)] = tableRung(y) - down;
            table[std::size_t(y * kLifeW + x)] = tableRung(y);
        }
    keepLadder(rung, kLifeW, kLifeH);
    for (std::size_t i = 0; i < rung.size(); ++i)
        if (rung[i] == table[i])
            rung[i] = -1;
    return rungLayer("shadows", rung, ramp("steel"));
}

project::Project stillLife(const fs::path& makerDir) {
    project::Project p;
    p.layers = LayerStack(kLifeW, kLifeH);
    p.ice = true;
    p.sauce = sauceFor("Still life");

    const Ramp& sea = ramp("sea");
    const Ramp& violet = ramp("violet");
    // the box's ramp without its black: its side is a face turned from the
    // light, not a shadow, and stays red against the dark wall
    const Ramp wood = [] {
        Ramp r = ramp("wood");
        r.colours.erase(r.colours.begin());
        return r;
    }();
    const Mask box = maskOf([](int x, int y) { return boxFront(x, y) || boxTop(x, y) || boxSide(x, y); });

    p.layers.add(wall());
    p.layers.add(table());
    // the first round: each object in flat colours on its own layer
    p.layers.add(rungLayer("cylinder", maskRungs(cylinderShape(), 4), violet));
    p.layers.add(rungLayer("box", maskRungs(box, 4), wood));
    p.layers.add(rungLayer("sphere", maskRungs(sphereShape(), 4), sea));
    project::takeSnapshot(p);

    // the second: each shaded inside its own shape, alpha-locked
    const auto form = [](const Mask& shape, const Ramp& r, FormShape kind) {
        return formRungs(r, shape, {kind, false, true}, Compass::NW);
    };
    for (auto [name, cells] : {std::pair{"cylinder", rungLayer("cylinder", form(cylinderShape(), violet,
                                                                                 FormShape::Cylinder), violet)},
                               {"box", rungLayer("box", boxRungs(), wood)},
                               {"sphere", rungLayer("sphere", form(sphereShape(), sea, FormShape::Dome), sea)}}) {
        Layer& l = layerNamed(p.layers, name);
        l.cells = cells.cells;
        l.props.alphaLocked = true;
    }
    project::takeSnapshot(p);

    // the third: the shadows between the table and the objects, and a rim
    // light from the east, on a layer of its own over them, picking out
    // each object's shadow side from the dark wall
    LayerStack st(kLifeW, kLifeH);
    for (std::size_t i = 0; i < p.layers.size(); ++i) {
        Layer l = p.layers[i];
        l.props.id = 0;
        const bool table = l.props.name == "table";
        st.add(std::move(l));
        if (table)
            st.add(shadows());
    }
    p.layers = st;
    // the rim light comes from the east, catching each object's shadow side:
    // a band along its east edge, brightest at the edge and a rung dimmer a
    // cell further in, kept where it is lighter than the shading under it
    std::vector<int> changed(std::size_t(kLifeW * kLifeH), -1);
    Canvas lit(kLifeW, kLifeH);
    for (const auto& [shape, shaded, r, peak] :
         {std::tuple{&cylinderShape(), form(cylinderShape(), violet, FormShape::Cylinder), &violet, 5},
          std::tuple{&box, boxRungs(), &wood, 4},
          std::tuple{&sphereShape(), form(sphereShape(), sea, FormShape::Dome), &sea, 7}}) {
        const std::vector<int> dist = rimDistances(*shape);
        for (int y = 0; y < kLifeH; ++y)
            for (int x = 0; x < kLifeW; ++x) {
                const int band = peak - dist[std::size_t(y * kLifeW + x)];
                const int under = shaded[std::size_t(y * kLifeW + x)];
                if (shape->contains(x, y) && band > under) {
                    changed[std::size_t(y * kLifeW + x)] = 0;
                    lit.at(x, y) = rungCell(*r, band);
                }
            }
    }
    Layer rim = rungLayer("rim light", changed, wood);
    for (int y = 0; y < rim.cells.height(); ++y)
        for (int x = 0; x < rim.cells.width(); ++x)
            if (!rim.cells.at(x, y).transparent)
                rim.cells.at(x, y) = lit.at(rim.props.x + x, rim.props.y + y);
    p.layers.add(std::move(rim));

    p.notes = readFile(makerDir / "notes" / "03-still-life.md");
    project::takeSnapshot(p);
    return p;
}

// ---- the generator piece ----------------------------------------------------

constexpr int kRidgeW = 80, kRidgeH = 34;
// the row the sky turns solid light red, where the ridges may begin
constexpr int kGlowRow = 18;
// the lake's top row and the shore's
constexpr int kLakeTop = 28, kShoreTop = 32;

// Dusk: black rows for the stars, then a rung a row up the sunset ramp to
// a solid light red glow behind the ridges. The black rows' spaces carry
// the colours the stars layer takes (chars only).
Layer duskSky() {
    Layer l = makeLayer(LayerKind::Cells, kRidgeW, kRidgeH, "sky");
    Lcg r{17};
    for (int y = 0; y < kRidgeH; ++y)
        for (int x = 0; x < kRidgeW; ++x) {
            const int rung = std::clamp(y - 2, 0, 16);
            if (rung > 0) {
                l.cells.at(x, y) = rungCell(ramp("sunset"), rung);
                continue;
            }
            const int pick = r.below(5);
            l.cells.at(x, y) =
                x == kRidgeW - 1 ? blackBlock() : cell(U' ', pick == 0 ? White : pick < 3 ? Grey : DarkGrey, Black);
        }
    l.props.locked = true;
    return l;
}

// The lake under the ridges: the sea ramp, with ripples a rung up on every
// other row.
Layer lake() {
    const int h = kShoreTop - kLakeTop;
    Layer l = makeLayer(LayerKind::Cells, kRidgeW, h, "lake", 0, kLakeTop);
    Lcg r{23};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < kRidgeW; ++x)
            l.cells.at(x, y) = rungCell(ramp("sea"), 6);
        if (y % 2 == 1)
            continue;
        for (int x = r.below(6); x + 2 <= kRidgeW;) {
            const int len = std::min(2 + r.below(4), kRidgeW - x);
            for (int i = 0; i < len; ++i)
                l.cells.at(x + i, y) = rungCell(ramp("sea"), 7);
            x += len + 4 + r.below(8);
        }
    }
    l.props.locked = true;
    return l;
}

// The near shore, a strip of grass along the bottom.
Layer shore() {
    const int h = kRidgeH - kShoreTop;
    Layer l = makeLayer(LayerKind::Cells, kRidgeW, h, "shore", 0, kShoreTop);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < kRidgeW; ++x)
            l.cells.at(x, y) = rungCell(ramp("grass"), y == 0 ? 3 : 2);
    l.props.locked = true;
    return l;
}

// Pines on the shore, in half-block pixels: a canopy widening a pixel
// every third row down, on a two-pixel trunk. Each is lit from the right:
// green on that side, and black in the shade of each bough, a pixel further
// round on each row of it. The tip, the crown's first three rows a pixel
// wide, has no bough over it to shade, so it stays green.
Layer pines() {
    struct Pine {
        int cx, height;  // its centre column and height in pixels
    };
    const Pine trees[] = {{3, 16}, {9, 20}, {15, 14}, {20, 18}, {72, 15}, {77, 20}};
    const int w = kRidgeW, ch = 12, top = kRidgeH - ch;
    Layer l = makeLayer(LayerKind::Pixels, w, ch, "pines", 0, top);
    const int ground = 2 * ch - 3;  // the pixel row the trunks stand on
    // which pine each pixel shows, -1 for none
    std::vector<int> owner(std::size_t(w * 2 * ch), -1);
    for (int i = 0; i < int(std::size(trees)); ++i) {
        const Pine& t = trees[i];
        const int crown = ground - t.height;
        for (int py = crown; py <= ground; ++py) {
            const bool trunk = py > ground - 2;
            const int half = trunk ? 0 : (py - crown) / 3;
            for (int px = t.cx - half; px <= t.cx + half; ++px) {
                if (px < 0 || px >= w)
                    continue;
                const bool lit = py - crown < 3 || px - t.cx >= (py - crown) % 3 - 1;
                Cell& c = l.cells.at(px, py / 2);
                c = setLayerPixel(c, py % 2 == 1, trunk ? Brown : lit ? Green : Black);
                owner[std::size_t(py * w + px)] = i;
            }
        }
    }
    // The pines stand left to right, back to front, so a later one hides the
    // lit side of the one behind it with its own shaded side. The boughs of
    // the one behind keep a lit edge against it, or the two shades would run
    // together into one dark mass. The edge stops two rows above the
    // lowest boughs, where its last pixels would stand alone as a speck.
    for (int py = 0; py <= ground - 4; ++py)
        for (int px = 0; px + 1 < w; ++px) {
            const int back = owner[std::size_t(py * w + px)], front = owner[std::size_t(py * w + px + 1)];
            if (back >= 0 && front > back)
                l.cells.at(px, py / 2) = setLayerPixel(l.cells.at(px, py / 2), py % 2 == 1, Green);
        }
    return l;
}

// A cabin on the shore with its window lit, in pixels: walls, a roof
// stepping in a pixel a row, a door and a window a cell each.
Layer cabin() {
    const int cw = 12, ch = 5;
    Layer l = makeLayer(LayerKind::Pixels, cw, ch, "cabin", 52, kShoreTop + 1 - ch);
    for (int py = 0; py < 2 * ch; ++py)
        for (int px = 0; px < cw; ++px) {
            std::optional<std::uint8_t> c;
            if (py < 5) {
                if (px >= 5 - py && px <= 6 + py)
                    c = Red;
            } else if (px >= 1 && px <= 10) {
                c = Brown;
                if (px >= 3 && px <= 4 && py >= 6 && py <= 7)
                    c = Yellow;
                if (px >= 7 && px <= 8 && py >= 6)
                    c = Black;
            }
            if (c)
                l.cells.at(px, py / 2) = setLayerPixel(l.cells.at(px, py / 2), py % 2 == 1, *c);
        }
    return l;
}

// A generator layer's params.json with each name set, in the form the
// generator form writes.
void setParams(project::Project& p, LayerId id, const std::vector<std::pair<std::string, generator::ParamValue>>& values) {
    const generator::Spec spec = generator::layerSpec(p, id);
    const std::string path = generator::directory(id) + std::string(generator::kParamsFile);
    std::string json = p.files.count(path) ? p.files.at(path) : std::string();
    for (const auto& [name, value] : values)
        json = generator::withParam(spec, json, name, value);
    p.files[path] = json;
    generator::refresh(p, id);
}

Layer& layerById(LayerStack& st, LayerId id) {
    for (std::size_t i = 0; i < st.size(); ++i)
        if (st[i].props.id == id)
            return st[i];
    throw std::runtime_error("no layer " + std::to_string(id));
}

project::Project generatorPiece(const fs::path& makerDir) {
    project::Project p;
    p.layers = LayerStack(kRidgeW, kRidgeH);
    p.ice = true;  // the glow's light red is a background under the ridges' edges
    p.sauce = sauceFor("Ridges at dusk");

    p.layers.add(duskSky());
    const LayerId starsId = generator::add(p, dirFiles(makerDir / "stars"), "stars");
    layerById(p.layers, starsId).props.mode = LayerMode::CharsOnly;
    const std::map<std::string, std::string> ridgeFiles = dirFiles(makerDir / "ridges");
    const LayerId ridgesId = generator::add(p, ridgeFiles, "ridges");
    // the first round: two ridges, smooth
    setParams(p, ridgesId, {{"ridges", 2.0}, {"roughness", 0.0}});
    project::takeSnapshot(p);

    // the second: the parameters the piece keeps, three ridges, rougher
    setParams(p, ridgesId, {{"ridges", 3.0}, {"roughness", 0.3}});
    project::takeSnapshot(p);

    // the same generator again with another seed, hidden, and a baked copy
    const LayerId otherId = generator::add(p, ridgeFiles, "ridges (seed 12)");
    setParams(p, otherId, {{"seed", 12.0}, {"ridges", 4.0}, {"roughness", 0.3}, {"sharp", true}});
    layerById(p.layers, otherId).props.visible = false;
    Layer baked = bake(layerById(p.layers, ridgesId), p.ice);
    baked.props.id = 0;
    baked.props.name = "ridges (baked)";
    baked.props.visible = false;
    p.layers.add(std::move(baked));

    // the third: the lake, shore, pines and cabin over what the generators made
    p.layers.add(lake());
    p.layers.add(shore());
    p.layers.add(pines());
    p.layers.add(cabin());

    p.notes = readFile(makerDir / "notes" / "04-generator-ridges.md");
    project::takeSnapshot(p);
    return p;
}

// ---- the character ----------------------------------------------------------

constexpr int kCharW = 80, kCharH = 30;
// the first row of the meadow, below the hills
constexpr int kMeadowTop = 20;
// where the figure's layers sit, and their size in cells
constexpr int kFigX = 44, kFigY = 5, kFigW = 32, kFigH = 23;

// A day sky on the ice ramp: a rung a row down from blue at the top to a
// solid light blue, then on to light cyan at the horizon.
Layer meadowSky() {
    Layer l = makeLayer(LayerKind::Cells, kCharW, kMeadowTop, "sky");
    const int rungs[kMeadowTop] = {6, 7, 8, 8, 8, 8, 8, 8, 8, 9, 10, 11, 12, 12, 12, 12, 12, 12, 12, 12};
    for (int y = 0; y < kMeadowTop; ++y)
        for (int x = 0; x < kCharW; ++x)
            l.cells.at(x, y) = rungCell(ramp("ice"), rungs[y]);
    l.props.locked = true;
    return l;
}

// Far hills in the haze, cyan against the light cyan sky, their tops in
// half-block steps over it.
Layer hills() {
    const int top = 14, h = kMeadowTop - top;
    Layer l = makeLayer(LayerKind::Cells, kCharW, h, "hills", 0, top);
    for (int x = 0; x < kCharW; ++x) {
        // the hills' top in pixel rows of the layer, rolling
        const double wave = 6.0 + 2.5 * std::sin(x / 8.0 + 0.6) + 1.2 * std::sin(x / 3.5 + 1.3);
        const int crest = std::clamp(int(std::lround(wave)), 1, 2 * h - 2);
        for (int y = 0; y < h; ++y) {
            if (2 * y + 1 < crest)
                continue;  // sky
            l.cells.at(x, y) = 2 * y < crest ? cell(U'▄', Cyan, LightCyan) : cell(U'█', Cyan, Cyan);
        }
    }
    l.props.locked = true;
    return l;
}

// The meadow the figure stands in: light green going back to the hills, a
// path across it, and tufts of darker grass in the near rows.
Layer meadow() {
    const int h = kCharH - kMeadowTop;
    Layer l = makeLayer(LayerKind::Cells, kCharW, h, "meadow", 0, kMeadowTop);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < kCharW; ++x)
            l.cells.at(x, y) = cell(U'█', LightGreen, LightGreen);
    // a path coming out of the hills and widening toward the front, past
    // where the traveller stands
    const auto onPath = [](int x, int y) {
        const double centre = 34 + 1.4 * y, half = 0.8 + 0.9 * y;
        return std::abs(x + 0.5 - centre) <= half;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < kCharW; ++x)
            if (onPath(x, y))
                l.cells.at(x, y) = cell(U'█', Brown, Brown);
    Lcg r{31};
    for (int y = 4; y < h; y += 2)
        for (int x = r.below(9); x + 3 <= kCharW;) {
            const int len = 2 + r.below(2);
            bool clear = true;
            for (int i = -1; i <= len; ++i)
                clear = clear && !onPath(x + i, y);
            for (int i = 0; clear && i < len; ++i)
                l.cells.at(x + i, y) = cell(U'▄', Green, LightGreen);
            x += len + 7 + r.below(12);
        }
    l.props.locked = true;
    return l;
}

// The traveller, in half-block pixels, a character a pixel: facing left, into
// the light from the upper left, a staff in the front hand and a pack on the
// back. Each letter is a part:
//   h hat   b hat band   a hair   s skin   y scarf   c coat   C coat, lit
//   g pack  G pack, lit  t trousers   o boots and glove   w staff
//   k the outline's black, filling the shade under the arm
// and the letters a layer above the body draws over a part:
//   e eye, m mouth (face detail, over skin)
//   H on the hat, Y on the lit coat, K on the staff (highlights)
const std::vector<std::string>& traveller() {
    static const std::vector<std::string> rows = {
        ".........hhhh.....",  //  0
        "........hHHhhh....",
        "........hHHhhh....",
        "........hhhhhh....",
        "........bbbbbb....",
        ".K..hhhhhhhhhhhhh.",  //  5
        ".K......sssssaa...",
        ".w.....sessssaa...",
        ".w....sssssssa....",
        ".w.....ssssssa....",
        ".w......mssss.....",  // 10
        ".w.......yyyy.....",
        ".w......yyyyccgg..",
        ".w.....CCCcccGGgg.",
        ".w....CCCcccccGgg.",
        ".w....CCc.ccccggg.",  // 15
        ".w....YYc.ccccggg.",
        ".w....YCc.ccccggg.",
        ".w...CCcc.ccccggg.",
        ".w..CCcc..ccccgg..",
        ".w.CCcc...Ccccc...",  // 20
        ".oooc..kk.Ccccc...",
        ".ooo.k.kk.Ccccc...",
        ".w........CCcccc..",
        ".w.......CCccccc..",
        ".w.......CCcccccc.",  // 25
        ".w.......Cccccccc.",
        ".w........ttt.ttt.",
        ".w........ttt.ttt.",
        ".w.......ttt..ttt.",
        ".w.......ttt..ttt.",  // 30
        ".w.......ttt..ttt.",
        ".w.......ttt..ttt.",
        ".w.......ttt..ttt.",
        ".w.....ttt....ttt.",
        ".w.....ttt....ttt.",  // 35
        ".w.....ttt....ttt.",
        ".w.....ttt....ttt.",
        ".w....ttt.....ttt.",
        ".w....ttt.....ttt.",
        ".w....ooo.....ooo.",  // 40
        ".w....ooo.....ooo.",
        ".w..ooooo....oooo.",
        ".w.oooooo....oooo.",
    };
    return rows;
}

enum class Part { Body, Face, Highlight };

// The colour a sprite letter gives one of the figure's layers, if any. The
// body in flat colours (`flat`) has no lit side.
std::optional<std::uint8_t> partColour(char ch, Part part, bool flat = false) {
    switch (part) {
    case Part::Face:
        return ch == 'e' ? std::optional<std::uint8_t>(Black) : ch == 'm' ? std::optional<std::uint8_t>(Red) : std::nullopt;
    case Part::Highlight:
        return ch == 'H' ? std::optional<std::uint8_t>(Grey)
               : ch == 'Y' || ch == 'K' ? std::optional<std::uint8_t>(Yellow) : std::nullopt;
    case Part::Body: break;
    }
    switch (ch) {
    case 'h': case 'H': return DarkGrey;
    case 'b': return Red;
    case 'a': return Grey;
    case 's': case 'e': case 'm': return Brown;
    case 'y': return Yellow;
    case 'c': return Red;
    case 'C': case 'Y': return flat ? Red : LightRed;
    case 'g': return Green;
    case 'G': return flat ? Green : LightGreen;
    case 't': return Blue;
    case 'o': return DarkGrey;
    case 'w': case 'K': return Brown;
    default: return std::nullopt;  // '.', and 'k', the outline's own
    }
}

// The sprite's pixel (px, py) in the figure's layers: a pixel of margin
// round it for the outline.
bool inSprite(int px, int py) {
    const auto& rows = traveller();
    const int sx = px - 1, sy = py - 1;
    return sy >= 0 && sy < int(rows.size()) && sx >= 0 && sx < int(rows[std::size_t(sy)].size()) &&
           rows[std::size_t(sy)][std::size_t(sx)] != '.';
}

char spriteAt(int px, int py) { return inSprite(px, py) ? traveller()[std::size_t(py - 1)][std::size_t(px - 1)] : '.'; }

Layer figureLayer(const std::string& name, const std::function<std::optional<std::uint8_t>(int, int)>& colourAt) {
    Layer l = makeLayer(LayerKind::Pixels, kFigW, kFigH, name, kFigX, kFigY);
    for (int py = 0; py < 2 * kFigH; ++py)
        for (int px = 0; px < kFigW; ++px)
            if (const auto c = colourAt(px, py))
                l.cells.at(px, py / 2) = setLayerPixel(l.cells.at(px, py / 2), py % 2 == 1, *c);
    return l;
}

Layer figurePart(const std::string& name, Part part, bool flat = false) {
    return figureLayer(name, [part, flat](int px, int py) { return partColour(spriteAt(px, py), part, flat); });
}

// The outline: the whole silhouette grown a pixel all round, in black, under
// the body. The body covers all of it but the ring, and the gaps between
// the arm, the staff and the legs.
Layer outline() {
    return figureLayer("outline", [](int px, int py) -> std::optional<std::uint8_t> {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (inSprite(px + dx, py + dy))
                    return Black;
        return std::nullopt;
    });
}

// The shadow the figure casts on the meadow, away from the light: a strip
// of darker green from the back boot out to the lower right. Its lower row
// runs on a single pixel past the upper, so the tip is one cell of a green
// bottom half on the light green: two such cells would look like a tuft in
// the grass, and with a tuft a cell past them read as a line with a break
// in it, wherever the figure is moved.
Layer castShadow() {
    return figureLayer("shadow", [](int px, int py) -> std::optional<std::uint8_t> {
        const int ground = 44;  // the boots' last pixel row
        if (py == ground && px >= 18 && px <= 30)
            return Green;
        if (py == ground + 1 && px >= 17 && px <= 31)
            return Green;
        return std::nullopt;
    });
}

project::Project character(const fs::path& makerDir) {
    project::Project p;
    p.layers = LayerStack(kCharW, kCharH);
    p.ice = true;  // the hills' light cyan sky behind their tops is a background
    p.sauce = sauceFor("Traveller");

    // the first round: the scene
    p.layers.add(meadowSky());
    p.layers.add(hills());
    p.layers.add(meadow());
    project::takeSnapshot(p);

    // the second: the figure with its outline and its parts flat
    p.layers.add(outline());
    p.layers.add(figurePart("body", Part::Body, true));
    project::takeSnapshot(p);

    // the third: the lit side, the face, the highlights and the shadow, and
    // the whole figure merged onto one layer, hidden
    layerNamed(p.layers, "body").cells = figurePart("body", Part::Body).cells;
    layerNamed(p.layers, "body").props.alphaLocked = true;
    LayerStack st(kCharW, kCharH);
    for (std::size_t i = 0; i < p.layers.size(); ++i) {
        Layer l = p.layers[i];
        l.props.id = 0;
        const bool meadowLayer = l.props.name == "meadow";
        st.add(std::move(l));
        if (meadowLayer)
            st.add(castShadow());
    }
    p.layers = st;
    p.layers.add(figurePart("face detail", Part::Face));
    p.layers.add(figurePart("highlights", Part::Highlight));
    Layer merged = makeLayer(LayerKind::Pixels, kFigW, kFigH, "figure (merged)", kFigX, kFigY);
    for (const char* name : {"shadow", "outline", "body", "face detail", "highlights"}) {
        const Layer& part = layerNamed(p.layers, name);
        for (int y = 0; y < kFigH; ++y)
            for (int x = 0; x < kFigW; ++x) {
                const LayerPixels over = layerPixels(part.cells.at(x, y));
                Cell& c = merged.cells.at(x, y);
                if (over.top)
                    c = setLayerPixel(c, false, *over.top);
                if (over.bottom)
                    c = setLayerPixel(c, true, *over.bottom);
            }
    }
    merged.props.visible = false;
    p.layers.add(std::move(merged));

    p.notes = readFile(makerDir / "notes" / "05-character.md");
    project::takeSnapshot(p);
    return p;
}

// ---- the Blockwright logo ---------------------------------------------------

constexpr int kBrandW = 80, kBrandH = 18;
// where the wordmark's layers sit, and their size in cells
constexpr int kWordX = 26, kWordY = 2, kWordW = 52, kWordH = 15;

// The wordmark, BLOCK over WRIGHT, in half-block pixels, a character a
// pixel, as it sits in its layers. The strokes are two pixels wide and
// rounded at the corners by single pixels. Each letter is how the pixel
// is drawn:
//   c light cyan   d cyan
//   D M L under a shade cell, dark, medium or light (▓ ▒ ░ in light cyan
//     on cyan), a cell's two pixels the same
// The shade runs down each stroke a rung a row, from light cyan through
// ▓ ▒ ░ to cyan at the foot, with a stem's left column a rung lighter,
// where the light from the upper left falls on it.
const std::vector<std::string>& wordmark() {
    static const std::vector<std::string> rows = {
        "ccccc...cc.......cccc....ccccc..cc..cc..............",  //  0
        "cccccc..cc......cccccc..cccccc..cc..cc..............",
        "cc..cc..cc......cc..cc..cc......cc.cc...............",
        "cc..cc..cc......cc..cc..cc......cc.cc...............",
        "cDDDD...cD......cD..DD..cD......cDDc................",
        "cDDDDc..cD......cD..DD..cD......cDD.................",  //  5
        "DM..MM..DM......DM..MM..DM......DMMM................",
        "DM..MM..DM......DM..MM..DM......DMMM................",
        "ML..LL..ML......ML..LL..ML......ML.ML...............",
        "ML..LL..ML......ML..LL..ML......ML.ML...............",
        "Lddddd..Lddddd..dLdddd..dLdddd..Ld..Ld..............",  // 10
        "Ldddd...Lddddd...Lddd....Ldddd..Ld..Ld..............",
        "....................................................",
        "....................................................",
        "....................................................",
        "....................................................",  // 15
        "cc......cc..ccccc....cccccc...ccccc..cc..cc..cccccc.",
        "cc......cc..cccccc...cccccc..cccccc..cc..cc..cccccc.",
        "cc......cc..cc..cc.....cc....cc......cc..cc....cc...",
        "cc......cc..cc..cc.....cc....cc......cc..cc....cc...",
        "cD..cD..DD..cD..DD.....DD....cD......cD..cD....cD...",  // 20
        "cD..cD..DD..cDccDD.....DD....cD.ccc..cDcccD....cD...",
        "DM..DM..MM..DMcMM......MM....DM.cMM..DMccDM....DM...",
        "DM..DM..MM..DM.MM......MM....DM..MM..DM..DM....DM...",
        "ML..ML..LL..ML..ML.....LL....ML..LL..ML..ML....ML...",
        "ML..ML..LL..ML..ML.....LL....ML..LL..ML..ML....ML...",  // 25
        "dLdddddddd..Ld...Ld..dddddd..dLdddd..Ld..Ld....Ld...",
        ".Ldd..ddd...Ld...Ld..dddddd...Ldddd..Ld..Ld....Ld...",
    };
    return rows;
}

char wordmarkAt(int px, int py) {
    const auto& rows = wordmark();
    if (py < 0 || py >= int(rows.size()) || px < 0 || px >= int(rows[std::size_t(py)].size()))
        return '.';
    return rows[std::size_t(py)][std::size_t(px)];
}

bool inWordmark(int px, int py) { return wordmarkAt(px, py) != '.'; }

// The pixels of the wordmark's layers no letter closes in: the inside of a
// B, an O or an R is a counter, and the drop doesn't fill it.
std::vector<bool> openPixels() {
    const int pw = kWordW, ph = 2 * kWordH;
    std::vector<bool> open(std::size_t(pw * ph), false);
    std::deque<std::pair<int, int>> todo;
    for (int px = 0; px < pw; ++px)
        for (int py = 0; py < ph; ++py)
            if (px == 0 || py == 0 || px == pw - 1 || py == ph - 1)
                todo.emplace_back(px, py);
    while (!todo.empty()) {
        const auto [px, py] = todo.front();
        todo.pop_front();
        if (px < 0 || py < 0 || px >= pw || py >= ph || inWordmark(px, py) || open[std::size_t(py * pw + px)])
            continue;
        open[std::size_t(py * pw + px)] = true;
        todo.emplace_back(px + 1, py);
        todo.emplace_back(px - 1, py);
        todo.emplace_back(px, py + 1);
        todo.emplace_back(px, py - 1);
    }
    return open;
}

Layer wordLayer(const std::string& name, const std::function<std::optional<std::uint8_t>(int, int)>& colourAt) {
    Layer l = makeLayer(LayerKind::Pixels, kWordW, kWordH, name, kWordX, kWordY);
    for (int py = 0; py < 2 * kWordH; ++py)
        for (int px = 0; px < kWordW; ++px)
            if (const auto c = colourAt(px, py))
                l.cells.at(px, py / 2) = setLayerPixel(l.cells.at(px, py / 2), py % 2 == 1, *c);
    return l;
}

// The letters in flat colours: light cyan, and cyan where the shade has
// gone dark. Under the shade cells it is the colour each mostly shows, so
// with the shading hidden the letters still go from light to dark.
Layer letters(bool flat = false) {
    return wordLayer("letters", [flat](int px, int py) -> std::optional<std::uint8_t> {
        switch (wordmarkAt(px, py)) {
        case 'c': case 'D': case 'M': return LightCyan;
        case 'd': case 'L': return flat ? LightCyan : Cyan;
        default: return std::nullopt;
        }
    });
}

// The drop: the letters moved one column right and one pixel down, in
// blue, under them, so they stand out from the black toward the lower
// right. The letters cover most of it; what shows is a rim along their
// right and lower edges. It stays out of the counters, and three pixels
// are taken off one by one where the rule leaves a spur: in the notch of the
// K, top and bottom, and under the R's bowl, by its leg.
Layer drop() {
    const std::vector<bool> open = openPixels();
    return wordLayer("drop", [&open](int px, int py) -> std::optional<std::uint8_t> {
        const bool trimmed = (px == 34 && (py == 2 || py == 8)) || (px == 17 && py == 22);
        if (inWordmark(px - 1, py - 1) && open[std::size_t(py * kWordW + px)] && !trimmed)
            return Blue;
        return std::nullopt;
    });
}

// The shade cells: ▓ ▒ ░ in light cyan on cyan, over the letters.
Layer shading() {
    Layer l = makeLayer(LayerKind::Cells, kWordW, kWordH, "shading", kWordX, kWordY);
    for (int y = 0; y < kWordH; ++y)
        for (int x = 0; x < kWordW; ++x)
            switch (wordmarkAt(x, 2 * y)) {
            case 'D': l.cells.at(x, y) = cell(U'▓', LightCyan, Cyan); break;
            case 'M': l.cells.at(x, y) = cell(U'▒', LightCyan, Cyan); break;
            case 'L': l.cells.at(x, y) = cell(U'░', LightCyan, Cyan); break;
            default: break;
            }
    return l;
}

// One block of the mark, 10 by 7 cells, at (x, y) of a layer: a white top
// edge in half blocks, five rows of `body` (the ramp across the face, lit
// from the left), and a bottom edge half a cell deep in the shadow's
// colour, which also runs down the last column of the top edge.
void markBlock(Canvas& c, int x, int y, const std::vector<Cell>& body, std::uint8_t shadow) {
    for (int i = 0; i < 10; ++i) {
        c.at(x + i, y) = cell(U'▄', i < 9 ? White : shadow, Black);
        for (int r = 1; r <= 5; ++r)
            c.at(x + i, y + r) = body[std::size_t(i)];
        c.at(x + i, y + 6) = cell(U'▀', shadow, Black);
    }
}

// The three white blocks of the mark, stacked in an L: a white edge on the
// left, the face going from ▓ to ░ in white on grey, and a dark grey side
// and foot.
Layer whiteBlocks() {
    Layer l = makeLayer(LayerKind::Cells, 22, 14, "white blocks", 2, 3);
    const std::vector<Cell> body = {
        cell(U'█', White, Black), cell(U'▓', White, Grey), cell(U'▓', White, Grey), cell(U'▓', White, Grey),
        cell(U'▒', White, Grey),  cell(U'▒', White, Grey), cell(U'░', White, Grey), cell(U'░', White, Grey),
        cell(U' ', Black, Grey),  cell(U'█', DarkGrey, Black),
    };
    markBlock(l.cells, 0, 0, body, DarkGrey);
    markBlock(l.cells, 0, 7, body, DarkGrey);
    markBlock(l.cells, 12, 7, body, DarkGrey);
    return l;
}

// The gold block that tops the mark, two rows higher than the white one
// beside it: yellow going through ▓ ▒ ░ to brown, with a brown side
// and foot.
Layer goldBlock() {
    Layer l = makeLayer(LayerKind::Cells, 10, 7, "gold block", 14, 1);
    const std::vector<Cell> body = {
        cell(U'█', White, Black),  cell(U'█', Yellow, Black), cell(U'█', Yellow, Black), cell(U'█', Yellow, Black),
        cell(U'▓', Yellow, Brown), cell(U'▓', Yellow, Brown), cell(U'▒', Yellow, Brown), cell(U'▒', Yellow, Brown),
        cell(U'░', Yellow, Brown), cell(U' ', Black, Brown),
    };
    markBlock(l.cells, 0, 0, body, Brown);
    return l;
}

Layer brandBackdrop() {
    Layer l = makeLayer(LayerKind::Cells, kBrandW, kBrandH, "backdrop");
    for (int y = 0; y < kBrandH; ++y)
        for (int x = 0; x < kBrandW; ++x)
            l.cells.at(x, y) = blackBlock();
    l.props.locked = true;
    return l;
}

project::Project brandLogo(const fs::path& makerDir) {
    project::Project p;
    p.layers = LayerStack(kBrandW, kBrandH);
    p.ice = false;  // the logo's own setting: no bright backgrounds in it
    p.sauce = sauceFor("Blockwright logo");

    // the first round: the mark and the letters, flat
    p.layers.add(brandBackdrop());
    p.layers.add(whiteBlocks());
    p.layers.add(goldBlock());
    p.layers.add(letters(true));
    project::takeSnapshot(p);

    // the second: the letters going dark toward the foot, the drop under
    // them
    LayerStack st(kBrandW, kBrandH);
    for (std::size_t i = 0; i < p.layers.size(); ++i) {
        Layer l = p.layers[i];
        l.props.id = 0;
        if (l.props.name == "letters")
            st.add(drop());
        st.add(std::move(l));
    }
    p.layers = st;
    layerNamed(p.layers, "letters").cells = letters().cells;
    layerNamed(p.layers, "letters").props.alphaLocked = true;
    project::takeSnapshot(p);

    // the third: the shade cells
    p.layers.add(shading());

    p.notes = readFile(makerDir / "notes" / "06-blockwright-logo.md");
    project::takeSnapshot(p);
    return p;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: bw-make-samples <samples dir>\n";
        return 2;
    }
    // generators need SIGCHLD at its default (generator.h)
    std::signal(SIGCHLD, SIG_DFL);
    try {
        const fs::path out = argv[1];
        constexpr auto raw = project::CellsFrame::Raw;
        project::writeDirectory(showcase(BW_MAKER_DIR), out / "01-layers-showcase", raw);
        project::writeDirectory(ddosKing(BW_MAKER_DIR), out / "02-ddos-king", raw);
        project::writeDirectory(stillLife(BW_MAKER_DIR), out / "03-still-life", raw);
        project::writeDirectory(generatorPiece(BW_MAKER_DIR), out / "04-generator-ridges", raw);
        project::writeDirectory(character(BW_MAKER_DIR), out / "05-character", raw);
        project::writeDirectory(brandLogo(BW_MAKER_DIR), out / "06-blockwright-logo", raw);
        project::writeDirectory(battlestation(BW_MAKER_DIR), out / "07-battlestation", raw);
        project::writeDirectory(brainOnIrc(BW_MAKER_DIR), out / "08-brain-on-irc", raw);
    } catch (const std::exception& e) {
        std::cerr << "bw-make-samples: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
