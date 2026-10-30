#include "shortcut_settings.h"

#include "settings.h"
#include "shortcuts_dialog.h"

#include "blockwright/glyph_sets.h"

#include <QAction>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace bw {

namespace {

constexpr char kDefaultKeys[] = "bwDefaultShortcuts";  // a dynamic property
constexpr int kSearchRole = Qt::UserRole;

QString native(const QList<QKeySequence>& keys) { return QKeySequence::listToString(keys, QKeySequence::NativeText); }

// Remembers `a`'s keys as its default the first time it is asked.
void rememberDefault(QAction* a) {
    if (!a->property(kDefaultKeys).isValid())
        a->setProperty(kDefaultKeys, QKeySequence::listToString(a->shortcuts(), QKeySequence::PortableText));
}

QString nameOf(const QAction* a) { return shortcutName(a->text()); }

// listFromString makes one empty key of an empty string; none is no keys.
QList<QKeySequence> keyList(const QString& portable) {
    QList<QKeySequence> keys = QKeySequence::listFromString(portable, QKeySequence::PortableText);
    keys.removeIf([](const QKeySequence& k) { return k.isEmpty(); });
    return keys;
}

} // namespace

QList<QAction*> bindableActions(QWidget* window) {
    QList<QAction*> out;
    QSet<QString> names;
    for (QAction* a : windowActions(window)) {
        const QString name = a->objectName();
        if (a->isSeparator() || !a->isVisible() || QMenu::menuInAction(a) || name.isEmpty() ||
            name.startsWith(QStringLiteral("_q_")) || names.contains(name))
            continue;
        names.insert(name);
        out.push_back(a);
    }
    return out;
}

QList<QKeySequence> defaultShortcuts(QAction* a) {
    rememberDefault(a);
    return keyList(a->property(kDefaultKeys).toString());
}

QString shortcutRefusal(const QKeySequence& key) {
    if (key.isEmpty())
        return {};
    if (key.count() > 1)
        return ShortcutsEditor::tr("A shortcut is one key, with Ctrl or Shift as it needs.");
    const QKeyCombination c = key[0];
    const Qt::KeyboardModifiers m = c.keyboardModifiers();
    const bool ctrl = m & Qt::ControlModifier, meta = m & Qt::MetaModifier, alt = m & Qt::AltModifier,
               shift = m & Qt::ShiftModifier;
    const int k = c.key();
    switch (k) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_unknown: return ShortcutsEditor::tr("A modifier alone is not a shortcut.");
    default: break;
    }
    if (k >= Qt::Key_F1 && k <= Qt::Key_F35) {
        // PLAN §3: desktops take Alt+F-keys (Alt+F4 closes the window),
        // Ctrl+F-keys (KDE and Xfce switch desktops) and Ctrl+Alt+F-keys
        // (Linux VTs).
        if (alt)
            return ShortcutsEditor::tr("The desktop takes Alt with an F-key.");
        if (ctrl && !shift && !meta)
            return ShortcutsEditor::tr("The desktop takes Ctrl with an F-key; add Shift.");
        // F1 to F10 type the glyph set's glyphs, and so do Shift+F2 to F10;
        // Shift+F1 is What's This.
        const int f = k - Qt::Key_F1;
        if (!ctrl && !meta && f < kGlyphSetKeys && !(shift && f == 0))
            return ShortcutsEditor::tr("Keyboard mode types a glyph with it.");
        return {};
    }
    // The system's: Ctrl+Alt+Del, and Ctrl+Alt+Backspace, which ends the X
    // session where it is on.
    if (ctrl && alt && (k == Qt::Key_Delete || k == Qt::Key_Backspace))
        return ShortcutsEditor::tr("The desktop takes Ctrl+Alt with Del or Backspace.");
    // A key is text unless Ctrl or Meta is held without Alt (PLAN §3), and
    // the keys that aren't text move the cursor or edit in keyboard mode,
    // which leaves them to the actions with Ctrl or Meta held.
    if (!ctrl && !meta)
        return ShortcutsEditor::tr("Keyboard mode types or edits with it; add Ctrl.");
    if (alt && k < Qt::Key_Escape)
        return ShortcutsEditor::tr("Ctrl+Alt with a character is AltGr on Windows, which types.");
    return {};
}

