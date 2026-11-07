#pragma once
// Help > Keyboard Shortcuts...: every action of the window that has a key,
// grouped by the menu it is in (a dock's, for an action in no menu), with a
// search field that filters by name or by key. Read-only. The list is built
// from the window's actions each time the sheet opens, so a new action
// shows up in it without anything here being edited.

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>

class QAction;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace bw {

struct ShortcutEntry {
    QString group;  // "Edit", "Edit > Symmetry", a dock's title, or "Other"
    QString name;   // shortcutName() of the action's text
    QString keys;   // its keys in native text, ", " between them
    QAction* action;
};

// An action's text as the sheet shows it: the mnemonic '&' gone ("&&" is a
// literal '&'), and with it anything after a tab and a trailing "..." or "…".
QString shortcutName(const QString& text);

// Every action whose key can fire in `window`, each once: the menu bar's
// and its menus', in menu order; then those added to the window or to a
// widget in it, and their menus', whoever owns them; then those it owns and
// has added to nothing.
QList<QAction*> windowActions(QWidget* window);

// The dynamic property, a QString, of an action a policy of the document
// can hide (setVisible(false)) that stays an action of the app while hidden
// (#400): it says where the action is offered, as a phrase that ends "only
// ..." ("in IRC and UTF-8 documents" for the Unicode glyph sets 17-21).
// Such an action is listed, and can be given a key, whether it is shown or
// not: bindableActions(), shortcutEntries(), --list-actions ("offeredWhen")
// and Preferences > Shortcuts, which says when it is hidden. Its key is
// kept while it is hidden and counts against every other action's, though
// Qt fires it only while it is shown.
inline constexpr char kOfferedWhenProperty[] = "bwOfferedWhen";

// Whether `a` carries kOfferedWhenProperty, so that hidden it still counts.
bool offeredByPolicy(const QAction* a);
// For an action a policy hides now (offeredByPolicy() and not visible), a
// sentence for its tooltip saying so; empty otherwise.
QString policyHiddenNote(const QAction* a);

// The windowActions() that have a key and are visible (or offeredByPolicy()): the menu bar's
// grouped by menu, the rest by dock or under "Other". A key shown after a
// tab in an action's text (Edit > Delete's, which the canvas handles)
// counts as one, and a list of them there as each. With `keyedOnly` false, those without a key too.
QList<ShortcutEntry> shortcutEntries(QWidget* window, bool keyedOnly = true);

struct ShortcutClash {
    QKeySequence key;
    QAction* first;   // the windowActions() order: `first` came before
    QAction* second;
};

// The keys two of windowActions() share, enabled or not, visible or not:
// one for each action that takes a key an earlier one has.
QList<ShortcutClash> shortcutClashes(QWidget* window);

class ShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    // The sheet for `window`, which is also its parent.
    explicit ShortcutsDialog(QWidget* window);

    // Rebuilds the list from the window's actions, keeping the filter.
    void reload();
    // Shows only the actions whose name or keys contain `text`, ignoring
    // case, and the groups that still have one. Empty shows them all.
    void setFilter(const QString& text);

    QLineEdit* filterEdit() const { return filter_; }
    QTreeWidget* tree() const { return tree_; }

private:
    QWidget* window_;
    QLineEdit* filter_;
    QTreeWidget* tree_;
    QHash<QAction*, QTreeWidgetItem*> rows_;  // reload()'s row for each action
};

// Shows `window`'s sheet, reloaded. The window has one, made the first time
// and hidden, not deleted, when it is closed.
ShortcutsDialog* showShortcuts(QWidget* window);

} // namespace bw
