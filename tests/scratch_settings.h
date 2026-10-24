#pragma once
// The app tests keep their settings in an INI file in the run's scratch
// directory, never in the user's real Blockwright settings, and on the
// platforms that allow it Qt's own settings go into that directory too.

#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace bwtest {

// The INI file useScratchSettings() injected.
inline QString& scratchSettingsFile() {
    static QString path;
    return path;
}

// Whether QSettings::setPath() moves the native settings Qt keeps for itself:
// the QtProject store a QFileDialog reads and, on destruction, writes its
// history and last directory to. On macOS they are CFPreferences and on
// Windows the registry, which setPath() leaves alone, so a test that builds a
// QFileDialog can't run there without writing the user's real store.
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN)
inline constexpr bool kQtSettingsRedirectable = false;
#else
inline constexpr bool kQtSettingsRedirectable = true;
#endif

inline bool inDir(const QString& file, const QString& dir) {
    return QFileInfo(file).absoluteFilePath().startsWith(QDir(dir).absolutePath() + QLatin1Char('/'));
}

// Whether the settings the app uses are the injected INI file, inside dir.
// The file name, not only the directory, is checked: on Linux the old
// QSettings::setPath() redirect also landed inside the scratch directory, but
// as a native .conf file that macOS and Windows would never have used.
inline bool settingsInScratch(const QString& dir) {
    const auto s = bw::appSettings();
    const QString file = QFileInfo(s->fileName()).absoluteFilePath();
    return s->format() == QSettings::IniFormat && !scratchSettingsFile().isEmpty() &&
           file == QFileInfo(scratchSettingsFile()).absoluteFilePath() && inDir(file, dir);
}

// Whether Qt's own settings, the ones QFileDialog uses, are inside dir.
inline bool qtSettingsInScratch(const QString& dir) {
    if (!kQtSettingsRedirectable)
        return false;
    const QSettings qt(QSettings::UserScope, QStringLiteral("QtProject"));
    return inDir(qt.fileName(), dir);
}

// Points the app's settings at settings.ini in dir and, where Qt allows it,
// every native and INI settings path at dir/qt. Called before any window is
// built or any setting cleared; the run stops if either would still use
// another file.
inline void useScratchSettings(const QTemporaryDir& dir) {
    scratchSettingsFile() = dir.filePath(QStringLiteral("settings.ini"));
    bw::setSettingsFile(scratchSettingsFile());
    if (kQtSettingsRedirectable) {
        for (const auto format : {QSettings::NativeFormat, QSettings::IniFormat}) {
            QSettings::setPath(format, QSettings::UserScope, dir.filePath(QStringLiteral("qt/user")));
            QSettings::setPath(format, QSettings::SystemScope, dir.filePath(QStringLiteral("qt/system")));
        }
    }
    if (!dir.isValid() || !settingsInScratch(dir.path()) ||
        (kQtSettingsRedirectable && !qtSettingsInScratch(dir.path()))) {
        std::fprintf(stderr, "the app's settings or Qt's are not in the scratch directory; not running\n");
        std::exit(1);
    }
}

} // namespace bwtest
