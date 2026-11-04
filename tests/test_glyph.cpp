// A cell's glyph and the cluster pool (glyph.h, PLAN §16 Storage). The pool
// is shared by every test in this process, so no test here asserts an id or
// how many clusters it holds: the cap and the checked lookup are tested on
// pools of their own.

#include "blockwright/ansi.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/cell.h"
#include "blockwright/cp437.h"
#include "blockwright/glyph.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace bw;

namespace {

std::optional<Glyph> intern(ClusterPool& pool, std::vector<char32_t> cps, PoolCap cap = PoolCap::Capped) {
    return pool.intern(cps, cap);
}

std::vector<char32_t> cpsOf(const GlyphCodePoints& g) { return {g.begin(), g.end()}; }

// The i-th of a family of distinct two-code-point clusters: a letter and a
// combining mark, then more marks as i grows.
std::vector<char32_t> nthCluster(std::size_t i) {
    std::vector<char32_t> cps{U'a' + char32_t(i % 26)};
    for (i /= 26;; i /= 112) {
        cps.push_back(0x300 + char32_t(i % 112));  // U+0300-036F, all Mn
        if (i < 112)
            break;
    }
    return cps;
}

std::string text(Glyph g) {
    std::ostringstream os;
    os << g;
    return os.str();
}

} // namespace

TEST_CASE("Cell is 20 bytes and Glyph 32 bits") {
    STATIC_CHECK(sizeof(Cell) == 20);
    STATIC_CHECK(sizeof(Glyph) == 4);
}

TEST_CASE("a glyph made from a char32_t is only ever a code point") {
    STATIC_CHECK(Glyph() == U' ');
    STATIC_CHECK(Glyph(U'A').codePoint() == U'A');
    STATIC_CHECK(Glyph(char32_t(0)).codePoint() == U'\0');
    STATIC_CHECK(Glyph(char32_t(0x10FFFF)).codePoint() == char32_t(0x10FFFF));
    // past U+10FFFF: U+FFFD, never the continuation or an id
    for (const char32_t v : {char32_t(0x110000), char32_t(0x110001), char32_t(0x7FFFFFFF), char32_t(0xFFFFFFFF)}) {
        INFO(std::hex << unsigned(v));
        const Glyph g(v);
        CHECK(g.isCodePoint());
        CHECK_FALSE(g.isContinuation());
        CHECK_FALSE(g.isCluster());
        CHECK(g == U'�');
    }
    // a surrogate is a value a cell can hold, as it does today (a .cells
    // reader refuses it, the writers write '?')
    CHECK(Glyph(char32_t(0xD800)).codePoint() == char32_t(0xD800));
    CHECK(Glyph(kNoCodePoint) == U'�');
}

TEST_CASE("the continuation has no code point") {
    constexpr Glyph c = Glyph::continuation();
    STATIC_CHECK(c.isContinuation());
    STATIC_CHECK_FALSE(c.isCodePoint());
    STATIC_CHECK_FALSE(c.isCluster());
    STATIC_CHECK_FALSE(c.codePoint().has_value());
    STATIC_CHECK(c.codePointOr(U'x') == U'x');
    STATIC_CHECK(c != Glyph(U' '));
    STATIC_CHECK(c != Glyph(char32_t(0x110000)));
    CHECK(codePoints(c).empty());
    CHECK(glyphColumns(c) == 0);
    CHECK(text(c) == "continuation");
}

