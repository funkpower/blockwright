// Bold, italic and underline under the IRC target (#350 slice 3a): the
// attribute bits in a project (format 5.0) and their refusals, the encoder's
// ^B ^] ^_ and decodeLine reading them back, the sender's view, the lint, and
// the formats that can't hold them, and the #360 review's findings.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/lint_rules.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/xbin.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <string>
#include <vector>

using namespace bw;
using namespace bw::irc;
using bw::project::Entries;
using bw::project::Project;
using bw::project::ProjectError;
using Catch::Matchers::ContainsSubstring;
using Kind = lint::Finding::Kind;

namespace {

Cell cell(char32_t ch, std::uint8_t fg = Grey, std::uint8_t bg = Black, std::uint8_t attrs = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    c.attrs = attrs;
    return c;
}

MircCell mirc(char32_t ch, std::uint8_t attrs = 0, std::uint8_t fg = 0, std::uint8_t bg = 1) {
    return MircCell{ch, fg, bg, attrs};
}

Project oneLayer(const Canvas& cells) {
    Project p;
    p.layers = LayerStack(cells.width(), cells.height());
    Layer l = makeLayer(LayerKind::Cells, cells.width(), cells.height(), "art");
    l.cells = cells;
    p.layers.add(std::move(l));
    return p;
}

QJsonObject manifest(const Entries& e) {
    const std::string& m = e.at("manifest.json");
    return QJsonDocument::fromJson(QByteArray(m.data(), qsizetype(m.size()))).object();
}

void setVersion(Entries& e, const char* v) {
    QJsonObject o = manifest(e);
    o["version"] = v;
    e["manifest.json"] = QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
}

std::vector<lint::Finding> of(const std::vector<lint::Finding>& all, Kind k) {
    std::vector<lint::Finding> out;
    std::copy_if(all.begin(), all.end(), std::back_inserter(out), [&](const auto& f) { return f.kind == k; });
    return out;
}

} // namespace

// ---- the project ------------------------------------------------------------

TEST_CASE("a project with bold, italic or underline is format 5.0 and reads back", "[irc][attrs][project]") {
    Canvas plain(4, 1);
    plain.at(0, 0) = cell(U'A', Grey, Black, attr::Blink);
    const Entries before = project::encode(oneLayer(plain));
    CHECK(manifest(before).value("version").toString().toStdString() ==
          std::to_string(project::kFormatMajor) + "." + std::to_string(project::kFormatMinor));

    for (const std::uint8_t a : {attr::Bold, attr::Italic, attr::Underline}) {
        Canvas c = plain;
        c.at(2, 0) = cell(U'B', Yellow, Blue, a);
        const Entries e = project::encode(oneLayer(c));
        CHECK(manifest(e).value("version").toString() == "5.0");
        const Project back = project::decode(e);
        CHECK(back.layers == oneLayer(c).layers);
        CHECK(back.layers[0].cells.at(2, 0).attrs == a);
    }
}

TEST_CASE("formatting in a project before 5.0 is cleared on load, and counted", "[irc][attrs][project]") {
    // no build before 5.0 wrote it, so it is a stray bit, not formatting
    Canvas c(2, 1);
    c.at(1, 0) = cell(U'x', Grey, Black, attr::Underline);
    Canvas plain = c;
    plain.at(1, 0).attrs = 0;
    Entries e = project::encode(oneLayer(c));
    for (const char* v : {"4.0", "3.0", "1.8"}) {
        INFO(v);
        setVersion(e, v);
        project::ReadNotes notes;
        CHECK(project::decode(e, &notes).layers == oneLayer(plain).layers);
        CHECK(notes.strayAttributes == 1);
    }
    setVersion(e, "5.1");  // a newer minor reads
    project::ReadNotes notes;
    CHECK(project::decode(e, &notes).layers == oneLayer(c).layers);
    CHECK(notes.strayAttributes == 0);
}

TEST_CASE("a cell with an attribute bit nothing defines is refused", "[irc][attrs][project]") {
    // the strikethrough bit is reserved, not yet read
    for (const std::uint8_t bit : {attr::ReservedStrike, std::uint8_t(1u << 5), std::uint8_t(1u << 6),
                                   std::uint8_t(1u << 7)}) {
        INFO(int(bit));
        Canvas c(1, 1);
        c.at(0, 0).attrs = std::uint8_t(bit | attr::Bold);
        CHECK_THROWS_WITH(project::decodeCells(project::encodeCells(c)), ContainsSubstring("unknown attribute"));
    }
}

