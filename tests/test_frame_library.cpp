// User frame styles (#345 slice 3): a selection cut into a frame style
// (frameStyleFromCells), its .frame.json in a library, and the scan that
// finds it.

#include "blockwright/frame.h"
#include "blockwright/library.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QByteArray>
#include <QtGlobal>

#include <climits>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace bw;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "frame-library" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

Cell cell(char32_t ch, int fg, int bg = Black) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(std::uint8_t(fg));
    c.bg = Color::indexed(std::uint8_t(bg));
    return c;
}

// A 4x3 box drawn as a selection would hold it: + corners, - and | edges
// in two colours, a dash gap (a blank, so transparent), and an x in the
// middle that only a filled style draws.
Canvas box() {
    Canvas c(4, 3);
    const std::u32string rows[] = {U"+- +", U"|xx|", U"+--+"};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 4; ++x)
            if (rows[y][x] != U' ')
                c.at(x, y) = cell(rows[y][x], x == 0 ? LightRed : White, Blue);
    return c;
}

std::u32string rowText(const Canvas& c, int y) {
    std::u32string s;
    for (int x = 0; x < c.width(); ++x)
        s += c.at(x, y).transparent ? U'.' : c.at(x, y).ch.codePoint().value_or(U'?');
    return s;
}

} // namespace

TEST_CASE("a selection is cut into a frame style by its insets") {
    std::string why;
    const auto s = frameStyleFromCells(box(), "box", "Box", 1, 1, 1, 1, false, false, &why);
    INFO(why);
    REQUIRE(s);
    CHECK(s->valid());
    CHECK(s->id == "box");
    CHECK(s->name == "Box");
    CHECK(s->minimumWidth() == 2);
    CHECK(s->minimumHeight() == 2);
    // the blank gap is transparent, and so is the open middle
    CHECK(rowText(s->source, 0) == U"+-.+");
    CHECK(rowText(s->source, 1) == U"|..|");
    CHECK(s->keepBg.empty());
    CHECK(s->source.at(0, 0).fg == Color::indexed(LightRed));
    CHECK(s->source.at(0, 0).bg == Color::indexed(Blue));

    // drawn larger, the edges repeat and the gap stays a gap
    const auto drawn = drawFrame(*s, 7, 4);
    REQUIRE(drawn);
    CHECK(rowText(*drawn, 0) == U"+-.-.-+");
    CHECK(rowText(*drawn, 1) == U"|.....|");
    CHECK(rowText(*drawn, 3) == U"+-----+");

    // its JSON reads back as the same style
    const auto back = frameStyleFromJson(frameStyleJson(*s), &why);
    INFO(why);
    REQUIRE(back);
    CHECK(back->source == s->source);
    CHECK(back->left == 1);
    CHECK_FALSE(back->fill);
}

TEST_CASE("a filled frame style keeps its middle, and keep background marks every drawn cell") {
    const auto s = frameStyleFromCells(box(), "box", "Box", 1, 1, 1, 1, true, true);
    REQUIRE(s);
    CHECK(rowText(s->source, 1) == U"|xx|");
    CHECK(s->keepsBackground(1, 1));
    CHECK(s->keepsBackground(0, 0));
    CHECK_FALSE(s->keepsBackground(2, 0));  // the gap: transparent, nothing to keep
    const auto back = frameStyleFromJson(frameStyleJson(*s));
    REQUIRE(back);
    CHECK(back->keepBg == s->keepBg);

    // thicker corners: two columns on the left
    const auto wide = frameStyleFromCells(box(), "box", "Box", 2, 1, 1, 1, true, false);
    REQUIRE(wide);
    CHECK(wide->minimumWidth() == 3);
}

