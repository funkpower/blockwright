// Reference layers in the core (PLAN §5, §6; #178): the picture's bytes,
// place, size and opacity round-trip through a project, the file under
// refs/ is checked as a reader checks it, and nothing that exports the art
// sees the layer. coverageAbove gives what a reference is drawn under.

#include "blockwright/composite.h"
#include "blockwright/irc.h"
#include "blockwright/project.h"
#include "blockwright/style_draw.h"
#include "blockwright/text_style.h"
#include "blockwright/text.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>
#include <memory>
#include <string>

using namespace bw;
using bw::project::Entries;
using bw::project::Project;
using bw::project::ProjectError;
namespace fs = std::filesystem;

namespace {

// Not decodable pictures, only their signatures: the core never decodes one.
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "a red square";
const std::string kJpg = std::string("\xff\xd8\xff\xe0", 4) + "a blue square";

std::string sha(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex().toStdString();
}

Layer referenceLayer(const std::string& bytes, const std::string& name, int x, int y, int w, int h, int opacity) {
    Layer l = makeLayer(LayerKind::Reference, 0, 0, name);
    l.props.reference = ReferenceImage{std::make_shared<const std::string>(bytes), x, y, w, h, opacity};
    return l;
}

// Art on the bottom, a reference over it and one under a second art layer,
// a third sharing the first one's picture, hidden.
Project withReferences() {
    Project p;
    Canvas field(6, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x)
            field.at(x, y) = Cell{U'░', Color::indexed(7), Color::indexed(1), 0, false};
    p.layers = LayerStack::fromCanvas(field);
    p.layers.add(referenceLayer(kPng, "photo", -13, 7, 640, 480, 50));
    Layer art = makeLayer(LayerKind::Cells, 2, 1, "eye", 2, 1);
    art.cells.at(0, 0) = Cell{U'@', Color::indexed(14), Color::indexed(0), 0, false};
    p.layers.add(std::move(art));
    p.layers.add(referenceLayer(kJpg, "sketch", 0, 0, 48, 20, 100), 1);
    Layer copy = referenceLayer(kPng, "photo again", 5, 5, 320, 100, 25);
    copy.props.visible = false;
    p.layers.add(std::move(copy));
    return p;
}

QJsonObject object(const std::string& text) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
}

std::string json(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString(); }

std::string layerJsonPath(const Project& p, std::size_t index) {
    return "layers/" + std::to_string(p.layers[index].props.id) + ".json";
}

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "reference" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("a reference image's type is read from its signature", "[layers][reference]") {
    CHECK(referenceImageType(kPng) == "png");
    CHECK(referenceImageType(kJpg) == "jpg");
    CHECK_FALSE(referenceImageType("GIF89a"));
    CHECK_FALSE(referenceImageType(""));
    // equal by the bytes, not the pointer
    const Layer a = referenceLayer(kPng, "a", 1, 2, 3, 4, 5);
    Layer b = referenceLayer(kPng, "a", 1, 2, 3, 4, 5);
    CHECK(a.props == b.props);
    b.props.reference->bytes = std::make_shared<const std::string>(kJpg);
    CHECK_FALSE(a.props == b.props);
}

