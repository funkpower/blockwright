// The Library's stamps (PLAN §12): the .ans + .json format, reading and
// checking it, scanning a library, categories, search and where the
// libraries are.

#include "fs_quirks.h"
#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/library.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <csignal>

#include <QByteArray>
#include <QtGlobal>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#endif

using namespace bw;
using namespace bw::library;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "library" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

Cell cell(char32_t ch, int fg, int bg = Black) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(std::uint8_t(fg));
    c.bg = Color::indexed(std::uint8_t(bg));
    return c;
}

// A 4x2 tree-ish block: blank cells round a trunk, black drawn as █.
CellBlock tree() {
    CellBlock b(4, 2);
    b.at(1, 0) = cell(U'▄', Green);
    b.at(2, 0) = cell(U'▄', LightGreen);
    b.at(1, 1) = cell(U'█', Black);  // black inside the part
    b.at(2, 1) = cell(U'▌', Brown);
    return b;
}

std::string sidecar(const std::string& body) { return "{" + body + "}"; }

struct EnvGuard {
    const char* name;
    explicit EnvGuard(const char* n, const std::string& value) : name(n) {
        qputenv(n, QByteArray::fromStdString(value));
    }
    ~EnvGuard() { qunsetenv(name); }
};

} // namespace

TEST_CASE("a stamp written from cells reads back with its name, sidecar and cells") {
    const fs::path dir = scratch("roundtrip");
    const CellBlock b = tree();
    const StampFiles f = encodeStamp("Pine", "trees", {"tree", "night"}, b, true);
    writeStamp(dir / "pine.ans", f);
    REQUIRE(fs::exists(dir / "pine.json"));

    const Stamp s = readStamp(dir / "pine.ans");
    CHECK(s.name == "Pine");
    CHECK(s.displayName() == "Pine");
    CHECK(s.meta.category == "trees");
    CHECK(s.meta.tags == std::vector<std::string>{"tree", "night"});
    REQUIRE(s.art.sauce);
    CHECK(s.art.sauce->title == "Pine");
    CHECK(s.art.canvas.width() == 4);
    CHECK(s.art.canvas.height() == 2);
    const CellBlock back = s.block();
    REQUIRE(back.width == 4);
    REQUIRE(back.height == 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            INFO(x << "," << y);
            CHECK(back.at(x, y).ch == b.at(x, y).ch);
            CHECK(back.at(x, y).fg.paletteIndex() == b.at(x, y).fg.paletteIndex());
            CHECK(back.at(x, y).bg.paletteIndex() == b.at(x, y).bg.paletteIndex());
            CHECK(isBlank(back.at(x, y)) == isBlank(b.at(x, y)));
        }
    // The black █ is not blank: it is drawn.
    CHECK_FALSE(isBlank(back.at(1, 1)));
    CHECK(isBlank(back.at(0, 0)));
    CHECK(isBlank(back.at(3, 1)));

    // The sidecar's bytes are the encoder's widest row.
    std::size_t widest = 0;
    for (const std::string& line : irc::encodeCanvas(s.art.canvas))
        widest = std::max(widest, line.size());
    CHECK(widest > 0);
    CHECK(s.meta.ircBytes == widest);
    CHECK(widestRowBytes(s.art.canvas) == widest);
}

TEST_CASE("the sidecar is three keys in a fixed order") {
    const StampMeta m{"sky", {"star", "a \"quoted\" tag"}, 42};
    CHECK(sidecarJson(m) == "{\n  \"category\": \"sky\",\n  \"tags\": [\"star\", \"a \\\"quoted\\\" tag\"],\n"
                            "  \"ircBytes\": 42\n}\n");
    CHECK(parseSidecar(sidecarJson(m)) == m);
    CHECK(parseSidecar(sidecarJson({"sky", {}, 0})).tags.empty());
    const StampMeta u = parseSidecar(sidecar(R"("ircBytes": 7, "tags": ["ü"], "category": "Bäume")"));
    CHECK(u.category == "Bäume");
    CHECK(u.tags == std::vector<std::string>{"ü"});
    CHECK(u.ircBytes == 7);
}

TEST_CASE("a malformed sidecar is refused") {
    const char* bad[] = {
        "",
        "not json",
        "[1, 2]",
        R"({"category": "sky", "tags": [], "ircBytes": 1, "name": "x"})",  // unknown key
        R"({"tags": [], "ircBytes": 1})",                                   // no category
        R"({"category": "", "tags": [], "ircBytes": 1})",
        R"({"category": "  ", "tags": [], "ircBytes": 1})",
        R"({"category": 3, "tags": [], "ircBytes": 1})",
        R"({"category": "sky", "ircBytes": 1})",                            // no tags
        R"({"category": "sky", "tags": "star", "ircBytes": 1})",
        R"({"category": "sky", "tags": [1], "ircBytes": 1})",
        R"({"category": "sky", "tags": [""], "ircBytes": 1})",
        R"({"category": "sky", "tags": ["a\tb"], "ircBytes": 1})",
        R"({"category": "sky", "tags": ["a,b"], "ircBytes": 1})",           // library list joins tags with ','
        R"({"category": "sky", "tags": []})",                               // no ircBytes
        R"({"category": "sky", "tags": [], "ircBytes": -1})",
        R"({"category": "sky", "tags": [], "ircBytes": 1.5})",
        R"({"category": "sky", "tags": [], "ircBytes": "12"})",
    };
    for (const char* j : bad) {
        INFO(j);
        CHECK_THROWS_AS(parseSidecar(j), LibraryError);
    }
}

