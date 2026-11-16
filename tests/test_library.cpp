// The Library's stamps (PLAN §12): the .ans + .json format, reading and
// checking it, scanning a library, categories, search and where the
// libraries are.

#include "fs_quirks.h"
#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/library.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>

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

constexpr std::string_view kBom = "\xEF\xBB\xBF";

Glyph stampCluster(std::vector<char32_t> cps) {
    const std::optional<Glyph> g = clusterPool().intern(cps, PoolCap::Capped);
    REQUIRE(g);
    return *g;
}

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

TEST_CASE("the widest row as the ascii script sends it, beside the sidecar's written size (#364)") {
    // 80 ★ in bright red and red by turns on black: written in 402 bytes,
    // the sidecar's ircBytes, and sent in 483
    Canvas c(80, 2);
    for (int x = 0; x < 80; ++x)
        c.at(x, 1) = Cell{U'★', Color::indexed(x % 2 ? Red : LightRed), Color::indexed(Black), 0, false};
    CHECK(widestRowBytes(c) == 402);
    const SentRow sent = widestSentRow(c, irc::kEfnetWeechatBudget);
    CHECK(sent.sent == 483);
    CHECK(sent.written == 402);
    // an empty canvas sends nothing
    CHECK(widestSentRow(Canvas(3, 1), irc::kEfnetWeechatBudget).sent == 0);
}

TEST_CASE("a text style's sample row sent over the budget is a problem though it is written within (#364)") {
    // a glow ring a cell wide in every rung, the letters a gradient across
    // the columns: the sample's rows change colour cell by cell, and the
    // ascii script sends each colour code in two digits
    const TextStyleEntry style = parseTextStyleEntry(
        R"({"kind": "text-style", "name": "Rings", "category": "test", "tags": [], "ice": "required",
            "effects": [{"kind": "glow", "enabled": true, "ramp": "sunset", "width": 16, "brightness": 16},
                        {"kind": "fill", "enabled": true, "mode": "columns", "colour": 7, "ramp": "sunset"}]})");
    const Canvas sample = textStyleSample(style.style, true);
    const std::size_t written = widestRowBytes(sample);
    const SentRow sent = widestSentRow(sample, written);
    INFO("written " << written << ", sent " << sent.sent << " (" << sent.written << " as written)");
    REQUIRE(sent.sent > written);
    REQUIRE(sent.written <= written);

    ScanResult scanned;
    scanned.styles.push_back(style);
    scanned.styles.back().path = "rings.style.json";
    // at a budget the widest row is written within, its sending is over
    const std::vector<Problem> over = budgetProblems(scanned, written);
    REQUIRE(over.size() == 1);
    CHECK(over[0].path == "rings.style.json");
    CHECK(over[0].message == "the sample's widest row with iCE colours on is " + std::to_string(sent.sent) +
                                 " IRC bytes as WeeChat's ascii script sends it (" + std::to_string(sent.written) +
                                 " as written), over the " + std::to_string(written) + "-byte budget");
    // with room for what is sent, nothing is over
    CHECK(budgetProblems(scanned, sent.sent).empty());
    // without "ice": "required" the sample is drawn with iCE colours off
    // too, but that one is within this budget: the problem is still the
    // iCE-on one alone (the iCE-off check is the next two tests', #384)
    scanned.styles.back().iceRequired = false;
    const std::vector<Problem> both = budgetProblems(scanned, written);
    REQUIRE(both.size() == 1);
    CHECK(both[0].message == over[0].message);
}

namespace {

// A plate in light blue behind the letters, one colour per letter: with iCE
// colours off the letters' half blocks can't take the bright plate as their
// background, so that sample's rows change colour more often and are the
// wider, written and sent (#384)
TextStyleEntry brightPlate(bool iceRequired) {
    TextStyleEntry style = parseTextStyleEntry(
        std::string(R"({"kind": "text-style", "name": "Bright plate", "category": "test", "tags": [],)") +
        (iceRequired ? R"( "ice": "required",)" : "") +
        R"( "effects": [{"kind": "plate", "enabled": true, "colour": 9, "padding": 1},
                        {"kind": "fill", "enabled": true, "mode": "rainbow", "colour": 7, "ramp": "fire"}]})");
    REQUIRE(style.iceRequired == iceRequired);
    style.path = "bright-plate.style.json";
    return style;
}

} // namespace

TEST_CASE("a text style's sample with iCE colours off sent over the budget is a problem (#384)") {
    ScanResult scanned;
    scanned.styles.push_back(brightPlate(false));
    const Canvas on = textStyleSample(scanned.styles[0].style, true);
    const Canvas off = textStyleSample(scanned.styles[0].style, false);
    // the budget the iCE-off sample is written within and sent over; the
    // iCE-on sample is written and sent within it
    const std::size_t budget = widestRowBytes(off);
    const SentRow sentOff = widestSentRow(off, budget);
    INFO("iCE on: written " << widestRowBytes(on) << ", sent " << widestSentRow(on, budget).sent
                            << "; iCE off: written " << budget << ", sent " << sentOff.sent);
    REQUIRE(widestRowBytes(on) <= budget);
    REQUIRE(widestSentRow(on, budget).sent <= budget);
    REQUIRE(sentOff.sent > budget);
    REQUIRE(sentOff.written <= budget);

    const std::vector<Problem> problems = budgetProblems(scanned, budget);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "bright-plate.style.json");
    CHECK(problems[0].message == "the sample's widest row with iCE colours off is " + std::to_string(sentOff.sent) +
                                     " IRC bytes as WeeChat's ascii script sends it (" +
                                     std::to_string(sentOff.written) + " as written), over the " +
                                     std::to_string(budget) + "-byte budget");
    // with room for what is sent, nothing is over
    CHECK(budgetProblems(scanned, sentOff.sent).empty());
}

TEST_CASE("a text style that needs iCE colours isn't checked with them off (#384)") {
    // the same style as the last test's, at the budget its iCE-off sample
    // is sent over: with "ice": "required" only the iCE-on sample is checked,
    // and that one is within
    ScanResult scanned;
    scanned.styles.push_back(brightPlate(true));
    const Canvas off = textStyleSample(scanned.styles[0].style, false);
    const std::size_t budget = widestRowBytes(off);
    REQUIRE(widestSentRow(off, budget).sent > budget);
    CHECK(budgetProblems(scanned, budget).empty());
    // and the iCE-on sample is still checked
    const std::size_t onSent = widestSentRow(textStyleSample(scanned.styles[0].style, true), budget).sent;
    REQUIRE(onSent > 0);
    const std::vector<Problem> problems = budgetProblems(scanned, onSent - 1);
    REQUIRE(problems.size() == 1);
    CHECK_THAT(problems[0].message, Catch::Matchers::StartsWith("the sample's widest row with iCE colours on is "));
}

TEST_CASE("a stamp's widest row is over the budget sent or written, once (#364)") {
    // 80 ★ in bright red and red by turns on black: written in 402 bytes and
    // sent in 483
    Stamp star;
    star.path = "stars.ans";
    star.art.canvas = Canvas(80, 1);
    for (int x = 0; x < 80; ++x)
        star.art.canvas.at(x, 0) = Cell{U'★', Color::indexed(x % 2 ? Red : LightRed), Color::indexed(Black), 0, false};
    star.meta.ircBytes = 402;
    ScanResult scanned;
    scanned.stamps.push_back(star);
    const std::vector<Problem> sent = budgetProblems(scanned, 407);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].message == "the widest row is 483 IRC bytes as WeeChat's ascii script sends it (402 as written), "
                             "over the 407-byte budget");
    CHECK(budgetProblems(scanned, 483).empty());
    // a sidecar that says the row is written over is believed
    scanned.stamps[0].meta.ircBytes = 500;
    const std::vector<Problem> written = budgetProblems(scanned, 483);
    REQUIRE(written.size() == 1);
    CHECK(written[0].message == "the widest row is 500 IRC bytes, over the 483-byte budget");
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

