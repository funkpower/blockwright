// Cell oracles: ascii.py 1.32.1 render_pixel, run headlessly from a scratch
// copy with grid_to_irc replaced by a cell capture (after strip_rows), width
// 78. Each hex digit is the reference background's mIRC colour; dot is CLEAR.
// The bundled cases use the thinned colours listed in PLAN §15; Rainbow keeps
// the reference colours and is thinned by thinRainbow below. The glyph
// oracle is that copy's PIXEL_FONT; mini-reference uses render_mini/parse_irc.
#include "blockwright/big_text.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/lint.h"
#include "blockwright/lint_rules.h"
#include "blockwright/ramp.h"
#include "blockwright/project.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <filesystem>
#include <set>

using namespace bw;
namespace {
QJsonArray fixtures(const char* name) {
    const auto bytes = bwtest::readFile(bwtest::dataDir() / "text" / name);
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(bytes));
    REQUIRE(doc.isArray());
    return doc.array();
}
std::vector<std::string> visible(const Canvas& canvas, bool halfPixels = false) {
    std::vector<std::string> rows;
    for (int y = 0; y < canvas.height(); ++y)
        for (int half = 0; half < (halfPixels ? 2 : 1); ++half) {
            std::string row;
            for (int x = 0; x < canvas.width(); ++x) {
                const auto& c = canvas.at(x, y);
                int colour = -1;
                if (!c.transparent) {
                    auto shown = c.fg;
                    if (c.ch == U' ' || (c.ch == U'▀' && half) || (c.ch == U'▄' && !half))
                        shown = c.bg;
                    if (shown != Color::indexed(Black))
                        colour = irc::mircColor(shown);
                }
                row += colour < 0 ? '.' : "0123456789abcdef"[colour];
            }
            rows.push_back(row);
        }
    return rows;
}
// Reference IRC drops leading clear columns globally and trailing clear
// cells per row. Compare a rectangle enclosing its visible cells.
std::vector<std::string> tight(std::vector<std::string> rows) {
    std::size_t left = std::string::npos, right = 0;
    for (const auto& row : rows)
        if (const auto x = row.find_first_not_of('.'); x != row.npos) {
            left = std::min(left, x);
            right = std::max(right, row.find_last_not_of('.') + 1);
        }
    if (left == std::string::npos)
        return {};
    for (auto& row : rows) {
        row.resize(right, '.');
        row = row.substr(left, right - left);
    }
    while (!rows.empty() && rows.back().find_first_not_of('.') == std::string::npos)
        rows.pop_back();
    while (!rows.empty() && rows.front().find_first_not_of('.') == std::string::npos)
        rows.erase(rows.begin());
    return rows;
}
std::vector<std::string> expected(const QJsonObject& o) {
    std::vector<std::string> rows;
    for (const auto row : o.value("rows").toArray())
        rows.push_back(row.toString().toStdString());
    return tight(rows);
}
// Big rainbow's thinning: a pixel with no edge-neighbour of its own colour
// shows white (mIRC 0).
std::vector<std::string> thinRainbow(std::vector<std::string> rows) {
    const auto at = [&](int x, int y) {
        return y < 0 || y >= int(rows.size()) || x < 0 || x >= int(rows[y].size()) ? '.' : rows[y][x];
    };
    auto out = rows;
    for (int y = 0; y < int(rows.size()); ++y)
        for (int x = 0; x < int(rows[y].size()); ++x)
            if (const char c = at(x, y);
                c != '.' && at(x - 1, y) != c && at(x + 1, y) != c && at(x, y - 1) != c && at(x, y + 1) != c)
                out[y][x] = '0';
    return out;
}
std::vector<std::string> expectedShipped(const QJsonObject& o) {
    return o.value("style").toString() == "rainbow" ? thinRainbow(expected(o)) : expected(o);
}
// The ladder findings the bundled matrix forbids: any from lintCanvas with
// the named ramps and craft pairs, and any but Noise from each ramp's ladder.
std::vector<std::string> ladderFailures(const Canvas& c, bool ice) {
    std::vector<std::string> out;
    lint::LintOptions options;
    options.ramps = namedRamps();
    options.craftPairs = true;
    options.ice = ice;
    for (const auto& f : lint::lintCanvas(c, options).findings)
        if (lint::ladderKind(f.kind))
            out.push_back(f.message + " at " + std::to_string(f.x) + "," + std::to_string(f.y) + " on " + f.ramp);
    for (const auto& ramp : namedRamps()) {
        lint::LadderOptions ladder;
        ladder.fallback = true;
        ladder.ice = ice;
        for (const auto& f : lint::checkLadder(c, ramp, {}, ladder).findings)
            if (f.kind != lint::LadderFinding::Kind::Noise)
                out.push_back("ladder " + ramp.name + " at " + std::to_string(f.x) + "," + std::to_string(f.y));
    }
    return out;
}
TextSource source(const QJsonObject& o) {
    TextSource s;
    s.text = o.value("text").toString().toStdU32String();
    const auto style = bigTextStyleFromName(o.value("style").toString().toStdString());
    REQUIRE(style);
    s.bigStyle = *style;
    for (const auto c : o.value("colours").toArray())
        s.bigColours.push_back(std::uint8_t(c.toInt()));
    if (s.text.find(U' ') != s.text.npos)
        s.wrapWidth = 80;
    return s;
}
} // namespace