TEST_CASE("readStamp refuses a sidecar that disagrees with the art, or a stamp missing a part") {
    const fs::path dir = scratch("refuse");
    const StampFiles f = encodeStamp("Pine", "trees", {}, tree(), true);
    const StampMeta m = parseSidecar(f.json);

    put(dir / "wrong.ans", f.ans);
    put(dir / "wrong.json", sidecarJson({"trees", {}, m.ircBytes + 1}));
    try {
        readStamp(dir / "wrong.ans");
        FAIL("read a stamp whose sidecar is wrong");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()) == "sidecar says the widest row is " + std::to_string(m.ircBytes + 1) +
                                           " IRC bytes; the art's is " + std::to_string(m.ircBytes));
    }

    put(dir / "alone.ans", f.ans);
    CHECK_THROWS_AS(readStamp(dir / "alone.ans"), LibraryError);

    // No SAUCE, so no name.
    put(dir / "plain.ans", "\x1b[0;32mx\x1b[0m\r\n");
    put(dir / "plain.json", sidecarJson({"trees", {}, 0}));
    CHECK_THROWS_AS(readStamp(dir / "plain.ans"), LibraryError);

    // A SAUCE record with an empty title.
    AnsiDocument d = parseAnsi(f.ans);
    d.sauce->title.clear();
    put(dir / "untitled.ans", writeAnsi(d).bytes);
    put(dir / "untitled.json", f.json);
    CHECK_THROWS_AS(readStamp(dir / "untitled.ans"), LibraryError);

    CHECK_THROWS_AS(readStamp(dir / "missing.ans"), LibraryError);

    // Every cell blank: placing it would write nothing.
    AnsiDocument blank;
    blank.canvas = Canvas(3, 1);
    blank.sauce = Sauce::fresh(3, 1);
    blank.sauce->title = "Blank";
    const std::string blankAns = writeAnsi(blank).bytes;
    put(dir / "blank.ans", blankAns);
    put(dir / "blank.json", sidecarJson({"test", {}, widestRowBytes(parseAnsi(blankAns).canvas)}));
    try {
        readStamp(dir / "blank.ans");
        FAIL("read an all-blank stamp");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()).find("every cell is blank") != std::string::npos);
    }
}

TEST_CASE("writeStamp leaves the old stamp intact when a file can't be written") {
    const fs::path dir = scratch("atomic");
    const StampFiles old = encodeStamp("Pine", "trees", {"old"}, tree(), true);
    CellBlock wide(5, 1);
    wide.at(0, 0) = cell(U'#', Red);
    const StampFiles next = encodeStamp("Pine", "trees", {"new"}, wide, true);

    SECTION("the sidecar can't be written") {
        writeStamp(dir / "pine.ans", old);
        // A sidecar that can't be replaced: a directory in its place.
        fs::remove(dir / "pine.json");
        fs::create_directory(dir / "pine.json");
        put(dir / "pine.json" / "keep", "x");
        CHECK_THROWS_AS(writeStamp(dir / "pine.ans", next), LibraryError);
        CHECK(bwtest::readFile(dir / "pine.ans") == old.ans);
    }
    SECTION("the art can't be written") {
        writeStamp(dir / "pine.ans", old);
        fs::remove(dir / "pine.ans");
        fs::create_directory(dir / "pine.ans");
        put(dir / "pine.ans" / "keep", "x");
        CHECK_THROWS_AS(writeStamp(dir / "pine.ans", next), LibraryError);
        CHECK(bwtest::readFile(dir / "pine.json") == old.json);
    }
    SECTION("both replace the old pair when they can") {
        writeStamp(dir / "pine.ans", old);
        writeStamp(dir / "pine.ans", next);
        CHECK(readStamp(dir / "pine.ans").meta.tags == std::vector<std::string>{"new"});
        CHECK(readStamp(dir / "pine.ans").art.canvas.width() == 5);
    }
    SECTION("a symbolic link is never written through") {
        if (!bwtest::symlinksWork())
            SKIP("the symbolic-link tests don't run here: no right to make links, or wine (fs_quirks.h, #251)");
        const fs::path outside = scratch("atomic-outside") / "target.ans";
        put(outside, "outside");
        fs::create_symlink(outside, dir / "pine.ans");
        CHECK_THROWS_AS(writeStamp(dir / "pine.ans", next), LibraryError);
        CHECK(bwtest::readFile(outside) == "outside");
        CHECK_FALSE(fs::exists(dir / "pine.json"));
    }
}

TEST_CASE("a name only replaces the stamp with that title; others get -2, -3") {
    const fs::path dir = scratch("names");
    const auto save = [&](const std::string& name) {
        const StampTarget t = stampPathFor(dir, name);
        writeStamp(t.path, encodeStamp(name, "test", {}, tree(), true));
        return t;
    };
    // ♥ and ☺ (CP437 0x03 and 0x01) have no ASCII letter or digit.
    const StampTarget heart = save("\x03");
    CHECK(heart.path == dir / "u2665.ans");
    CHECK_FALSE(heart.exists);
    const StampTarget smile = save("\x01");
    CHECK(smile.path == dir / "u263a.ans");
    CHECK(readStamp(dir / "u2665.ans").name == "\x03");
    CHECK(readStamp(dir / "u263a.ans").displayName() == "☺");

    CHECK(save("Oak Tree").path == dir / "oak-tree.ans");
    const StampTarget other = save("oak-tree");
    CHECK(other.path == dir / "oak-tree-2.ans");
    CHECK_FALSE(other.exists);
    CHECK(save("OAK TREE").path == dir / "oak-tree-3.ans");
    // The same title again finds its own file, wherever it is.
    const StampTarget again = stampPathFor(dir, "oak-tree");
    CHECK(again.path == dir / "oak-tree-2.ans");
    CHECK(again.exists);
    CHECK(stampPathFor(dir, "Oak Tree").path == dir / "oak-tree.ans");
    CHECK(readStamp(dir / "oak-tree.ans").name == "Oak Tree");

    // A lone sidecar holds its name too.
    put(dir / "fir.json", "{}");
    CHECK(stampPathFor(dir, "Fir").path == dir / "fir-2.ans");
}

