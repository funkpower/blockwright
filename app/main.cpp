// blockwright: the GUI.
//
//   blockwright [file.ans | file.xb | file.bin | project.bwp | project-directory]
//   blockwright --version | --help

#include "file_open.h"
#include "generator_runner.h"
#include "main_window.h"
#include "start_screen.h"

#include "blockwright/credits.h"
#include "blockwright/version.h"

#include <QApplication>
#include <QFileInfo>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// On Windows the app is a GUI program, started with no console: what it
// says about its arguments goes to the console of the program that started
// it, if there is one, unless its stdout or stderr was redirected. Only then,
// so closing that console doesn't take a running app with it.
void messagesToConsole() {
#ifdef _WIN32
    const bool out = ::GetFileType(::GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN;
    const bool err = ::GetFileType(::GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN;
    if ((out && err) || !::AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    if (!out && std::freopen("CONOUT$", "w", stdout))
        std::cout.clear();
    if (!err && std::freopen("CONOUT$", "w", stderr))
        std::cerr.clear();
#endif
}

} // namespace

int main(int argc, char** argv) {
    // The arguments are read before QApplication exists, so --version,
    // --help and usage errors work without a display.
    std::vector<const char*> files;
    bool dashdash = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (dashdash || a[0] != '-' || !a[1]) {
            files.push_back(a);
        } else if (!std::strcmp(a, "--")) {
            dashdash = true;
        } else if (!std::strcmp(a, "--version") || !std::strcmp(a, "-v")) {
            messagesToConsole();
            std::cout << "blockwright " << BLOCKWRIGHT_VERSION << " (" << BLOCKWRIGHT_GIT_COMMIT << ")\n"
                      << bw::kFontCredit << bw::kIconCredit;
            return 0;
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            messagesToConsole();
            std::cout << "Usage: blockwright [file.ans | file.xb | file.bin | project.bwp | project-directory]\n\n"
                         "ANSI and IRC art editor.\n\n"
                         "  -h, --help     show this help\n"
                         "  -v, --version  show the version and the font and icon credits\n";
            return 0;
        } else {
            messagesToConsole();
            std::cerr << "blockwright: unknown option " << a << "\n";
            return 2;
        }
    }
    if (files.size() > 1) {
        messagesToConsole();
        std::cerr << "blockwright: one file at a time\n";
        return 2;
    }

    // Qt's own options (-platform, -style, ...) are not offered: QApplication
    // would take them out of argv, but they were refused above.
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Blockwright"));
    QApplication::setApplicationVersion(QStringLiteral(BLOCKWRIGHT_VERSION " (" BLOCKWRIGHT_GIT_COMMIT ")"));
    // The dev install runs as blockwright-dev, with a .desktop file of that
    // name: matching it lets the desktop find its launcher entry.
    QApplication::setDesktopFileName(QFileInfo(QString::fromLocal8Bit(argv[0])).fileName());

    // A generator still running when the app quits is stopped, not waited for.
    bw::killGeneratorsOnQuit(&app);

    bw::MainWindow w;
    // A document opened from the Finder or the Dock (macOS) arrives as an event.
    app.installEventFilter(new bw::FileOpenForwarder(&w));
    w.show();
    // A file named opens; with none, the start screen asks what to do.
    bw::startUp(&w, files.empty() ? QString() : QString::fromLocal8Bit(files.front()));
    return app.exec();
}
