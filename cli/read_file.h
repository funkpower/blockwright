#pragma once

#include <cstddef>
#include <string>

namespace bwcli {

// A failure the CLI reports as one line and exit status 1.
struct Failure {
    std::string message;
};

// The whole of `path`. Throws Failure for a directory, an unreadable file, a
// read error, or a file longer than maxSize bytes ("input too large"), which
// is checked before each append so std::string never throws length_error.
std::string readFile(const std::string& path, std::size_t maxSize = std::string().max_size());

} // namespace bwcli