TEST_CASE("encodeStamp refuses what a stamp can't hold") {
    const CellBlock b = tree();
    CHECK_THROWS_AS(encodeStamp("", "trees", {}, b, true), LibraryError);
    CHECK_THROWS_AS(encodeStamp("   ", "trees", {}, b, true), LibraryError);
    CHECK_THROWS_AS(encodeStamp(std::string(36, 'x'), "trees", {}, b, true), LibraryError);
    CHECK_NOTHROW(encodeStamp(std::string(35, 'x'), "trees", {}, b, true));
    CHECK_THROWS_AS(encodeStamp("Pine ", "trees", {}, b, true), LibraryError);
    CHECK_THROWS_AS(encodeStamp(std::string("Pi\0ne", 5), "trees", {}, b, true), LibraryError);
    // CP437 0x01-0x1F are glyphs in a name (☺, ♥, ←).
    CHECK_NOTHROW(encodeStamp("\x03 \x1b", "trees", {}, b, true));
    CHECK_THROWS_AS(encodeStamp("Pine", "", {}, b, true), LibraryError);
    CHECK_THROWS_AS(encodeStamp("Pine", "trees", {""}, b, true), LibraryError);
    CHECK_THROWS_AS(encodeStamp("Pine", "trees", {"a,b"}, b, true), LibraryError);
    CHECK_THROWS_AS(encodeStamp("Pine", "trees", {}, CellBlock{}, true), LibraryError);
    CHECK_THROWS_AS(encodeStamp("Pine", "trees", {}, CellBlock(3, 2), true), LibraryError);  // all blank
    CellBlock star = b;
    star.at(0, 0) = cell(U'★', Yellow);  // no CP437 glyph
    CHECK_THROWS_AS(encodeStamp("Pine", "trees", {}, star, true), LibraryError);
}

TEST_CASE("a stamp is written as the editor saves its cells with the document's iCE") {
    CellBlock b(2, 1);
    b.at(0, 0) = cell(U'A', White, Blue + 8);  // a bright background
    b.at(1, 0) = cell(U'B', White, Red);
    b.at(1, 0).attrs = attr::Blink;
    {
        const AnsiDocument d = parseAnsi(encodeStamp("Ice", "test", {}, b, true).ans);
        CHECK(d.ice);
        CHECK(d.canvas.at(0, 0).bg.paletteIndex() == Blue + 8);
        CHECK(d.canvas.at(1, 0).bg.paletteIndex() == Red + 8);  // blink shows as bright
    }
    {
        // iCE off: the bright background is saved as blink on the dark one,
        // so nothing is lost.
        const AnsiDocument d = parseAnsi(encodeStamp("NoIce", "test", {}, b, false).ans);
        CHECK_FALSE(d.ice);
        CHECK(d.canvas.at(0, 0).bg.paletteIndex() == Blue);
        CHECK(d.canvas.at(0, 0).attrs == attr::Blink);
        CHECK(d.canvas.at(1, 0).attrs == attr::Blink);
    }
}

TEST_CASE("a stamp's blank cells leave the canvas as it is under transparent paste") {
    const Stamp s = [] {
        const fs::path dir = scratch("paste");
        writeStamp(dir / "pine.ans", encodeStamp("Pine", "trees", {}, tree(), true));
        return readStamp(dir / "pine.ans");
    }();
    Canvas cv(6, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x)
            cv.at(x, y) = cell(U'░', Blue);
    pasteBlock(cv, s.block(), 1, 1, PasteMode::Transparent).apply(cv);
    CHECK(cv.at(1, 1).ch == U'░');  // blank stamp cell
    CHECK(cv.at(4, 2).ch == U'░');
    CHECK(cv.at(2, 1).ch == U'▄');
    CHECK(cv.at(2, 2).ch == U'█');  // black █ is drawn
    CHECK(cv.at(2, 2).fg.paletteIndex() == Black);
}

TEST_CASE("scanning a library finds stamps recursively, sorted, and lists what it can't read") {
    const fs::path dir = scratch("scan");
    writeStamp(dir / "trees" / "pine.ans", encodeStamp("Pine", "trees", {"tree"}, tree(), true));
    writeStamp(dir / "b.ans", encodeStamp("Bush", "plants", {}, tree(), true));
    writeStamp(dir / "a.ans", encodeStamp("Acorn", "trees", {}, tree(), true));
    put(dir / "lonely.json", sidecarJson({"x", {}, 0}));
    put(dir / "broken.ans", encodeStamp("Broken", "x", {}, tree(), true).ans);
    put(dir / "notes.txt", "not a stamp");

    const ScanResult r = scan(dir);
    REQUIRE(r.stamps.size() == 3);
    CHECK(r.stamps[0].name == "Acorn");
    CHECK(r.stamps[1].name == "Bush");
    CHECK(r.stamps[2].name == "Pine");
    REQUIRE(r.problems.size() == 2);
    CHECK(r.problems[0].path == dir / "broken.ans");
    CHECK(r.problems[0].message == "no sidecar broken.json");
    CHECK(r.problems[1].path == dir / "lonely.json");
    CHECK(r.problems[1].message == "sidecar with no lonely.ans");

    CHECK(categories(r.stamps) == std::vector<std::string>{"plants", "trees"});

    SECTION("symbolic links: files are read through, folders and broken links are reported") {
        if (!bwtest::symlinksWork())
            SKIP("the symbolic-link tests don't run here: no right to make links, or wine (fs_quirks.h, #251)");
        const fs::path shared = scratch("scan-shared");
        writeStamp(shared / "oak.ans", encodeStamp("Oak", "trees", {}, tree(), true));
        fs::create_directory_symlink(shared, dir / "linked");
        fs::create_symlink(dir / "gone.ans", dir / "broken.ans.lnk.ans");
        fs::create_symlink(shared / "oak.ans", dir / "oak.ans");
        fs::create_symlink(shared / "oak.json", dir / "oak.json");
        const ScanResult l = scan(dir);
        REQUIRE(l.stamps.size() == 4);
        CHECK(std::any_of(l.stamps.begin(), l.stamps.end(), [](const Stamp& s) { return s.name == "Oak"; }));
        const auto problem = [&](const fs::path& p) {
            for (const Problem& pr : l.problems)
                if (pr.path == p)
                    return pr.message;
            return std::string();
        };
        CHECK(problem(dir / "linked") == "symbolic link to a folder, not followed");
        CHECK(problem(dir / "broken.ans.lnk.ans") == "broken symbolic link, skipped");
        CHECK(l.problems.size() == 4);
    }

    const ScanResult none = scan(dir / "nope");
    CHECK(none.stamps.empty());
    CHECK(none.problems.empty());
}

