#pragma once
// The recent list (PLAN §11): the last kRecentLimit files and projects opened
// or saved, newest first, each once, kept through appSettings() as
// recent/files. MainWindow adds to it at its two choke points, openFile and
// Document::fileWritten; File > Open Recent and the start screen read it.

#include <QString>
#include <QStringList>

namespace bw {

inline constexpr int kRecentLimit = 10;

// The list as saved, newest first: each path once, at most kRecentLimit,
// whatever a hand-edited settings file holds. A path that no longer exists
// is still listed (the start screen shows it disabled).
QStringList recentFiles();
// Puts `path` (made absolute and clean) first, taking it out from further
// down and dropping what falls past kRecentLimit.
void addRecentFile(const QString& path);
// Takes `path` out of the list.
void removeRecentFile(const QString& path);

} // namespace bw
