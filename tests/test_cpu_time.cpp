#include "cpu_time.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace {

double wallSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// #254: the timing tests' clock counts the process's own work, not the time
// it spends waiting while the machine runs others.
TEST_CASE("cpuSeconds counts work, not waiting", "[cpu_time]") {
    SECTION("busy work raises it, no faster than the wall clock") {
        // Spin until it has risen a fifth of a second; load only makes that
        // take longer. What fails:
        // - a clock that reads 7.5x or more low, such as FILETIME's 100 ns
        //   ticks read as nanoseconds (100x): it needs 1.5 s of real CPU, so
        //   at least 1.5 s of wall time, and fails the wall bound. A clock
        //   that doesn't move hits the 60 s cap and fails the CPU check.
        // - a clock that reads high: it overshoots the wall clock, unless
        //   load slows this thread by as much as the clock runs fast.
        // A clock less than 7.5x low can pass. Wall time passed off as CPU
        // time is the sleep section's to catch.
        const double c0 = bwtest::cpuSeconds(), w0 = wallSeconds();
        volatile unsigned x = 1;
        double c = c0, w = w0;
        while (c - c0 < 0.2 && w - w0 < 60) {
            for (int i = 0; i < 100000; ++i)
                x = x * 1664525u + 1013904223u;
            c = bwtest::cpuSeconds();
            w = wallSeconds();
        }
        INFO("cpu " << c - c0 << " s in " << w - w0 << " s");
        CHECK(c - c0 >= 0.2);
        // a correct clock needs this thread to get 13% of one core
        CHECK(w - w0 < 1.5);
        // one thread can't use CPU faster than the wall clock runs; the
        // slack is a Windows clock's tick
        CHECK(c - c0 <= w - w0 + 0.02);
    }
    SECTION("a sleep raises it by much less than the sleep") {
        const double c0 = bwtest::cpuSeconds(), w0 = wallSeconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const double c = bwtest::cpuSeconds() - c0, w = wallSeconds() - w0;
        INFO("cpu " << c << " s in " << w << " s");
        REQUIRE(w >= 0.19);
        CHECK(c < 0.05);
    }
}

// #285: a CPU-time budget is set from an optimised build; a Debug build
// (no NDEBUG) gets it 20 times over, an optimised one as written.
TEST_CASE("cpuBudget scales a budget only without NDEBUG", "[cpu_time]") {
#ifdef NDEBUG
    STATIC_REQUIRE(bwtest::kUnoptimisedSlowdown == 1.0);
    STATIC_REQUIRE(bwtest::cpuBudget(0.3) == 0.3);
#else
    STATIC_REQUIRE(bwtest::kUnoptimisedSlowdown == 20.0);
    STATIC_REQUIRE(bwtest::cpuBudget(0.3) == 0.3 * 20.0);
#endif
    STATIC_REQUIRE(bwtest::cpuBudget(0.0) == 0.0);
    CHECK(bwtest::cpuBudget(0.05) == 0.05 * bwtest::kUnoptimisedSlowdown);
}
