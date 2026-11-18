// Palette swaps and colour remaps (#497, PLAN §8, Palettes): the presets,
// XBIN's 6-bit rounding, the map parser, remap and fit, palette files, and
// the project manifest's palette.

#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/mirc.h"
#include "blockwright/palette_swap.h"
#include "blockwright/project.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <cstdio>
#include <cstdlib>
#include <QJsonDocument>
#include <QJsonObject>

using namespace bw;

namespace {

Rgb rgbOf(std::uint32_t v) { return {std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)}; }

Palette reversed(const Palette& p) {
    Palette r{};
    for (std::size_t i = 0; i < 16; ++i)
        r[i] = p[15 - i];
    return r;
}

Cell cell(std::uint8_t fg, std::uint8_t bg, char32_t ch = U'A') {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

std::string xbinFixture() { return bwtest::readFile(bwtest::dataDir() / "xbin" / "tux-palette-font.xb"); }

} // namespace

TEST_CASE("the palette presets: the standard one first, names in any case, the aliases", "[palette]") {
    const auto presets = palettePresets();
    REQUIRE(presets.size() >= 6);
    CHECK(presets[0].name == "cga");
    CHECK(presets[0].colours == kPalette);
    for (const char* alias : {"cga", "CGA", "standard", "vga", "EGA"}) {
        INFO(alias);
        REQUIRE(palettePreset(alias));
        CHECK(palettePreset(alias)->colours == kPalette);
    }
    CHECK_FALSE(palettePreset("nope"));
    CHECK_FALSE(palettePreset(""));
    for (const PalettePreset& p : presets) {
        INFO(p.name);
        CHECK(palettePreset(p.name) == &p);
        CHECK_FALSE(p.title.empty());
    }
    // the brown fix: CGA's colour 6 without it is dark yellow
    CHECK(palettePreset("cga-yellow")->colours[6] == Rgb{0xaa, 0xaa, 0x00});
    // xterm's defaults, in CGA order: 1 is blue, 4 red
    CHECK(palettePreset("xterm")->colours[1] == Rgb{0x00, 0x00, 0xee});
    CHECK(palettePreset("xterm")->colours[4] == Rgb{0xcd, 0x00, 0x00});
}

TEST_CASE("the mIRC preset is what an IRC client shows each indexed colour as", "[palette]") {
    const Palette& m = palettePreset("mirc")->colours;
    for (std::uint8_t i = 0; i < 16; ++i) {
        INFO(int(i));
        CHECK(m[i] == rgbOf(irc::kMircRgb[irc::mircColor(Color::indexed(i))]));
    }
}

TEST_CASE("xbinColour rounds each channel to 6 bits and widens it as the reader does", "[palette]") {
    CHECK(xbinColour({0, 0, 0}) == Rgb{0, 0, 0});
    CHECK(xbinColour({255, 255, 255}) == Rgb{255, 255, 255});
    for (const Rgb& c : kPalette)  // the standard colours are 6-bit ones
        CHECK(xbinColour(c) == c);
    // 0x80 is 32.1 of 63: 32, widened 0x82
    CHECK(xbinColour({0x80, 0x80, 0x80}) == Rgb{0x82, 0x82, 0x82});
    for (int v = 0; v < 256; ++v) {
        const Rgb r = xbinColour({std::uint8_t(v), 0, 0});
        // a fixed point, and within half a step of what it was given
        CHECK(xbinColour(r) == r);
        CHECK(std::abs(int(r.r) - v) <= 3);
    }
}

TEST_CASE("xbinRoundedEntries counts the entries an XBIN writes as another colour", "[palette][xbin]") {
    CHECK(xbinRoundedEntries(kPalette) == 0);
    Palette p = kPalette;
    p[3] = {1, 2, 3};
    p[9] = {0x80, 0, 0};
    CHECK(xbinRoundedEntries(p) == 2);
    p[3] = xbinColour(p[3]);
    CHECK(xbinRoundedEntries(p) == 1);
    for (const PalettePreset& pre : palettePresets()) {
        Palette x{};
        for (std::size_t i = 0; i < 16; ++i)
            x[i] = xbinColour(pre.colours[i]);
        CHECK(xbinRoundedEntries(x) == 0);
    }
}