TEST_CASE("big styles match reference cells for fixture words and custom colours", "[big-text][oracle]") {
    int drawings = 0;
    for (const auto value : fixtures("big-styles.json")) {
        const auto o = value.toObject();
        const auto s = source(o);
        INFO(bigTextStyleName(s.bigStyle) << " / " << o.value("text").toString().toStdString());
        REQUIRE_FALSE(textSourceProblem(s));
        const auto result = renderText(s, blockFace());
        CHECK(result.missing.empty());
        CHECK(tight(visible(result.cells)) == expectedShipped(o));
        // Full foreground blocks preserve bright colours without iCE.
        for (int y = 0; y < result.cells.height(); ++y)
            for (int x = 0; x < result.cells.width(); ++x) {
                const auto& c = result.cells.at(x, y);
                if (!c.transparent) {
                    CHECK(c.ch == U'█');
                    CHECK(c.bg == Color::indexed(Black));
                    CHECK(c.attrs == 0);
                }
            }
        ++drawings;
    }
    CHECK(drawings == 86);
}

TEST_CASE("the big font adapter matches all reference printable glyphs", "[big-text][oracle]") {
    std::istringstream in(bwtest::readFile(bwtest::dataDir() / "text" / "big-glyphs.txt"));
    std::string hex;
    int ch = 32;
    while (in >> hex) {
        INFO(ch);
        REQUIRE(hex.size() == 16);
        const auto g = bigTextFace().glyph(char32_t(ch));
        REQUIRE(g);
        for (int y = 0; y < 8; ++y) {
            const auto bits = std::stoul(hex.substr(2 * y, 2), nullptr, 16);
            for (int x = 0; x < 8; ++x) {
                CHECK(g->mask.at(x, 2 * y) == bool(bits & (0x80 >> x)));
                CHECK(g->mask.at(x, 2 * y + 1) == bool(bits & (0x80 >> x)));
            }
        }
        ++ch;
    }
    CHECK(ch == 127);
    CHECK(screenConfusables(bigTextFace()) == bigTextFace().confusables());
    CHECK_FALSE(bigTextFace().glyph(U'λ'));
}

TEST_CASE("mini's existing face matches the reference half pixels", "[big-text][oracle]") {
    for (const auto v : fixtures("mini-reference.json")) {
        const auto o = v.toObject();
        TextSource s;
        s.face.name = "mini";
        s.colour = Yellow;
        s.text = o.value("text").toString().toStdU32String();
        CHECK(tight(visible(renderText(s, miniFace()).cells, true)) == expected(o));
    }
}

