// .cells format version 2 and project format 4.0: wide pairs and clusters in
// a layer's cells (PLAN §16, Project format), the limits and every refusal,
// and version 1 written as before for a layer without them.

#include "blockwright/project.h"
#include "blockwright/glyph.h"
#include "blockwright/text_style.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <zip.h>
#include <zstd.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace bw;
using namespace bwtest;
using bw::project::Entries;
using bw::project::Project;
using bw::project::ProjectError;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

const Glyph kCont = Glyph::continuation();

Cell cell(Glyph ch, std::uint8_t fg = 7, std::uint8_t bg = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

Glyph cluster(std::vector<char32_t> cps) {
    const std::optional<Glyph> g = clusterPool().intern(cps, PoolCap::Capped);
    REQUIRE(g);
    REQUIRE(g->isCluster());
    return *g;
}

std::string u32(std::uint32_t v) {
    std::string out;
    for (int i = 0; i < 4; ++i)
        out += char((v >> (8 * i)) & 0xff);
    return out;
}

// The 14 bytes of one cell: the code point field, a palette foreground and
// background, attributes and flags.
std::string record(std::uint32_t field, int flags = 0, int fg = 7, int bg = 0, int attrs = 0) {
    std::string r = u32(field);
    r += char(0);
    r += char(fg);
    r += std::string(2, '\0');
    r += char(0);
    r += char(bg);
    r += std::string(2, '\0');
    r += char(attrs);
    r += char(flags);
    return r;
}
std::string lead(char32_t cp = U'中') { return record(std::uint32_t(cp)); }
std::string cont(int flags = 0, int fg = 7) { return record(0, 0x02 | flags, fg); }
std::string clusterCell(std::uint32_t index, int flags = 0) { return record(index, 0x04 | flags); }
std::string blank() { return record(U' '); }

// A cluster table: its count and each entry.
std::string table(const std::vector<std::vector<std::uint32_t>>& entries) {
    std::string out = u32(std::uint32_t(entries.size()));
    for (const auto& e : entries) {
        out += char(e.size());
        for (std::uint32_t cp : e)
            out += u32(cp);
    }
    return out;
}
const std::vector<std::uint32_t> kAcute{U'e', 0x301};
// A table of `bytes` bytes that holds no cluster: a count of 0, then zeros.
std::string zeroTable(std::size_t bytes) { return u32(0) + std::string(bytes - 4, '\0'); }

// A .cells file of `version` around `content`, compressed as it is.
std::string rawCells(std::uint32_t w, std::uint32_t h, const std::string& content, int version = 2) {
    std::string out = "BWCL";
    out += char(version);
    out += std::string(3, '\0');
    out += u32(w) + u32(h);
    std::string frame(ZSTD_compressBound(content.size()), '\0');
    frame.resize(ZSTD_compress(frame.data(), frame.size(), content.data(), content.size(), 3));
    return out + frame;
}

void refused(const std::string& data, const std::string& why) {
    INFO(why);
    CHECK_THROWS_AS(project::decodeCells(data), ProjectError);
    CHECK_THROWS_WITH(project::decodeCells(data), ContainsSubstring(why));
}

std::string version(const std::string& cells) { return cells.substr(4, 4); }

Project oneLayer(const Canvas& cells) {
    Project p;
    p.layers = LayerStack(cells.width(), cells.height());
    Layer l = makeLayer(LayerKind::Cells, cells.width(), cells.height(), "art");
    l.cells = cells;
    p.layers.add(std::move(l));
    return p;
}

std::string manifestVersion(const Entries& e) {
    const std::string& m = e.at("manifest.json");
    return QJsonDocument::fromJson(QByteArray(m.data(), qsizetype(m.size())))
        .object()
        .value("version")
        .toString()
        .toStdString();
}

void setManifestVersion(Entries& e, const char* v) {
    const std::string& m = e.at("manifest.json");
    QJsonObject o = QJsonDocument::fromJson(QByteArray(m.data(), qsizetype(m.size()))).object();
    o["version"] = v;
    e["manifest.json"] = QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
}

// A canvas with a pair of each kind: palette, RGB, transparent, a cluster
// lead; clusters, one used twice; and a lone wide cell.
Canvas widePiece() {
    Canvas c(8, 3);
    c.at(0, 0) = cell(U'中', 14, 1);
    c.at(1, 0) = cell(kCont, 14, 1);
    c.at(2, 0) = cell(cluster({U'e', 0x301}));
    c.at(3, 0) = cell(cluster({U'♥', 0xFE0F}), 12);
    c.at(4, 0) = cell(cluster({U'e', 0x301}), 3);
    c.at(0, 1) = cell(U'中');
    c.at(0, 1).fg = c.at(0, 1).bg = Color::fromRgb({250, 128, 0});
    c.at(1, 1) = c.at(0, 1);
    c.at(1, 1).ch = kCont;
    c.at(3, 1) = cell(U'国');
    c.at(3, 1).transparent = true;
    c.at(4, 1) = c.at(3, 1);
    c.at(4, 1).ch = kCont;
    c.at(6, 1) = cell(cluster({U'👍', 0x1F3FD}), 11, 2);
    c.at(6, 1).attrs = attr::Blink;
    c.at(7, 1) = c.at(6, 1);
    c.at(7, 1).ch = kCont;
    c.at(2, 2) = cell(U'中');  // a lone wide cell is a code point
    return c;
}

} // namespace