TEST_CASE("writeXbin counts a palette it rounds, and writes a 6-bit one back exactly", "[palette][xbin]") {
    AnsiDocument d = parseXbin(xbinFixture());
    REQUIRE(customPalette(d));
    AnsiOutput out = writeXbin(d);
    CHECK(out.paletteRounded == 0);
    CHECK_FALSE(out.lossy());
    CHECK(parseXbin(out.bytes).palette == d.palette);

    d.palette = palettePreset("xterm")->colours;  // 0xcd, 0xee and 0x7f aren't 6-bit
    out = writeXbin(d);
    CHECK(out.paletteRounded == xbinRoundedEntries(*d.palette));
    CHECK(out.paletteRounded > 0);
    CHECK(out.lossy());
    // what it reads back as is each channel's top 6 bits
    const Palette back = *parseXbin(out.bytes).palette;
    for (std::size_t i = 0; i < 16; ++i) {
        INFO(i);
        CHECK(back[i].r == std::uint8_t(((*d.palette)[i].r >> 2) << 2 | ((*d.palette)[i].r >> 2) >> 4));
    }
}

TEST_CASE("parseColourMap reads pairs and sixteen hex digits, and refuses anything else", "[palette][remap]") {
    std::string error;
    auto m = parseColourMap("3=11,5=13", &error);
    REQUIRE(m);
    ColourMap want = identityColourMap();
    want[3] = 11;
    want[5] = 13;
    CHECK(*m == want);
    m = parseColourMap(" 3 = 11 , 11=3 ");
    REQUIRE(m);
    CHECK(*m == swapColourMap(3, 11));
    m = parseColourMap("0123456789abcdef");
    REQUIRE(m);
    CHECK(*m == identityColourMap());
    m = parseColourMap("fedcba9876543210");
    REQUIRE(m);
    CHECK((*m)[0] == 15);
    CHECK((*m)[15] == 0);
    CHECK((*parseColourMap("0123456789ABCDEF")) == identityColourMap());

    for (const char* bad : {"", "3", "3=", "=3", "3=16", "16=3", "-1=2", "3=11,", "3=11,3=12", "0123456789abcdeg",
                            "0123456789abcde", "a=b"}) {
        INFO(bad);
        error.clear();
        CHECK_FALSE(parseColourMap(bad, &error));
        CHECK_FALSE(error.empty());
    }
    CHECK(colourMapText(identityColourMap()).empty());
    CHECK(colourMapText(swapColourMap(3, 11)) == "3=11,11=3");
    CHECK(*parseColourMap(colourMapText(swapColourMap(0, 15))) == swapColourMap(0, 15));
}

TEST_CASE("remapEdit maps indexed colours, leaves RGB and transparent cells, and keeps to its area",
          "[palette][remap]") {
    Canvas cv(4, 2);
    cv.at(0, 0) = cell(3, 1);
    cv.at(1, 0) = cell(11, 3, U'█');
    Cell rgb = cell(3, 0);
    rgb.fg = Color::fromRgb({1, 2, 3});
    cv.at(2, 0) = rgb;
    Cell hole = cell(3, 3);
    hole.transparent = true;
    cv.at(3, 0) = hole;
    cv.at(0, 1) = cell(3, 3);
    const ColourMap swap = swapColourMap(3, 11);

    const CellEdit all = remapEdit(cv, swap);
    Canvas after = cv;
    all.apply(after);
    CHECK(after.at(0, 0).fg == Color::indexed(11));
    CHECK(after.at(0, 0).bg == Color::indexed(1));
    CHECK(after.at(0, 0).ch == cv.at(0, 0).ch);
    CHECK(after.at(1, 0).fg == Color::indexed(3));
    CHECK(after.at(1, 0).bg == Color::indexed(11));
    CHECK(after.at(2, 0).fg == Color::fromRgb({1, 2, 3}));  // RGB stays
    CHECK(after.at(2, 0).bg == Color::indexed(0));
    CHECK(identical(after.at(3, 0), hole));  // transparent stays
    CHECK(after.at(0, 1).fg == Color::indexed(11));
    // nor are the blank cells (grey on black), nor the RGB cell's black
    CHECK(all.changes.size() == 3);
    all.revert(after);
    CHECK(after == cv);

    // a pixel layer's half pixel is in a transparent cell's foreground
    const CellEdit pixels = remapEdit(cv, swap, std::nullopt, true);
    CHECK(pixels.changes.size() == 4);

    const CellEdit area = remapEdit(cv, swap, CellRect{0, 1, 4, 1});
    REQUIRE(area.changes.size() == 1);
    CHECK(area.changes[0].x == 0);
    CHECK(area.changes[0].y == 1);
    // an area past the canvas is cut to it
    CHECK(remapEdit(cv, swap, CellRect{-5, -5, 100, 100}).changes.size() == 3);

    CHECK(remapEdit(cv, identityColourMap()).empty());
    Canvas c2 = cv, expect = cv;
    all.apply(expect);
    CHECK(remapCanvas(c2, swap) == 3);
    CHECK(c2 == expect);
}