TEST_CASE("each big style passes the fixture lint and IRC budget with iCE on and off", "[big-text][matrix]") {
    int drawings = 0;
    for (const auto v : fixtures("big-styles.json")) {
        const auto o = v.toObject();
        if (!o.value("bundled").toBool() && o.value("style").toString() != "rainbow")
            continue;
        const auto s = source(o);
        for (const bool ice : {true, false}) {
            INFO(bigTextStyleName(s.bigStyle)
                 << " / " << o.value("text").toString().toStdString() << " / " << ice);
            const auto c = renderStyledText(s, {}, blockFace(), ice).cells;
            CHECK(tight(visible(c)) == expectedShipped(o));
            CHECK(c.width() <= 80);
            CHECK(ladderFailures(c, ice).empty());
            for (const auto& line : irc::encodeCanvas(c))
                CHECK(irc::txtSentBytes(line) <= irc::kEfnetWeechatBudget);
            ++drawings;
        }
    }
    CHECK(drawings == 92);
}

TEST_CASE("big text project round trips edits undo redo and bake", "[big-text][project]") {
    namespace fs = std::filesystem;
    const auto root = fs::path(BW_TEST_TMP) / "big-text";
    fs::remove_all(root);
    fs::create_directories(root);
    int cases = 0;
    for (const auto v : fixtures("big-styles.json")) {
        const auto s = source(v.toObject());
        for (const bool ice : {true, false}) {
            project::Project p;
            p.layers = LayerStack(80, 80);
            p.ice = ice;
            const auto id = p.layers.add(makeTextLayer(s, "Big text", 1, 1));
            auto& layer = *p.layers.find(id);
            const auto before = layer;
            TextSource next = s;
            next.text = U"BLAZE";
            next.bigColours = {11, 6, 8};
            const auto e = textEdit(layer, next, 0, ice);
            applyEdit(layer, e);
            const auto edited = layer;
            applyEdit(layer, e, true);
            CHECK(layer == before);
            applyEdit(layer, e);
            CHECK(layer == edited);
            applyEdit(layer, e, true);
            CHECK(project::decode(project::encode(p)) == p);
            project::writeZip(p, root / "text.bwp");
            project::writeDirectory(p, root / "text");
            for (const auto path : {root / "text.bwp", root / "text"}) {
                auto reopened = project::read(path);
                REQUIRE(reopened == p);
                auto& l = *reopened.layers.find(id);
                const auto bake = bakeEdit(l, ice);
                applyEdit(l, bake);
                CHECK(l.cells == before.cells);
                CHECK_FALSE(l.props.text);
                applyEdit(l, bake, true);
                CHECK(l == before);
                applyEdit(l, textEdit(l, next, 0, ice));
                CHECK(l == edited);
            }
            ++cases;
        }
    }
    CHECK(cases == 172);
    fs::remove_all(root);
}

TEST_CASE("big styles validate colours modes faces and allocation bounds", "[big-text]") {
    CHECK(parseBigTextColours("YELLOW, purple") == std::vector<std::uint8_t>{8, 6});
    CHECK(parseBigTextColours("04,7,8") == std::vector<std::uint8_t>{4, 7, 8});
    CHECK(parseBigTextColours("fire") == std::vector<std::uint8_t>{4, 7, 8, 7, 4, 5});
    CHECK(parseBigTextColours("rainbow")->size() == 13);
    for (const auto bad : {"", "4,", ",4", "16", "-1", "4,x", "1.5", "4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0"})
        CHECK_FALSE(parseBigTextColours(bad));
    TextSource s;
    s.text = U"420";
    s.bigStyle = BigTextStyle::Shadow;
    s.face.name = "mini";
    CHECK(textSourceProblem(s));
    s.face.name = "block";
    s.bigColours = {16};
    CHECK(textSourceProblem(s));
    s.bigColours.assign(17, 4);
    CHECK(textSourceProblem(s));
    s.bigColours.clear();
    s.bigStyle = BigTextStyle(255);
    CHECK(textSourceProblem(s));
    s.bigStyle = BigTextStyle::Block;
    s.text.assign(TextSource::kMaxLength, U'A');
    s.wrapWidth = TextSource::kMaxWrapWidth;
    s.letterSpacing = TextSource::kMaxSpacing;
    s.lineSpacing = TextSource::kMaxSpacing;
    CHECK(textSourceProblem(s));
    CHECK_THROWS_AS(renderText(s, blockFace()), TextError);
}