TEST_CASE("encodeStamp counts the glyphs even a UTF-8 stamp can't store as one cell or as cells") {
    const auto why = [](const CellBlock& b) {
        try {
            encodeStamp("Pine", "trees", {}, b, true);
        } catch (const LibraryError& e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    // 中 in one cell, with no continuation: a lone wide cell, which UTF-8
    // writes as ? so the row keeps its columns
    CellBlock b = tree();
    b.at(0, 0) = cell(U'\u4E2D', Green);
    CHECK(why(b) == "1 wide glyph has no cell for its right half, which a stamp can't store");
    b.at(3, 1) = cell(U'\u4E2D', Green);
    CHECK(why(b) == "2 wide glyphs have no cell for their right half, which a stamp can't store");
    b.at(3, 0) = cell(U'\u0085', Green);  // NEL, a C1 control
    CHECK(why(b) == "1 cell holds a control character; 2 wide glyphs have no cell for their right half, which a stamp "
                    "can't store");
}

TEST_CASE("stampLossMessage counts each loss as one cell or as cells") {
    AnsiOutput out;
    CHECK(stampLossMessage(out).empty());
    out.unwritable = {{0, 0, U'★'}};
    CHECK(stampLossMessage(out) == "1 cell holds a glyph with no CP437 code, which .ans can't store");
    out.unwritable = {{0, 0, U'◙', Loss::Control}, {1, 0, U'○', Loss::Control}};
    CHECK(stampLossMessage(out) ==
          "2 cells hold a glyph whose CP437 byte .ans reads as a control code (LF, TAB), which .ans can't store");
    // the glyphs come first; blink is told once every glyph can be written
    out.blinkDropped = 1;
    CHECK(stampLossMessage(out).find("blink") == std::string::npos);
    out.unwritable.clear();
    CHECK(stampLossMessage(out) == "1 blinking cell would lose its blink");
    out.blinkDropped = 3;
    CHECK(stampLossMessage(out) == "3 blinking cells would lose their blink");
    // a UTF-8 stamp's losses are those UTF-8 can't write
    out = {};
    out.encoding = TextEncoding::Utf8;
    out.unwritable = {{0, 0, U'\u0085', Loss::Control}, {2, 0, U'\u4E2D', Loss::LoneWide},
                      {3, 0, U'\u0301', Loss::LoneMark}, {4, 0, U'\u0301', Loss::LoneMark}};
    CHECK(stampLossMessage(out) == "1 cell holds a control character; 1 wide glyph has no cell for its right half; "
                                   "2 marks have no glyph of their own to sit on, which a stamp can't store");
}

// #336: a stamp CP437 can't hold is written to the user library as UTF-8,
// with a byte order mark and "encoding": "utf-8" in its sidecar.

TEST_CASE("a stamp CP437 holds is written as before: no byte order mark, three sidecar keys") {
    const StampFiles f = encodeStamp("Pine", "trees", {"tree"}, tree(), true);
    CHECK_FALSE(f.ans.starts_with(kBom));
    CHECK(f.ans.starts_with('\x1b'));  // so a mark never opens a CP437 stamp
    CHECK(f.json == "{\n  \"category\": \"trees\",\n  \"tags\": [\"tree\"],\n  \"ircBytes\": " +
                        std::to_string(parseSidecar(f.json).ircBytes) + "\n}\n");
    CHECK(parseSidecar(f.json).encoding == TextEncoding::Cp437);
    // the bytes are writeAnsi's CP437 output for the same cells
    AnsiDocument doc = parseAnsi(f.ans, ParseOptions{.encoding = TextEncoding::Cp437});
    CHECK(writeAnsi(doc).bytes == f.ans);
}

TEST_CASE("a stamp with a wide glyph and a cluster is written as UTF-8 with a byte order mark and reads back") {
    const fs::path dir = scratch("utf8");
    const Glyph acute = stampCluster({U'e', U'́'});
    CellBlock b(5, 2);
    b.at(0, 0) = cell(U'中', Yellow, Blue);  // 中, a pair
    b.at(1, 0) = cell(U'中', Yellow, Blue);
    b.at(1, 0).ch = Glyph::continuation();
    b.at(2, 0) = cell(U'e', LightGreen);
    b.at(2, 0).ch = acute;
    b.at(3, 0) = cell(U'▄', Green);  // CP437 cells beside them stay as they are
    b.at(1, 1) = cell(U'█', Black);
    const StampFiles f = encodeStamp("Han", "symbols", {"wide"}, b, true);
    REQUIRE(f.ans.starts_with(kBom));
    CHECK(f.ans.find("中") != std::string::npos);
    CHECK(f.ans.find("é") != std::string::npos);
    CHECK(f.json.find("\"encoding\": \"utf-8\"") != std::string::npos);
    writeStamp(dir / "han.ans", f);
    CHECK(bwtest::readFile(dir / "han.ans") == f.ans);

    const Stamp s = readStamp(dir / "han.ans");
    CHECK(s.meta.encoding == TextEncoding::Utf8);
    CHECK(s.art.encoding == TextEncoding::Utf8);
    REQUIRE(s.art.canvas.width() == 5);
    REQUIRE(s.art.canvas.height() == 2);
    const Canvas& cv = s.art.canvas;
    CHECK(cv.at(0, 0).ch == Glyph(U'中'));
    CHECK(cv.at(1, 0).ch.isContinuation());
    CHECK(cv.at(0, 0).fg == b.at(0, 0).fg);
    CHECK(cv.at(0, 0).bg == b.at(0, 0).bg);
    CHECK(cv.at(2, 0).ch == acute);
    CHECK(cv.at(2, 0).fg == b.at(2, 0).fg);
    CHECK(cv.at(3, 0).ch == Glyph(U'▄'));
    CHECK(cv.at(1, 1).ch == Glyph(U'█'));
    // saved again from what it read, it is the same stamp
    const StampFiles again = encodeStamp(s.name, s.meta.category, s.meta.tags, s.block(), s.art.ice);
    CHECK(again.ans.substr(0, again.ans.size() - 128) == f.ans.substr(0, f.ans.size() - 128));
    CHECK(again.json == f.json);
}

TEST_CASE("a cluster CP437 would compose to one byte is written as UTF-8, so it reads back as the cluster") {
    const fs::path dir = scratch("utf8-cluster");
    CellBlock b(2, 1);
    b.at(0, 0) = cell(U'e', White);
    b.at(0, 0).ch = stampCluster({U'e', U'́'});  // CP437 writes é, 0x82
    const StampFiles f = encodeStamp("Acute", "symbols", {}, b, true);
    CHECK(f.ans.starts_with(kBom));
    writeStamp(dir / "acute.ans", f);
    CHECK(readStamp(dir / "acute.ans").art.canvas.at(0, 0).ch == b.at(0, 0).ch);
}

TEST_CASE("a stamp with no byte order mark is read as CP437, whatever its high bytes look like") {
    const fs::path dir = scratch("cp437-high");
    // Σ╕¡ is E4 B8 AD in CP437, which is 中 in UTF-8: detectEncoding would
    // call the file UTF-8, and a stamp is never read by detection
    CellBlock b(3, 1);
    b.at(0, 0) = cell(U'Σ', White);
    b.at(1, 0) = cell(U'╕', White);
    b.at(2, 0) = cell(U'¡', White);
    const StampFiles f = encodeStamp("Sigma", "symbols", {}, b, true);
    REQUIRE_FALSE(f.ans.starts_with(kBom));
    REQUIRE(f.ans.find("\xE4\xB8\xAD") != std::string::npos);
    CHECK(detectEncoding(f.ans.substr(0, f.ans.find('\x1A'))) == TextEncoding::Utf8);
    writeStamp(dir / "sigma.ans", f);
    const Stamp s = readStamp(dir / "sigma.ans");
    CHECK(s.art.encoding == TextEncoding::Cp437);
    CHECK(s.art.canvas.at(0, 0).ch == Glyph(U'Σ'));
    CHECK(s.art.canvas.at(1, 0).ch == Glyph(U'╕'));
    CHECK(s.art.canvas.at(2, 0).ch == Glyph(U'¡'));
}

TEST_CASE("a stamp's byte order mark and its sidecar's encoding must agree for it to read as UTF-8") {
    const fs::path dir = scratch("utf8-agree");
    CellBlock b(3, 1);
    b.at(0, 0) = cell(U'★', Yellow);

    SECTION("a sidecar that says UTF-8 over an .ans with no byte order mark is refused") {
        const StampFiles f = encodeStamp("Star", "symbols", {}, b, true);
        REQUIRE(f.ans.starts_with(kBom));
        put(dir / "star.ans", f.ans.substr(kBom.size()));
        put(dir / "star.json", f.json);
        CHECK_THROWS_WITH(readStamp(dir / "star.ans"),
                          "sidecar says the .ans is UTF-8, but it doesn't start with a byte order mark");
    }
    SECTION("an .ans that starts with the mark under a sidecar that doesn't say UTF-8 is refused") {
        // A CP437 stamp starts with ESC, so a mark with no key is a UTF-8
        // stamp that lost its key, or a hand-made file whose first glyphs
        // are ∩╗┐. Both are refused rather than read as CP437.
        const std::string why = ".ans starts with a byte order mark, but its sidecar has no \"encoding\": \"utf-8\"";
        // a UTF-8 stamp whose widest row is plain ASCII: its CP437 read
        // costs the same IRC bytes, so only this check stops a misread
        CellBlock mixed(30, 2);
        mixed.at(0, 0) = cell(U'中', White);
        mixed.at(1, 0) = cell(U'中', White);
        mixed.at(1, 0).ch = Glyph::continuation();
        for (int x = 0; x < 30; ++x)
            mixed.at(x, 1) = cell(U'a' + char32_t(x % 26), White);
        const StampFiles f = encodeStamp("Mixed", "symbols", {}, mixed, true);
        REQUIRE(f.ans.starts_with(kBom));
        StampMeta meta = parseSidecar(f.json);
        meta.encoding = TextEncoding::Cp437;
        REQUIRE(widestRowBytes(parseAnsi(f.ans, ParseOptions{.encoding = TextEncoding::Cp437}).canvas) ==
                meta.ircBytes);
        put(dir / "mixed.ans", f.ans);
        put(dir / "mixed.json", sidecarJson(meta));
        CHECK_THROWS_WITH(readStamp(dir / "mixed.ans"), why);
        // a CP437 stamp with the mark put in front, its ircBytes as CP437 reads it
        CellBlock plain(5, 1);
        plain.at(0, 0) = cell(U'#', Yellow);
        const std::string ans = std::string(kBom) + encodeStamp("Hash", "symbols", {}, plain, true).ans;
        const Canvas old = parseAnsi(ans, ParseOptions{.encoding = TextEncoding::Cp437}).canvas;
        put(dir / "hash.ans", ans);
        put(dir / "hash.json", sidecarJson({"symbols", {}, widestRowBytes(old)}));
        CHECK_THROWS_WITH(readStamp(dir / "hash.ans"), why);
        // scanned, both are problems and neither is a stamp
        const ScanResult found = scan(dir);
        CHECK(found.stamps.empty());
        CHECK(found.problems.size() == 2);
    }
    SECTION("the sidecar names no encoding but UTF-8") {
        CHECK_THROWS_AS(parseSidecar(sidecar(R"("category": "a", "tags": [], "ircBytes": 1, "encoding": "cp437")")),
                        LibraryError);
        CHECK_THROWS_AS(parseSidecar(sidecar(R"("category": "a", "tags": [], "ircBytes": 1, "encoding": 8)")),
                        LibraryError);
        CHECK(parseSidecar(sidecar(R"("category": "a", "tags": [], "ircBytes": 1, "encoding": "utf-8")")).encoding ==
              TextEncoding::Utf8);
    }
}

TEST_CASE("an older build's CP437 read of a UTF-8 stamp can match its ircBytes; the fourth key is what refuses it") {
    // A build before #336 reads every stamp as CP437 and knows only three
    // sidecar keys. When the widest row is plain ASCII, its CP437 read of
    // the art costs the same IRC bytes as the UTF-8 one, so the ircBytes
    // check alone would let it place the stamp misread; the "encoding" key
    // is what makes it list the stamp as a problem.
    CellBlock b(30, 2);
    b.at(0, 0) = cell(U'中', White);
    b.at(1, 0) = cell(U'中', White);
    b.at(1, 0).ch = Glyph::continuation();
    for (int x = 0; x < 30; ++x)
        b.at(x, 1) = cell(U'a' + char32_t(x % 26), White);
    const StampFiles f = encodeStamp("Mixed", "symbols", {}, b, true);
    const StampMeta meta = parseSidecar(f.json);
    const Canvas misread = parseAnsi(f.ans, ParseOptions{.encoding = TextEncoding::Cp437}).canvas;
    CHECK(widestRowBytes(misread) == meta.ircBytes);
    CHECK(misread.at(0, 0).ch == Glyph(U'∩'));
    CHECK(f.json.find("\"encoding\"") != std::string::npos);
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
            SKIP("the symbolic-link tests don't run here: no right to make links (fs_quirks.h)");
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
    // What CP437 can't hold is saved as UTF-8 (#336): ★ has no CP437 code,
    // and ◙ and ○ have 0x0A and 0x09, which .ans reads as LF and TAB (#300)
    for (const char32_t ch : {U'★', U'◙', U'○'}) {
        CellBlock odd = b;
        odd.at(0, 0) = cell(ch, Yellow);
        const StampFiles f = encodeStamp("Pine", "trees", {}, odd, true);
        CHECK(f.ans.starts_with(kBom));
        CHECK(parseSidecar(f.json).encoding == TextEncoding::Utf8);
    }
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
            SKIP("the symbolic-link tests don't run here: no right to make links (fs_quirks.h)");
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
    CHECK(r.stamps[0].meta.ircBytes == 79);
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
        SKIP("the symbolic-link tests don't run here: no right to make links (fs_quirks.h)");
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

#if defined(__linux__) || defined(__APPLE__)
// The descriptors this process holds open, as /dev/fd lists them (Linux's
// links to /proc/self/fd; macOS's is the fdesc file system)
std::size_t openDescriptors() {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator("/dev/fd"))
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
#if defined(__linux__) || defined(__APPLE__)
        const std::size_t fds = openDescriptors();
#endif
        try {
            writeStamp(dir / "pine.ans", next);
            FAIL("no error");
        } catch (const LibraryError& e) {
            CHECK(std::string(e.what()).find("write failed: No space left on device") != std::string::npos);
        }
#if defined(__linux__) || defined(__APPLE__)
        CHECK(openDescriptors() == fds);
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

TEST_CASE("a stamp is read as CP437 even when its bytes would pass for UTF-8") {
    // α░▒ is E0 B0 B1 in CP437, which is also UTF-8 for one Telugu letter
    const fs::path dir = scratch("cp437-not-utf8");
    CellBlock b(3, 1);
    b.at(0, 0) = cell(U'α', White);
    b.at(1, 0) = cell(U'░', White);
    b.at(2, 0) = cell(U'▒', White);
    const StampFiles f = encodeStamp("Alpha", "letters", {}, b, true);
    REQUIRE(f.ans.find("\xE0\xB0\xB1") != std::string::npos);
    REQUIRE(detectEncoding(splitSauce(f.ans).content) == TextEncoding::Utf8);
    writeStamp(dir / "alpha.ans", f);
    const Stamp s = readStamp(dir / "alpha.ans");
    CHECK(s.art.encoding == TextEncoding::Cp437);
    const CellBlock back = s.block();
    REQUIRE(back.width == 3);
    CHECK(back.at(0, 0).ch == U'α');
    CHECK(back.at(1, 0).ch == U'░');
    CHECK(back.at(2, 0).ch == U'▒');
}

// Collections (#464): folders in the user's library.

namespace {

std::vector<std::string> names(const std::vector<Collection>& cs) {
    std::vector<std::string> out;
    for (const Collection& c : cs)
        out.push_back(c.name);
    return out;
}

// The bytes of every file under `dir`, by relative path.
std::map<std::string, std::string> snapshot(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir))
        out[e.path().lexically_relative(dir).generic_string()] =
            e.is_regular_file() ? bwtest::readFile(e.path()) : std::string("<dir>");
    return out;
}

std::string refusal(const std::function<void()>& f) {
    try {
        f();
    } catch (const LibraryError& e) {
        return e.what();
    }
    return "no error";
}

} // namespace

TEST_CASE("a collection's folder is its name's slug") {
    CHECK(collectionSlug("My Trees") == "my-trees");
    CHECK(collectionSlug("Café — Signs") == "caf-signs");
    CHECK(collectionSlug("木") == "u6728");
    CHECK(collectionSlug("") == "stamp");
}

TEST_CASE("an empty collection is kept and named by its marker") {
    const fs::path lib = scratch("coll-empty") / "user";
    const Collection c = createCollection(lib, "My Trees");
    CHECK(c.name == "My Trees");
    CHECK(c.dir == lib / "my-trees");
    CHECK(c.stamps == 0);
    CHECK(fs::is_directory(lib / "my-trees"));
    CHECK(bwtest::readFile(lib / "my-trees" / kCollectionMarker) == "My Trees\n");

    // A fresh look finds it, with its name, though nothing is in it.
    const std::vector<Collection> all = collections(lib);
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "My Trees");
    CHECK(all[0].stamps == 0);
    // The marker isn't a stamp or a problem to scan.
    const ScanResult r = scan(lib);
    CHECK(r.stamps.empty());
    CHECK(r.problems.empty());
    CHECK(collections(scratch("coll-none") / "missing").empty());
}

