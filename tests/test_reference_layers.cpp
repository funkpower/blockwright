// Reference layers in the core (PLAN §5, §6; #178): the picture's bytes,
// place, size and opacity round-trip through a project, the file under
// refs/ is checked as a reader checks it, and nothing that exports the art
// sees the layer.

#include "blockwright/composite.h"
#include "blockwright/irc.h"
#include "blockwright/project.h"
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
