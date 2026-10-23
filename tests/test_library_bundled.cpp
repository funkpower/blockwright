// The bundled Library (PLAN §12 "What ships"): every stamp in the repo's
// library/ reads and checks clean, fits one IRC line, is signed funkpower,
// sits in library/<category>/, has a shape of its own and can be found.

#include "blockwright/irc.h"
#include "blockwright/library.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace bw;
using namespace bw::library;
namespace fs = std::filesystem;

namespace {

const fs::path kBundled = BW_BUNDLED_LIBRARY;

bool blankAt(const Canvas& c, int x, int y) { return isBlank(c.at(x, y)); }

// Whether a frame's middle is open: its centre cell is blank, and the blank
// cells joined to the centre (4-connected) stay off the outer ring and are at
// least half of the cells inside it. A filled box with a pinhole fails, and
// so does a frame whose "opening" leaks out through a gap in its ring.
bool hasOpening(const Canvas& c) {
    const int w = c.width(), h = c.height();
    if (w < 3 || h < 3 || !blankAt(c, w / 2, h / 2))
        return false;
    std::vector<char> seen(static_cast<size_t>(w) * h, 0);
    std::vector<std::pair<int, int>> todo{{w / 2, h / 2}};
    seen[static_cast<size_t>(h / 2) * w + w / 2] = 1;
    int open = 0;
    while (!todo.empty()) {
        const auto [x, y] = todo.back();
        todo.pop_back();
        if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
            return false;
        ++open;
        for (const auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            char& s = seen[static_cast<size_t>(ny) * w + nx];
            if (!s && blankAt(c, nx, ny)) {
                s = 1;
                todo.push_back({nx, ny});
            }
        }
    }
    return 2 * open >= (w - 2) * (h - 2);
}

// Whether every drawn cell shows black on black: a silhouette, which looks
// like nothing on the default canvas or in a thumbnail.
bool allBlack(const Stamp& s) {
    const Canvas& c = s.art.canvas;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            if (isBlank(c.at(x, y)))
                continue;
            const Cell v = iceCell(c.at(x, y), s.art.ice);
            const bool lit = v.ch == U' ' || v.ch == U'\0' ? v.bg.paletteIndex() != 0
                                                             : v.fg.paletteIndex() != 0 || v.bg.paletteIndex() != 0;
            if (lit)
                return false;
        }
    return true;
}

Canvas grid(const std::vector<std::string>& rows) {
    Canvas c(static_cast<int>(rows[0].size()), static_cast<int>(rows.size()));
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (rows[y][x] != '.') {
                c.at(x, y).ch = U'X';
                c.at(x, y).fg = Color::indexed(Grey);
            }
    return c;
}

} // namespace

TEST_CASE("the bundled library checks clean and holds the first set") {
    const ScanResult r = scan(kBundled);
    for (const Problem& p : r.problems)
        UNSCOPED_INFO(p.path.string() << ": " << p.message);
    CHECK(r.problems.empty());
    CHECK(r.stamps.size() >= 20);
    const std::set<std::string> expected{"buildings", "fire", "frames", "land", "smoke", "sky", "trees", "water"};
    const std::vector<std::string> cats = categories(r.stamps);
    CHECK(std::set<std::string>(cats.begin(), cats.end()) == expected);
}

TEST_CASE("every bundled stamp's widest IRC row is under the budget") {
    for (const Stamp& s : scan(kBundled).stamps) {
        INFO(s.path.string());
        CHECK(s.meta.ircBytes == widestRowBytes(s.art.canvas));
        CHECK(s.meta.ircBytes < irc::kEfnetWeechatBudget);
    }
}

TEST_CASE("every bundled stamp is signed and filed under its category") {
    for (const Stamp& s : scan(kBundled).stamps) {
        INFO(s.path.string());
        REQUIRE(s.art.sauce.has_value());
        CHECK(s.art.sauce->author == "funkpower");
        CHECK(s.name.size() <= Sauce::kTitleLen);
        CHECK(s.path.parent_path().filename().string() == s.meta.category);
        CHECK(s.path.stem().string() == slug(s.path.stem().string()));
    }
}