TEST_CASE("reference layers round-trip with their picture, place, size, opacity and visibility",
          "[project][reference]") {
    const Project p = withReferences();
    const Entries e = project::encode(p);
    // one file a picture, however many layers show it
    const std::string png = "refs/" + sha(kPng) + ".png", jpg = "refs/" + sha(kJpg) + ".jpg";
    CHECK(e.at(png) == kPng);
    CHECK(e.at(jpg) == kJpg);
    CHECK(std::count_if(e.begin(), e.end(), [](const auto& f) { return f.first.starts_with("refs/"); }) == 2);
    CHECK(object(e.at("manifest.json")).value("version").toString() == "1.5");
    const QJsonObject ref = object(e.at(layerJsonPath(p, 2))).value("reference").toObject();
    CHECK(ref.value("image").toString().toStdString() == png);
    CHECK(ref.value("x").toInt() == -13);
    CHECK(ref.value("y").toInt() == 7);
    CHECK(ref.value("width").toInt() == 640);
    CHECK(ref.value("height").toInt() == 480);
    CHECK(ref.value("opacity").toInt() == 50);

    const Project back = project::decode(e);
    CHECK(back == p);
    CHECK(back.files.empty());  // the pictures are the layers', not kept files
    // the stack order: art, sketch, photo, eye, hidden copy
    REQUIRE(back.layers.size() == 5);
    CHECK(back.layers[1].props.name == "sketch");
    CHECK(back.layers[2].props.name == "photo");
    CHECK_FALSE(back.layers[4].props.visible);
    CHECK(back.layers[4].props.reference->opacity == 25);

    const fs::path dir = scratch("roundtrip");
    project::writeZip(p, dir / "refs.bwp");
    CHECK(project::read(dir / "refs.bwp") == p);
    project::writeDirectory(p, dir / "refs");
    CHECK(fs::is_regular_file(dir / "refs" / fs::path(png)));
    CHECK(project::read(dir / "refs") == p);
    // dropping a layer drops its picture from the directory once nothing shows it
    Project fewer = p;
    fewer.layers.remove(fewer.layers[1].props.id);
    project::writeDirectory(fewer, dir / "refs");
    CHECK_FALSE(fs::exists(dir / "refs" / fs::path(jpg)));
    CHECK(project::read(dir / "refs") == fewer);
}

TEST_CASE("a project without reference pictures stays 1.3, a styled one with them is 2.1",
          "[project][reference][version]") {
    Project plain;
    plain.layers = LayerStack(4, 2);
    plain.layers.add(makeLayer(LayerKind::Reference, 0, 0, "empty"));
    CHECK(object(project::encode(plain).at("manifest.json")).value("version").toString() == "1.3");

    Project styled = withReferences();
    TextStyle style;
    style.effects.push_back(Effect{true, OutlineEffect{}});
    styled.layers[3].props.style = style;
    const Entries e = project::encode(styled);
    CHECK(object(e.at("manifest.json")).value("version").toString() == "2.1");
    CHECK(project::decode(e) == styled);
}

TEST_CASE("a project with big text and a reference picture is 1.5", "[project][reference][version][big-text]") {
    // 1.4 is a live big text source; 1.5 a reference picture, and implies 1.4
    TextSource big;
    big.text = U"BLAZE";
    big.bigStyle = BigTextStyle::Outline;
    Project p;
    p.layers = LayerStack(80, 25);
    p.layers.add(makeTextLayer(big, "Title"));
    CHECK(object(project::encode(p).at("manifest.json")).value("version").toString() == "1.4");

    p.layers.add(referenceLayer(kPng, "trace", 0, 0, 640, 400, 50));
    const Entries e = project::encode(p);
    CHECK(object(e.at("manifest.json")).value("version").toString() == "1.5");
    CHECK(project::decode(e) == p);
}

TEST_CASE("a 1.3 reader's save of a 1.5 project opens with an empty reference layer", "[project][reference][version]") {
    // what a reader that doesn't know the "reference" key writes back: the
    // key dropped, the picture kept as a file it doesn't know
    const Project p = withReferences();
    Entries e = project::encode(p);
    for (std::size_t i : {1u, 2u, 4u}) {
        QJsonObject o = object(e.at(layerJsonPath(p, i)));
        o.remove("reference");
        e[layerJsonPath(p, i)] = json(o);
    }
    QJsonObject m = object(e.at("manifest.json"));
    m["version"] = "1.3";
    e["manifest.json"] = json(m);
    const Project back = project::decode(e);
    CHECK_FALSE(back.layers[2].props.reference);
    CHECK(back.layers[2].props.kind == LayerKind::Reference);
    CHECK(back.files.count("refs/" + sha(kPng) + ".png"));
    CHECK(flatten(back.layers, {}) == flatten(p.layers, {}));
}