// ---- the encoder ------------------------------------------------------------

TEST_CASE("the encoder switches bold, italic and underline on and off, a byte a toggle", "[irc][attrs][encode]") {
    const std::vector<MircCell> plain = {mirc(U'a'), mirc(U'b'), mirc(U'c'), mirc(U'd')};
    const std::string base = encodeMircRow(plain);

    std::vector<MircCell> bold = plain;
    bold[1].attrs = bold[2].attrs = attr::Bold;
    const std::string b = encodeMircRow(bold);
    CHECK(b.size() == base.size() + 2);  // on before b, off before d
    CHECK(b.find("\x02" "bc\x02" "d") != std::string::npos);

    // all three switch on in one place, in ATTR_CODES order
    std::vector<MircCell> all = plain;
    all[0].attrs = attr::Bold | attr::Italic | attr::Underline;
    const std::string a = encodeMircRow(all);
    CHECK(a.find("\x02\x1d\x1f" "a") != std::string::npos);
    CHECK(a.size() == base.size() + 6);

    // a toggle after a colour code: the code's digits can't swallow it
    std::vector<MircCell> coloured = {mirc(U'1', 0, 4, 2), mirc(U'2', attr::Italic, 3, 2)};
    const std::string s = encodeMircRow(coloured);
    const auto cells = decodeLine(s);
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].cell.ch == Glyph(U'1'));
    CHECK(cells[1].cell.ch == Glyph(U'2'));
    CHECK(cells[1].cell.fg == 3);
    CHECK(cells[1].cell.attrs == attr::Italic);
}

TEST_CASE("an encoded row with formatting reads back as its cells", "[irc][attrs][encode][view]") {
    const std::uint8_t attrs[] = {0, attr::Bold, attr::Italic, attr::Underline, attr::Bold | attr::Underline,
                                  attr::Italic | attr::Underline, attr::Bold | attr::Italic};
    std::vector<MircCell> row;
    for (int i = 0; i < 40; ++i)
        // never a glyph in its own background, which draws as a blank
        row.push_back(mirc(char32_t(U'A' + i % 26), attrs[(i * 5) % 7], std::uint8_t((i / 3) % 16),
                           std::uint8_t(((i / 3) % 16 + 1 + (i / 7) % 15) % 16)));
    const std::string line = encodeMircRow(row);
    const auto back = decodeLine(line);
    REQUIRE(back.size() == row.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
        INFO(i);
        CHECK(back[i].cell.ch == row[i].ch);
        CHECK(back[i].cell.attrs == row[i].attrs);
        CHECK(back[i].cell.fg == row[i].fg);
        CHECK(back[i].cell.bg == row[i].bg);
    }
}

TEST_CASE("an underlined cell is drawn as itself, and an underlined space at a row's end is kept",
          "[irc][attrs][encode]") {
    // ▀ underlined: its own glyph in its own colours, not ▄ reversed
    const std::vector<MircCell> half = {mirc(U'x', 0, 4, 2), mirc(U'▀', attr::Underline, 2, 4)};
    const auto cells = decodeLine(encodeMircRow(half));
    REQUIRE(cells.size() == 2);
    CHECK(cells[1].cell.ch == Glyph(U'▀'));
    CHECK(cells[1].cell.fg == 2);
    CHECK(cells[1].cell.bg == 4);

    // a black space ends the row: dropped plain, kept underlined
    Canvas c(3, 1);
    c.at(0, 0) = cell(U'a', White, Blue);
    c.at(1, 0) = cell(U' ');
    c.at(2, 0) = cell(U' ');
    CHECK(decodeLine(encodeCanvas(c)[0]).size() == 1);
    c.at(2, 0).attrs = attr::Underline;
    const auto kept = decodeLine(encodeCanvas(c)[0]);
    REQUIRE(kept.size() == 3);
    CHECK(kept[2].cell.attrs == attr::Underline);
}

TEST_CASE("a row without formatting encodes as before", "[irc][attrs][encode]") {
    // the attribute-free search is the same: Blink alone isn't formatting
    std::vector<MircCell> row;
    for (int i = 0; i < 30; ++i)
        row.push_back(mirc(i % 2 ? U'▀' : U'▄', 0, std::uint8_t(i % 5), std::uint8_t(i % 3)));
    std::vector<MircCell> blink = row;
    for (MircCell& m : blink)
        m.attrs = attr::Blink;
    CHECK(encodeMircRow(blink) == encodeMircRow(row));
}

