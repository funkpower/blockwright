#pragma once
// The settings the app keeps between runs (the dock layout, PLAN §11). Every
// QSettings in app/ comes from appSettings(), so the tests can point them all
// at one scratch file.

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace bw {

// The named native settings ("Blockwright", "Blockwright"), or the INI file
// setSettingsFile() named.
std::unique_ptr<QSettings> appSettings();

// Keeps the settings in the INI file at path from now on; an empty path goes
// back to the native settings. QSettings::setPath() can't do this: macOS and
// Windows ignore it for the native format.
void setSettingsFile(const QString& path);

// While one lives, appSettings() is an empty INI file of its own, removed
// with it, and then the settings are what they were: what is read is the
// defaults and what is written is dropped. The tool illustrations make their
// tools under one (tool_illustration.h).
class ScratchSettings {
public:
    ScratchSettings();
    ~ScratchSettings();
    ScratchSettings(const ScratchSettings&) = delete;
    ScratchSettings& operator=(const ScratchSettings&) = delete;

    // False when no scratch file could be made: the settings are left as
    // they were, and whoever needed them scratch shouldn't go on.
    bool valid() const { return dir_.isValid(); }

private:
    QString before_;
    QTemporaryDir dir_;
};

} // namespace bw