TEST_CASE("a collection with no marker goes by its stamps' category, else its folder") {
    const fs::path lib = scratch("coll-nomarker");
    writeStamp(lib / "signs" / "stop.ans", encodeStamp("Stop", "Road Signs", {}, tree(), true));
    fs::create_directories(lib / "bare");
    CHECK(names(collections(lib)) == std::vector<std::string>{"Road Signs", "bare"});
}

TEST_CASE("collection names that clash are refused") {
    const fs::path lib = scratch("coll-clash");
    createCollection(lib, "Trees");
    CHECK(refusal([&] { createCollection(lib, "Trees"); }) == "a collection named \"Trees\" already exists");
    // Another name with the same slug: the folder is taken.
    CHECK_THAT(refusal([&] { createCollection(lib, "trees!"); }),
               Catch::Matchers::ContainsSubstring("as the collection \"Trees\""));
    put(lib / "rocks", "a file");
    CHECK_THAT(refusal([&] { createCollection(lib, "Rocks"); }),
               Catch::Matchers::ContainsSubstring("already exists"));
    CHECK_THAT(refusal([&] { createCollection(lib, ""); }), Catch::Matchers::ContainsSubstring("empty"));
    CHECK(refusal([&] { createCollection(lib, "bad\xff"); }) == "the collection name isn't UTF-8");
    CHECK(names(collections(lib)) == std::vector<std::string>{"Trees"});
}

