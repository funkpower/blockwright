// Projects (PLAN §6): the .cells codec, the manifest, every layer kind
// through zip and directory, snapshots, and flattening through the
// compositor.

#include "blockwright/project.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/composite.h"
#include "blockwright/cp437.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"
#include "blockwright/shapes.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <zip.h>
#include <zstd.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <thread>
#endif

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <new>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// An allocation a test makes fail: set, the next one on this thread throws
// std::bad_alloc (and clears it). The whole test binary allocates through
// this, which otherwise is malloc.
namespace bwtest {
thread_local bool failNextAllocation = false;
// While `allocations` is 0 or more, each allocation on this thread counts
// in it, and the one it counts as `failAllocation` (from 0) throws
// std::bad_alloc instead.
thread_local long allocations = -1;
thread_local long failAllocation = -1;
// While set, every allocation on this thread throws std::bad_alloc.
thread_local bool failAllocations = false;
}

void* operator new(std::size_t size) {
    if (bwtest::failAllocations)
        throw std::bad_alloc();
    if (bwtest::failNextAllocation) {
        bwtest::failNextAllocation = false;
        throw std::bad_alloc();
    }
    if (bwtest::allocations >= 0 && bwtest::allocations++ == bwtest::failAllocation)
        throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#if defined(__linux__)
#include <dlfcn.h>

// The archives and open entries libzip hands the project code that aren't
// freed yet: zip_open takes an archive, zip_discard and a zip_close that
// succeeds give it back; zip_fopen_index takes an entry, zip_fclose gives it
// back. Kept without allocating, so a test counting allocations counts none
// here.
namespace bwtest {
struct Held {
    const void* at[64] = {};
    void take(const void* p) {
        for (const void*& a : at)
            if (!a) {
                a = p;
                return;
            }
    }
    void give(const void* p) {
        for (const void*& a : at)
            if (a == p)
                a = nullptr;
    }
    long count() const { return std::count_if(std::begin(at), std::end(at), [](const void* a) { return a; }); }
};
inline Held zips, zipFiles;
template <typename F>
F nextSymbol(const char* name) {
    return reinterpret_cast<F>(::dlsym(RTLD_NEXT, name));
}
} // namespace bwtest

extern "C" {
zip_t* zip_open(const char* path, int flags, int* err) {
    static const auto next = bwtest::nextSymbol<zip_t* (*)(const char*, int, int*)>("zip_open");
    zip_t* z = next(path, flags, err);
    if (z)
        bwtest::zips.take(z);
    return z;
}
int zip_close(zip_t* z) {
    static const auto next = bwtest::nextSymbol<int (*)(zip_t*)>("zip_close");
    const int r = next(z);
    if (r == 0)
        bwtest::zips.give(z);
    return r;
}
zip_file_t* zip_fopen_index(zip_t* z, zip_uint64_t i, zip_flags_t flags) {
    static const auto next = bwtest::nextSymbol<zip_file_t* (*)(zip_t*, zip_uint64_t, zip_flags_t)>("zip_fopen_index");
    zip_file_t* f = next(z, i, flags);
    if (f)
        bwtest::zipFiles.take(f);
    return f;
}
int zip_fclose(zip_file_t* f) {
    static const auto next = bwtest::nextSymbol<int (*)(zip_file_t*)>("zip_fclose");
    bwtest::zipFiles.give(f);
    return next(f);
}
void zip_discard(zip_t* z) {
    static const auto next = bwtest::nextSymbol<void (*)(zip_t*)>("zip_discard");
    bwtest::zips.give(z);
    next(z);
}
}
#endif

using namespace bw;
using bw::project::Entries;
using bw::project::Project;
using bw::project::ProjectError;
namespace fs = std::filesystem;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg, std::uint8_t attrs = 0) {
    return Cell{ch, Color::indexed(fg), Color::indexed(bg), attrs, false};
}

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "project" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeBytes(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

std::string json(const QJsonObject& o) {
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

QJsonObject object(const std::string& text) {
    return QJsonDocument::fromJson(QByteArray(text.data(), static_cast<qsizetype>(text.size()))).object();
}

// The 14 bytes encodeCells writes for one cell: code point, foreground
// (kind, three bytes), background as the foreground, attributes, flags.
std::string cellRecord(std::uint32_t cp, int fgKind, int fg0, int fg1, int bgKind, int bg0, int bg1, int attrs,
                       int flags) {
    std::string r(14, '\0');
    for (int i = 0; i < 4; ++i)
        r[i] = char((cp >> (8 * i)) & 0xff);
    r[4] = char(fgKind);
    r[5] = char(fg0);
    r[6] = char(fg1);
    r[8] = char(bgKind);
    r[9] = char(bg0);
    r[10] = char(bg1);
    r[12] = char(attrs);
    r[13] = char(flags);
    return r;
}

// A .cells file around `records`, compressed as they are.
std::string rawCells(std::uint32_t w, std::uint32_t h, const std::string& records) {
    std::string out = "BWCL";
    out += std::string("\x01\0\0\0", 4);
    for (std::uint32_t v : {w, h})
        for (int i = 0; i < 4; ++i)
            out += char((v >> (8 * i)) & 0xff);
    std::string frame(ZSTD_compressBound(records.size()), '\0');
    frame.resize(ZSTD_compress(frame.data(), frame.size(), records.data(), records.size(), 3));
    return out + frame;
}

// A project with one layer of every kind and every field set away from
// its default.
Project everything() {
    Project p;
    p.layers = LayerStack(20, 8);

    Layer background = makeLayer(LayerKind::Cells, 20, 8, "Background");
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 20; ++x)
            background.cells.at(x, y) = cell(x % 2 ? kShadeLight : kShadeMedium, std::uint8_t(x % 16),
                                             std::uint8_t(y % 8), y == 3 ? attr::Blink : 0);
    background.cells.at(4, 4).fg = Color::fromRgb({0x12, 0x34, 0x56});
    background.cells.at(5, 4).bg = Color::fromRgb({0xfe, 0x01, 0x80});
    background.cells.at(6, 4).ch = U'\U0001F600';  // no CP437 byte: kept in the project
    p.layers.add(std::move(background));

    Layer scratchLayer = makeLayer(LayerKind::Cells, 2, 2, "gone");
    const LayerId gone = p.layers.add(std::move(scratchLayer));

    Layer px = makeLayer(LayerKind::Pixels, 6, 4, "moon", -2, 5);
    px.cells.at(0, 0) = setLayerPixel(px.cells.at(0, 0), false, Yellow);
    px.cells.at(1, 0) = setLayerPixel(px.cells.at(1, 0), true, White);
    px.cells.at(2, 1) = setLayerPixel(setLayerPixel(px.cells.at(2, 1), false, Red), true, Blue);
    px.props.alphaLocked = true;
    p.layers.add(std::move(px));

    Layer text = makeLayer(LayerKind::Text, 5, 1, "title", 3, 0);
    for (int x = 0; x < 5; ++x)
        text.cells.at(x, 0) = cell(U"HELLO"[x], White, Black);
    text.props.mode = LayerMode::CharsOnly;
    text.props.locked = true;
    p.layers.add(std::move(text), 1);

    Layer gen = makeLayer(LayerKind::Generator, 4, 3, "stars", 30, -1);
    gen.cells.at(1, 1) = cell(U'*', Yellow, Black);
    gen.props.visible = false;
    const LayerId genId = p.layers.add(std::move(gen));

    Layer ref = makeLayer(LayerKind::Reference, 20, 8, "sketch");
    ref.props.mode = LayerMode::ColoursOnly;
    p.layers.add(std::move(ref), 0);

    p.layers.remove(gone);  // so nextId is past an id no layer holds

    p.ice = false;
    Sauce s = Sauce::fresh(20, 8);
    s.title = "Moon \x82t\xE9";  // CP437: é, Θ
    s.author = "funkpower";
    s.group = "LRH";
    s.date = "20261023";
    s.font = "IBM VGA50";
    s.setIce(false);
    s.setLetterSpacing(2);
    s.setAspectRatio(2);
    s.comments = {"one", "two \xB0\xB1\xB2"};
    p.sauce = s;
    p.ircBudget = {"custom", 380};
    p.notes = "# brief\n\nA moon over steel.\n";
    p.snapshots = {{1, "first round"}, {2, std::string("\x1b[0m\0x", 6)}};
    p.files["refs/sketch.png"] = std::string("\x89PNG\r\n\x1a\n\0\1", 10);
    p.files["scripts/" + std::to_string(genId) + "/scene.py"] = "print('stars')\n";
    p.files["scripts/" + std::to_string(genId) + "/params.json"] = "{\"n\": 40}\n";
    p.files["thumbnail.png"] = "not really a png";
    p.files["log.md"] = "first pass: too dark\n";
    return p;
}

// A shaded steel field with a pixel layer cutting its shade cells in half,
// as PLAN §11's Phase 3 done-when draws it.
LayerStack shadeUnderPixels() {
    const Ramp steel = *findRamp("steel");
    constexpr int W = 80, H = 26;
    Canvas field(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            field.at(x, y) = stepRung(steel, Cell{}, y / 2).value_or(Cell{});
    LayerStack s = LayerStack::fromCanvas(field);
    Layer px = makeLayer(LayerKind::Pixels, W, H, "shapes");
    const auto put = [&](const Pixels& pixels, std::uint8_t colour) {
        for (const auto& [x, py] : pixels) {
            Cell& c = px.cells.at(x, py >> 1);
            c = setLayerPixel(c, (py & 1) != 0, colour);
        }
    };
    put(ellipseShape(5, 21, 30, 51, true, W, 2 * H), Black);
    put(rectShape(34, 13, 45, 30, true, W, 2 * H), Black);
    s.add(std::move(px));
    return s;
}

} // namespace

// ---- .cells ----------------------------------------------------------------

TEST_CASE("layer cells round-trip through the .cells codec", "[project][cells]") {
    Canvas c(7, 3);
    c.at(0, 0) = cell(U'▀', Yellow, Blue, attr::Blink);
    c.at(1, 0).transparent = true;
    c.at(1, 0).ch = U'▄';
    c.at(2, 1).fg = Color::fromRgb({1, 2, 3});
    c.at(3, 1).bg = Color::fromRgb({250, 128, 0});
    c.at(4, 2).ch = U'\U0010FFFF';
    c.at(5, 2).attrs = 0xA0;  // attribute bits the editor doesn't use are kept
    const std::string data = project::encodeCells(c);
    REQUIRE(data.size() > 16);
    CHECK(data.substr(0, 4) == "BWCL");
    CHECK(data.substr(4, 4) == std::string("\x01\0\0\0", 4));
    CHECK(data.substr(8, 8) == std::string("\x07\0\0\0\x03\0\0\0", 8));
    CHECK(project::decodeCells(data) == c);

    // a 24-bit colour keeps its RGB, not just its palette index
    const Canvas back = project::decodeCells(data);
    CHECK_FALSE(back.at(2, 1).fg.isIndexed());
    CHECK(back.at(2, 1).fg.rgb == Rgb{1, 2, 3});

    CHECK(project::decodeCells(project::encodeCells(Canvas(0, 0))) == Canvas(0, 0));
}

TEST_CASE("the .cells codec refuses what it can't read exactly", "[project][cells]") {
    Canvas c(3, 2);
    const std::string good = project::encodeCells(c);
    const auto refused = [](std::string data) { CHECK_THROWS_AS(project::decodeCells(data), ProjectError); };
    refused("");
    refused(good.substr(0, 12));
    refused("BWCX" + good.substr(4));
    std::string v2 = good;
    v2[4] = 2;
    refused(v2);
    std::string reserved = good;
    reserved[6] = 1;
    refused(reserved);
    refused(good.substr(0, good.size() - 1));  // the frame cut short
    refused(good + "x");                        // bytes after the frame
    std::string wider = good;
    wider[8] = 4;  // says 4 wide: the frame holds 3
    refused(wider);
    std::string huge = good;
    huge[10] = 1;  // 65,539 wide
    refused(huge);

    // a frame that says nothing of its size, or holds cells that aren't cells
    const std::string blank = cellRecord(U' ', 0, 7, 0, 0, 0, 0, 0, 0);
    CHECK(project::decodeCells(rawCells(1, 1, blank)) == Canvas(1, 1));
    std::string unsized = rawCells(1, 1, blank);
    unsized.resize(16);
    unsized += [&] {
        ZSTD_CCtx* cx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(cx, ZSTD_c_contentSizeFlag, 0);
        std::string out(ZSTD_compressBound(blank.size()), '\0');
        out.resize(ZSTD_compress2(cx, out.data(), out.size(), blank.data(), blank.size()));
        ZSTD_freeCCtx(cx);
        return out;
    }();
    refused(unsized);
    refused(rawCells(1, 1, blank + blank));  // two cells in a one-cell layer
    refused(rawCells(2, 1, blank));
    refused(rawCells(1, 1, cellRecord(0x110000, 0, 7, 0, 0, 0, 0, 0, 0)));
    refused(rawCells(1, 1, cellRecord(0xD800, 0, 7, 0, 0, 0, 0, 0, 0)));  // a surrogate
    refused(rawCells(1, 1, cellRecord(U' ', 2, 7, 0, 0, 0, 0, 0, 0)));     // no colour kind 2
    refused(rawCells(1, 1, cellRecord(U' ', 0, 16, 0, 0, 0, 0, 0, 0)));    // no palette index 16
    refused(rawCells(1, 1, cellRecord(U' ', 0, 7, 1, 0, 0, 0, 0, 0)));     // an index's padding set
    refused(rawCells(1, 1, cellRecord(U' ', 0, 7, 0, 0, 0, 0, 0, 2)));     // no flag bit 1
    CHECK(project::decodeCells(rawCells(1, 1, cellRecord(U' ', 1, 9, 8, 0, 0, 0, 0, 1))).at(0, 0) ==
          Cell{U' ', Color::fromRgb({9, 8, 0}), Color::indexed(0), 0, true});
}

// ---- the manifest and every layer kind --------------------------------------

TEST_CASE("every layer kind and field survives encode and decode", "[project]") {
    const Project p = everything();
    const Entries e = project::encode(p);
    CHECK(e.count("manifest.json"));
    CHECK(e.count("notes.md"));
    CHECK(e.count("snapshots/r1.ans"));
    CHECK(e.count("snapshots/r2.ans"));
    for (const Layer& l : p.layers.layers()) {
        const std::string id = std::to_string(l.props.id);
        CHECK(e.count("layers/" + id + ".json"));
        CHECK(e.count("layers/" + id + ".cells"));
    }
    const Project back = project::decode(e);
    CHECK(back.layers == p.layers);
    CHECK(back.layers.nextId() == p.layers.nextId());
    CHECK(back.ice == p.ice);
    REQUIRE(back.sauce);
    CHECK(*back.sauce == *p.sauce);
    CHECK(back.ircBudget == p.ircBudget);
    CHECK(back.notes == p.notes);
    CHECK(back.snapshots == p.snapshots);
    CHECK(back.files == p.files);
    CHECK(back == p);

    // the manifest is readable JSON with the layout PLAN §6 gives
    const QJsonObject m = object(e.at("manifest.json"));
    CHECK(m.value("format").toString() == "blockwright");
    CHECK(m.value("version").toString() == "1.0");
    CHECK(m.value("width").toInt() == 20);
    CHECK(m.value("height").toInt() == 8);
    CHECK(m.value("palette").toString() == "cga");
    CHECK(m.value("ice").toBool() == false);
    CHECK(m.value("layers").toArray().size() == qsizetype(p.layers.size()));
    CHECK(m.value("ircBudget").toObject().value("bytes").toInt() == 380);
    const QJsonObject sauce = m.value("sauce").toObject();
    CHECK(sauce.value("title").toString() == QString::fromUtf8("Moon étΘ"));
    CHECK(sauce.value("letterSpacing").toInt() == 9);
    CHECK(sauce.value("aspectRatio").toString() == "square");
    CHECK(sauce.value("font").toString() == "IBM VGA50");
    const QJsonObject title = object(e.at("layers/" + std::to_string(p.layers[2].props.id) + ".json"));
    CHECK(title.value("kind").toString() == "text");
    CHECK(title.value("mode").toString() == "chars-only");
}

TEST_CASE("a project without SAUCE or notes round-trips", "[project]") {
    Project p;
    p.layers = LayerStack(3, 2);
    const Entries e = project::encode(p);
    CHECK_FALSE(e.count("notes.md"));
    CHECK(object(e.at("manifest.json")).value("sauce").isNull());
    CHECK(project::decode(e) == p);
}

TEST_CASE("every layer kind survives a zip and a directory", "[project][io]") {
    const Project p = everything();
    const fs::path dir = scratch("kinds");

    project::writeZip(p, dir / "moon.bwp");
    CHECK(project::isProject(dir / "moon.bwp"));
    CHECK(project::read(dir / "moon.bwp") == p);

    project::writeDirectory(p, dir / "moon");
    CHECK(project::isProject(dir / "moon"));
    CHECK(fs::is_regular_file(dir / "moon" / "manifest.json"));
    CHECK(fs::is_regular_file(dir / "moon" / "refs" / "sketch.png"));
    CHECK(project::read(dir / "moon") == p);

    // a zip writes the same bytes every time
    project::writeZip(p, dir / "again.bwp");
    CHECK(bwtest::readFile(dir / "moon.bwp") == bwtest::readFile(dir / "again.bwp"));

    // and zip to directory to zip is the same project
    project::writeZip(project::read(dir / "moon"), dir / "third.bwp");
    CHECK(bwtest::readFile(dir / "moon.bwp") == bwtest::readFile(dir / "third.bwp"));

    CHECK_FALSE(project::isProject(dir));
    CHECK_FALSE(project::isProject(bwtest::dataDir() / "golden" / "tux.ans"));
    CHECK_FALSE(project::isProject(dir / "missing"));
}

// ---- versions and what a reader refuses --------------------------------------

TEST_CASE("a reader refuses another major version and reads a newer minor", "[project][version]") {
    Project p = everything();
    Entries e = project::encode(p);
    QJsonObject m = object(e.at("manifest.json"));

    m["version"] = "2.0";
    e["manifest.json"] = json(m);
    CHECK_THROWS_AS(project::decode(e), ProjectError);

    m["version"] = "1.7";
    m["mask"] = "nick!user@host";  // a key 1.7 added: ignored
    e["manifest.json"] = json(m);
    e["layers/" + std::to_string(p.layers[0].props.id) + ".mask"] = "a file 1.7 added";
    const Project newer = project::decode(e);
    CHECK(newer.layers == p.layers);
    CHECK(newer.files.at("layers/" + std::to_string(p.layers[0].props.id) + ".mask") == "a file 1.7 added");

    for (const char* v : {"1", "one.0", "1.0.0", "-1.0", "1.x", ""}) {
        INFO(v);
        m["version"] = v;
        e["manifest.json"] = json(m);
        CHECK_THROWS_AS(project::decode(e), ProjectError);
    }
    m["version"] = "1.0";
    m["format"] = "icy-draw";
    e["manifest.json"] = json(m);
    CHECK_THROWS_AS(project::decode(e), ProjectError);
}

TEST_CASE("decode refuses a manifest and layers that disagree", "[project]") {
    const Project p = everything();
    const Entries good = project::encode(p);
    const std::string id0 = std::to_string(p.layers[0].props.id);
    const std::string id1 = std::to_string(p.layers[1].props.id);
    const auto refused = [](const Entries& e) { CHECK_THROWS_AS(project::decode(e), ProjectError); };
    const auto withManifest = [&](auto change) {
        Entries e = good;
        QJsonObject m = object(e.at("manifest.json"));
        change(m);
        e["manifest.json"] = json(m);
        return e;
    };
    const auto withLayer = [&](const std::string& id, auto change) {
        Entries e = good;
        QJsonObject l = object(e.at("layers/" + id + ".json"));
        change(l);
        e["layers/" + id + ".json"] = json(l);
        return e;
    };

    {
        Entries e = good;
        e.erase("manifest.json");
        refused(e);
        e["manifest.json"] = "{not json";
        refused(e);
        e["manifest.json"] = "[]";
        refused(e);
    }
    {
        // a key twice, at the top or in an object under it: JSON readers
        // disagree on which one counts
        Entries e = good;
        std::string m = good.at("manifest.json");
        e["manifest.json"] = "{\"ice\": true, " + m.substr(m.find('{') + 1);
        refused(e);
        e["manifest.json"] = std::regex_replace(m, std::regex("\"bytes\":"), "\"bytes\": 1, \"bytes\":");
        REQUIRE(e["manifest.json"] != m);
        refused(e);
        // the same name in two objects is fine
        e["manifest.json"] = m;
        CHECK(project::decode(e) == project::decode(good));
        std::string l = good.at("layers/" + id0 + ".json");
        e["layers/" + id0 + ".json"] = "{\"n\\u0061me\": \"x\", " + l.substr(l.find('{') + 1);
        refused(e);
    }
    {
        Entries e = good;
        e.erase("layers/" + id0 + ".cells");
        refused(e);
    }
    {
        Entries e = good;
        e.erase("layers/" + id0 + ".json");
        refused(e);
    }
    {
        // a layer file the manifest doesn't list
        Entries e = good;
        e["layers/999.json"] = good.at("layers/" + id0 + ".json");
        refused(e);
        e.erase("layers/999.json");
        e["layers/999.cells"] = good.at("layers/" + id0 + ".cells");
        refused(e);
    }
    refused(withManifest([&](QJsonObject& m) {
        QJsonArray order = m["layers"].toArray();
        order.append(order.at(0));  // one id twice
        m["layers"] = order;
    }));
    refused(withManifest([&](QJsonObject& m) { m["nextLayerId"] = qint64(p.layers[0].props.id); }));
    refused(withManifest([&](QJsonObject& m) { m["width"] = 0; }));
    refused(withManifest([&](QJsonObject& m) { m["width"] = kMaxColumns + 1; }));
    refused(withManifest([&](QJsonObject& m) { m["height"] = 2.5; }));
    refused(withManifest([&](QJsonObject& m) { m["palette"] = "amiga"; }));
    refused(withManifest([&](QJsonObject& m) { m["ice"] = 1; }));
    refused(withManifest([&](QJsonObject& m) { m.remove("layers"); }));
    refused(withManifest([&](QJsonObject& m) { m["ircBudget"] = QJsonObject{{"profile", "x"}, {"bytes", 0}}; }));
    refused(withManifest([&](QJsonObject& m) {
        QJsonObject s = m["sauce"].toObject();
        s["title"] = QString::fromUtf8("★");  // no CP437 byte
        m["sauce"] = s;
    }));
    refused(withManifest([&](QJsonObject& m) {
        QJsonObject s = m["sauce"].toObject();
        s["letterSpacing"] = 10;
        m["sauce"] = s;
    }));
    refused(withLayer(id0, [&](QJsonObject& l) { l["id"] = qint64(p.layers[1].props.id); }));
    refused(withLayer(id0, [&](QJsonObject& l) { l["width"] = l["width"].toInt() + 1; }));
    refused(withLayer(id0, [&](QJsonObject& l) { l["kind"] = "group"; }));
    refused(withLayer(id0, [&](QJsonObject& l) { l["mode"] = "multiply"; }));
    refused(withLayer(id1, [&](QJsonObject& l) { l["visible"] = "yes"; }));
    refused(withLayer(id1, [&](QJsonObject& l) { l["x"] = 1e12; }));
    refused(withLayer(id1, [&](QJsonObject& l) { l.remove("name"); }));
}

TEST_CASE("paths in a project are relative, plain and not the format's own", "[project][paths]") {
    for (const char* ok : {"refs/a.png", "scripts/3/scene.py", "thumbnail.png", "a b/c-d_e.f"})
        CHECK(project::safeEntryPath(ok));
    for (const char* bad : {"", "/etc/passwd", "../x", "refs/../../x", "refs//a", "refs/", "./a", ".git/config",
                            "refs/.hidden", "a\\b", "c:x", "a\nb", "refs/a\x7f"})
        CHECK_FALSE(project::safeEntryPath(bad));

    for (const char* owned : {"manifest.json", "notes.md", "layers/1.json", "layers/1.cells", "snapshots/r1.ans",
                              "../escape", ".hidden"}) {
        INFO(owned);
        Project p;
        p.files[owned] = "x";
        CHECK_THROWS_AS(project::encode(p), ProjectError);
    }
    // under the format's directories, other names are anyone's
    Project p;
    p.files["layers/1.mask"] = "m";
    p.files["snapshots/notes.txt"] = "n";
    p.files["snapshots/r01.ans"] = "not a round: r01 isn't r1";
    CHECK(project::decode(project::encode(p)) == p);
}

// ---- reading files -----------------------------------------------------------

TEST_CASE("a zip with an unsafe path is refused", "[project][io]") {
    const fs::path dir = scratch("unsafe");
    const Entries good = project::encode(everything());
    const auto zipWith = [&](const fs::path& path, const std::string& extra) {
        int err = 0;
        zip_t* z = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
        REQUIRE(z);
        const auto add = [&](const std::string& name, const std::string& bytes) {
            zip_source_t* src = zip_source_buffer(z, bytes.data(), bytes.size(), 0);
            REQUIRE(src);
            REQUIRE(zip_file_add(z, name.c_str(), src, ZIP_FL_ENC_UTF_8) >= 0);
        };
        for (const auto& [name, bytes] : good)
            add(name, bytes);
        add(extra, "x");
        REQUIRE(zip_close(z) == 0);
    };
    zipWith(dir / "ok.bwp", "refs/fine.txt");
    CHECK(project::read(dir / "ok.bwp").files.at("refs/fine.txt") == "x");
    zipWith(dir / "hidden.bwp", "__MACOSX/.DS_Store");
    CHECK(project::read(dir / "hidden.bwp").files.count("__MACOSX/.DS_Store") == 0);
    for (const char* bad : {"../evil", "/abs", "refs/../../evil", "a\\b"}) {
        INFO(bad);
        zipWith(dir / "bad.bwp", bad);
        CHECK_THROWS_AS(project::read(dir / "bad.bwp"), ProjectError);
    }
    writeBytes(dir / "junk.bwp", "PK\x03\x04 but not a zip");
    CHECK_THROWS_AS(project::read(dir / "junk.bwp"), ProjectError);
    CHECK_THROWS_AS(project::read(dir / "missing.bwp"), ProjectError);
    CHECK_THROWS_AS(project::read(bwtest::dataDir() / "golden" / "tux.ans"), ProjectError);
}

TEST_CASE("a directory save keeps hidden files and removes what the project dropped", "[project][io]") {
    const fs::path dir = scratch("dirsave") / "piece";
    Project p = everything();
    project::writeDirectory(p, dir);
    writeBytes(dir / ".git" / "HEAD", "ref: refs/heads/main\n");
    writeBytes(dir / "refs" / ".keep", "");
    CHECK(project::read(dir) == p);  // hidden files are not part of it

    // drop a layer, a snapshot, the notes and a ref, then save over it
    const LayerId dropped = p.layers[1].props.id;
    p.layers.remove(dropped);
    p.snapshots.pop_back();
    p.notes.clear();
    p.files.erase("refs/sketch.png");
    project::writeDirectory(p, dir);
    CHECK_FALSE(fs::exists(dir / "layers" / (std::to_string(dropped) + ".cells")));
    CHECK_FALSE(fs::exists(dir / "layers" / (std::to_string(dropped) + ".json")));
    CHECK_FALSE(fs::exists(dir / "snapshots" / "r2.ans"));
    CHECK_FALSE(fs::exists(dir / "notes.md"));
    CHECK_FALSE(fs::exists(dir / "refs" / "sketch.png"));
    CHECK(fs::exists(dir / "refs" / ".keep"));
    CHECK(fs::exists(dir / ".git" / "HEAD"));
    CHECK(project::read(dir) == p);
    // nothing left behind from writing
    for (const auto& e : fs::recursive_directory_iterator(dir))
        CHECK(e.path().filename().string().find("bw-tmp") == std::string::npos);

    // the files directory removed once empty
    p.files.erase("scripts/" + std::to_string(p.layers[3].props.id) + "/scene.py");
    p.files.erase("scripts/" + std::to_string(p.layers[3].props.id) + "/params.json");
    project::writeDirectory(p, dir);
    CHECK_FALSE(fs::exists(dir / "scripts"));
}

TEST_CASE("a directory save refuses a directory that isn't a project", "[project][io]") {
    const fs::path dir = scratch("notproject");
    writeBytes(dir / "holiday.jpg", "precious");
    CHECK_THROWS_AS(project::writeDirectory(everything(), dir), ProjectError);
    CHECK(bwtest::readFile(dir / "holiday.jpg") == "precious");
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 1);

    // an empty directory, or one with only hidden files, is fine
    const fs::path empty = scratch("emptydir");
    writeBytes(empty / ".git" / "HEAD", "x");
    project::writeDirectory(everything(), empty);
    CHECK(project::read(empty) == everything());

    // and a file in the way is an error, not overwritten
    writeBytes(dir / "file", "keep");
    CHECK_THROWS_AS(project::writeDirectory(everything(), dir / "file"), ProjectError);
    CHECK(bwtest::readFile(dir / "file") == "keep");
}

