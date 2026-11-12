#pragma once

#include <cstddef>
#include <string>

namespace bwcli {

// A failure the CLI reports as one line and exit status 1.
struct Failure {
    std::string message;
};

// The whole of the regular file at `path`, read through its opened handle
// (bw::readRegular), so a FIFO, device or socket there is refused at once
// and never waited on. Throws Failure "<path>: <why>": "is a directory, not a
// regular file" (or a FIFO, a device, a socket), the system's reason for a
// missing or unreadable file, or "input too large" for a file longer than
// maxSize bytes, checked before each append so std::string never throws
// length_error.
std::string readFile(const std::string& path, std::size_t maxSize = std::string().max_size());

} // namespace bwcli
