#include "generators.h"

#include "read_file.h"

#include "blockwright/links.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace bwcli {

std::map<std::string, std::string> readGeneratorDir(const fs::path& dir) {
    std::error_code ec;
    // a junction is a link here too (links.h)
    if (!fs::is_directory(bw::linkStatus(dir, ec)))
        throw Failure{dir.string() + ": not a directory"};
    std::map<std::string, std::string> files;
    std::size_t total = 0;
    fs::recursive_directory_iterator it(dir, ec);
    if (ec)
        throw Failure{dir.string() + ": " + ec.message()};
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec)
            throw Failure{dir.string() + ": " + ec.message()};
        const fs::path& p = it->path();
        std::error_code entryEc;  // ec is the step's alone, checked after the loop too
        if (p.filename().string().starts_with(".")) {
            it.disable_recursion_pending();  // never walked into, whatever it is
            continue;
        }
        const fs::file_status st = bw::linkStatus(*it, entryEc);
        if (fs::is_directory(st))
            continue;
        if (!fs::is_regular_file(st))
            throw Failure{p.string() + ": not a plain file (a generator's files can't be links, pipes or devices)"};
        if (files.size() == kMaxGeneratorFiles)
            throw Failure{dir.string() + ": more than " + std::to_string(kMaxGeneratorFiles) + " files"};
        std::string bytes = readFile(p.string(), kMaxGeneratorBytes - total);
        total += bytes.size();
        files.emplace(fs::relative(p, dir).generic_string(), std::move(bytes));
    }
    // a failed step (a directory it can't open) can leave the iterator at its
    // end, which ends the loop before the check at its top
    if (ec)
        throw Failure{dir.string() + ": " + ec.message()};
    return files;
}

namespace {

#ifdef _WIN32
// A console's Ctrl+C, Ctrl+Break or close, a logoff or a shutdown: the
// running generator is stopped, and the next handler (the default one ends
// this program) runs.
BOOL WINAPI endOnControl(DWORD) {
    bw::generator::killRunning();
    return FALSE;
}
#else
void endOnSignal(int sig) {
    bw::generator::killRunning();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
#endif

} // namespace

void prepareToRunGenerators() {
#ifdef _WIN32
    ::SetConsoleCtrlHandler(endOnControl, TRUE);
#else
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    ::sigaction(SIGCHLD, &dfl, nullptr);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) {
        struct sigaction old {};
        if (::sigaction(sig, nullptr, &old) != 0 || old.sa_handler == SIG_IGN)
            continue;
        struct sigaction sa {};
        sa.sa_handler = endOnSignal;
        sigemptyset(&sa.sa_mask);
        ::sigaction(sig, &sa, nullptr);
    }
#endif
}

std::vector<bw::LayerId> generatorsToRun(const bw::LayerStack& stack, const std::vector<bw::LayerId>& wanted) {
    const std::vector<bw::LayerId> all = bw::generator::generatorLayers(stack);
    if (wanted.empty())
        return all;
    std::vector<bw::LayerId> ids;
    for (const bw::LayerId id : wanted) {
        const bw::Layer* l = stack.find(id);
        if (!l)
            throw Failure{"no layer " + std::to_string(id)};
        if (l->props.kind != bw::LayerKind::Generator)
            throw Failure{"layer " + std::to_string(id) + " (" + l->props.name + ") is not a generator layer"};
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    }
    // bottom first, as the stack draws them
    std::vector<bw::LayerId> ordered;
    for (const bw::LayerId id : all)
        if (std::find(ids.begin(), ids.end(), id) != ids.end())
            ordered.push_back(id);
    return ordered;
}

} // namespace bwcli