// The shape rule. A stamp is trimmed to its part: its first and last row and
// column each hold a drawn (non-blank) cell, so it lands where it is dropped
// with no empty margin. Then either
//  - it is a frame (category "frames"): its middle is open (hasOpening), so
//    what it is placed around shows through; or
//  - it is any other part: its outer ring holds at least one blank cell, so
//    it has an outline of its own and is not a filled rectangle that would
//    cover the scene it is dropped on with its own background.
TEST_CASE("every bundled stamp is trimmed and has a transparent border, or is a frame") {
    for (const Stamp& s : scan(kBundled).stamps) {
        INFO(s.path.string());
        const Canvas& c = s.art.canvas;
        const int w = c.width(), h = c.height();
        REQUIRE(w >= 1);
        REQUIRE(h >= 1);
        bool top = false, bottom = false, left = false, right = false, ringBlank = false;
        for (int x = 0; x < w; ++x) {
            top |= !blankAt(c, x, 0);
            bottom |= !blankAt(c, x, h - 1);
            ringBlank |= blankAt(c, x, 0) || blankAt(c, x, h - 1);
        }
        for (int y = 0; y < h; ++y) {
            left |= !blankAt(c, 0, y);
            right |= !blankAt(c, w - 1, y);
            ringBlank |= blankAt(c, 0, y) || blankAt(c, w - 1, y);
        }
        CHECK(top);
        CHECK(bottom);
        CHECK(left);
        CHECK(right);
        if (s.meta.category == "frames") {
            CHECK(hasOpening(c));
        } else {
            CHECK(ringBlank);
        }
    }
}

TEST_CASE("the frame rule wants a real opening, not a pinhole") {
    // A ring one cell thick: open.
    CHECK(hasOpening(grid({"XXXXX", "X...X", "X...X", "X...X", "XXXXX"})));
    // The hazard frame's shape: a thick ring with a wide middle.
    CHECK(hasOpening(grid({"XXXXXXXX", "XX....XX", "XX....XX", "XXXXXXXX"})));
    // A filled box with one blank cell off centre (the round-1 counterexample).
    CHECK_FALSE(hasOpening(grid({"XXXXX", "X.XXX", "XXXXX", "XXXXX", "XXXXX"})));
    // A filled box with a blank centre and nothing else.
    CHECK_FALSE(hasOpening(grid({"XXXXXXX", "XXXXXXX", "XXXXXXX", "XXX.XXX", "XXXXXXX", "XXXXXXX", "XXXXXXX"})));
    // A ring with a gap: the opening leaks to the edge.
    CHECK_FALSE(hasOpening(grid({"XXXXX", "X...X", "X....", "X...X", "XXXXX"})));
    // Too small to have a middle.
    CHECK_FALSE(hasOpening(grid({"XX", "XX"})));
}

// A stamp drawn only in black is a silhouette for a lit sky: on the default
// black canvas and in its thumbnail it shows nothing, so the dock's search is
// how it is found.
TEST_CASE("every all-black bundled stamp is tagged silhouette and dark") {
    int silhouettes = 0;
    for (const Stamp& s : scan(kBundled).stamps) {
        INFO(s.path.string());
        if (!allBlack(s))
            continue;
        ++silhouettes;
        CHECK(matches(s, "silhouette dark"));
    }
    CHECK(silhouettes >= 1);  // the palm
    const ScanResult r = scan(kBundled);
    const auto palm = std::find_if(r.stamps.begin(), r.stamps.end(),
                                   [](const Stamp& s) { return s.path.stem() == "palm-silhouette"; });
    REQUIRE(palm != r.stamps.end());
    CHECK(allBlack(*palm));
    CHECK(matches(*palm, "dark"));
}