TEST_CASE("a reader and a writer refuse a bad reference", "[project][reference]") {
    const Project p = withReferences();
    const Entries good = project::encode(p);
    const std::string path = layerJsonPath(p, 2);
    const std::string png = "refs/" + sha(kPng) + ".png";
    const auto with = [&](auto change) {
        Entries e = good;
        change(e);
        return e;
    };
    const auto setRef = [&](Entries& e, const char* key, const QJsonValue& v) {
        QJsonObject o = object(e.at(path));
        QJsonObject r = o.value("reference").toObject();
        r[key] = v;
        o["reference"] = r;
        e[path] = json(o);
    };
    // bytes that don't hash to the name, or aren't the type the name says
    CHECK_THROWS_AS(project::decode(with([&](Entries& e) { e[png] = kPng + "!"; })), ProjectError);
    CHECK_THROWS_AS(project::decode(with([&](Entries& e) {
                        const std::string wrong = "refs/" + sha(kJpg) + ".png";
                        e[wrong] = kJpg;
                        setRef(e, "image", QString::fromStdString(wrong));
                    })),
                    ProjectError);
    // a path the project lacks, or that isn't refs/<sha256>.<type>
    CHECK_THROWS_AS(project::decode(with([&](Entries& e) { e.erase(png); })), ProjectError);
    for (const char* bad : {"refs/photo.png", "pictures/x.png", "refs/../manifest.json"}) {
        INFO(bad);
        CHECK_THROWS_AS(project::decode(with([&](Entries& e) { setRef(e, "image", bad); })), ProjectError);
    }
    // out of range
    CHECK_THROWS_AS(project::decode(with([&](Entries& e) { setRef(e, "width", 0); })), ProjectError);
    CHECK_THROWS_AS(project::decode(with([&](Entries& e) { setRef(e, "opacity", 101); })), ProjectError);
    CHECK_THROWS_AS(project::decode(with([&](Entries& e) { setRef(e, "x", (1 << 24) + 1); })), ProjectError);
    // on a layer that isn't a reference layer
    CHECK_THROWS_AS(project::decode(with([&](Entries& e) {
                        const std::string art = layerJsonPath(p, 3);
                        QJsonObject o = object(e.at(art));
                        o["reference"] = object(e.at(path)).value("reference");
                        e[art] = json(o);
                    })),
                    ProjectError);

    // the writer refuses what the reader would, before writing
    Project bad = p;
    bad.layers[3].props.reference = p.layers[2].props.reference;
    CHECK_THROWS_AS(project::encode(bad), ProjectError);
    bad = p;
    bad.layers[2].props.reference->bytes = std::make_shared<const std::string>("GIF89a");
    CHECK_THROWS_AS(project::encode(bad), ProjectError);
    bad = p;
    bad.layers[2].props.reference->height = 0;
    CHECK_THROWS_AS(project::encode(bad), ProjectError);
}

TEST_CASE("no export sees a reference layer", "[project][reference][flatten]") {
    const Project with = withReferences();
    Project without = with;
    for (std::size_t i = without.layers.size(); i-- > 0;)
        if (without.layers[i].props.kind == LayerKind::Reference)
            without.layers.remove(without.layers[i].props.id);
    REQUIRE(without.layers.size() == 2);

    // the composite, and so every flatten an export or merge makes
    CHECK(flatten(with.layers, {}) == flatten(without.layers, {}));
    CompositeOptions off;
    off.ice = false;
    CHECK(flatten(with.layers, off) == flatten(without.layers, off));
    // .ans
    const AnsiDocument a = project::toAnsi(with), b = project::toAnsi(without);
    CHECK(writeAnsi(a).bytes == writeAnsi(b).bytes);
    // IRC
    CHECK(irc::exportCanvas(a.canvas) == irc::exportCanvas(b.canvas));
    // snapshots
    Project snapWith = with, snapWithout = without;
    project::takeSnapshot(snapWith);
    project::takeSnapshot(snapWithout);
    CHECK(snapWith.snapshots == snapWithout.snapshots);
}

