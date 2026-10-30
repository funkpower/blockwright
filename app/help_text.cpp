#include "help_text.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>

namespace bw::help {

namespace {

// The file's text, trimmed; empty when it isn't there. Read once.
QString resource(const QString& path) {
    static QHash<QString, QString> cache;
    const auto it = cache.constFind(path);
    if (it != cache.constEnd())
        return *it;
    QString text;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        text = QString::fromUtf8(f.readAll()).trimmed();
    cache.insert(path, text);
    return text;
}

QStringList paragraphs(const QString& markdown) {
    static const QRegularExpression blank(QStringLiteral("\\n\\s*\\n"));
    QStringList out;
    for (const QString& p : markdown.split(blank, Qt::SkipEmptyParts))
        out << p.simplified();
    return out;
}

} // namespace

QString toolText(const QString& id) {
    if (id.isEmpty() || id.contains(QLatin1Char('/')))
        return {};
    return resource(QStringLiteral(":/help/tools/%1.md").arg(id));
}

QString toolSummary(const QString& id) {
    const QStringList p = paragraphs(toolText(id));
    return p.isEmpty() ? QString() : plainText(p.front());
}

QStringList toolIds() {
    QStringList ids;
    for (const QString& f : QDir(QStringLiteral(":/help/tools")).entryList({QStringLiteral("*.md")}, QDir::Files))
        ids << f.chopped(3);
    ids.sort();
    return ids;
}

QString layerText(LayerKind kind) {
    const std::string_view name = layerKindName(kind);
    if (name.empty())
        return {};
    return resource(QStringLiteral(":/help/layers/%1.md").arg(QString::fromUtf8(name.data(), qsizetype(name.size()))));
}

QString plainText(const QString& markdown) {
    return paragraphs(markdown).join(QLatin1Char('\n')).remove(QStringLiteral("**"));
}

} // namespace bw::help
