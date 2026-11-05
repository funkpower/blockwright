// Frame styles (frame.h): the nine-slice drawn to a size, its JSON form, and
// the bundled styles, held to the bar the Library's stamps are.

#include "blockwright/frame.h"
#include "blockwright/ansi.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/library.h"

#include "../cli/lint.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace bw;

namespace {

std::u32string rowText(const Canvas& c, int y) {
    std::u32string s;
    for (int x = 0; x < c.width(); ++x)
        s += c.at(x, y).transparent ? U'.' : c.at(x, y).ch.codePoint().value_or(U'?');
    return s;
}

std::u32string colText(const Canvas& c, int x) {
    std::u32string s;
    for (int y = 0; y < c.height(); ++y)
        s += c.at(x, y).transparent ? U'.' : c.at(x, y).ch.codePoint().value_or(U'?');
    return s;
}

const FrameStyle& bundled(const char* id) {
    const FrameStyle* s = findFrameStyle(id);
    REQUIRE(s);
    return *s;
}

// The frame as it shows on an empty layer: a transparent cell is a blank.
Canvas shown(Canvas c) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (c.at(x, y).transparent)
                c.at(x, y) = Cell{};
    return c;
}

} // namespace

TEST_CASE("a frame at its minimum size is its corners") {
    const FrameStyle& s = bundled("single");
    CHECK(s.minimumWidth() == 2);
    CHECK(s.minimumHeight() == 2);
    const auto f = drawFrame(s, 2, 2);
    REQUIRE(f);
    CHECK(rowText(*f, 0) == U"┌┐");
    CHECK(rowText(*f, 1) == U"└┘");

    // three deep on the sides, two on the top and bottom
    const FrameStyle& shaded = bundled("shaded");
    CHECK(shaded.minimumWidth() == 6);
    CHECK(shaded.minimumHeight() == 4);
    const auto g = drawFrame(shaded, 6, 4);
    REQUIRE(g);
    CHECK(rowText(*g, 0) == U"▓▓▓▓▓▓");
    CHECK(rowText(*g, 1) == U"▓▒▒▒▒▓");
    CHECK(rowText(*g, 2) == U"▓▒▒▒▒▓");
    CHECK(rowText(*g, 3) == U"▓▓▓▓▓▓");
}

TEST_CASE("a frame at odd and even sizes keeps its corners and repeats its edges") {
    const FrameStyle& s = bundled("double");
    for (const auto& [w, h] : {std::pair{7, 5}, {8, 4}, {3, 3}, {10, 9}}) {
        INFO(w << "x" << h);
        const auto f = drawFrame(s, w, h);
        REQUIRE(f);
        REQUIRE(f->width() == w);
        REQUIRE(f->height() == h);
        CHECK(rowText(*f, 0) == U"╔" + std::u32string(w - 2, U'═') + U"╗");
        CHECK(rowText(*f, h - 1) == U"╚" + std::u32string(w - 2, U'═') + U"╝");
        for (int y = 1; y < h - 1; ++y)
            CHECK(rowText(*f, y) == U"║" + std::u32string(w - 2, U'.') + U"║");
    }
}

TEST_CASE("a multi-cell edge keeps its depth at every size") {
    const FrameStyle& s = bundled("shaded");
    for (int w = 6; w <= 13; ++w)
        for (int h = 4; h <= 9; ++h) {
            INFO(w << "x" << h);
            const auto f = drawFrame(s, w, h);
            REQUIRE(f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int down = std::min(y, h - 1 - y);
                    const int depth = std::min({x, w - 1 - x, down < 2 ? down : 3});
                    const Cell& c = f->at(x, y);
                    if (depth >= 3) {
                        CHECK(c.transparent);
                        continue;
                    }
                    CHECK_FALSE(c.transparent);
                    CHECK(c.ch == Glyph(depth == 0 ? U'▓' : depth == 1 ? U'▒' : U'░'));
                }
        }
}