TEST_CASE("decoded layers share identical reference bytes", "[project][reference]") {
    Project p;
    p.layers = LayerStack(1, 1);
    const auto bytes = std::make_shared<const std::string>(kPng);

    for (int i = 0; i < 2; ++i) {
        Layer l = makeLayer(LayerKind::Reference, 0, 0, "reference");
        l.props.reference = ReferenceImage{bytes, 0, 0, 1, 1, 50};
        p.layers.add(std::move(l));
    }

    const Project back = project::decode(project::encode(p));
    REQUIRE(back.layers.size() == 2);
    REQUIRE(back.layers[0].props.reference);
    REQUIRE(back.layers[1].props.reference);
    CHECK(back.layers[0].props.reference->bytes.get() == back.layers[1].props.reference->bytes.get());
}

TEST_CASE("a kept file cannot silently replace a reference picture",
          "[project][reference]") {
    Project p;
    p.layers = LayerStack::fromCanvas(Canvas(1, 1));

    const std::string path = "refs/" + sha(kPng) + ".png";
    p.files[path] = "unrelated kept bytes";

    // Valid input: no reference layer names this opaque file yet.
    p = project::decode(project::encode(p));
    p.layers.add(referenceLayer(kPng, "picture", 0, 0, 1, 1, 50));

    Entries saved;
    try {
        saved = project::encode(p);
    } catch (const ProjectError&) {
        SUCCEED("the conflicting kept file was refused before writing");
        return;
    }

    CHECK(saved.at(path) == kPng);
    CHECK_NOTHROW(project::decode(saved));
}

TEST_CASE("a project with Big rainbow is 1.6, or 2.2 when styled", "[project][reference][version][big-text]") {
    // 1.6 is a live Big rainbow source, and implies 1.5 and 1.4
    TextSource rainbow;
    rainbow.text = U"BLAZE";
    rainbow.bigStyle = BigTextStyle::Rainbow;
    Project p = withReferences();
    p.layers.add(makeTextLayer(rainbow, "Title"));
    Entries e = project::encode(p);
    CHECK(object(e.at("manifest.json")).value("version").toString() == "1.6");
    CHECK(project::decode(e) == p);

    TextStyle style;
    style.effects.push_back(Effect{true, OutlineEffect{}});
    p.layers[3].props.style = style;
    e = project::encode(p);
    CHECK(object(e.at("manifest.json")).value("version").toString() == "2.2");
    CHECK(project::decode(e) == p);
}

namespace {

Cell ink(char32_t ch = U'#') { return Cell{ch, Color::indexed(15), Color::indexed(4), 0, false}; }

} // namespace

