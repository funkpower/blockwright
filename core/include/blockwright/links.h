#pragma once
// What counts as a link when a walk refuses or doesn't follow one.
//
// On Windows std::filesystem (libc++) takes only a symbolic-link reparse
// point (IO_REPARSE_TAG_SYMLINK) for a symbolic link: a junction, or any
// other name surrogate, reads as the folder or file it leads to. These read
// every name-surrogate reparse point as a symbolic link, so a junction is
// reported and refused exactly as a symbolic link is. Elsewhere they are
// std::filesystem's own.

#include <filesystem>
#include <system_error>

namespace bw {

// fs::symlink_status(p), but on Windows a name-surrogate reparse point (a
// junction, a mount point) is file_type::symlink too.
std::filesystem::file_status linkStatus(const std::filesystem::path& p, std::error_code& ec);
// The same for a directory entry (its cached status, where that is enough).
std::filesystem::file_status linkStatus(const std::filesystem::directory_entry& e, std::error_code& ec);

// Whether `p` itself is a link, as linkStatus has it.
inline bool isLink(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_symlink(linkStatus(p, ec));
}
inline bool isLink(const std::filesystem::directory_entry& e) {
    std::error_code ec;
    return std::filesystem::is_symlink(linkStatus(e, ec));
}

} // namespace bw