TEST_CASE("a directory save refuses an empty path before writing anything", "[project][io][paths]") {
    // an empty path is no directory, not the working one
    const fs::path dir = scratch("emptypath");
    writeBytes(dir / "notes.md", "precious");
    struct Cwd {
        fs::path was = fs::current_path();
        ~Cwd() { fs::current_path(was); }
    } cwd;
    fs::current_path(dir);
    CHECK_THROWS_AS(project::writeDirectory(everything(), fs::path{}), ProjectError);
    CHECK(bwtest::readFile(dir / "notes.md") == "precious");
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 1);
}

TEST_CASE("a directory with a symbolic link is refused", "[project][io]") {
    const fs::path dir = scratch("symlink") / "piece";
    project::writeDirectory(everything(), dir);
    fs::create_symlink("/etc/hostname", dir / "refs" / "link.txt");
    CHECK_THROWS_AS(project::read(dir), ProjectError);
}

// ---- flattening and snapshots --------------------------------------------------

TEST_CASE("a project flattens to .ans through the compositor", "[project][flatten]") {
    Project p;
    p.layers = shadeUnderPixels();
    p.ice = true;
    Sauce s = Sauce::fresh(80, 26);
    s.title = "steel";
    s.author = "funkpower";
    p.sauce = s;

    const fs::path dir = scratch("flatten");
    project::writeZip(p, dir / "steel.bwp");
    const Project back = project::read(dir / "steel.bwp");
    const AnsiDocument doc = project::toAnsi(back);
    CHECK(doc.canvas == flatten(p.layers, {}));
    CHECK(doc.ice);
    REQUIRE(doc.sauce);
    CHECK(doc.sauce->title == "steel");
    CHECK(doc.sauce->tinfo1 == 80);
    CHECK(doc.sauce->tinfo2 == 26);

    // the .ans holds the composite, and the composite has no specks
    const AnsiOutput out = writeAnsi(doc);
    CHECK_FALSE(out.lossy());
    const AnsiDocument parsed = parseAnsi(out.bytes);
    const auto rep = lint::checkLadder(parsed.canvas, *findRamp("steel"));
    for (const auto& f : rep.findings)
        UNSCOPED_INFO(f.message);
    CHECK(rep.clean());

    // hidden layers and iCE off flatten as the compositor has them
    Project off = back;
    off.ice = false;
    off.layers[1].props.visible = false;
    CompositeOptions o;
    o.ice = false;
    CHECK(project::toAnsi(off).canvas == flatten(off.layers, o));
    CHECK_FALSE(project::toAnsi(off).ice);
    CHECK_FALSE(project::toAnsi(off).sauce->ice());
}

