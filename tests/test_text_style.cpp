// Text styles (PLAN §15): each effect's mask on a fixture word in a bundled
// pixel face and in the TheDraw test fonts, the fixed draw order, iCE on and
// off, lint, property undo, the project file, Bake, and the stacks refused.

#include "blockwright/text_style.h"

#include "blockwright/composite.h"
#include "blockwright/halfblock.h"
#include "blockwright/lint.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/style_draw.h"
#include "blockwright/tdf.h"
#include "blockwright/text.h"
#include "util.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace bw;
using bw::project::Entries;
using bw::project::Project;
using bw::project::ProjectError;
namespace fs = std::filesystem;

namespace {

constexpr int kNone = -1;   // no pixel: transparent, or black
constexpr int kShade = -2;  // a cell that isn't two pixels (a shade, a TheDraw glyph)

// The colour each half-block pixel of a canvas shows, two pixel rows a cell.
struct Pixels {
    int width = 0, height = 0;
    std::vector<int> colours;
    int at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return kNone;
        return colours[static_cast<std::size_t>(y) * width + x];
    }
    bool ink(int x, int y) const { return at(x, y) != kNone; }
    bool cellInk(int x, int y) const { return ink(x, 2 * y) || ink(x, 2 * y + 1); }
    bool fullCell(int x, int y) const { return ink(x, 2 * y) && ink(x, 2 * y + 1); }
};

Pixels pixelsOf(const Canvas& c, bool ice = true) {
    Pixels p{c.width(), 2 * c.height(), {}};
    p.colours.assign(static_cast<std::size_t>(p.width) * p.height, kNone);
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const Cell& cell = c.at(x, y);
            if (cell.transparent)
                continue;
            const auto halves = halfBlocks(cell, ice);
            const int top = halves ? halves->top : kShade, bottom = halves ? halves->bottom : kShade;
            p.colours[static_cast<std::size_t>(2 * y) * p.width + x] = top == Black ? kNone : top;
            p.colours[static_cast<std::size_t>(2 * y + 1) * p.width + x] = bottom == Black ? kNone : bottom;
        }
    return p;
}

TextSource word(std::u32string text, std::string face = "block", std::uint8_t colour = White) {
    TextSource s;
    s.text = std::move(text);
    s.face.name = std::move(face);
    s.colour = colour;
    return s;
}

Effect on(EffectSettings s) { return Effect{true, std::move(s)}; }

TextStyle styleOf(std::vector<EffectSettings> settings) {
    TextStyle s;
    for (EffectSettings& e : settings)
        s.effects.push_back(on(std::move(e)));
    return s;
}

const Ramp& steel() {
    static const Ramp r = *findRamp("steel");
    return r;
}
const Ramp& fire() {
    static const Ramp r = *findRamp("fire");
    return r;
}

Canvas plain(const TextSource& s, const Face& face) { return renderText(s, face).cells; }
Canvas plain(const TextSource& s) { return plain(s, *findFace(s.face)); }
Canvas styled(const TextSource& s, const TextStyle& style, const Face& face, bool ice = true) {
    return renderStyledText(s, style, face, ice).cells;
}
Canvas styled(const TextSource& s, const TextStyle& style, bool ice = true) {
    return styled(s, style, *findFace(s.face), ice);
}

// The TheDraw test font in `file` (tests/data/tdf).
std::unique_ptr<Face> tdf(const char* file) {
    auto fonts = readTdfFile(bwtest::dataDir() / "tdf" / file);
    REQUIRE(fonts.size() == 1);
    return tdfFace(std::move(fonts[0]));
}

// The letters of a one-line text: runs of columns with ink, each its first
// and last column and its ink's first and last cell row.
struct Letter {
    int x0, x1, y0, y1;
};
std::vector<Letter> lettersOf(const Pixels& p) {
    std::vector<Letter> out;
    std::optional<Letter> cur;
    for (int x = 0; x <= p.width; ++x) {
        int top = -1, bottom = -1;
        for (int y = 0; x < p.width && y < p.height / 2; ++y)
            if (p.cellInk(x, y)) {
                if (top < 0)
                    top = y;
                bottom = y;
            }
        if (top >= 0) {
            if (!cur)
                cur = Letter{x, x, top, bottom};
            cur->x1 = x;
            cur->y0 = std::min(cur->y0, top);
            cur->y1 = std::max(cur->y1, bottom);
        } else if (cur) {
            out.push_back(*cur);
            cur.reset();
        }
    }
    return out;
}

// Counts the pixels of `got` that aren't `want(x, y)`, the first in `first`.
template <typename Want>
int mismatches(const Pixels& got, Want want, std::string& first) {
    int n = 0;
    for (int y = 0; y < got.height; ++y)
        for (int x = 0; x < got.width; ++x)
            if (const int w = want(x, y); got.at(x, y) != w && n++ == 0)
                first = "pixel " + std::to_string(x) + "," + std::to_string(y) + ": " + std::to_string(got.at(x, y)) +
                        ", not " + std::to_string(w);
    return n;
}

bool brightBackground(const Canvas& c) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (!c.at(x, y).transparent && c.at(x, y).bg.paletteIndex() >= 8)
                return true;
    return false;
}

// A stack of every effect, as an app would offer it.
TextStyle everyEffect() {
    return styleOf({PlateEffect{Blue, 1}, GlowEffect{"steel", 2}, ShadowEffect{ShadowForm::Drop, Compass::SE, 1, DarkGrey},
                    OutlineEffect{OutlineForm::Pixels, Cyan}, FillEffect{FillMode::Rows, Grey, "steel"},
                    ShadeEffect{"steel", Compass::N}, BevelEffect{"steel", Compass::NW}});
}

// A cells layer of two full-block letters' worth of cells, for the styles
// the compositor draws.
Layer blockLayer(int x = 10, int y = 5) {
    Layer l = makeLayer(LayerKind::Cells, 7, 4, "blocks", x, y);
    for (int cy = 1; cy <= 2; ++cy)
        for (int cx : {1, 2, 4, 5})
            l.cells.at(cx, cy) = Cell{kFullBlock, Color::indexed(Grey), Color::indexed(Black), 0, false};
    return l;
}

Layer pixelLayer() {
    Layer l = makeLayer(LayerKind::Pixels, 6, 4, "pixels", 3, 3);
    for (int py = 2; py <= 5; ++py)
        for (int px = 2; px <= 3; ++px)
            l.cells.at(px, py / 2) = setLayerPixel(l.cells.at(px, py / 2), py & 1, Yellow);
    return l;
}

Layer liveStyled(const TextSource& s, const TextStyle& style, int x = 2, int y = 1) {
    Layer l = makeTextLayer(s, "title", x, y);
    LayerProperties p = l.props;
    p.style = style;
    applyLayerProperties(l, p);
    return l;
}