TEST_CASE("unthinned reference colours and Rainbow retain their lint failures", "[big-text][matrix]") {
    // the reference's own cells: Big rainbow ships thinned (thinRainbow)
    std::set<std::string> failing;
    for (const auto v : fixtures("big-styles.json")) {
        const auto o = v.toObject();
        if (o.value("bundled").toBool() || o.value("text").toString() != "ZB8S5O0I1")
            continue;
        // The reference cells are background-colour pixels. Use equivalent
        // foreground blocks, as the four shipped modes do for iCE off.
        const auto rows = expected(o);
        REQUIRE_FALSE(rows.empty());
        Canvas c(int(rows[0].size()), int(rows.size()));
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x) {
                Cell pixel = transparentCell();
                if (rows[y][x] != '.') {
                    const int mirc = int(std::string_view("0123456789abcdef").find(rows[y][x]));
                    pixel = Cell{};
                    pixel.ch = U'█';
                    for (int n = 0; n < 16; ++n)
                        if (irc::mircColor(Color::indexed(n)) == mirc)
                            pixel.fg = Color::indexed(n);
                }
                c.at(x, y) = pixel;
            }
        for (const bool ice : {true, false}) {
            lint::LintOptions options;
            options.ramps = namedRamps();
            options.craftPairs = true;
            options.ice = ice;
            bool fails = false;
            for (const auto& f : lint::lintCanvas(c, options).findings)
                fails = fails || lint::ladderKind(f.kind);
            const auto style = o.value("style").toString().toStdString();
            CHECK(fails == (style != "outline"));
            if (fails)
                failing.insert(style);
        }
    }
    CHECK(failing == std::set<std::string>{"shadow", "3d", "block", "rainbow"});
    CHECK(bigTextStyleFromName("rainbow") == BigTextStyle::Rainbow);
}

TEST_CASE("Big rainbow walks its colours a character at a time and thins every glyph clean",
          "[big-text][matrix][rainbow]") {
    TextSource s;
    s.bigStyle = BigTextStyle::Rainbow;
    CHECK(bigTextDefaultColours(BigTextStyle::Rainbow) == std::vector<std::uint8_t>{4, 7, 8, 9, 11, 12, 13});
    // the defaults are the reference walk
    s.text = U"BLAZE 420";
    s.wrapWidth = 80;
    const auto defaults = renderText(s, blockFace()).cells;
    s.bigColours = bigTextDefaultColours(BigTextStyle::Rainbow);
    CHECK(renderText(s, blockFace()).cells == defaults);
    // each character takes the next colour, spaces and missing glyphs too,
    // and each line starts the walk again
    s.wrapWidth = 0;
    s.bigColours = {4, 9, 12};
    s.text = U"II I\nIλI";
    const auto drawn = renderBigText(s);
    REQUIRE(drawn.lines.size() == 2);
    const auto colourAt = [&](int x, int y) {
        const auto& c = drawn.drawing.cells.at(x, y);
        return c.transparent ? -1 : irc::mircColor(c.fg);
    };
    // an I's stem is columns 3 and 4 of its 8, rows 1 to 5
    const auto stem = [&](const BigTextLine& line, int k) { return colourAt(line.left + 8 * k + 3, line.top + 3); };
    CHECK(stem(drawn.lines[0], 0) == 4);
    CHECK(stem(drawn.lines[0], 1) == 9);
    CHECK(stem(drawn.lines[0], 3) == 4);  // after the space, the walk wraps
    CHECK(stem(drawn.lines[1], 0) == 4);
    CHECK(stem(drawn.lines[1], 2) == 12);  // λ is missing and still takes 9
    s.letterSpacing = 3;
    s.text = U"II";
    const auto spaced = renderBigText(s);
    const auto& sc = spaced.drawing.cells.at(spaced.lines[0].left + 8 + 3 + 3, spaced.lines[0].top + 3);
    REQUIRE_FALSE(sc.transparent);
    CHECK(irc::mircColor(sc.fg) == 9);
    // every printable glyph in every colour slot, and every pair of glyphs
    // side by side, passes the lint with iCE on and off
    s.letterSpacing = 0;
    s.bigColours.clear();
    std::u32string all;
    for (char32_t ch = 0x21; ch < 0x7f; ++ch)
        all += ch;
    std::vector<std::u32string> texts;
    for (int slot = 0; slot < 7; ++slot)
        texts.push_back(std::u32string(std::size_t(slot), U' ') + all);
    for (const char32_t a : all) {
        std::u32string pairs;
        for (const char32_t b : all)
            pairs += std::u32string{a, b};
        texts.push_back(pairs);
    }
    for (const auto& text : texts) {
        s.text = text;
        const auto c = renderText(s, blockFace()).cells;
        for (const bool ice : {true, false}) {
            INFO(QString::fromStdU32String(text).toStdString() << " / " << ice);
            CHECK(ladderFailures(c, ice).empty());
        }
    }
}

