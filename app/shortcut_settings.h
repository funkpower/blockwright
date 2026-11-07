#pragma once
// Edit > Preferences > Shortcuts (#149): any action of the window can be
// given another key, or none, and the change is kept in the settings
// (appSettings) under kShortcutsGroup, one entry per action's objectName in
// portable text. An action with no entry keeps its default. A key has to
// leave keyboard mode alone, so one that types, moves the cursor or is a
// glyph key there is refused, as are the F-key chords desktops take, and
// no two actions share one.

#include <QKeySequence>
#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

class QAction;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace bw {

inline constexpr char kShortcutsGroup[] = "shortcuts";

// The dynamic property an action given a standard key (QKeySequence::Quit,
// ::Undo, ...) carries it in, as the enum's int.
inline constexpr char kStandardKeyProperty[] = "bwStandardKey";

// The keys the platform theme gives standard key `key`, as
// QKeySequence::keyBindings() has them; `a` is marked with the key
// (kStandardKeyProperty). After useLinuxStandardKeys(), the keys a generic
// Linux desktop's theme gives it (Qt's X11 and GNOME schemes) instead,
// whatever the platform and its theme: --list-actions runs offscreen, and
// that theme has the Windows keys.
QList<QKeySequence> standardKeys(QAction* a, QKeySequence::StandardKey key);
void useLinuxStandardKeys(bool on = true);

// The windowActions() a key can be given: visible or hidden by a policy
// (offeredByPolicy()), not a separator or a submenu, with an objectName of
// their own (not Qt's "_q_" ones), each once.
QList<QAction*> bindableActions(QWidget* window);

// The keys `a` had before a setting changed them.
QList<QKeySequence> defaultShortcuts(QAction* a);

// Why `key` can't be a shortcut, or empty if it can (an empty key can).
QString shortcutRefusal(const QKeySequence& key);

// Gives each of bindableActions(window) the keys the settings keep for it,
// or its default. A kept key wins over another action's default, which
// loses it; a kept key that is refused (and not one of the action's own
// defaults), or that an earlier action already keeps, is dropped.
void applyShortcutSettings(QWidget* window);

// Sets `target`'s tooltip to make() now and again whenever one of `actions`
// changes, so a key shown in it follows a rebinding.
void keepToolTip(QWidget* target, const QList<QAction*>& actions, std::function<QString()> make);

// The Shortcuts page of Edit > Preferences: every bindable action under its
// menu with its keys, a search field, and for the action picked a key field
// with No Key and Default, and Reset All. Nothing changes until apply().
class ShortcutsEditor : public QWidget {
    Q_OBJECT
public:
    explicit ShortcutsEditor(QWidget* window, QWidget* parent = nullptr);

    // Picks `a`'s row; the key field then edits it.
    void select(QAction* a);
    QAction* selected() const { return selected_; }
    // Gives the picked action `key` alone (empty for none). Refused, with the
    // reason in message(), when shortcutRefusal() says so or another action
    // has the key here.
    bool assign(const QKeySequence& key);
    // The picked action's default keys, but for any another action has here.
    void resetSelected();
    // Every action's default keys.
    void resetAll();
    // The keys each action has here.
    QList<QKeySequence> keys(QAction* a) const { return pending_.value(a); }
    // Gives the actions their keys here and keeps those that differ from the
    // default in the settings, removing the rest; the window's Keyboard
    // Shortcuts sheet, open or not, is reloaded.
    void apply();

    QLineEdit* filterEdit() const { return filter_; }
    QTreeWidget* tree() const { return tree_; }
    QKeySequenceEdit* keyEdit() const { return keyEdit_; }
    QPushButton* noKeyButton() const { return noKey_; }
    QPushButton* defaultButton() const { return default_; }
    QPushButton* resetAllButton() const { return resetAll_; }
    QLabel* message() const { return message_; }

private:
    void setFilter(const QString& text);
    void showRow(QTreeWidgetItem* item);
    void showPicked();
    QAction* holder(const QKeySequence& key, const QAction* except) const;

    QWidget* window_;
    QList<QAction*> actions_;
    QHash<QAction*, QList<QKeySequence>> pending_;
    QHash<QAction*, QTreeWidgetItem*> rows_;
    QAction* selected_ = nullptr;
    bool loading_ = false;
    QLineEdit* filter_;
    QTreeWidget* tree_;
    QKeySequenceEdit* keyEdit_;
    QPushButton* noKey_;
    QPushButton* default_;
    QPushButton* resetAll_;
    QLabel* message_;
};

} // namespace bw
