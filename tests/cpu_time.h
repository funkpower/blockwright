#pragma once

namespace bwtest {

// The CPU time this process has used, user and kernel, in seconds, so a
// timing bound doesn't depend on what else the machine is running. Not
// std::clock(): on Windows that is the wall time since the process started.
double cpuSeconds();

} // namespace bwtest