// ---- the codec --------------------------------------------------------------

TEST_CASE("a layer with pairs and clusters round-trips as .cells version 2", "[wide][cells]") {
    const Canvas c = widePiece();
    const std::string data = project::encodeCells(c);
    CHECK(version(data) == std::string("\x02\0\0\0", 4));
    const Canvas back = project::decodeCells(data);
    CHECK(back == c);
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            CHECK(identical(back.at(x, y), c.at(x, y)));

    // the table holds each cluster once, in the order the cells first use them
    const std::string frame = data.substr(16);
    std::string raw(ZSTD_getFrameContentSize(frame.data(), frame.size()), '\0');
    raw.resize(ZSTD_decompress(raw.data(), raw.size(), frame.data(), frame.size()));
    const std::size_t cells = 8 * 3 * 14;
    CHECK(raw.substr(cells) ==
          table({kAcute, {U'♥', 0xFE0F}, {U'👍', 0x1F3FD}}));
    CHECK(raw.substr(14, 14) == record(0, 0x02, 14, 1));  // the continuation of 中
    CHECK(raw.substr(4 * 14, 4) == u32(0));  // the second e + U+0301: entry 0 again
}

TEST_CASE("a layer of code points only is .cells version 1, byte for byte", "[wide][cells]") {
    Canvas c(3, 1);
    c.at(0, 0) = cell(U'中');  // a lone wide cell holds no continuation
    c.at(1, 0) = cell(U'A');
    const std::string data = project::encodeCells(c);
    CHECK(version(data) == std::string("\x01\0\0\0", 4));
    CHECK(data == rawCells(3, 1, lead() + record(U'A') + blank(), 1));
}

TEST_CASE("a .cells version 2 reads its crafted cells", "[wide][cells]") {
    SECTION("a pair, its lead any glyph") {
        const Canvas c = project::decodeCells(rawCells(3, 1, record(U'A') + cont() + blank() + table({})));
        CHECK(c.at(0, 0).ch == Glyph(U'A'));
        CHECK(c.at(1, 0).ch.isContinuation());
    }
    SECTION("a cluster lead") {
        const Canvas c = project::decodeCells(rawCells(2, 1, clusterCell(0) + cont() + table({kAcute})));
        CHECK(codePoints(c.at(0, 0).ch).size == 2);
        CHECK(c.at(1, 0).ch.isContinuation());
    }
    SECTION("a cluster of 16 code points and one holding U+0000") {
        std::vector<std::uint32_t> long16{U'a'};
        long16.resize(16, 0x301);
        const Canvas c =
            project::decodeCells(rawCells(2, 1, clusterCell(0) + clusterCell(1) + table({long16, {0, 0x301}})));
        CHECK(codePoints(c.at(0, 0).ch).size == 16);
        CHECK(codePoints(c.at(1, 0).ch)[0] == U'\0');
    }
    SECTION("a transparent pair") {
        const Canvas c = project::decodeCells(rawCells(2, 1, record(U'中', 1) + cont(1) + table({})));
        CHECK(c.at(0, 0).transparent);
        CHECK(c.at(1, 0).transparent);
    }
}