TEST_CASE("the bundled test library reads clean") {
    const ScanResult r = scan(bwtest::dataDir() / "library");
    CHECK(r.problems.empty());
    REQUIRE(r.stamps.size() == 2);
    CHECK(r.stamps[0].name == "Tux");
    CHECK(r.stamps[0].meta.ircBytes == 80);
    CHECK(r.stamps[1].name == "Small star");
}

TEST_CASE("search matches every term in the name or a tag, ignoring case") {
    Stamp s;
    s.name = "Tall Pine";
    s.meta = {"trees", {"forest", "Night"}, 0};
    CHECK(matches(s, ""));
    CHECK(matches(s, "   "));
    CHECK(matches(s, "pine"));
    CHECK(matches(s, "PINE"));
    CHECK(matches(s, "night"));
    CHECK(matches(s, "for"));
    CHECK(matches(s, "tall night"));
    CHECK_FALSE(matches(s, "tall oak"));
    CHECK_FALSE(matches(s, "trees"));  // the category is the category list's job
}

TEST_CASE("slug makes a file name") {
    CHECK(slug("Tall Pine") == "tall-pine");
    CHECK(slug("  sun & moon!! ") == "sun-moon");
    CHECK(slug("\x82t\x82") == "t");
    // No ASCII letter or digit: the code points.
    CHECK(slug("***") == "u002a-u002a-u002a");
    CHECK(slug("\x03") == "u2665");
    CHECK(slug("\x01 \x03") == "u263a-u2665");
    CHECK(slug(std::string(20, '\x03')).size() <= 48);
    CHECK(slug("   ") == "stamp");
    CHECK(slug(std::string(60, 'a')).size() == 48);
}

TEST_CASE("the library directories follow the install layout, or the environment") {
    qunsetenv("BLOCKWRIGHT_BUNDLED_LIBRARY");
    qunsetenv("BLOCKWRIGHT_USER_LIBRARY");
    const fs::path bundled = bundledLibraryDir("/opt/bw/bin");
    CHECK(bundled.filename() == "library");
    CHECK(bundled.parent_path().filename() == "blockwright");
    CHECK(userLibraryDir().filename() == "library");
    CHECK(userLibraryDir().parent_path().filename() == "Blockwright");
    {
        EnvGuard b("BLOCKWRIGHT_BUNDLED_LIBRARY", "/somewhere/bundled");
        EnvGuard u("BLOCKWRIGHT_USER_LIBRARY", "/somewhere/user");
        CHECK(bundledLibraryDir("/opt/bw/bin") == fs::path("/somewhere/bundled"));
        CHECK(userLibraryDir() == fs::path("/somewhere/user"));
    }
}

TEST_CASE("the bundled library is found from the real executable, or where it was started") {
    const fs::path dir = scratch("realbindir");
    fs::create_directories(dir / "prefix/bin");
    fs::create_directories(dir / "links");
    { std::ofstream(dir / "prefix/bin/blockwright-cli") << "x"; }
    if (!bwtest::symlinksWork())
        SKIP("the symbolic-link tests don't run here: no right to make links, or wine (fs_quirks.h, #251)");
    fs::create_symlink(dir / "prefix/bin/blockwright-cli", dir / "links/blockwright-cli-dev");
    const fs::path real = fs::canonical(dir / "prefix/bin");
    // through a link in another directory: the link's target's directory
    CHECK(realBinDir(dir / "links/blockwright-cli-dev", dir / "links") == real);
    CHECK(realBinDir(dir / "prefix/bin/blockwright-cli", dir / "prefix/bin") == real);
    // renamed or deleted while running: where it was started, never a relative path
    CHECK(realBinDir(dir / "prefix/bin/gone", dir / "prefix/bin") == dir / "prefix/bin");
    CHECK(realBinDir("", "/opt/bw/bin") == fs::path("/opt/bw/bin"));
}

TEST_CASE("a block past the .ans limits is refused as a LibraryError, not an AnsiError") {
    // The Codex repro: a 1x401 float turned to 401x1.
    CellBlock wide(kMaxColumns + 1, 1);
    wide.at(0, 0) = cell(U'#', Red);
    try {
        encodeStamp("Wide", "test", {}, wide, true);
        FAIL("encoded a 401-wide stamp");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()) == "the stamp is 401 x 1 cells; a stamp is at most 400 x 5000");
    }
    CellBlock tall(1, kMaxRows + 1);
    tall.at(0, 0) = cell(U'#', Red);
    CHECK_THROWS_AS(encodeStamp("Tall", "test", {}, tall, true), LibraryError);
    CellBlock edge(kMaxColumns, 1);
    edge.at(kMaxColumns - 1, 0) = cell(U'#', Red);
    CHECK_NOTHROW(encodeStamp("Edge", "test", {}, edge, true));
}

TEST_CASE("a sidecar that repeats a key is refused") {
    // The Codex repro: the first of each pair would fail, the last passes.
    try {
        parseSidecar(R"({"category":7,"category":"sky","tags":[],"ircBytes":9999,"ircBytes":16})");
        FAIL("read a sidecar with repeated keys");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()) == "sidecar has the key \"category\" more than once");
    }
    // Spelled differently, still the same key.
    CHECK_THROWS_AS(parseSidecar(R"({"tags":[],"tags":[],"category":"sky","ircBytes":1})"), LibraryError);
    // Keys inside values are not the object's.
    CHECK(parseSidecar(R"({"category":"a \"category\": b","tags":["tags"],"ircBytes":1})").category ==
          "a \"category\": b");
}

