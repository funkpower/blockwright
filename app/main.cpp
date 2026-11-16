// blockwright: the GUI.
//
//   blockwright [file.ans | file.xb | file.bin | project.bwp | project-directory]
//   blockwright --version | --help | --list-actions
//   blockwright --screenshot <steps.json> -o <dir>

#include "branding.h"
#include "file_open.h"
#include "generator_runner.h"
#include "action_list.h"
#include "shortcut_settings.h"
#include "main_window.h"
#include "screenshot.h"
#include "settings.h"
#include "start_screen.h"

#include "blockwright/credits.h"
#include "blockwright/logo_text.h"
#include "blockwright/version.h"

#include <QApplication>
#include <QFileInfo>
#include <QStyle>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
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
    bool listActions = false;
    const char* screenshotSteps = nullptr;
    const char* screenshotDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (dashdash || a[0] != '-' || !a[1]) {
            files.push_back(a);
        } else if (!std::strcmp(a, "--")) {
            dashdash = true;
        } else if (!std::strcmp(a, "--version") || !std::strcmp(a, "-v")) {
            messagesToConsole();
            // The version stays the first line, for the scripts that read it;
            // the second is the Qt it runs on, which its standard keys follow.
            std::cout << "blockwright " << BLOCKWRIGHT_VERSION << " (" << BLOCKWRIGHT_GIT_COMMIT << ")\n"
                      << "Qt " << qVersion() << "\n\n"
                      << bw::kLogoText << "\n"
                      << bw::kFontCredit << bw::kUnifontCredit << bw::kIconCredit << bw::kUiFontCredit;
            return 0;
        } else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            messagesToConsole();
            std::cout << "Usage: blockwright [file.ans | file.xb | file.bin | project.bwp | project-directory]\n"
                         "       blockwright --screenshot <steps.json> -o <dir>\n\n"
                         "ANSI and IRC art editor.\n\n"
                         "  -h, --help      show this help\n"
                         "  -v, --version   show the version, the Qt it runs on and the credits\n"
                         "  --list-actions  print every menu and dock action as JSON (id, menu, text,\n"
                         "                  default shortcuts, tips, tool and help ids) and exit\n"
                         "  --screenshot <steps.json> -o <dir>\n"
                         "                  replay a steps file (blockwright-cli replay --help gives\n"
                         "                  the format) in the window, offscreen, and write a PNG of\n"
                         "                  the window, a dock or a crop for each step's \"screenshot\":\n"
                         "                  {\"of\": \"window\" | a dock or widget's object name,\n"
                         "                   \"crop\": {\"cells\": [x, y, w, h]} | {\"pixels\": [x, y, w, h]},\n"
                         "                   \"when\": \"before\" | \"after\" | \"both\", \"scale\": 1 | 2,\n"
                         "                   \"pointer\": true | false},\n"
                         "                  as NN-<name>.png (-before, -after); a top-level\n"
                         "                  \"window\": [w, h] sizes the window (default 1280 x 800),\n"
                         "                  and a screenshot's \"scale\": 2 draws that picture at\n"
                         "                  device pixel ratio 2, twice the pixels each way;\n"
                         "                  \"pointer\": true draws a mouse pointer at the step's\n"
                         "                  last point (its first in a picture before it); Tool\n"
                         "                  Options and the palette show the options and colours\n"
                         "                  each step is applied with; the window's font is the\n"
                         "                  bundled Noto Sans at 10 pt\n";
            return 0;
        } else if (!std::strcmp(a, "--list-actions")) {
            listActions = true;
        } else if (!std::strcmp(a, "--screenshot") || !std::strcmp(a, "-o") || !std::strcmp(a, "--output")) {
            const bool steps = !std::strcmp(a, "--screenshot");
            if (i + 1 >= argc || (steps ? screenshotSteps : screenshotDir)) {
                messagesToConsole();
                std::cerr << "blockwright: " << a << (i + 1 >= argc ? " needs a value\n" : " given twice\n");
                return 2;
            }
            (steps ? screenshotSteps : screenshotDir) = argv[++i];
        } else {
            messagesToConsole();
            std::cerr << "blockwright: unknown option " << a << "\n";
            return 2;
        }
    }
    if (listActions && !files.empty()) {
        messagesToConsole();
        std::cerr << "blockwright: --list-actions takes no file\n";
        return 2;
    }
    if (listActions) {
        messagesToConsole();
        // The window is made but never shown, so no display is needed. The
        // desktop's theme and style are not loaded: a theme can need a display
        // (gtk3) or change the keys (kde, qt6ct), and the dump is the same
        // everywhere.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qunsetenv("QT_QPA_PLATFORMTHEME");
        qunsetenv("QT_STYLE_OVERRIDE");
    }
    const bool screenshot = screenshotSteps || screenshotDir;
    if (screenshot && (!screenshotSteps || !screenshotDir || listActions || !files.empty())) {
        messagesToConsole();
        std::cerr << "blockwright: usage: blockwright --screenshot <steps.json> -o <dir>\n";
        return 2;
    }
    // Offscreen on fixed screens, one at device pixel ratio 1 and one at 2,
    // in Qt's own Fusion style: as for --list-actions, the desktop's theme
    // and style are not loaded, and nothing in the environment scales the
    // screens or their fonts. The screens' file is read as the app is made.
    std::optional<bw::screenshot::FixedScreens> screens;
    if (screenshot) {
        messagesToConsole();
        screens.emplace();
        if (!screens->error().empty()) {
            std::cerr << "blockwright: " << screens->error() << "\n";
            return 1;
        }
        qunsetenv("QT_QPA_PLATFORMTHEME");
        qunsetenv("QT_STYLE_OVERRIDE");
    }
    if (files.size() > 1) {
        messagesToConsole();
        std::cerr << "blockwright: one file at a time\n";
        return 2;
    }

    // Qt's own options (-platform, -style, ...) are not offered: QApplication
    // would take them out of argv, but they were refused above.
    QApplication app(argc, argv);
    if (screens)
        screens->release();
    QApplication::setApplicationName(QStringLiteral("Blockwright"));
    QApplication::setApplicationVersion(QStringLiteral(BLOCKWRIGHT_VERSION " (" BLOCKWRIGHT_GIT_COMMIT ")"));
    // The dev install runs as blockwright-dev, with a .desktop file of that
    // name, and the Flatpak's entry is named for its app id: matching it lets
    // the desktop find its launcher entry.
    QApplication::setDesktopFileName(
        bw::desktopFileName(QString::fromLocal8Bit(argv[0]), qEnvironmentVariable("FLATPAK_ID")));
    // Every window's icon (#205); a desktop shell may take the launcher's instead
    QApplication::setWindowIcon(bw::applicationIcon());

    // A generator still running when the app quits is stopped, not waited for.
    bw::killGeneratorsOnQuit(&app);

    if (screenshot) {
        QApplication::setStyle(QStringLiteral("Fusion"));
        QApplication::setPalette(QApplication::style()->standardPalette());
        // the bundled UI font at a fixed size, so text doesn't depend on the
        // machine's fonts (but for the block and shade glyphs it lacks)
        if (const std::string why = bw::screenshot::useBundledFont(); !why.empty()) {
            std::cerr << "blockwright: the bundled UI font can't be loaded: " << why << "\n";
            return 1;
        }
        bw::useLinuxStandardKeys();
        return bw::screenshot::run(QString::fromLocal8Bit(screenshotSteps), QString::fromLocal8Bit(screenshotDir));
    }
    if (listActions) {
        // Under scratch settings the window has its default keys and layout,
        // whatever the user changed, and what it writes is dropped.
        bw::ScratchSettings scratch;
        if (!scratch.valid()) {
            std::cerr << "blockwright: no scratch settings file could be made\n";
            return 1;
        }
        bw::useLinuxStandardKeys();
        QByteArray json;
        {
            bw::MainWindow w;
            json = bw::actionListJson(&w);
        }
        std::cout.write(json.constData(), json.size());
        std::cout.flush();
        return std::cout ? 0 : 1;
    }

    bw::MainWindow w;
    // A document opened from the Finder or the Dock (macOS) arrives as an event.
    app.installEventFilter(new bw::FileOpenForwarder(&w));
    // A file named opens in the window; with none, the start screen asks
    // what to do before the window shows.
    bw::startUp(&w, files.empty() ? QString() : QString::fromLocal8Bit(files.front()));
    return app.exec();
}