std::string json(const QJsonObject& o) {
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

QJsonObject object(const std::string& text) {
    return QJsonDocument::fromJson(QByteArray(text.data(), static_cast<qsizetype>(text.size()))).object();
}

std::string layerPath(const Layer& l) { return "layers/" + std::to_string(l.props.id) + ".json"; }

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "text_style" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

// ---- each effect's mask ------------------------------------------------------

TEST_CASE("Outline: a ring of pixels round the ink, or of cells round the cells with ink", "[style][mask]") {
    for (const char* face : {"block", "mini"}) {
        INFO(face);
        const TextSource s = word(U"funk", face);
        const Pixels ink = pixelsOf(plain(s));
        const auto near = [&](int x, int y, int r) {
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (ink.ink(x + dx, y + dy))
                        return true;
            return false;
        };
        std::string first;

        const TextStyle pixels = styleOf({OutlineEffect{OutlineForm::Pixels, Blue}});
        CHECK(styleMargins(pixels) == StyleMargins{1, 1, 1, 1});
        const Pixels got = pixelsOf(styled(s, pixels));
        CHECK(got.width == ink.width + 2);
        CHECK(got.height == ink.height + 4);
        CHECK(mismatches(
                  got,
                  [&](int x, int y) -> int {
                      const int ix = x - 1, iy = y - 2;
                      return ink.ink(ix, iy) ? White : near(ix, iy, 1) ? Blue : kNone;
                  },
                  first) == 0);
        INFO(first);

        const TextStyle cells = styleOf({OutlineEffect{OutlineForm::Cells, Blue}});
        const Pixels gotCells = pixelsOf(styled(s, cells));
        CHECK(mismatches(
                  gotCells,
                  [&](int x, int y) -> int {
                      const int ix = x - 1, iy = y - 2, cy = (iy - (iy & 1)) / 2;
                      if (ink.ink(ix, iy))
                          return White;
                      if (ink.cellInk(ix, cy))
                          return kNone;
                      for (int dy = -1; dy <= 1; ++dy)
                          for (int dx = -1; dx <= 1; ++dx)
                              if (ink.cellInk(ix + dx, cy + dy))
                                  return Blue;
                      return kNone;
                  },
                  first) == 0);
    }
}

TEST_CASE("Shadow: the ink moved its distance towards its direction, or extruded", "[style][mask]") {
    const TextSource s = word(U"funk", "block");
    const Pixels ink = pixelsOf(plain(s));
    std::string first;

    const TextStyle drop = styleOf({ShadowEffect{ShadowForm::Drop, Compass::SE, 2, DarkGrey}});
    CHECK(styleMargins(drop) == StyleMargins{0, 0, 2, 1});
    CHECK(mismatches(
              pixelsOf(styled(s, drop)),
              [&](int x, int y) -> int { return ink.ink(x, y) ? White : ink.ink(x - 2, y - 2) ? DarkGrey : kNone; }, first) == 0);
    INFO(first);

    const TextStyle extrude = styleOf({ShadowEffect{ShadowForm::Extrude, Compass::SE, 3, DarkGrey}});
    CHECK(styleMargins(extrude) == StyleMargins{0, 0, 3, 2});
    CHECK(mismatches(
              pixelsOf(styled(s, extrude)),
              [&](int x, int y) -> int {
                  if (ink.ink(x, y))
                      return White;
                  for (int k = 1; k <= 3; ++k)
                      if (ink.ink(x - k, y - k))
                          return int(DarkGrey);
                  return kNone;
              },
              first) == 0);

    // west: across, a pixel is a column
    const TextStyle west = styleOf({ShadowEffect{ShadowForm::Drop, Compass::W, 3, Red}});
    CHECK(styleMargins(west) == StyleMargins{3, 0, 0, 0});
    CHECK(mismatches(
              pixelsOf(styled(s, west)),
              [&](int x, int y) -> int { return ink.ink(x - 3, y) ? White : ink.ink(x, y) ? Red : kNone; }, first) == 0);
}

TEST_CASE("Plate: the text's box and its padding in one colour", "[style][mask]") {
    const TextSource s = word(U"funk", "mini");
    const Pixels ink = pixelsOf(plain(s));
    const TextStyle plate = styleOf({PlateEffect{Magenta, 2}});
    CHECK(styleMargins(plate) == StyleMargins{2, 2, 2, 2});
    const Pixels got = pixelsOf(styled(s, plate));
    CHECK(got.width == ink.width + 4);
    CHECK(got.height == ink.height + 8);
    std::string first;
    CHECK(mismatches(got, [&](int x, int y) -> int { return ink.ink(x - 2, y - 4) ? White : Magenta; }, first) == 0);
    INFO(first);
}

TEST_CASE("Glow: rings of the ramp a rung darker each, out to its width", "[style][mask]") {
    const TextSource s = word(U"funk", "block");
    const Pixels ink = pixelsOf(plain(s));
    const TextStyle glow = styleOf({GlowEffect{"fire", 3}});
    CHECK(styleMargins(glow) == StyleMargins{3, 3, 3, 3});
    const Canvas out = styled(s, glow);
    int checked = 0;
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) {
            // Chebyshev distance in cells to a cell with ink
            int d = 99;
            for (int iy = 0; iy < ink.height / 2; ++iy)
                for (int ix = 0; ix < ink.width; ++ix)
                    if (ink.cellInk(ix, iy))
                        d = std::min(d, std::max(std::abs(x - 3 - ix), std::abs(y - 3 - iy)));
            INFO(x << "," << y << " ring " << d);
            if (d == 0)
                continue;
            if (d > 3)
                CHECK(out.at(x, y).transparent);
            else
                CHECK(out.at(x, y) == rungCell(fire(), 3 - d + 1));
            ++checked;
        }
    CHECK(checked > 0);
    // the half of a fringe off the letter shows the glow's rung under it,
    // as the compositor puts a pixel over a shade (halvesUnder)
    const HalfPair under = halvesUnder(rungCell(fire(), 3));
    for (int y = 0; y < ink.height / 2; ++y)
        for (int x = 0; x < ink.width; ++x)
            if (ink.cellInk(x, y) && !ink.fullCell(x, y)) {
                const auto h = halfBlocks(out.at(x + 3, y + 3));
                INFO(x << "," << y);
                REQUIRE(h);
                const bool top = ink.ink(x, 2 * y);
                CHECK((top ? h->top : h->bottom) == White);
                CHECK((top ? h->bottom : h->top) == (top ? under.bottom : under.top));
            }
}

TEST_CASE("Fill flat: every ink pixel in its colour", "[style][mask]") {
    const TextSource s = word(U"funkpower", "vga", Grey);
    const Pixels ink = pixelsOf(plain(s));
    std::string first;
    CHECK(mismatches(
              pixelsOf(styled(s, styleOf({FillEffect{FillMode::Flat, Yellow, "fire"}}))),
              [&](int x, int y) -> int { return ink.ink(x, y) ? Yellow : kNone; }, first) == 0);
    INFO(first);
}

TEST_CASE("Fill rows and columns: the ladder down the rows or across the columns, light first", "[style][mask]") {
    const TextSource s = word(U"funk", "block");
    const Pixels ink = pixelsOf(plain(s));
    for (const FillMode mode : {FillMode::Rows, FillMode::Columns}) {
        const bool rows = mode == FillMode::Rows;
        INFO(fillModeName(mode));
        const Canvas out = styled(s, styleOf({FillEffect{mode, Grey, "steel"}}));
        REQUIRE(out.width() == ink.width);
        std::vector<int> along(static_cast<std::size_t>(rows ? out.height() : out.width()), -1);
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x) {
                INFO(x << "," << y);
                if (!ink.cellInk(x, y)) {
                    CHECK(out.at(x, y).transparent);
                    continue;
                }
                if (!ink.fullCell(x, y)) {
                    // a fringe: a solid ramp colour
                    const auto h = halfBlocks(out.at(x, y));
                    REQUIRE(h);
                    const int colour = ink.ink(x, 2 * y) ? h->top : h->bottom;
                    CHECK(std::find(steel().colours.begin(), steel().colours.end(), colour) != steel().colours.end());
                    continue;
                }
                const auto rung = rungOf(steel(), out.at(x, y));
                REQUIRE(rung);
                CHECK(*rung >= kRungsPerStep);
                int& r = along[static_cast<std::size_t>(rows ? y : x)];
                if (r < 0)
                    r = *rung;
                CHECK(*rung == r);  // one rung a row (a column)
            }
        along.erase(std::remove(along.begin(), along.end(), -1), along.end());
        REQUIRE(along.size() > 1);
        CHECK(std::is_sorted(along.rbegin(), along.rend()));  // light to dark
        CHECK(along.front() > along.back());
        for (std::size_t i = 1; i < along.size(); ++i)
            CHECK(along[i - 1] - along[i] <= 1);  // no rung skipped
    }
}

TEST_CASE("Fill rainbow: each letter one ramp colour, in turn", "[style][mask]") {
    const TextSource s = word(U"funk", "block");
    const Pixels ink = pixelsOf(plain(s));
    const std::vector<Letter> letters = lettersOf(ink);
    REQUIRE(letters.size() >= 3);
    const Pixels got = pixelsOf(styled(s, styleOf({FillEffect{FillMode::Rainbow, Grey, "fire"}})));
    for (std::size_t i = 0; i < letters.size(); ++i) {
        const int want = fire().colours[1 + i % (fire().colours.size() - 1)];
        for (int y = 0; y < got.height; ++y)
            for (int x = letters[i].x0; x <= letters[i].x1; ++x) {
                INFO("letter " << i << " pixel " << x << "," << y);
                CHECK(got.at(x, y) == (ink.ink(x, y) ? want : kNone));
            }
    }
}

TEST_CASE("Shade: full ink cells a rung darker a cell from the lit side", "[style][mask]") {
    const TextSource s = word(U"funk", "block");
    const Pixels ink = pixelsOf(plain(s));
    for (const Compass light : {Compass::N, Compass::W, Compass::SE}) {
        INFO(compassPointName(light));
        const Canvas out =
            styled(s, styleOf({FillEffect{FillMode::Flat, White, "steel"}, ShadeEffect{"steel", light}}));
        for (const Letter& l : lettersOf(ink))
            for (int y = l.y0; y <= l.y1; ++y)
                for (int x = l.x0; x <= l.x1; ++x) {
                    INFO(x << "," << y);
                    if (!ink.fullCell(x, y)) {
                        if (ink.cellInk(x, y))
                            CHECK(halfBlocks(out.at(x, y)));  // a fringe keeps its flat fill
                        continue;
                    }
                    const int kx = light == Compass::W ? x - l.x0 : light == Compass::SE ? l.x1 - x : 0;
                    const int ky = light == Compass::N ? y - l.y0 : light == Compass::SE ? l.y1 - y : 0;
                    const int k = light == Compass::SE ? (kx + ky) / 2 : kx + ky;
                    CHECK(rungOf(steel(), out.at(x, y)) == std::max(kRungsPerStep, steel().topRung() - k));
                }
    }
}

TEST_CASE("Bevel: the full edge cells facing the light a rung lighter", "[style][mask]") {
    const TextSource s = word(U"funk", "block");
    const Pixels ink = pixelsOf(plain(s));
    const TextStyle flat = styleOf({FillEffect{FillMode::Flat, Grey, "steel"}});
    TextStyle bevel = flat;
    bevel.effects.push_back(on(BevelEffect{"steel", Compass::NW}));
    const Canvas before = styled(s, flat), after = styled(s, bevel);
    int lit = 0;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x) {
            if (after.at(x, y) == before.at(x, y))
                continue;
            INFO(x << "," << y);
            ++lit;
            CHECK(ink.fullCell(x, y));
            CHECK(rungOf(steel(), after.at(x, y)) > rungOf(steel(), before.at(x, y)));
            // on the side facing the light
            CHECK((!ink.fullCell(x, y - 1) || !ink.fullCell(x - 1, y) || !ink.fullCell(x - 1, y - 1)));
        }
    CHECK(lit > 0);
}