TEST_CASE("deleting a collection removes its folder and only that") {
    const fs::path lib = scratch("coll-delete");
    createCollection(lib, "Trees");
    createCollection(lib, "Rocks");
    addStamp(lib, "Trees", "Pine", {}, tree(), true);
    addStamp(lib, "Trees", "Oak", {}, tree(), true);
    writeStamp(lib / "top.ans", encodeStamp("Top", "loose", {}, tree(), true));

    const Removed r = deleteCollection(lib, "Trees", Removal::Delete);
    CHECK(r.stamps == 2);
    CHECK_FALSE(r.trashed);
    CHECK_FALSE(fs::exists(lib / "trees"));
    CHECK(names(collections(lib)) == std::vector<std::string>{"Rocks"});
    CHECK(fs::exists(lib / "top.ans"));
    CHECK(refusal([&] { deleteCollection(lib, "Trees", Removal::Delete); }) ==
          "no collection named \"Trees\" in " + lib.string());
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("deleting to the trash puts the folder in the XDG trash") {
    const fs::path root = scratch("coll-trash");
    EnvGuard home("HOME", (root / "home").string());
    EnvGuard data("XDG_DATA_HOME", (root / "home" / "data").string());
    fs::create_directories(root / "home" / "data");
    const fs::path lib = root / "home" / "lib";
    createCollection(lib, "Trees");
    addStamp(lib, "Trees", "Pine", {}, tree(), true);
    Removed r;
    try {
        r = deleteCollection(lib, "Trees");
    } catch (const TrashUnavailable&) {
        // No trash this machine's Qt can use: nothing may have gone.
        CHECK(fs::exists(lib / "trees" / "pine.ans"));
        return;
    }
    CHECK(r.stamps == 1);
    CHECK(r.trashed);
    CHECK_FALSE(fs::exists(lib / "trees"));
    CHECK(fs::exists(root / "home" / "data" / "Trash" / "files" / "trees" / "pine.ans"));
}
#endif

TEST_CASE("renaming a collection moves its folder and rewrites its stamps' category") {
    const fs::path lib = scratch("coll-rename");
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {"green"}, tree(), true);
    const std::string art = bwtest::readFile(pine);

    const Collection c = renameCollection(lib, "Trees", "Forest Things");
    CHECK(c.name == "Forest Things");
    CHECK(c.dir == lib / "forest-things");
    CHECK(c.stamps == 1);
    CHECK_FALSE(fs::exists(lib / "trees"));
    const Stamp s = readStamp(lib / "forest-things" / "pine.ans");
    CHECK(s.meta.category == "Forest Things");
    CHECK(s.meta.tags == std::vector<std::string>{"green"});
    CHECK(bwtest::readFile(s.path) == art);
    CHECK(bwtest::readFile(c.dir / kCollectionMarker) == "Forest Things\n");

    SECTION("a new name with the same slug keeps the folder") {
        const Collection d = renameCollection(lib, "Forest Things", "forest things");
        CHECK(d.dir == lib / "forest-things");
        CHECK(names(collections(lib)) == std::vector<std::string>{"forest things"});
        CHECK(readStamp(d.dir / "pine.ans").meta.category == "forest things");
    }
    SECTION("onto another collection's name or folder: refused, nothing moves") {
        createCollection(lib, "Rocks");
        const auto before = snapshot(lib);
        CHECK(refusal([&] { renameCollection(lib, "Forest Things", "Rocks"); }) ==
              "a collection named \"Rocks\" already exists");
        CHECK_THAT(refusal([&] { renameCollection(lib, "Forest Things", "rocks?"); }),
                   Catch::Matchers::ContainsSubstring("already exists"));
        CHECK(refusal([&] { renameCollection(lib, "Nope", "Other"); }) ==
              "no collection named \"Nope\" in " + lib.string());
        CHECK(snapshot(lib) == before);
    }
}

TEST_CASE("a stamp made from cells goes into its collection") {
    const fs::path lib = scratch("coll-add");
    createCollection(lib, "My Trees");
    const fs::path p = addStamp(lib, "My Trees", "Pine", {"tree"}, tree(), true);
    CHECK(p == lib / "my-trees" / "pine.ans");
    const Stamp s = readStamp(p);
    CHECK(s.name == "Pine");
    CHECK(s.meta.category == "My Trees");
    CHECK(collections(lib)[0].stamps == 1);

    CHECK_THAT(refusal([&] { addStamp(lib, "My Trees", "Pine", {}, tree(), true); }),
               Catch::Matchers::StartsWith("the collection \"My Trees\" already holds a stamp named \"Pine\""));
    CHECK(refusal([&] { addStamp(lib, "Nope", "Pine", {}, tree(), true); }) ==
          "no collection named \"Nope\" in " + lib.string());

    SECTION("replacing it, a failed write leaves the old files") {
        const std::string oldArt = bwtest::readFile(p), oldJson = bwtest::readFile(lib / "my-trees" / "pine.json");
        struct Reset {
            ~Reset() { testing::writeStampFault = nullptr; }
        } reset;
        testing::writeStampFault = [](std::string_view step) {
            if (step == "art")
                throw LibraryError("art: injected");
        };
        CellBlock wide(6, 1);
        wide.at(0, 0) = cell(U'#', Red);
        CHECK_THROWS_AS(addStamp(lib, "My Trees", "Pine", {}, wide, true, std::nullopt, true), LibraryError);
        CHECK(bwtest::readFile(p) == oldArt);
        CHECK(bwtest::readFile(lib / "my-trees" / "pine.json") == oldJson);
    }
}

