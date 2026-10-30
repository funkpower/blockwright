// blockwright: the GUI.
//
//   blockwright [file.ans | file.xb | file.bin | project.bwp | project-directory]
//   blockwright --version | --help

#include "generator_runner.h"
#include "main_window.h"

#include "blockwright/credits.h"
#include "blockwright/version.h"

#include <QApplication>
#include <QFileInfo>

#include <cstring>
#include <iostream>
#include <vector>

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
            std::cout << "blockwright " << BLOCKWRIGHT_VERSION << " (" << BLOCKWRIGHT_GIT_COMMIT << ")\n"
                      << bw::kFontCredit;
            return 0;
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            std::cout << "Usage: blockwright [file.ans | file.xb | file.bin | project.bwp | project-directory]\n\n"
                         "ANSI and IRC art editor.\n\n"
                         "  -h, --help     show this help\n"
                         "  -v, --version  show the version and the font credit\n";
            return 0;
        } else {
            std::cerr << "blockwright: unknown option " << a << "\n";
            return 2;
        }
    }
    if (files.size() > 1) {
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
    w.show();
    if (!files.empty())
        w.openInteractive(QString::fromLocal8Bit(files.front()));
    return app.exec();
}