TEST_CASE("a TheDraw face's cells: kept, recoloured or outlined, and shaded only where they are █",
          "[style][mask][tdf]") {
    const TextSource s = word(U"FUNK");
    for (const char* file : {"bw-colour.tdf", "bw-block.tdf"}) {
        INFO(file);
        const auto face = tdf(file);
        const Canvas base = plain(s, *face);
        // every cell of the font is a source cell
        int sources = 0;
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x)
                sources += !base.at(x, y).transparent;
        REQUIRE(sources > 0);

        // Fill off: the letters as they are, the cell outline round the
        // cells with ink (a blank cell has none)
        const Canvas outlined = styled(s, styleOf({OutlineEffect{OutlineForm::Cells, Blue}}), *face);
        for (int y = 0; y < outlined.height(); ++y)
            for (int x = 0; x < outlined.width(); ++x) {
                INFO(x << "," << y);
                const int bx = x - 1, by = y - 1;
                const bool inBase = bx >= 0 && by >= 0 && bx < base.width() && by < base.height();
                if (inBase && !base.at(bx, by).transparent) {
                    CHECK(outlined.at(x, y) == base.at(bx, by));
                    continue;
                }
                bool near = false;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = bx + dx, ny = by + dy;
                        near = near || (nx >= 0 && ny >= 0 && nx < base.width() && ny < base.height() &&
                                        !base.at(nx, ny).transparent && !isBlank(base.at(nx, ny)));
                    }
                if (near)
                    CHECK(halfBlocks(outlined.at(x, y)) == HalfPair{Blue, Blue});
                else
                    CHECK(outlined.at(x, y).transparent);
            }

        // flat: the foreground only
        const Canvas flat = styled(s, styleOf({FillEffect{FillMode::Flat, Yellow, "fire"}}), *face);
        // a ramp: each colour onto the ramp by rank, the characters kept
        const Canvas ramped = styled(s, styleOf({FillEffect{FillMode::Rows, Grey, "fire"}}), *face);
        const Canvas shaded =
            styled(s, styleOf({FillEffect{FillMode::Rows, Grey, "fire"}, ShadeEffect{"fire", Compass::N}}), *face);
        const auto onFire = [](const Color& c) {
            return std::find(fire().colours.begin(), fire().colours.end(), c.paletteIndex()) != fire().colours.end();
        };
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x) {
                const Cell& b = base.at(x, y);
                if (b.transparent)
                    continue;
                INFO(x << "," << y);
                CHECK(flat.at(x, y).ch == b.ch);
                CHECK(flat.at(x, y).bg == b.bg);
                CHECK(flat.at(x, y).fg.paletteIndex() == Yellow);
                CHECK(ramped.at(x, y).ch == b.ch);
                CHECK(onFire(ramped.at(x, y).fg));
                CHECK(onFire(ramped.at(x, y).bg));
                if (b.ch != kFullBlock)
                    CHECK(shaded.at(x, y) == ramped.at(x, y));
            }
        // ranks kept: a darker colour never maps lighter than a lighter one
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x)
                for (int y2 = 0; y2 < base.height(); ++y2)
                    for (int x2 = 0; x2 < base.width(); ++x2) {
                        const Cell &a = base.at(x, y), &b = base.at(x2, y2);
                        if (a.transparent || b.transparent || a.fg == b.fg)
                            continue;
                        if (ramped.at(x, y).fg == ramped.at(x2, y2).fg)
                            continue;
                        const auto rank = [](const Color& c) {
                            return std::find(fire().colours.begin(), fire().colours.end(), c.paletteIndex()) -
                                   fire().colours.begin();
                        };
                        const Rgb& ca = kPalette[a.fg.paletteIndex()];
                        const Rgb& cb = kPalette[b.fg.paletteIndex()];
                        const int la = 299 * ca.r + 587 * ca.g + 114 * ca.b, lb = 299 * cb.r + 587 * cb.g + 114 * cb.b;
                        if (la < lb)
                            CHECK(rank(ramped.at(x, y).fg) < rank(ramped.at(x2, y2).fg));
                    }
    }
}

// #184: a ramp Fill maps a TheDraw face's colours onto consecutive ramp
// steps from the top: the lightest to the top step a Fill reaches, each
// darker colour the step below, black kept black. bw-colour uses black (the
// background of its █), blue and cyan; bw-block draws in one colour on black.
TEST_CASE("a ramp Fill maps a TheDraw face's colours onto consecutive steps from the ramp's top and keeps black",
          "[style][tdf]") {
    const TextSource s = word(U"FUNK");
    const auto colour = tdf("bw-colour.tdf");
    const auto block = tdf("bw-block.tdf");
    const Canvas base = plain(s, *colour);
    // what each source colour becomes, read off the drawing: every source
    // cell's foreground and background
    const auto mapping = [&](const Face& face, const char* ramp, FillMode mode, bool ice) {
        const Canvas from = plain(s, face);
        const Canvas to = styled(s, styleOf({FillEffect{mode, Grey, ramp}}), face, ice);
        std::map<std::uint8_t, std::set<std::uint8_t>> got;
        for (int y = 0; y < from.height(); ++y)
            for (int x = 0; x < from.width(); ++x) {
                const Cell& b = from.at(x, y);
                if (b.transparent)
                    continue;
                got[b.fg.paletteIndex()].insert(to.at(x, y).fg.paletteIndex());
                if (b.ch != kFullBlock)
                    got[b.bg.paletteIndex()].insert(to.at(x, y).bg.paletteIndex());
            }
        // black (a blank's, a space's foreground) stays black
        if (const auto k = got.find(Black); k != got.end()) {
            CHECK(k->second == std::set<std::uint8_t>{Black});
            got.erase(k);
        }
        return got;
    };
    using M = std::map<std::uint8_t, std::set<std::uint8_t>>;
    REQUIRE(mapping(*colour, "sea", FillMode::Rows, true).count(Cyan));

    for (const FillMode mode : {FillMode::Rows, FillMode::Columns, FillMode::Rainbow}) {
        INFO(int(mode));
        // iCE on: the ramp's top two steps
        CHECK(mapping(*colour, "fire", mode, true) == M{{Cyan, {White}}, {Blue, {Yellow}}});
        CHECK(mapping(*colour, "sunset", mode, true) == M{{Cyan, {Yellow}}, {Blue, {LightRed}}});
        CHECK(mapping(*colour, "ice", mode, true) == M{{Cyan, {White}}, {Blue, {LightCyan}}});
        CHECK(mapping(*colour, "toxic", mode, true) == M{{Cyan, {White}}, {Blue, {LightCyan}}});
        CHECK(mapping(*colour, "steel", mode, true) == M{{Cyan, {White}}, {Blue, {Grey}}});
        // a ramp of two steps past black: no step skipped
        CHECK(mapping(*colour, "sea", mode, true) == M{{Cyan, {LightCyan}}, {Blue, {Cyan}}});
        // iCE off: the top a Fill reaches without a bright background, as a
        // Fill's gradient tops out (fire at LightRed, sunset at LightMagenta)
        CHECK(mapping(*colour, "fire", mode, false) == M{{Cyan, {LightRed}}, {Blue, {Red}}});
        CHECK(mapping(*colour, "sunset", mode, false) == M{{Cyan, {LightMagenta}}, {Blue, {Magenta}}});
        // one colour on black: the ramp's top
        const M one = mapping(*block, "fire", mode, true);
        REQUIRE(one.size() == 1);
        CHECK(one.begin()->second == std::set<std::uint8_t>{White});
    }
    // the black under bw-colour's █ stays black, as its blank cells do
    const Canvas fired = styled(s, styleOf({FillEffect{FillMode::Rows, Grey, "fire"}}), *colour);
    for (int y = 0; y < base.height(); ++y)
        for (int x = 0; x < base.width(); ++x)
            if (!base.at(x, y).transparent && base.at(x, y).ch == kFullBlock) {
                INFO(x << "," << y);
                CHECK(fired.at(x, y).bg.paletteIndex() == Black);
            }
}

// A colour face whose one glyph is a row of ▒, a cell for each ordered pair
// of unequal `colours` (foreground, background) and then each of `extra`.
std::unique_ptr<Face> pairFace(const std::vector<std::uint8_t>& colours,
                               const std::vector<std::pair<std::uint8_t, std::uint8_t>>& extra = {}) {
    std::vector<std::pair<std::uint8_t, std::uint8_t>> pairs;
    for (const std::uint8_t a : colours)
        for (const std::uint8_t b : colours)
            if (a != b)
                pairs.push_back({a, b});
    pairs.insert(pairs.end(), extra.begin(), extra.end());
    TdfFont font;
    font.name = "pairs";
    font.type = TdfType::Colour;
    CellBlock row(int(pairs.size()), 1);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        Cell& c = row.at(int(i), 0);
        c.ch = kShadeMedium;
        c.fg = Color::indexed(pairs[i].first);
        c.bg = Color::indexed(pairs[i].second);
    }
    font.glyphs[U'A' - TdfFont::kFirst] = row;
    return tdfFace(std::move(font));
}

// A palette colour's perceived brightness, as the rank mapping orders them.
int luma(const Color& c) {
    const Rgb& p = kPalette[c.paletteIndex() & 15];
    return 299 * p.r + 587 * p.g + 114 * p.b;
}