TEST_CASE("a .cells version 2 refuses every malformed cell and table", "[wide][cells][safety]") {
    const std::string none = table({});
    refused(rawCells(2, 1, blank() + blank() + none), "no wide glyph or cluster");
    refused(rawCells(2, 1, lead() + cont()), "no cluster table");
    refused(rawCells(2, 1, lead() + cont() + none + "x"), "bytes after the cluster table");
    refused(rawCells(2, 1, lead() + cont() + u32(1)), "the cluster table is short");
    refused(rawCells(2, 1, clusterCell(0) + blank() + u32(1) + char(2) + u32(U'e')), "the cluster table is short");

    // the table's entries
    refused(rawCells(1, 1, clusterCell(0) + table({{U'e'}})), "has 1 code points, not 2 to 16");
    std::vector<std::uint32_t> long17{U'a'};
    long17.resize(17, 0x301);
    refused(rawCells(1, 1, clusterCell(0) + table({long17})), "has 17 code points");
    refused(rawCells(1, 1, clusterCell(0) + table({{U'e', 0xD800}})), "a code point a cluster can't");
    refused(rawCells(1, 1, clusterCell(0) + table({{U'e', 0x110000}})), "a code point a cluster can't");
    refused(rawCells(1, 1, clusterCell(0) + table({{U'e', 0x1B}})), "a code point a cluster can't");
    refused(rawCells(1, 1, clusterCell(0) + table({{0x85, 0x301}})), "a code point a cluster can't");
    refused(rawCells(2, 1, clusterCell(0) + clusterCell(1) + table({kAcute, kAcute})), "repeats an earlier one");
    refused(rawCells(2, 1, clusterCell(0) + blank() + table({kAcute, {U'a', 0x301}})), "cluster 1 is never used");

    // the cells' uses of it
    refused(rawCells(1, 1, clusterCell(1) + table({kAcute})), "uses cluster 1 of 1");
    refused(rawCells(2, 1, clusterCell(1) + clusterCell(0) + table({kAcute, {U'a', 0x301}})),
            "uses cluster 1 before cluster 0");

    // flags
    refused(rawCells(2, 1, lead() + record(0, 0x06) + none), "both a continuation and a cluster");
    refused(rawCells(2, 1, lead() + cont(0x08) + none), "an unknown flag");
    refused(rawCells(2, 1, lead() + record(0, 0x80) + none), "an unknown flag");
    refused(rawCells(2, 1, lead() + cont(), 1), "an unknown flag");  // a continuation in version 1
    refused(rawCells(1, 1, clusterCell(0), 1), "an unknown flag");

    // continuations
    refused(rawCells(2, 1, lead() + record(U'x', 0x02) + none), "a continuation, has a code point");
    refused(rawCells(2, 1, cont() + blank() + none), "has no lead to its left");
    refused(rawCells(2, 2, lead() + blank() + cont() + blank() + none), "cell 2, a continuation, has no lead");
    refused(rawCells(3, 1, lead() + cont() + cont() + none), "cell 2, a continuation, has no lead");
    refused(rawCells(2, 1, lead() + cont(0, 12) + none), "differs from its lead");
    refused(rawCells(2, 1, lead() + record(0, 0x02, 7, 1) + none), "differs from its lead");
    refused(rawCells(2, 1, lead() + record(0, 0x02, 7, 0, 1) + none), "differs from its lead");
    refused(rawCells(2, 1, lead() + cont(0x01) + none), "differs from its lead");
    refused(rawCells(2, 1, record(U'中', 0x01) + cont() + none), "differs from its lead");

    // a code point field in a version 2 cell is checked as in version 1
    refused(rawCells(2, 1, record(0x110000) + cont() + none), "not a Unicode character");
}