TEST_CASE("the pool interns a cluster once and gives back its code points and width") {
    ClusterPool pool;
    const auto e = intern(pool, {U'e', 0x301});
    REQUIRE(e);
    CHECK(e->isCluster());
    CHECK_FALSE(e->codePoint());
    CHECK(e->codePointOr(U'?') == U'?');
    CHECK(cpsOf(pool.codePoints(*e)) == std::vector<char32_t>{U'e', 0x301});
    CHECK(pool.width(*e) == 1);
    // the same sequence is the same glyph; another is another
    CHECK(intern(pool, {U'e', 0x301}) == e);
    const auto a = intern(pool, {U'a', 0x301});
    REQUIRE(a);
    CHECK(*a != *e);
    CHECK(intern(pool, {0x301, U'e'}) != e);
    // a cluster is never equal to a code point, its base included
    CHECK(*e != Glyph(U'e'));
    // its width is its base's
    const auto wide = intern(pool, {0x4E2D, 0x301});
    REQUIRE(wide);
    CHECK(pool.width(*wide) == 2);
    const auto heart = intern(pool, {0x2764, 0xFE0F});
    REQUIRE(heart);
    CHECK(pool.width(*heart) == 1);
}

TEST_CASE("interning one code point gives the code point, not an id") {
    ClusterPool pool;
    const auto g = intern(pool, {0x4E2D});
    REQUIRE(g);
    CHECK(g->isCodePoint());
    CHECK(*g == Glyph(char32_t(0x4E2D)));
}

TEST_CASE("the pool takes 1 to 16 Unicode scalar values") {
    ClusterPool pool;
    std::vector<char32_t> sixteen{U'a'};
    sixteen.resize(16, 0x301);
    const auto g = intern(pool, sixteen);
    REQUIRE(g);
    CHECK(pool.codePoints(*g).size == 16);
    std::vector<char32_t> seventeen = sixteen;
    seventeen.push_back(0x301);
    CHECK_THROWS_AS(intern(pool, seventeen), std::invalid_argument);
    CHECK_THROWS_AS(intern(pool, {}), std::invalid_argument);
    CHECK_THROWS_AS(intern(pool, {U'a', 0xD800}), std::invalid_argument);
    CHECK_THROWS_AS(intern(pool, {U'a', char32_t(0x110000)}), std::invalid_argument);
    CHECK_THROWS_AS(intern(pool, {char32_t(0x110000)}), std::invalid_argument);
    CHECK_THROWS_AS(intern(pool, {0xDFFF}), std::invalid_argument);
}

TEST_CASE("the pool refuses a cluster that holds a control, and keeps one on a zero-width base") {
    ClusterPool pool;
    // C0 but U+0000, DEL, C1, and the line and paragraph separators
    for (const char32_t c : {char32_t(0x01), char32_t(0x09), char32_t(0x1B), char32_t(0x1F), char32_t(0x7F),
                             char32_t(0x80), char32_t(0x85), char32_t(0x9F), char32_t(0x2028), char32_t(0x2029)}) {
        INFO("U+" << std::hex << std::uint32_t(c));
        CHECK_THROWS_AS(intern(pool, {c, 0x301}), std::invalid_argument);
        CHECK_THROWS_AS(intern(pool, {U'a', c}), std::invalid_argument);
        // alone it is a code point, not a cluster, as before
        CHECK(intern(pool, {c}) == Glyph(c));
    }
    // U+0000 is a cell's blank, width 1
    const auto nul = intern(pool, {0x0000, 0x301});
    REQUIRE(nul);
    CHECK(nul->isCluster());
    // a lone mark takes a cell and later marks join it (lint lone-mark)
    const auto marks = intern(pool, {0x301, 0x302});
    REQUIRE(marks);
    CHECK(marks->isCluster());
    CHECK(pool.width(*marks) == 0);
}

TEST_CASE("an id a pool doesn't hold reads as U+FFFD") {
    ClusterPool other, fresh;
    const auto g = intern(other, {U'e', 0x301});
    REQUIRE(g);
    CHECK(cpsOf(fresh.codePoints(*g)) == std::vector<char32_t>{U'�'});
    CHECK(fresh.width(*g) == 1);
}