// #184: with iCE off a background shows only a dark ramp colour, so the top
// steps can merge a cell's two colours. A colour face whose one glyph is a
// ▓, blue on cyan (glyph bytes 01 01 B2 31 00): steel's top two steps would
// draw it grey on grey (cyan's white can't be a background), fire's red on
// red (light red can't either, and red is the step below).
TEST_CASE("a ramp Fill keeps a TheDraw cell's two colours apart with iCE off", "[style][tdf]") {
    TdfFont font;
    font.name = "shade";
    font.type = TdfType::Colour;
    CellBlock glyph(1, 1);
    glyph.at(0, 0).ch = kShadeDark;
    glyph.at(0, 0).fg = Color::indexed(Blue);
    glyph.at(0, 0).bg = Color::indexed(Cyan);
    font.glyphs[U'A' - TdfFont::kFirst] = glyph;
    const auto face = tdfFace(std::move(font));
    const TextSource s = word(U"A");
    REQUIRE(plain(s, *face).at(0, 0).ch == kShadeDark);

    struct Want {
        const char* ramp;
        bool ice;
        std::uint8_t fg, bg;
    };
    const Want wants[] = {
        // the top steps a step lower: still consecutive
        {"steel", false, DarkGrey, Grey},
        {"sunset", false, Blue, Magenta},
        {"wood", false, Red, Brown},
        // one dark step past black: no consecutive steps keep them apart,
        // so blue goes to black, as a darker colour under a lighter
        {"fire", false, Black, Red},
        {"ice", false, Black, Blue},
        // iCE on shows the bright background: the top two steps
        {"steel", true, Grey, White},
        {"fire", true, Yellow, White},
    };
    for (const FillMode mode : {FillMode::Rows, FillMode::Columns, FillMode::Rainbow})
        for (const Want& w : wants) {
            INFO(w.ramp << " / mode " << int(mode) << " / iCE " << (w.ice ? "on" : "off"));
            const Canvas out = styled(s, styleOf({FillEffect{mode, Grey, w.ramp}}), *face, w.ice);
            CHECK(out.at(0, 0).ch == kShadeDark);
            CHECK(out.at(0, 0).fg != out.at(0, 0).bg);
            CHECK(out.at(0, 0).fg == Color::indexed(w.fg));
            CHECK(out.at(0, 0).bg == Color::indexed(w.bg));
        }
    // every named ramp, either iCE: the two colours stay apart
    for (const Ramp& r : namedRamps())
        for (const bool ice : {true, false}) {
            INFO(r.name << " / iCE " << (ice ? "on" : "off"));
            const Canvas out = styled(s, styleOf({FillEffect{FillMode::Rows, Grey, r.name}}), *face, ice);
            CHECK(out.at(0, 0).fg != out.at(0, 0).bg);
        }

    // more colours than steps, each pair on a cell: no steps keep them all
    // apart, so a merged cell is moved a step apart, its source order kept
    // and black kept black
    const std::vector<std::uint8_t> colours = {Black, Blue, Green, Cyan, Red, Magenta, Brown, Grey};
    const auto manyFace = pairFace(colours);
    const std::size_t cells = colours.size() * (colours.size() - 1);
    REQUIRE(plain(s, *manyFace).width() == int(cells));
    const Canvas src = plain(s, *manyFace);
    // where a drawn colour sits on the ramp
    const auto stepOn = [](const Ramp& r, const Color& c) {
        return std::find(r.colours.begin(), r.colours.end(), c.paletteIndex()) - r.colours.begin();
    };
    for (const char* ramp : {"sea", "fire", "steel"})
        for (const FillMode mode : {FillMode::Rows, FillMode::Columns, FillMode::Rainbow})
            for (const bool ice : {true, false}) {
                const Ramp r = *findRamp(ramp);
                const Canvas out = styled(s, styleOf({FillEffect{mode, Grey, ramp}}), *manyFace, ice);
                for (int x = 0; x < out.width(); ++x) {
                    INFO(ramp << " / mode " << int(mode) << " / iCE " << (ice ? "on" : "off") << " / cell " << x);
                    const Cell& a = src.at(x, 0);
                    const Cell& b = out.at(x, 0);
                    CHECK(b.fg != b.bg);
                    if (!ice)
                        CHECK(b.bg.paletteIndex() < 8);
                    // black stays black
                    if (a.fg.paletteIndex() == Black)
                        CHECK(b.fg.paletteIndex() == Black);
                    if (a.bg.paletteIndex() == Black)
                        CHECK(b.bg.paletteIndex() == Black);
                    // the lighter side stays the lighter on the ramp
                    CHECK((luma(a.fg) < luma(a.bg)) == (stepOn(r, b.fg) < stepOn(r, b.bg)));
                }
            }
    // sea with iCE on puts the seven colours on steps 0 1 1 2 2 3 3 (blue
    // with black, grey with cyan): black on blue keeps its black and moves
    // blue up, grey on cyan moves cyan down at the ramp's top
    const auto at = [&](std::uint8_t fg, std::uint8_t bg) {
        for (int x = 0; x < src.width(); ++x)
            if (src.at(x, 0).fg.paletteIndex() == fg && src.at(x, 0).bg.paletteIndex() == bg)
                return x;
        FAIL("no cell");
        return -1;
    };
    for (const FillMode mode : {FillMode::Rows, FillMode::Columns, FillMode::Rainbow}) {
        INFO(int(mode));
        const Canvas out = styled(s, styleOf({FillEffect{mode, Grey, "sea"}}), *manyFace, true);
        CHECK(out.at(at(Black, Blue), 0).fg == Color::indexed(Black));
        CHECK(out.at(at(Black, Blue), 0).bg == Color::indexed(Blue));
        CHECK(out.at(at(Grey, Cyan), 0).fg == Color::indexed(LightCyan));
        CHECK(out.at(at(Grey, Cyan), 0).bg == Color::indexed(Cyan));
    }
}

// #184: the steps that keep the most cells apart, counting every cell: a
// face with each pair of black, blue, red, magenta and green on a cell and
// 20 more blue on red. On sea with iCE on the four colours past black can't
// take four steps; putting blue and red on one step merges 22 cells, red
// and magenta 2.
TEST_CASE("a ramp Fill keeps the most TheDraw cells apart, counting each cell", "[style][tdf]") {
    const TextSource s = word(U"A");
    std::vector<std::pair<std::uint8_t, std::uint8_t>> extra(20, {Blue, Red});
    const auto face = pairFace({Black, Blue, Red, Magenta, Green}, extra);
    const Canvas src = plain(s, *face);
    REQUIRE(src.width() == 40);
    for (const FillMode mode : {FillMode::Rows, FillMode::Columns, FillMode::Rainbow}) {
        INFO(int(mode));
        const Canvas out = styled(s, styleOf({FillEffect{mode, Grey, "sea"}}), *face, true);
        // blue, red, magenta, green on steps 1 2 2 3: blue, cyan, cyan,
        // light cyan
        const std::map<std::uint8_t, std::uint8_t> want = {
            {Black, Black}, {Blue, Blue}, {Red, Cyan}, {Magenta, Cyan}, {Green, LightCyan}};
        int merged = 0;
        for (int x = 0; x < src.width(); ++x) {
            INFO("cell " << x);
            const Cell& a = src.at(x, 0);
            const Cell& b = out.at(x, 0);
            CHECK(b.fg != b.bg);
            if (want.at(a.fg.paletteIndex()) == want.at(a.bg.paletteIndex()))
                ++merged;
            else {
                CHECK(b.fg == Color::indexed(want.at(a.fg.paletteIndex())));
                CHECK(b.bg == Color::indexed(want.at(a.bg.paletteIndex())));
            }
        }
        CHECK(merged == 2);
    }
}

// ---- order, switches, iCE ---------------------------------------------------------

TEST_CASE("the effects draw in the fixed order whatever order the stack lists them in", "[style][order]") {
    const TextSource s = word(U"funk", "block");
    const TextStyle forward = everyEffect();
    TextStyle backward = forward;
    std::reverse(backward.effects.begin(), backward.effects.end());
    TextStyle shuffled = forward;
    std::rotate(shuffled.effects.begin(), shuffled.effects.begin() + 3, shuffled.effects.end());
    const Canvas a = styled(s, forward);
    CHECK(styled(s, backward) == a);
    CHECK(styled(s, shuffled) == a);
    CHECK(styled(s, forward, false) == styled(s, backward, false));

    // a later effect wins where two meet: the outline over the shadow, both
    // over the plate, the letters over all
    const Pixels ink = pixelsOf(plain(s));
    const auto inkAt = [&](int x, int y) { return ink.ink(x, y); };
    const auto ring = [&](int x, int y) {
        if (inkAt(x, y))
            return false;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (inkAt(x + dx, y + dy))
                    return true;
        return false;
    };
    const TextStyle layered = styleOf({OutlineEffect{OutlineForm::Pixels, Blue},
                                       ShadowEffect{ShadowForm::Drop, Compass::SE, 1, DarkGrey}, PlateEffect{Magenta, 0}});
    const StyleMargins m = styleMargins(layered);
    CHECK(m == StyleMargins{1, 1, 2, 2});
    std::string first;
    CHECK(mismatches(
              pixelsOf(styled(s, layered)),
              [&](int x, int y) -> int {
                  const int ix = x - m.left, iy = y - 2 * m.top;
                  if (inkAt(ix, iy))
                      return White;
                  if (ring(ix, iy))
                      return Blue;
                  if (inkAt(ix - 1, iy - 1) || ring(ix - 1, iy - 1))
                      return DarkGrey;
                  // the plate: the text's box with no padding
                  if (ix >= 0 && iy >= 0 && ix < ink.width && iy < ink.height)
                      return Magenta;
                  return kNone;
              },
              first) == 0);
    INFO(first);
}

TEST_CASE("an effect switched off draws nothing and adds no margin", "[style]") {
    const TextSource s = word(U"funk", "mini");
    TextStyle style = everyEffect();
    for (Effect& e : style.effects)
        e.enabled = false;
    CHECK(styleMargins(style) == StyleMargins{});
    CHECK(styled(s, style) == plain(s));
    style.effects[3].enabled = true;  // the outline
    CHECK(styled(s, style) == styled(s, styleOf({OutlineEffect{OutlineForm::Pixels, Cyan}})));
    CHECK(styled(s, TextStyle{}) == plain(s));
}