TEST_CASE("a flat .ans opens as a project and writes back the same bytes", "[project][flatten]") {
    const std::string file = bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans");
    const AnsiDocument doc = parseAnsi(file);
    const Project p = project::fromAnsi(doc);
    REQUIRE(p.layers.size() == 1);
    CHECK(p.layers[0].props.name == "Background");
    CHECK(p.layers[0].props.kind == LayerKind::Cells);
    const Project back = project::decode(project::encode(p));
    CHECK(back == p);
    CHECK(writeAnsi(project::toAnsi(back)).bytes == file);

    // without SAUCE, flatten writes a fresh record
    AnsiDocument bare = doc;
    bare.sauce.reset();
    const AnsiDocument flat = project::toAnsi(project::fromAnsi(bare));
    REQUIRE(flat.sauce);
    CHECK(flat.sauce->tinfo1 == doc.canvas.width());
}

TEST_CASE("snapshots are numbered rounds of the flattened project", "[project][snapshots]") {
    Project p;
    p.layers = shadeUnderPixels();
    const AnsiOutput r1 = project::takeSnapshot(p);
    REQUIRE(p.snapshots.size() == 1);
    CHECK(p.snapshots[0].round == 1);
    CHECK(p.snapshots[0].ans == r1.bytes);
    CHECK(r1.bytes == writeAnsi(project::toAnsi(p)).bytes);
    CHECK(project::toAnsi(p).canvas == flatten(p.layers));

    p.layers[1].props.visible = false;
    project::takeSnapshot(p);
    REQUIRE(p.snapshots.size() == 2);
    CHECK(p.snapshots[1].round == 2);
    const AnsiDocument field = project::toAnsi(p);
    CHECK(field.canvas == p.layers[0].cells);  // the hidden layer isn't in it
    CHECK(p.snapshots[1].ans == writeAnsi(field).bytes);
    CHECK(p.snapshots[1].ans != p.snapshots[0].ans);

    // rounds sort by number, and a new one goes past the last
    p.snapshots.push_back({9, "nine"});
    const Entries e = [&] {
        Entries x = project::encode(p);
        x["snapshots/r10.ans"] = "ten";
        return x;
    }();
    Project back = project::decode(e);
    REQUIRE(back.snapshots.size() == 4);
    CHECK(back.snapshots[2].round == 9);
    CHECK(back.snapshots[3].round == 10);
    project::takeSnapshot(back);
    CHECK(back.snapshots.back().round == 11);

    // a snapshot of a glyph with no CP437 byte says so
    Project lossy;
    lossy.layers = LayerStack(2, 1);
    Layer l = makeLayer(LayerKind::Cells, 2, 1);
    l.cells.at(0, 0) = cell(U'★', White, Black);
    lossy.layers.add(std::move(l));
    CHECK(project::takeSnapshot(lossy).unwritable.size() == 1);
    CHECK(lossy.snapshots.size() == 1);
}

// ---- safety: links, budgets, entry types, collisions, limits ------------------

namespace {

using project::kMaxEntries, project::kMaxLayers, project::kMaxTotalCells;

// Writes `entries` as a zip; an entry named in `modes` gets that UNIX mode
// (file type and permissions) in its external attributes.
void zipEntries(const fs::path& path, const Entries& entries, const std::map<std::string, zip_uint32_t>& modes = {}) {
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    REQUIRE(z);
    for (const auto& [name, bytes] : entries) {
        zip_source_t* src = zip_source_buffer(z, bytes.data(), bytes.size(), 0);
        REQUIRE(src);
        const zip_int64_t i = zip_file_add(z, name.c_str(), src, ZIP_FL_ENC_UTF_8);
        REQUIRE(i >= 0);
        if (const auto m = modes.find(name); m != modes.end())
            REQUIRE(zip_file_set_external_attributes(z, zip_uint64_t(i), 0, ZIP_OPSYS_UNIX, m->second << 16) == 0);
    }
    REQUIRE(zip_close(z) == 0);
}

// The entries of a project with `layers` layers of w × h blank cells, all
// sharing one .cells payload.
Entries blankLayers(std::size_t layers, int w, int h) {
    Project one;
    one.layers = LayerStack(w, h);
    one.layers.add(makeLayer(LayerKind::Cells, w, h));
    const Entries base = project::encode(one);
    Entries out;
    QJsonArray order;
    for (std::size_t i = 1; i <= layers; ++i) {
        QJsonObject l = object(base.at("layers/1.json"));
        l["id"] = qint64(i);
        out["layers/" + std::to_string(i) + ".json"] = json(l);
        out["layers/" + std::to_string(i) + ".cells"] = base.at("layers/1.cells");
        order.append(qint64(i));
    }
    QJsonObject m = object(base.at("manifest.json"));
    m["layers"] = order;
    m["nextLayerId"] = qint64(layers + 1);
    out["manifest.json"] = json(m);
    return out;
}

} // namespace

TEST_CASE("a directory save doesn't follow symbolic links", "[project][io][safety]") {
    const fs::path root = scratch("links");
    const fs::path dir = root / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    writeBytes(root / "victim", "precious");

    // a hidden link where a predictable temporary file would go
    fs::create_symlink("../victim", dir / ".manifest.json.bw-tmp");
    CHECK(project::read(dir) == p);   // the reader skips it, as any hidden entry
    project::writeDirectory(p, dir);  // the writer leaves it alone
    CHECK(bwtest::readFile(root / "victim") == "precious");
    CHECK(fs::is_symlink(dir / ".manifest.json.bw-tmp"));
    fs::remove(dir / ".manifest.json.bw-tmp");

    // a linked directory isn't written through
    fs::create_directories(root / "outside");
    fs::remove_all(dir / "refs");
    fs::create_directory_symlink("../outside", dir / "refs");
    CHECK_THROWS_AS(project::writeDirectory(p, dir), ProjectError);
    CHECK(fs::is_empty(root / "outside"));
    fs::remove(dir / "refs");

    // nor is a linked file
    fs::remove(dir / "notes.md");
    fs::create_symlink("../victim", dir / "notes.md");
    CHECK_THROWS_AS(project::writeDirectory(p, dir), ProjectError);
    CHECK(bwtest::readFile(root / "victim") == "precious");
    CHECK(fs::is_symlink(dir / "notes.md"));
}

TEST_CASE("a project over the cell budget is refused before its cells are decoded", "[project][limits][safety]") {
    // nine full-size layers: each within the limits, together over the budget
    const std::size_t full = std::size_t(kMaxColumns) * kMaxRows;
    const std::size_t over = kMaxTotalCells / full + 1;
    const Entries big = blankLayers(over, kMaxColumns, kMaxRows);
    CHECK_THROWS_AS(project::decode(big), ProjectError);
    const fs::path dir = scratch("budget");
    zipEntries(dir / "big.bwp", big);
    CHECK(fs::file_size(dir / "big.bwp") < (std::size_t{1} << 20));
    CHECK_THROWS_AS(project::read(dir / "big.bwp"), ProjectError);

    // too many layers, however small
    CHECK(project::decode(blankLayers(kMaxLayers, 1, 1)).layers.size() == kMaxLayers);
    CHECK_THROWS_AS(project::decode(blankLayers(kMaxLayers + 1, 1, 1)), ProjectError);

    // too many entries, however small
    Entries many = blankLayers(1, 1, 1);
    for (std::size_t i = 0; many.size() < kMaxEntries; ++i)
        many["refs/" + std::to_string(i)] = "";
    CHECK_NOTHROW(project::decode(many));
    many["refs/one-more"] = "";
    CHECK_THROWS_AS(project::decode(many), ProjectError);
    zipEntries(dir / "many.bwp", many);
    CHECK_THROWS_AS(project::read(dir / "many.bwp"), ProjectError);
}

TEST_CASE("a project flattens with its own iCE colours", "[project][flatten]") {
    const auto one = [](Cell c, bool ice) {
        Project p;
        p.layers = LayerStack(1, 1);
        Layer l = makeLayer(LayerKind::Cells, 1, 1);
        l.cells.at(0, 0) = c;
        p.layers.add(std::move(l));
        p.ice = ice;
        return p;
    };
    for (const char32_t ch : {U' ', U'X'}) {
        INFO(int(ch));
        // iCE on: blink is the bright background
        Project on = one(cell(ch, White, Black, attr::Blink), true);
        const AnsiOutput out = project::takeSnapshot(on);
        CHECK_FALSE(out.lossy());
        const AnsiDocument parsed = parseAnsi(out.bytes);
        CHECK(parsed.ice);
        CHECK(parsed.canvas.at(0, 0).bg.paletteIndex() == 8);
        CHECK(project::toAnsi(on).canvas.at(0, 0) == iceCell(cell(ch, White, Black, attr::Blink), true));

        // iCE off: a bright background is blink on the dark one
        Project off = one(cell(ch, White, LightRed), false);
        const AnsiOutput offOut = project::takeSnapshot(off);
        CHECK_FALSE(offOut.lossy());
        const AnsiDocument offParsed = parseAnsi(offOut.bytes);
        CHECK_FALSE(offParsed.ice);
        CHECK(offParsed.canvas.at(0, 0).bg.paletteIndex() == Red);
        CHECK(offParsed.canvas.at(0, 0).attrs == attr::Blink);
    }
}

TEST_CASE("a flat file with bright backgrounds and no iCE opens as the editor opens it", "[project][flatten]") {
    // writeAnsi writes it with iCE on and drops the blink; so does a project
    AnsiDocument doc;
    doc.ice = false;
    doc.sauce.reset();
    doc.canvas = Canvas(2, 1);
    doc.canvas.at(0, 0) = cell(U'X', White, Red, attr::Blink);
    doc.canvas.at(1, 0) = cell(U'Y', White, LightRed);
    const AnsiOutput want = writeAnsi(doc);
    REQUIRE(want.blinkDropped == 1);
    Opened opened;
    const Project p = project::fromAnsi(doc, &opened);
    CHECK(opened.iceInferred);
    CHECK(opened.blinkDropped == 1);
    CHECK(p.ice);
    CHECK(writeAnsi(project::toAnsi(p)).bytes == want.bytes);
}

TEST_CASE("a zip's links and special files are refused", "[project][io][safety]") {
    const fs::path dir = scratch("ziptypes");
    Entries e = project::encode(everything());
    e["refs/link.txt"] = "../target";
    zipEntries(dir / "plain.bwp", e, {{"refs/link.txt", 0100644u}});
    CHECK(project::read(dir / "plain.bwp").files.at("refs/link.txt") == "../target");
    for (const zip_uint32_t mode : {0120777u, 0010644u, 0020644u, 0060644u, 0140755u}) {
        INFO(mode);
        zipEntries(dir / "bad.bwp", e, {{"refs/link.txt", mode}});
        CHECK_THROWS_AS(project::read(dir / "bad.bwp"), ProjectError);
    }
    // hidden ones too, and a directory entry that is a link
    Entries hidden = project::encode(everything());
    hidden[".link"] = "/etc";
    zipEntries(dir / "hidden.bwp", hidden, {{".link", 0120777u}});
    CHECK_THROWS_AS(project::read(dir / "hidden.bwp"), ProjectError);
    Entries d = project::encode(everything());
    d["refs/sub/"] = "";
    zipEntries(dir / "dir.bwp", d, {{"refs/sub/", 0040755u}});
    CHECK_NOTHROW(project::read(dir / "dir.bwp"));
    zipEntries(dir / "dirlink.bwp", d, {{"refs/sub/", 0120777u}});
    CHECK_THROWS_AS(project::read(dir / "dirlink.bwp"), ProjectError);
}

TEST_CASE("paths that are one file on a case-insensitive disk are refused", "[project][paths][safety]") {
    const Entries good = project::encode(everything());
    for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{
             {"MANIFEST.JSON", ""},
             {"refs/A", "refs/a"},
             {"refs/x", "refs/x/y"},
             {"refs/X/y", "refs/x/z"},
             {"refs/Notes", "refs/notes/n"},
             {"refs/\xC3\xA9", "refs/e\xCC\x81"},  // é composed and decomposed
             {"refs/\xC3\x9F", "refs/ss"},          // ß, folded in full
             {"refs/\xEF\xAC\x81", "refs/fi"},      // the fi ligature
             {"refs/\xC4\xB1", "refs/I"},           // dotless i
             {"refs/\xE1\xBA\x9E", "refs/\xC3\x9F"},   // capital and small sharp s
             {"refs/\xE1\xBA\x9E", "refs/ss"},
             {"refs/\xE1\xBA\x9E", "refs/SS"},
             {"LAYERS/9.txt", ""},
         }) {
        INFO(a << " " << b);
        Entries e = good;
        e[a] = "keep";
        if (!b.empty())
            e[b] = "keep";
        CHECK_THROWS_AS(project::decode(e), ProjectError);
        const fs::path dir = scratch("collide");
        zipEntries(dir / "c.bwp", e);
        CHECK_THROWS_AS(project::read(dir / "c.bwp"), ProjectError);

        Project p = everything();
        p.files[a] = "keep";
        if (!b.empty())
            p.files[b] = "keep";
        CHECK_THROWS_AS(project::encode(p), ProjectError);
        CHECK_THROWS_AS(project::writeZip(p, dir / "w.bwp"), ProjectError);
        CHECK_FALSE(fs::exists(dir / "w.bwp"));
        CHECK_THROWS_AS(project::writeDirectory(p, dir / "w"), ProjectError);
        CHECK_FALSE(fs::exists(dir / "w"));
    }
    // case alone is fine where nothing else is that name
    Project p = everything();
    p.files["refs/Moon.png"] = "m";
    CHECK(project::decode(project::encode(p)) == p);
}

