// Junctions on Windows (#139): std::filesystem reads one as the folder it
// leads to, never as a link, so every walk that refuses or doesn't follow a
// symbolic link asks links.h instead. A junction is refused or skipped
// exactly as a symbolic link is on Linux, with the same messages. They are
// made with `mklink /J`, which needs no privilege (wine makes them too).

#include "blockwright/font_store.h"
#include "blockwright/library.h"
#include "blockwright/links.h"
#include "blockwright/project.h"

#include "generators.h"
#include "read_file.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <process.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace bw;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
namespace fs = std::filesystem;

namespace {

// Removes `p` and what is under it, a link removed and never walked into:
// fs::remove_all walks into a junction, and under wine leaves it there.
void removeTree(const fs::path& p) {
    std::error_code ec;
    if (!isLink(p) && fs::is_directory(fs::symlink_status(p, ec))) {
        std::vector<fs::path> under;
        for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
            under.push_back(it->path());
        for (const fs::path& u : under)
            removeTree(u);
    }
    fs::remove(p, ec);
}

// A fresh folder for one test, removed when the test ends. Wine can't open a
// junction a process before this one made (it is listed but reads as missing),
// so each test removes its own while they still open, and each run gets its
// own folder so what a killed run left can't get in the way.
struct Scratch {
    fs::path dir;
    explicit Scratch(const std::string& name)
        : dir(fs::absolute(fs::path(BW_TEST_TMP) / ("junction-" + std::to_string(_getpid())) / name)) {
        removeTree(dir);
        REQUIRE_FALSE(fs::exists(fs::symlink_status(dir)));
        fs::create_directories(dir);
    }
    ~Scratch() {
        removeTree(dir);
        std::error_code ec;
        fs::remove(dir.parent_path(), ec); // when it is the last one
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

// A junction at `link` to the folder `target`.
void junction(const fs::path& link, const fs::path& target) {
    const std::wstring cmd = L"mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >NUL";
    REQUIRE(_wsystem(cmd.c_str()) == 0);
    REQUIRE(fs::exists(fs::symlink_status(link)));
}

void writeBytes(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

std::size_t entriesIn(const fs::path& dir) {
    return std::size_t(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

project::Project dots() {
    Canvas c(4, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            c.at(x, y) = Cell{U'.', Color::indexed(7), Color::indexed(0), 0, false};
    AnsiDocument doc;
    doc.canvas = c;
    doc.ice = false;
    return project::fromAnsi(doc);
}

// A saved project whose layers\ folder is a junction to `outside`, which
// holds what layers\ held.
fs::path projectThroughJunction(const fs::path& dir, const fs::path& outside) {
    const fs::path proj = dir / "proj";
    project::writeDirectory(dots(), proj);
    fs::create_directories(outside);
    for (const fs::directory_entry& e : fs::directory_iterator(proj / "layers"))
        fs::rename(e.path(), outside / e.path().filename());
    fs::remove(proj / "layers");
    junction(proj / "layers", outside);
    return proj;
}

} // namespace

TEST_CASE("a junction reads as a link; a folder and a file don't", "[junction]") {
    const Scratch sc("kinds");
    const fs::path& dir = sc.dir;
    fs::create_directories(dir / "folder");
    writeBytes(dir / "file.txt", "x");
    junction(dir / "link", dir / "folder");
    std::error_code ec;
    CHECK(fs::is_symlink(linkStatus(dir / "link", ec)));
    CHECK(isLink(dir / "link"));
    CHECK(isLink(fs::directory_entry(dir / "link")));
    CHECK(fs::is_directory(linkStatus(dir / "folder", ec)));
    CHECK_FALSE(isLink(dir / "folder"));
    CHECK(fs::is_regular_file(linkStatus(dir / "file.txt", ec)));
    CHECK_FALSE(isLink(dir / "file.txt"));
    CHECK_FALSE(fs::exists(linkStatus(dir / "missing", ec)));
}

TEST_CASE("a project read refuses a junction inside it", "[junction][project]") {
    const Scratch sc("read");
    const fs::path& dir = sc.dir;
    const fs::path proj = projectThroughJunction(dir, dir / "outside");
    try {
        project::read(proj);
        FAIL("read through a junction");
    } catch (const project::ProjectError& e) {
        CHECK_THAT(std::string(e.what()), EndsWith("layers is a symbolic link"));
    }
}

TEST_CASE("a project save refuses a junction inside it and writes nothing through it", "[junction][project]") {
    const Scratch sc("save");
    const fs::path& dir = sc.dir;
    const fs::path proj = dir / "proj";
    project::writeDirectory(dots(), proj);
    fs::create_directories(dir / "outside");
    fs::remove_all(proj / "layers");
    junction(proj / "layers", dir / "outside");
    try {
        project::writeDirectory(dots(), proj);
        FAIL("saved through a junction");
    } catch (const project::ProjectError& e) {
        CHECK_THAT(std::string(e.what()), EndsWith("layers is a symbolic link"));
    }
    CHECK(entriesIn(dir / "outside") == 0);
    CHECK(isLink(proj / "layers"));
}

TEST_CASE("a project save refuses a junction where a file of it goes", "[junction][project]") {
    const Scratch sc("save-file");
    const fs::path& dir = sc.dir;
    const fs::path proj = dir / "proj";
    project::writeDirectory(dots(), proj);
    fs::create_directories(dir / "outside");
    std::string file;
    for (const fs::directory_entry& e : fs::directory_iterator(proj / "layers"))
        if (e.path().extension() == ".cells")
            file = e.path().filename().string();
    REQUIRE_FALSE(file.empty());
    fs::remove(proj / "layers" / file);
    junction(proj / "layers" / file, dir / "outside");
    CHECK_THROWS_WITH(project::writeDirectory(dots(), proj), EndsWith(file + " is a symbolic link"));
    CHECK(entriesIn(dir / "outside") == 0);
}

TEST_CASE("the library scan doesn't follow a junction to a folder", "[junction][library]") {
    const Scratch sc("library");
    const fs::path& dir = sc.dir;
    const fs::path lib = dir / "lib";
    fs::create_directories(lib);
    fs::create_directories(dir / "outside");
    for (const char* f : {"small-star.ans", "small-star.json"})
        fs::copy_file(fs::path(BW_TEST_DATA) / "library" / "sky" / f, dir / "outside" / f);
    junction(lib / "ext", dir / "outside");
    const library::ScanResult r = library::scan(lib);
    CHECK(r.stamps.empty());
    REQUIRE(r.problems.size() == 1);
    CHECK(r.problems[0].path == lib / "ext");
    CHECK(r.problems[0].message == "symbolic link to a folder, not followed");
    // the folder itself, scanned, has the stamp
    CHECK(library::scan(dir / "outside").stamps.size() == 1);
}

TEST_CASE("the fonts folder scan doesn't walk into a junction", "[junction][fonts]") {
    const Scratch sc("fonts");
    const fs::path& dir = sc.dir;
    fs::create_directories(dir / "fonts");
    fs::create_directories(dir / "outside");
    fs::copy_file(fs::path(BW_TEST_DATA) / "tdf" / "bw-block.tdf", dir / "outside" / "bw-block.tdf");
    junction(dir / "fonts" / "ext", dir / "outside");
    const FontCatalogue cat = scanFontsFolder(dir / "fonts");
    CHECK(cat.fonts.empty());
    CHECK(cat.rejected.empty());
    CHECK_FALSE(scanFontsFolder(dir / "outside").fonts.empty());
}

TEST_CASE("a generator's folder can't be a junction or hold one", "[junction][generator]") {
    const Scratch sc("generator");
    const fs::path& dir = sc.dir;
    const fs::path gen = dir / "gen";
    writeBytes(gen / "generator.json", R"({"command": ["bw-gen-helper.exe"]})");
    fs::create_directories(dir / "outside");
    writeBytes(dir / "outside" / "util.txt", "u");

    junction(dir / "to-gen", gen);
    try {
        bwcli::readGeneratorDir(dir / "to-gen");
        FAIL("read a generator through a junction");
    } catch (const bwcli::Failure& f) {
        CHECK_THAT(f.message, EndsWith("to-gen: not a directory"));
    }

    junction(gen / "lib", dir / "outside");
    try {
        bwcli::readGeneratorDir(gen);
        FAIL("read a generator holding a junction");
    } catch (const bwcli::Failure& f) {
        CHECK_THAT(f.message, ContainsSubstring("lib: not a plain file"));
    }

    // hidden, it is never walked into
    fs::remove(gen / "lib");
    junction(gen / ".lib", dir / "outside");
    CHECK(bwcli::readGeneratorDir(gen).size() == 1);
}