TEST_CASE("the cap refuses a new cluster on a lossy path only") {
    ClusterPool pool;
    std::vector<Glyph> first;
    for (std::size_t i = 0; i < ClusterPool::kLossyCap; ++i) {
        const auto g = intern(pool, nthCluster(i), PoolCap::Capped);
        if (!g)
            FAIL("cluster " << i << " refused under the cap");
        if (i % 4099 == 0 || i + 1 == ClusterPool::kLossyCap)
            first.push_back(*g);
    }
    // full: a new one is refused on a lossy path...
    CHECK_FALSE(intern(pool, nthCluster(ClusterPool::kLossyCap), PoolCap::Capped));
    // ...but one it holds is still found
    CHECK(intern(pool, nthCluster(0), PoolCap::Capped) == first.front());
    // and a project load (uncapped) still adds, so whether a project opens
    // doesn't depend on what the session read before
    std::vector<Glyph> past;
    for (std::size_t i = ClusterPool::kLossyCap; i < ClusterPool::kLossyCap + 3000; ++i) {
        const auto g = intern(pool, nthCluster(i), PoolCap::Uncapped);
        REQUIRE(g);
        past.push_back(*g);
    }
    // each reads back as itself across the chunks
    for (std::size_t i = 0; i < past.size(); ++i)
        if (cpsOf(pool.codePoints(past[i])) != nthCluster(ClusterPool::kLossyCap + i))
            FAIL("cluster " << ClusterPool::kLossyCap + i << " read back wrong");
    for (std::size_t k = 0; k < first.size(); ++k) {
        const std::size_t i = k + 1 == first.size() ? ClusterPool::kLossyCap - 1 : k * 4099;
        CHECK(cpsOf(pool.codePoints(first[k])) == nthCluster(i));
    }
    // still capped for a lossy path
    CHECK_FALSE(intern(pool, nthCluster(ClusterPool::kLossyCap + 5000), PoolCap::Capped));
}

TEST_CASE("the pool is safe to read while other threads add") {
    ClusterPool pool;
    constexpr int kThreads = 6;
    constexpr std::size_t kEach = 6000;
    std::atomic<int> wrong{0}, shared{0};
    std::vector<std::vector<Glyph>> got(kThreads);
    // the ids as each is interned, by any thread, for the others to read
    // (a default glyph is a space, never a cluster)
    std::vector<std::atomic<Glyph>> slots(kEach);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            // every thread interns the same clusters in a different order and
            // reads each back at once, and every so often an earlier one
            std::vector<Glyph>& mine = got[std::size_t(t)];
            mine.resize(kEach);
            for (std::size_t n = 0; n < kEach; ++n) {
                const std::size_t i = (n * 7919 + std::size_t(t) * 1031) % kEach;
                const auto g = pool.intern(nthCluster(i), PoolCap::Uncapped);
                if (!g || cpsOf(pool.codePoints(*g)) != nthCluster(i))
                    ++wrong;
                else
                    mine[i] = *g;
                if (g)
                    slots[i].store(*g, std::memory_order_release);
                const std::size_t j = (i * 31) % kEach;
                if (mine[j].isCluster() && cpsOf(pool.codePoints(mine[j])) != nthCluster(j))
                    ++wrong;
                // an id another thread may have interned, read here
                const std::size_t k = (i * 17 + std::size_t(t) * 389) % kEach;
                if (const Glyph other = slots[k].load(std::memory_order_acquire); other.isCluster()) {
                    if (cpsOf(pool.codePoints(other)) != nthCluster(k))
                        ++wrong;
                    else if (mine[k] != other)
                        ++shared;
                }
            }
        });
    for (auto& th : threads)
        th.join();
    INFO("ids read before this thread interned them: " << shared.load());
    CHECK(wrong == 0);
    // one id a sequence, whichever thread added it
    for (int t = 1; t < kThreads; ++t)
        CHECK(got[std::size_t(t)] == got[0]);
}