TEST_CASE("a .cells cluster table is held to its code-point limit before it is decoded", "[wide][cells][limits]") {
    // ⌊B / 4⌋ for a table of B bytes: over the limit is refused by what the
    // frame declares; at it, the table is read (and here refused for what
    // it holds)
    const std::string cells = lead() + cont();
    const std::size_t limit = project::kMaxTableCodePoints;
    refused(rawCells(2, 1, cells + zeroTable(limit * 4 + 4)), "could hold over 4000000 code points");
    const std::string at = rawCells(2, 1, cells + zeroTable(limit * 4));
    CHECK_THROWS_WITH(project::decodeCells(at), ContainsSubstring("bytes after the cluster table"));
}

TEST_CASE("the writer refuses a pair a reader would refuse", "[wide][cells]") {
    Canvas c(3, 1);
    c.at(0, 0).ch = kCont;
    CHECK_THROWS_WITH(project::encodeCells(c), ContainsSubstring("cell 0,0"));
    c.at(0, 0) = cell(U'中');
    c.at(1, 0) = cell(kCont);
    c.at(2, 0) = cell(kCont);
    CHECK_THROWS_WITH(project::encodeCells(c), ContainsSubstring("cell 2,0"));
    c.at(2, 0) = cell(U' ');
    CHECK_NOTHROW(project::encodeCells(c));
    for (int field = 0; field < 4; ++field) {
        INFO(field);
        Canvas d = c;
        Cell& k = d.at(1, 0);
        if (field == 0)
            k.fg = Color::indexed(12);
        if (field == 1)
            k.attrs = attr::Blink;
        if (field == 2)
            k.transparent = true;
        if (field == 3)
            k.dithered = true;  // the mark the layer's JSON keeps
        CHECK_THROWS_WITH(project::encodeCells(d), ContainsSubstring("differs from its left half"));
    }
}

// ---- projects ---------------------------------------------------------------

TEST_CASE("a project with a pair or a cluster is format 4.0, and one without keeps its version",
          "[wide][project][version]") {
    Canvas plain(4, 2);
    plain.at(0, 0) = cell(U'中');
    const Entries before = project::encode(oneLayer(plain));
    CHECK(manifestVersion(before) == std::to_string(project::kFormatMajor) + "." + std::to_string(project::kFormatMinor));

    Canvas pair = plain;
    pair.at(1, 0) = cell(kCont);
    const Entries wide = project::encode(oneLayer(pair));
    CHECK(manifestVersion(wide) == "4.0");
    CHECK(project::decode(wide).layers == oneLayer(pair).layers);

    Canvas clustered = plain;
    clustered.at(3, 1) = cell(cluster({U'e', 0x301}));
    CHECK(manifestVersion(project::encode(oneLayer(clustered))) == "4.0");

    // 4.0 implies every 3.x and 2.x minor: a Glow on a cells layer (3.0)
    Project glow = oneLayer(pair);
    TextStyle style;
    style.effects.push_back(Effect{true, GlowEffect{}});
    glow.layers[0].props.style = style;
    const Entries glowing = project::encode(glow);
    CHECK(manifestVersion(glowing) == "4.0");
    CHECK(project::decode(glowing).layers == glow.layers);
}

TEST_CASE("a version 2 layer in a project before 4.0 is refused", "[wide][project][version]") {
    Canvas c(4, 1);
    c.at(0, 0) = cell(cluster({U'a', 0x302}));
    c.at(1, 0) = cell(kCont);
    Entries e = project::encode(oneLayer(c));
    for (const char* v : {"3.0", "2.4", "1.8"}) {
        INFO(v);
        setManifestVersion(e, v);
        CHECK_THROWS_WITH(project::decode(e), ContainsSubstring("wide glyphs or clusters in a"));
    }
}

TEST_CASE("a reader refuses the dither marks that split a pair", "[wide][project][dither]") {
    Canvas c(4, 1);
    c.at(1, 0) = cell(U'中');
    c.at(2, 0) = cell(kCont);
    c.at(1, 0).dithered = c.at(2, 0).dithered = true;
    Entries e = project::encode(oneLayer(c));
    const Project p = project::decode(e);
    CHECK(p.layers[0].cells.at(2, 0).dithered);

    const std::string json = "layers/" + std::to_string(p.layers[0].props.id) + ".json";
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(e.at(json))).object();
    REQUIRE(o.contains("dither"));
    o["dither"] = QString::fromLatin1(QByteArray(1, char(0x02)).toBase64());  // the lead only
    e[json] = QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
    CHECK_THROWS_WITH(project::decode(e), ContainsSubstring("the dither marks split the wide glyph at 1,0"));
    o["dither"] = QString::fromLatin1(QByteArray(1, char(0x04)).toBase64());  // the continuation only
    e[json] = QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
    CHECK_THROWS_WITH(project::decode(e), ContainsSubstring("the dither marks split the wide glyph at 1,0"));
}

