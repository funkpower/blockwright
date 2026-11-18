// IRC in 16 colours (#496): the fold to mIRC 0-15, the swatches, what is
// sent, the irc-fold lint note and the project's "ircColours".

#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/lint_rules.h"
#include "blockwright/mirc.h"
#include "blockwright/project.h"
#include "blockwright/target.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cctype>
#include <climits>

using namespace bw;
namespace fs = std::filesystem;

namespace {

Color mircRgb(int i) {
    const std::uint32_t v = irc::kMircRgb[std::size_t(i)];
    return Color::fromRgb({std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)});
}

std::uint32_t packed(const Color& c) { return std::uint32_t(c.rgb.r) << 16 | std::uint32_t(c.rgb.g) << 8 | c.rgb.b; }

// The nearest of mIRC 0-15 the slow way: squared distance, ties to the lower.
int nearest16(std::uint32_t rgb) {
    int best = -1, bestD = INT_MAX;
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t c = irc::kMircRgb[std::size_t(i)];
        const int dr = int(c >> 16 & 255) - int(rgb >> 16 & 255), dg = int(c >> 8 & 255) - int(rgb >> 8 & 255),
                  db = int(c & 255) - int(rgb & 255);
        const int d = dr * dr + dg * dg + db * db;
        if (d < bestD)
            best = i, bestD = d;
    }
    return best;
}

// Every ^C colour number in an IRC line.
std::vector<int> colourNumbers(const std::string& line) {
    std::vector<int> out;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '\x03')
            continue;
        const auto number = [&](std::size_t& at) {
            int n = -1;
            for (int k = 0; k < 2 && at < line.size() && std::isdigit(static_cast<unsigned char>(line[at])); ++k, ++at)
                n = (n < 0 ? 0 : n * 10) + (line[at] - '0');
            return n;
        };
        std::size_t at = i + 1;
        if (const int f = number(at); f >= 0) {
            out.push_back(f);
            if (at + 1 < line.size() && line[at] == ',' && std::isdigit(static_cast<unsigned char>(line[at + 1]))) {
                ++at;
                out.push_back(number(at));
            }
        }
    }
    return out;
}

// A row of RGB cells: mIRC 77 on 40, the six that share an RGB with one of
// 0-15, and an indexed cell.
Canvas extended() {
    Canvas c(9, 1);
    const int fgs[] = {77, 52, 54, 58, 62, 88, 98, 33};
    for (int x = 0; x < 8; ++x) {
        c.at(x, 0).ch = U'▓';
        c.at(x, 0).fg = mircRgb(fgs[x]);
        c.at(x, 0).bg = mircRgb(40);
    }
    c.at(8, 0).ch = U'A';
    c.at(8, 0).fg = Color::indexed(14);
    c.at(8, 0).bg = Color::indexed(1);
    return c;
}

std::vector<lint::Finding> folds(const std::vector<lint::Finding>& all) {
    std::vector<lint::Finding> out;
    std::copy_if(all.begin(), all.end(), std::back_inserter(out),
                 [](const lint::Finding& f) { return f.kind == lint::Finding::Kind::IrcFold; });
    return out;
}

QJsonObject object(const std::string& text) { return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object(); }
std::string json(const QJsonObject& o) { return QJsonDocument(o).toJson().toStdString(); }

} // namespace

TEST_CASE("IRC colours are 16 or 99, and do something only under IRC", "[irc-colours]") {
    CHECK(parseIrcColours("16") == IrcColours::Mirc16);
    CHECK(parseIrcColours("99") == IrcColours::Mirc99);
    for (const char* bad : {"", "15", "0x10", " 16", "sixteen", "98"})
        CHECK_FALSE(parseIrcColours(bad));
    CHECK(ircColourCount(IrcColours::Mirc16) == 16);
    CHECK(ircColourCount(IrcColours::Mirc99) == 99);
    CHECK(sentIrcColours(Target::Irc, IrcColours::Mirc16) == IrcColours::Mirc16);
    CHECK(sentIrcColours(Target::Ansi, IrcColours::Mirc16) == IrcColours::Mirc99);
    CHECK(sentIrcColours(Target::Micron, IrcColours::Mirc16) == IrcColours::Mirc99);
    CHECK(sentIrcColours(Target::Irc, IrcColours::Mirc99) == IrcColours::Mirc99);
}