TEST_CASE("a loaded SAUCE title is held to the name rules") {
    const fs::path dir = scratch("nul-title");
    // The Codex repro: the small star's title field set to "Small\0star".
    std::string bytes = bwtest::readFile(bwtest::dataDir() / "library" / "sky" / "small-star.ans");
    const std::size_t title = bytes.size() - Sauce::kRecordSize + 7;
    REQUIRE(bytes.compare(title, 10, "Small star") == 0);
    bytes[title + 5] = '\0';
    put(dir / "star.ans", bytes);
    put(dir / "star.json", bwtest::readFile(bwtest::dataDir() / "library" / "sky" / "small-star.json"));
    try {
        readStamp(dir / "star.ans");
        FAIL("read a title holding a NUL");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()) == ".ans SAUCE title: the name holds a NUL");
    }
}

TEST_CASE("a gap in the -n series never lets a name be saved twice") {
    const fs::path dir = scratch("gap");
    for (const char* n : {"Oak Tree", "oak-tree", "OAK TREE"})
        writeStamp(stampPathFor(dir, n).path, encodeStamp(n, "test", {}, tree(), true));
    REQUIRE(fs::exists(dir / "oak-tree-3.ans"));
    fs::remove(dir / "oak-tree-2.ans");
    fs::remove(dir / "oak-tree-2.json");
    // #582 N1: OAK TREE is still in -3, so that is where it goes.
    const StampTarget t = stampPathFor(dir, "OAK TREE");
    CHECK(t.path == dir / "oak-tree-3.ans");
    CHECK(t.exists);
    // A new title takes the lowest free slot.
    const StampTarget fresh = stampPathFor(dir, "Oak; Tree");
    CHECK(fresh.path == dir / "oak-tree-2.ans");
    CHECK_FALSE(fresh.exists);
    // Other slugs that start the same way are not this one's slots.
    writeStamp(dir / "oak-tree-x.ans", encodeStamp("Oak Tree X", "test", {}, tree(), true));
    writeStamp(dir / "oak-tree-02.ans", encodeStamp("Oak Tree 02", "test", {}, tree(), true));
    CHECK(stampPathFor(dir, "Oak; Tree").path == dir / "oak-tree-2.ans");
}

TEST_CASE("writeStamp says when the old sidecar can't be put back") {
    const fs::path dir = scratch("restore");
    const StampFiles old = encodeStamp("Pine", "trees", {"old"}, tree(), true);
    CellBlock wide(5, 1);
    wide.at(0, 0) = cell(U'#', Red);
    const StampFiles next = encodeStamp("Pine", "trees", {"new"}, wide, true);
    writeStamp(dir / "pine.ans", old);
    struct Reset {
        ~Reset() { testing::writeStampFault = nullptr; }
    } reset;

    SECTION("the art's rename fails: the old pair is put back") {
        testing::writeStampFault = [](std::string_view step) {
            if (step == "art")
                throw LibraryError("art: injected");
        };
        try {
            writeStamp(dir / "pine.ans", next);
            FAIL("no error");
        } catch (const LibraryError& e) {
            CHECK(std::string(e.what()) == "art: injected");
        }
        CHECK(bwtest::readFile(dir / "pine.ans") == old.ans);
        CHECK(bwtest::readFile(dir / "pine.json") == old.json);
        CHECK_NOTHROW(readStamp(dir / "pine.ans"));
    }
    SECTION("and the restore fails too: the error says so") {
        testing::writeStampFault = [](std::string_view step) {
            if (step == "art" || step == "restore")
                throw LibraryError(std::string(step) + ": injected");
        };
        try {
            writeStamp(dir / "pine.ans", next);
            FAIL("no error");
        } catch (const LibraryError& e) {
            CHECK(std::string(e.what()) == "art: injected; the old sidecar could not be put back either (restore: "
                                           "injected), so the new sidecar no longer matches the old art");
        }
    }
    SECTION("a new stamp whose art fails leaves no sidecar") {
        testing::writeStampFault = [](std::string_view step) {
            if (step == "art")
                throw LibraryError("art: injected");
        };
        CHECK_THROWS_AS(writeStamp(dir / "fir.ans", next), LibraryError);
        CHECK_FALSE(fs::exists(dir / "fir.json"));
        CHECK_FALSE(fs::exists(dir / "fir.ans"));
    }
}

TEST_CASE("files that aren't regular are never opened") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    const fs::path dir = scratch("fifo");
    writeStamp(dir / "pine.ans", encodeStamp("Pine", "trees", {}, tree(), true));
    // The Codex repro: a FIFO with no writer at a stamp's path.
    REQUIRE(::mkfifo((dir / "fir.ans").c_str(), 0600) == 0);
    REQUIRE(::mkfifo((dir / "oak.json").c_str(), 0600) == 0);
    put(dir / "oak.ans", encodeStamp("Oak", "trees", {}, tree(), true).ans);

    try {
        readStamp(dir / "fir.ans");
        FAIL("read a FIFO");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()) == ".ans is not a regular file");
    }
    try {
        readStamp(dir / "oak.ans");
        FAIL("read a FIFO sidecar");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()) == "sidecar oak.json is not a regular file");
    }
    // Save to Library goes past it, and won't write onto it.
    const StampTarget t = stampPathFor(dir, "Fir");
    CHECK(t.path == dir / "fir-2.ans");
    CHECK_FALSE(t.exists);
    CHECK_THROWS_AS(writeStamp(dir / "fir.ans", encodeStamp("Fir", "trees", {}, tree(), true)), LibraryError);
    CHECK_THROWS_AS(writeStamp(dir / "oak.ans", encodeStamp("Oak", "trees", {}, tree(), true)), LibraryError);

    const ScanResult r = scan(dir);
    CHECK(r.stamps.size() == 1);
    bool fifoListed = false;
    for (const Problem& p : r.problems)
        if (p.path == dir / "fir.ans")
            fifoListed = p.message == "not a regular file, skipped";
    CHECK(fifoListed);