TEST_CASE("with iCE off no effect needs a bright background, and a letter on top keeps its colour",
          "[style][ice]") {
    const TextSource s = word(U"funk", "block");

    // the glow, fill and shade walk their rungs by stepRung with iCE off
    for (const TextStyle& style :
         {styleOf({GlowEffect{"fire", 12}}), styleOf({FillEffect{FillMode::Rows, Grey, "fire"}}),
          styleOf({FillEffect{FillMode::Columns, Grey, "fire"}, ShadeEffect{"fire", Compass::W}}),
          styleOf({FillEffect{FillMode::Flat, Yellow, "fire"}, BevelEffect{"fire", Compass::NW}}),
          styleOf({FillEffect{FillMode::Rows, Grey, "fire"}, BevelEffect{"fire", Compass::NW}})}) {
        INFO(effectKindName(style.effects.back().kind()));
        CHECK_FALSE(brightBackground(styled(s, style, false)));
    }
    CHECK(brightBackground(styled(s, styleOf({GlowEffect{"fire", 12}}), true)));
    CHECK(brightBackground(styled(s, styleOf({FillEffect{FillMode::Rows, Grey, "fire"}}), true)));

    // iCE off, the glow's rings still go dark outward, all on the ramp
    const Canvas glow = styled(s, styleOf({GlowEffect{"fire", 12}}), false);
    const Pixels ink = pixelsOf(plain(s));
    std::vector<int> byRing(13, -1);
    for (int y = 0; y < glow.height(); ++y)
        for (int x = 0; x < glow.width(); ++x) {
            int d = 99;
            for (int iy = 0; iy < ink.height / 2; ++iy)
                for (int ix = 0; ix < ink.width; ++ix)
                    if (ink.cellInk(ix, iy))
                        d = std::min(d, std::max(std::abs(x - 12 - ix), std::abs(y - 12 - iy)));
            if (d == 0 || d > 12)
                continue;
            const auto rung = rungOf(fire(), glow.at(x, y));
            REQUIRE(rung);
            byRing[static_cast<std::size_t>(d)] = *rung;
        }
    for (std::size_t d = 2; d <= 12; ++d)
        CHECK(byRing[d] <= byRing[d - 1]);

    // a bright plate under white letters: with iCE off the letter's half
    // keeps white and the plate's half under it shows dark blue
    const TextStyle plate = styleOf({PlateEffect{LightBlue, 1}, FillEffect{FillMode::Flat, White, "fire"}});
    const Canvas on = styled(s, plate, true), off = styled(s, plate, false);
    CHECK(brightBackground(on));
    CHECK_FALSE(brightBackground(off));
    int fringes = 0;
    for (int y = 0; y < ink.height / 2; ++y)
        for (int x = 0; x < ink.width; ++x) {
            if (!ink.cellInk(x, y) || ink.fullCell(x, y))
                continue;
            ++fringes;
            INFO(x << "," << y);
            const bool top = ink.ink(x, 2 * y);
            CHECK(halfBlocks(on.at(x + 1, y + 1), true) == (top ? HalfPair{White, LightBlue} : HalfPair{LightBlue, White}));
            CHECK(halfBlocks(off.at(x + 1, y + 1), false) == (top ? HalfPair{White, Blue} : HalfPair{Blue, White}));
        }
    CHECK(fringes > 0);
}

// Codex round 1: a ring or row drawn with iCE off is at most one rung from
// the one beside it, even where the ramp's next rungs need a bright
// background (fire's LightRed–Yellow step): the walk stops short of them.
TEST_CASE("with iCE off the glow and fill gradients skip no ladder rung", "[style][ice]") {
    // the rungs of each pair of neighbouring cells (8-connected) on the
    // ramp, other than `skip`, at most one apart
    auto skips = [](const Canvas& c, CellRect skip, int& pairs) {
        std::string first;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x) {
                if (c.at(x, y).transparent || skip.contains(x, y))
                    continue;
                const auto a = rungOf(fire(), c.at(x, y));
                if (!a)
                    continue;
                for (const auto [dx, dy] : {std::pair{1, 0}, {0, 1}, {1, 1}, {-1, 1}}) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny >= c.height() || nx >= c.width() || c.at(nx, ny).transparent ||
                        skip.contains(nx, ny))
                        continue;
                    const auto b = rungOf(fire(), c.at(nx, ny));
                    if (!b)
                        continue;
                    ++pairs;
                    if (std::abs(*a - *b) > 1 && first.empty())
                        first = std::to_string(x) + "," + std::to_string(y) + " rung " + std::to_string(*a) + " beside " +
                                std::to_string(nx) + "," + std::to_string(ny) + " rung " + std::to_string(*b);
                }
            }
        return first;
    };
    auto solid = [](int w, int h) {
        Layer l = makeLayer(LayerKind::Pixels, w, h, "solid");
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                l.cells.at(x, y) = setLayerPixel(setLayerPixel(l.cells.at(x, y), 0, White), 1, White);
        return l;
    };

    // Codex's repro: one white cell's glow, its rings 12 and 8 side by side
    const Layer dot = solid(1, 1);
    const TextStyle glow = styleOf({GlowEffect{"fire", 12}});
    const Canvas halo = drawStyle(styleInputOf(dot), glow, false, styleMargins(glow));
    REQUIRE(halo.width() == 25);
    int pairs = 0;
    CHECK(skips(halo, CellRect{12, 12, 1, 1}, pairs) == "");
    CHECK(pairs > 500);
    CHECK_FALSE(brightBackground(halo));
    // still a ring of each rung down to the ramp's darkest but one
    CHECK(rungOf(fire(), halo.at(0, 12)) == 1);

    // a fill down 13 rows and across 13 columns
    for (const FillMode mode : {FillMode::Rows, FillMode::Columns}) {
        INFO(int(mode));
        const Layer block = mode == FillMode::Rows ? solid(2, 13) : solid(13, 2);
        const Canvas filled = drawStyle(styleInputOf(block), styleOf({FillEffect{mode, Grey, "fire"}}), false);
        pairs = 0;
        CHECK(skips(filled, CellRect{}, pairs) == "");
        CHECK(pairs > 30);
        CHECK_FALSE(brightBackground(filled));
        // and with iCE on it climbs the whole ramp
        const Canvas on = drawStyle(styleInputOf(block), styleOf({FillEffect{mode, Grey, "fire"}}), true);
        CHECK(rungOf(fire(), on.at(0, 0)) == fire().topRung());
    }
}

TEST_CASE("a representative stack draws lint-clean with iCE on and off", "[style][lint]") {
    // a plate, an outline and a shadow round a ladder fill; and a flat fill
    // shaded and bevelled. (A ramp fill under Shade or Bevel stacks two
    // gradients, two rungs a cell, which the ladder lint rightly flags.)
    const TextStyle framed = styleOf({PlateEffect{Blue, 1}, OutlineEffect{OutlineForm::Pixels, Cyan},
                                      ShadowEffect{ShadowForm::Drop, Compass::SE, 1, DarkGrey},
                                      FillEffect{FillMode::Rows, Grey, "steel"}});
    const TextStyle carved = styleOf({FillEffect{FillMode::Flat, White, "steel"}, ShadeEffect{"steel", Compass::N},
                                      BevelEffect{"steel", Compass::NW}});
    const std::vector<std::pair<const char*, const TextStyle*>> cases = {
        {"block", &framed}, {"vga", &framed}, {"mini", &framed}, {"block", &carved}};
    for (const auto& [face, style] : cases)
        for (const bool ice : {true, false}) {
            INFO(face << " iCE " << ice);
            const Canvas out = styled(word(U"funk", face), *style, ice);
            lint::LadderOptions options;
            options.ice = ice;
            const lint::LadderReport r = lint::checkLadder(out, steel(), std::nullopt, options);
            for (const lint::LadderFinding& f : r.findings)
                UNSCOPED_INFO(f.x << "," << f.y << ": " << f.message);
            CHECK(r.clean());
            CHECK(r.ladderCells > 0);
        }
}

// ---- checks ---------------------------------------------------------------------

TEST_CASE("a stack that can't be drawn says why", "[style][refuse]") {
    CHECK_FALSE(textStyleProblem(TextStyle{}));
    CHECK_FALSE(textStyleProblem(everyEffect()));
    const auto problem = [](std::vector<EffectSettings> e) { return textStyleProblem(styleOf(std::move(e))).value_or(""); };
    CHECK(problem({PlateEffect{}, PlateEffect{}}).find("two plate effects") != std::string::npos);
    CHECK(problem({GlowEffect{"neon", 2}}).find("\"neon\" is not a named ramp") != std::string::npos);
    CHECK(problem({GlowEffect{"steel", 13}}).find("over ramp \"steel\"'s 12 rungs") != std::string::npos);
    CHECK(problem({GlowEffect{"fire", 0}}).find("glow width 0") != std::string::npos);
    CHECK(problem({PlateEffect{16, 1}}).find("plate colour 16") != std::string::npos);
    CHECK(problem({PlateEffect{Blue, TextStyle::kMaxPadding + 1}}).find("plate padding") != std::string::npos);
    CHECK(problem({ShadowEffect{ShadowForm::Drop, Compass::S, 0, Black}}).find("shadow distance 0") != std::string::npos);
    CHECK(problem({ShadowEffect{ShadowForm::Drop, Compass::S, TextStyle::kMaxDistance + 1, Black}}) != "");
    CHECK(problem({OutlineEffect{OutlineForm::Cells, 99}}) != "");
    CHECK(problem({FillEffect{FillMode::Flat, 16, "fire"}}) != "");
    CHECK(problem({FillEffect{FillMode::Flat, Grey, "neon"}}) == "");  // flat has no ramp
    CHECK(problem({FillEffect{FillMode::Rows, Grey, "neon"}}) != "");
    CHECK(problem({ShadeEffect{"neon", Compass::N}}) != "");
    CHECK(problem({BevelEffect{"neon", Compass::N}}) != "");
    // switched off is checked too
    TextStyle off = styleOf({GlowEffect{"neon", 2}});
    off.effects[0].enabled = false;
    CHECK(textStyleProblem(off));
    TextStyle many = everyEffect();
    many.effects.push_back(on(PlateEffect{}));
    CHECK(textStyleProblem(many).value_or("").find("8 effects, over 7") != std::string::npos);
}

TEST_CASE("a style goes only on a cells, pixels or live text layer, and a bad one changes nothing",
          "[style][refuse]") {
    Layer generator = makeLayer(LayerKind::Generator, 4, 2, "g");
    generator.props.style = everyEffect();
    CHECK(layerStyleProblem(generator.props).value_or("").find("generator layer has no style") != std::string::npos);
    Layer text = makeLayer(LayerKind::Text, 4, 2, "t");
    text.props.style = everyEffect();
    CHECK(layerStyleProblem(text.props).value_or("").find("without a text source") != std::string::npos);
    for (const LayerKind kind : {LayerKind::Cells, LayerKind::Pixels}) {
        LayerProperties p = makeLayer(kind, 2, 2).props;
        p.style = everyEffect();
        CHECK_FALSE(layerStyleProblem(p));
    }

    Layer live = makeTextLayer(word(U"funk"), "t");
    const Layer before = live;
    LayerProperties bad = live.props;
    bad.style = styleOf({PlateEffect{}, PlateEffect{}});
    CHECK_THROWS_AS(applyLayerProperties(live, bad), TextError);
    CHECK(live == before);
    LayerProperties onGenerator = makeLayer(LayerKind::Generator, 1, 1).props;
    onGenerator.style = everyEffect();
    Layer g = makeLayer(LayerKind::Generator, 1, 1);
    CHECK_THROWS_AS(applyLayerProperties(g, onGenerator), TextError);
    // an edit is checked when it is applied, as a text edit is
    Layer edited = before;
    CHECK_THROWS_AS(applyEdit(edited, styleEdit(before, styleOf({GlowEffect{"neon", 2}}))), TextError);
    CHECK(edited == before);
}

