// portable_random.h: its draws are libstdc++'s, on every platform.

#include "portable_random.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

TEST_CASE("portable draws are the same numbers on every platform", "[testing]") {
    std::mt19937 g(2026103101);
    std::vector<int> ints;
    for (int i = 0; i < 8; ++i)
        ints.push_back(bwtest::uniformInt(g, -3, 1000));
    std::vector<int> order(10);
    std::iota(order.begin(), order.end(), 0);
    bwtest::shuffle(order.begin(), order.end(), g);
    const double real = bwtest::uniformReal(g, 0.02, 1.0);
    CHECK(ints == (std::vector<int>{102, 438, 897, 405, 968, 283, 618, 667}));
    CHECK(order == (std::vector<int>{9, 3, 6, 2, 5, 0, 7, 4, 8, 1}));
    CHECK(real == 0.67015374274790684);
}

#ifdef __GLIBCXX__
TEST_CASE("portable draws match libstdc++'s distributions and shuffle", "[testing]") {
    std::mt19937 a(7), b(7);
    for (const auto& [lo, hi] : std::vector<std::pair<int, int>>{
             {0, 0}, {0, 1}, {-5, 5}, {0, 99}, {1, 1000000}, {-2147483647 - 1, 2147483647}, {0, 2147483647}}) {
        for (int i = 0; i < 2000; ++i)
            REQUIRE(bwtest::uniformInt(a, lo, hi) == std::uniform_int_distribution<int>(lo, hi)(b));
    }
    for (int i = 0; i < 2000; ++i)
        REQUIRE(bwtest::uniformReal(a, 0.02, 1.0) == std::uniform_real_distribution<double>(0.02, 1.0)(b));
    for (std::size_t n : {1u, 2u, 3u, 10u, 11u, 400u, 65535u, 65536u, 70000u}) {
        std::vector<int> x(n), y(n);
        std::iota(x.begin(), x.end(), 0);
        y = x;
        bwtest::shuffle(x.begin(), x.end(), a);
        std::shuffle(y.begin(), y.end(), b);
        REQUIRE(x == y);
    }
}
#endif
