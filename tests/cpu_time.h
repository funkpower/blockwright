#pragma once

namespace bwtest {

// The CPU time this process has used, user and kernel, in seconds, so a
// timing bound doesn't depend on what else the machine is running. Not
// std::clock(): on Windows that is the wall time since the process started.
double cpuSeconds();

// How many times slower a build without NDEBUG (Debug: no optimisation)
// runs the work a CPU-time budget is set for (#285). The budgets are set
// from an optimised build (RelWithDebInfo, the default and CI's). Measured
// with GCC 16: the form brush's first dab on a 160x1000 selection takes
// 0.18 s optimised and 3.0 s at -O0 (17x), the same dab under the alpha
// lock 0.05 s and 0.52 s (10x). 20 leaves a Debug build the headroom an
// optimised one has, and still fails a change that makes the work a few
// times bigger, let alone one that changes its order.
#ifdef NDEBUG
inline constexpr double kUnoptimisedSlowdown = 1.0;
#else
inline constexpr double kUnoptimisedSlowdown = 20.0;
#endif

// A CPU-time budget of `seconds` in an optimised build, for this build.
constexpr double cpuBudget(double seconds) { return seconds * kUnoptimisedSlowdown; }

} // namespace bwtest