namespace {

// Unicode 16.0 CaseFolding.txt, status F: the full folds that differ from
// the simple ones (Qt's toCaseFolded). Every other code point's full fold is
// its simple fold.
const std::pair<char32_t, std::u16string_view> kFullFolds[] = {
    {0x00DF, u"\u0073\u0073"}, {0x0130, u"\u0069\u0307"}, {0x0149, u"\u02BC\u006E"},
    {0x01F0, u"\u006A\u030C"}, {0x0390, u"\u03B9\u0308\u0301"}, {0x03B0, u"\u03C5\u0308\u0301"},
    {0x0587, u"\u0565\u0582"}, {0x1E96, u"\u0068\u0331"}, {0x1E97, u"\u0074\u0308"},
    {0x1E98, u"\u0077\u030A"}, {0x1E99, u"\u0079\u030A"}, {0x1E9A, u"\u0061\u02BE"},
    {0x1E9E, u"\u0073\u0073"}, {0x1F50, u"\u03C5\u0313"}, {0x1F52, u"\u03C5\u0313\u0300"},
    {0x1F54, u"\u03C5\u0313\u0301"}, {0x1F56, u"\u03C5\u0313\u0342"}, {0x1F80, u"\u1F00\u03B9"},
    {0x1F81, u"\u1F01\u03B9"}, {0x1F82, u"\u1F02\u03B9"}, {0x1F83, u"\u1F03\u03B9"},
    {0x1F84, u"\u1F04\u03B9"}, {0x1F85, u"\u1F05\u03B9"}, {0x1F86, u"\u1F06\u03B9"},
    {0x1F87, u"\u1F07\u03B9"}, {0x1F88, u"\u1F00\u03B9"}, {0x1F89, u"\u1F01\u03B9"},
    {0x1F8A, u"\u1F02\u03B9"}, {0x1F8B, u"\u1F03\u03B9"}, {0x1F8C, u"\u1F04\u03B9"},
    {0x1F8D, u"\u1F05\u03B9"}, {0x1F8E, u"\u1F06\u03B9"}, {0x1F8F, u"\u1F07\u03B9"},
    {0x1F90, u"\u1F20\u03B9"}, {0x1F91, u"\u1F21\u03B9"}, {0x1F92, u"\u1F22\u03B9"},
    {0x1F93, u"\u1F23\u03B9"}, {0x1F94, u"\u1F24\u03B9"}, {0x1F95, u"\u1F25\u03B9"},
    {0x1F96, u"\u1F26\u03B9"}, {0x1F97, u"\u1F27\u03B9"}, {0x1F98, u"\u1F20\u03B9"},
    {0x1F99, u"\u1F21\u03B9"}, {0x1F9A, u"\u1F22\u03B9"}, {0x1F9B, u"\u1F23\u03B9"},
    {0x1F9C, u"\u1F24\u03B9"}, {0x1F9D, u"\u1F25\u03B9"}, {0x1F9E, u"\u1F26\u03B9"},
    {0x1F9F, u"\u1F27\u03B9"}, {0x1FA0, u"\u1F60\u03B9"}, {0x1FA1, u"\u1F61\u03B9"},
    {0x1FA2, u"\u1F62\u03B9"}, {0x1FA3, u"\u1F63\u03B9"}, {0x1FA4, u"\u1F64\u03B9"},
    {0x1FA5, u"\u1F65\u03B9"}, {0x1FA6, u"\u1F66\u03B9"}, {0x1FA7, u"\u1F67\u03B9"},
    {0x1FA8, u"\u1F60\u03B9"}, {0x1FA9, u"\u1F61\u03B9"}, {0x1FAA, u"\u1F62\u03B9"},
    {0x1FAB, u"\u1F63\u03B9"}, {0x1FAC, u"\u1F64\u03B9"}, {0x1FAD, u"\u1F65\u03B9"},
    {0x1FAE, u"\u1F66\u03B9"}, {0x1FAF, u"\u1F67\u03B9"}, {0x1FB2, u"\u1F70\u03B9"},
    {0x1FB3, u"\u03B1\u03B9"}, {0x1FB4, u"\u03AC\u03B9"}, {0x1FB6, u"\u03B1\u0342"},
    {0x1FB7, u"\u03B1\u0342\u03B9"}, {0x1FBC, u"\u03B1\u03B9"}, {0x1FC2, u"\u1F74\u03B9"},
    {0x1FC3, u"\u03B7\u03B9"}, {0x1FC4, u"\u03AE\u03B9"}, {0x1FC6, u"\u03B7\u0342"},
    {0x1FC7, u"\u03B7\u0342\u03B9"}, {0x1FCC, u"\u03B7\u03B9"}, {0x1FD2, u"\u03B9\u0308\u0300"},
    {0x1FD3, u"\u03B9\u0308\u0301"}, {0x1FD6, u"\u03B9\u0342"}, {0x1FD7, u"\u03B9\u0308\u0342"},
    {0x1FE2, u"\u03C5\u0308\u0300"}, {0x1FE3, u"\u03C5\u0308\u0301"}, {0x1FE4, u"\u03C1\u0313"},
    {0x1FE6, u"\u03C5\u0342"}, {0x1FE7, u"\u03C5\u0308\u0342"}, {0x1FF2, u"\u1F7C\u03B9"},
    {0x1FF3, u"\u03C9\u03B9"}, {0x1FF4, u"\u03CE\u03B9"}, {0x1FF6, u"\u03C9\u0342"},
    {0x1FF7, u"\u03C9\u0342\u03B9"}, {0x1FFC, u"\u03C9\u03B9"}, {0xFB00, u"\u0066\u0066"},
    {0xFB01, u"\u0066\u0069"}, {0xFB02, u"\u0066\u006C"}, {0xFB03, u"\u0066\u0066\u0069"},
    {0xFB04, u"\u0066\u0066\u006C"}, {0xFB05, u"\u0073\u0074"}, {0xFB06, u"\u0073\u0074"},
    {0xFB13, u"\u0574\u0576"}, {0xFB14, u"\u0574\u0565"}, {0xFB15, u"\u0574\u056B"},
    {0xFB16, u"\u057E\u0576"}, {0xFB17, u"\u0574\u056D"},
};

std::string utf8(const QString& s) { return s.toStdString(); }

} // namespace

TEST_CASE("paths are one under full case folding, for every code point", "[project][paths][safety]") {
    for (const auto& [cp, fold] : kFullFolds) {
        const QString c = QString::fromUcs4(&cp, 1);
        const QString f = QString::fromUtf16(fold.data(), qsizetype(fold.size()));
        INFO(utf8(c) << " " << utf8(f));
        CHECK(project::samePath(utf8(c), utf8(f)));
        CHECK(project::samePath(utf8(c), utf8(c.toCaseFolded())));
    }
    // every assigned code point is one with its simple fold and its full
    // upper and lower case, so a disk folding any of those ways is covered
    std::vector<std::string> apart;
    std::size_t swept = 0;
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        const QChar::Category cat = QChar::category(cp);
        if (cat == QChar::Other_NotAssigned || cat == QChar::Other_PrivateUse || cat == QChar::Other_Surrogate)
            continue;
        ++swept;
        const QString c = QString::fromUcs4(&cp, 1);
        const std::string s = utf8(c);
        for (const QString& other : {c.toCaseFolded(), c.toUpper(), c.toLower()})
            if (!project::samePath(s, utf8(other)))
                apart.push_back(s + " " + utf8(other));
    }
    CHECK(swept > 150'000);  // Unicode 16 assigns over 150 000 outside private use
    INFO(apart.size() << " apart, first: " << (apart.empty() ? "" : apart.front()));
    CHECK(apart.empty());
    CHECK_FALSE(project::samePath("refs/a", "refs/b"));
    CHECK_FALSE(project::samePath("refs/\xC3\x9F", "refs/s"));
}

TEST_CASE("a flat file's SAUCE survives a project as an .ans save keeps it", "[project][flatten][sauce]") {
    AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"));
    REQUIRE(doc.sauce);
    for (const std::uint8_t flags : {std::uint8_t(0x80), std::uint8_t(0xE0 | 0x06 | 0x18)}) {
        INFO(int(flags));
        AnsiDocument d = doc;
        d.sauce->tinfo3 = 7;
        d.sauce->tinfo4 = 9;
        d.sauce->flags |= flags;
        const std::string want = writeAnsi(d).bytes;
        const Project p = project::fromAnsi(d);
        CHECK(writeAnsi(project::toAnsi(p)).bytes == want);
        CHECK(writeAnsi(project::toAnsi(project::decode(project::encode(p)))).bytes == want);
        const fs::path dir = scratch("sauce");
        project::writeZip(p, dir / "s.bwp");
        CHECK(writeAnsi(project::toAnsi(project::read(dir / "s.bwp"))).bytes == want);
    }
}

TEST_CASE("a writer refuses what a reader would refuse, before writing anything", "[project][limits][safety]") {
    const fs::path dir = scratch("writelimits");
    Project p = everything();
    p.files["refs/blob"] = std::string(project::kMaxEntryBytes + 1, 'x');
    CHECK_THROWS_AS(project::encode(p), ProjectError);
    CHECK_THROWS_AS(project::writeZip(p, dir / "blob.bwp"), ProjectError);
    CHECK_FALSE(fs::exists(dir / "blob.bwp"));
    CHECK_THROWS_AS(project::writeDirectory(p, dir / "blob"), ProjectError);
    CHECK_FALSE(fs::exists(dir / "blob"));
    p.files.erase("refs/blob");

    // a snapshot that takes a readable project over a limit isn't saved
    for (std::size_t i = 0; project::encode(p).size() < kMaxEntries; ++i)
        p.files["refs/f" + std::to_string(i)] = "";
    project::writeZip(p, dir / "full.bwp");
    project::writeDirectory(p, dir / "full");
    const std::string zipBefore = bwtest::readFile(dir / "full.bwp");
    for (const fs::path& path : {dir / "full.bwp", dir / "full"}) {
        INFO(path);
        Project q = project::read(path);
        CHECK(q == p);
        project::takeSnapshot(q);
        if (fs::is_directory(path))
            CHECK_THROWS_AS(project::writeDirectory(q, path), ProjectError);
        else
            CHECK_THROWS_AS(project::writeZip(q, path), ProjectError);
        CHECK(project::read(path) == p);
    }
    const bool same = bwtest::readFile(dir / "full.bwp") == zipBefore;
    CHECK(same);
    CHECK_FALSE(fs::exists(dir / "full" / "snapshots" / "r3.ans"));
}

namespace {

// `json`, an object, grown to `size` bytes by a key a newer minor version
// might add, holding a string.
std::string padded(const std::string& json, std::size_t size) {
    const std::string key = "\"zz\":\"\",";
    REQUIRE(json.size() + key.size() <= size);
    return "{\"zz\":\"" + std::string(size - json.size() - key.size(), 'x') + "\"," + json.substr(1);
}

} // namespace

TEST_CASE("a JSON entry over its limit is refused before it is parsed", "[project][limits][safety]") {
    const Entries good = project::encode(everything());
    const std::string layerJson = "layers/" + std::to_string(everything().layers[0].props.id) + ".json";
    REQUIRE(good.count(layerJson));
    const fs::path dir = scratch("jsonlimits");
    for (const std::string& name : {std::string("manifest.json"), layerJson}) {
        INFO(name);
        Entries e = good;
        e[name] = padded(good.at(name), project::kMaxJsonBytes);
        CHECK(project::decode(e) == everything());
        e[name] = padded(good.at(name), project::kMaxJsonBytes + 1);
        CHECK_THROWS_AS(project::decode(e), ProjectError);
        zipEntries(dir / "big.bwp", e);
        CHECK(fs::file_size(dir / "big.bwp") < (std::size_t{64} << 10));
        CHECK_THROWS_AS(project::read(dir / "big.bwp"), ProjectError);
    }

    // a writer refuses a layer whose JSON a reader would refuse
    Project p = everything();
    p.layers[0].props.name = std::string(project::kMaxJsonBytes, 'n');
    CHECK_THROWS_AS(project::encode(p), ProjectError);
    CHECK_THROWS_AS(project::writeZip(p, dir / "w.bwp"), ProjectError);
    CHECK_FALSE(fs::exists(dir / "w.bwp"));
}

TEST_CASE("a writer refuses SAUCE text a reader would refuse", "[project][limits][sauce]") {
    const fs::path dir = scratch("saucelimits");
    const std::vector<std::pair<std::string Sauce::*, std::size_t>> fields{
        {&Sauce::title, Sauce::kTitleLen}, {&Sauce::author, Sauce::kAuthorLen}, {&Sauce::group, Sauce::kGroupLen},
        {&Sauce::date, 8},                 {&Sauce::font, Sauce::kFontLen},
    };
    std::vector<Project> over;
    for (const auto& [field, len] : fields) {
        Project p = everything();
        (*p.sauce).*field = std::string(len, 'S');
        CHECK(project::decode(project::encode(p)) == p);
        (*p.sauce).*field = std::string(len + 1, 'S');
        over.push_back(p);
    }
    Project p = everything();
    p.sauce->comments = {std::string(Sauce::kCommentLen, 'c')};
    CHECK(project::decode(project::encode(p)) == p);
    p.sauce->comments = {std::string(Sauce::kCommentLen + 1, 'c')};
    over.push_back(p);
    p.sauce->comments.assign(Sauce::kMaxComments + 1, "c");
    over.push_back(p);
    for (std::size_t i = 0; i < over.size(); ++i) {
        INFO(i);
        CHECK_THROWS_AS(project::encode(over[i]), ProjectError);
        CHECK_THROWS_AS(project::writeZip(over[i], dir / "s.bwp"), ProjectError);
        CHECK_FALSE(fs::exists(dir / "s.bwp"));
    }
}

TEST_CASE("a name as long as a disk takes is written to a directory", "[project][io][paths]") {
    const fs::path dir = scratch("longname");
    Project p = everything();
    p.files["refs/" + std::string(255, 'n')] = "long";
    project::writeDirectory(p, dir / "piece");
    CHECK(project::read(dir / "piece") == p);
    p.notes = "saved again";
    project::writeDirectory(p, dir / "piece");
    CHECK(project::read(dir / "piece") == p);

    // a longer one no disk takes is refused by the writer and the reader
    const std::string longer = "refs/" + std::string(256, 'n');
    CHECK_FALSE(project::safeEntryPath(longer));
    Project q = everything();
    q.files[longer] = "x";
    CHECK_THROWS_AS(project::encode(q), ProjectError);
    Entries e = project::encode(everything());
    e[longer] = "x";
    zipEntries(dir / "long.bwp", e);
    CHECK_THROWS_AS(project::read(dir / "long.bwp"), ProjectError);
}

