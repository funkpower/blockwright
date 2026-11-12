#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace bw {

// Why readRegular refused a path. what() carries no path, so each caller
// words the refusal its own way: "doesn't exist", "is a FIFO, not a regular
// file", "is larger than N bytes", "can't be read: <reason>", "read failed:
// <reason>".
class RegularFileError : public std::runtime_error {
public:
    enum class Reason {
        Missing,     // no such file (or a path through a file)
        NotRegular,  // a directory, FIFO, device or socket: `kind` names it
        TooLarge,    // more than the limit, found before it is read or as it grows
        Unreadable,  // the open, fstat or a read failed: `code` is errno
    };
    RegularFileError(Reason reason, std::string message, std::string kind = {}, int code = 0)
        : std::runtime_error(std::move(message)), reason(reason), kind(std::move(kind)), code(code) {}
    Reason reason;
    // For NotRegular: "a directory", "a FIFO", "a device", "a socket" or
    // "not a regular file" when it is none of those.
    std::string kind;
    // The errno behind Missing or Unreadable, where there is one.
    int code;
    // The system's words for `code` ("No such file or directory",
    // "Permission denied"), or what() when there is no errno.
    std::string systemMessage() const;
};

// The whole of the regular file at `p` (through a link), at most `limit`
// bytes. On POSIX the opened handle is what is checked: the path is opened
// once, without blocking and without becoming a controlling terminal, and
// refused unless fstat calls that handle a regular file, so a FIFO, device or
// socket at the path, there from the start or swapped in after an earlier
// path check, is refused at once and never waited on; the handle is then
// read blocking. Every reader of a file a user names goes through this.
std::string readRegular(const std::filesystem::path& p,
                        std::size_t limit = std::numeric_limits<std::size_t>::max());

// At most the first `n` bytes of the regular file at `p`, opened and checked
// as readRegular does; never TooLarge.
std::string readRegularHead(const std::filesystem::path& p, std::size_t n);

} // namespace bw