// ---- reading ----------------------------------------------------------------

TEST_CASE("decodeLine toggles bold, italic and underline, and ^O clears them", "[irc][attrs][view]") {
    const auto cells = decodeLine("\x02" "a\x1d" "b\x0f" "c\x1f" "d\x1f" "e\x1e" "f\x11" "g");
    REQUIRE(cells.size() == 7);
    CHECK(cells[0].cell.attrs == attr::Bold);
    CHECK(cells[1].cell.attrs == (attr::Bold | attr::Italic));
    CHECK(cells[2].cell.attrs == 0);
    CHECK(cells[3].cell.attrs == attr::Underline);
    CHECK(cells[4].cell.attrs == 0);
    // strikethrough and monospace are read and draw nothing
    CHECK(cells[5].cell.ch == Glyph(U'f'));
    CHECK(cells[5].cell.attrs == 0);
    CHECK(cells[6].cell.ch == Glyph(U'g'));
    // ^B right after a colour code ends it
    const auto coloured = decodeLine("\x03" "4\x02" "5");
    REQUIRE(coloured.size() == 1);
    CHECK(coloured[0].cell.ch == Glyph(U'5'));
    CHECK(coloured[0].cell.fg == 4);
    CHECK(coloured[0].cell.attrs == attr::Bold);
}

TEST_CASE("the IRC view shows a cell's formatting, and a formatted line is cut by the sender's full encoding",
          "[irc][attrs][view]") {
    Canvas c(4, 1);
    c.at(0, 0) = cell(U'a', White, Blue, attr::Bold);
    c.at(1, 0) = cell(U'b', White, Blue, attr::Italic | attr::Underline);
    const View v = ircView(c);
    CHECK(v.canvas.at(0, 0).attrs == attr::Bold);
    CHECK(v.canvas.at(1, 0).attrs == (attr::Italic | attr::Underline));
    CHECK(v.canvas.at(2, 0).attrs == 0);

    // a long line, cut: the cells it shows keep their formatting
    Canvas wide(300, 1);
    for (int x = 0; x < 300; ++x)
        wide.at(x, 0) = cell(char32_t(U'a' + x % 26), std::uint8_t(x % 16), std::uint8_t((x / 2) % 8),
                             x % 3 ? attr::Bold : std::uint8_t(0));
    const View cut = ircView(wide, 407);
    REQUIRE(cut.rows[0].cut());
    REQUIRE(cut.rows[0].shown > 0);
    for (int x = 0; x < cut.rows[0].shown; ++x)
        CHECK(cut.canvas.at(x, 0).attrs == wide.at(x, 0).attrs);
    // the same line without formatting carries more cells in the budget
    Canvas unformatted = wide;
    for (int x = 0; x < 300; ++x)
        unformatted.at(x, 0).attrs = 0;
    CHECK(ircView(unformatted, 407).rows[0].shown > cut.rows[0].shown);
}

// ---- the lint and the other formats -------------------------------------------

TEST_CASE("wasted-format: formatting on a cell where it draws nothing", "[irc][attrs][lint]") {
    Canvas c(6, 1);
    c.at(0, 0) = cell(U' ', Grey, Black, attr::Bold);           // wasted
    c.at(1, 0) = cell(U' ', Grey, Black, attr::Underline);      // draws a line
    c.at(2, 0) = cell(U'█', Grey, Black, attr::Underline | attr::Italic);  // italic shears it; underline wasted
    c.at(3, 0) = cell(U'A', Grey, Black, attr::Bold | attr::Italic | attr::Underline);
    c.at(4, 0) = cell(U'▀', Grey, Black, attr::Bold);  // a full-width half: wasted
    c.at(5, 0) = cell(U' ', Grey, Black, attr::Italic);
    c.at(5, 0).transparent = true;  // nothing is sent for it
    for (const Target t : {Target::Ansi, Target::Irc}) {
        lint::RuleOptions o;
        o.target = t;
        const auto found = of(lint::checkRules(c, o), Kind::WastedFormat);
        REQUIRE(found.size() == 3);
        CHECK(found[0].x == 0);
        CHECK(found[0].message == "bold on a blank draws nothing and costs bytes in IRC text");
        CHECK(found[1].x == 2);
        CHECK(found[1].message == "underline on a full block draws nothing and costs bytes in IRC text");
        CHECK(found[2].x == 4);
        CHECK(found[2].message == "bold on ▀ draws nothing and costs bytes in IRC text");
    }
    CHECK(lint::kindName(Kind::WastedFormat) == "wasted-format");
    CHECK_FALSE(lint::noteKind(Kind::WastedFormat));
}