TEST_CASE("stamp files and plain .ans files are added to a collection") {
    const fs::path root = scratch("coll-addfile");
    const fs::path lib = root / "user";
    createCollection(lib, "Mine");

    SECTION("a stamp is copied as it is, into the collection's category") {
        writeStamp(root / "bundled" / "pine.ans", encodeStamp("Pine", "trees", {"green"}, tree(), true));
        const Added a = addStampFile(lib, "Mine", root / "bundled" / "pine.ans");
        CHECK(a.name == "Pine");
        CHECK(a.path == lib / "mine" / "pine.ans");
        CHECK(bwtest::readFile(a.path) == bwtest::readFile(root / "bundled" / "pine.ans"));
        const Stamp s = readStamp(a.path);
        CHECK(s.meta.category == "Mine");
        CHECK(s.meta.tags == std::vector<std::string>{"green"});
        CHECK(readStamp(root / "bundled" / "pine.ans").meta.category == "trees");
        CHECK_THAT(refusal([&] { addStampFile(lib, "Mine", root / "bundled" / "pine.ans"); }),
                   Catch::Matchers::ContainsSubstring("already holds a stamp named \"Pine\""));
    }
    SECTION("an .ans with no sidecar and no SAUCE is named from its file") {
        put(root / "in" / "Big Moon.ans", "\x1b[31mAB\r\n\x1b[32mCD\r\n");
        const Added a = addStampFile(lib, "Mine", root / "in" / "Big Moon.ans");
        CHECK(a.name == "Big Moon");
        CHECK(a.path == lib / "mine" / "big-moon.ans");
        const Stamp s = readStamp(a.path);
        CHECK(s.name == "Big Moon");
        CHECK(s.meta.category == "Mine");
        CHECK(s.art.canvas.width() == 2);
        CHECK(s.art.canvas.height() == 2);
    }
    SECTION("an .ans with nothing drawn is refused") {
        put(root / "in" / "blank.ans", "   \r\n");
        CHECK(refusal([&] { addStampFile(lib, "Mine", root / "in" / "blank.ans"); }) == ".ans has nothing drawn in it");
    }
    SECTION("an .ans with a SAUCE title is named from it") {
        Sauce sauce;
        sauce.title = "Crescent";
        const std::string body = "\x1b[33mXY\r\n";
        put(root / "in" / "moon.ans", body + encodeSauce(sauce, std::uint32_t(body.size())));
        CHECK(addStampFile(lib, "Mine", root / "in" / "moon.ans").name == "Crescent");
    }
    SECTION("a long file name is cut to SAUCE's 35 bytes") {
        put(root / "in" / (std::string(40, 'a') + ".ans"), "hi\r\n");
        CHECK(addStampFile(lib, "Mine", root / "in" / (std::string(40, 'a') + ".ans")).name == std::string(35, 'a'));
    }
    SECTION("refusals") {
        put(root / "in" / "note.txt", "hi");
        CHECK(refusal([&] { addStampFile(lib, "Mine", root / "in" / "note.txt"); }) == "not an .ans file");
        CHECK_THAT(refusal([&] { addStampFile(lib, "Mine", root / "in" / "missing.ans"); }),
                   Catch::Matchers::StartsWith(".ans "));
    }
}

TEST_CASE("addStamps adds a batch with one scan of the library and refuses each file as addStampFile does", "[collections][addStamps]") {
    const fs::path root = scratch("coll-addstamps");
    const fs::path lib = root / "user";
    createCollection(lib, "Mine");
    createCollection(lib, "Other");
    addStamp(lib, "Other", "Elm", {}, tree(), true);
    const fs::path pine = addStamp(lib, "Mine", "Pine", {}, tree(), true);
    std::size_t scans = 0;
    testing::scanned = [&scans](const fs::path&) { ++scans; };
    struct Reset {
        ~Reset() { testing::scanned = nullptr; }
    } reset;
    // n plain files named f000.., then the ones refused
    const auto batch = [&](const fs::path& in, int n, char prefix = 'f') {
        std::vector<fs::path> files;
        for (int i = 0; i < n; ++i) {
            char name[16];
            std::snprintf(name, sizeof name, "%c%03d.ans", prefix, i);
            put(in / name, "\x1b[31mX" + std::to_string(i) + "\r\n");
            files.push_back(in / name);
        }
        return files;
    };

    SECTION("the scans don't grow with the files") {
        const std::vector<fs::path> few = batch(root / "few", 4), many = batch(root / "many", 64, 'g');
        scans = 0;
        const std::vector<AddOutcome> a = addStamps(lib, "Mine", few);
        const std::size_t forFew = scans;
        scans = 0;
        const std::vector<AddOutcome> b = addStamps(lib, "Mine", many);
        CHECK(scans == forFew);
        // the library's two collections once each, and Mine's titles once
        CHECK(scans <= 3);
        REQUIRE(a.size() == 4);
        REQUIRE(b.size() == 64);
        for (const AddOutcome& o : b) {
            REQUIRE(o.added);
            CHECK(o.error.empty());
            CHECK(o.added->path.parent_path() == lib / "mine");
        }
        CHECK(b[63].added->name == "g063");
        CHECK(scan(lib / "mine").stamps.size() == 1 + 4 + 64);
    }
    SECTION("each file gets addStampFile's result, a title added earlier in the call included") {
        Sauce sauce;
        sauce.title = "Twin";
        const std::string body = "\x1b[33mTW\r\n";
        put(root / "in" / "twin-a.ans", body + encodeSauce(sauce, std::uint32_t(body.size())));
        put(root / "in" / "twin-b.ans", body + encodeSauce(sauce, std::uint32_t(body.size())));
        put(root / "in" / "Pine.ans", "pp\r\n");
        put(root / "in" / "blank.ans", "   \r\n");
        put(root / "in" / "note.txt", "hi");
        put(root / "in" / "moon.ans", "mm\r\n");
        const std::vector<fs::path> files = {root / "in" / "twin-a.ans", root / "in" / "Pine.ans",
                                             root / "in" / "blank.ans",  root / "in" / "note.txt",
                                             root / "in" / "missing.ans", root / "in" / "twin-b.ans",
                                             root / "in" / "moon.ans"};
        const std::vector<AddOutcome> out = addStamps(lib, "Mine", files);
        REQUIRE(out.size() == files.size());
        for (std::size_t i = 0; i < out.size(); ++i)
            CHECK(out[i].file == files[i]);
        REQUIRE(out[0].added);
        CHECK(out[0].added->name == "Twin");
        CHECK(out[0].added->path == lib / "mine" / "twin.ans");
        CHECK_FALSE(out[1].added);
        CHECK(out[1].error == "the collection \"Mine\" already holds a stamp named \"Pine\" (" + pine.string() + ")");
        CHECK(out[2].error == ".ans has nothing drawn in it");
        CHECK(out[3].error == "not an .ans file");
        CHECK_THAT(out[4].error, Catch::Matchers::StartsWith(".ans "));
        CHECK_FALSE(out[5].added);
        CHECK(out[5].error == "the collection \"Mine\" already holds a stamp named \"Twin\" (" +
                                  (lib / "mine" / "twin.ans").string() + ")");
        REQUIRE(out[6].added);
        CHECK(out[6].added->name == "moon");
        // each refusal is the one addStampFile gives for the file
        for (const std::size_t i : {1, 2, 3, 4, 5})
            CHECK(refusal([&] { addStampFile(lib, "Mine", files[i]); }) == out[i].error);
        CHECK(scan(lib / "mine").stamps.size() == 3);
    }
    SECTION("next stops the call between files") {
        const std::vector<fs::path> files = batch(root / "in", 10);
        std::vector<std::size_t> asked;
        const std::vector<AddOutcome> out = addStamps(lib, "Mine", files, [&asked](std::size_t done) {
            asked.push_back(done);
            return done < 3;
        });
        CHECK(asked == std::vector<std::size_t>{0, 1, 2, 3});
        CHECK(out.size() == 3);
        CHECK(scan(lib / "mine").stamps.size() == 1 + 3);
    }
    SECTION("what every file would be refused for is thrown before any is read") {
        const std::vector<fs::path> files = batch(root / "in", 3);
        CHECK(refusal([&] { addStamps(lib, "Nope", files); }) ==
              "no collection named \"Nope\" in " + lib.string());
        CHECK(scan(lib / "mine").stamps.size() == 1);
    }
}

