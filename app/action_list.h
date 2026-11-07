#pragma once
// blockwright --list-actions (#209): every action of the window as JSON, so
// the docs site's shortcuts and menu pages (and anything else reading the
// app's actions) are made from the app and can't drift from it. The list is
// the Keyboard Shortcuts sheet's (shortcutEntries), with the actions that
// have no key as well, in the same order: the menus as they show, then the
// docks', then "Other". Only actions with an id are in it, the ones
// Preferences > Shortcuts can give a key (bindableActions): not submenus,
// and not the entries of lists like File > Open Recent and Help > Samples.
// An action the document's policy hides is in it all the same (#400): the
// window's first document is a new ANSI one, which hides glyph sets 17-20.
// The sheet lists no action without an id (a test holds it to that).
//
// One object per action:
//   "id"          its objectName: the stable name settings (shortcuts/<id>)
//                 and scripts use for it
//   "menu"        the sheet's group: "Edit > Symmetry", a dock's title, "Other"
//   "path"        the same as a list, ["Edit", "Symmetry"]
//   "text"        its name as the sheet shows it, no mnemonic or "..."
//   "shortcuts"   its keys in portable text ("Ctrl+Shift+B"), [] for none.
//                 They are the defaults, and a standard key's are the ones a
//                 generic Linux desktop gives it (useLinuxStandardKeys()),
//                 so the dump is the same on every machine
//   "standardKey" for an action whose keys are a standard key's, its name in
//                 QKeySequence::StandardKey ("Quit", "Undo"), so a page for
//                 another platform can name that platform's keys; absent
//                 otherwise
//   "hint"        the key named after a tab in its menu text that is no key of
//                 its own (Edit > Delete's Del), in portable text; absent otherwise
//   "statusTip", "toolTip"
//   "checkable", "checked"
//   "tool"        for a Tools menu action, the tool id; absent otherwise
//   "help"        the help id (help_text.h), "tools/<tool id>", when the tool
//                 has help text; absent otherwise
//   "offeredWhen" for an action a policy of the document can hide, where it
//                 is offered ("in IRC and UTF-8 documents" for glyph sets
//                 17-20), whether it is hidden in the dump's window or not;
//                 absent otherwise
//   "inSheet"     whether Help > Keyboard Shortcuts lists it (it has a key)

#include <QByteArray>
#include <QJsonArray>

class QWidget;

namespace bw {

// The dynamic property a Tools menu action carries its tool id in.
inline constexpr char kToolIdProperty[] = "bwToolId";

QJsonArray actionList(QWidget* window);
// actionList() as indented JSON, a newline at the end.
QByteArray actionListJson(QWidget* window);

} // namespace bw