#endif
}

TEST_CASE("a folder that can't be read is a problem, not a silent skip") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root reads every folder");
    const fs::path dir = scratch("unreadable");
    writeStamp(dir / "ok.ans", encodeStamp("Ok", "test", {}, tree(), true));
    writeStamp(dir / "locked" / "bad.ans", encodeStamp("Bad", "test", {}, tree(), true));
    fs::permissions(dir / "locked", fs::perms::none);
    const ScanResult r = scan(dir);
    fs::permissions(dir / "locked", fs::perms::owner_all);
    REQUIRE(r.stamps.size() == 1);
    REQUIRE(r.problems.size() == 1);
    CHECK(r.problems[0].path == dir / "locked");
    CHECK(r.problems[0].message.rfind("folder can't be read (", 0) == 0);
#endif
}

TEST_CASE("a library folder that can't be listed is refused, not taken as empty") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root lists every folder");
    // The Codex/Claude repro: a write-and-search-only (0300) folder holding
    // Oak Tree. Taken as empty, saving oak-tree would replace it unasked.
    const fs::path dir = scratch("unlistable");
    writeStamp(dir / "oak-tree.ans", encodeStamp("Oak Tree", "test", {}, tree(), true));
    fs::permissions(dir, fs::perms::owner_write | fs::perms::owner_exec);
    std::string message;
    try {
        stampPathFor(dir, "oak-tree");
    } catch (const LibraryError& e) {
        message = e.what();
    }
    fs::permissions(dir, fs::perms::owner_all);
    CHECK(message == dir.string() + " can't be listed: Permission denied");
    CHECK(readStamp(dir / "oak-tree.ans").name == "Oak Tree");
    // A folder that doesn't exist yet is empty.
    CHECK(stampPathFor(dir / "new", "Oak").path == dir / "new" / "oak.ans");
#endif
}

TEST_CASE("a FIFO is refused by the opened handle, without blocking") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    const fs::path dir = scratch("fifo-handle");
    const fs::path fifo = dir / "pipe.ans";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    // Straight to the reader, past every path check, as if a FIFO had been
    // swapped in after them.
    auto result = std::make_shared<std::promise<std::string>>();
    std::future<std::string> done = result->get_future();
    std::thread([fifo, result] {
        try {
            readRegularFile(fifo);
            result->set_value("read");
        } catch (const LibraryError& e) {
            result->set_value(e.what());
        }
    }).detach();
    if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        // Unblock the reader so the test process can end, then fail.
        const int fd = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0)
            ::close(fd);
        FAIL("reading a FIFO blocked");
    }
    CHECK(done.get() == "is not a regular file");
    // And a regular file still reads.
    put(dir / "x.ans", "hello");
    CHECK(readRegularFile(dir / "x.ans") == "hello");
    CHECK_THROWS_AS(readRegularFile(dir / "missing.ans"), LibraryError);
#endif
}

TEST_CASE("a stamp is written under its exact native name") {
#ifndef __linux__
    SKIP("byte file names are a Linux test");
#else
    const fs::path dir = scratch("native");
    writeStamp(dir / "\xfd.ans", encodeStamp("FD", "test", {}, tree(), true));
    writeStamp(dir / "caf\xe9" / "x.ans", encodeStamp("Cafe", "test", {}, tree(), true));
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir))
        names.push_back(e.path().filename().native());
    std::sort(names.begin(), names.end());
    // No U+FFFD stand-ins, and no temporaries left behind.
    CHECK(names == std::vector<std::string>{"caf\xe9", "\xfd.ans", "\xfd.json"});
    CHECK(readStamp(dir / "\xfd.ans").name == "FD");
    CHECK(readStamp(dir / "caf\xe9" / "x.ans").name == "Cafe");
    const ScanResult r = scan(dir);
    CHECK(r.stamps.size() == 2);
    CHECK(r.problems.empty());
#endif
}

TEST_CASE("files in a folder that can be listed but not searched can't be read") {
#ifdef _WIN32
    SKIP("no FIFOs or POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root searches every folder");
    const fs::path dir = scratch("unsearchable");
    writeStamp(dir / "sub" / "pine.ans", encodeStamp("Pine", "test", {}, tree(), true));
    fs::permissions(dir / "sub", fs::perms::owner_read);
    const ScanResult r = scan(dir);
    fs::permissions(dir / "sub", fs::perms::owner_all);
    REQUIRE(r.problems.size() == 2);
    for (const Problem& p : r.problems) {
        INFO(p.path.string() << ": " << p.message);
        CHECK(p.message == "can't be read (Permission denied), skipped");
    }
#endif
}

namespace {

std::vector<std::string> namesIn(const fs::path& dir) {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(dir))
        out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

#ifdef __linux__
std::size_t openDescriptors() {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator("/proc/self/fd"))
        ++n;
    return n;
}
#endif

} // namespace

TEST_CASE("a case variant of a slot's file name is that slot") {
    // Codex round 3 P1, on a case-sensitive volume: OAK-TREE.ans is
    // oak-tree.ans on a case-insensitive one, so it is never a fresh slot.
    const fs::path dir = scratch("case");
    writeStamp(dir / "OAK-TREE.ans", encodeStamp("Oak Tree", "test", {}, tree(), true));
    const StampTarget other = stampPathFor(dir, "oak-tree");
    CHECK(other.path == dir / "oak-tree-2.ans");
    CHECK_FALSE(other.exists);
    // The same title is that stamp, as it is named: saving replaces it.
    const StampTarget same = stampPathFor(dir, "Oak Tree");
    CHECK(same.path == dir / "OAK-TREE.ans");
    CHECK(same.exists);
    // Upper-case extensions and -N slots count too.
    put(dir / "Oak-Tree-2.JSON", "{}");
    CHECK(stampPathFor(dir, "oak-tree").path == dir / "oak-tree-3.ans");
}