TEST_CASE("removing a stamp takes its art and sidecar, inside the library only") {
    const fs::path root = scratch("coll-remove");
    const fs::path lib = root / "user";
    createCollection(lib, "Mine");
    const fs::path p = addStamp(lib, "Mine", "Pine", {}, tree(), true);
    addStamp(lib, "Mine", "Oak", {}, tree(), true);
    writeStamp(root / "bundled" / "fir.ans", encodeStamp("Fir", "trees", {}, tree(), true));

    const Removed r = removeStamp(lib, p, Removal::Delete);
    CHECK(r.stamps == 1);
    CHECK_FALSE(fs::exists(p));
    CHECK_FALSE(fs::exists(lib / "mine" / "pine.json"));
    CHECK(fs::exists(lib / "mine" / "oak.ans"));

    CHECK_THAT(refusal([&] { removeStamp(lib, root / "bundled" / "fir.ans", Removal::Delete); }),
               Catch::Matchers::ContainsSubstring("isn't in the library"));
    CHECK_THAT(refusal([&] { removeStamp(lib, lib / "mine" / ".." / ".." / "bundled" / "fir.ans", Removal::Delete); }),
               Catch::Matchers::ContainsSubstring("isn't in the library"));
    CHECK(fs::exists(root / "bundled" / "fir.ans"));
    CHECK_THAT(refusal([&] { removeStamp(lib, p, Removal::Delete); }), Catch::Matchers::EndsWith("doesn't exist"));
    CHECK_THAT(refusal([&] { removeStamp(lib, lib / "mine" / "oak.json", Removal::Delete); }),
               Catch::Matchers::EndsWith("isn't a stamp's .ans"));
}

TEST_CASE("editing a stamp keeps its file where it can") {
    const fs::path lib = scratch("coll-edit");
    createCollection(lib, "Trees");
    createCollection(lib, "Rocks");
    const fs::path p = addStamp(lib, "Trees", "Pine", {"a"}, tree(), true);
    const std::string body = std::string(splitSauce(bwtest::readFile(p)).content);

    SECTION("a new name retitles it in place") {
        CHECK(editStamp(lib, p, {.name = "Tall Pine"}) == p);
        const Stamp s = readStamp(p);
        CHECK(s.name == "Tall Pine");
        CHECK(std::string(splitSauce(bwtest::readFile(p)).content) == body);
        CHECK(s.meta.category == "Trees");
    }
    SECTION("new tags stay in place; an empty list clears them") {
        CHECK(editStamp(lib, p, {.tags = std::vector<std::string>{"x", "y"}}) == p);
        CHECK(readStamp(p).meta.tags == std::vector<std::string>{"x", "y"});
        editStamp(lib, p, {.tags = std::vector<std::string>{}});
        CHECK(readStamp(p).meta.tags.empty());
    }
    SECTION("another collection's name moves it there") {
        const fs::path q = editStamp(lib, p, {.category = "Rocks"});
        CHECK(q == lib / "rocks" / "pine.ans");
        CHECK_FALSE(fs::exists(p));
        CHECK_FALSE(fs::exists(lib / "trees" / "pine.json"));
        CHECK(readStamp(q).meta.category == "Rocks");
        // A category no collection has: out to the top of the library.
        const fs::path t = editStamp(lib, q, {.category = "loose"});
        CHECK(t == lib / "pine.ans");
        CHECK(readStamp(t).meta.category == "loose");
        // And a top-level stamp given a collection's name goes into it.
        CHECK(editStamp(lib, t, {.category = "Trees"}) == p);
    }
    SECTION("a clash is refused and nothing changes") {
        addStamp(lib, "Rocks", "Pine", {}, tree(), true);
        addStamp(lib, "Trees", "Oak", {}, tree(), true);
        const auto before = snapshot(lib);
        CHECK_THAT(refusal([&] { editStamp(lib, p, {.category = "Rocks"}); }),
                   Catch::Matchers::ContainsSubstring("already holds a stamp named \"Pine\""));
        CHECK_THAT(refusal([&] { editStamp(lib, p, {.name = "Oak"}); }),
                   Catch::Matchers::ContainsSubstring("already holds a stamp named \"Oak\""));
        CHECK_THAT(refusal([&] { editStamp(lib, p, {.name = std::string(36, 'x')}); }),
                   Catch::Matchers::ContainsSubstring("35"));
        CHECK_THAT(refusal([&] { editStamp(lib, p, {.tags = std::vector<std::string>{"a,b"}}); }),
                   Catch::Matchers::ContainsSubstring("comma"));
        CHECK(snapshot(lib) == before);
    }
    SECTION("a failed write leaves the old files") {
        const auto before = snapshot(lib);
        struct Reset {
            ~Reset() { testing::writeStampFault = nullptr; }
        } reset;
        testing::writeStampFault = [](std::string_view step) {
            if (step == "art")
                throw LibraryError("art: injected");
        };
        CHECK_THROWS_AS(editStamp(lib, p, {.name = "Tall Pine"}), LibraryError);
        CHECK_THROWS_AS(editStamp(lib, p, {.category = "Rocks"}), LibraryError);
        CHECK(snapshot(lib) == before);
    }
}

TEST_CASE("collections never touch a read-only bundled library") {
#ifdef _WIN32
    SKIP("no POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root writes every folder");
    const fs::path root = scratch("coll-bundled");
    const fs::path bundled = root / "bundled", lib = root / "user";
    writeStamp(bundled / "trees" / "pine.ans", encodeStamp("Pine", "trees", {}, tree(), true));
    fs::permissions(bundled / "trees", fs::perms::owner_read | fs::perms::owner_exec);
    fs::permissions(bundled, fs::perms::owner_read | fs::perms::owner_exec);
    struct Unlock {
        fs::path a, b;
        ~Unlock() {
            fs::permissions(a, fs::perms::owner_all);
            fs::permissions(b, fs::perms::owner_all);
        }
    } unlock{bundled, bundled / "trees"};
    const auto before = snapshot(bundled);

    // A collection named like a bundled category is the user's own.
    createCollection(lib, "trees");
    const Added a = addStampFile(lib, "trees", bundled / "trees" / "pine.ans");
    CHECK(a.path == lib / "trees" / "pine.ans");
    editStamp(lib, a.path, {.name = "My Pine"});
    renameCollection(lib, "trees", "evergreens");
    deleteCollection(lib, "evergreens", Removal::Delete);
    CHECK_THROWS_AS(removeStamp(lib, bundled / "trees" / "pine.ans", Removal::Delete), LibraryError);
    CHECK_THROWS_AS(editStamp(lib, bundled / "trees" / "pine.ans", {.name = "X"}), LibraryError);
    CHECK(snapshot(bundled) == before);
#endif
}

// Fix round 1 of #500's review.

namespace {

struct TrashHook {
    explicit TrashHook(std::function<bool(const fs::path&)> f) { testing::moveToTrash = std::move(f); }
    ~TrashHook() { testing::moveToTrash = nullptr; }
};

} // namespace

TEST_CASE("a trash that refuses removes nothing and never deletes instead") {
    const fs::path lib = scratch("coll-notrash") / "user";
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {}, tree(), true);
    addStamp(lib, "Trees", "Oak", {}, tree(), true);
    const auto before = snapshot(lib);
    std::vector<fs::path> asked;
    TrashHook hook([&](const fs::path& p) {
        asked.push_back(p);
        return false;
    });

    CHECK_THROWS_AS(deleteCollection(lib, "Trees"), TrashUnavailable);
    CHECK_THAT(refusal([&] { deleteCollection(lib, "Trees"); }), Catch::Matchers::ContainsSubstring("nothing was removed"));
    CHECK_THROWS_AS(removeStamp(lib, pine), TrashUnavailable);
    CHECK(snapshot(lib) == before);
    // The whole folder in one call, not stamp by stamp.
    REQUIRE_FALSE(asked.empty());
    CHECK(asked.front() == lib / "trees");

    // Deleting outright is the caller's own choice.
    CHECK(removeStamp(lib, pine, Removal::Delete).stamps == 1);
    CHECK_FALSE(fs::exists(pine));
    const Removed r = deleteCollection(lib, "Trees", Removal::Delete);
    CHECK_FALSE(r.trashed);
    CHECK_FALSE(fs::exists(lib / "trees"));
}

