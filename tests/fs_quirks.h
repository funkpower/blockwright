#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bwtest {

// Whether the symbolic-link tests run here. Windows makes symbolic links only
// with the right to (Developer Mode, or an elevated process); wine makes them
// always. Under wine a link must be named by a path with a drive: one named by
// a path that starts with / reads as what it points at, so the tests' scratch
// folder is on Z: there (tests/CMakeLists.txt). Junctions, which need no
// right, are tested on Windows in test_junction_win.cpp.
inline bool symlinksWork() {
#ifdef _WIN32
    static const bool works = [] {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path link =
            fs::temp_directory_path(ec) / ("bw-link-probe-" + std::to_string(::GetCurrentProcessId()));
        fs::create_symlink("nowhere", link, ec);
        if (ec)
            return false;
        const bool is = fs::is_symlink(link, ec);
        fs::remove(link, ec);
        return is;
    }();
    return works;
#else
    return true;
#endif
}

// Whether a rename that changed only the case of a name took: a directory
// entry spelled exactly as `path` now exists. Some file systems keep the old
// spelling (wine's view of a case-sensitive one does).
inline bool caseRenamed(const std::filesystem::path& path) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(path.parent_path(), ec))
        if (e.path().filename() == path.filename())
            return true;
    return false;
}

} // namespace bwtest
