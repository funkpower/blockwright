#include "action_list.h"

#include "help_text.h"
#include "shortcut_settings.h"
#include "shortcuts_dialog.h"

#include <QAction>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMetaEnum>
#include <QSet>

namespace bw {

namespace {

QJsonArray portable(const QList<QKeySequence>& keys) {
    QJsonArray out;
    for (const QKeySequence& k : keys)
        out.append(k.toString(QKeySequence::PortableText));
    return out;
}

// The keys after a tab in the menu text that are not the action's own.
QString hintKeys(const QAction* a) {
    const qsizetype tab = a->text().indexOf(QLatin1Char('\t'));
    if (tab < 0)
        return {};
    QStringList own;
    for (const QKeySequence& k : a->shortcuts())
        own << k.toString(QKeySequence::NativeText);
    QStringList out;
    for (const QString& part : a->text().mid(tab + 1).split(QStringLiteral(", "))) {
        const QString hint = part.trimmed();
        if (hint.isEmpty() || own.contains(hint))
            continue;
        out << QKeySequence::fromString(hint, QKeySequence::NativeText).toString(QKeySequence::PortableText);
    }
    return out.join(QStringLiteral(", "));
}

} // namespace

QJsonArray actionList(QWidget* window) {
    QSet<const QAction*> keyed;
    for (const ShortcutEntry& e : shortcutEntries(window))
        keyed.insert(e.action);
    QSet<const QAction*> named;
    for (const QAction* a : bindableActions(window))
        named.insert(a);
    QJsonArray out;
    for (const ShortcutEntry& e : shortcutEntries(window, false)) {
        const QAction* a = e.action;
        if (!named.contains(a))
            continue;
        QJsonObject o;
        o[QStringLiteral("id")] = a->objectName();
        o[QStringLiteral("menu")] = e.group;
        o[QStringLiteral("path")] = QJsonArray::fromStringList(e.group.split(QStringLiteral(" > ")));
        o[QStringLiteral("text")] = e.name;
        o[QStringLiteral("shortcuts")] = portable(a->shortcuts());
        if (const QVariant key = a->property(kStandardKeyProperty); key.isValid())
            o[QStringLiteral("standardKey")] =
                QLatin1String(QMetaEnum::fromType<QKeySequence::StandardKey>().valueToKey(key.toInt()));
        if (const QString hint = hintKeys(a); !hint.isEmpty())
            o[QStringLiteral("hint")] = hint;
        o[QStringLiteral("statusTip")] = a->statusTip();
        o[QStringLiteral("toolTip")] = a->toolTip();
        o[QStringLiteral("checkable")] = a->isCheckable();
        o[QStringLiteral("checked")] = a->isChecked();
        if (const QString tool = a->property(kToolIdProperty).toString(); !tool.isEmpty()) {
            o[QStringLiteral("tool")] = tool;
            if (!help::toolText(tool).isEmpty())
                o[QStringLiteral("help")] = QStringLiteral("tools/") + tool;
        }
        o[QStringLiteral("inSheet")] = keyed.contains(a);
        out.append(o);
    }
    return out;
}

QByteArray actionListJson(QWidget* window) {
    return QJsonDocument(actionList(window)).toJson(QJsonDocument::Indented);
}

} // namespace bw