TEST_CASE("a path longer than a directory save can write is refused", "[project][io][paths]") {
    const fs::path dir = scratch("longpath");
    // 1024 bytes in all is written and read back
    std::string deep = "refs";
    while (deep.size() + 251 <= 1024)
        deep += "/" + std::string(250, 'p');
    deep += "/" + std::string(1024 - deep.size() - 1, 'q');
    REQUIRE(deep.size() == 1024);
    CHECK(project::safeEntryPath(deep));
    Project p = everything();
    p.files[deep] = "deep";
    project::writeDirectory(p, dir / "piece");
    CHECK(project::read(dir / "piece") == p);

    // a byte more, or a path no disk takes, is refused everywhere
    std::string past = "refs";
    for (int i = 0; i < 17; ++i)
        past += "/" + std::string(250, 'p');
    for (const std::string& longer : {deep + "x", past + "/x"}) {
        INFO(longer.size());
        CHECK_FALSE(project::safeEntryPath(longer));
        Project q = everything();
        q.files[longer] = "x";
        CHECK_THROWS_AS(project::encode(q), ProjectError);
        CHECK_THROWS_AS(project::writeZip(q, dir / "w.bwp"), ProjectError);
        CHECK_FALSE(fs::exists(dir / "w.bwp"));
        CHECK_THROWS_AS(project::writeDirectory(q, dir / "w"), ProjectError);
        CHECK_FALSE(fs::exists(dir / "w"));
        Entries e = project::encode(everything());
        e[longer] = "x";
        zipEntries(dir / "long.bwp", e);
        CHECK_THROWS_AS(project::read(dir / "long.bwp"), ProjectError);
    }
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("a first directory save that fails part way leaves nothing behind", "[project][io]") {
    const fs::path root = scratch("firstfail");
    Project p = everything();
    p.files["refs/big.bin"] = std::string(std::size_t{1} << 20, 'b');  // written after layers/ and notes.md
    // files can't grow past 256 KiB, so refs/big.bin fails part way
    const auto failingSave = [&](const fs::path& dir) {
        rlimit old{};
        REQUIRE(::getrlimit(RLIMIT_FSIZE, &old) == 0);
        rlimit low = old;
        low.rlim_cur = 256 << 10;
        const auto handler = std::signal(SIGXFSZ, SIG_IGN);
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &low) == 0);
        bool threw = false;
        try {
            project::writeDirectory(p, dir);
        } catch (const ProjectError&) {
            threw = true;
        }
        ::setrlimit(RLIMIT_FSIZE, &old);
        std::signal(SIGXFSZ, handler);
        CHECK(threw);
    };

    // a directory the save made is gone, so the next save can make it
    failingSave(root / "new");
    CHECK_FALSE(fs::exists(root / "new"));
    project::writeDirectory(p, root / "new");
    CHECK(project::read(root / "new") == p);

    // one that was there holds what it held
    writeBytes(root / "old" / ".git" / "HEAD", "x");
    failingSave(root / "old");
    std::vector<std::string> left;
    for (const auto& e : fs::recursive_directory_iterator(root / "old"))
        left.push_back(e.path().lexically_relative(root / "old").generic_string());
    std::sort(left.begin(), left.end());
    CHECK(left == std::vector<std::string>{".git", ".git/HEAD"});
    project::writeDirectory(p, root / "old");
    CHECK(project::read(root / "old") == p);

    // nor do the directories it made on the way to one
    failingSave(root / "a" / "b" / "c");
    CHECK_FALSE(fs::exists(root / "a"));
    CHECK(fs::is_directory(root));
    project::writeDirectory(p, root / "a" / "b" / "c");
    CHECK(project::read(root / "a" / "b" / "c") == p);

    // and never one that was there before it, however the path reaches it
    fs::create_directory(root / "keep");
    failingSave(root / "gone" / ".." / "keep" / "piece");
    CHECK(fs::is_directory(root / "keep"));
    CHECK_FALSE(fs::exists(root / "keep" / "piece"));
    CHECK_FALSE(fs::exists(root / "gone"));
}

TEST_CASE("a first directory save that can't make a directory on the way leaves none behind", "[project][io]") {
    if (::geteuid() == 0)
        SKIP("root writes into any directory");
    const fs::path root = scratch("mkdirfail");
    // a directory made while `f` runs can't be written into; the mask is the
    // process's, so it is put back however `f` ends
    const auto masked = [](const auto& f) {
        struct Restore {
            mode_t mask;
            ~Restore() { ::umask(mask); }
        } restore{::umask(0222)};
        f();
    };
    const auto currentMask = [] {
        const mode_t m = ::umask(0);
        ::umask(m);
        return m;
    };
    const mode_t before = currentMask();
    CHECK_THROWS_AS(masked([] { throw std::bad_alloc(); }), std::bad_alloc);
    CHECK(currentMask() == before);

    // so the save makes a/ and is refused a/b
    bool threw = false;
    masked([&] {
        try {
            project::writeDirectory(everything(), root / "a" / "b" / "c");
        } catch (const ProjectError&) {
            threw = true;
        }
    });
    CHECK(threw);
    CHECK_FALSE(fs::exists(root / "a"));
    std::error_code ec;
    fs::permissions(root / "a", fs::perms::owner_all, fs::perm_options::add, ec);
}

TEST_CASE("a directory save never removes a file it wrote, whatever name the disk lists it under",
          "[project][io][safety]") {
    // a disk that folds names some other way than samePath lists a file the
    // save wrote under a name no entry folds to; a hard link made just
    // before the save looks for stale files stands in for one here
    const fs::path dir = scratch("samefile") / "piece";
    project::writeDirectory(everything(), dir);
    Project p = everything();
    p.notes = "saved again";
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    bool foldedLink = false;
    project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
        if (step != "scan")
            return;
        CHECK(at == dir);
        fs::create_hard_link(dir / "notes.md", dir / "NOTES.alias");
        writeBytes(dir / "stray", "x");  // not a file the save wrote
        fs::create_symlink("notes.md", dir / "link");  // a link to one is not one
        // nor under a name that folds to an entry's (a case-insensitive disk
        // won't take it)
        std::error_code ec;
        fs::create_symlink("notes.md", dir / "NOTES.MD", ec);
        foldedLink = !ec;
    };
    project::writeDirectory(p, dir);
    project::testing::directoryStep = nullptr;
    CHECK(fs::equivalent(dir / "NOTES.alias", dir / "notes.md"));
    CHECK_FALSE(fs::exists(dir / "stray"));
    CHECK_FALSE(fs::exists(fs::symlink_status(dir / "link")));
    if (foldedLink)
        CHECK_FALSE(fs::exists(fs::symlink_status(dir / "NOTES.MD")));
    Project kept = p;
    kept.files["NOTES.alias"] = "saved again";
    CHECK(project::read(dir) == kept);
}

TEST_CASE("a directory save removes a link put at an entry's own name", "[project][io][safety]") {
    // between the save writing notes.md and looking for stale files, someone
    // moves it aside and puts a link to it at notes.md: the file moved aside
    // is the one the save wrote, the link isn't, whatever its name
    const fs::path dir = scratch("exactlink") / "piece";
    project::writeDirectory(everything(), dir);
    Project p = everything();
    p.notes = "saved again";
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
        if (step != "scan")
            return;
        fs::rename(at / "notes.md", at / "moved");
        fs::create_symlink("moved", at / "notes.md");
    };
    project::writeDirectory(p, dir);
    project::testing::directoryStep = nullptr;
    CHECK_FALSE(fs::exists(fs::symlink_status(dir / "notes.md")));
    CHECK(bwtest::readFile(dir / "moved") == "saved again");
    Project kept = p;
    kept.notes.clear();
    kept.files["moved"] = "saved again";
    CHECK(project::read(dir) == kept);
}

TEST_CASE("a directory save keeps the file it wrote when another takes its name", "[project][io][safety]") {
    // between the save writing notes.md and looking for stale files, someone
    // moves it aside and puts another file at notes.md; the one moved aside
    // is still the one the save wrote
    const fs::path dir = scratch("renamed") / "piece";
    project::writeDirectory(everything(), dir);
    Project p = everything();
    p.notes = "saved again";
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
        if (step == "wrote" && at == dir / "notes.md") {
            fs::rename(at, dir / "moved");
            writeBytes(at, "someone else's");
        }
    };
    project::writeDirectory(p, dir);
    project::testing::directoryStep = nullptr;
    REQUIRE(fs::exists(dir / "moved"));
    CHECK(bwtest::readFile(dir / "moved") == "saved again");
    CHECK(bwtest::readFile(dir / "notes.md") == "someone else's");
}

namespace {

// Every path under `dir`, relative to it, with each file's bytes.
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        out[e.path().lexically_relative(dir).generic_string()] =
            e.is_regular_file() ? bwtest::readFile(e.path()) : std::string("<dir>");
    return out;
}

// `p` with an existing layer's cells changed, a new layer, and new notes:
// a save of it rewrites files the old manifest lists and adds two it doesn't.
Project edited(Project p) {
    p.layers[0].cells.at(0, 0) = cell(U'#', White, Red);
    Layer added = makeLayer(LayerKind::Cells, 3, 2, "added", 1, 1);
    added.cells.at(1, 1) = cell(U'@', Yellow, Blue);
    p.layers.add(std::move(added));
    p.notes = "saved again\n";
    return p;
}

} // namespace

TEST_CASE("a directory save over a project that fails before replacing a file leaves it as it was",
          "[project][io]") {
    if (::geteuid() == 0)
        SKIP("root writes into a read-only directory");
    const fs::path dir = scratch("overfail") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    const Project q = edited(p);

    // refs/ comes after layers/, log.md and notes.md, and can't be written
    REQUIRE(::chmod((dir / "refs").c_str(), 0555) == 0);
    CHECK_THROWS_AS(project::writeDirectory(q, dir), ProjectError);
    ::chmod((dir / "refs").c_str(), 0755);
    CHECK(tree(dir) == before);  // no new layer, no temporary, no file rewritten
    CHECK(project::read(dir) == p);

    project::writeDirectory(q, dir);
    CHECK(project::read(dir) == q);
}

TEST_CASE("a directory save over a project that fails while replacing files puts them back", "[project][io]") {
    const fs::path dir = scratch("overfail-rename") / "piece";
    Project p = everything();
    p.files["refs/a/x"] = "in a folder";
    project::writeDirectory(p, dir);
    const auto before = tree(dir);

    // refs/a is a file now, but the folder holding refs/a/x is still in the
    // way when its temporary is renamed, after the layers and the notes
    Project q = edited(p);
    q.files.erase("refs/a/x");
    q.files["refs/a"] = "a file";
    CHECK_THROWS_AS(project::writeDirectory(q, dir), ProjectError);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a directory save that fails at the manifest puts back every file and directory", "[project][io]") {
    const fs::path dir = scratch("overfail-manifest") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);

    // a reference replaced and one added in two new folders, both renamed
    // into place before the manifest's rename fails
    Project q = edited(p);
    q.files["refs/sketch.png"] = "redrawn";
    q.files["refs/new/sub/file"] = "added";
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    bool failed = false;
    project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
        if (step != "wrote" || failed)
            return;
        // the manifest's temporary is renamed last, so once it is the only
        // one left, taking it away fails that rename
        std::vector<fs::path> tmps;
        for (const auto& e : fs::recursive_directory_iterator(dir))
            if (e.path().filename().string().starts_with(".bw-tmp-"))
                tmps.push_back(e.path());
        if (tmps.size() == 1 && tmps[0].parent_path() == dir) {
            fs::remove(tmps[0]);
            failed = true;
        }
    };
    std::string error;
    try {
        project::writeDirectory(q, dir);
    } catch (const ProjectError& e) {
        error = e.what();
    }
    project::testing::directoryStep = nullptr;
    REQUIRE(failed);
    CHECK(error.find("manifest.json") != std::string::npos);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a directory save that can't remove a file the project dropped leaves the project as it was",
          "[project][io]") {
    if (::geteuid() == 0)
        SKIP("root writes into a read-only directory");
    const fs::path dir = scratch("stalefail") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);

    // a layer and the only reference dropped: nothing is written into refs/,
    // so only taking the reference away fails, after the layer's files are
    // gone from layers/ and the manifest is replaced
    Project q = edited(p);
    REQUIRE(q.layers.remove(3));
    q.files.erase("refs/sketch.png");
    REQUIRE(::chmod((dir / "refs").c_str(), 0555) == 0);
    CHECK_THROWS_AS(project::writeDirectory(q, dir), ProjectError);
    ::chmod((dir / "refs").c_str(), 0755);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);

    // and once it can, nothing it set aside is left behind
    project::writeDirectory(q, dir);
    CHECK(project::read(dir) == q);
    for (const auto& [name, bytes] : tree(dir))
        CHECK(name.find(".bw-") == std::string::npos);
    CHECK_FALSE(fs::exists(dir / "refs"));
}

TEST_CASE("a failed directory save never restores a file through a folder swapped for a link",
          "[project][io][safety]") {
    const fs::path root = scratch("swapped-parent");
    const fs::path dir = root / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    fs::create_directory(root / "outside");
    writeBytes(root / "outside" / "1.cells", "sentinel");

    // straight after layers/1.cells is replaced, layers/ is moved aside and
    // a link to another folder holding a 1.cells put in its place; the
    // temporaries moved with it are taken away, so the next rename fails
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    bool swapped = false;
    project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
        if (step != "wrote" || swapped || at != dir / "layers" / "1.cells")
            return;
        swapped = true;
        fs::rename(dir / "layers", root / "moved");
        for (const auto& e : fs::directory_iterator(root / "moved"))
            if (e.path().filename().string().starts_with(".bw-tmp-"))
                fs::remove(e.path());
        fs::create_directory_symlink("../outside", dir / "layers");
    };
    Project q = edited(p);
    q.layers.find(1)->cells.at(0, 0) = cell(U'%', Red, Black);  // so layers/1.cells is replaced by other bytes
    CHECK_THROWS_AS(project::writeDirectory(q, dir), ProjectError);
    project::testing::directoryStep = nullptr;
    REQUIRE(swapped);
    CHECK(tree(root / "outside") == std::map<std::string, std::string>{{"1.cells", "sentinel"}});
    // the folder the save wrote into gets its old bytes back, wherever it is
    std::map<std::string, std::string> layers;
    for (const auto& [name, bytes] : before)
        if (name.starts_with("layers/"))
            layers[name.substr(7)] = bytes;
    CHECK(tree(root / "moved") == layers);
}

TEST_CASE("a failed first directory save never removes through a parent swapped for a link",
          "[project][io][safety]") {
    const fs::path root = scratch("swapped-made");
    writeBytes(root / "outside" / "child" / "piece", "sentinel");

    // the save makes new/, new/child/ and new/child/piece/; once it has put
    // the first file in place, new/ is moved aside, a link to outside/ put
    // in its place, and the save fails
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    bool swapped = false;
    project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
        if (step != "wrote" || swapped)
            return;
        swapped = true;
        fs::rename(root / "new", root / "moved");
        fs::create_directory_symlink("outside", root / "new");
        throw ProjectError("failed on purpose");
    };
    CHECK_THROWS_AS(project::writeDirectory(everything(), root / "new" / "child" / "piece"), ProjectError);
    project::testing::directoryStep = nullptr;
    REQUIRE(swapped);
    CHECK(tree(root / "outside") ==
          std::map<std::string, std::string>{{"child", "<dir>"}, {"child/piece", "sentinel"}});
    // the directories the save made are gone from where they went, and the
    // link, which isn't one, stays
    CHECK(tree(root / "moved").empty());
    CHECK(fs::is_symlink(root / "new"));
}

