#include "blockwright/regular_file.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#define BW_REGULAR_POSIX 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define BW_REGULAR_POSIX 0
#include <fstream>
#endif

namespace fs = std::filesystem;

namespace bw {

namespace {

using Reason = RegularFileError::Reason;

RegularFileError tooLarge(std::size_t limit) {
    return {Reason::TooLarge, "is larger than " + std::to_string(limit) + " bytes"};
}

RegularFileError notRegular(const std::string& kind) {
    return {Reason::NotRegular,
            kind == "not a regular file" ? "is not a regular file" : "is " + kind + ", not a regular file", kind};
}

#if BW_REGULAR_POSIX
std::string kindOf(mode_t mode) {
    return S_ISDIR(mode)                    ? "a directory"
           : S_ISFIFO(mode)                 ? "a FIFO"
           : S_ISSOCK(mode)                 ? "a socket"
           : S_ISCHR(mode) || S_ISBLK(mode) ? "a device"
                                            : "not a regular file";
}

RegularFileError unreadable(const char* what, int code) {
    return {Reason::Unreadable, std::string(what) + ": " + std::strerror(code), {}, code};
}
#endif

// The regular file at `p`: all of it, refused past `limit` (head false), or
// only its first `limit` bytes (head true).
std::string read(const fs::path& p, std::size_t limit, bool head) {
#if BW_REGULAR_POSIX
    int fd;
    do
        fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        const int e = errno;
        if (e == ENOENT || e == ENOTDIR)
            throw RegularFileError(Reason::Missing, "doesn't exist", {}, e);
        // a socket can't be opened at all (ENXIO): name what is there
        struct stat st {};
        if (::stat(p.c_str(), &st) == 0 && !S_ISREG(st.st_mode))
            throw notRegular(kindOf(st.st_mode));
        throw unreadable("can't be read", e);
    }
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    struct stat st {};
    if (::fstat(fd, &st) != 0)
        throw unreadable("can't be read", errno);
    if (!S_ISREG(st.st_mode))
        throw notRegular(kindOf(st.st_mode));
    if (!head && std::uintmax_t(st.st_size) > limit)
        throw tooLarge(limit);
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
        throw unreadable("can't be read", errno);
    std::string out;
    out.reserve(std::min<std::uintmax_t>(std::uintmax_t(st.st_size), limit));
    char buf[65536];
    for (;;) {
        const std::size_t want = head ? std::min(sizeof buf, limit - out.size()) : sizeof buf;
        if (want == 0)
            break;
        const ssize_t n = ::read(fd, buf, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw unreadable("read failed", errno);
        if (n == 0)
            break;
        if (std::size_t(n) > limit - out.size())  // it grew since fstat
            throw tooLarge(limit);
        out.append(buf, std::size_t(n));
    }
    return out;
#else
    // No FIFOs to block on: the path is checked, then read.
    std::error_code ec;
    const fs::file_status status = fs::status(p, ec);
    if (status.type() == fs::file_type::not_found)
        throw RegularFileError(Reason::Missing, "doesn't exist", {}, ENOENT);
    if (status.type() == fs::file_type::directory)
        throw notRegular("a directory");
    if (!ec && status.type() != fs::file_type::regular)
        throw notRegular("not a regular file");
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw RegularFileError(Reason::Unreadable, "can't be read", {}, errno);
    const std::uintmax_t size = fs::file_size(p, ec);
    if (!head && !ec && size > limit)
        throw tooLarge(limit);
    std::string out;
    char buf[65536];
    const auto want = [&] {
        return std::streamsize(head ? std::min(sizeof buf, limit - out.size()) : sizeof buf);
    };
    while (want() > 0 && (in.read(buf, want()) || in.gcount() > 0)) {
        if (std::size_t(in.gcount()) > limit - out.size())
            throw tooLarge(limit);
        out.append(buf, std::size_t(in.gcount()));
    }
    if (in.bad())
        throw RegularFileError(Reason::Unreadable, "read failed", {}, errno);
    return out;
#endif
}

} // namespace

std::string RegularFileError::systemMessage() const {
    return code != 0 ? std::generic_category().message(code) : std::string(what());
}

std::string readRegular(const fs::path& p, std::size_t limit) { return read(p, limit, false); }

std::string readRegularHead(const fs::path& p, std::size_t n) { return read(p, n, true); }

} // namespace bw