// Fix round 2: a stamp's two files go one at a time, so a trash that takes
// one and refuses the other must still leave the stamp whole.
TEST_CASE("a trash that refuses half a stamp leaves the stamp whole") {
    const fs::path root = scratch("coll-halftrash");
    const fs::path lib = root / "user";
    const fs::path bin = root / "trash";
    fs::create_directories(bin);
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {}, tree(), true);
    const fs::path side = lib / "trees" / "pine.json";
    REQUIRE(fs::exists(side));
    const auto before = snapshot(lib);
    // A trash that takes every file but those with the extension `refuse`.
    std::string refuse;
    TrashHook hook([&](const fs::path& p) {
        if (p.extension() == refuse)
            return false;
        fs::rename(p, bin / p.filename());
        return true;
    });

    SECTION("the sidecar refused") {
        refuse = ".json";
        const std::string why = refusal([&] { removeStamp(lib, pine); });
        CHECK_THROWS_AS(removeStamp(lib, pine), TrashUnavailable);
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring("nothing was removed"));
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring(side.string()));
        CHECK(snapshot(lib) == before);
        CHECK(fs::is_empty(bin));
    }
    SECTION("the art refused, after its sidecar went") {
        refuse = ".ans";
        const std::string why = refusal([&] { removeStamp(lib, pine); });
        CHECK_THROWS_AS(removeStamp(lib, pine), TrashUnavailable);
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring("nothing was removed"));
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring("put back"));
        CHECK(snapshot(lib) == before);
        CHECK(readStamp(pine).name == "Pine");
    }
    // Then a trash that takes both removes the stamp.
    refuse = ".none";
    const Removed r = removeStamp(lib, pine);
    CHECK(r.trashed);
    CHECK_FALSE(fs::exists(pine));
    CHECK_FALSE(fs::exists(side));
    CHECK(fs::exists(bin / "pine.ans"));
    CHECK(fs::exists(bin / "pine.json"));
}

#ifndef _WIN32
TEST_CASE("a sidecar that can't be put back leaves art remove still takes") {
    if (::geteuid() == 0)
        SKIP("root writes every folder");
    const fs::path root = scratch("coll-noputback");
    const fs::path lib = root / "user";
    const fs::path bin = root / "trash";
    fs::create_directories(bin);
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {}, tree(), true);
    const fs::path dir = lib / "trees";
    struct Unlock {
        fs::path p;
        ~Unlock() { fs::permissions(p, fs::perms::owner_all); }
    } unlock{dir};
    // The sidecar goes, then the folder turns read-only: the art can't
    // follow it and the sidecar can't come back.
    TrashHook hook([&](const fs::path& p) {
        fs::rename(p, bin / p.filename());
        fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
        return true;
    });
    const std::string why = refusal([&] { removeStamp(lib, pine); });
    CHECK_THAT(why, !Catch::Matchers::ContainsSubstring("nothing was removed"));
    CHECK_THAT(why, Catch::Matchers::ContainsSubstring("couldn't be put back"));
    CHECK(fs::exists(pine));
    CHECK_FALSE(fs::exists(dir / "pine.json"));
    CHECK(fs::exists(bin / "pine.json"));

    // What is left is art alone, and remove still takes it.
    fs::permissions(dir, fs::perms::owner_all);
    testing::moveToTrash = nullptr;
    CHECK(removeStamp(lib, pine, Removal::Delete).stamps == 1);
    CHECK_FALSE(fs::exists(pine));
}
#endif

TEST_CASE("a delete a permission would stop halfway removes nothing") {
#ifdef _WIN32
    SKIP("no POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root writes every folder");
    const fs::path lib = scratch("coll-half") / "user";
    createCollection(lib, "Half");
    for (const char* n : {"H1", "H2", "H3"})
        addStamp(lib, "Half", n, {}, tree(), true);
    writeStamp(lib / "half" / "sub" / "s.ans", encodeStamp("S", "Half", {}, tree(), true));
    fs::permissions(lib / "half" / "sub", fs::perms::owner_read | fs::perms::owner_exec);
    struct Unlock {
        fs::path p;
        ~Unlock() { fs::permissions(p, fs::perms::owner_all); }
    } unlock{lib / "half" / "sub"};
    const auto before = snapshot(lib);
    CHECK_THAT(refusal([&] { deleteCollection(lib, "Half", Removal::Delete); }),
               Catch::Matchers::ContainsSubstring("nothing was removed"));
    CHECK(snapshot(lib) == before);

    // The library itself read-only: the folder can't be taken out of it, so
    // nothing inside it is deleted either.
    fs::permissions(lib / "half" / "sub", fs::perms::owner_all);
    fs::permissions(lib, fs::perms::owner_read | fs::perms::owner_exec);
    struct Unlock2 {
        fs::path p;
        ~Unlock2() { fs::permissions(p, fs::perms::owner_all); }
    } unlock2{lib};
    const auto before2 = snapshot(lib);
    CHECK_THROWS_AS(deleteCollection(lib, "Half", Removal::Delete), LibraryError);
    CHECK(snapshot(lib) == before2);
#endif
}

TEST_CASE("nothing changes the bundled library, by any spelling of it") {
    const fs::path root = scratch("coll-bundled-dir");
    const fs::path bundled = root / "bundled";
    writeStamp(bundled / "trees" / "pine.ans", encodeStamp("Pine", "trees", {}, tree(), true));
    writeStamp(bundled / "plants" / "fern.ans", encodeStamp("Fern", "plants", {}, tree(), true));
    put(root / "in" / "moon.ans", "\x1b[31mAB\r\n");
    EnvGuard env("BLOCKWRIGHT_BUNDLED_LIBRARY", bundled.string());
    CHECK(bundledLibraryDir() == bundled);
    const auto before = snapshot(bundled);

    std::vector<fs::path> spellings = {bundled, bundled / "trees", root / "x" / ".." / "bundled", root};
#ifndef _WIN32
    fs::create_symlink(bundled, root / "link");
    spellings.push_back(root / "link");
#endif
    for (const fs::path& lib : spellings) {
        INFO(lib.string());
        const auto bundledRefusal = Catch::Matchers::ContainsSubstring("the bundled library");
        CHECK_THAT(refusal([&] { createCollection(lib, "New"); }), bundledRefusal);
        CHECK_THAT(refusal([&] { deleteCollection(lib, "trees", Removal::Delete); }), bundledRefusal);
        CHECK_THAT(refusal([&] { deleteCollection(lib, "trees"); }), bundledRefusal);
        CHECK_THAT(refusal([&] { renameCollection(lib, "plants", "Flora"); }), bundledRefusal);
        CHECK_THAT(refusal([&] { addStamp(lib, "trees", "Oak", {}, tree(), true); }), bundledRefusal);
        CHECK_THAT(refusal([&] { addStampFile(lib, "trees", root / "in" / "moon.ans"); }), bundledRefusal);
        CHECK_THAT(refusal([&] { removeStamp(lib, lib / "trees" / "pine.ans", Removal::Delete); }), bundledRefusal);
        CHECK_THAT(refusal([&] { editStamp(lib, lib / "trees" / "pine.ans", {.name = "X"}); }), bundledRefusal);
    }
    CHECK(snapshot(bundled) == before);

    // A library beside it is the user's own, and works.
    createCollection(root / "user", "trees");
    CHECK(addStampFile(root / "user", "trees", bundled / "trees" / "pine.ans").name == "Pine");
    CHECK(snapshot(bundled) == before);
}

TEST_CASE("a title is checked across the collection, a stamp renamed in place included") {
    const fs::path root = scratch("coll-title");
    const fs::path lib = root / "user";
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {}, tree(), true);
    CHECK(editStamp(lib, pine, {.name = "Birch"}) == pine);  // kept as pine.ans
    const fs::path oak = addStamp(lib, "Trees", "Oak", {}, tree(), true);
    writeStamp(lib / "trees" / "sub" / "elm.ans", encodeStamp("Elm", "Trees", {}, tree(), true));
    const auto before = snapshot(lib);

    CHECK_THAT(refusal([&] { addStamp(lib, "Trees", "Birch", {}, tree(), true); }),
               Catch::Matchers::ContainsSubstring("already holds a stamp named \"Birch\" (" + pine.string() + ")"));
    put(root / "in" / "Birch.ans", "\x1b[31mAB\r\n");
    CHECK_THAT(refusal([&] { addStampFile(lib, "Trees", root / "in" / "Birch.ans"); }),
               Catch::Matchers::ContainsSubstring("already holds a stamp named \"Birch\""));
    CHECK_THAT(refusal([&] { editStamp(lib, oak, {.name = "Birch"}); }),
               Catch::Matchers::ContainsSubstring("already holds a stamp named \"Birch\""));
    // In a subfolder of the collection too.
    CHECK_THAT(refusal([&] { editStamp(lib, oak, {.name = "Elm"}); }),
               Catch::Matchers::ContainsSubstring("already holds a stamp named \"Elm\""));
    CHECK(snapshot(lib) == before);

    // A tags-only edit makes no new clash.
    CHECK(editStamp(lib, pine, {.tags = std::vector<std::string>{"white"}}) == pine);
    // Replacing writes over the stamp that has the title, wherever it is.
    CHECK(addStamp(lib, "Trees", "Birch", {"new"}, tree(), true, std::nullopt, true) == pine);
    CHECK(readStamp(pine).meta.tags == std::vector<std::string>{"new"});
    CHECK_FALSE(fs::exists(lib / "trees" / "birch.ans"));
}

