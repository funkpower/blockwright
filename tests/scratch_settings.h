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

// The scratch directory useScratchSettings() was given, kept apart from the
// file name so that settingsInScratch() checks the file against the directory
// the run created rather than against the file's own folder. Empty until a
// valid directory is given.
inline QString& scratchRoot() {
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

// Whether the settings the app uses are the injected INI file, inside the
// scratch directory useScratchSettings() was given.
inline bool settingsInScratch() { return !scratchRoot().isEmpty() && settingsInScratch(scratchRoot()); }

// Whether Qt's own settings, the ones QFileDialog uses, are inside dir.
inline bool qtSettingsInScratch(const QString& dir) {
    if (!kQtSettingsRedirectable)
        return false;
    const QSettings qt(QSettings::UserScope, QStringLiteral("QtProject"));
    return inDir(qt.fileName(), dir);
}

// Empties the app's settings to what a fresh run starts with: nothing, except
// that a test window must never discover the caller's local font collection.
// Writes nothing, and returns false, unless the settings are the scratch file.
inline bool resetScratchSettings() {
    if (!settingsInScratch())
        return false;
    const auto s = bw::appSettings();
    s->clear();
    s->setValue(QStringLiteral("fonts/folder"), QString());
    return true;
}

[[noreturn]] inline void refuseToRun() {
    std::fprintf(stderr, "the app's settings or Qt's are not in the scratch directory; not running\n");
    std::exit(1);
}

// Points the app's settings at settings.ini in dir and, where Qt allows it,
// every native and INI settings path at dir/qt, then empties the settings.
// Called before any window is built; the run stops, having written nothing,
// if dir couldn't be made or either kind of settings would still use another
// file.
inline void useScratchSettings(const QTemporaryDir& dir) {
    if (!dir.isValid())
        refuseToRun();
    scratchRoot() = dir.path();
    scratchSettingsFile() = dir.filePath(QStringLiteral("settings.ini"));
    bw::setSettingsFile(scratchSettingsFile());
    if (kQtSettingsRedirectable) {
        for (const auto format : {QSettings::NativeFormat, QSettings::IniFormat}) {
            QSettings::setPath(format, QSettings::UserScope, dir.filePath(QStringLiteral("qt/user")));
            QSettings::setPath(format, QSettings::SystemScope, dir.filePath(QStringLiteral("qt/system")));
        }
    }
    if (!settingsInScratch() || (kQtSettingsRedirectable && !qtSettingsInScratch(dir.path())))
        refuseToRun();
    resetScratchSettings();
}

} // namespace bwtest