void applyShortcutSettings(QWidget* window) {
    const QList<QAction*> actions = bindableActions(window);
    for (QAction* a : actions)
        rememberDefault(a);
    const auto settings = appSettings();
    settings->beginGroup(QLatin1String(kShortcutsGroup));
    QHash<QAction*, QList<QKeySequence>> want;
    QSet<QKeySequence> kept;
    for (QAction* a : actions) {
        if (!settings->contains(a->objectName()))
            continue;
        QList<QKeySequence> keys;
        const QList<QKeySequence> own = defaultShortcuts(a);  // the platform's Undo keys, say
        for (const QKeySequence& k : keyList(settings->value(a->objectName()).toString()))
            if ((own.contains(k) || shortcutRefusal(k).isEmpty()) && !kept.contains(k)) {
                kept.insert(k);
                keys << k;
            }
        want.insert(a, keys);
    }
    for (QAction* a : actions) {
        if (!want.contains(a)) {
            QList<QKeySequence> keys = defaultShortcuts(a);
            keys.removeIf([&](const QKeySequence& k) { return kept.contains(k); });
            want.insert(a, keys);
        }
        if (a->shortcuts() != want.value(a))
            a->setShortcuts(want.value(a));
    }
}

void keepToolTip(QWidget* target, const QList<QAction*>& actions, std::function<QString()> make) {
    target->setToolTip(make());
    for (QAction* a : actions)
        QObject::connect(a, &QAction::changed, target, [target, make] { target->setToolTip(make()); });
}

ShortcutsEditor::ShortcutsEditor(QWidget* window, QWidget* parent) : QWidget(parent), window_(window) {
    setObjectName(QStringLiteral("shortcutsEditor"));
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("shortcutsEditorFilter"));
    filter_->setPlaceholderText(tr("Search by action or key"));
    filter_->setClearButtonEnabled(true);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("shortcutsEditorTree"));
    tree_->setHeaderLabels({tr("Action"), tr("Shortcut")});
    tree_->setRootIsDecorated(false);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setAllColumnsShowFocus(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(false);

    keyEdit_ = new QKeySequenceEdit(this);
    keyEdit_->setObjectName(QStringLiteral("shortcutKey"));
    keyEdit_->setMaximumSequenceLength(1);
    keyEdit_->setClearButtonEnabled(true);
    keyEdit_->setToolTip(tr("Press the new key for the action picked above"));
    noKey_ = new QPushButton(tr("&No Key"), this);
    noKey_->setObjectName(QStringLiteral("shortcutNoKey"));
    noKey_->setToolTip(tr("Take the action's keys away"));
    default_ = new QPushButton(tr("De&fault"), this);
    default_->setObjectName(QStringLiteral("shortcutDefault"));
    default_->setToolTip(tr("Give the action its default keys back"));
    resetAll_ = new QPushButton(tr("Reset &All"), this);
    resetAll_->setObjectName(QStringLiteral("shortcutResetAll"));
    resetAll_->setToolTip(tr("Give every action its default keys back"));
    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("shortcutMessage"));
    message_->setWordWrap(true);

    auto* keyLabel = new QLabel(tr("&Key:"), this);
    keyLabel->setBuddy(keyEdit_);
    auto* row = new QHBoxLayout;
    row->addWidget(keyLabel);
    row->addWidget(keyEdit_, 1);
    row->addWidget(noKey_);
    row->addWidget(default_);
    row->addWidget(resetAll_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(filter_);
    layout->addWidget(tree_, 1);
    layout->addLayout(row);
    layout->addWidget(message_);

    actions_ = bindableActions(window);
    for (QAction* a : actions_)
        pending_.insert(a, a->shortcuts());
    const QSet<QAction*> bindable(actions_.begin(), actions_.end());
    QHash<QString, QTreeWidgetItem*> groups;
    QFont bold = tree_->font();
    bold.setBold(true);
    for (const ShortcutEntry& e : shortcutEntries(window, false)) {
        if (!bindable.contains(e.action) || rows_.contains(e.action))
            continue;
        QTreeWidgetItem*& g = groups[e.group];
        if (!g) {
            g = new QTreeWidgetItem(tree_, {e.group});
            g->setFont(0, bold);
            g->setFirstColumnSpanned(true);
            g->setFlags(Qt::ItemIsEnabled);
        }
        auto* item = new QTreeWidgetItem(g, {e.name});
        item->setToolTip(0, e.action->toolTip());
        rows_.insert(e.action, item);
        showRow(item);
    }
    tree_->expandAll();

    connect(filter_, &QLineEdit::textChanged, this, &ShortcutsEditor::setFilter);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        selected_ = item ? rows_.key(item, nullptr) : nullptr;
        message_->clear();
        showPicked();
    });
    connect(keyEdit_, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence& key) {
        if (!loading_ && selected_)
            assign(key);
    });
    connect(noKey_, &QPushButton::clicked, this, [this] { assign(QKeySequence()); });
    connect(default_, &QPushButton::clicked, this, &ShortcutsEditor::resetSelected);
    connect(resetAll_, &QPushButton::clicked, this, &ShortcutsEditor::resetAll);
    showPicked();
}