TEST_CASE("an edge's pattern repeats from its corner at every length") {
    SECTION("one cell deep") {
        const FrameStyle& s = bundled("dashed");
        const std::u32string dash = U"── ";
        for (int w = 2; w <= 24; ++w) {
            INFO("width " << w);
            const auto f = drawFrame(s, w, 3);
            REQUIRE(f);
            std::u32string want = U"┌";
            for (int i = 0; i < w - 2; ++i)
                want += dash[i % 3];
            CHECK(rowText(*f, 0) == want + U"┐");
            CHECK(colText(*f, 0) == U"┌│└");
        }
        for (int h = 2; h <= 12; ++h) {
            INFO("height " << h);
            const auto f = drawFrame(s, 4, h);
            REQUIRE(f);
            CHECK(colText(*f, 3) == U"┐" + std::u32string(h - 2, U'│') + U"┘");
        }
    }
    SECTION("two cells deep, the stripes running on from the top left") {
        const FrameStyle& s = bundled("hazard");
        for (int w = 4; w <= 21; ++w)
            for (int h = 4; h <= 11; ++h) {
                INFO(w << "x" << h);
                const auto f = drawFrame(s, w, h);
                REQUIRE(f);
                // the top edge and the left edge, from the source's top left
                for (int x = 0; x < w - 2; ++x)
                    for (int y = 0; y < 2; ++y) {
                        const int sx = x < 2 ? x : 2 + (x - 2) % 8;
                        CHECK(f->at(x, y) == s.source.at(sx, y));
                    }
                for (int y = 0; y < h - 2; ++y)
                    for (int x = 0; x < 2; ++x) {
                        const int sy = y < 2 ? y : 2 + (y - 2) % 4;
                        CHECK(f->at(x, y) == s.source.at(x, sy));
                    }
                // the corners as they are, solid yellow, so every edge meets
                // them in step at whatever phase it is cut
                for (int y = 0; y < 2; ++y)
                    for (int x = 0; x < 2; ++x) {
                        for (const auto& [cx, cy] : {std::pair{x, y}, {w - 2 + x, y}, {x, h - 2 + y},
                                                     {w - 2 + x, h - 2 + y}}) {
                            CHECK(f->at(cx, cy).ch == Glyph(U' '));
                            CHECK(f->at(cx, cy).bg == Color::indexed(Yellow));
                        }
                        CHECK(f->at(w - 2 + x, y) == s.source.at(10 + x, y));
                        CHECK(f->at(x, h - 2 + y) == s.source.at(x, 6 + y));
                        CHECK(f->at(w - 2 + x, h - 2 + y) == s.source.at(10 + x, 6 + y));
                    }
            }
    }
}

TEST_CASE("below its minimum a frame isn't drawn") {
    CHECK_FALSE(drawFrame(bundled("single"), 1, 5));
    CHECK_FALSE(drawFrame(bundled("single"), 5, 1));
    CHECK_FALSE(drawFrame(bundled("single"), 0, 0));
    CHECK_FALSE(drawFrame(bundled("shaded"), 5, 9));
    CHECK_FALSE(drawFrame(bundled("shaded"), 9, 3));
    CHECK_FALSE(drawFrame(bundled("hazard"), 9, 3));
    FrameStyle s = bundled("single");
    s.minWidth = 6;
    s.minHeight = 4;
    CHECK(s.minimumWidth() == 6);
    CHECK(s.minimumHeight() == 4);
    CHECK_FALSE(drawFrame(s, 5, 4));
    CHECK_FALSE(drawFrame(s, 6, 3));
    CHECK(drawFrame(s, 6, 4));
    // a style whose insets leave no edge isn't drawn at any size
    FrameStyle bad = bundled("single");
    bad.left = 2;
    CHECK_FALSE(bad.valid());
    CHECK_FALSE(drawFrame(bad, 10, 10));
}

TEST_CASE("a frame larger than a canvas isn't drawn, and nothing is allocated for it") {
    // each of these is refused before the canvas is made: drawn, 2000000000
    // x 5 would be ten billion cells
    const FrameStyle& s = bundled("single");
    CHECK_FALSE(drawFrame(s, 2000000000, 5));
    CHECK_FALSE(drawFrame(s, 5, 2000000000));
    CHECK_FALSE(drawFrame(s, kMaxColumns + 1, 5));
    CHECK_FALSE(drawFrame(s, 5, kMaxRows + 1));
    const auto widest = drawFrame(s, kMaxColumns, 3);
    REQUIRE(widest);
    CHECK(widest->width() == kMaxColumns);
    // a minimum past the limit makes the style invalid, so no size draws it
    FrameStyle huge = s;
    huge.minWidth = 2000000000;
    huge.minHeight = 5;
    CHECK_FALSE(huge.valid());
    CHECK_FALSE(drawFrame(huge, 2000000000, 5));
    huge.minWidth = kMaxColumns;
    CHECK(huge.valid());
}

