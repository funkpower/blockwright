#pragma once

// Random draws for property tests that come out the same with every standard
// library. std::mt19937 is fully specified, but the distributions and
// std::shuffle are not: libc++ (macOS, the Windows build) draws other numbers
// from the same engine than libstdc++ does, so a test's cases, and the
// thresholds and expectations tuned on them, would change with the platform.
// These follow libstdc++'s algorithms for a 32-bit engine, so the cases stay
// those the tests were written against (test_portable_random.cpp checks it
// where libstdc++ is the library).

#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>

namespace bwtest {

// A number in [0, range) from a 32-bit engine: Lemire's nearly divisionless
// method, as libstdc++'s uniform_int_distribution draws it; range 0 is 2^32.
template <class Engine>
std::uint32_t below(Engine& g, std::uint64_t range) {
    static_assert(Engine::min() == 0 && Engine::max() == 0xffffffffu, "a 32-bit engine");
    if (range > 0xffffffffu)
        return std::uint32_t(g());
    const auto r = std::uint32_t(range);
    std::uint64_t product = std::uint64_t(std::uint32_t(g())) * r;
    auto low = std::uint32_t(product);
    if (low < r) {
        const std::uint32_t threshold = std::uint32_t(-r) % r;
        while (low < threshold) {
            product = std::uint64_t(std::uint32_t(g())) * r;
            low = std::uint32_t(product);
        }
    }
    return std::uint32_t(product >> 32);
}

// std::uniform_int_distribution<int>(lo, hi)(g), as libstdc++ draws it.
template <class Engine>
int uniformInt(Engine& g, int lo, int hi) {
    const std::uint64_t range = std::uint64_t(std::int64_t(hi) - lo) + 1;
    return int(std::int64_t(lo) + below(g, range));
}

// std::uniform_real_distribution<double>(lo, hi)(g), as libstdc++ draws it:
// two 32-bit draws make a fraction in [0, 1).
template <class Engine>
double uniformReal(Engine& g, double lo, double hi) {
    const double base = 4294967296.0;
    const double first = double(std::uint32_t(g()));
    const double second = double(std::uint32_t(g()));
    double u = (first + second * base) / (base * base);
    if (u >= 1.0)
        u = std::nextafter(1.0, 0.0);
    return u * (hi - lo) + lo;
}

// std::shuffle(first, last, g), as libstdc++ shuffles: two swap positions
// from one draw while the squared range fits the engine's.
template <class It, class Engine>
void shuffle(It first, It last, Engine& g) {
    if (first == last)
        return;
    const std::uint64_t n = std::uint64_t(last - first);
    It i = first + 1;
    if (0xffffffffu / n >= n) {
        if (n % 2 == 0)
            std::iter_swap(i++, first + below(g, 2));
        while (i != last) {
            const std::uint64_t b0 = std::uint64_t(i - first) + 1, b1 = b0 + 1;
            const std::uint64_t x = below(g, b0 * b1);
            std::iter_swap(i++, first + std::ptrdiff_t(x / b1));
            std::iter_swap(i++, first + std::ptrdiff_t(x % b1));
        }
        return;
    }
    for (; i != last; ++i)
        std::iter_swap(i, first + std::ptrdiff_t(below(g, std::uint64_t(i - first) + 1)));
}

} // namespace bwtest