TEST_CASE("fitting to a palette keeps the look where the new one has the colours", "[palette][fit]") {
    Canvas cv(16, 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            cv.at(x, y) = cell(std::uint8_t(x), std::uint8_t(y));

    // to itself: nothing moves, nothing lost
    CHECK(fitColourMap(kPalette, kPalette) == identityColourMap());
    CHECK(fitLoss(cv, kPalette, kPalette, identityColourMap()) == 0);

    // a reversed palette holds every colour: each index goes to its mirror and
    // every cell looks as it did
    const Palette rev = reversed(kPalette);
    const ColourMap m = fitColourMap(kPalette, rev);
    for (std::size_t i = 0; i < 16; ++i)
        CHECK(m[i] == 15 - i);
    CHECK(fitLoss(cv, kPalette, rev, m) == 0);
    Canvas fitted = cv;
    remapCanvas(fitted, m);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const Cell& a = cv.at(x, y);
            const Cell& b = fitted.at(x, y);
            CHECK(rev[b.fg.index] == kPalette[a.fg.index]);
            CHECK(rev[b.bg.index] == kPalette[a.bg.index]);
        }

    // a palette missing one colour: the cells showing it are counted
    Palette near = kPalette;
    near[14] = {0xfe, 0xfe, 0x50};  // yellow, nearly
    const ColourMap mn = fitColourMap(kPalette, near);
    CHECK(mn[14] == 14);
    // a cell is lost when its foreground or background is 14: row 14 and column 14
    CHECK(fitLoss(cv, kPalette, near, mn) == 31);
    Cell hole = cell(14, 14);
    hole.transparent = true;
    cv.at(14, 14) = hole;
    CHECK(fitLoss(cv, kPalette, near, mn) == 30);
}

TEST_CASE("remapStyle maps the colours a style names and leaves its ramps", "[palette][remap]") {
    const ColourMap m = swapColourMap(1, 14);
    ShadowEffect shadow;
    shadow.colour = 1;
    FillEffect fill;
    fill.colour = 1;
    const TextStyle style{{Effect{true, PlateEffect{Blue, 1}}, Effect{true, GlowEffect{}}, Effect{false, shadow},
                           Effect{true, OutlineEffect{OutlineForm::Pixels, 1}}, Effect{true, fill},
                           Effect{true, ShadeEffect{}}}};
    const TextStyle out = remapStyle(style, m);
    CHECK(std::get<PlateEffect>(out.effects[0].settings).colour == 14);
    CHECK(out.effects[1] == style.effects[1]);  // a glow is a ramp's
    CHECK(std::get<ShadowEffect>(out.effects[2].settings).colour == 14);  // off, mapped all the same
    CHECK_FALSE(out.effects[2].enabled);
    CHECK(std::get<OutlineEffect>(out.effects[3].settings).colour == 14);
    CHECK(std::get<FillEffect>(out.effects[4].settings).colour == 14);
    CHECK(std::get<FillEffect>(out.effects[4].settings).ramp == "fire");
    CHECK(out.effects[5] == style.effects[5]);

    // remapStyles: the layers remapLayers maps whose style the map changes
    LayerStack stack(4, 2);
    Layer plated;
    plated.cells = Canvas(4, 2);
    plated.props.style = TextStyle{{Effect{true, PlateEffect{Blue, 1}}}};
    const LayerId a = stack.add(std::move(plated));
    Layer glowing;
    glowing.cells = Canvas(4, 2);
    glowing.props.style = TextStyle{{Effect{true, GlowEffect{}}}};
    stack.add(std::move(glowing));
    Layer plain;
    plain.cells = Canvas(4, 2);
    stack.add(std::move(plain));
    const auto styles = remapStyles(stack, m);
    REQUIRE(styles.size() == 1);
    CHECK(styles[0].first == a);
    CHECK(std::get<PlateEffect>(styles[0].second.effects[0].settings).colour == 14);
}