TEST_CASE("big sources use the compatible project key and reject malformed settings", "[big-text][project]") {
    project::Project p;
    p.layers = LayerStack(80, 25);
    TextSource s;
    s.text = U"BLAZE";
    s.bigStyle = BigTextStyle::Outline;
    const auto id = p.layers.add(makeTextLayer(s, "Title"));
    const auto entries = project::encode(p);
    const auto key = "layers/" + std::to_string(id) + ".json";
    auto object = QJsonDocument::fromJson(QByteArray::fromStdString(entries.at(key))).object();
    const auto original = object;
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(entries.at("manifest.json")))
              .object()
              .value("version")
              .toString() == "1.4");
    CHECK(object.contains("bigSource"));
    CHECK_FALSE(object.contains("source"));
    CHECK_FALSE(object.contains("tdfSource"));
    const auto sourceObject = object.value("bigSource").toObject();
    const auto refused = [&](const QJsonObject& o) {
        auto e = entries;
        e[key] = QJsonDocument(o).toJson().toStdString();
        CHECK_THROWS_AS(project::decode(e), project::ProjectError);
    };
    for (const auto bad : {QJsonValue("rainbow"), QJsonValue("unknown"), QJsonValue(1), QJsonValue()}) {
        auto source = sourceObject;
        source["bigStyle"] = bad;
        object["bigSource"] = source;
        refused(object);
    }
    for (const auto bad : {QJsonValue("7,0"), QJsonValue(QJsonArray{16}), QJsonValue(QJsonArray{-1}),
                           QJsonValue(QJsonArray{1.5}), QJsonValue(QJsonArray{true})}) {
        auto source = sourceObject;
        source["bigColours"] = bad;
        object["bigSource"] = source;
        refused(object);
    }
    auto source = sourceObject;
    QJsonArray tooMany;
    for (int i = 0; i < 17; ++i)
        tooMany.append(4);
    source["bigColours"] = tooMany;
    object["bigSource"] = source;
    refused(object);
    source = sourceObject;
    source["face"] = QJsonObject{{"kind", "pixel"}, {"name", "mini"}};
    object["bigSource"] = source;
    refused(object);
    object = original;
    object["source"] = sourceObject;
    refused(object);
    object = original;
    object["tdfSource"] = sourceObject;
    refused(object);
    object = original;
    object.remove("bigSource");
    object["source"] = sourceObject;
    refused(object);
    // An older reader ignores bigSource and shows the saved cells. Emulate
    // that key being dropped, rather than changing the normal face's size.
    object = original;
    object.remove("bigSource");
    auto old = entries;
    old[key] = QJsonDocument(object).toJson().toStdString();
    const auto dropped = project::decode(old);
    CHECK_FALSE(dropped.layers[0].props.text);
    CHECK(dropped.layers[0].cells == p.layers[0].cells);
}

