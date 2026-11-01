#include "recent_files.h"

#include "settings.h"

#include <QDir>
#include <QFileInfo>

namespace bw {

namespace {

const QString kKey = QStringLiteral("recent/files");

QString normalized(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

void store(const QStringList& list) { appSettings()->setValue(kKey, list); }

} // namespace

QStringList recentFiles() {
    QStringList out;
    for (const QString& p : appSettings()->value(kKey).toStringList())
        if (!p.isEmpty() && !out.contains(p) && out.size() < kRecentLimit)
            out << p;
    return out;
}

void addRecentFile(const QString& path) {
    if (path.isEmpty())
        return;
    const QString p = normalized(path);
    QStringList list = recentFiles();
    list.removeAll(p);
    list.prepend(p);
    while (list.size() > kRecentLimit)
        list.removeLast();
    store(list);
}

void removeRecentFile(const QString& path) {
    QStringList list = recentFiles();
    if (list.removeAll(path) || list.removeAll(normalized(path)))
        store(list);
}

} // namespace bw