// ---- live text, undo, the compositor, Bake ---------------------------------------------

TEST_CASE("a live text layer draws its style into its cells, growing right and down", "[style][layer]") {
    const TextSource s = word(U"funk", "block");
    const TextStyle style = everyEffect();
    const Layer l = liveStyled(s, style, 2, 1);
    const StyleMargins m = styleMargins(style);
    const Canvas text = plain(s);
    CHECK(l.props.kind == LayerKind::Text);
    CHECK(l.props.x == 2);
    CHECK(l.props.y == 1);
    CHECK(l.width() == text.width() + m.left + m.right);
    CHECK(l.height() == text.height() + m.top + m.bottom);
    CHECK(l.cells == styled(s, style));
    CHECK_FALSE(styledAtComposite(l.props));
    // the compositor shows its cells as they are
    LayerStack stack(80, 25);
    stack.add(l);
    const Canvas c = composite(stack);
    for (int y = 0; y < l.height(); ++y)
        for (int x = 0; x < l.width(); ++x)
            if (!l.cells.at(x, y).transparent)
                CHECK(c.at(x + 2, y + 1) == l.cells.at(x, y));
}

TEST_CASE("a style change is one step that undoes and redoes", "[style][undo]") {
    LayerStack stack(80, 25);
    const TextSource s = word(U"funk", "block");
    const LayerId id = stack.add(makeTextLayer(s, "t", 3, 2));
    const Layer before = *stack.find(id);
    const TextStyle style = everyEffect();

    const LayerPropsEdit e = styleEdit(before, style);
    CHECK(e.before == before.props);
    CHECK(e.after.style == style);
    CHECK(e.cellsBefore == before.cells);
    const CellRect r = applyEdit(stack, e);
    const Layer after = *stack.find(id);
    CHECK(after.cells == styled(s, style));
    CHECK(after.props.style == style);
    CHECK(r == unite(before.bounds(), after.bounds(), stack.canvasRect()));

    applyEdit(stack, e, true);
    CHECK(*stack.find(id) == before);
    applyEdit(stack, e);
    CHECK(*stack.find(id) == after);

    // one setting changed is one step too
    TextStyle next = style;
    std::get<PlateEffect>(next.effects[0].settings).colour = Red;
    const LayerPropsEdit change = styleEdit(after, next);
    applyEdit(stack, change);
    CHECK(stack.find(id)->cells == styled(s, next));
    applyEdit(stack, change, true);
    CHECK(*stack.find(id) == after);

    // and dropping the style
    const LayerPropsEdit drop = styleEdit(after, std::nullopt);
    applyEdit(stack, drop);
    CHECK(stack.find(id)->cells == before.cells);
    CHECK_FALSE(stack.find(id)->props.style);
    applyEdit(stack, drop, true);
    CHECK(*stack.find(id) == after);

    // a step drawn with iCE off draws its colours so
    const LayerPropsEdit off = styleEdit(before, styleOf({GlowEffect{"fire", 12}}), false);
    Layer l = before;
    applyEdit(l, off);
    CHECK(l.cells == styled(s, styleOf({GlowEffect{"fire", 12}}), false));

    // a style step never merges with typing
    LayerPropsEdit typed = textEdit(before, word(U"funky", "block"), 7);
    const LayerPropsEdit styledTyping = styleEdit(before, style);
    CHECK_FALSE(mergeEdits(typed, styledTyping));
}

TEST_CASE("the compositor draws a cells or pixels layer's style, clipped to the layer", "[style][composite]") {
    LayerStack stack(40, 12);
    const LayerId cellsId = stack.add(blockLayer());
    const LayerId pixelsId = stack.add(pixelLayer());
    const Canvas unstyled = composite(stack);

    const TextStyle style = styleOf({OutlineEffect{OutlineForm::Cells, Blue}, FillEffect{FillMode::Flat, Yellow, "fire"}});
    for (const LayerId id : {cellsId, pixelsId}) {
        const LayerPropsEdit e = styleEdit(*stack.find(id), style);
        CHECK_FALSE(e.cellsBefore);
        const Layer before = *stack.find(id);
        applyEdit(stack, e);
        CHECK(stack.find(id)->cells == before.cells);  // the cells stay as drawn
        CHECK(styledAtComposite(stack.find(id)->props));
    }
    const Canvas c = composite(stack);
    CHECK(c != unstyled);
    for (const LayerId id : {cellsId, pixelsId}) {
        const Layer& l = *stack.find(id);
        const Canvas cells = styledCells(l, true);
        REQUIRE(cells.width() == l.width());
        REQUIRE(cells.height() == l.height());
        for (int y = 0; y < l.height(); ++y)
            for (int x = 0; x < l.width(); ++x) {
                INFO(int(id) << " " << x << "," << y);
                // the pixels layer lies on top of the cells layer
                if (id == cellsId && stack.find(pixelsId)->bounds().contains(x + l.props.x, y + l.props.y))
                    continue;
                if (!cells.at(x, y).transparent)
                    CHECK(c.at(x + l.props.x, y + l.props.y) == cells.at(x, y));
            }
    }
    // the cell outline of the block layer reaches its edges and no further
    const Layer& blocks = *stack.find(cellsId);
    CHECK(c.at(blocks.props.x, blocks.props.y).bg.paletteIndex() == Blue);
    CHECK(c.at(blocks.props.x - 1, blocks.props.y) == unstyled.at(blocks.props.x - 1, blocks.props.y));

    // one cell composited, and a rect recomposited, give the same cells
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            CHECK(compositeCell(stack, x, y) == c.at(x, y));
    Canvas partial = unstyled;
    recomposite(stack, partial, stack.canvasRect());
    CHECK(partial == c);

    // with iCE off the style draws with iCE off
    const Canvas off = composite(stack, CompositeOptions{false, Cell{}});
    const Canvas offCells = styledCells(blocks, false);
    CHECK(off.at(blocks.props.x + 1, blocks.props.y + 1) == offCells.at(1, 1));

    // an edit anywhere on a styled layer restyles all of it
    CellEdit edit;
    edit.changes.push_back(CellChange{0, 0, blocks.cells.at(0, 0), Cell{}});
    CHECK(dirtyRect(blocks, edit) == blocks.bounds());
    Layer unstyledBlocks = blocks;
    unstyledBlocks.props.style.reset();
    CHECK(dirtyRect(unstyledBlocks, edit) == CellRect{blocks.props.x, blocks.props.y, 1, 1});
}

// Codex round 1: a styled pixel layer still never covers a text cell and
// still ignores its mode, and a stack with nothing switched on composites as
// no style at all.
TEST_CASE("a styled pixel layer keeps text protection and ignores the mode; an empty stack draws nothing",
          "[style][composite]") {
    LayerStack stack(6, 4);
    Layer text = makeLayer(LayerKind::Text, 6, 4, "text");
    text.cells.at(2, 1) = Cell{U'A', Color::indexed(White), Color::indexed(Black), 0, false};
    stack.add(std::move(text));
    Layer red = makeLayer(LayerKind::Pixels, 6, 4, "red");
    red.cells.at(2, 1) = setLayerPixel(setLayerPixel(red.cells.at(2, 1), false, Red), true, Red);
    red.cells.at(3, 1) = setLayerPixel(setLayerPixel(red.cells.at(3, 1), false, Red), true, Red);
    const LayerId redId = stack.add(std::move(red));
    const Canvas unstyled = composite(stack);
    const Canvas unstyledOff = composite(stack, CompositeOptions{false, Cell{}});
    REQUIRE(unstyled.at(2, 1).ch == U'A');

    TextStyle disabled = everyEffect();
    for (Effect& e : disabled.effects)
        e.enabled = false;
    for (const TextStyle& none : {TextStyle{}, disabled}) {
        stack.find(redId)->props.style = none;
        CHECK_FALSE(styledAtComposite(stack.find(redId)->props));
        CHECK(composite(stack) == unstyled);
        CHECK(composite(stack, CompositeOptions{false, Cell{}}) == unstyledOff);
        CHECK(compositeCell(stack, 2, 1) == unstyled.at(2, 1));
        // Bake drops the style and keeps the cells
        const Layer baked = bake(*stack.find(redId), true);
        CHECK_FALSE(baked.props.style);
        CHECK(baked.cells == stack.find(redId)->cells);
    }

    // switched on, the style draws round the pixels but not over the text
    stack.find(redId)->props.style = styleOf({OutlineEffect{OutlineForm::Cells, Blue}, FillEffect{FillMode::Flat, Yellow, "fire"}});
    REQUIRE(styledAtComposite(stack.find(redId)->props));
    for (const LayerMode mode : {LayerMode::Normal, LayerMode::CharsOnly, LayerMode::ColoursOnly}) {
        INFO(int(mode));
        stack.find(redId)->props.mode = mode;
        const Canvas c = composite(stack);
        CHECK(c.at(2, 1) == unstyled.at(2, 1));
        CHECK(compositeCell(stack, 2, 1) == unstyled.at(2, 1));
        CHECK(halfBlocks(c.at(3, 1), true) == HalfPair{Yellow, Yellow});
        CHECK(c.at(4, 1).bg.paletteIndex() == Blue);
        // the mode changes nothing
        Layer normal = *stack.find(redId);
        normal.props.mode = LayerMode::Normal;
        LayerStack same(6, 4);
        same.add(stack[0]);
        same.add(normal);
        CHECK(c == composite(same));
    }
}

