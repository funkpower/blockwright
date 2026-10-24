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

} // namespace bw