TEST_CASE("a directory save never removes a file it wrote through a project swapped for a link",
          "[project][io][safety]") {
    const fs::path root = scratch("swapped-scan");
    const fs::path dir = root / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    // a link at a layer file's name, which a scan of outside/ would take
    // for a stale file
    fs::create_directories(root / "outside" / "layers");
    fs::create_symlink("gone", root / "outside" / "layers" / "1.cells");

    // between writing the manifest and looking for stale files, the project
    // is moved and a link to outside/ put where it was
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    bool swapped = false;
    project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
        if (step != "scan")
            return;
        swapped = true;
        fs::rename(dir, root / "moved");
        fs::create_directory_symlink("outside", dir);
    };
    Project q = p;
    q.notes = "saved again";
    project::writeDirectory(q, dir);
    project::testing::directoryStep = nullptr;
    REQUIRE(swapped);
    CHECK(fs::is_regular_file(fs::symlink_status(root / "moved" / "layers" / "1.cells")));
    CHECK(project::read(root / "moved") == q);
    CHECK(fs::is_symlink(root / "outside" / "layers" / "1.cells"));
}

TEST_CASE("a directory save that can't remove what it set aside leaves a project that reads",
          "[project][io][safety]") {
    // as in "a directory save removes a link put at an entry's own name",
    // with a layer dropped too, but nothing set aside can be removed once
    // the save is done
    const fs::path dir = scratch("asidefail") / "piece";
    project::writeDirectory(everything(), dir);
    Project p = everything();
    p.notes = "saved again";
    REQUIRE(p.layers.remove(3));
    struct Reset {
        ~Reset() {
            project::testing::directoryStep = nullptr;
            project::testing::systemCall = nullptr;
        }
    } reset;
    project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
        if (step != "scan")
            return;
        fs::rename(at / "notes.md", at / "moved");
        fs::create_symlink("moved", at / "notes.md");
    };
    std::vector<fs::path> refused;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call != "unlinkat" || !at.filename().string().starts_with(".bw-old-"))
            return 0;
        refused.push_back(at);
        return EACCES;
    };
    project::writeDirectory(p, dir);
    project::testing::directoryStep = nullptr;
    project::testing::systemCall = nullptr;
    // the link and the layer's two files stay under their hidden names
    REQUIRE(refused.size() == 3);
    std::size_t links = 0, inLayers = 0;
    for (const fs::path& at : refused) {
        CHECK(fs::exists(fs::symlink_status(at)));
        links += fs::is_symlink(fs::symlink_status(at));
        inLayers += at.parent_path() == dir / "layers";
    }
    CHECK(links == 1);
    CHECK(inLayers == 2);
    Project kept = p;
    kept.notes.clear();
    kept.files["moved"] = "saved again";
    CHECK(project::read(dir) == kept);

    // and the next save leaves them, as it leaves every hidden file it
    // didn't make
    project::writeDirectory(kept, dir);
    for (const fs::path& at : refused)
        CHECK(fs::exists(fs::symlink_status(at)));
    CHECK(project::read(dir) == kept);
}

TEST_CASE("a directory save without a no-replace rename never replaces a file put at its hidden name",
          "[project][io][safety]") {
    // renameat2 can't refuse to replace (as on a disk without it), and just
    // as the save is about to move a dropped file to a hidden name, someone
    // puts a file there
    const fs::path dir = scratch("noreplace") / "piece";
    project::writeDirectory(everything(), dir);
    Project p = everything();
    p.notes.clear();
    struct Reset {
        ~Reset() { project::testing::systemCall = nullptr; }
    } reset;
    fs::path raced;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call == "renameat2")
            return ENOSYS;
        if (call == "noreplace" && raced.empty()) {
            raced = at;
            writeBytes(at, "sentinel");
        }
        return 0;
    };
    project::writeDirectory(p, dir);
    project::testing::systemCall = nullptr;
    REQUIRE_FALSE(raced.empty());
    CHECK(raced.filename().string().starts_with(".bw-old-"));
    REQUIRE(fs::is_regular_file(fs::symlink_status(raced)));
    CHECK(bwtest::readFile(raced) == "sentinel");
    CHECK_FALSE(fs::exists(dir / "notes.md"));
    CHECK(project::read(dir) == p);

    // and on a disk without hard links either, where no rename can refuse
    // to replace, the save refuses the move and puts the directory back
    fs::remove(raced);
    project::writeDirectory(everything(), dir);
    const auto before = tree(dir);
    int links = 0;
    project::testing::systemCall = [&](std::string_view call, const fs::path&) {
        if (call == "renameat2")
            return ENOSYS;
        if (call == "linkat") {
            ++links;
            return EPERM;
        }
        return 0;
    };
    CHECK_THROWS_AS(project::writeDirectory(p, dir), ProjectError);
    project::testing::systemCall = nullptr;
    CHECK(links == 1);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == everything());
}

TEST_CASE("a failed directory save without a no-replace rename names what it couldn't put back",
          "[project][io][safety]") {
    // notes.md and log.md are dropped: the first is set aside, the second
    // can't be, and by the time the save puts notes.md back the disk can't
    // refuse to replace (no renameat2, no hard links)
    const fs::path dir = scratch("noreplace-back") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    Project q = p;
    q.notes.clear();
    q.files.erase("log.md");
    struct Reset {
        ~Reset() { project::testing::systemCall = nullptr; }
    } reset;
    int asides = 0;
    fs::path aside;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call == "renameat2" && at.filename().string().starts_with(".bw-old-")) {
            if (++asides == 1) {
                aside = at;
                return 0;
            }
            return EACCES;
        }
        if (call == "renameat2")
            return ENOSYS;
        if (call == "linkat")
            return EPERM;
        return 0;
    };
    std::string error;
    try {
        project::writeDirectory(q, dir);
    } catch (const ProjectError& e) {
        error = e.what();
    }
    project::testing::systemCall = nullptr;
    REQUIRE(asides == 2);
    INFO(error);
    CHECK(error.find("couldn't be put back") != std::string::npos);
    CHECK(error.find(aside.filename().string()) != std::string::npos);
    // nothing is lost: what wasn't put back is under the name the error gives
    const fs::path left = dir / aside.lexically_relative(dir).parent_path();
    const std::string name = fs::exists(left / "notes.md") ? "log.md" : "notes.md";
    CHECK_FALSE(fs::exists(left / name));
    CHECK(bwtest::readFile(aside) == (name == "notes.md" ? p.notes : p.files.at("log.md")));
}

TEST_CASE("a directory save whose second name for a file can't be taken back says so", "[project][io][safety]") {
    // the move to a hidden name goes by a hard link, then the old name can't
    // be removed, and nor can the new one
    const fs::path dir = scratch("twonames") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    Project q = p;
    q.notes.clear();
    struct Reset {
        ~Reset() { project::testing::systemCall = nullptr; }
    } reset;
    std::vector<fs::path> refused;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call == "renameat2")
            return ENOSYS;
        if (call == "unlinkat" && (at.filename() == "notes.md" || at.filename().string().starts_with(".bw-old-"))) {
            refused.push_back(at);
            return EACCES;
        }
        return 0;
    };
    std::string error;
    try {
        project::writeDirectory(q, dir);
    } catch (const ProjectError& e) {
        error = e.what();
    }
    project::testing::systemCall = nullptr;
    INFO(error);
    // the rollback tries the second name again
    REQUIRE(refused.size() == 3);
    CHECK(refused[0] == dir / "notes.md");
    CHECK(refused[2] == refused[1]);
    CHECK(error.find("couldn't be put back") != std::string::npos);
    CHECK(error.find(refused[1].string()) != std::string::npos);
    // the file is still at its name, and the second name is the same file
    CHECK(bwtest::readFile(dir / "notes.md") == p.notes);
    CHECK(fs::equivalent(dir / "notes.md", refused[1]));
    CHECK(project::read(dir) == p);
}

TEST_CASE("a directory save whose second name for a file the rollback can remove leaves nothing",
          "[project][io][safety]") {
    // the old name can't be removed, nor the second one at first; the
    // rollback removes the second
    const fs::path dir = scratch("twonames-back") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    Project q = p;
    q.notes.clear();
    struct Reset {
        ~Reset() { project::testing::systemCall = nullptr; }
    } reset;
    std::vector<fs::path> seconds;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call == "renameat2")
            return ENOSYS;
        if (call == "unlinkat" && at.filename() == "notes.md")
            return EACCES;
        if (call == "unlinkat" && at.filename().string().starts_with(".bw-old-")) {
            seconds.push_back(at);
            return seconds.size() == 1 ? EACCES : 0;
        }
        return 0;
    };
    std::string error;
    try {
        project::writeDirectory(q, dir);
    } catch (const ProjectError& e) {
        error = e.what();
    }
    project::testing::systemCall = nullptr;
    INFO(error);
    CHECK(error.find((dir / "notes.md").string()) != std::string::npos);
    CHECK(error.find("couldn't be put back") == std::string::npos);
    CHECK(seconds.size() == 2);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a directory save leaves a hidden file it didn't make, whatever its name", "[project][io][safety]") {
    const fs::path dir = scratch("old-named") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    writeBytes(dir / ".bw-old-backup", "sentinel");
    writeBytes(dir / "layers" / ".bw-old-1", "sentinel");
    project::writeDirectory(p, dir);
    CHECK(bwtest::readFile(dir / ".bw-old-backup") == "sentinel");
    CHECK(bwtest::readFile(dir / "layers" / ".bw-old-1") == "sentinel");

    // nor a file it wrote, moved to such a name before the save is done
    Project q = p;
    q.notes = "saved again\n";
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
        if (step == "scan")
            fs::rename(at / "notes.md", at / ".bw-old-kept");
    };
    project::writeDirectory(q, dir);
    project::testing::directoryStep = nullptr;
    REQUIRE(fs::exists(dir / ".bw-old-kept"));
    CHECK(bwtest::readFile(dir / ".bw-old-kept") == q.notes);
}

TEST_CASE("a failed directory save never removes a directory put where one it made was",
          "[project][io][safety]") {
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    // once it has put the first file in place, the directory `made` (one the
    // save made) is moved aside, an empty one put in its place, and the save
    // fails
    bool swapped = false;
    fs::path made, moved;
    const auto arm = [&] {
        project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
            if (step != "wrote" || swapped)
                return;
            swapped = true;
            fs::rename(made, moved);
            fs::create_directory(made);
            throw ProjectError("failed on purpose");
        };
    };

    SECTION("on the way to the project") {
        const fs::path root = scratch("replaced-made");
        made = root / "new";
        moved = root / "moved";
        arm();
        CHECK_THROWS_AS(project::writeDirectory(everything(), root / "new" / "child" / "piece"), ProjectError);
        REQUIRE(swapped);
        CHECK(fs::is_directory(fs::symlink_status(root / "new")));
        CHECK(fs::is_empty(root / "new"));
        // what the save made in the one it moved is gone from there
        CHECK(tree(root / "moved").empty());
    }
    SECTION("in the project") {
        const fs::path dir = scratch("replaced-made-in") / "piece";
        const Project p = everything();
        project::writeDirectory(p, dir);
        const auto before = tree(dir);
        Project q = p;
        q.files["refs/new/sub/file"] = "added";
        made = dir / "refs" / "new";
        moved = dir / "refs" / "moved";
        arm();
        CHECK_THROWS_AS(project::writeDirectory(q, dir), ProjectError);
        REQUIRE(swapped);
        auto expected = before;
        expected["refs/new"] = "<dir>";
        expected["refs/moved"] = "<dir>";
        CHECK(tree(dir) == expected);
    }
}

TEST_CASE("a directory save refuses to replace a file it can't read", "[project][io][safety]") {
    // a failure after notes.md is replaced would have no old bytes to give
    // it back, so the save never gets that far
    const fs::path dir = scratch("unreadable-old") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    Project q = p;
    q.notes = "saved again\n";
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    bool scanned = false;
    project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
        if (step != "scan")
            return;
        scanned = true;
        throw ProjectError("failed on purpose");
    };
    std::string error;
    const auto save = [&] {
        try {
            project::writeDirectory(q, dir);
        } catch (const ProjectError& e) {
            error = e.what();
        }
        project::testing::directoryStep = nullptr;
    };

    SECTION("a file it can't read") {
        if (::geteuid() == 0)
            SKIP("root reads any file");
        const auto before = tree(dir);
        REQUIRE(::chmod((dir / "notes.md").c_str(), 0) == 0);
        save();
        ::chmod((dir / "notes.md").c_str(), 0644);
        INFO(error);
        CHECK(error.find((dir / "notes.md").string()) != std::string::npos);
        CHECK_FALSE(scanned);
        CHECK(tree(dir) == before);
        CHECK(project::read(dir) == p);
    }
    SECTION("something that isn't a file or a directory") {
        fs::remove(dir / "notes.md");
        REQUIRE(::mkfifo((dir / "notes.md").c_str(), 0600) == 0);
        const auto before = tree(dir);
        save();
        INFO(error);
        CHECK(error.find((dir / "notes.md").string()) != std::string::npos);
        CHECK_FALSE(scanned);
        CHECK(fs::is_fifo(fs::symlink_status(dir / "notes.md")));
        CHECK(tree(dir) == before);
    }
}