TEST_CASE("the process-wide pool backs codePoints and glyphColumns") {
    const auto g = clusterPool().intern(std::vector<char32_t>{0x1F44D, 0x1F3FD}, PoolCap::Capped);
    REQUIRE(g);
    CHECK(cpsOf(codePoints(*g)) == std::vector<char32_t>{0x1F44D, 0x1F3FD});
    CHECK(glyphColumns(*g) == 2);
    CHECK(cpsOf(codePoints(U'A')) == std::vector<char32_t>{U'A'});
    CHECK(glyphColumns(U'A') == 1);
    CHECK(glyphColumns(char32_t(0x301)) == 0);
    CHECK(glyphColumns(char32_t(0x4E2D)) == 2);
    CHECK(text(*g) == "cluster U+1F44D U+1F3FD");
    CHECK(text(U'A') == "U+0041 'A'");
    CHECK(text(char32_t(0x2591)) == "U+2591");
}

TEST_CASE("a cluster or continuation is quoted by its code points") {
    const auto e = clusterPool().intern(std::vector<char32_t>{U'e', 0x301}, PoolCap::Capped);
    const auto heart = clusterPool().intern(std::vector<char32_t>{U'♥', 0xFE0F}, PoolCap::Capped);
    REQUIRE(e);
    REQUIRE(heart);
    CHECK(quotedGlyph(*e) == "e U+0301");
    CHECK(quotedGlyph(*heart) == "♥ U+FE0F");
    CHECK(codePointNames(*e) == "U+0065 U+0301");
    CHECK(quotedGlyph(Glyph(U'中')) == "中");
    CHECK(codePointNames(Glyph(U'中')) == "U+4E2D");
    CHECK(quotedGlyph(Glyph::continuation()).empty());
    CHECK(codePointNames(Glyph::continuation()).empty());
}

TEST_CASE("a writer that doesn't know pairs yet writes a lead and a cluster as nothing and a continuation as a space") {
    const Glyph cont = Glyph::continuation();
    const auto e = clusterPool().intern(std::vector<char32_t>{U'e', 0x301}, PoolCap::Capped);
    REQUIRE(e);
    CHECK(interimCodePoint(U'A', U'B') == U'A');
    CHECK(interimCodePoint(U'中', U'B') == U'中');  // a lone wide cell, as today
    CHECK_FALSE(interimCodePoint(U'中', cont));     // a lead
    CHECK_FALSE(interimCodePoint(U'A', cont));      // a lead is a lead whatever it holds
    CHECK(interimCodePoint(cont, U'A') == U' ');
    CHECK(interimCodePoint(cont, cont) == U' ');
    CHECK_FALSE(interimCodePoint(*e, U'A'));
    CHECK_FALSE(interimCodePoint(*e, cont));
}

TEST_CASE("a cluster or continuation passes no glyph test and has no mirror") {
    const Glyph cont = Glyph::continuation();
    const auto shade = clusterPool().intern(std::vector<char32_t>{kShadeMedium, 0x301}, PoolCap::Capped);
    const auto half = clusterPool().intern(std::vector<char32_t>{kUpperHalf, 0x301}, PoolCap::Capped);
    const auto slash = clusterPool().intern(std::vector<char32_t>{U'/', 0x301}, PoolCap::Capped);
    REQUIRE(shade);
    REQUIRE(half);
    REQUIRE(slash);
    for (const Glyph g : {cont, *shade, *half}) {
        INFO(g);
        CHECK_FALSE(blankGlyph(g));
        CHECK_FALSE(shadeLevel(g));
        CHECK_FALSE(isSprayStar(g));
        Cell c;
        c.ch = g;
        c.fg = Color::indexed(4);
        c.bg = Color::indexed(4);
        CHECK_FALSE(halfBlocks(c, true));
        CHECK_FALSE(solidColour(c));
    }
    CHECK(flipGlyph(*slash, Flip::Horizontal) == *slash);
    CHECK(rotateGlyph(*slash, true) == *slash);
    CHECK(flipGlyph(cont, Flip::Vertical) == cont);
    // a code point as before
    CHECK(flipGlyph(Glyph(U'/'), Flip::Horizontal) == U'\\');
    CHECK(rotateGlyph(Glyph(kUpperHalf), true) == U'▐');
    CHECK(shadeLevel(Glyph(kShadeMedium)) == 2);
    CHECK(blankGlyph(Glyph(U'\0')));
}