TEST_CASE("nearestMirc16 folds every mIRC colour to the nearest of 0-15, ties to the lower", "[irc-colours]") {
    for (int i = 0; i < 16; ++i)
        CHECK(irc::nearestMirc16(irc::kMircRgb[std::size_t(i)]) == i);
    for (int i = 16; i < 99; ++i) {
        INFO("mIRC " << i);
        const int n = irc::nearestMirc16(irc::kMircRgb[std::size_t(i)]);
        CHECK(n < 16);
        CHECK(n == nearest16(irc::kMircRgb[std::size_t(i)]));
    }
    // the six that share an RGB with one of 0-15 fold to that one, as
    // nearestMirc maps them
    const std::pair<int, int> same[] = {{52, 4}, {54, 8}, {58, 11}, {62, 13}, {88, 1}, {98, 0}};
    for (const auto& [from, to] : same) {
        INFO("mIRC " << from);
        CHECK(irc::nearestMirc16(irc::kMircRgb[std::size_t(from)]) == to);
        CHECK(irc::nearestMirc(irc::kMircRgb[std::size_t(from)]) == to);
    }
    // white is mIRC 0 and black 1: a tie between equal colours goes to the
    // lower number, and any other colour to the one the slow search finds
    for (std::uint32_t rgb : {0x000000u, 0x123456u, 0xfedcbau, 0x808080u, 0x7f7f80u, 0x404040u})
        CHECK(irc::nearestMirc16(rgb) == nearest16(rgb));
}

TEST_CASE("fold16 keeps a palette colour and sends an RGB one as the palette colour of its nearest of 0-15",
          "[irc-colours]") {
    for (int i = 0; i < 16; ++i) {
        const Color c = Color::indexed(std::uint8_t(i));
        CHECK(irc::fold16(c) == c);
        CHECK(irc::paletteOfMirc(irc::mircColor(c)) == i);
    }
    for (int i = 0; i < 99; ++i) {
        INFO("mIRC " << i);
        const Color folded = irc::fold16(mircRgb(i));
        REQUIRE(folded.isIndexed());
        CHECK(irc::mircColor(folded) == irc::nearestMirc16(irc::kMircRgb[std::size_t(i)]));
    }
    const Color picked = Color::fromRgb({0x12, 0x9a, 0xe0});
    CHECK(irc::mircColor(irc::fold16(picked)) == nearest16(packed(picked)));
}

TEST_CASE("under IRC in 16 colours the palette has the 16 swatches", "[irc-colours][target]") {
    CHECK(swatchCount(Target::Irc, IrcColours::Mirc16) == 16);
    CHECK(swatchCount(Target::Irc, IrcColours::Mirc99) == 99);
    CHECK(swatchCount(Target::Irc) == 99);
    // nothing under the other targets
    CHECK(swatchCount(Target::Ansi, IrcColours::Mirc16) == 16);
    CHECK(swatchCount(Target::Micron, IrcColours::Mirc16) == 99);
    for (int i = 0; i < 16; ++i)
        CHECK(swatchOf(swatchColor(i), Target::Irc, IrcColours::Mirc16) == i);
    // an extended colour shows as the swatch it is sent as
    for (int i = 16; i < 99; ++i) {
        INFO("swatch " << i);
        const int at = swatchOf(swatchColor(i), Target::Irc, IrcColours::Mirc16);
        CHECK(at == irc::fold16(swatchColor(i)).index);
        CHECK(irc::mircColor(Color::indexed(std::uint8_t(at))) < 16);
    }
    CHECK(swatchOf(swatchColor(77), Target::Micron, IrcColours::Mirc16) == 77);
}