TEST_CASE("line, half-block and shaded cells keep the background; fills and stripes don't") {
    const std::vector<std::string> keep{"single", "double", "double-across", "double-down", "heavy",
                                        "half-block", "shaded", "dashed"};
    for (const FrameStyle& s : bundledFrameStyles()) {
        INFO(s.id);
        const bool keeps = std::find(keep.begin(), keep.end(), s.id) != keep.end();
        // every drawn cell keeps it or none does, and a cell that keeps it
        // is drawn
        std::vector<bool> mask;
        const auto f = drawFrame(s, 17, 9, &mask);
        REQUIRE(f);
        REQUIRE(mask.size() == std::size_t(17 * 9));
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 17; ++x) {
                INFO(x << "," << y);
                CHECK(mask[std::size_t(y * 17 + x)] == (keeps && !f->at(x, y).transparent));
            }
    }
    // the dashed line's gaps keep it too
    std::vector<bool> mask;
    const auto d = drawFrame(bundled("dashed"), 8, 3, &mask);
    REQUIRE(d);
    REQUIRE(d->at(3, 0).ch == Glyph(U' '));
    CHECK(mask[3]);
}

TEST_CASE("a frame's fill is its centre repeated, or transparent") {
    const auto open = drawFrame(bundled("single"), 6, 4);
    REQUIRE(open);
    CHECK(open->at(2, 2).transparent);
    const FrameStyle& button = bundled("button");
    REQUIRE(button.fill);
    const auto filled = drawFrame(button, 9, 5);
    REQUIRE(filled);
    for (int y = 1; y < 4; ++y)
        for (int x = 1; x < 8; ++x) {
            CHECK_FALSE(filled->at(x, y).transparent);
            CHECK(filled->at(x, y) == button.source.at(1, 1));
        }
}

TEST_CASE("recolouring a frame to the pen keeps its glyphs and its transparent cells") {
    const auto f = drawFrame(bundled("bevel"), 6, 4);
    REQUIRE(f);
    const Canvas r = recolourFrame(*f, Color::indexed(LightRed), Color::indexed(Blue));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x) {
            CHECK(r.at(x, y).ch == f->at(x, y).ch);
            CHECK(r.at(x, y).transparent == f->at(x, y).transparent);
            if (!r.at(x, y).transparent) {
                CHECK(r.at(x, y).fg == Color::indexed(LightRed));
                CHECK(r.at(x, y).bg == Color::indexed(Blue));
            }
        }
}

TEST_CASE("a frame style's JSON reads back as the same style") {
    for (const FrameStyle& s : bundledFrameStyles()) {
        INFO(s.id);
        std::string error;
        const auto back = frameStyleFromJson(frameStyleJson(s), &error);
        INFO(error);
        REQUIRE(back);
        CHECK(back->id == s.id);
        CHECK(back->name == s.name);
        CHECK(back->left == s.left);
        CHECK(back->right == s.right);
        CHECK(back->top == s.top);
        CHECK(back->bottom == s.bottom);
        CHECK(back->fill == s.fill);
        CHECK(back->minWidth == s.minWidth);
        CHECK(back->minHeight == s.minHeight);
        CHECK(back->keepBg == s.keepBg);
        REQUIRE(back->source.width() == s.source.width());
        REQUIRE(back->source.height() == s.source.height());
        for (int y = 0; y < s.source.height(); ++y)
            for (int x = 0; x < s.source.width(); ++x)
                CHECK(back->source.at(x, y) == s.source.at(x, y));
    }
}