// Codex round 1: a cell at a time over two overlapping styled layers styles
// each once, not once a cell.
TEST_CASE("compositing a cell at a time over overlapping styled layers styles each layer once",
          "[style][composite][perf]") {
    LayerStack stack(160, 100);
    for (int i = 0; i < 2; ++i) {
        Layer l = makeLayer(LayerKind::Cells, 160, 100, "blocks");
        for (int y = 10; y < 90; y += 3)
            for (int x = 10 + i; x < 150; x += 4)
                l.cells.at(x, y) = Cell{kFullBlock, Color::indexed(Grey), Color::indexed(Black), 0, false};
        l.props.style = styleOf({OutlineEffect{OutlineForm::Cells, i ? Blue : Red}, FillEffect{FillMode::Flat, Yellow, "fire"}});
        stack.add(std::move(l));
    }
    long long start = canvasCellsCopied();
    const Canvas whole = composite(stack);
    const long long once = canvasCellsCopied() - start;

    start = canvasCellsCopied();
    int wrong = 0;
    for (int y = 0; y < stack.height(); ++y)
        for (int x = 0; x < stack.width(); ++x)
            wrong += compositeCell(stack, x, y) != whole.at(x, y);
    const long long perCell = canvasCellsCopied() - start;
    CHECK(wrong == 0);
    INFO("whole " << once << ", a cell at a time " << perCell);
    // styling both layers once, and keeping a copy of each
    CHECK(perCell <= 2 * once + 2 * 160 * 100);
}

TEST_CASE("a canvas's revision names its cells: kept by a copy or a move, new after a write", "[canvas]") {
    Canvas a(4, 3);
    const std::uint64_t r = a.revision();
    CHECK(r != 0);
    CHECK(a.revision() == r);
    CHECK(std::as_const(a).at(1, 1) == Cell{});  // a read keeps it
    CHECK(a.revision() == r);
    CHECK(Canvas(4, 3).revision() != r);  // the same cells may have another
    Canvas copy = a;
    CHECK(copy.revision() == r);
    copy.at(1, 1).ch = U'x';
    CHECK(copy.revision() != r);
    CHECK(a.revision() == r);
    Canvas assigned(1, 1);
    assigned = a;
    CHECK(assigned.revision() == r);
    const Canvas moved = std::move(assigned);
    CHECK(moved.revision() == r);
    a.resize(5, 3);
    CHECK(a.revision() != r);
}