void ShortcutsEditor::select(QAction* a) {
    if (QTreeWidgetItem* item = rows_.value(a))
        tree_->setCurrentItem(item);
}

QAction* ShortcutsEditor::holder(const QKeySequence& key, const QAction* except) const {
    for (QAction* a : actions_)
        if (a != except && pending_.value(a).contains(key))
            return a;
    // and an action the editor can't change, if one has it
    for (QAction* a : windowActions(window_))
        if (a != except && !pending_.contains(a) && a->shortcuts().contains(key))
            return a;
    return nullptr;
}

bool ShortcutsEditor::assign(const QKeySequence& key) {
    if (!selected_)
        return false;
    QString why = shortcutRefusal(key);
    if (why.isEmpty() && !key.isEmpty())
        if (const QAction* other = holder(key, selected_))
            why = tr("%1 is already the key for %2.").arg(key.toString(QKeySequence::NativeText), nameOf(other));
    if (!why.isEmpty()) {
        message_->setText(tr("%1 can't be a shortcut: %2").arg(key.toString(QKeySequence::NativeText), why));
        showPicked();
        return false;
    }
    pending_[selected_] = key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key};
    message_->clear();
    showRow(rows_.value(selected_));
    showPicked();
    return true;
}

void ShortcutsEditor::resetSelected() {
    if (!selected_)
        return;
    QList<QKeySequence> keys;
    QStringList taken;
    for (const QKeySequence& k : defaultShortcuts(selected_)) {
        if (const QAction* other = holder(k, selected_))
            taken << tr("%1 is the key for %2 now.").arg(k.toString(QKeySequence::NativeText), nameOf(other));
        else
            keys << k;
    }
    pending_[selected_] = keys;
    message_->setText(taken.join(QLatin1Char(' ')));
    showRow(rows_.value(selected_));
    showPicked();
}

void ShortcutsEditor::resetAll() {
    for (QAction* a : actions_) {
        pending_[a] = defaultShortcuts(a);
        if (QTreeWidgetItem* item = rows_.value(a))
            showRow(item);
    }
    message_->clear();
    showPicked();
}

void ShortcutsEditor::apply() {
    const auto settings = appSettings();
    settings->beginGroup(QLatin1String(kShortcutsGroup));
    // Each action loses its old keys before any takes them up, so a key
    // moved from one action to another never fires two for a moment.
    for (QAction* a : actions_)
        if (a->shortcuts() != pending_.value(a))
            a->setShortcuts({});
    for (QAction* a : actions_) {
        const QList<QKeySequence> keys = pending_.value(a);
        if (a->shortcuts() != keys)
            a->setShortcuts(keys);
        if (keys == defaultShortcuts(a))
            settings->remove(a->objectName());
        else
            settings->setValue(a->objectName(), QKeySequence::listToString(keys, QKeySequence::PortableText));
    }
    // An open Help > Keyboard Shortcuts sheet shows the new keys.
    if (auto* sheet = window_->findChild<ShortcutsDialog*>(QString(), Qt::FindDirectChildrenOnly))
        sheet->reload();
}

void ShortcutsEditor::setFilter(const QString& text) {
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

void ShortcutsEditor::showRow(QTreeWidgetItem* item) {
    QAction* a = rows_.key(item, nullptr);
    if (!a)
        return;
    const QList<QKeySequence> keys = pending_.value(a);
    item->setText(1, native(keys));
    QFont f = item->font(0);
    f.setItalic(keys != defaultShortcuts(a));  // changed from the default
    item->setFont(0, f);
    item->setFont(1, f);
    item->setData(0, kSearchRole,
                  QStringList{item->text(0), native(keys), QKeySequence::listToString(keys, QKeySequence::PortableText)}
                      .join(QLatin1Char('\n')));
}

void ShortcutsEditor::showPicked() {
    const bool any = selected_ != nullptr;
    keyEdit_->setEnabled(any);
    noKey_->setEnabled(any && !pending_.value(selected_).isEmpty());
    default_->setEnabled(any && pending_.value(selected_) != defaultShortcuts(selected_));
    const QScopedValueRollback guard(loading_, true);
    const QList<QKeySequence> keys = pending_.value(selected_);
    keyEdit_->setKeySequence(keys.isEmpty() ? QKeySequence() : keys.first());
}

} // namespace bw