TEST_CASE("a failed directory save removes a directory it made but couldn't look at", "[project][io][safety]") {
    // refs/new is made, and the look at it for its device and inode fails
    const fs::path dir = scratch("made-unseen") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    Project q = p;
    q.files["refs/new/file"] = "added";
    struct Reset {
        ~Reset() { project::testing::systemCall = nullptr; }
    } reset;
    int looks = 0;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        return call == "fstatat" && at == dir / "refs" / "new" && looks++ == 0 ? EIO : 0;
    };
    CHECK_THROWS_AS(project::writeDirectory(q, dir), ProjectError);
    project::testing::systemCall = nullptr;
    REQUIRE(looks >= 1);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a failed directory save names each thing of its own it couldn't remove", "[project][io][safety]") {
    const fs::path dir = scratch("cleanup-fail") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    struct Reset {
        ~Reset() {
            project::testing::directoryStep = nullptr;
            project::testing::systemCall = nullptr;
        }
    } reset;
    std::string error;
    const auto save = [&](const Project& q) {
        try {
            project::writeDirectory(q, dir);
        } catch (const ProjectError& e) {
            error = e.what();
        }
        project::testing::directoryStep = nullptr;
        project::testing::systemCall = nullptr;
    };
    const fs::path made = dir / "refs" / "new";
    Project q = p;
    q.files["refs/new/file"] = "added";
    const auto failAtScan = [] {
        project::testing::directoryStep = [](std::string_view step, const fs::path&) {
            if (step == "scan")
                throw ProjectError("failed on purpose");
        };
    };

    SECTION("a directory it made that can't be removed") {
        failAtScan();
        project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
            return call == "unlinkat" && at == made ? EIO : 0;
        };
        save(q);
        INFO(error);
        CHECK(error.find("failed on purpose") != std::string::npos);
        CHECK(error.find("couldn't be put back") != std::string::npos);
        CHECK(error.find(made.string()) != std::string::npos);
        auto expected = before;
        expected["refs/new"] = "<dir>";
        CHECK(tree(dir) == expected);
    }
    SECTION("a directory it made that can't be looked at before it is removed") {
        failAtScan();
        int looks = 0;
        project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
            return call == "fstatat" && at == made && ++looks == 2 ? EIO : 0;
        };
        save(q);
        INFO(error);
        REQUIRE(looks == 2);
        CHECK(error.find("couldn't be put back") != std::string::npos);
        CHECK(error.find(made.string()) != std::string::npos);
    }
    SECTION("temporaries that can't be removed") {
        // files can't grow past 256 KiB, so refs/big.bin's temporary fails
        // part way, after those of layers/, log.md and notes.md are written
        Project big = p;
        big.files["refs/big.bin"] = std::string(std::size_t{1} << 20, 'b');
        project::testing::systemCall = [](std::string_view call, const fs::path& at) {
            return call == "unlinkat" && at.filename().string().starts_with(".bw-tmp-") ? EIO : 0;
        };
        rlimit old{};
        REQUIRE(::getrlimit(RLIMIT_FSIZE, &old) == 0);
        rlimit low = old;
        low.rlim_cur = 256 << 10;
        const auto handler = std::signal(SIGXFSZ, SIG_IGN);
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &low) == 0);
        save(big);
        ::setrlimit(RLIMIT_FSIZE, &old);
        std::signal(SIGXFSZ, handler);
        INFO(error);
        std::vector<fs::path> left;
        for (const auto& e : fs::recursive_directory_iterator(dir))
            if (e.path().filename().string().starts_with(".bw-tmp-"))
                left.push_back(e.path());
        // refs/big.bin's own, and at least one written before it
        REQUIRE(left.size() >= 2);
        for (const fs::path& at : left) {
            CHECK(error.find(at.string()) != std::string::npos);
            fs::remove(at);
        }
        CHECK(tree(dir) == before);
    }
    CHECK(project::read(dir) == p);
}

TEST_CASE("rollback cleanup failures are named after a non-ProjectError exception", "[project][io][safety]") {
    // the save fails with something other than a ProjectError, and the
    // rollback can't remove refs/new, which the save made
    const fs::path dir = scratch("cleanup-fail-other") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    struct Reset {
        ~Reset() {
            project::testing::directoryStep = nullptr;
            project::testing::systemCall = nullptr;
        }
    } reset;
    const fs::path made = dir / "refs" / "new";
    Project q = p;
    q.files["refs/new/file"] = "added";
    const auto save = [&](const std::function<void()>& thrower) {
        project::testing::directoryStep = [thrower](std::string_view step, const fs::path&) {
            if (step == "scan")
                thrower();
        };
        project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
            return call == "unlinkat" && at == made ? EIO : 0;
        };
        std::string error;
        try {
            project::writeDirectory(q, dir);
        } catch (const ProjectError& e) {
            error = e.what();
        } catch (...) {
            error = "not a ProjectError";
        }
        project::testing::directoryStep = nullptr;
        project::testing::systemCall = nullptr;
        return error;
    };

    std::string error;
    SECTION("std::bad_alloc") {
        error = save([] { throw std::bad_alloc(); });
        CHECK(error.find(std::bad_alloc().what()) != std::string::npos);
    }
    SECTION("an exception that isn't a std::exception") {
        error = save([] { throw 7; });
        CHECK(error.find("an unknown error") != std::string::npos);
    }
    INFO(error);
    CHECK(error.find("can't write " + dir.string()) != std::string::npos);
    CHECK(error.find("couldn't be put back") != std::string::npos);
    CHECK(error.find(made.string() + ": " + std::strerror(EIO)) != std::string::npos);
    auto expected = before;
    expected["refs/new"] = "<dir>";
    CHECK(tree(dir) == expected);
    fs::remove(made);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a non-ProjectError a directory save rolls back cleanly is passed on as it is", "[project][io]") {
    const fs::path dir = scratch("other-clean") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    project::testing::directoryStep = [](std::string_view step, const fs::path&) {
        if (step == "scan")
            throw std::bad_alloc();
    };
    CHECK_THROWS_AS(project::writeDirectory(edited(p), dir), std::bad_alloc);
    project::testing::directoryStep = nullptr;
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("allocation failure while recording an aside restores the original project", "[project][io][safety]") {
    // the first file the project dropped is renamed aside, and the next
    // allocation fails: the rename is already recorded, so it goes back
    const fs::path dir = scratch("alloc-aside") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    Project q = p;
    REQUIRE(q.layers.remove(3));
    struct Reset {
        ~Reset() {
            project::testing::systemCall = nullptr;
            bwtest::failNextAllocation = false;
        }
    } reset;
    bool armed = false;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call == "renameat2" && !armed && at.filename().string().starts_with(".bw-old-"))
            armed = bwtest::failNextAllocation = true;
        return 0;
    };
    CHECK_THROWS_AS(project::writeDirectory(q, dir), std::bad_alloc);
    project::testing::systemCall = nullptr;
    REQUIRE(armed);
    CHECK_FALSE(bwtest::failNextAllocation);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("allocation failure after any change a directory save makes leaves the directory as it was",
          "[project][io][safety]") {
    // A run for each change the save makes (a directory, a temporary, a
    // rename into place or aside), in which the first allocation after it
    // fails: the save either fails and leaves the directory as it found it,
    // or (the failure falling after it is done) succeeds.
    const auto sweep = [](const fs::path& root, const fs::path& dir, const std::optional<Project>& old,
                          const Project& q) {
        struct Reset {
            ~Reset() {
                project::testing::systemCall = nullptr;
                bwtest::failNextAllocation = false;
            }
        } reset;
        const std::set<std::string_view> changes = {"mkdirat", "openat", "renameat2", "linkat"};
        int failed = 0;
        for (int run = 0;; ++run) {
            fs::remove_all(root);
            fs::create_directories(root);
            if (old)
                project::writeDirectory(*old, dir);
            const auto before = tree(root);
            int seen = 0;
            std::string at;
            project::testing::systemCall = [&](std::string_view call, const fs::path& path) {
                if (changes.count(call) && seen++ == run) {
                    at = std::string(call) + " " + path.lexically_relative(root).generic_string();
                    bwtest::failNextAllocation = true;
                }
                return 0;
            };
            bool threw = false;
            try {
                project::writeDirectory(q, dir);
            } catch (const std::exception&) {
                threw = true;
            }
            project::testing::systemCall = nullptr;
            bwtest::failNextAllocation = false;
            if (at.empty()) {  // past the save's last change
                REQUIRE_FALSE(threw);
                CHECK(project::read(dir) == q);
                break;
            }
            INFO("run " << run << ", after " << at);
            if (threw) {
                ++failed;
                CHECK(tree(root) == before);
                if (old)
                    CHECK(project::read(dir) == *old);
            } else {
                CHECK(project::read(dir) == q);
            }
        }
        return failed;
    };

    SECTION("over a project") {
        // files replaced and added, directories made in it, and a layer
        // and a reference set aside
        const fs::path root = scratch("alloc-sweep-over");
        Project p = everything();
        p.files["refs/old/gone"] = "dropped";
        Project q = edited(p);
        REQUIRE(q.layers.remove(3));
        q.files.erase("refs/old/gone");
        q.files["refs/new/sub/file"] = "added";
        CHECK(sweep(root, root / "piece", p, q) > 10);
    }
    SECTION("a first save, its parents made") {
        const fs::path root = scratch("alloc-sweep-first");
        CHECK(sweep(root, root / "a" / "b" / "piece", std::nullopt, edited(everything())) > 10);
    }
}

TEST_CASE("a directory save that runs out of descriptors puts back every file it replaced", "[project][io]") {
    const fs::path dir = scratch("emfile") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    // more directories than the save can hold open at once: it has replaced
    // every file by the time it runs out looking in them
    for (int i = 0; i < 128; ++i)
        fs::create_directory(dir / ("d" + std::to_string(i)));
    const auto before = tree(dir);
    Project q = edited(p);
    REQUIRE(q.layers.remove(3));

    std::string error;
    {
        struct Limit {
            rlimit old{};
            Limit() {
                REQUIRE(::getrlimit(RLIMIT_NOFILE, &old) == 0);
                rlimit low = old;
                low.rlim_cur = 64;
                REQUIRE(::setrlimit(RLIMIT_NOFILE, &low) == 0);
            }
            ~Limit() { ::setrlimit(RLIMIT_NOFILE, &old); }
        } limit;
        try {
            project::writeDirectory(q, dir);
        } catch (const ProjectError& e) {
            error = e.what();
        }
    }
    INFO(error);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("couldn't be put back") == std::string::npos);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a directory read that fails part way leaves no descriptor open", "[project][io]") {
    if (!fs::is_directory("/proc/self/fd"))
        SKIP("no /proc/self/fd to count descriptors in");
    const auto open = [] {
        return std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator());
    };
    const fs::path dir = scratch("readleak") / "piece";
    project::writeDirectory(everything(), dir);
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    project::testing::directoryStep = [](std::string_view step, const fs::path&) {
        if (step == "reading")
            throw ProjectError("failed on purpose");
    };
    const auto before = open();
    CHECK_THROWS_AS(project::read(dir), ProjectError);
    project::testing::directoryStep = nullptr;
    CHECK(open() == before);
}

namespace {

// The descriptors this process holds open.
long openDescriptors() {
    return long(std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator()));
}

// Stops counting allocations; how many were counted.
long stopCounting() {
    const long made = bwtest::allocations;
    bwtest::allocations = bwtest::failAllocation = -1;
    return made;
}

// Stops counting allocations, and clears the hooks, however a test leaves.
struct AllocationReset {
    ~AllocationReset() {
        bwtest::allocations = bwtest::failAllocation = -1;
        bwtest::failNextAllocation = bwtest::failAllocations = false;
        project::testing::directoryStep = nullptr;
        project::testing::systemCall = nullptr;
    }
};

} // namespace

TEST_CASE("a directory save that runs out of memory opening a directory it made closes it",
          "[project][io][safety]") {
    // refs/new is made and looked at; the next allocation, the opened
    // directory's path, fails
    if (!fs::is_directory("/proc/self/fd"))
        SKIP("no /proc/self/fd to count descriptors in");
    const fs::path dir = scratch("alloc-open") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    Project q = p;
    q.files["refs/new/file"] = "added";
    const fs::path made = dir / "refs" / "new";
    AllocationReset reset;
    bool armed = false;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call == "fstatat" && at == made && !armed)
            armed = bwtest::failNextAllocation = true;
        return 0;
    };
    const long fds = openDescriptors();
    CHECK_THROWS_AS(project::writeDirectory(q, dir), std::bad_alloc);
    project::testing::systemCall = nullptr;
    REQUIRE(armed);
    CHECK_FALSE(bwtest::failNextAllocation);
    CHECK(tree(dir) == before);
    CHECK(openDescriptors() == fds);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a directory save that runs out of memory over a file's second name still names it",
          "[project][io][safety]") {
    // the move to a hidden name goes by a hard link, then the old name
    // can't be removed, nor the new one, and the next allocation fails
    const fs::path dir = scratch("twonames-alloc") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    Project q = p;
    q.notes.clear();
    AllocationReset reset;
    std::vector<fs::path> refused;
    bool armed = false;
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (call == "renameat2")
            return ENOSYS;
        const bool second = at.filename().string().starts_with(".bw-old-");
        if (call == "unlinkat" && (at.filename() == "notes.md" || second)) {
            refused.push_back(at);
            if (second && !armed)
                armed = bwtest::failNextAllocation = true;
            return EACCES;
        }
        return 0;
    };
    std::string error = "not a ProjectError";
    try {
        project::writeDirectory(q, dir);
    } catch (const ProjectError& e) {
        error = e.what();
    } catch (...) {
    }
    project::testing::systemCall = nullptr;
    INFO(error);
    REQUIRE(armed);
    CHECK_FALSE(bwtest::failNextAllocation);
    REQUIRE(refused.size() >= 2);
    CHECK(refused[0] == dir / "notes.md");
    CHECK(error.find("couldn't be put back") != std::string::npos);
    CHECK(error.find(refused[1].string()) != std::string::npos);
    CHECK(bwtest::readFile(dir / "notes.md") == p.notes);
    CHECK(fs::equivalent(dir / "notes.md", refused[1]));
    CHECK(project::read(dir) == p);
}

TEST_CASE("a rollback that runs out of memory naming a failure still takes every other step",
          "[project][io][safety]") {
    // the save fails at the scan; the rollback can't remove zz-added, and
    // the first allocation naming that fails
    const fs::path dir = scratch("alloc-name") / "piece";
    const Project p = everything();
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    Project q = edited(p);
    q.files["zz-added"] = "new";
    const fs::path added = dir / "zz-added";
    AllocationReset reset;
    bool rollingBack = false, armed = false;
    project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
        if (step == "scan") {
            rollingBack = true;
            throw 7;
        }
    };
    project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
        if (rollingBack && call == "unlinkat" && at == added) {
            armed = bwtest::failNextAllocation = true;
            return EIO;
        }
        return 0;
    };
    std::string error = "not a ProjectError";
    try {
        project::writeDirectory(q, dir);
    } catch (const ProjectError& e) {
        error = e.what();
    } catch (...) {
    }
    project::testing::directoryStep = nullptr;
    project::testing::systemCall = nullptr;
    INFO(error);
    REQUIRE(armed);
    CHECK_FALSE(bwtest::failNextAllocation);
    CHECK(error.find("couldn't be put back") != std::string::npos);
    CHECK(error.find(added.string() + ": " + std::strerror(EIO)) != std::string::npos);
    // everything but zz-added put back: the layers' old bytes, the added
    // layer's files removed
    auto expected = before;
    expected["zz-added"] = "new";
    CHECK(tree(dir) == expected);
    fs::remove(added);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a rollback that can't allocate at all still takes every step and says some couldn't be named",
          "[project][io][safety]") {
    // the save fails at the scan, from where every allocation fails, and
    // zz-added is a directory with a file in it by then, which the rollback
    // can't remove as the file it added
    if (!fs::is_directory("/proc/self/fd"))
        SKIP("no /proc/self/fd to count descriptors in");
    const fs::path dir = scratch("alloc-never") / "piece";
    Project p = everything();
    p.files["refs/old/gone"] = "dropped";
    project::writeDirectory(p, dir);
    auto expected = tree(dir);
    Project q = edited(p);
    q.files["zz-added"] = "new";
    q.files["refs/new/sub/file"] = "added";
    const fs::path added = dir / "zz-added";
    const long fds = openDescriptors();
    AllocationReset reset;
    project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
        if (step == "scan") {
            fs::remove(added);
            fs::create_directory(added);
            writeBytes(added / "inside", "kept");
            bwtest::failAllocations = true;
            throw 7;
        }
    };
    std::string error = "not a ProjectError";
    try {
        project::writeDirectory(q, dir);
    } catch (const ProjectError& e) {
        bwtest::failAllocations = false;
        error = e.what();
    } catch (...) {
        bwtest::failAllocations = false;
    }
    project::testing::directoryStep = nullptr;
    INFO(error);
    CHECK(openDescriptors() == fds);
    CHECK(error.find("couldn't be put back") != std::string::npos);
    CHECK(error.find("couldn't be named") != std::string::npos);
    // every other step taken: old bytes back, the added folders removed
    expected["zz-added"] = "<dir>";
    expected["zz-added/inside"] = "kept";
    CHECK(tree(dir) == expected);
    fs::remove_all(added);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a rollback with nothing failing allocates nothing", "[project][io][safety]") {
    // so running out of memory can't stop it: from the failure at the scan
    // to the save giving up, no allocation is made
    const fs::path dir = scratch("alloc-none") / "piece";
    Project p = everything();
    p.files["refs/old/gone"] = "dropped";
    project::writeDirectory(p, dir);
    const auto before = tree(dir);
    Project q = edited(p);
    q.files["refs/new/sub/file"] = "added";
    AllocationReset reset;
    project::testing::directoryStep = [&](std::string_view step, const fs::path&) {
        if (step == "scan") {
            bwtest::allocations = 0;
            throw 7;
        }
    };
    CHECK_THROWS_AS(project::writeDirectory(q, dir), int);
    const long made = bwtest::allocations;
    bwtest::allocations = -1;
    project::testing::directoryStep = nullptr;
    CHECK(made == 0);
    CHECK(tree(dir) == before);
    CHECK(project::read(dir) == p);
}