TEST_CASE("lookChanged counts the cells that show another colour", "[palette][fit]") {
    Canvas before(3, 1);
    before.at(0, 0).fg = Color::indexed(1);
    before.at(1, 0).bg = Color::indexed(4);
    before.at(2, 0).transparent = true;
    Palette rev{};
    for (std::size_t i = 0; i < 16; ++i)
        rev[i] = kPalette[15 - i];
    // the same indices in another palette: the two opaque cells change
    Canvas same = before;
    CHECK(lookChanged(before, kPalette, same, kPalette) == 0);
    CHECK(lookChanged(before, kPalette, same, rev) == 2);
    // mapped to the entries that hold their colours: no change
    Canvas mapped = before;
    remapCanvas(mapped, fitColourMap(kPalette, rev));
    CHECK(lookChanged(before, kPalette, mapped, rev) == 0);
    // a space's foreground doesn't show, but for an underline
    Canvas space(1, 1);
    space.at(0, 0).fg = Color::indexed(1);
    Canvas spaceAfter = space;
    spaceAfter.at(0, 0).bg = Color::indexed(15);  // rev[15] is black
    CHECK(lookChanged(space, kPalette, spaceAfter, rev) == 0);
    space.at(0, 0).attrs = attr::Underline;
    spaceAfter.at(0, 0).attrs = attr::Underline;
    CHECK(lookChanged(space, kPalette, spaceAfter, rev) == 1);
    // a space and a full block in the colour it showed look the same
    Canvas block = space;
    block.at(0, 0) = Cell{};
    block.at(0, 0).ch = U'\u2588';
    block.at(0, 0).fg = Color::indexed(15);  // rev[15] is the standard black
    space.at(0, 0).attrs = 0;
    CHECK(lookChanged(space, kPalette, block, rev) == 0);
    block.at(0, 0).fg = Color::indexed(14);
    CHECK(lookChanged(space, kPalette, block, rev) == 1);
    // a cell transparent in one and not the other changes
    mapped.at(2, 0).transparent = false;
    CHECK(lookChanged(before, kPalette, mapped, rev) == 1);
    // an RGB cell shows its own colour in any palette
    Canvas rgb(1, 1);
    rgb.at(0, 0).fg = Color::fromRgb(kPalette[1]);
    rgb.at(0, 0).bg = Color::fromRgb(kPalette[4]);
    CHECK(lookChanged(rgb, kPalette, rgb, rev) == 0);
}

TEST_CASE("readPaletteFile reads GIMP, JASC, hex and XBIN palettes", "[palette][file]") {
    std::string gpl = "GIMP Palette\nName: test\nColumns: 8\n# a comment\n";
    std::string jasc = "JASC-PAL\r\n0100\r\n16\r\n";
    std::string hex;
    for (const Rgb& c : reversed(kPalette)) {
        gpl += std::to_string(c.r) + " " + std::to_string(c.g) + "\t" + std::to_string(c.b) + "  name\n";
        jasc += std::to_string(c.r) + " " + std::to_string(c.g) + " " + std::to_string(c.b) + "\r\n";
        char buf[8];
        std::snprintf(buf, sizeof buf, "%02x%02X%02x", c.r, c.g, c.b);
        hex += (hex.size() % 2 ? "#" : "") + std::string(buf) + "\n\n";
    }
    std::string error;
    int n = 0;
    auto p = readPaletteFile("p.gpl", gpl, &error, &n);
    REQUIRE(p);
    CHECK(*p == reversed(kPalette));
    CHECK(n == 16);
    p = readPaletteFile("p.pal", jasc, &error);
    REQUIRE(p);
    CHECK(*p == reversed(kPalette));
    p = readPaletteFile("p.hex", hex, &error);
    REQUIRE(p);
    CHECK(*p == reversed(kPalette));

    // more than 16: the first 16, and how many it had
    p = readPaletteFile("p.hex", hex + "123456\n", &error, &n);
    REQUIRE(p);
    CHECK(n == 17);
    CHECK(*p == reversed(kPalette));

    // an XBIN's own, and the standard one for an XBIN without one
    const std::string xb = xbinFixture();
    p = readPaletteFile("t.xb", xb);
    REQUIRE(p);
    CHECK(*p == *parseXbin(xb).palette);
    AnsiDocument plain = parseXbin(xb);
    plain.palette.reset();
    p = readPaletteFile("plain.xb", writeXbin(plain).bytes);
    REQUIRE(p);
    CHECK(*p == kPalette);

    for (const std::string& bad : {std::string("ff0000\n00ff00\n"), std::string("GIMP Palette\n1 2\n"),
                                   std::string("hello\n"), std::string(), std::string("JASC-PAL\n0100\n")}) {
        INFO(bad);
        error.clear();
        CHECK_FALSE(readPaletteFile("x.txt", bad, &error));
        CHECK_FALSE(error.empty());
    }
    readPaletteFile("x.hex", "ff0000\n00ff00\n", &error);
    CHECK(error == "the palette has 2 colours; it needs 16");
}

