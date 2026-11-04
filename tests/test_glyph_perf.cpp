// Timings for the cell glyph (PLAN §16, Storage): the composite and
// renderCanvas of a 400x2000 canvas, the parse of a large UTF-8 .ans with and
// without combining marks and with and without pairs and clusters, and the
// cluster pool's lookup. Hidden ([.perf]);
// run in a RelWithDebInfo or Release tree with
// `blockwright-render-tests "[perf]"` and compare against the same build of
// the commit before.

#include "blockwright/ansi.h"
#include "blockwright/composite.h"
#include "blockwright/cp437.h"
#include "blockwright/glyph.h"
#include "blockwright/layer.h"
#include "blockwright/render.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace bw;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// A 400x2000 canvas of shades and letters in every colour.
Canvas busyCanvas() {
    Canvas c(400, 2000);
    const char32_t glyphs[] = {U'░', U'▒', U'▓', U'█', U'▀', U'▄', U'A', U'z', U' ', U'╬'};
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            Cell& cell = c.at(x, y);
            cell.ch = glyphs[(x * 7 + y * 3) % 10];
            cell.fg = Color::indexed(static_cast<std::uint8_t>((x + y) % 16));
            cell.bg = Color::indexed(static_cast<std::uint8_t>((x * 3 + y) % 8));
        }
    return c;
}

// A UTF-8 .ans of `rows` rows of 80 cells: shades, box drawing and letters,
// a colour change every few cells, and with `marks` a combining acute after
// every fourth letter.
std::string utf8Ans(int rows, bool marks) {
    std::string out;
    const char32_t glyphs[] = {U'░', U'▒', U'▓', U'█', U'e', U'é', U'中', U'─'};
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < 80; ++x) {
            if (x % 5 == 0)
                out += "\x1b[" + std::to_string(30 + (x + y) % 8) + ";" + std::to_string(40 + (x * 3 + y) % 8) + "m";
            appendUtf8(out, glyphs[(x + y) % 8]);
            if (marks && x % 4 == 0)
                appendUtf8(out, U'́');
        }
        out += "\x1b[0m\r\n";
    }
    return out;
}

} // namespace

TEST_CASE("timing: composite and renderCanvas of 400x2000", "[.perf]") {
    LayerStack s(400, 2000);
    Layer bottom = makeLayer(LayerKind::Cells, 400, 2000);
    bottom.cells = busyCanvas();
    s.add(std::move(bottom));
    Layer top = makeLayer(LayerKind::Cells, 300, 1500, {}, 50, 250);
    for (int y = 0; y < top.height(); ++y)
        for (int x = 0; x < top.width(); ++x)
            if ((x + y) % 3 == 0)
                top.cells.at(x, y) = Cell{U'▄', Color::indexed(12), Color::indexed(1)};
    s.add(std::move(top));

    Canvas out = composite(s);
    constexpr int kComposites = 10;
    auto t0 = Clock::now();
    for (int i = 0; i < kComposites; ++i)
        out = composite(s);
    const double comp = msSince(t0) / kComposites;

    constexpr int kRenders = 5;
    QImage img;
    t0 = Clock::now();
    for (int i = 0; i < kRenders; ++i)
        img = renderCanvas(out, 8);
    const double render = msSince(t0) / kRenders;
    std::printf("perf: composite 400x2000, 2 layers: %.2f ms; renderCanvas 400x2000: %.2f ms\n", comp, render);
    CHECK(img.width() == 400 * 8);
}

TEST_CASE("timing: parsing a large UTF-8 .ans with and without combining marks", "[.perf]") {
    for (const bool marks : {false, true}) {
        const std::string file = utf8Ans(2400, marks);
        ParseOptions opts;
        opts.encoding = TextEncoding::Utf8;
        AnsiDocument doc = parseAnsi(file, opts);
        constexpr int kParses = 5;
        const auto t0 = Clock::now();
        for (int i = 0; i < kParses; ++i)
            doc = parseAnsi(file, opts);
        const double ms = msSince(t0) / kParses;
        std::printf("perf: parse UTF-8 .ans, 80x2400, %s: %.2f ms (%zu bytes)\n",
                    marks ? "with marks" : "without marks", ms, file.size());
        CHECK(doc.canvas.height() >= 2400);
    }
}

// A UTF-8 .ans of `rows` rows of 80 columns. With `clusters`, each 10
// columns hold a pair (中), a narrow cluster (e U+0301), a wide one (👨
// U+200D), another pair (👩), one of three code points (a U+0301 U+0302) and
// two shades; without, 10 shades and letters. A colour change every 10.
std::string clusterAns(int rows, bool clusters) {
    std::string out;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < 80; x += 10) {
            out += "\x1b[" + std::to_string(30 + (x / 10 + y) % 8) + ";" + std::to_string(40 + (x / 10 * 3 + y) % 8) + "m";
            out += clusters ? "中e\u0301░👨\u200D👩a\u0301\u0302▓" : "░▒▓█eé─░▒▓";
        }
        out += "\x1b[0m\r\n";
    }
    return out;
}

TEST_CASE("timing: parsing a large UTF-8 .ans with and without pairs and clusters", "[.perf]") {
    for (const bool clusters : {false, true}) {
        const std::string file = clusterAns(2400, clusters);
        ParseOptions opts;
        opts.encoding = TextEncoding::Utf8;
        AnsiDocument doc = parseAnsi(file, opts);
        constexpr int kParses = 5;
        const auto t0 = Clock::now();
        for (int i = 0; i < kParses; ++i)
            doc = parseAnsi(file, opts);
        const double ms = msSince(t0) / kParses;
        std::printf("perf: parse UTF-8 .ans, 80x2400, %s: %.2f ms (%zu bytes)\n",
                    clusters ? "with pairs and clusters" : "without", ms, file.size());
        CHECK(doc.canvas.height() >= 2400);
    }
}

TEST_CASE("timing: the cluster pool's lookup", "[.perf]") {
    // a pool of its own, so the shared one stays as the other tests left it
    ClusterPool pool;
    std::vector<Glyph> ids;
    constexpr int kClusters = 50000;
    for (int i = 0; i < kClusters; ++i) {
        const std::vector<char32_t> cps{U'a' + char32_t(i % 26), 0x300 + char32_t(i / 26 % 112),
                                        0x300 + char32_t(i / (26 * 112))};
        ids.push_back(pool.intern(cps, PoolCap::Uncapped).value());
    }
    constexpr int kRounds = 40;
    std::uint64_t sum = 0;
    const auto t0 = Clock::now();
    for (int r = 0; r < kRounds; ++r)
        for (int i = 0; i < kClusters; ++i) {
            const Glyph g = ids[std::size_t((i * 7919) % kClusters)];
            sum += pool.codePoints(g)[1] + std::uint64_t(pool.width(g));
        }
    const double ns = msSince(t0) * 1e6 / (double(kRounds) * kClusters);
    std::printf("perf: cluster pool, codePoints + width of one of %d clusters: %.1f ns\n", kClusters, ns);
    CHECK(sum > 0);
}
