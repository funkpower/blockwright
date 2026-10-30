#include "settings.h"

namespace bw {

namespace {

QString& settingsFile() {
    static QString path;
    return path;
}

} // namespace

std::unique_ptr<QSettings> appSettings() {
    if (!settingsFile().isEmpty())
        return std::make_unique<QSettings>(settingsFile(), QSettings::IniFormat);
    return std::make_unique<QSettings>(QStringLiteral("Blockwright"), QStringLiteral("Blockwright"));
}

void setSettingsFile(const QString& path) { settingsFile() = path; }

ScratchSettings::ScratchSettings() : before_(settingsFile()) {
    if (dir_.isValid())
        setSettingsFile(dir_.filePath(QStringLiteral("settings.ini")));
}

ScratchSettings::~ScratchSettings() { setSettingsFile(before_); }

} // namespace bw
