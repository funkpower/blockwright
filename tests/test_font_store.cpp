// TheDraw fonts in text layers (PLAN §15, #18): font files keyed by their
// sha256, a face reference resolved through the project's copies and the
// fonts folder, the folder's catalogue, and the copies in the project file
// (fonts/<sha256>.tdf, format 1.2).

#include "blockwright/font_store.h"

#include "blockwright/project.h"
#include "blockwright/text.h"
#include "util.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace bw;
using bw::project::Entries;
using bw::project::Project;
using bw::project::ProjectError;
namespace fs = std::filesystem;

namespace {

std::string tdfData(const std::string& name) { return bwtest::readFile(bwtest::dataDir() / "tdf" / name); }

FontFilePtr fontFile(const std::string& name) { return loadFontFile(tdfData(name)); }

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "font-store" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << bytes;
}

FaceRef tdfRef(const FontFile& file, int index) {
    FaceRef ref;
    ref.kind = FaceRef::Kind::Tdf;
    ref.file = file.sha256;
    ref.index = index;
    ref.name = file.names.at(static_cast<std::size_t>(index));
    return ref;
}

TextSource tdfSource(std::u32string text, const FontFile& file, int index = 0) {
    TextSource s;
    s.text = std::move(text);
    s.face = tdfRef(file, index);
    return s;
}

QJsonObject object(const std::string& text) {
    return QJsonDocument::fromJson(QByteArray(text.data(), static_cast<qsizetype>(text.size()))).object();
}

std::string json(const QJsonObject& o) {
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

std::string layerPath(const Layer& l) { return "layers/" + std::to_string(l.props.id) + ".json"; }
std::string fontPath(const FontFile& f) { return "fonts/" + f.sha256 + ".tdf"; }

// A project with one TheDraw text layer in bw-block, its file copied in.
Project blockProject(FontFilePtr* fileOut = nullptr) {
    const FontFilePtr file = fontFile("bw-block.tdf");
    Project p;
    p.layers = LayerStack(80, 25);
    p.fonts.add(file);
    p.layers.add(makeTextLayer(tdfSource(U"Funk", *file), "Title", 2, 1, {&p.fonts, nullptr}));
    if (fileOut)
        *fileOut = file;
    return p;
}

std::string refusal(const Entries& e) {
    try {
        project::decode(e);
    } catch (const ProjectError& err) {
        return err.what();
    }
    return "(read)";
}

void checkSaves(const Project& p, const fs::path& dir) {
    project::writeZip(p, dir / "piece.bwp");
    CHECK(project::read(dir / "piece.bwp") == p);
    project::writeDirectory(p, dir / "piece");
    CHECK(project::read(dir / "piece") == p);
}

} // namespace

// ---- the store --------------------------------------------------------------