namespace {

// A cluster no other test in the process interns: a base from plane 3,
// counted up.
std::vector<std::uint32_t> freshCluster() {
    static std::uint32_t next = 0x30000;
    return {next++, 0x301};
}

// A layer of `w` x 1 with a pair at its left, so the project is 4.0.
Layer pairLayer(int w, const char* name) {
    Layer l = makeLayer(LayerKind::Cells, w, 1, name);
    l.cells.at(0, 0) = cell(U'中');
    l.cells.at(1, 0) = cell(kCont);
    return l;
}

std::string cellsPath(const Project& p, std::size_t i) {
    return "layers/" + std::to_string(p.layers[i].props.id) + ".cells";
}

// `decode` refuses `e` for `why`, and the pool holds no more than before.
void refusedInterningNothing(const Entries& e, const std::string& why) {
    INFO(why);
    const std::size_t before = clusterPool().size();
    CHECK_THROWS_WITH(project::decode(e), ContainsSubstring(why));
    CHECK(clusterPool().size() == before);
}

} // namespace

TEST_CASE("a refused project interns none of its clusters", "[wide][project][pool]") {
    SECTION("a split dither mark on its only layer") {
        Project p;
        p.layers = LayerStack(2, 1);
        p.layers.add(pairLayer(2, "art"));
        p.layers[0].cells.at(0, 0).dithered = p.layers[0].cells.at(1, 0).dithered = true;
        Entries e = project::encode(p);
        e[cellsPath(p, 0)] = rawCells(2, 1, clusterCell(0) + cont() + table({freshCluster()}));
        const std::string json = "layers/" + std::to_string(p.layers[0].props.id) + ".json";
        QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(e.at(json))).object();
        o["dither"] = QString::fromLatin1(QByteArray(1, char(0x01)).toBase64());  // the lead only
        e[json] = QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
        refusedInterningNothing(e, "the dither marks split the wide glyph at 0,0");
    }
    SECTION("a malformed second layer, the first holding 100 clusters") {
        Project p;
        p.layers = LayerStack(100, 1);
        p.layers.add(pairLayer(100, "first"));
        p.layers.add(pairLayer(100, "second"));
        Entries e = project::encode(p);
        std::string cells;
        std::vector<std::vector<std::uint32_t>> entries;
        for (std::uint32_t i = 0; i < 100; ++i) {
            cells += clusterCell(i);
            entries.push_back(freshCluster());
        }
        e[cellsPath(p, 0)] = rawCells(100, 1, cells + table(entries));
        std::string orphan = cont();
        for (int i = 1; i < 100; ++i)
            orphan += blank();
        e[cellsPath(p, 1)] = rawCells(100, 1, orphan + table({}));
        refusedInterningNothing(e, "has no lead to its left");
    }
    SECTION("a project-wide check after every layer") {
        Project p;
        p.layers = LayerStack(2, 1);
        p.layers.add(pairLayer(2, "art"));
        Entries e = project::encode(p);
        e[cellsPath(p, 0)] = rawCells(2, 1, clusterCell(0) + cont() + table({freshCluster()}));
        const Entries good = e;
        const std::string& m = e.at("manifest.json");
        QJsonObject o = QJsonDocument::fromJson(QByteArray(m.data(), qsizetype(m.size()))).object();
        o["nextLayerId"] = double(p.layers[0].props.id);
        e["manifest.json"] = QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
        refusedInterningNothing(e, "is not past every layer id");

        // the same project, accepted, interns its cluster
        const std::size_t before = clusterPool().size();
        const Project back = project::decode(good);
        CHECK(clusterPool().size() == before + 1);
        CHECK(codePoints(back.layers[0].cells.at(0, 0).ch).size == 2);
    }
}