TEST_CASE("a collection name is at most kMaxCollectionName bytes") {
    const fs::path lib = scratch("coll-long");
    const std::string longest(kMaxCollectionName, 'x'), over(kMaxCollectionName + 1, 'x');
    CHECK(refusal([&] { createCollection(lib, over); }) == "the collection name is longer than 128 bytes");
    const Collection c = createCollection(lib, longest);
    CHECK(findCollection(lib, longest).has_value());
    CHECK(collections(lib).at(0).name == longest);
    CHECK(refusal([&] { renameCollection(lib, longest, over); }) == "the collection name is longer than 128 bytes");
    CHECK(collections(lib).at(0).name == longest);
    CHECK(fs::exists(c.dir));
}

TEST_CASE("a name two folders go by is refused, naming both") {
    const fs::path lib = scratch("coll-dupe");
    createCollection(lib, "Trees");
    addStamp(lib, "Trees", "Pine", {}, tree(), true);
    // A folder made by hand whose stamps share the name.
    writeStamp(lib / "zz" / "oak.ans", encodeStamp("Oak", "Trees", {}, tree(), true));
    CHECK(names(collections(lib)) == std::vector<std::string>{"Trees", "Trees"});
    const auto before = snapshot(lib);
    const std::string why = refusal([&] { findCollection(lib, "Trees"); });
    CHECK_THAT(why, Catch::Matchers::StartsWith("more than one folder goes by the collection name \"Trees\""));
    CHECK_THAT(why, Catch::Matchers::ContainsSubstring((lib / "trees").string()));
    CHECK_THAT(why, Catch::Matchers::ContainsSubstring((lib / "zz").string()));
    CHECK_THAT(refusal([&] { deleteCollection(lib, "Trees", Removal::Delete); }),
               Catch::Matchers::StartsWith("more than one folder"));
    CHECK_THAT(refusal([&] { renameCollection(lib, "Trees", "Woods"); }), Catch::Matchers::StartsWith("more than one folder"));
    CHECK_THAT(refusal([&] { addStamp(lib, "Trees", "Fir", {}, tree(), true); }),
               Catch::Matchers::StartsWith("more than one folder"));
    CHECK(snapshot(lib) == before);
    // Giving one a name of its own settles it.
    put(lib / "zz" / kCollectionMarker, "Oaks\n");
    CHECK(findCollection(lib, "Trees")->dir == lib / "trees");
}

#ifndef _WIN32
TEST_CASE("a folder's own name never gives a collection a tab or newline") {
    const fs::path lib = scratch("coll-ctrl");
    fs::create_directories(lib / "a\tb");
    fs::create_directories(lib / "c\nd");
    std::vector<std::string> want = {"a\xEF\xBF\xBD" "b", "c\xEF\xBF\xBD" "d"};
#ifndef __APPLE__
    // APFS refuses a name that isn't UTF-8.
    fs::create_directories(lib / "e\xff");
    want.push_back("e\xEF\xBF\xBD");
#endif
    CHECK(names(collections(lib)) == want);
    CHECK(findCollection(lib, "a\xEF\xBF\xBD" "b")->dir == lib / "a\tb");
}
#endif

TEST_CASE("a collection named as a Windows device gets a folder Windows allows") {
    CHECK(collectionSlug("CON") == "con-");
    CHECK(collectionSlug("aux") == "aux-");
    CHECK(collectionSlug("Com1") == "com1-");
    CHECK(collectionSlug("LPT9") == "lpt9-");
    CHECK(collectionSlug("Con Air") == "con-air");
    CHECK(collectionSlug("com10") == "com10");
    const fs::path lib = scratch("coll-reserved");
    CHECK(createCollection(lib, "Nul").dir == lib / "nul-");
    CHECK(findCollection(lib, "Nul").has_value());
}

// Fix round 3 of #500's review.

TEST_CASE("remove's check refuses a stamp its folder won't let go, as remove does") {
#ifdef _WIN32
    SKIP("no POSIX file modes on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root writes every folder");
    const fs::path lib = scratch("coll-rmcheck") / "user";
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {}, tree(), true);
    const fs::path dir = lib / "trees";
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
    struct Unlock {
        fs::path p;
        ~Unlock() { fs::permissions(p, fs::perms::owner_all); }
    } unlock{dir};
    const auto before = snapshot(lib);
    CHECK_THAT(refusal([&] { requireRemovable(lib, pine); }),
               Catch::Matchers::ContainsSubstring(dir.string() + " can't be emptied"));
    CHECK_THAT(refusal([&] { removeStamp(lib, pine, Removal::Delete); }),
               Catch::Matchers::ContainsSubstring("nothing was removed"));
    CHECK(snapshot(lib) == before);
#endif
}

#ifndef _WIN32
TEST_CASE("a sidecar that can't be read is refused before the trash takes anything") {
    const fs::path root = scratch("coll-unreadable");
    const fs::path lib = root / "user";
    const fs::path bin = root / "trash";
    fs::create_directories(bin);
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {}, tree(), true);
    const fs::path side = lib / "trees" / "pine.json";
    // A trash that refuses the art and takes everything else.
    TrashHook hook([&](const fs::path& p) {
        if (p.extension() == ".ans")
            return false;
        fs::rename(p, bin / p.filename());
        return true;
    });

    SECTION("mode 000") {
        if (::geteuid() == 0)
            SKIP("root reads every file");
        fs::permissions(side, fs::perms::none);
        struct Unlock {
            fs::path p;
            ~Unlock() {
                std::error_code ec;
                fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, ec);
            }
        } unlock{side};
        const std::string why = refusal([&] { removeStamp(lib, pine); });
        CHECK_THROWS_AS(removeStamp(lib, pine), TrashUnavailable);
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring(side.string() + " can't be read"));
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring("nothing was removed"));
        CHECK(fs::exists(side));
        CHECK(fs::exists(pine));
        CHECK(fs::is_empty(bin));
        // Delete takes it.
        CHECK(removeStamp(lib, pine, Removal::Delete).stamps == 1);
        CHECK_FALSE(fs::exists(pine));
        CHECK_FALSE(fs::exists(side));
    }
    SECTION("not a regular file") {
        fs::remove(side);
        REQUIRE(::mkfifo(side.c_str(), 0600) == 0);
        const std::string why = refusal([&] { removeStamp(lib, pine); });
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring("is not a regular file"));
        CHECK_THAT(why, Catch::Matchers::ContainsSubstring("nothing was removed"));
        CHECK(fs::is_fifo(fs::symlink_status(side)));
        CHECK(fs::exists(pine));
        CHECK(fs::is_empty(bin));
    }
}

TEST_CASE("a sidecar put back keeps its permission bits and modification time") {
    const fs::path root = scratch("coll-putback-mode");
    const fs::path lib = root / "user";
    const fs::path bin = root / "trash";
    fs::create_directories(bin);
    createCollection(lib, "Trees");
    const fs::path pine = addStamp(lib, "Trees", "Pine", {}, tree(), true);
    const fs::path side = lib / "trees" / "pine.json";
    // Read-only for its owner alone: no umask makes a new file that.
    const fs::perms mode = fs::perms::owner_read;
    fs::permissions(side, mode);
    const fs::file_time_type mtime = fs::last_write_time(side) - std::chrono::hours(48);
    fs::last_write_time(side, mtime);
    TrashHook hook([&](const fs::path& p) {
        if (p.extension() == ".ans")
            return false;
        fs::rename(p, bin / p.filename());
        return true;
    });
    const std::string why = refusal([&] { removeStamp(lib, pine); });
    CHECK_THAT(why, Catch::Matchers::ContainsSubstring("was put back; the trash keeps a copy of it"));
    REQUIRE(fs::exists(side));
    CHECK((fs::status(side).permissions() & fs::perms::all) == mode);
    // Milliseconds as a plain integer: libc++'s file_time_type counts in
    // __int128, which Catch2 can't print.
    const long long off = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(fs::last_write_time(side) - mtime).count());
    CHECK(off > -2000);
    CHECK(off < 2000);
    CHECK(readStamp(pine).name == "Pine");
}
#endif