TEST_CASE("a failed write leaves no temporary file, no open descriptor, and the old pair") {
    const fs::path dir = scratch("staged");
    const StampFiles old = encodeStamp("Pine", "trees", {"old"}, tree(), true);
    CellBlock big(300, 10);
    for (int x = 0; x < 300; ++x)
        big.at(x, 0) = cell(U'#', Red);
    const StampFiles next = encodeStamp("Pine", "trees", {"new"}, big, true);
    writeStamp(dir / "pine.ans", old);
    const std::vector<std::string> before = namesIn(dir);
    REQUIRE(before == std::vector<std::string>{"pine.ans", "pine.json"});

#ifndef _WIN32
    SECTION("a write that runs out of room part way (RLIMIT_FSIZE, in a child)") {
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            ::signal(SIGXFSZ, SIG_IGN);
            const rlimit lim{100, RLIM_INFINITY};
            ::setrlimit(RLIMIT_FSIZE, &lim);
            int status = 1;
            try {
                writeStamp(dir / "pine.ans", next);
            } catch (const LibraryError& e) {
                status = std::string(e.what()).find("File too large") != std::string::npos ? 0 : 2;
            }
            ::_exit(status);
        }
        int status = -1;
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(namesIn(dir) == before);
        CHECK(bwtest::readFile(dir / "pine.ans") == old.ans);
        CHECK(bwtest::readFile(dir / "pine.json") == old.json);
    }
#endif
    SECTION("fsync fails") {
        struct Reset {
            ~Reset() { testing::writeStampFault = nullptr; }
        } reset;
        testing::writeStampFault = [](std::string_view step) {
            if (step == "fsync")
                throw LibraryError("fsync: injected");
        };
#ifdef __linux__
        const std::size_t fds = openDescriptors();
#endif
        try {
            writeStamp(dir / "pine.ans", next);
            FAIL("no error");
        } catch (const LibraryError& e) {
            CHECK(std::string(e.what()).find("write failed: No space left on device") != std::string::npos);
        }
#ifdef __linux__
        CHECK(openDescriptors() == fds);  // /proc/self/fd is Linux's
#endif
        CHECK(namesIn(dir) == before);
        CHECK(bwtest::readFile(dir / "pine.ans") == old.ans);
        CHECK(bwtest::readFile(dir / "pine.json") == old.json);
    }
}

TEST_CASE("replacing a stamp keeps its files' permissions") {
#ifdef _WIN32
    SKIP("Windows files have no POSIX permission bits");
#endif
    const fs::path dir = scratch("mode");
    const StampFiles old = encodeStamp("Pine", "trees", {"old"}, tree(), true);
    const StampFiles next = encodeStamp("Pine", "trees", {"new"}, tree(), true);
    writeStamp(dir / "pine.ans", old);
    const auto mode = [](const fs::path& p) { return fs::status(p).permissions() & fs::perms::mask; };
    const fs::perms ownerRw = fs::perms::owner_read | fs::perms::owner_write;
    fs::permissions(dir / "pine.ans", ownerRw);
    fs::permissions(dir / "pine.json", ownerRw);

    SECTION("a replace") {
        writeStamp(dir / "pine.ans", next);
        CHECK(readStamp(dir / "pine.ans").meta.tags == std::vector<std::string>{"new"});
        CHECK(mode(dir / "pine.ans") == ownerRw);
        CHECK(mode(dir / "pine.json") == ownerRw);
    }
    SECTION("a rollback") {
        struct Reset {
            ~Reset() { testing::writeStampFault = nullptr; }
        } reset;
        testing::writeStampFault = [](std::string_view step) {
            if (step == "art")
                throw LibraryError("art: injected");
        };
        CHECK_THROWS_AS(writeStamp(dir / "pine.ans", next), LibraryError);
        CHECK(bwtest::readFile(dir / "pine.json") == old.json);
        CHECK(mode(dir / "pine.ans") == ownerRw);
        CHECK(mode(dir / "pine.json") == ownerRw);
    }
    SECTION("set-ID and sticky bits are not kept") {
        fs::permissions(dir / "pine.ans", fs::perms(04755));
        writeStamp(dir / "pine.ans", next);
        CHECK(mode(dir / "pine.ans") == fs::perms(0755));
        CHECK(mode(dir / "pine.json") == ownerRw);
    }
    SECTION("a new stamp gets 0666 less the umask") {
        const mode_t mask = ::umask(022);
        writeStamp(dir / "fir.ans", next);
        ::umask(mask);
        CHECK(mode(dir / "fir.ans") == (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                        fs::perms::others_read));
    }
}

TEST_CASE("an art file whose extension is in another case is scanned, read and replaced") {
    // Codex round 4 P2: Oak Tree with its art renamed to oak-tree.ANS. A
    // replace goes to oak-tree.ANS (stampPathFor), so scan must see it.
    const fs::path dir = scratch("ext-case");
    writeStamp(dir / "oak-tree.ans", encodeStamp("Oak Tree", "test", {"old"}, tree(), true));
    fs::rename(dir / "oak-tree.ans", dir / "oak-tree.ANS");
    if (!bwtest::caseRenamed(dir / "oak-tree.ANS"))
        SKIP("this file system keeps a file's old case through a rename");
    const StampTarget t = stampPathFor(dir, "Oak Tree");
    REQUIRE(t.path == dir / "oak-tree.ANS");
    REQUIRE(t.exists);
    writeStamp(t.path, encodeStamp("Oak Tree", "test", {"new"}, tree(), true));
    ScanResult r = scan(dir);
    CHECK(r.problems.empty());
    REQUIRE(r.stamps.size() == 1);
    CHECK(r.stamps[0].path == dir / "oak-tree.ANS");
    CHECK(r.stamps[0].meta.tags == std::vector<std::string>{"new"});
    CHECK(namesIn(dir) == std::vector<std::string>{"oak-tree.ANS", "oak-tree.json"});

    // A sidecar in another case pairs too, and a replace writes over it.
    fs::rename(dir / "oak-tree.json", dir / "oak-tree.JSON");
    CHECK(pairedSidecar(dir / "oak-tree.ANS") == dir / "oak-tree.JSON");
    r = scan(dir);
    CHECK(r.problems.empty());
    REQUIRE(r.stamps.size() == 1);
    writeStamp(dir / "oak-tree.ANS", encodeStamp("Oak Tree", "test", {"newer"}, tree(), true));
    CHECK(namesIn(dir) == std::vector<std::string>{"oak-tree.ANS", "oak-tree.JSON"});
    CHECK(readStamp(dir / "oak-tree.ANS").meta.tags == std::vector<std::string>{"newer"});
    CHECK(isArtName("x.AnS"));
    CHECK(isSidecarName("x.JSON"));
    CHECK_FALSE(isArtName("x.ans.bak"));
}