TEST_CASE("a project's cluster tables count against one limit, before any layer is decoded",
          "[wide][project][limits]") {
    // two layers each under the limit, over it together
    Canvas c(2, 1);
    c.at(0, 0) = cell(U'中');
    c.at(1, 0) = cell(kCont);
    Project p = oneLayer(c);
    Layer second = makeLayer(LayerKind::Cells, 2, 1, "second");
    second.cells = c;
    p.layers.add(std::move(second));
    Entries e = project::encode(p);
    CHECK(manifestVersion(e) == "4.0");
    const std::size_t half = project::kMaxTableCodePoints / 2;
    for (const Layer& l : p.layers.layers())
        e["layers/" + std::to_string(l.props.id) + ".cells"] = rawCells(2, 1, lead() + cont() + zeroTable(half * 4 + 4));
    CHECK_THROWS_WITH(project::decode(e), ContainsSubstring("the project's layers hold clusters of over 4000000"));
}

TEST_CASE("a project without pairs or clusters writes the entries it was read from", "[wide][project][golden]") {
    // the samples and a test project, saved by builds before version 2:
    // every entry the writer writes is the file as it was (the samples' cells
    // in raw frames, as bw-make-samples writes them)
    for (const char* name : {"01-layers-showcase", "02-text-style-logo"}) {
        INFO(name);
        const fs::path dir = fs::path(BW_SAMPLES_SOURCE) / name;
        const Entries e = project::encode(project::read(dir), project::CellsFrame::Raw);
        std::size_t cells = 0;
        for (const auto& [path, bytes] : e) {
            INFO(path);
            CHECK(bytes == readFile(dir / path));
            cells += path.ends_with(".cells");
        }
        CHECK(cells > 0);
    }

    // a project an older build wrote (its manifest at 1.0, which this
    // build writes as 1.3): its cells as they were
    const fs::path bwp = dataDir() / "projects" / "unwritable.bwp";
    const Entries e = project::encode(project::read(bwp));
    int err = 0;
    zip_t* z = zip_open(bwp.string().c_str(), ZIP_RDONLY, &err);
    REQUIRE(z);
    std::size_t cells = 0;
    for (const auto& [path, bytes] : e) {
        if (!path.ends_with(".cells"))
            continue;
        ++cells;
        INFO(path);
        zip_stat_t st;
        REQUIRE(zip_stat(z, path.c_str(), 0, &st) == 0);
        std::string on(st.size, '\0');
        zip_file_t* f = zip_fopen(z, path.c_str(), 0);
        REQUIRE(f);
        CHECK(zip_fread(f, on.data(), st.size) == zip_int64_t(st.size));
        zip_fclose(f);
        CHECK(bytes == on);
    }
    zip_close(z);
    CHECK(cells > 0);
}

TEST_CASE("a project's clusters open whatever the session's pool holds", "[.pool-full]") {
    // run alone (tests/CMakeLists.txt): fill the pool to its lossy cap, as
    // a session that read many clusters would, then open a project with new
    // ones; a project load is uncapped (PoolCap::Uncapped)
    std::vector<char32_t> cps{0x10000, 0x301};
    std::size_t n = 0;
    while (clusterPool().intern(cps, PoolCap::Capped)) {
        ++cps[0];
        REQUIRE(++n <= ClusterPool::kLossyCap);
    }
    CHECK_FALSE(clusterPool().intern(std::vector<char32_t>{U'q', 0x302}, PoolCap::Capped));
    Canvas c(2, 1);
    c.at(0, 0) = cell(*clusterPool().intern(std::vector<char32_t>{U'q', 0x302}, PoolCap::Uncapped));
    c.at(1, 0) = cell(kCont);
    const Entries e = project::encode(oneLayer(c));
    const std::string data =
        rawCells(3, 1, clusterCell(0) + cont() + clusterCell(1) + table({{U'z', 0x303}, {U'y', 0x304}}));
    const Canvas back = project::decodeCells(data);
    CHECK(codePoints(back.at(0, 0).ch)[0] == U'z');
    CHECK(codePoints(back.at(2, 0).ch)[0] == U'y');
    CHECK(project::decode(e).layers == oneLayer(c).layers);
}