TEST_CASE("IRC text in 16 colours sends mIRC 77 as one of 0-15, the export, view and meter agreeing",
          "[irc-colours][irc]") {
    const Canvas canvas = extended();
    // 99: mIRC 77 goes as itself
    const Canvas all = irc::sentCanvas(canvas, true, Target::Irc);
    const std::string line99 = irc::exportCanvas(all).at(0);
    CHECK(line99.find("\x03" "77,40") != std::string::npos);

    const Canvas sent = irc::sentCanvas(canvas, true, Target::Irc, IrcColours::Mirc16);
    const std::vector<std::string> lines = irc::exportCanvas(sent);
    REQUIRE(lines.size() == 1);
    const std::vector<int> numbers = colourNumbers(lines[0]);
    REQUIRE_FALSE(numbers.empty());
    for (const int n : numbers)
        CHECK(n < 16);
    // the meter's line and the IRC view's are the export's
    CHECK(irc::encodeLine(sent, 0).text == lines[0]);
    const irc::View view = irc::ircView(sent);
    for (const irc::ShownCell& s : irc::decodeLine(lines[0])) {
        CHECK(s.cell.fg < 16);
        CHECK(s.cell.bg < 16);
    }
    // the view shows what an indexed canvas of the folded colours shows
    Canvas folded = canvas;
    for (int x = 0; x < folded.width(); ++x) {
        folded.at(x, 0).fg = irc::fold16(folded.at(x, 0).fg);
        folded.at(x, 0).bg = irc::fold16(folded.at(x, 0).bg);
    }
    CHECK(irc::exportCanvas(irc::sentCanvas(folded, true, Target::Irc)) == lines);
    CHECK(irc::ircView(folded).canvas == view.canvas);
    // an indexed cell is sent the same either way
    CHECK(irc::sentCell(canvas.at(8, 0), true, Target::Irc, IrcColours::Mirc16) == canvas.at(8, 0));
    // a transparent cell too
    Cell clear;
    clear.transparent = true;
    clear.fg = mircRgb(77);
    CHECK(irc::sentCell(clear, true, Target::Irc, IrcColours::Mirc16) == clear);
}

// Every golden piece is indexed only, so it sends golden-irc's bytes in 16
// colours as in 99.
TEST_CASE("golden pieces send golden-irc's bytes in 16 colours", "[irc-colours][irc]") {
    std::vector<fs::path> pieces;
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden"))
        if (e.path().extension() == ".ans")
            pieces.push_back(e.path());
    std::sort(pieces.begin(), pieces.end());
    REQUIRE(pieces.size() >= 12);
    for (const auto& p : pieces) {
        DYNAMIC_SECTION(p.filename().string()) {
            const std::string want = bwtest::readFile(bwtest::dataDir() / "golden-irc" / (p.stem().string() + ".txt"));
            const AnsiDocument doc = parseAnsi(bwtest::readFile(p));
            CHECK(irc::joinLines(irc::exportCanvas(irc::sentCanvas(doc.canvas, doc.ice, Target::Irc,
                                                                   IrcColours::Mirc16))) == want);
        }
    }
}

