#include "shortcuts_dialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFont>
#include <QHash>
#include <QHeaderView>
#include <QKeySequence>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace bw {

namespace {

constexpr int kSearchRole = Qt::UserRole;  // the text the filter looks in

QString keyText(const QAction* a) {
    QStringList keys;
    for (const QKeySequence& k : a->shortcuts())
        keys << k.toString(QKeySequence::NativeText);
    if (const qsizetype tab = a->text().indexOf(QLatin1Char('\t')); tab >= 0)
        if (const QString hint = a->text().mid(tab + 1).trimmed(); !hint.isEmpty() && !keys.contains(hint))
            keys << hint;
    return keys.join(QStringLiteral(", "));
}

// Its keys in portable text as well, so "ctrl" finds them on a Mac too.
QString searchText(const QAction* a, const QString& name, const QString& keys) {
    QStringList s{name, keys};
    for (const QKeySequence& k : a->shortcuts())
        s << k.toString(QKeySequence::PortableText);
    return s.join(QLatin1Char('\n'));
}

// The dock an action in no menu belongs to: an ancestor of the action, or
// of a widget it was added to.
QDockWidget* dockOf(const QAction* a) {
    const auto up = [](const QObject* o) -> QDockWidget* {
        for (; o; o = o->parent())
            if (auto* d = qobject_cast<QDockWidget*>(const_cast<QObject*>(o)))
                return d;
        return nullptr;
    };
    if (QDockWidget* d = up(a))
        return d;
    for (const QObject* o : a->associatedObjects())
        if (QDockWidget* d = up(o))
            return d;
    return nullptr;
}

} // namespace

QString shortcutName(const QString& text) {
    QString s = text.left(text.indexOf(QLatin1Char('\t')));
    QString out;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('&')) {
            if (i + 1 < s.size() && s[i + 1] == QLatin1Char('&'))
                out += s[++i];
            continue;
        }
        out += s[i];
    }
    out = out.trimmed();
    if (out.endsWith(QStringLiteral("...")))
        out.chop(3);
    else if (out.endsWith(QChar(0x2026)))
        out.chop(1);
    return out.trimmed();
}

QList<QAction*> windowActions(QWidget* window) {
    QList<QAction*> out;
    QSet<const QAction*> seen;
    const auto add = [&](const auto& self, const QList<QAction*>& actions) -> void {
        for (QAction* a : actions) {
            if (seen.contains(a))
                continue;
            seen.insert(a);
            out.push_back(a);
            if (QMenu* sub = QMenu::menuInAction(a))
                self(self, sub->actions());
        }
    };
    auto* main = qobject_cast<QMainWindow*>(window);
    if (QMenuBar* bar = main ? main->menuBar() : window->findChild<QMenuBar*>())
        add(add, bar->actions());
    // A widget's actions() are those added to it, which it needn't own.
    add(add, window->actions());
    for (QWidget* w : window->findChildren<QWidget*>())
        add(add, w->actions());
    add(add, window->findChildren<QAction*>());
    return out;
}

QList<ShortcutEntry> shortcutEntries(QWidget* window) {
    QList<ShortcutEntry> out;
    QSet<const QAction*> seen;
    const auto take = [&](QAction* a, const QString& group) {
        if (a->isSeparator() || !a->isVisible() || seen.contains(a))
            return;
        const QString keys = keyText(a);
        if (keys.isEmpty())
            return;
        seen.insert(a);
        out.push_back({group, shortcutName(a->text()), keys, a});
    };
    const auto walk = [&](const auto& self, const QList<QAction*>& actions, const QString& path) -> void {
        for (QAction* a : actions) {
            if (QMenu* sub = QMenu::menuInAction(a)) {
                const QString title = shortcutName(sub->title());
                self(self, sub->actions(), path.isEmpty() ? title : path + QStringLiteral(" > ") + title);
            } else {
                take(a, path);
            }
        }
    };
    auto* main = qobject_cast<QMainWindow*>(window);
    if (QMenuBar* bar = main ? main->menuBar() : window->findChild<QMenuBar*>())
        walk(walk, bar->actions(), QString());
    for (QAction* a : windowActions(window)) {
        const QDockWidget* d = dockOf(a);
        take(a, d ? shortcutName(d->windowTitle()) : ShortcutsDialog::tr("Other"));
    }
    return out;
}

QList<ShortcutClash> shortcutClashes(QWidget* window) {
    QList<ShortcutClash> out;
    QHash<QKeySequence, QAction*> owner;
    for (QAction* a : windowActions(window))
        for (const QKeySequence& k : a->shortcuts()) {
            if (QAction* first = owner.value(k))
                out.push_back({k, first, a});
            else
                owner.insert(k, a);
        }
    return out;
}

ShortcutsDialog::ShortcutsDialog(QWidget* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("shortcutsDialog"));
    setWindowTitle(tr("Keyboard Shortcuts"));
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("shortcutsFilter"));
    filter_->setPlaceholderText(tr("Search by action or key"));
    filter_->setClearButtonEnabled(true);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("shortcutsTree"));
    tree_->setHeaderLabels({tr("Action"), tr("Shortcut")});
    tree_->setRootIsDecorated(false);
    // It takes the focus after the search field, and the current row shows,
    // so the arrow keys and Page Down move through it.
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setAllColumnsShowFocus(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(false);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(filter_);
    layout->addWidget(tree_);
    layout->addWidget(buttons);
    connect(filter_, &QLineEdit::textChanged, this, &ShortcutsDialog::setFilter);
    resize(460, 560);
    reload();
}

void ShortcutsDialog::reload() {
    tree_->clear();
    QHash<QString, QTreeWidgetItem*> groups;
    QFont bold = tree_->font();
    bold.setBold(true);
    for (const ShortcutEntry& e : shortcutEntries(window_)) {
        QTreeWidgetItem*& g = groups[e.group];
        if (!g) {
            g = new QTreeWidgetItem(tree_, {e.group});
            g->setFont(0, bold);
            g->setFirstColumnSpanned(true);
        }
        auto* item = new QTreeWidgetItem(g, {e.name, e.keys});
        item->setData(0, kSearchRole, searchText(e.action, e.name, e.keys));
        item->setToolTip(0, e.action->toolTip());
    }
    tree_->expandAll();
    setFilter(filter_->text());
}

void ShortcutsDialog::setFilter(const QString& text) {
    if (filter_->text() != text)
        filter_->setText(text);  // comes back here through textChanged
    const QString needle = text.trimmed();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* g = tree_->topLevelItem(i);
        bool any = false;
        for (int j = 0; j < g->childCount(); ++j) {
            QTreeWidgetItem* item = g->child(j);
            const bool shown =
                needle.isEmpty() || item->data(0, kSearchRole).toString().contains(needle, Qt::CaseInsensitive);
            item->setHidden(!shown);
            any |= shown;
        }
        g->setHidden(!any);
    }
}

ShortcutsDialog* showShortcuts(QWidget* window) {
    auto* d = window->findChild<ShortcutsDialog*>(QString(), Qt::FindDirectChildrenOnly);
    if (d)
        d->reload();
    else
        d = new ShortcutsDialog(window);
    d->show();
    d->raise();
    d->activateWindow();
    return d;
}

} // namespace bw
