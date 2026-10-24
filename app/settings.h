#pragma once
// The settings the app keeps between runs (the dock layout, PLAN §11). Every
// QSettings in app/ comes from appSettings(), so the tests can point them all
// at one scratch file.

#include <QSettings>
#include <QString>

#include <memory>

namespace bw {

// The named native settings ("Blockwright", "Blockwright"), or the INI file
// setSettingsFile() named.
std::unique_ptr<QSettings> appSettings();

// Keeps the settings in the INI file at path from now on; an empty path goes
// back to the native settings. QSettings::setPath() can't do this: macOS and
// Windows ignore it for the native format.
void setSettingsFile(const QString& path);

} // namespace bw