TEST_CASE("a directory save that runs out of memory at any allocation leaves the directory as it was",
          "[project][io][safety]") {
    // A run for each allocation the save makes, in which that one fails:
    // the save fails with std::bad_alloc and leaves the directory as it
    // found it, or (the failure falling after it is done, or caught where
    // the save gives nothing up) succeeds; and either way it leaves no
    // descriptor open.
    if (!fs::is_directory("/proc/self/fd"))
        SKIP("no /proc/self/fd to count descriptors in");
    // `root` is made anew for each run, holding `old` saved at `dir`, or
    // where `there` says an empty directory at `dir`, which a failed save
    // leaves
    const auto sweep = [](const fs::path& root, const fs::path& dir, const std::optional<Project>& old,
                          const Project& q, bool there = false) {
        AllocationReset reset;
        long failed = 0;
        for (long n = 0;; ++n) {
            fs::remove_all(root);
            fs::create_directories(root);
            if (old)
                project::writeDirectory(*old, dir);
            else if (there)
                fs::create_directory(dir);
            REQUIRE(fs::is_directory(dir) == (old || there));
            const auto before = tree(root);
            const long fds = openDescriptors();
            std::string error;
            bool threw = false;
            long made = 0;
            bwtest::failAllocation = n;
            bwtest::allocations = 0;
            try {
                project::writeDirectory(q, dir);
            } catch (const std::bad_alloc&) {
                threw = true;
            } catch (const std::exception& e) {
                made = stopCounting();  // before the test's own allocations
                threw = true;
                error = e.what();
            }
            if (bwtest::allocations >= 0)
                made = stopCounting();
            INFO("allocation " << n << " of " << made << ": " << error);
            CHECK(openDescriptors() == fds);
            if (made <= n) {  // none failed
                REQUIRE_FALSE(threw);
                CHECK(project::read(dir) == q);
                break;
            }
            if (threw) {
                ++failed;
                CHECK(error.empty());
                CHECK(tree(root) == before);
                CHECK(fs::is_directory(dir) == (old || there));
            } else {
                CHECK(project::read(dir) == q);
            }
        }
        return failed;
    };

    SECTION("over a project") {
        const fs::path root = scratch("alloc-every-over");
        Project p = everything();
        p.files["refs/old/gone"] = "dropped";
        Project q = edited(p);
        REQUIRE(q.layers.remove(3));
        q.files.erase("refs/old/gone");
        q.files["refs/new/sub/file"] = "added";
        CHECK(sweep(root, root / "piece", p, q) > 100);
    }
    SECTION("a first save, its parents made") {
        const fs::path root = scratch("alloc-every-first");
        CHECK(sweep(root, root / "a" / "b" / "piece", std::nullopt, edited(everything())) > 100);
    }
    SECTION("a first save into a directory that is there") {
        const fs::path root = scratch("alloc-every-empty");
        CHECK(sweep(root, root / "piece", std::nullopt, edited(everything()), true) > 100);
    }
}

TEST_CASE("a rollback that runs out of memory at any allocation still takes every step it can",
          "[project][io][safety]") {
    // The save fails at the scan and the rollback can't remove zz-added; in
    // a run for each allocation from there on, that one fails. The save
    // fails with a ProjectError; every path left otherwise than the save
    // found it is named in it, or it says some couldn't be; and no
    // descriptor is left open.
    if (!fs::is_directory("/proc/self/fd"))
        SKIP("no /proc/self/fd to count descriptors in");
    const fs::path dir = scratch("alloc-every-rollback") / "piece";
    Project p = everything();
    p.files["refs/old/gone"] = "dropped";
    Project q = edited(p);
    q.files["zz-added"] = "new";
    q.files["refs/new/sub/file"] = "added";
    AllocationReset reset;
    long runs = 0;
    for (long n = 0;; ++n) {
        fs::remove_all(dir);
        project::writeDirectory(p, dir);
        const auto before = tree(dir);
        const long fds = openDescriptors();
        project::testing::directoryStep = [n](std::string_view step, const fs::path&) {
            if (step == "scan") {
                bwtest::failAllocation = n;
                bwtest::allocations = 0;
                throw 7;
            }
        };
        project::testing::systemCall = [&](std::string_view call, const fs::path& at) {
            return bwtest::allocations >= 0 && call == "unlinkat" && at == dir / "zz-added" ? EIO : 0;
        };
        std::string error = "not a ProjectError";
        long made = 0;
        try {
            project::writeDirectory(q, dir);
        } catch (const ProjectError& e) {
            made = stopCounting();  // before the test's own allocations
            error = e.what();
        } catch (...) {
        }
        if (bwtest::allocations >= 0)
            made = stopCounting();
        project::testing::directoryStep = nullptr;
        project::testing::systemCall = nullptr;
        ++runs;
        INFO("allocation " << n << " of " << made << ": " << error);
        CHECK(openDescriptors() == fds);
        REQUIRE(error.find("couldn't be put back") != std::string::npos);
        const bool unnamed = error.find("couldn't be named") != std::string::npos;
        const auto after = tree(dir);
        std::set<std::string> differ;
        for (const auto& [name, bytes] : before)
            if (!after.count(name) || after.at(name) != bytes)
                differ.insert(name);
        for (const auto& [name, bytes] : after)
            if (!before.count(name))
                differ.insert(name);
        CHECK(differ.count("zz-added"));
        for (const std::string& name : differ) {
            INFO(name);
            CHECK((unnamed || error.find((dir / fs::path(name)).string()) != std::string::npos));
        }
        if (made <= n) {  // none failed: only zz-added is left
            CHECK(differ == std::set<std::string>{"zz-added"});
            break;
        }
    }
    CHECK(runs > 10);
}

TEST_CASE("a zip save or read that runs out of memory at any allocation frees what libzip gave it",
          "[project][io][safety]") {
#if !defined(__linux__)
    SKIP("libzip's calls are counted on Linux only");
#else
    if (::geteuid() == 0)
        SKIP("root writes into a read-only directory");
    // A run for each allocation, in which that one fails: whatever the save
    // or read throws, every archive and entry libzip handed it in the run is
    // freed.
    const auto sweep = [](const std::function<void()>& run) {
        AllocationReset reset;
        long runs = 0;
        for (long n = 0;; ++n) {
            const long zips = bwtest::zips.count(), files = bwtest::zipFiles.count();
            bwtest::failAllocation = n;
            bwtest::allocations = 0;
            try {
                run();
            } catch (const std::exception&) {
            }
            const long made = bwtest::allocations;
            bwtest::allocations = bwtest::failAllocation = -1;
            ++runs;
            INFO("allocation " << n << " of " << made);
            CHECK(bwtest::zips.count() == zips);
            CHECK(bwtest::zipFiles.count() == files);
            if (made <= n)
                break;
        }
        return runs;
    };
    const fs::path root = scratch("alloc-zip");
    const Project p = everything();
    SECTION("a save the directory refuses") {
        fs::create_directories(root / "locked");
        project::writeZip(p, root / "locked" / "piece.bwp");
        fs::permissions(root / "locked", fs::perms::owner_read | fs::perms::owner_exec);
        struct Unlock {
            fs::path at;
            ~Unlock() { fs::permissions(at, fs::perms::owner_all); }
        } unlock{root / "locked"};
        CHECK_THROWS_AS(project::writeZip(p, root / "locked" / "piece.bwp"), ProjectError);
        CHECK(sweep([&] { project::writeZip(p, root / "locked" / "piece.bwp"); }) > 10);
    }
    SECTION("a save where libzip can't open the path") {
        writeBytes(root / "file", "not a directory");
        CHECK_THROWS_AS(project::writeZip(p, root / "file" / "piece.bwp"), ProjectError);
        CHECK(sweep([&] { project::writeZip(p, root / "file" / "piece.bwp"); }) > 10);
    }
    SECTION("a read of a file that isn't a zip") {
        writeBytes(root / "junk.bwp", "not a zip at all");
        CHECK_THROWS_AS(project::read(root / "junk.bwp"), ProjectError);
        CHECK(sweep([&] { (void)project::read(root / "junk.bwp"); }) > 2);
    }
    SECTION("a save and a read that succeed") {
        CHECK(sweep([&] {
                  project::writeZip(p, root / "piece.bwp");
                  (void)project::read(root / "piece.bwp");
              }) > 100);
    }
#endif
}

TEST_CASE("a hidden entry in a project directory is skipped whatever it is", "[project][io][safety]") {
    const fs::path root = scratch("hidden-special");
    const fs::path dir = root / "piece";
    project::writeDirectory(everything(), dir);
    // a FIFO, links, a link loop, and folders no one can read, one of them
    // through a link: none is opened, followed or listed
    REQUIRE(::mkfifo((dir / ".pipe").c_str(), 0600) == 0);
    REQUIRE(::mkfifo((dir / "refs" / ".pipe").c_str(), 0600) == 0);
    fs::create_symlink("notes.md", dir / ".link");
    fs::create_symlink(".loop", dir / ".loop");
    fs::create_directory(root / "locked");
    fs::create_directory_symlink("../../locked", dir / "refs" / ".locked");
    fs::create_directory(dir / ".locked");
    REQUIRE(::chmod((root / "locked").c_str(), 0) == 0);
    REQUIRE(::chmod((dir / ".locked").c_str(), 0) == 0);
    CHECK(project::read(dir) == everything());
    ::chmod((root / "locked").c_str(), 0755);
    ::chmod((dir / ".locked").c_str(), 0755);

    // one that isn't hidden is refused
    for (const fs::path& at : {dir / "pipe", dir / "refs" / "pipe"}) {
        INFO(at);
        REQUIRE(::mkfifo(at.c_str(), 0600) == 0);
        CHECK_THROWS_AS(project::read(dir), ProjectError);
        fs::remove(at);
    }
    CHECK(project::read(dir) == everything());
}

#ifdef PATH_MAX
TEST_CASE("a directory save refuses a path the system can't open before writing", "[project][io][paths]") {
    // a project deep enough that a long ref under it is past PATH_MAX
    fs::path deep = scratch("deepdir");
    while (deep.native().size() < PATH_MAX - 700)
        deep /= std::string(200, 'd');
    const Project p = everything();
    project::writeDirectory(p, deep / "p");
    Project q = p;
    q.notes = "saved again";
    const std::string part(250, 'r');
    q.files["refs/" + part + "/" + part + "/" + part] = "x";
    CHECK_THROWS_AS(project::writeDirectory(q, deep / "p"), ProjectError);
    CHECK(project::read(deep / "p") == p);
    CHECK_FALSE(fs::exists(deep / "p" / "refs" / part));
}
#endif
#endif

TEST_CASE("a directory read refuses a file that changed size after the walk saw it", "[project][io][safety]") {
    const fs::path dir = scratch("grown") / "piece";
    Project p = everything();
    p.files["refs/empty"] = "";
    p.files["refs/short"] = "abc";
    project::writeDirectory(p, dir);
    struct Reset {
        ~Reset() { project::testing::directoryStep = nullptr; }
    } reset;
    // grown from empty, grown, shrunk, emptied
    const std::vector<std::pair<std::string, std::string>> changes = {
        {"refs/empty", "GROWN"}, {"refs/short", "abcd"}, {"refs/short", "ab"}, {"refs/short", ""}};
    // refused before a byte of it is read
    bool reading = false;
    for (const auto& [victim, now] : changes) {
        INFO(victim << " -> \"" << now << "\"");
        reading = false;
        project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
            if (step == "read" && at == dir / victim)
                writeBytes(at, now);
            if (step == "reading" && at == dir / victim)
                reading = true;
        };
        std::string error;
        try {
            project::read(dir);
        } catch (const ProjectError& e) {
            error = e.what();
        }
        project::testing::directoryStep = nullptr;
        CHECK(error.find("changed while it was read") != std::string::npos);
        CHECK_FALSE(reading);
        writeBytes(dir / victim, p.files.at(victim));
    }
    // and one that didn't change is read
    project::testing::directoryStep = [&](std::string_view step, const fs::path& at) {
        if (step == "reading" && at == dir / "refs/short")
            reading = true;
    };
    CHECK(project::read(dir) == p);
    // "reading" fires only where the read checks the opened file's size
    // before reading it
#if defined(__unix__) || defined(__APPLE__)
    CHECK(reading);
#endif
}

#if defined(__linux__)
TEST_CASE("a directory read refuses a link swapped in after the walk saw a file", "[project][io][safety]") {
    const fs::path root = scratch("swap");
    const fs::path dir = root / "piece";
    Project p = everything();
    p.files["refs/swap/a"] = "hello!";
    p.files["refs/swap/b"] = "hello!";
    project::writeDirectory(p, dir);
    writeBytes(root / "outside.txt", "SECRET");
    // the walk reads refs/swap/ in this order: the victim is read after a big
    // file, and is swapped for a link while the big one is read
    std::vector<fs::path> order;
    for (const auto& e : fs::directory_iterator(dir / "refs" / "swap"))
        order.push_back(e.path());
    REQUIRE(order.size() == 2);
    const fs::path big = order[0], victim = order[1];
    writeBytes(big, std::string(std::size_t{32} << 20, 'b'));

    const int in = ::inotify_init1(IN_CLOEXEC);
    REQUIRE(in >= 0);
    REQUIRE(::inotify_add_watch(in, big.c_str(), IN_OPEN) >= 0);
    std::thread swapper([&] {
        pollfd pfd{in, POLLIN, 0};
        if (::poll(&pfd, 1, 10'000) != 1)
            return;
        fs::remove(victim);
        fs::create_symlink(root / "outside.txt", victim);
    });
    std::string read;
    try {
        read = project::read(dir).files.at(fs::path(victim).lexically_relative(dir).generic_string());
    } catch (const ProjectError&) {
        read = "refused";
    }
    swapper.join();
    ::close(in);
    REQUIRE(fs::is_symlink(fs::symlink_status(victim)));
    CHECK(read == "refused");
}
#endif