TEST_CASE("coverageAbove covers by the visible art layers above the index, by half cells",
          "[layers][reference][composite]") {
    // 0: art, 1: reference, 2: cells (normal), 3: pixels, 4: cells colours-only,
    // 5: cells chars-only, hidden
    LayerStack stack = LayerStack::fromCanvas(Canvas(6, 2));
    stack[0].cells.at(0, 0) = ink();
    stack.add(referenceLayer(kPng, "photo", 0, 0, 48, 32, 100));
    Layer cells = makeLayer(LayerKind::Cells, 6, 2, "cells");
    cells.cells.at(1, 0) = ink();
    stack.add(std::move(cells));
    Layer pixels = makeLayer(LayerKind::Pixels, 6, 2, "pixels");
    pixels.cells.at(2, 0) = pixelLayerCell({std::uint8_t(12), std::nullopt});
    pixels.cells.at(3, 0) = pixelLayerCell({std::nullopt, std::uint8_t(9)});
    pixels.cells.at(4, 0) = pixelLayerCell({std::uint8_t(12), std::uint8_t(9)});
    stack.add(std::move(pixels));
    Layer colours = makeLayer(LayerKind::Cells, 6, 2, "colours");
    colours.props.mode = LayerMode::ColoursOnly;
    colours.cells.at(5, 0) = ink();
    stack.add(std::move(colours));
    Layer hidden = makeLayer(LayerKind::Cells, 6, 2, "hidden");
    hidden.props.mode = LayerMode::CharsOnly;
    hidden.props.visible = false;
    hidden.cells.at(0, 1) = ink();
    stack.add(std::move(hidden));
    REQUIRE(stack.size() == 6);

    const Coverage c = coverageAbove(stack, 1, stack.canvasRect());
    CHECK(c.rect == stack.canvasRect());
    CHECK(c.at(0, 0) == 0);  // the art under the reference
    CHECK(c.at(1, 0) == Coverage::kBoth);
    CHECK(c.at(2, 0) == Coverage::kTop);
    CHECK(c.at(3, 0) == Coverage::kBottom);
    CHECK(c.at(4, 0) == Coverage::kBoth);
    CHECK(c.at(5, 0) == Coverage::kBoth);  // colours-only still shows over it
    CHECK(c.at(0, 1) == 0);  // hidden
    for (int x = 1; x < 6; ++x)
        CHECK(c.at(x, 1) == 0);

    // above the top, nothing covers; at the bottom layer everything above does
    CHECK(coverageAbove(stack, 5, stack.canvasRect()).halves == std::vector<std::uint8_t>(12, 0));
    CHECK(coverageAbove(stack, 0, stack.canvasRect()).at(1, 0) == Coverage::kBoth);
    // a layer at the index itself doesn't cover
    CHECK(coverageAbove(stack, 2, stack.canvasRect()).at(1, 0) == 0);
    CHECK(coverageAbove(stack, 2, stack.canvasRect()).at(2, 0) == Coverage::kTop);
    // another reference above covers nothing
    stack.add(referenceLayer(kJpg, "sketch", 0, 0, 48, 32, 100));
    CHECK(coverageAbove(stack, 1, stack.canvasRect()).at(0, 1) == 0);

    // the rect is clipped to the canvas; outside it nothing is covered
    const Coverage part = coverageAbove(stack, 1, {3, -4, 10, 5});
    CHECK(part.rect == CellRect{3, 0, 3, 1});
    CHECK(part.at(3, 0) == Coverage::kBottom);
    CHECK(part.at(1, 0) == 0);
    CHECK(coverageAbove(stack, 1, {10, 10, 2, 2}).rect.empty());

    // a layer shown at another offset, as a move previews it, covers there
    const LayerId moved = stack[2].props.id;
    const Coverage shifted = coverageAbove(stack, 1, stack.canvasRect(), {}, ShownOffset{moved, 2, 1});
    CHECK(shifted.at(1, 0) == 0);
    CHECK(shifted.at(3, 1) == Coverage::kBoth);
    CHECK(stack[2].props.x == 0);
}

TEST_CASE("coverageAbove: pixels above a text cell don't cover it", "[layers][reference][composite]") {
    // a text cell under the reference, and pixels over both: the composite
    // shows the text there, not the pixels, so the reference shows over it
    LayerStack stack(3, 1);
    Layer text = makeLayer(LayerKind::Text, 3, 1, "text");
    text.cells.at(0, 0) = ink(U'A');
    stack.add(std::move(text));
    stack.add(referenceLayer(kPng, "photo", 0, 0, 24, 16, 100));
    Layer pixels = makeLayer(LayerKind::Pixels, 3, 1, "pixels");
    for (int x = 0; x < 3; ++x)
        pixels.cells.at(x, 0) = pixelLayerCell({std::uint8_t(12), std::uint8_t(9)});
    stack.add(std::move(pixels));
    Coverage c = coverageAbove(stack, 1, stack.canvasRect());
    CHECK(c.at(0, 0) == 0);
    CHECK(c.at(1, 0) == Coverage::kBoth);
    CHECK(compositeCell(stack, 0, 0).ch == U'A');

    // a cells layer over the reference that writes its glyph on the text
    // covers it, and makes it a cells cell again, which the pixels cover
    Layer over = makeLayer(LayerKind::Cells, 3, 1, "over");
    over.cells.at(0, 0) = ink();
    stack.add(std::move(over), 2);
    REQUIRE(stack[2].props.name == "over");
    CHECK(coverageAbove(stack, 1, stack.canvasRect()).at(0, 0) == Coverage::kBoth);
    CHECK(coverageAbove(stack, 2, stack.canvasRect()).at(0, 0) == Coverage::kBoth);
    // in colours-only it covers but leaves the text, so the pixels don't
    stack[2].props.mode = LayerMode::ColoursOnly;
    CHECK(coverageAbove(stack, 1, stack.canvasRect()).at(0, 0) == Coverage::kBoth);
    CHECK(coverageAbove(stack, 2, stack.canvasRect()).at(0, 0) == 0);
}