TEST_CASE("ans-loss counts the formatted cells an .ans leaves out", "[irc][attrs][lint]") {
    Canvas c(4, 1);
    c.at(1, 0) = cell(U'A', Grey, Black, attr::Bold);
    c.at(3, 0) = cell(U'B', Grey, Black, attr::Underline);
    lint::RuleOptions o;
    o.target = Target::Irc;
    const auto found = of(lint::checkRules(c, o), Kind::AnsLoss);
    REQUIRE(found.size() == 1);
    CHECK(found[0].x == 1);
    CHECK(found[0].message == "2 cells have bold, italic or underline, which an .ans leaves out (fine for IRC)");
    o.target = Target::Ansi;
    CHECK(of(lint::checkRules(c, o), Kind::AnsLoss).empty());
}

TEST_CASE(".ans, XBIN and BIN report the formatting they leave out", "[irc][attrs][ansi]") {
    AnsiDocument doc;
    doc.canvas = Canvas(5, 1);
    doc.canvas.at(0, 0) = cell(U'A', Grey, Black, attr::Bold);
    doc.canvas.at(1, 0) = cell(U'B', Grey, Black, attr::Italic | attr::Underline);
    doc.canvas.at(2, 0) = cell(U'C', Grey, Black, attr::Blink);  // not formatting
    doc.canvas.at(3, 0) = cell(U'D', Grey, Black, attr::Bold);
    doc.canvas.at(3, 0).transparent = true;  // not drawn
    CHECK(formattedCellCount(doc.canvas) == 2);
    for (const ArtFormat f : {ArtFormat::Ansi, ArtFormat::Xbin, ArtFormat::Bin}) {
        const AnsiOutput out = writeArt(doc, f);
        CHECK(out.formattingDropped == 2);
        CHECK(out.lossy());
    }
    AnsiDocument plain;
    plain.canvas = Canvas(2, 1);
    CHECK(writeAnsi(plain).formattingDropped == 0);
}

// ---- the #360 review ----------------------------------------------------------

TEST_CASE("the shade tools keep a cell's formatting", "[irc][attrs][brush]") {
    const Ramp steel = *findRamp("steel");
    Cell c = rungCell(steel, 5);
    c.attrs = attr::Underline;
    const std::optional<Cell> up = stepRung(steel, c, 1);
    REQUIRE(up);
    CHECK(up->attrs == attr::Underline);

    // the Shade Brush on a full block, which it can't step further, and on a
    // light shade, which it steps
    const Pen pen{Color::indexed(Grey), Color::indexed(Black)};
    Canvas canvas(2, 1);
    canvas.at(0, 0) = cell(U'█', Grey, Black, attr::Underline);
    canvas.at(1, 0) = cell(U'░', Grey, Black, attr::Bold | attr::Italic);
    BrushSettings s;
    s.size = 1;
    const CellEdit e = paintStroke(canvas, ShadeBrush(pen, false), s, {{0, 0}, {1, 0}});
    for (const CellChange& ch : e.changes) {
        INFO(ch.x);
        CHECK(ch.after.attrs == canvas.at(ch.x, ch.y).attrs);
    }
    const auto stepped = std::find_if(e.changes.begin(), e.changes.end(), [](const auto& ch) { return ch.x == 1; });
    REQUIRE(stepped != e.changes.end());
    CHECK(stepped->after.ch == Glyph(U'▒'));

    // the Form Shade brush, which writes each cell's rung afresh
    Canvas field(5, 5);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) {
            field.at(x, y) = rungCell(steel, 2 + (x + y) % 3);
            field.at(x, y).attrs = attr::Underline;
        }
    BrushSettings wide;
    wide.size = 5;
    const CellEdit formed = paintStroke(field, FormShadeBrush(steel), wide, {{2, 2}});
    REQUIRE_FALSE(formed.empty());
    for (const CellChange& ch : formed.changes) {
        INFO(ch.x << "," << ch.y);
        CHECK(ch.after.attrs == attr::Underline);
    }
}