TEST_CASE("a project keeps a palette: the manifest's list, and cga for the standard one", "[palette][project]") {
    AnsiDocument d = parseXbin(xbinFixture());
    project::Project p = project::fromAnsi(d);
    REQUIRE(p.palette);
    CHECK(*p.palette == *d.palette);
    project::Entries e = project::encode(p);
    const QJsonObject m = QJsonDocument::fromJson(QByteArray::fromStdString(e.at("manifest.json"))).object();
    REQUIRE(m.value("palette").isArray());
    CHECK(m.value("palette").toArray().size() == 16);
    CHECK(m.value("palette").toArray()[0].toString().size() == 7);
    const project::Project back = project::decode(e);
    CHECK(back.palette == p.palette);
    // flattened, it is the document's palette again, which an XBIN writes
    const AnsiDocument flat = project::toAnsi(back);
    CHECK(flat.palette == d.palette);
    CHECK(parseXbin(writeXbin(flat).bytes).palette == d.palette);

    // the standard palette is "cga", as before
    p.palette.reset();
    e = project::encode(p);
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(e.at("manifest.json"))).object().value("palette") ==
          QJsonValue(QStringLiteral("cga")));
    CHECK_FALSE(project::decode(e).palette);
    p.palette = kPalette;
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(project::encode(p).at("manifest.json")))
              .object()
              .value("palette") == QJsonValue(QStringLiteral("cga")));
}

TEST_CASE("a project's palette that isn't cga or 16 #rrggbb colours is refused", "[palette][project]") {
    project::Project p = project::fromAnsi(parseXbin(xbinFixture()));
    const project::Entries good = project::encode(p);
    const auto with = [&](const QJsonValue& palette) {
        project::Entries e = good;
        QJsonObject m = QJsonDocument::fromJson(QByteArray::fromStdString(e.at("manifest.json"))).object();
        m.insert("palette", palette);
        e["manifest.json"] = QJsonDocument(m).toJson().toStdString();
        return e;
    };
    QJsonArray list = QJsonDocument::fromJson(QByteArray::fromStdString(good.at("manifest.json")))
                          .object()
                          .value("palette")
                          .toArray();
    CHECK(project::decode(with(list)).palette == p.palette);
    QJsonArray upper = list;
    upper[0] = QStringLiteral("#AABBCC");
    CHECK((*project::decode(with(upper)).palette)[0] == Rgb{0xaa, 0xbb, 0xcc});
    QJsonArray shorter = list;
    shorter.removeLast();
    QJsonArray bad = list;
    bad[3] = QStringLiteral("#12345g");
    QJsonArray sign = list;
    sign[3] = QStringLiteral("#+12345");
    QJsonArray number = list;
    number[3] = 7;
    for (const QJsonValue& v : {QJsonValue(QStringLiteral("vga")), QJsonValue(shorter), QJsonValue(bad),
                                QJsonValue(sign), QJsonValue(number), QJsonValue(3)}) {
        INFO(QJsonDocument(QJsonArray{v}).toJson().toStdString());
        CHECK_THROWS_AS(project::decode(with(v)), project::ProjectError);
    }
}