TEST_CASE("coverageAbove: a styled layer covers by its styled cells", "[layers][reference][composite]") {
    LayerStack stack(5, 3);
    stack.add(referenceLayer(kPng, "photo", 0, 0, 40, 48, 100));
    Layer art = makeLayer(LayerKind::Cells, 5, 3, "styled");
    art.cells.at(2, 1) = ink(U'█');
    TextStyle style;
    style.effects.push_back(Effect{true, OutlineEffect{OutlineForm::Cells, Blue}});
    art.props.style = style;
    REQUIRE(styledAtComposite(art.props));
    const Canvas styled = styledCells(art, true);
    stack.add(std::move(art));
    const Coverage c = coverageAbove(stack, 0, stack.canvasRect());
    bool grown = false;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x) {
            CHECK(c.at(x, y) == (styled.at(x, y).transparent ? 0 : Coverage::kBoth));
            grown = grown || ((x != 2 || y != 1) && !styled.at(x, y).transparent);
        }
    CHECK(grown);  // the outline covers cells the layer's own cells don't
}

TEST_CASE("coverageAbove: a tool's cells in progress cover and uncover as they will", "[layers][reference][composite]") {
    // 0: reference, 1: cells with ink at (1, 0), 2: pixels
    LayerStack stack(4, 1);
    stack.add(referenceLayer(kPng, "photo", 0, 0, 32, 16, 100));
    Layer cells = makeLayer(LayerKind::Cells, 4, 1, "cells");
    cells.cells.at(1, 0) = ink();
    stack.add(std::move(cells));
    stack.add(makeLayer(LayerKind::Pixels, 4, 1, "pixels"));
    const LayerId art = stack[1].props.id, pixels = stack[2].props.id;
    REQUIRE(coverageAbove(stack, 0, stack.canvasRect()).at(1, 0) == Coverage::kBoth);

    // ink drawn at (0, 0) and (1, 0) erased, on the cells layer
    const std::vector<CellChange> stroke{{0, 0, transparentCell(), ink()}, {1, 0, ink(), transparentCell()}};
    Coverage c = coverageAbove(stack, 0, stack.canvasRect(), {}, {}, ShownCells{art, &stroke});
    CHECK(c.at(0, 0) == Coverage::kBoth);
    CHECK(c.at(1, 0) == 0);
    CHECK(c.at(2, 0) == 0);
    // for a layer the stack doesn't hold they change nothing
    c = coverageAbove(stack, 0, stack.canvasRect(), {}, {}, ShownCells{LayerId(pixels + 100), &stroke});
    CHECK(c.at(0, 0) == 0);
    CHECK(c.at(1, 0) == Coverage::kBoth);
    // a cell's last change wins
    const std::vector<CellChange> twice{{2, 0, transparentCell(), ink()}, {2, 0, ink(), transparentCell()}};
    CHECK(coverageAbove(stack, 0, stack.canvasRect(), {}, {}, ShownCells{art, &twice}).at(2, 0) == 0);
    // a pixel layer's cell in progress covers the half it sets
    const std::vector<CellChange> half{{3, 0, transparentCell(), pixelLayerCell({std::nullopt, std::uint8_t(9)})}};
    c = coverageAbove(stack, 0, stack.canvasRect(), {}, {}, ShownCells{pixels, &half});
    CHECK(c.at(3, 0) == Coverage::kBottom);
    // outside the rect asked for, nothing
    CHECK(coverageAbove(stack, 0, {0, 0, 2, 1}, {}, {}, ShownCells{pixels, &half}).at(3, 0) == 0);
    // the stack itself is as it was
    CHECK(coverageAbove(stack, 0, stack.canvasRect()).at(1, 0) == Coverage::kBoth);
}