TEST_CASE("an underlined space draws a line: paste reads it as drawn", "[irc][attrs][selection]") {
    const Cell u = cell(U' ', Grey, Black, attr::Underline);
    const Cell letter = cell(U'A');
    CHECK_FALSE(isBlank(u));
    CHECK(pasteCell(u, Cell{}, PasteMode::Transparent) == u);
    CHECK(pasteCell(letter, u, PasteMode::Under) == std::nullopt);
    // bold and italic on a space draw nothing: it stays blank
    const Cell b = cell(U' ', Grey, Black, attr::Bold | attr::Italic);
    CHECK(isBlank(b));
    CHECK(pasteCell(b, Cell{}, PasteMode::Transparent) == std::nullopt);
    CHECK(pasteCell(letter, b, PasteMode::Under) == letter);
}

TEST_CASE("the IRC trim lint keeps an underlined space as the encoder does", "[irc][attrs][lint]") {
    lint::RuleOptions o;
    o.target = Target::Irc;
    // row 0 ends in an underlined black space, which the line keeps
    Canvas c(2, 2);
    c.at(0, 0) = cell(U'A');
    c.at(1, 0) = cell(U' ', Grey, Black, attr::Underline);
    c.at(0, 1) = cell(U'A');
    c.at(1, 1) = cell(U'A');
    REQUIRE(decodeLine(encodeCanvas(c)[0]).size() == 2);
    CHECK(of(lint::checkRules(c, o), Kind::TrailingBlack).empty());
    // plain, the space is dropped and the lint says so
    c.at(1, 0).attrs = attr::Bold;
    REQUIRE(decodeLine(encodeCanvas(c)[0]).size() == 1);
    CHECK(of(lint::checkRules(c, o), Kind::TrailingBlack).size() == 1);

    // an edge row of underlined spaces is sent, so it isn't a blank edge row
    Canvas e(2, 2);
    e.at(0, 0) = cell(U' ', Grey, Black, attr::Underline);
    e.at(1, 0) = cell(U' ', Grey, Black, attr::Underline);
    e.at(0, 1) = cell(U'A');
    e.at(1, 1) = cell(U'A');
    REQUIRE(encodeCanvas(e).size() == 2);
    REQUIRE_FALSE(encodeCanvas(e)[0].empty());
    CHECK(of(lint::checkRules(e, o), Kind::BlankEdgeRows).empty());
}

TEST_CASE("wasted-format: italic shears a full block, so it draws", "[irc][attrs][lint]") {
    Canvas c(1, 1);
    c.at(0, 0) = cell(U'█', Grey, Black, attr::Italic);
    lint::RuleOptions o;
    o.target = Target::Irc;
    CHECK(of(lint::checkRules(c, o), Kind::WastedFormat).empty());
}

TEST_CASE("a project before 5.0 opens with its stray attribute bits cleared", "[irc][attrs][project]") {
    // no build before 5.0 wrote any bit but blink, so the rest are stray
    Canvas c(3, 1);
    c.at(0, 0) = cell(U'x', Grey, Black, attr::Underline);
    c.at(1, 0) = cell(U'y', Grey, Black, std::uint8_t(0xA0 | attr::Blink));
    c.at(2, 0) = cell(U'z', Grey, Black, attr::Blink);
    Entries e = project::encode(oneLayer(c));
    // what a 4.x writer could have left: bits above blink in a .cells file
    Canvas raw = c;
    raw.at(0, 0).attrs = std::uint8_t(attr::Underline | attr::ReservedStrike);
    for (const auto& [name, data] : e)
        if (name.size() > 6 && name.substr(name.size() - 6) == ".cells")
            e[name] = project::encodeCells(raw);
    for (const char* v : {"4.0", "3.0", "1.8"}) {
        INFO(v);
        setVersion(e, v);
        const Project p = project::decode(e);
        const Canvas& back = p.layers[0].cells;
        CHECK(back.at(0, 0).attrs == 0);
        CHECK(back.at(1, 0).attrs == attr::Blink);
        CHECK(back.at(2, 0).attrs == attr::Blink);
    }
    // in 5.0 the reserved bit is still refused
    setVersion(e, "5.0");
    CHECK_THROWS_WITH(project::decode(e), ContainsSubstring("unknown attribute"));
}
