// Writes the sample projects (samples/<name>/) so every layer of
// them can be made again the same way:
//
//   bw-make-samples <samples dir>
//
// It writes each sample as a directory project under <samples dir>,
// replacing what is there. The fog layer's cells are its generator's first
// run (python3 fog.py), as Add Generator makes them.
#include "blockwright/composite.h"
#include "blockwright/generator.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/palette.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/sauce.h"
#include "blockwright/text.h"

#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QPen>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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

QByteArray sketchPng() {
    // the composition as drawn before the cells: horizon, towers, moon
    QImage img(640, 400, QImage::Format_RGB32);
    img.fill(QColor(250, 248, 240));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(60, 60, 70), 2));
    p.drawLine(0, 240, 640, 240);
    const int towers[][3] = {{32, 72, 216}, {72, 104, 168}, {104, 152, 120}, {200, 256, 144}, {232, 264, 96},
                             {296, 344, 192}, {336, 384, 128}};
    for (const auto& t : towers)
        p.drawRect(t[0], t[2], t[1] - t[0], 240 - t[2]);
    p.drawEllipse(QPointF(544, 175), 40, 40);
    p.setPen(QPen(QColor(120, 120, 140), 1, Qt::DashLine));
    for (int y = 260; y < 380; y += 16)
        p.drawLine(520 - (y - 240) / 4, y, 568 + (y - 240) / 4, y);
    p.end();
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
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
    const QByteArray png = sketchPng();
    img.bytes = std::make_shared<const std::string>(png.constData(), std::size_t(png.size()));
    img.x = 0, img.y = 0, img.width = kW, img.height = kH;
    img.opacity = 40;
    ref.props.reference = img;
    ref.props.visible = false;
    p.layers.add(std::move(ref));

    p.notes = readFile(makerDir / "notes" / "01-layers-showcase.md");
    project::takeSnapshot(p);
    return p;
}

// ---- the text-style logo ----------------------------------------------------

constexpr int kLogoW = 80, kLogoH = 27;

TextStyle styleFrom(const fs::path& file) { return library::readTextStyleEntry(file).style; }

Layer styledText(const TextSource& src, const std::string& name, int x, int y, const TextStyle& style, bool ice) {
    Layer l = makeTextLayer(src, name, x, y);
    LayerProperties props = l.props;
    props.style = style;
    applyLayerProperties(l, props, {}, ice);
    return l;
}

TextSource bigWord(const std::u32string& word) {
    TextSource src;
    src.text = word;
    src.bigStyle = BigTextStyle::Block;
    src.letterSpacing = 2;
    src.colour = Yellow;
    return src;
}

// The black the logo sits on, locked: black blocks, which the IRC export
// keeps, so the empty rows over and under the lettering stay as margins.
Layer backdrop() {
    Layer l = makeLayer(LayerKind::Cells, kLogoW, kLogoH, "backdrop");
    for (int y = 0; y < kLogoH; ++y)
        for (int x = 0; x < kLogoW; ++x)
            l.cells.at(x, y) = blackBlock();
    l.props.locked = true;
    return l;
}

// Embers drifting up off the lettering: CP437 dots in the fire colours, a
// glyph each, never two side by side, in the two corners the words leave.
Layer sparks() {
    Layer l = makeLayer(LayerKind::Cells, kLogoW, kLogoH, "sparks");
    struct Zone {
        int x0, y0, x1, y1, n;
    };
    const Zone zones[] = {{58, 1, 77, 8, 6}, {3, 11, 32, 17, 7}};
    Lcg r{7};
    for (const Zone& z : zones)
        for (int placed = 0, tries = 0; placed < z.n && tries < 500; ++tries) {
            const int x = z.x0 + r.below(z.x1 - z.x0 + 1), y = z.y0 + r.below(z.y1 - z.y0 + 1);
            bool clear = true;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (x + dx >= 0 && x + dx < kLogoW && y + dy >= 0 && y + dy < kLogoH &&
                        !l.cells.at(x + dx, y + dy).transparent)
                        clear = false;
            if (!clear)
                continue;
            const int pick = r.below(3);
            l.cells.at(x, y) = pick == 0 ? cell(U'*', Yellow, Black) : pick == 1 ? cell(U'+', LightRed, Black)
                                                                                  : cell(U'·', Yellow, Black);
            ++placed;
        }
    return l;
}

project::Project logo(const fs::path& makerDir) {
    const fs::path dir = makerDir / "logo";
    const bool ice = true;  // the fire ramp's yellow and white rungs are iCE backgrounds
    project::Project p;
    p.layers = LayerStack(kLogoW, kLogoH);
    p.ice = ice;
    p.sauce = sauceFor("Ember Kiln");

    p.layers.add(backdrop());
    p.layers.add(sparks());

    // one saved stack on both words, a second stack on hidden copies of them
    const TextStyle molten = styleFrom(dir / "molten.style.json");
    const TextStyle quenched = styleFrom(dir / "quenched.style.json");
    Layer ember = styledText(bigWord(U"EMBER"), "EMBER", 3, 1, molten, ice);
    Layer baked = bake(ember, ice);
    baked.props.id = 0;
    baked.props.name = "EMBER (baked)";
    baked.props.visible = false;
    p.layers.add(std::move(baked));
    for (const auto& [word, name, x, y] :
         {std::tuple{U"EMBER", "EMBER (quenched)", 3, 1}, {U"KILN", "KILN (quenched)", 37, 10}}) {
        Layer cold = styledText(bigWord(word), name, x, y, quenched, ice);
        cold.props.visible = false;
        p.layers.add(std::move(cold));
    }
    p.layers.add(std::move(ember));
    p.layers.add(styledText(bigWord(U"KILN"), "KILN", 37, 10, molten, ice));

    // the first round: the two words, no tagline
    project::takeSnapshot(p);

    TextSource tag;
    tag.text = U"ART AT WHITE HEAT";
    tag.face.name = "mini";
    tag.colour = Yellow;
    p.layers.add(styledText(tag, "tagline", 5, 20, styleFrom(dir / "kiln-tag.style.json"), ice));

    // the stacks travel with the project, to copy into a library folder
    for (const auto& [name, bytes] : dirFiles(dir))
        p.files["styles/" + name] = bytes;
    p.notes = readFile(makerDir / "notes" / "02-text-style-logo.md");
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
        project::writeDirectory(showcase(BW_MAKER_DIR),
                                out / "01-layers-showcase");
        project::writeDirectory(logo(BW_MAKER_DIR), out / "02-text-style-logo");
    } catch (const std::exception& e) {
        std::cerr << "bw-make-samples: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