TEST_CASE("Big rainbow saves under its own key so a 1.4 reader shows the cells", "[big-text][project][rainbow]") {
    project::Project p;
    p.layers = LayerStack(80, 25);
    TextSource s;
    s.text = U"BLAZE";
    s.bigStyle = BigTextStyle::Rainbow;
    const auto id = p.layers.add(makeTextLayer(s, "Title"));
    const auto entries = project::encode(p);
    CHECK(project::decode(entries) == p);
    const auto key = "layers/" + std::to_string(id) + ".json";
    const auto original = QJsonDocument::fromJson(QByteArray::fromStdString(entries.at(key))).object();
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(entries.at("manifest.json")))
              .object()
              .value("version")
              .toString() == "1.6");
    REQUIRE(original.contains("bigRainbowSource"));
    for (const auto other : {"source", "tdfSource", "bigSource"})
        CHECK_FALSE(original.contains(other));
    const auto sourceObject = original.value("bigRainbowSource").toObject();
    CHECK(sourceObject.value("bigStyle").toString() == "rainbow");
    const auto decoded = [&](const QJsonObject& o) {
        auto e = entries;
        e[key] = QJsonDocument(o).toJson().toStdString();
        return project::decode(e);
    };
    // under any other key, or beside another source, it is refused
    for (const auto other : {"source", "tdfSource", "bigSource"}) {
        INFO(other);
        auto moved = original;
        moved.remove("bigRainbowSource");
        moved[other] = sourceObject;
        CHECK_THROWS_AS(decoded(moved), project::ProjectError);
        auto both = original;
        both[other] = sourceObject;
        CHECK_THROWS_AS(decoded(both), project::ProjectError);
    }
    // a non-rainbow big style under bigRainbowSource is refused too
    auto outline = sourceObject;
    outline["bigStyle"] = "outline";
    auto wrong = original;
    wrong["bigRainbowSource"] = outline;
    CHECK_THROWS_AS(decoded(wrong), project::ProjectError);
    // a 1.4 reader skips the key it doesn't know and shows the saved cells
    auto older = original;
    older.remove("bigRainbowSource");
    const auto dropped = decoded(older);
    CHECK_FALSE(dropped.layers[0].props.text);
    CHECK(dropped.layers[0].cells == p.layers[0].cells);
}

TEST_CASE("big text spacing alignment wrapping missing glyphs and effect margins", "[big-text]") {
    TextSource s;
    s.bigStyle = BigTextStyle::Outline;
    s.text = U"BLAZE 420";
    s.wrapWidth = 42;
    auto drawn = renderBigText(s);
    REQUIRE(drawn.lines.size() == 2);
    CHECK(drawn.lines[0].first == 0);
    CHECK(drawn.lines[0].count == 5);
    CHECK(drawn.lines[1].first == 6);
    CHECK(drawn.lines[1].count == 3);
    CHECK(drawn.drawing.cells.width() == 42);
    const int height = drawn.drawing.cells.height();
    s.lineSpacing = 3;
    CHECK(renderBigText(s).drawing.cells.height() == height + 3);
    s.align = TextAlign::Right;
    const auto right = renderBigText(s);
    CHECK(right.lines[1].left == 42 - 24 - 1);
    s.align = TextAlign::Centre;
    const auto centre = renderBigText(s);
    CHECK(centre.lines[1].left == (42 - 24 - 2) / 2 + 1);
    s.text = U"ab";
    s.wrapWidth = 0;
    s.letterSpacing = 2;
    s.textCase = TextCase::Capitals;
    auto capitals = renderText(s, blockFace()).cells;
    s.text = U"AB";
    s.textCase = TextCase::AsTyped;
    CHECK(renderText(s, blockFace()).cells == capitals);
    s.text = U"A\nB";
    CHECK(renderBigText(s).lines.size() == 2);
    s.text = U"AλB";
    CHECK(renderText(s, blockFace()).missing == std::vector<std::size_t>{1});
    s.text = U"420";
    s.letterSpacing = 0;
    s.lineSpacing = 0;
    const auto plain = renderText(s, blockFace()).cells;
    TextStyle plate{{Effect{true, PlateEffect{Blue, 2}}}};
    const auto styled = renderStyledText(s, plate, blockFace()).cells;
    REQUIRE(styled.width() == plain.width() + 4);
    REQUIRE(styled.height() == plain.height() + 4);
    for (int y = 0; y < plain.height(); ++y)
        for (int x = 0; x < plain.width(); ++x)
            if (!plain.at(x, y).transparent)
                CHECK(styled.at(x + 2, y + 2) == plain.at(x, y));
    // A text plate covers the full wrap/alignment box, including its
    // unused columns, rather than only the letters' ink bounds.
    s.wrapWidth = 80;
    const auto wrappedPlate = renderStyledText(s, plate, blockFace()).cells;
    REQUIRE(wrappedPlate.width() == 84);
    for (int y = 0; y < wrappedPlate.height(); ++y) {
        CHECK_FALSE(wrappedPlate.at(0,y).transparent);
        CHECK_FALSE(wrappedPlate.at(83,y).transparent);
    }
}

