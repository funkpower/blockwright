#include "generators.h"

#include "read_file.h"

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
    try {
        return bw::generator::readDirectory(dir);
    } catch (const bw::generator::GeneratorError& e) {
        throw Failure{e.what()};
    }
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

std::vector<bw::LayerId> generatorsToRun(const bw::LayerStack& stack, const std::vector<bw::LayerId>& wanted,
                                         std::vector<bw::LayerId>* hidden) {
    const std::vector<bw::LayerId> all = bw::generator::generatorLayers(stack);
    if (wanted.empty()) {
        // a hidden layer shows nothing, and its program runs with the
        // user's rights: it runs only when named (PLAN §7 Trust, #440)
        std::vector<bw::LayerId> shown;
        for (const bw::LayerId id : all) {
            if (stack.find(id)->props.visible)
                shown.push_back(id);
            else if (hidden)
                hidden->push_back(id);
        }
        return shown;
    }
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
