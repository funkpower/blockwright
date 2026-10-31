#include "blockwright/links.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace bw {

namespace {

#ifdef _WIN32
enum class Surrogate { No, Yes, Gone };

bool notFound(DWORD err) {
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

// Whether `p` is a reparse point that names another place: a symbolic link,
// a junction or a mount point. It is not one only when its attributes or its
// tag say so; one whose tag can't be read either is taken for one, so it is
// never walked into, and one that no longer exists is Gone.
Surrogate nameSurrogate(const fs::path& p) {
    const DWORD attrs = ::GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (notFound(::GetLastError()))
            return Surrogate::Gone;
        // Attributes unreadable: the tag, read below, decides.
    } else if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return Surrogate::No;
    }
    const HANDLE h = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return notFound(::GetLastError()) ? Surrogate::Gone : Surrogate::Yes;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool read = ::GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag);
    ::CloseHandle(h);
    return (!read || ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag)))
               ? Surrogate::Yes
               : Surrogate::No;
}

fs::file_status asLink(fs::file_status st) {
    return fs::file_status(fs::file_type::symlink, st.permissions());
}

fs::file_status surrogateAsLink(const fs::path& p, fs::file_status st, std::error_code& ec) {
    if (!fs::exists(st) || fs::is_symlink(st))
        return st;
    switch (nameSurrogate(p)) {
    case Surrogate::No:
        return st;
    case Surrogate::Yes:
        return asLink(st);
    case Surrogate::Gone:
        break;
    }
    // Removed since its status was read: read it again, so it reads as not
    // existing the way the library reports that. Anything found there now is
    // taken for a link.
    const fs::file_status again = fs::symlink_status(p, ec);
    return !ec && fs::exists(again) ? asLink(again) : again;
}
#endif

} // namespace

fs::file_status linkStatus(const fs::path& p, std::error_code& ec) {
    const fs::file_status st = fs::symlink_status(p, ec);
#ifdef _WIN32
    if (!ec)
        return surrogateAsLink(p, st, ec);
#endif
    return st;
}

fs::file_status linkStatus(const fs::directory_entry& e, std::error_code& ec) {
    const fs::file_status st = e.symlink_status(ec);
#ifdef _WIN32
    if (!ec)
        return surrogateAsLink(e.path(), st, ec);
#endif
    return st;
}

} // namespace bw