TEST_CASE("lint in 16 colours notes the cells folded, as a note", "[irc-colours][lint]") {
    using lint::Finding;
    Canvas c = extended();
    lint::LintOptions o;
    o.target = Target::Irc;
    // 99: no note
    CHECK(folds(lint::lintCanvas(c, o).findings).empty());

    o.colours = IrcColours::Mirc16;
    const lint::LintReport r = lint::lintCanvas(c, o);
    const auto found = folds(r.findings);
    REQUIRE(found.size() == 1);
    // 77, 33 and every cell's background 40 are extended; 52 ... 98 on 40
    // are folded for their background: all eight RGB cells, not the indexed
    CHECK(found[0].x == 0);
    CHECK(found[0].y == 0);
    CHECK(found[0].x2 == 0);
    CHECK(found[0].message == "8 cells are in mIRC colours past 15, sent as the nearest of 0-15 (IRC in 16 colours)");
    CHECK(lint::kindName(Finding::Kind::IrcFold) == "irc-fold");
    CHECK(lint::noteKind(Finding::Kind::IrcFold));
    CHECK(lint::summary(r).find("irc-fold") != std::string::npos);

    // only an RGB colour nearest one of 16-98 counts: one of the six that
    // share an RGB with 0-15 on black isn't folded, nor is a palette colour
    Canvas one(4, 2);
    one.at(1, 0).fg = mircRgb(52);
    one.at(1, 0).bg = mircRgb(98);
    one.at(2, 1).ch = U'x';
    one.at(2, 1).fg = mircRgb(70);
    one.at(3, 1).fg = mircRgb(70);
    one.at(3, 1).transparent = true;
    const auto single = folds(lint::lintCanvas(one, o).findings);
    REQUIRE(single.size() == 1);
    CHECK(single[0].x == 2);
    CHECK(single[0].y == 1);
    CHECK(single[0].message == "1 cell is in an mIRC colour past 15, sent as the nearest of 0-15 (IRC in 16 colours)");
    // a note alone leaves the report clean
    Canvas plain(2, 1);
    plain.at(0, 0).bg = mircRgb(70);
    plain.at(1, 0).bg = mircRgb(70);
    const lint::LintReport alone = lint::lintCanvas(plain, o);
    CHECK(folds(alone.findings).size() == 1);
    CHECK(alone.clean());
}

TEST_CASE("irc-fold counts only a colour the cell shows, a wide pair once", "[irc-colours][lint]") {
    lint::LintOptions o;
    o.target = Target::Irc;
    o.colours = IrcColours::Mirc16;
    const Color pink = Color::fromRgb({0xff, 0x9c, 0x9c});  // mIRC 76
    REQUIRE(irc::nearestMirc(packed(pink)) == 76);
    // a coloured foreground on a space and a coloured background under a
    // full block send the same bytes in 16 colours as in 99
    Canvas a(2, 1);
    a.at(0, 0) = Cell{U' ', pink, Color::indexed(0), 0, false};
    a.at(1, 0) = Cell{U'X', Color::indexed(7), Color::indexed(0), 0, false};
    Canvas b = a;
    b.at(0, 0) = Cell{U'\u2588', Color::indexed(0), pink, 0, false};
    for (const Canvas* c : {&a, &b}) {
        CHECK(irc::exportCanvas(irc::sentCanvas(*c, true, Target::Irc, IrcColours::Mirc16)) ==
              irc::exportCanvas(irc::sentCanvas(*c, true, Target::Irc)));
        CHECK(folds(lint::lintCanvas(*c, o).findings).empty());
    }
    // the same colour where the cell shows it counts
    a.at(0, 0).ch = U'X';
    CHECK(folds(lint::lintCanvas(a, o).findings).size() == 1);
    b.at(0, 0).ch = U'\u2592';
    CHECK(folds(lint::lintCanvas(b, o).findings).size() == 1);

    // a wide glyph is one cell, counted at its lead
    Canvas wide(3, 1);
    wide.at(0, 0) = Cell{U'\u4e2d', mircRgb(70), Color::indexed(0), 0, false};
    wide.at(1, 0) = Cell{Glyph::continuation(), mircRgb(70), mircRgb(70), 0, false};
    const auto found = folds(lint::lintCanvas(wide, o).findings);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 0);
    CHECK(found[0].message.rfind("1 cell is in an mIRC colour past 15", 0) == 0);
}