TEST_CASE("a text style preset gets the lowest free <slug>.style.json and never replaces a file") {
    const fs::path dir = scratch("presets");
    TextStyleEntry entry;
    entry.name = "Neon Sign";
    entry.category = "neon";
    entry.style = TextStyle{{Effect{true, OutlineEffect{}}}};

    // a folder that doesn't exist yet is made
    const fs::path inside = dir / "user";
    CHECK(textStylePathFor(inside, entry.name) == inside / "neon-sign.style.json");
    const fs::path first = writeTextStyle(inside, entry);
    CHECK(first == inside / "neon-sign.style.json");
    CHECK(readTextStyleEntry(first).style == entry.style);

    // a file in any case, of any kind, holds its slot
    put(inside / "NEON-SIGN-2.STYLE.JSON", "not mine");
    entry.tags = {"second"};
    const fs::path third = writeTextStyle(inside, entry);
    CHECK(third == inside / "neon-sign-3.style.json");
    CHECK(bwtest::readFile(inside / "NEON-SIGN-2.STYLE.JSON") == "not mine");
    CHECK(readTextStyleEntry(first).tags.empty());
    CHECK(readTextStyleEntry(third).tags == std::vector<std::string>{"second"});

    // an entry the parser would refuse is refused before anything is written
    TextStyleEntry bad = entry;
    bad.name = "Bad";
    bad.category = "";
    CHECK_THROWS_AS(writeTextStyle(inside, bad), LibraryError);
    CHECK_FALSE(fs::exists(inside / "bad.style.json"));
}

TEST_CASE("a text style preset keeps a Glow brightness, and one written without it reads at the width's rung") {
    const fs::path dir = scratch("presets-glow");
    TextStyleEntry entry;
    entry.name = "Halo";
    entry.category = "glow";
    entry.style = TextStyle{{Effect{true, GlowEffect{"toxic", 2, 7}}}};
    const fs::path written = writeTextStyle(dir, entry);
    CHECK(readTextStyleEntry(written).style == entry.style);

    put(dir / "old.style.json", R"({
  "kind": "text-style",
  "name": "Old",
  "category": "glow",
  "tags": [],
  "effects": [
    {"kind": "glow", "enabled": true, "ramp": "fire", "width": 3}
  ]
})");
    CHECK(readTextStyleEntry(dir / "old.style.json").style == TextStyle{{Effect{true, GlowEffect{"fire", 3, 3}}}});
}

#ifndef _WIN32
TEST_CASE("a text style preset published by the hard-link fallback leaves no temporary, even when unlink fails") {
    const fs::path dir = scratch("presets-link");
    TextStyleEntry entry;
    entry.name = "Neon";
    entry.category = "neon";
    entry.style = TextStyle{{Effect{true, OutlineEffect{}}}};
    struct Reset {
        ~Reset() { testing::textStylePublishFault = nullptr; }
    } reset;
    // a file system without the exclusive rename, and the temporary's name
    // failing to go once the preset is linked into place
    std::vector<std::string> steps;
    testing::textStylePublishFault = [&steps](std::string_view step) {
        steps.emplace_back(step);
        throw LibraryError(std::string(step));
    };
    const fs::path p = writeTextStyle(dir, entry);
    CHECK(steps == std::vector<std::string>{"rename", "unlink"});
    CHECK(p == dir / "neon.style.json");
    CHECK(readTextStyleEntry(p).name == "Neon");
    std::vector<std::string> names;
    for (const fs::directory_entry& f : fs::directory_iterator(dir))
        names.push_back(f.path().filename().string());
    CHECK(names == std::vector<std::string>{"neon.style.json"});
}
#endif

TEST_CASE("a text style preset never replaces a file made at its path while it is written") {
    const fs::path dir = scratch("presets-race");
    TextStyleEntry entry;
    entry.name = "Neon";
    entry.category = "neon";
    entry.style = TextStyle{{Effect{true, OutlineEffect{}}}};
    struct Reset {
        ~Reset() { testing::beforeTextStylePublish = nullptr; }
    } reset;
    // another editor saves there between the slot being picked and the
    // preset being moved into place
    testing::beforeTextStylePublish = [](const fs::path& p) { put(p, "sentinel"); };
    try {
        writeTextStyle(dir, entry);
        FAIL("no error");
    } catch (const LibraryError& e) {
        CHECK(std::string(e.what()).find("already exists") != std::string::npos);
    }
    CHECK(bwtest::readFile(dir / "neon.style.json") == "sentinel");
    // the temporary is gone: the sentinel is all the folder holds
    std::vector<std::string> names;
    for (const fs::directory_entry& f : fs::directory_iterator(dir))
        names.push_back(f.path().filename().string());
    CHECK(names == std::vector<std::string>{"neon.style.json"});

    // with the hook off the next save takes the next slot
    testing::beforeTextStylePublish = nullptr;
    CHECK(writeTextStyle(dir, entry) == dir / "neon-2.style.json");
    CHECK(bwtest::readFile(dir / "neon.style.json") == "sentinel");
}
