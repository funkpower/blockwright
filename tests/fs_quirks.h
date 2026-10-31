#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bwtest {

// Whether the symbolic-link tests run here. Windows makes symbolic links only
// with the right to (Developer Mode, or an elevated process). Under wine it
// makes them, and they read back as links, but the tests that use them don't
// run there yet (#251): two use POSIX paths or expect `/` as the separator
// (test_library.cpp's realBinDir case, test_generator.cpp's readGeneratorDir
// case), and two (test_project.cpp's and test_pipeline.cpp's link swapped in
// mid-save) see their own link as a plain file through a wine stat quirk in
// the same process. Junctions, which need no right, are tested on Windows in
// test_junction_win.cpp.
inline bool symlinksWork() {
#ifdef _WIN32
    static const bool works = [] {
        if (::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))
            return false;
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