TEST_CASE("big sources keep effects with the source for older reader fallback", "[big-text][project]") {
    const auto root = std::filesystem::path(BW_TEST_TMP) / "big-effects";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    for (const auto mode : {BigTextStyle::Shadow, BigTextStyle::ThreeD, BigTextStyle::Outline, BigTextStyle::Block,
                            BigTextStyle::Rainbow})
        for (const bool ice : {true, false}) {
            const QString bigKey = mode == BigTextStyle::Rainbow ? "bigRainbowSource" : "bigSource";
            project::Project p;
            p.layers = LayerStack(80, 25);
            p.ice = ice;
            TextSource s;
            s.text = U"420";
            s.bigStyle = mode;
            const auto id = p.layers.add(makeTextLayer(s, "Title"));
            const TextStyle style{{Effect{true, PlateEffect{Blue, 1}}}};
            auto& layer = *p.layers.find(id);
            applyEdit(layer, styleEdit(layer, style, ice));
            const auto entries = project::encode(p);
            CHECK(project::decode(entries) == p);
            const auto key = "layers/" + std::to_string(id) + ".json";
            auto object = QJsonDocument::fromJson(QByteArray::fromStdString(entries.at(key))).object();
            REQUIRE_FALSE(object.contains("style"));
            REQUIRE(object.value(bigKey).toObject().contains("style"));
            auto invalid = entries;
            auto misplaced = object;
            misplaced["style"] = object.value(bigKey).toObject().value("style");
            invalid[key] = QJsonDocument(misplaced).toJson().toStdString();
            CHECK_THROWS_AS(project::decode(invalid), project::ProjectError);
            // Model the old reader's unknown-key skip for both properties.
            object.remove(bigKey);
            auto older = entries;
            older[key] = QJsonDocument(object).toJson().toStdString();
            const auto dropped = project::decode(older);
            CHECK_FALSE(dropped.layers[0].props.text);
            CHECK_FALSE(dropped.layers[0].props.style);
            CHECK(dropped.layers[0].cells == layer.cells);
            project::writeZip(p, root / "styled.bwp");
            project::writeDirectory(p, root / "styled");
            for (const auto path : {root / "styled.bwp", root / "styled"}) {
                auto back = project::read(path);
                CHECK(back == p);
                auto& l = back.layers[0];
                s.text = U"BLAZE";
                const auto edit = textEdit(l, s, 0, ice);
                applyEdit(l, edit);
                CHECK(l.cells == renderStyledText(s, style, blockFace(), ice).cells);
                applyEdit(l, edit, true);
                CHECK(l == layer);
                const auto bake = bakeEdit(l, ice);
                applyEdit(l, bake);
                CHECK(l.cells == layer.cells);
                CHECK_FALSE(l.props.text);
                CHECK_FALSE(l.props.style);
                applyEdit(l, bake, true);
                CHECK(l == layer);
            }
        }
    std::filesystem::remove_all(root);
}