TEST_CASE("irc-fold notes a glyph the fold hides", "[irc-colours][lint]") {
    lint::LintOptions o;
    o.target = Target::Irc;
    o.colours = IrcColours::Mirc16;
    const Color pink = Color::fromRgb({0xff, 0x9c, 0x9c});  // mIRC 76, folds to 15
    // X in light grey (mIRC 15) on mIRC 76: in 16 colours both are 15
    Canvas c(3, 1);
    c.at(0, 0) = Cell{U'A', Color::indexed(14), Color::indexed(0), 0, false};
    c.at(1, 0) = Cell{U'X', Color::indexed(7), pink, 0, false};
    c.at(2, 0) = Cell{U'Y', Color::indexed(7), pink, 0, false};
    REQUIRE(irc::mircColor(irc::fold16(pink)) == irc::mircColor(Color::indexed(7)));
    const std::string sent99 = irc::exportCanvas(irc::sentCanvas(c, false, Target::Irc)).at(0);
    CHECK(sent99.find("X") != std::string::npos);
    const std::string sent16 = irc::exportCanvas(irc::sentCanvas(c, false, Target::Irc, IrcColours::Mirc16)).at(0);
    CHECK(sent16.find("X") == std::string::npos);
    o.ice = false;
    const auto found = folds(lint::lintCanvas(c, o).findings);
    REQUIRE(found.size() == 2);
    CHECK(found[0].x == 1);
    CHECK(found[0].message == "2 cells are in mIRC colours past 15, sent as the nearest of 0-15 (IRC in 16 colours)");
    CHECK(found[1].x == 1);
    CHECK(found[1].message == "2 cells' glyphs are hidden in 16 colours: their foregrounds and backgrounds fold to the "
                              "same colour");
    // one hidden glyph, in the singular; a glyph whose colours fold apart
    // isn't hidden
    c.at(2, 0).fg = Color::indexed(0);
    const auto one = folds(lint::lintCanvas(c, o).findings);
    REQUIRE(one.size() == 2);
    CHECK(one[1].message == "1 cell's glyph is hidden in 16 colours: its foreground and background fold to the same "
                            "colour");
    // nor is a glyph whose colours were the same already
    Canvas same(1, 1);
    same.at(0, 0) = Cell{U'X', mircRgb(77), mircRgb(77), 0, false};
    CHECK(folds(lint::lintCanvas(same, o).findings).size() == 1);
}

TEST_CASE("a project keeps its IRC colours; one without the key, or another number, is 99",
          "[irc-colours][project]") {
    using project::Project;
    AnsiDocument d;
    d.canvas = Canvas(4, 2);
    Project p = project::fromAnsi(d);
    p.target = Target::Irc;
    project::Entries e = project::encode(p);
    const QJsonObject all = object(e.at("manifest.json"));
    CHECK_FALSE(all.contains("ircColours"));
    CHECK(project::decode(e).ircColours == IrcColours::Mirc99);

    p.ircColours = IrcColours::Mirc16;
    e = project::encode(p);
    QJsonObject m = object(e.at("manifest.json"));
    CHECK(m.value("ircColours").toInt() == 16);
    // a preference: no version bump
    CHECK(m.value("version") == all.value("version"));
    Project back = project::decode(e);
    CHECK(back.ircColours == IrcColours::Mirc16);
    CHECK(back.target == Target::Irc);
    CHECK(back.layers == p.layers);
    // kept under the other targets, where it does nothing
    p.target = Target::Ansi;
    CHECK(project::decode(project::encode(p)).ircColours == IrcColours::Mirc16);

    // an older build drops the key and reads the same cells
    m.remove("ircColours");
    e["manifest.json"] = json(m);
    back = project::decode(e);
    CHECK(back.ircColours == IrcColours::Mirc99);
    CHECK(back.layers == p.layers);

    for (const double other : {99.0, 15.0, 256.0, 16.5, -16.0}) {
        INFO(other);
        m["ircColours"] = other;
        e["manifest.json"] = json(m);
        Project read;
        REQUIRE_NOTHROW(read = project::decode(e));
        CHECK(read.ircColours == IrcColours::Mirc99);
    }
    for (const QJsonValue bad : {QJsonValue("16"), QJsonValue(true), QJsonValue(QJsonValue::Null)}) {
        m["ircColours"] = bad;
        e["manifest.json"] = json(m);
        CHECK_THROWS_AS(project::decode(e), project::ProjectError);
    }
}