TEST_CASE("a font file is keyed by the sha256 of its bytes and has a face per font", "[tdf][fonts]") {
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(isSha256Hex(sha256Hex("abc")));
    CHECK_FALSE(isSha256Hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    CHECK_FALSE(isSha256Hex("abc"));

    const FontFilePtr all = fontFile("bw-all.tdf");
    CHECK(all->sha256 == sha256Hex(tdfData("bw-all.tdf")));
    CHECK(all->bytes == tdfData("bw-all.tdf"));
    REQUIRE(all->names.size() == 3);
    CHECK(all->names == std::vector<std::string>{"BW Outline", "BW Block", "BW Colour"});
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(all->face(i)->name() == all->names[i]);
    CHECK_THROWS_AS(loadFontFile("not a font"), TdfError);
}

TEST_CASE("a TheDraw face is found by file, index and name, the project's copy first, a held file last",
          "[tdf][fonts]") {
    const FontFilePtr all = fontFile("bw-all.tdf");
    FontStore store;
    CHECK(store.add(all) == all);
    CHECK(store.add(fontFile("bw-all.tdf")) == all);  // a file held is kept
    CHECK(store.files().size() == 1);

    const FaceRef colour = tdfRef(*all, 2);
    CHECK(store.face(colour) == all->face(2));
    FaceRef wrong = colour;
    wrong.name = "BW Block";  // the font at 2 isn't that one
    CHECK(store.face(wrong) == nullptr);
    wrong = colour;
    wrong.index = 3;
    CHECK(store.face(wrong) == nullptr);
    wrong = colour;
    wrong.file = sha256Hex("x");
    CHECK(store.face(wrong) == nullptr);
    wrong = colour;
    wrong.kind = FaceRef::Kind::Pixel;
    CHECK(store.face(wrong) == nullptr);

    // findFace: the project's copy, then the folder's
    FontStore folder;
    folder.add(fontFile("bw-all.tdf"));
    CHECK(findFace(colour) == nullptr);
    CHECK(findFace(colour, {nullptr, &folder}) == folder.file(all->sha256)->face(2));
    CHECK(findFace(colour, {&store, &folder}) == all->face(2));
    CHECK(findFace(colour, {&store, nullptr}) == all->face(2));
    // a held file comes last
    FontStore held;
    held.add(fontFile("bw-all.tdf"));
    CHECK(findFace(colour, {nullptr, nullptr, &held}) == held.file(all->sha256)->face(2));
    CHECK(findFace(colour, {nullptr, &folder, &held}) == folder.file(all->sha256)->face(2));
    CHECK(findFace(colour, {&store, &folder, &held}) == all->face(2));
    // a pixel face never comes from a store
    FaceRef pixel;
    CHECK(findFace(pixel, {&store, &folder}) == &blockFace());
}

TEST_CASE("a source in a TheDraw font no source holds is a clean problem", "[tdf][fonts][text]") {
    const FontFilePtr block = fontFile("bw-block.tdf");
    FontStore folder;
    folder.add(block);
    const TextSource s = tdfSource(U"Funk", *block);
    const auto problem = textSourceProblem(s);
    REQUIRE(problem);
    CHECK(*problem == "no TheDraw font \"BW Block\" (font 0 of fonts/" + block->sha256 +
                          ".tdf) in the project or the fonts folder");
    CHECK_FALSE(textSourceProblem(s, {nullptr, &folder}));
    TextSource renamed = s;
    renamed.face.name = "BW Blocky";
    CHECK(textSourceProblem(renamed, {nullptr, &folder}).value_or("").find("the file has no such font") !=
          std::string::npos);
    CHECK_THROWS_AS(makeTextLayer(s), TextError);

    // drawn with the font's own source cells
    const Layer l = makeTextLayer(s, "t", 0, 0, {nullptr, &folder});
    CHECK(l.cells == renderText(s, *block->face(0)).cells);
    CHECK(l.width() > 0);

    // without the font, the layer keeps its cells (#163): a change that
    // leaves the source keeps them; one that changes it is refused before
    // the layer is touched
    Layer kept = l;
    LayerProperties props = kept.props;
    props.name = "renamed";
    CHECK_FALSE(applyLayerProperties(kept, props));
    CHECK(kept.cells == l.cells);
    CHECK(kept.props.name == "renamed");
    const Layer before = kept;
    TextSource next = s;
    next.text = U"Power";
    CHECK_THROWS_AS(applyEdit(kept, textEdit(kept, next)), TextError);
    CHECK(kept == before);
    // with it, the change draws
    const LayerPropsEdit e = textEdit(kept, next);
    CHECK(applyEdit(kept, e, false, {nullptr, &folder}));
    CHECK(kept.cells == renderText(next, *block->face(0)).cells);
    // and undo puts the cells back without the font
    CHECK(applyEdit(kept, e, true));
    CHECK(kept == before);
}

// ---- the fonts folder -------------------------------------------------------

TEST_CASE("the fonts folder's catalogue lists every font and every file it can't read", "[tdf][fonts]") {
    const fs::path dir = scratch("folder");
    writeFile(dir / "bw-all.tdf", tdfData("bw-all.tdf"));
    writeFile(dir / "sub" / "BLOCK.TDF", tdfData("bw-block.tdf"));
    writeFile(dir / "sub" / "again.tdf", tdfData("bw-block.tdf"));  // the same file twice
    writeFile(dir / "broken.tdf", "not a font");
    writeFile(dir / "cut.tdf", tdfData("bw-colour.tdf").substr(0, 40));
    writeFile(dir / "readme.txt", "hello");
    writeFile(dir / "colour.tdf.bak", tdfData("bw-colour.tdf"));
    fs::create_directories(dir / "dir.tdf");  // a directory with the extension

    const FontCatalogue c = scanFontsFolder(dir);
    CHECK(c.folder == dir);
    std::vector<std::string> shown;
    for (const auto& f : c.fonts) {
        shown.push_back(f.displayName);
        CHECK(f.face == c.store.face(f.ref));
        CHECK(f.face->name() == f.ref.name);
    }
    // by path, then file order; the second copy of bw-block adds nothing
    CHECK(shown == std::vector<std::string>{"BW Outline (bw-all.tdf)", "BW Block (bw-all.tdf)",
                                            "BW Colour (bw-all.tdf)", "BW Block (BLOCK.TDF)"});
    CHECK(c.store.files().size() == 2);
    REQUIRE(c.rejected.size() == 2);
    CHECK(c.rejected[0].file == dir / "broken.tdf");
    std::string why;
    try {
        readTdf("not a font");
    } catch (const TdfError& e) {
        why = e.what();
    }
    CHECK(c.rejected[0].reason == why);
    CHECK(c.rejected[1].file == dir / "cut.tdf");
    CHECK_FALSE(c.rejected[1].reason.empty());

    const std::vector<FaceEntry> entries = c.entries();
    REQUIRE(entries.size() == c.fonts.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        CHECK(entries[i].ref == c.fonts[i].ref);
        CHECK(entries[i].displayName == c.fonts[i].displayName);
        CHECK(entries[i].face == c.fonts[i].face);
    }

    // no folder, an empty one or one that isn't there: nothing, no throw
    CHECK(scanFontsFolder({}).fonts.empty());
    CHECK(scanFontsFolder(scratch("empty")).fonts.empty());
    const FontCatalogue gone = scanFontsFolder(dir / "nowhere");
    CHECK(gone.fonts.empty());
    CHECK(gone.rejected.empty());
}

// ---- the project file -------------------------------------------------------

TEST_CASE("a TheDraw text layer's font is copied into the project and reads back", "[tdf][fonts][project]") {
    FontFilePtr file;
    const Project p = blockProject(&file);
    const Layer& l = p.layers[0];
    const Entries e = project::encode(p);
    CHECK(object(e.at("manifest.json")).value("version").toString() == "1.3");
    CHECK(e.at(fontPath(*file)) == file->bytes);
    const QJsonObject layer = object(e.at(layerPath(l)));
    CHECK_FALSE(layer.contains("source"));
    const QJsonObject face = layer.value("tdfSource").toObject().value("face").toObject();
    CHECK(face.value("kind").toString() == "tdf");
    CHECK(face.value("file").toString().toStdString() == file->sha256);
    CHECK(face.value("index").toInt() == 0);
    CHECK(face.value("name").toString() == "BW Block");

    const Project back = project::decode(e);
    CHECK(back == p);
    CHECK(back.layers[0].cells == l.cells);
    CHECK(back.layers[0].props.text == l.props.text);
    REQUIRE(back.fonts.file(file->sha256));
    CHECK(back.fonts.file(file->sha256)->bytes == file->bytes);
    checkSaves(p, scratch("round-trip"));

    // a pixel source stays under "source"
    Project pixel = p;
    TextSource s;
    s.text = U"x";
    pixel.layers.add(makeTextLayer(s, "pixel"));
    const QJsonObject pl = object(project::encode(pixel).at(layerPath(pixel.layers[1])));
    CHECK(pl.contains("source"));
    CHECK_FALSE(pl.contains("tdfSource"));
}

TEST_CASE("each font file is written once, and only the ones a layer uses", "[tdf][fonts][project]") {
    const FontFilePtr all = fontFile("bw-all.tdf"), block = fontFile("bw-block.tdf"), colour = fontFile("bw-colour.tdf");
    Project p;
    p.layers = LayerStack(80, 40);
    p.fonts.add(all);
    p.fonts.add(block);
    p.fonts.add(colour);  // used by no layer
    const FontSources fonts{&p.fonts, nullptr};
    p.layers.add(makeTextLayer(tdfSource(U"Ab", *all, 0), "outline", 0, 0, fonts));
    p.layers.add(makeTextLayer(tdfSource(U"Cd", *all, 2), "colour", 0, 12, fonts));
    p.layers.add(makeTextLayer(tdfSource(U"Ef", *all, 2), "colour again", 0, 20, fonts));
    p.layers.add(makeTextLayer(tdfSource(U"Gh", *block, 0), "block", 0, 30, fonts));
    const Entries e = project::encode(p);
    std::vector<std::string> copies;
    for (const auto& [path, bytes] : e)
        if (path.starts_with("fonts/"))
            copies.push_back(path);
    std::vector<std::string> expected{fontPath(*all), fontPath(*block)};
    std::sort(expected.begin(), expected.end());
    CHECK(copies == expected);

    const Project back = project::decode(e);
    CHECK(back.layers == p.layers);
    CHECK(back.fonts.files().size() == 2);
    CHECK_FALSE(back.fonts.file(colour->sha256));
}

TEST_CASE("a font copy's faces are made when a layer asks for one, not when the project opens",
          "[tdf][fonts][project]") {
    FontFilePtr block;
    const Project p = blockProject(&block);
    Entries e = project::encode(p);
    const std::string colour = tdfData("bw-colour.tdf");
    e["fonts/" + sha256Hex(colour) + ".tdf"] = colour;  // a copy no layer uses

    const Project back = project::decode(e);
    REQUIRE(back.fonts.file(sha256Hex(colour)));
    CHECK_FALSE(back.fonts.file(sha256Hex(colour))->facesMade());
    CHECK(back.fonts.file(block->sha256)->facesMade());

    const FontFilePtr unused = back.fonts.file(sha256Hex(colour));
    REQUIRE(unused->face(0));
    CHECK(unused->facesMade());
    CHECK(unused->face(0)->name() == "BW Colour");
    CHECK_FALSE(unused->face(1));
}

TEST_CASE("a tampered or missing font copy is refused cleanly", "[tdf][fonts][project]") {
    FontFilePtr file;
    const Project p = blockProject(&file);
    const Entries good = project::encode(p);
    REQUIRE(refusal(good) == "(read)");

    // a byte changed: the copy no longer hashes to its name
    Entries tampered = good;
    tampered[fontPath(*file)][100] ^= 1;
    CHECK(refusal(tampered).find("its bytes hash to") != std::string::npos);

    // bytes that hash to their name but aren't a TheDraw file
    Entries garbage = good;
    garbage["fonts/" + sha256Hex("not a font") + ".tdf"] = "not a font";
    CHECK(refusal(garbage).find("fonts/" + sha256Hex("not a font") + ".tdf: ") != std::string::npos);

    // over the size a .tdf file may be
    Entries big = good;
    const std::string huge(kMaxTdfBytes + 1, '\0');
    big["fonts/" + sha256Hex(huge) + ".tdf"] = huge;
    CHECK(refusal(big).find("is over") != std::string::npos);

    // the copy a layer names is gone
    Entries missing = good;
    missing.erase(fontPath(*file));
    CHECK(refusal(missing).find("no " + fontPath(*file) + " in the project") != std::string::npos);

    // the copy is there but the font the layer names isn't in it
    const auto withFace = [&](auto change) {
        Entries e = good;
        QJsonObject o = object(e.at(layerPath(p.layers[0])));
        QJsonObject s = o.value("tdfSource").toObject();
        QJsonObject f = s.value("face").toObject();
        change(f);
        s["face"] = f;
        o["tdfSource"] = s;
        e[layerPath(p.layers[0])] = json(o);
        return refusal(e);
    };
    CHECK(withFace([](QJsonObject&) {}) == "(read)");
    CHECK(withFace([](QJsonObject& f) { f["index"] = 1; }).find("the file has no such font") != std::string::npos);
    CHECK(withFace([](QJsonObject& f) { f["name"] = "BW Blocky"; }).find("the file has no such font") !=
          std::string::npos);
    CHECK(withFace([](QJsonObject& f) { f["file"] = "ABC"; }).find("not a sha256") != std::string::npos);
    CHECK(withFace([](QJsonObject& f) { f["index"] = -1; }) != "(read)");
    CHECK(withFace([](QJsonObject& f) { f.remove("file"); }) != "(read)");
}

TEST_CASE("a font file over kMaxTdfBytes is refused when it's loaded and when it's written",
          "[tdf][fonts][project]") {
    // Codex round 1 (#168): bw-block.tdf padded with zeros past 4 MiB loaded,
    // and a project using it saved, which no reader then opened.
    std::string bytes = tdfData("bw-block.tdf");
    bytes.resize(kMaxTdfBytes + 1, '\0');
    try {
        loadFontFile(bytes);
        FAIL("loaded");
    } catch (const TdfError& e) {
        CHECK(std::string(e.what()).find("over 4 MiB") != std::string::npos);
    }
    bytes.resize(kMaxTdfBytes);
    CHECK(loadFontFile(bytes)->names == std::vector<std::string>{"BW Block"});

    // a file made some other way is refused by the writer before it
    // touches what is at the destination
    bytes.resize(kMaxTdfBytes + 1, '\0');
    const auto big = std::make_shared<const FontFile>(bytes, std::vector<std::string>{"BW Block"});
    Project p;
    p.layers = LayerStack(80, 25);
    p.fonts.add(big);
    p.layers.add(makeTextLayer(tdfSource(U"Funk", *big), "Title", 2, 1, {&p.fonts, nullptr}));
    const fs::path dir = scratch("oversized-copy");
    writeFile(dir / "piece.bwp", "before");
    try {
        project::writeZip(p, dir / "piece.bwp");
        FAIL("written");
    } catch (const ProjectError& e) {
        CHECK(std::string(e.what()).find(fontPath(*big) + " is over 4 MiB") != std::string::npos);
    }
    CHECK(bwtest::readFile(dir / "piece.bwp") == "before");
    CHECK_THROWS_AS(project::writeDirectory(p, dir / "piece"), ProjectError);
    CHECK_FALSE(fs::exists(dir / "piece"));
    const Project good = blockProject();
    project::writeDirectory(good, dir / "kept");
    CHECK_THROWS_AS(project::writeDirectory(p, dir / "kept"), ProjectError);
    CHECK(project::read(dir / "kept") == good);
}

TEST_CASE("the copy is what a project opens with when the fonts folder no longer has the font",
          "[tdf][fonts][project]") {
    const fs::path dir = scratch("folder-gone");
    writeFile(dir / "fonts" / "block.tdf", tdfData("bw-block.tdf"));
    const auto folder = std::make_unique<FontCatalogue>(scanFontsFolder(dir / "fonts"));
    REQUIRE(folder->fonts.size() == 1);
    const FaceRef ref = folder->fonts[0].ref;

    // set from the folder, copied into the project as the app does
    Project p;
    p.layers = LayerStack(80, 25);
    TextSource s;
    s.text = U"Funk";
    s.face = ref;
    p.layers.add(makeTextLayer(s, "Title", 0, 0, {&p.fonts, &folder->store}));
    p.fonts.add(folder->store.file(ref.file));
    project::writeZip(p, dir / "piece.bwp");
    project::writeDirectory(p, dir / "piece");

    fs::remove_all(dir / "fonts");
    for (const fs::path& path : {dir / "piece.bwp", dir / "piece"}) {
        Project back = project::read(path);
        CHECK(back == p);
        const FontCatalogue empty = scanFontsFolder(dir / "fonts");
        const FontSources fonts{&back.fonts, &empty.store};
        CHECK_FALSE(textSourceProblem(*back.layers[0].props.text, fonts));
        // and its text is edited with the copy
        TextSource next = s;
        next.text = U"Power";
        Layer& l = back.layers[0];
        CHECK(applyEdit(l, textEdit(l, next), false, fonts));
        CHECK(l.cells == renderText(next, *back.fonts.face(ref)).cells);
    }
}

TEST_CASE("a 1.1 reader drops a TheDraw source and shows the saved cells", "[tdf][fonts][project]") {
    FontFilePtr file;
    const Project p = blockProject(&file);
    Entries e = project::encode(p);
    // what a 1.1 reader does with the layer: "tdfSource" is a key it doesn't
    // know and drops; fonts/ is a file it doesn't know and keeps
    QJsonObject o = object(e.at(layerPath(p.layers[0])));
    o.remove("tdfSource");
    e[layerPath(p.layers[0])] = json(o);
    const Project old = project::decode(e);
    CHECK(old.layers[0].props.kind == LayerKind::Text);
    CHECK_FALSE(old.layers[0].props.text);
    CHECK(old.layers[0].cells == p.layers[0].cells);

    // and the keys can't be mixed up
    const auto withLayer = [&](auto change) {
        Entries x = project::encode(p);
        QJsonObject l = object(x.at(layerPath(p.layers[0])));
        change(l);
        x[layerPath(p.layers[0])] = json(l);
        return refusal(x);
    };
    CHECK(withLayer([](QJsonObject& l) {
              l["source"] = l.value("tdfSource");
              l.remove("tdfSource");
          }).find("a tdf face under \"source\"") != std::string::npos);
    CHECK(withLayer([](QJsonObject& l) { l["source"] = l.value("tdfSource"); }).find("both") != std::string::npos);
    CHECK(withLayer([](QJsonObject& l) {
              QJsonObject s = l.value("tdfSource").toObject();
              s["face"] = QJsonObject{{"kind", "pixel"}, {"name", "block"}};
              l["tdfSource"] = s;
          }).find("a pixel face under \"tdfSource\"") != std::string::npos);
}

TEST_CASE("a writer refuses a TheDraw source without its font, and a kept file at a copy's path",
          "[tdf][fonts][project]") {
    FontFilePtr file;
    Project p = blockProject(&file);
    Project noCopy = p;
    noCopy.fonts = {};
    try {
        project::encode(noCopy);
        FAIL("encoded");
    } catch (const ProjectError& e) {
        CHECK(std::string(e.what()).find("in the project or the fonts folder") != std::string::npos);
    }

    Project kept = p;
    kept.files[fontPath(*file)] = file->bytes;
    CHECK_THROWS_AS(project::encode(kept), ProjectError);
    // a path under fonts/ that isn't a copy's is an ordinary kept file
    Project other = p;
    other.files["fonts/readme.txt"] = "x";
    other.files["fonts/" + std::string(64, 'A') + ".tdf"] = "y";
    CHECK(project::decode(project::encode(other)) == other);
}

TEST_CASE("a project holds at most kMaxFontFiles font copies, read or written", "[tdf][fonts][project]") {
    // bw-block.tdf with its font renamed, a different file each time
    const std::string block = tdfData("bw-block.tdf");
    const std::size_t at = block.find("BW Block");
    REQUIRE(at != std::string::npos);
    std::vector<FontFilePtr> files;
    for (std::size_t i = 0; i <= project::kMaxFontFiles; ++i) {
        std::string bytes = block;
        bytes[at + 7] = char('a' + i);
        files.push_back(loadFontFile(bytes));
    }

    Project p;
    p.layers = LayerStack(80, 25);
    for (std::size_t i = 0; i < project::kMaxFontFiles; ++i) {
        p.fonts.add(files[i]);
        p.layers.add(makeTextLayer(tdfSource(U"A", *files[i]), "t", 0, 0, {&p.fonts, nullptr}));
    }
    const Entries e = project::encode(p);
    CHECK(project::decode(e) == p);

    Project over = p;
    over.fonts.add(files.back());
    over.layers.add(makeTextLayer(tdfSource(U"A", *files.back()), "t", 0, 0, {&over.fonts, nullptr}));
    try {
        project::encode(over);
        FAIL("encoded");
    } catch (const ProjectError& err) {
        CHECK(std::string(err.what()).find("over 16 font files") != std::string::npos);
    }
    // a reader counts the copies before reading any, used or not
    Entries more = e;
    more["fonts/" + sha256Hex("x") + ".tdf"] = "x";
    CHECK(refusal(more).find("over 16 font files") != std::string::npos);
}