// Codex round 2: a cell at a time over an unchanged styled layer checks the
// cache in constant time, not by comparing the whole layer each cell.
TEST_CASE("a warm cell-at-a-time scan of a styled layer compares no canvas", "[style][composite][perf]") {
    constexpr int W = 80, H = 2000;
    Layer l = makeLayer(LayerKind::Cells, W, H, "block");
    l.cells.at(40, 1000) = Cell{kFullBlock, Color::indexed(White), Color::indexed(Black), 0, false};
    l.props.style = styleOf({FillEffect{FillMode::Flat, Yellow, "fire"}});
    LayerStack stack(W, H);
    stack.add(std::move(l));
    compositeCell(stack, 0, 0);

    const long long compared = canvasCellsCompared(), copied = canvasCellsCopied();
    Canvas result(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            result.at(x, y) = compositeCell(stack, x, y);
    INFO("compared " << canvasCellsCompared() - compared << ", copied " << canvasCellsCopied() - copied);
    CHECK(canvasCellsCompared() - compared == 0);
    CHECK(canvasCellsCopied() - copied == 0);
    CHECK(result == composite(stack));

    // a change to the layer is still seen
    stack.find(stack[0].props.id)->cells.at(10, 10) = Cell{kFullBlock, Color::indexed(White), Color::indexed(Black), 0, false};
    CHECK(compositeCell(stack, 10, 10) == composite(stack).at(10, 10));
    CHECK(halfBlocks(compositeCell(stack, 10, 10), true) == HalfPair{Yellow, Yellow});
}

// Codex round 2: a styled pixel layer composites in the normal mode whatever
// its own; baked, it is a cells layer in the normal mode, so the picture
// stays as it was, and undo gives the mode back.
TEST_CASE("Bake of a styled pixel layer in an ignored mode keeps the picture; undo gives the mode back",
          "[style][bake][undo]") {
    for (const LayerMode mode : {LayerMode::CharsOnly, LayerMode::ColoursOnly})
        for (const bool ice : {true, false}) {
            INFO("mode " << int(mode) << " iCE " << ice);
            const CompositeOptions options{ice, Cell{}};
            LayerStack stack(1, 1);
            Layer base = makeLayer(LayerKind::Cells, 1, 1);
            base.cells.at(0, 0) = Cell{U'A', Color::indexed(Green), Color::indexed(Black), 0, false};
            stack.add(std::move(base));
            Layer pixels = makeLayer(LayerKind::Pixels, 1, 1);
            pixels.props.mode = mode;
            pixels.cells.at(0, 0) = setLayerPixel(setLayerPixel(pixels.cells.at(0, 0), false, White), true, White);
            pixels.props.style = styleOf({FillEffect{FillMode::Flat, Yellow, "fire"}});
            const LayerId id = stack.add(std::move(pixels));
            const Canvas before = composite(stack, options);
            const Layer live = *stack.find(id);

            // bake()
            const Layer baked = bake(live, ice);
            CHECK(baked.props.mode == LayerMode::Normal);
            LayerStack direct = stack;
            *direct.find(id) = baked;
            CHECK(composite(direct, options) == before);

            // bakeEdit, undone and redone
            const LayerPropsEdit e = bakeEdit(live, ice);
            applyEdit(stack, e);
            CHECK(*stack.find(id) == baked);
            CHECK(composite(stack, options) == before);
            applyEdit(stack, e, true);
            CHECK(stack.find(id)->props.mode == mode);
            CHECK(*stack.find(id) == live);
            CHECK(composite(stack, options) == before);
            applyEdit(stack, e);
            CHECK(composite(stack, options) == before);
        }
}

TEST_CASE("Bake keeps the styled cells and drops the style; undo gives the style back", "[style][bake][undo]") {
    LayerStack stack(80, 25);
    const LayerId textId = stack.add(liveStyled(word(U"funk", "block"), everyEffect(), 2, 1));
    const LayerId cellsId = stack.add(blockLayer(40, 10));
    {
        Layer& c = *stack.find(cellsId);
        LayerProperties p = c.props;
        p.style = everyEffect();
        applyLayerProperties(c, p);
    }
    const LayerId pixelsId = stack.add(pixelLayer());
    {
        Layer& c = *stack.find(pixelsId);
        LayerProperties p = c.props;
        p.style = styleOf({OutlineEffect{OutlineForm::Pixels, Blue}});
        // clear of the text: baked, it is a cells layer, which covers text
        // as a pixel layer doesn't
        p.x = 60;
        p.y = 15;
        applyLayerProperties(c, p);
    }
    for (const bool ice : {true, false}) {
        const Canvas before = composite(stack, CompositeOptions{ice, Cell{}});
        for (const LayerId id : {textId, cellsId, pixelsId}) {
            INFO(int(id) << " iCE " << ice);
            const Layer live = *stack.find(id);
            const LayerPropsEdit e = bakeEdit(live, ice);
            applyEdit(stack, e);
            const Layer baked = *stack.find(id);
            CHECK(baked.props.kind == LayerKind::Cells);
            CHECK_FALSE(baked.props.style);
            CHECK_FALSE(baked.props.text);
            // a live text layer's style is in its cells already
            CHECK(baked.cells == (styledAtComposite(live.props) ? styledCells(live, ice) : live.cells));
            CHECK(baked == bake(live, ice));
            CHECK(composite(stack, CompositeOptions{ice, Cell{}}) == before);
            applyEdit(stack, e, true);
            CHECK(*stack.find(id) == live);
            applyEdit(stack, e);
            CHECK(*stack.find(id) == baked);
            applyEdit(stack, e, true);
        }
    }
}

// ---- the project file ----------------------------------------------------------------

TEST_CASE("the stack is saved in the layer file and reopens to the same cells", "[style][project]") {
    Project p;
    p.layers = LayerStack(80, 25);
    TextStyle listed = everyEffect();
    std::reverse(listed.effects.begin(), listed.effects.end());  // kept in its order
    listed.effects[2].enabled = false;
    const LayerId textId = p.layers.add(liveStyled(word(U"funk", "block"), listed));

    Entries e = project::encode(p);
    CHECK(object(e.at("manifest.json")).value("version").toString() == "1.3");
    const QJsonObject layer = object(e.at(layerPath(*p.layers.find(textId))));
    const QJsonArray effects = layer.value("style").toObject().value("effects").toArray();
    REQUIRE(effects.size() == 7);
    CHECK(effects[0].toObject().value("kind").toString() == "bevel");
    CHECK(effects[0].toObject().value("light").toString() == "nw");
    CHECK(effects[2].toObject().value("kind").toString() == "fill");
    CHECK(effects[2].toObject().value("enabled").toBool() == false);
    CHECK(effects[2].toObject().value("mode").toString() == "rows");
    CHECK(effects[4].toObject().value("kind").toString() == "shadow");
    CHECK(effects[4].toObject().value("direction").toString() == "se");
    CHECK(effects[4].toObject().value("form").toString() == "drop");
    CHECK(effects[6].toObject().value("kind").toString() == "plate");
    CHECK(effects[6].toObject().value("padding").toInt() == 1);
    CHECK(project::decode(e) == p);

    // a 1.2 reader drops the style and shows the same cells
    QJsonObject dropped = layer;
    dropped.remove("style");
    e[layerPath(*p.layers.find(textId))] = json(dropped);
    const Project old = project::decode(e);
    CHECK(old.layers.find(textId)->cells == p.layers.find(textId)->cells);

    // a styled cells or pixels layer makes it 2.0
    const LayerId cellsId = p.layers.add(blockLayer(40, 10));
    p.layers.find(cellsId)->props.style = styleOf({OutlineEffect{OutlineForm::Cells, Blue}, ShadeEffect{"steel", Compass::N}});
    const LayerId pixelsId = p.layers.add(pixelLayer());
    p.layers.find(pixelsId)->props.style = styleOf({GlowEffect{"steel", 2}});
    const Entries v2 = project::encode(p);
    CHECK(object(v2.at("manifest.json")).value("version").toString() == "2.0");
    const Project back = project::decode(v2);
    CHECK(back == p);
    for (const bool ice : {true, false})
        CHECK(composite(back.layers, CompositeOptions{ice, Cell{}}) == composite(p.layers, CompositeOptions{ice, Cell{}}));

    const fs::path dir = scratch("roundtrip");
    project::writeZip(p, dir / "piece.bwp");
    CHECK(project::read(dir / "piece.bwp") == p);
    project::writeDirectory(p, dir / "piece");
    CHECK(project::read(dir / "piece") == p);
}

TEST_CASE("a reader refuses a malformed or misplaced stack with a clear error", "[style][project][refuse]") {
    Project p;
    p.layers = LayerStack(80, 25);
    const LayerId id = p.layers.add(liveStyled(word(U"funk", "block"), everyEffect()));
    const Entries good = project::encode(p);
    const std::string path = "layers/" + std::to_string(id) + ".json";

    const auto refuses = [&](auto edit) {
        Entries e = good;
        QJsonObject o = object(e.at(path));
        edit(o);
        e[path] = json(o);
        try {
            project::decode(e);
        } catch (const ProjectError& err) {
            return std::string(err.what());
        }
        return std::string("(read)");
    };
    const auto inEffects = [&](auto change) {
        return refuses([&](QJsonObject& o) {
            QJsonObject s = o.value("style").toObject();
            QJsonArray a = s.value("effects").toArray();
            change(a);
            s["effects"] = a;
            o["style"] = s;
        });
    };
    // the effect at `i` (everyEffect's order) changed
    const auto inEffect = [&](int i, auto change) {
        return inEffects([&](QJsonArray& a) {
            QJsonObject e = a[i].toObject();
            change(e);
            a[i] = e;
        });
    };
    const auto has = [](const std::string& what, const char* text) {
        INFO(what);
        return what.find(text) != std::string::npos;
    };
    CHECK(refuses([](QJsonObject&) {}) == "(read)");
    CHECK(has(inEffects([](QJsonArray& a) { a.append(a[0]); }), "over 7 effects"));
    // a long list is refused by count, a huge one by the layer file's size,
    // before either is read
    CHECK(has(inEffects([](QJsonArray& a) {
              const QJsonValue v = a[0];
              for (int i = 0; i < 500; ++i)
                  a.append(v);
          }),
          "over 7 effects"));
    CHECK(has(inEffects([](QJsonArray& a) {
              const QJsonValue v = a[0];
              for (int i = 0; i < 100000; ++i)
                  a.append(v);
          }),
          " KiB"));
    CHECK(has(inEffects([](QJsonArray& a) { a[1] = a[0]; }), "two plate effects"));
    CHECK(has(inEffects([](QJsonArray& a) { a[0] = 3; }), "effect 1 is not an object"));
    CHECK(has(inEffect(0, [](QJsonObject& e) { e["kind"] = "sparkle"; }), "unknown kind \"sparkle\""));
    CHECK(has(inEffect(0, [](QJsonObject& e) { e.remove("enabled"); }), "has no \"enabled\""));
    CHECK(has(inEffect(0, [](QJsonObject& e) { e["padding"] = 17; }), "\"padding\" is not a whole number from 0 to 16"));
    CHECK(has(inEffect(0, [](QJsonObject& e) { e["colour"] = 16; }), "\"colour\""));
    CHECK(has(inEffect(0, [](QJsonObject& e) { e["colour"] = 1.5; }), "\"colour\""));
    CHECK(has(inEffect(1, [](QJsonObject& e) { e["ramp"] = "neon"; }), "\"neon\" is not a named ramp"));
    CHECK(has(inEffect(1, [](QJsonObject& e) { e["width"] = 13; }), "over ramp \"steel\"'s 12 rungs"));
    CHECK(has(inEffect(1, [](QJsonObject& e) { e["width"] = 0; }), "\"width\""));
    CHECK(has(inEffect(2, [](QJsonObject& e) { e["direction"] = "north"; }), "unknown direction \"north\""));
    CHECK(has(inEffect(2, [](QJsonObject& e) { e["form"] = "soft"; }), "unknown form \"soft\""));
    CHECK(has(inEffect(2, [](QJsonObject& e) { e["distance"] = 17; }), "\"distance\""));
    CHECK(has(inEffect(3, [](QJsonObject& e) { e["form"] = "dotted"; }), "unknown form \"dotted\""));
    CHECK(has(inEffect(4, [](QJsonObject& e) { e["mode"] = "gradient"; }), "unknown mode \"gradient\""));
    CHECK(has(inEffect(5, [](QJsonObject& e) { e["light"] = 3; }), "\"light\" is not a string"));
    CHECK(has(inEffect(6, [](QJsonObject& e) { e.remove("ramp"); }), "has no \"ramp\""));
    CHECK(has(refuses([](QJsonObject& o) { o["style"] = "fancy"; }), "\"style\" is not an object"));
    CHECK(has(refuses([](QJsonObject& o) { o["style"] = QJsonObject{{"effects", 1}}; }), "\"effects\" is not a list"));
    // misplaced: a text layer without a source, a generator
    CHECK(has(refuses([](QJsonObject& o) { o.remove("source"); }), "without a text source has no style"));
    CHECK(has(refuses([](QJsonObject& o) {
              o.remove("source");
              o["kind"] = "generator";
          }),
          "generator layer has no style"));
    // a style the compositor draws in a 1.x project
    CHECK(has(refuses([](QJsonObject& o) {
              o.remove("source");
              o["kind"] = "cells";
          }),
          "a styled cells layer in a 1.x project"));
    // a 3.x project: refused before any layer is read
    Entries newer = good;
    QJsonObject m = object(newer.at("manifest.json"));
    m["version"] = "3.0";
    newer["manifest.json"] = json(m);
    CHECK_THROWS_AS(project::decode(newer), ProjectError);
}

TEST_CASE("a writer refuses a stack the reader refuses", "[style][project][refuse]") {
    const auto writes = [](Layer l) {
        Project p;
        p.layers = LayerStack(80, 25);
        p.layers.add(std::move(l));
        try {
            project::encode(p);
        } catch (const ProjectError& e) {
            return std::string(e.what());
        }
        return std::string("(written)");
    };
    Layer generator = makeLayer(LayerKind::Generator, 3, 1, "g");
    generator.props.style = everyEffect();
    CHECK(writes(generator).find("generator layer has no style") != std::string::npos);
    Layer cells = blockLayer();
    cells.props.style = styleOf({PlateEffect{}, PlateEffect{}});
    CHECK(writes(cells).find("two plate effects") != std::string::npos);
    cells.props.style = styleOf({GlowEffect{"neon", 1}});
    CHECK(writes(cells).find("\"neon\"") != std::string::npos);
    cells.props.style = everyEffect();
    CHECK(writes(cells) == "(written)");
}

// A dithered cell a style recolours isn't the dither's any more: the styled
// cells (and so Bake) keep its mark only where it looks as it did (#160).
TEST_CASE("Style: a styled cell keeps the dither mark only while it looks as its source did", "[style][dither]") {
    const Ramp steel = *findRamp("steel");
    Layer l = makeLayer(LayerKind::Cells, 3, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            l.cells.at(x, y) = rungCell(steel, 4);
    Cell marked = rungCell(steel, 5);
    marked.dithered = true;
    l.cells.at(1, 1) = marked;
    const auto speckAt = [&](const Canvas& c) {
        const lint::LadderReport r = lint::checkLadder(c, steel);
        return std::any_of(r.findings.begin(), r.findings.end(), [](const lint::LadderFinding& f) {
            return f.kind == lint::LadderFinding::Kind::Speckle && f.x == 1 && f.y == 1;
        });
    };
    REQUIRE_FALSE(speckAt(l.cells));

    SECTION("a flat fill") {
        l.props.style = styleOf({FillEffect{FillMode::Flat, White, "steel"}});
        const LayerPropsEdit e = bakeEdit(l, true);
        REQUIRE(e.cellsAfter);
        const Canvas& out = *e.cellsAfter;
        CHECK(rungOf(steel, out.at(1, 1)) == 11);
        CHECK(rungOf(steel, out.at(0, 1)) == 12);
        CHECK_FALSE(out.at(1, 1).dithered);
        CHECK(speckAt(out));
        CHECK_FALSE(bake(l, true).cells.at(1, 1).dithered);
    }
    SECTION("a ramp's recolouring") {
        l.props.style = styleOf({FillEffect{FillMode::Rows, Grey, "fire"}});
        const Canvas out = styledCells(l, true);
        REQUIRE_FALSE(out.at(1, 1) == marked);
        CHECK_FALSE(out.at(1, 1).dithered);
        CHECK_FALSE(bake(l, true).cells.at(1, 1).dithered);
    }
    SECTION("a style that leaves the cell as it was keeps the mark") {
        // steel's own ramp maps the colours of its top two steps to
        // themselves (a ramp Fill ends a TheDraw face's colours at the top,
        // #184): the same cells a step up, grey with a white rung
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                l.cells.at(x, y) = rungCell(steel, 8);
        marked = rungCell(steel, 9);
        marked.dithered = true;
        l.cells.at(1, 1) = marked;
        REQUIRE_FALSE(speckAt(l.cells));
        l.props.style = styleOf({FillEffect{FillMode::Rows, Grey, "steel"}});
        const Canvas out = styledCells(l, true);
        REQUIRE(out.at(1, 1) == marked);
        CHECK(out.at(1, 1).dithered);
        CHECK_FALSE(speckAt(out));
    }
}
