// Cell oracles: ascii.py 1.32.1 render_pixel, run headlessly from a scratch
// copy with grid_to_irc replaced by a cell capture (after strip_rows), width
// 78. Each hex digit is the reference background's mIRC colour; dot is CLEAR.
// The bundled cases use the thinned colours listed in PLAN §15. The glyph
// oracle is that copy's PIXEL_FONT; mini-reference uses render_mini/parse_irc.
#include "blockwright/big_text.h"
#include "blockwright/irc.h"
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
        if (o.value("style").toString() == "rainbow")
            continue;
        const auto s = source(o);
        INFO(bigTextStyleName(s.bigStyle) << " / " << o.value("text").toString().toStdString());
        REQUIRE_FALSE(textSourceProblem(s));
        const auto result = renderText(s, blockFace());
        CHECK(result.missing.empty());
        CHECK(tight(visible(result.cells)) == expected(o));
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
    CHECK(drawings == 76);
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
        if (o.value("style").toString() == "rainbow")
            continue;
        if (!o.value("bundled").toBool())
            continue;
        const auto s = source(o);
        for (const bool ice : {true, false}) {
            INFO(bigTextStyleName(s.bigStyle)
                 << " / " << o.value("text").toString().toStdString() << " / " << ice);
            const auto c = renderStyledText(s, {}, blockFace(), ice).cells;
            CHECK(tight(visible(c)) == expected(o));
            CHECK(c.width() <= 80);
            lint::LintOptions options;
            options.ramps = namedRamps();
            options.craftPairs = true;
            options.ice = ice;
            for (const auto& f : lint::lintCanvas(c, options).findings)
                if (lint::ladderKind(f.kind)) {
                    INFO(f.message << " at " << f.x << "," << f.y << " on " << f.ramp);
                    CHECK_FALSE(lint::ladderKind(f.kind));
                }
            for (const auto& ramp : namedRamps()) {
                lint::LadderOptions ladder;
                ladder.fallback = true;
                ladder.ice = ice;
                for (const auto& f : lint::checkLadder(c, ramp, {}, ladder).findings)
                    CHECK(f.kind == lint::LadderFinding::Kind::Noise);
            }
            for (const auto& line : irc::encodeCanvas(c))
                CHECK(line.size() <= irc::kEfnetWeechatBudget);
            ++drawings;
        }
    }
    CHECK(drawings == 72);
}

TEST_CASE("big text project round trips edits undo redo and bake", "[big-text][project]") {
    namespace fs = std::filesystem;
    const auto root = fs::path(BW_TEST_TMP) / "big-text";
    fs::remove_all(root);
    fs::create_directories(root);
    int cases = 0;
    for (const auto v : fixtures("big-styles.json")) {
        if (v.toObject().value("style").toString() == "rainbow")
            continue;
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
    CHECK(cases == 152);
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
    CHECK_FALSE(bigTextStyleFromName("rainbow"));
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
    for (const auto mode :
         {BigTextStyle::Shadow, BigTextStyle::ThreeD, BigTextStyle::Outline, BigTextStyle::Block})
        for (const bool ice : {true, false}) {
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
            REQUIRE(object.value("bigSource").toObject().contains("style"));
            auto invalid = entries;
            auto misplaced = object;
            misplaced["style"] = object.value("bigSource").toObject().value("style");
            invalid[key] = QJsonDocument(misplaced).toJson().toStdString();
            CHECK_THROWS_AS(project::decode(invalid), project::ProjectError);
            // Model the old reader's unknown-key skip for both properties.
            object.remove("bigSource");
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