TEST_CASE("a frame style is refused what the JSON form can't hold") {
    std::string why;
    SECTION("insets that leave no edge") {
        CHECK_FALSE(frameStyleFromCells(box(), "b", "B", 2, 2, 1, 1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("no edge to repeat"));
        CHECK_FALSE(frameStyleFromCells(box(), "b", "B", 1, 1, 1, 2, false, false, &why));
        CHECK_FALSE(frameStyleFromCells(Canvas(2, 2), "b", "B", 1, 1, 1, 1, false, false, &why));
    }
    SECTION("insets that would overflow when added, said as they are") {
        CHECK_FALSE(frameStyleFromCells(Canvas(3, 3), "", "bad", INT_MAX, INT_MAX, 1, 1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("the corners 4294967294x2 "));
        CHECK_FALSE(frameStyleFromCells(Canvas(3, 3), "", "bad", 1, 1, INT_MAX, INT_MAX, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("the corners 2x4294967294 "));
    }
    SECTION("a selection larger than a source may be") {
        const auto sized = [](int w, int h) {
            Canvas c(w, h);
            c.at(0, 0) = cell(U'+', White);
            return c;
        };
        std::string at;
        const auto most = frameStyleFromCells(sized(kMaxFrameSourceColumns, kMaxFrameSourceRows), "", "Most", 1, 1,
                                              1, 1, false, false, &at);
        INFO(at);
        REQUIRE(most);
        const fs::path p = library::writeFrameStyle(scratch("limit"), *most);
        CHECK(library::readFrameStyleEntry(p).style.source == most->source);
        CHECK_FALSE(frameStyleFromCells(sized(kMaxFrameSourceColumns + 1, kMaxFrameSourceRows), "", "Wide", 1, 1, 1,
                                        1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("the selection is 101x100, larger than"));
        CHECK_FALSE(frameStyleFromCells(sized(kMaxFrameSourceColumns, kMaxFrameSourceRows + 1), "", "Tall", 1, 1, 1,
                                        1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("the selection is 100x101, larger than"));
    }
    SECTION("a colour past the 16") {
        Canvas c = box();
        c.at(1, 2).fg = Color::fromRgb(Rgb{10, 20, 30});
        CHECK_FALSE(frameStyleFromCells(c, "b", "B", 1, 1, 1, 1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("column 2, row 3"));
        CHECK_THAT(why, ContainsSubstring("colour"));
    }
    SECTION("blink") {
        Canvas c = box();
        c.at(0, 1).attrs = 1;
        CHECK_FALSE(frameStyleFromCells(c, "b", "B", 1, 1, 1, 1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("blinks"));
    }
    SECTION("a wide glyph") {
        Canvas c = box();
        c.at(1, 0).ch = U'中';
        CHECK_FALSE(frameStyleFromCells(c, "b", "B", 1, 1, 1, 1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("glyph"));
    }
    SECTION("nothing drawn") {
        Canvas c(3, 3);
        c.at(1, 1) = cell(U'x', White);  // only in the open middle
        CHECK_FALSE(frameStyleFromCells(c, "b", "B", 1, 1, 1, 1, false, false, &why));
        CHECK_THAT(why, ContainsSubstring("blank"));
        CHECK(frameStyleFromCells(c, "b", "B", 1, 1, 1, 1, true, false, &why));
    }
    SECTION("an open middle isn't checked") {
        Canvas c = box();
        c.at(1, 1).fg = Color::fromRgb(Rgb{1, 2, 3});
        CHECK(frameStyleFromCells(c, "b", "B", 1, 1, 1, 1, false, false));
        CHECK_FALSE(frameStyleFromCells(c, "b", "B", 1, 1, 1, 1, true, false));
    }
}

TEST_CASE("a frame style is written to the library as <slug>.frame.json, never over another file") {
    const fs::path dir = scratch("write") / "made";  // made by the write
    const auto s = frameStyleFromCells(box(), "", "Rope Edge", 1, 1, 1, 1, false, true);
    REQUIRE(s);
    const fs::path p = library::writeFrameStyle(dir, *s);
    CHECK(p == dir / "rope-edge.frame.json");
    CHECK(library::isFrameStyleName(p));
    CHECK(library::isFrameStyleName("A.FRAME.JSON"));
    CHECK_FALSE(library::isFrameStyleName(".frame.json"));
    CHECK_FALSE(library::isFrameStyleName("a.json"));
    const library::FrameStyleEntry e = library::readFrameStyleEntry(p);
    CHECK(e.path == p);
    CHECK(e.style.id == "rope-edge");
    CHECK(e.style.name == "Rope Edge");
    CHECK(e.style.source == s->source);
    CHECK(e.style.keepBg == s->keepBg);

    // the same name again takes the next free slot
    const std::string first = slurp(p);
    const fs::path q = library::writeFrameStyle(dir, *s);
    CHECK(q == dir / "rope-edge-2.frame.json");
    CHECK(library::readFrameStyleEntry(q).style.id == "rope-edge-2");
    CHECK(slurp(p) == first);
    CHECK(library::frameStylePathFor(dir, "Rope Edge") == dir / "rope-edge-3.frame.json");

    // a style with no name, or one holding a control character, isn't written
    FrameStyle unnamed = *s;
    unnamed.name = "  ";
    CHECK_THROWS_AS(library::writeFrameStyle(dir, unnamed), library::LibraryError);
    unnamed.name = "a\tb";
    CHECK_THROWS_AS(library::writeFrameStyle(dir, unnamed), library::LibraryError);
    // nor one the JSON form refuses
    FrameStyle bad = *s;
    bad.left = 4;
    CHECK_THROWS_AS(library::writeFrameStyle(dir, bad), library::LibraryError);
}

TEST_CASE("a scan finds frame styles beside the stamps, and reports the broken ones") {
    const fs::path dir = scratch("scan");
    const auto s = frameStyleFromCells(box(), "", "Box", 1, 1, 1, 1, false, false);
    REQUIRE(s);
    const fs::path a = library::writeFrameStyle(dir, *s);
    const fs::path b = library::writeFrameStyle(dir / "sub", *s);
    put(dir / "broken.frame.json", "{\"id\": \"x\", \"insets\": [1, 1, 1, 1], \"rows\": [\"ab\"]}");
    put(dir / "bad.frame.ans", "not read");

    const library::ScanResult r = library::scan(dir);
    REQUIRE(r.frames.size() == 2);
    CHECK(r.frames[0].path == a);
    CHECK(r.frames[1].path == b);
    CHECK(r.frames[0].style.source == s->source);
    CHECK(r.stamps.empty());
    CHECK(r.styles.empty());
    // the broken style and the stamp named as a frame style, nothing else:
    // a .frame.json is never taken for a stamp's lone sidecar
    REQUIRE(r.problems.size() == 2);
    CHECK(r.problems[0].path == dir / "bad.frame.ans");
    CHECK_THAT(r.problems[0].message, ContainsSubstring("frame style's file"));
    CHECK(r.problems[1].path == dir / "broken.frame.json");
    CHECK_THAT(r.problems[1].message, ContainsSubstring("frame style"));
}

TEST_CASE("a frame style whose insets would overflow when added is refused") {
    const fs::path dir = scratch("overflow");
    const fs::path p = dir / "x.frame.json";
    put(p, R"({"id":"x","insets":[2147483647,2147483647,0,0],"fill":true,"rows":["X"],"fg":["7"],"bg":["0"]})");
    CHECK_THROWS_AS(library::readFrameStyleEntry(p), library::LibraryError);
    put(dir / "y.frame.json",
        R"({"id":"y","insets":[0,0,2147483647,2147483647],"fill":true,"rows":["X"],"fg":["7"],"bg":["0"]})");
    const library::ScanResult r = library::scan(dir);
    CHECK(r.frames.empty());
    CHECK(r.problems.size() == 2);

    FrameStyle s;
    s.left = s.right = 2147483647;
    CHECK_FALSE(s.valid());
    CHECK_FALSE(drawFrame(s, 5, 3));
}

TEST_CASE("a frame style file too large to be one is a problem, not read whole") {
    const fs::path dir = scratch("large");
    // 2 MiB of name on an otherwise good style
    const std::string name(2 << 20, 'n');
    put(dir / "long.frame.json", R"({"id":"x","name":")" + name +
                                     R"(","insets":[1,1,1,1],"rows":["+-+","| |","+-+"],"fg":["777","777","777"],)"
                                     R"("bg":["000","000","000"]})");
    CHECK_THROWS_WITH(library::readFrameStyleEntry(dir / "long.frame.json"), ContainsSubstring("larger than"));

    // a source past the frame limit, in a small file
    std::string rows, colours;
    for (int y = 0; y < 200; ++y) {
        rows += std::string(y ? "," : "") + "\"" + std::string(200, 'X') + "\"";
        colours += std::string(y ? "," : "") + "\"" + std::string(200, '7') + "\"";
    }
    const std::string big = R"({"id":"big","insets":[1,1,1,1],"rows":[)" + rows + R"(],"fg":[)" + colours +
                            R"(],"bg":[)" + colours + "]}";
    REQUIRE(big.size() < (1 << 20));
    put(dir / "big.frame.json", big);
    CHECK_THROWS_WITH(library::readFrameStyleEntry(dir / "big.frame.json"), ContainsSubstring("larger than"));
    std::string why;
    CHECK_FALSE(frameStyleFromJson(big, &why));

    const library::ScanResult r = library::scan(dir);
    CHECK(r.frames.empty());
    CHECK(r.problems.size() == 2);
}

TEST_CASE("a frame style is never written in the bundled library, by path or by file identity (#504)") {
    const fs::path root = scratch("bundled");
    const fs::path bundled = root / "bundled";
    fs::create_directories(bundled / "trees");
    qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", QByteArray::fromStdString(bundled.string()));
    struct Unset {
        ~Unset() {
            qunsetenv("BLOCKWRIGHT_BUNDLED_LIBRARY");
            library::sameFolderHook() = {};
        }
    } unset;
    const auto s = frameStyleFromCells(box(), "", "Rope Edge", 1, 1, 1, 1, false, true);
    REQUIRE(s);
    // the dock's Save Frame Style into a user library that is the bundled one
    CHECK_THROWS_WITH(library::writeFrameStyle(bundled, *s), ContainsSubstring("is the bundled library"));
    CHECK_FALSE(fs::exists(bundled / "rope-edge.frame.json"));
    // a user folder that is a second path to a bundled folder (a bind mount)
    const fs::path user = root / "user";
    fs::create_directories(user);
    const auto canon = [](const fs::path& p) { return fs::weakly_canonical(p); };
    library::sameFolderHook() = [&](const fs::path& a, const fs::path& b) {
        return (canon(a) == canon(user) && canon(b) == canon(bundled / "trees")) ||
               (canon(b) == canon(user) && canon(a) == canon(bundled / "trees"));
    };
    CHECK_THROWS_WITH(library::writeFrameStyle(user, *s), ContainsSubstring("is inside the bundled library"));
    CHECK(fs::is_empty(user));
    library::sameFolderHook() = {};
    CHECK(library::writeFrameStyle(user, *s) == user / "rope-edge.frame.json");
}