TEST_CASE("a frame style's JSON is refused when it isn't one") {
    const std::string good =
        R"({"id": "box", "insets": [1, 1, 1, 1], "rows": ["+-+", "| |", "+-+"], "fg": ["777", "777", "777"],)"
        R"( "bg": ["000", "0.0", "000"]})";
    std::string error;
    const auto s = frameStyleFromJson(good, &error);
    INFO(error);
    REQUIRE(s);
    CHECK(s->name == "box");
    CHECK_FALSE(s->fill);
    CHECK(s->source.at(1, 1).transparent);
    CHECK_FALSE(s->source.at(0, 1).transparent);
    // no "keepBg": no cell keeps the background
    CHECK(s->keepBg.empty());
    CHECK_FALSE(s->keepsBackground(0, 0));
    {
        std::string why;
        const auto k = frameStyleFromJson(
            good.substr(0, good.size() - 1) + R"(, "keepBg": ["111", "1.1", "1.."]})", &why);
        INFO(why);
        REQUIRE(k);
        CHECK(k->keepsBackground(0, 0));
        CHECK_FALSE(k->keepsBackground(1, 1));
        CHECK_FALSE(k->keepsBackground(2, 2));
        CHECK(frameStyleFromJson(frameStyleJson(*k))->keepBg == k->keepBg);
    }

    const auto refused = [](const std::string& json) {
        std::string why;
        const bool no = !frameStyleFromJson(json, &why);
        INFO(json);
        CHECK_FALSE(why.empty());
        return no;
    };
    const auto with = [&](const std::string& from, const std::string& to) {
        std::string j = good;
        j.replace(j.find(from), from.size(), to);
        return j;
    };
    CHECK(refused("[]"));
    CHECK(refused("not json"));
    CHECK(refused(with(R"("id": "box")", R"("id": "box", "tags": [])")));
    CHECK(refused(with(R"("id": "box")", R"("id": "")")));
    CHECK(refused(with("[1, 1, 1, 1]", "[1, 1, 1]")));
    CHECK(refused(with("[1, 1, 1, 1]", "[1, 2, 1, 1]")));
    CHECK(refused(with("[1, 1, 1, 1]", "[1, 1, -1, 1]")));
    CHECK(refused(with(R"("| |")", R"("|  |")")));
    CHECK(refused(with(R"("| |")", R"("|中|")")));
    CHECK(refused(with(R"("0.0")", R"("0x0")")));
    CHECK(refused(with(R"(["777", "777", "777"])", R"(["777", "777"])")));
    // a source with no cells
    CHECK(refused(R"({"id": "a", "insets": [0, 0, 0, 0], "rows": [""], "fg": [""], "bg": [""]})"));
    // a minimum, or a source, larger than a canvas
    CHECK(refused(with(R"("id": "box")", R"("id": "box", "min": [2000000000, 5])")));
    CHECK(refused(with(R"("id": "box")", R"("id": "box", "min": [5, 2000000000])")));
    CHECK(refused(with(R"("id": "box")", R"("id": "box", "min": [401, 5])")));
    CHECK(frameStyleFromJson(with(R"("id": "box")", R"("id": "box", "min": [400, 5000])")));
    {
        const std::string wide(kMaxColumns + 1, '-'), digits(kMaxColumns + 1, '7');
        CHECK(refused(R"({"id": "a", "insets": [1, 1, 0, 0], "rows": [")" + wide + R"("], "fg": [")" + digits +
                      R"("], "bg": [")" + digits + R"("]})"));
    }
    // a "keepBg" that doesn't match the rows, or says something else
    const std::string tail = good.substr(0, good.size() - 1);
    CHECK(refused(tail + R"(, "keepBg": ["111", "1.1"]})"));
    CHECK(refused(tail + R"(, "keepBg": ["111", "1.1", "11"]})"));
    CHECK(refused(tail + R"(, "keepBg": ["111", "1x1", "111"]})"));
    CHECK(refused(tail + R"(, "keepBg": "111"})"));
}

TEST_CASE("every bundled frame style lints clean and fits one IRC line a row at every size") {
    // Every width and height from the style's minimum up to 40x14, and
    // 80x24. The longest repeat is hazard's, eight across and four down, so
    // that covers every phase the far edges can be cut at against every
    // corner, many times over.
    REQUIRE(bundledFrameStyles().size() >= 10);
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    for (const FrameStyle& s : bundledFrameStyles()) {
        REQUIRE(s.valid());
        CHECK_FALSE(s.name.empty());
        std::vector<std::pair<int, int>> sizes;
        for (int w = s.minimumWidth(); w <= 40; ++w)
            for (int h = s.minimumHeight(); h <= 14; ++h)
                sizes.emplace_back(w, h);
        sizes.emplace_back(80, 24);
        for (const auto& [w, h] : sizes) {
            const auto f = drawFrame(s, w, h);
            REQUIRE(f);
            AnsiDocument doc;
            doc.canvas = shown(*f);
            const lint::LintReport r = bwcli::lintDocument(doc, {});
            ++frames;
            if (!r.findings.empty() ||
                !irc::txtOverBudget(irc::encodeCanvas(doc.canvas)).empty()) {
                INFO(s.id << " at " << w << "x" << h);
                INFO(bwcli::lintText(r));
                CHECK(r.findings.empty());
                CHECK(irc::txtOverBudget(irc::encodeCanvas(doc.canvas)).empty());
            }
        }
    }
    CHECK(frames > 5000);
    WARN(frames << " frames in "
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s");
}
