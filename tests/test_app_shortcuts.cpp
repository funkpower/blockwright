// Help > Keyboard Shortcuts... (#149): the sheet lists the window's own
// actions with their keys, grouped by menu, and filters them; and no two
// of the window's actions share a key. main() is in test_app.cpp.

#include "main_window.h"
#include "shortcuts_dialog.h"

#include "system_input.h"

#include "blockwright/glyph_sets.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHash>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QTreeWidget>

using namespace bw;

namespace {

QAction* action(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

QString native(const QKeySequence& k) { return k.toString(QKeySequence::NativeText); }

// The sheet's row for `name` under `group`, or null.
QTreeWidgetItem* row(const ShortcutsDialog& d, const QString& group, const QString& name) {
    for (int i = 0; i < d.tree()->topLevelItemCount(); ++i) {
        QTreeWidgetItem* g = d.tree()->topLevelItem(i);
        if (g->text(0) != group)
            continue;
        for (int j = 0; j < g->childCount(); ++j)
            if (g->child(j)->text(0) == name)
                return g->child(j);
    }
    return nullptr;
}

// The names of the rows the filter leaves shown.
QStringList shownRows(const ShortcutsDialog& d) {
    QStringList out;
    for (int i = 0; i < d.tree()->topLevelItemCount(); ++i) {
        QTreeWidgetItem* g = d.tree()->topLevelItem(i);
        for (int j = 0; j < g->childCount(); ++j)
            if (!g->isHidden() && !g->child(j)->isHidden())
                out << g->child(j)->text(0);
    }
    return out;
}

QMenu* menu(MainWindow& w, const QString& title) {
    for (QAction* a : w.menuBar()->actions())
        if (QMenu* m = QMenu::menuInAction(a); m && shortcutName(m->title()) == title)
            return m;
    FAIL("no menu " << title.toStdString());
    return nullptr;
}

} // namespace

TEST_CASE("shortcut names lose their mnemonic, ellipsis and tab hint", "[shortcuts]") {
    CHECK(shortcutName(QStringLiteral("Save &As...")) == QStringLiteral("Save As"));
    CHECK(shortcutName(QStringLiteral("P&revious Background")) == QStringLiteral("Previous Background"));
    CHECK(shortcutName(QStringLiteral("Fish && &Chips")) == QStringLiteral("Fish & Chips"));
    CHECK(shortcutName(QStringLiteral("&Open…")) == QStringLiteral("Open"));
    CHECK(shortcutName(QStringLiteral("&Delete\tDel")) == QStringLiteral("Delete"));
    CHECK(shortcutName(QStringLiteral("Wait... What")) == QStringLiteral("Wait... What"));
}

TEST_CASE("Help > Keyboard Shortcuts opens the sheet, and has no key of its own", "[shortcuts]") {
    MainWindow w;
    QAction* sheet = action(w, "keyboardShortcuts");
    CHECK(menu(w, QStringLiteral("Help"))->actions().contains(sheet));
    CHECK(shortcutName(sheet->text()) == QStringLiteral("Keyboard Shortcuts"));
    // F1, the standard help key, is a glyph key in keyboard mode (as are F2
    // to F10), so neither the sheet nor anything else takes one.
    CHECK(sheet->shortcuts().isEmpty());
    for (QAction* a : windowActions(&w))
        for (const QKeySequence& k : a->shortcuts())
            for (int f = 0; f < kGlyphSetKeys; ++f)
                CHECK(k != QKeySequence(Qt::Key(Qt::Key_F1 + f)));

    sheet->trigger();
    auto* d = w.findChild<ShortcutsDialog*>();
    REQUIRE(d);
    CHECK(d->isVisible());
    CHECK(d->windowTitle() == QStringLiteral("Keyboard Shortcuts"));
    // Opening it again raises the same sheet.
    sheet->trigger();
    CHECK(w.findChildren<ShortcutsDialog*>().size() == 1);
    d->close();
}

TEST_CASE("the sheet lists each action with its keys under its menu", "[shortcuts]") {
    MainWindow w;
    ShortcutsDialog d(&w);

    QTreeWidgetItem* r = row(d, QStringLiteral("View"), QStringLiteral("Zoom In"));
    REQUIRE(r);
    CHECK(r->text(1) == native(QKeySequence(Qt::CTRL | Qt::Key_Equal)) + QStringLiteral(", ") +
                            native(QKeySequence(Qt::CTRL | Qt::Key_Plus)));
    // The mnemonic and the ellipsis go.
    r = row(d, QStringLiteral("File"), QStringLiteral("Save As"));
    REQUIRE(r);
    CHECK(r->text(1) == native(QKeySequence(QKeySequence::SaveAs)));
    r = row(d, QStringLiteral("Colours"), QStringLiteral("Previous Background"));
    REQUIRE(r);
    CHECK(r->text(1) == native(QKeySequence(Qt::CTRL | Qt::Key_Left)));
    // The toolbox's buttons are the Tools menu's actions.
    const QString halfBlock = shortcutName(action(w, "halfBlockTool")->text());
    r = row(d, QStringLiteral("Tools"), halfBlock);
    REQUIRE(r);
    CHECK(r->text(1) == native(QKeySequence(Qt::CTRL | Qt::Key_B)));
    // Edit > Delete's key is the canvas's, shown after a tab in its text.
    r = row(d, QStringLiteral("Edit"), QStringLiteral("Delete"));
    REQUIRE(r);
    CHECK(r->text(1) == native(QKeySequence(Qt::Key_Delete)));
    // Nothing without a key, and no name keeps a mnemonic.
    CHECK_FALSE(row(d, QStringLiteral("File"), QStringLiteral("SAUCE")));
    CHECK_FALSE(row(d, QStringLiteral("Help"), QStringLiteral("Keyboard Shortcuts")));
    for (const ShortcutEntry& e : shortcutEntries(&w)) {
        CHECK_FALSE(e.keys.isEmpty());
        CHECK_FALSE(e.name.contains(QLatin1Char('&')));
        CHECK_FALSE(e.name.endsWith(QStringLiteral("...")));
    }
    // Every action with a key is listed once.
    int keyed = 0;
    for (QAction* a : windowActions(&w))
        keyed += a->isVisible() && !a->shortcuts().isEmpty();
    int listed = 0;
    for (const ShortcutEntry& e : shortcutEntries(&w))
        listed += !e.action->shortcuts().isEmpty();
    CHECK(listed == keyed);
}

TEST_CASE("the sheet's filter narrows by name and by key", "[shortcuts]") {
    MainWindow w;
    ShortcutsDialog d(&w);
    const int all = int(shownRows(d).size());
    REQUIRE(all > 20);

    d.filterEdit()->setText(QStringLiteral("zoom"));
    CHECK(shownRows(d) == QStringList{QStringLiteral("Zoom In"), QStringLiteral("Zoom Out")});
    CHECK(d.tree()->topLevelItem(0)->isHidden());  // File has no zoom

    // By key, in native text and ignoring case.
    const QString despeckle = shortcutName(action(w, "despeckleTool")->text());
    d.setFilter(native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)));
    CHECK(shownRows(d) == QStringList{despeckle});
    d.setFilter(native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)).toLower());
    CHECK(shownRows(d) == QStringList{despeckle});
    CHECK(d.filterEdit()->text() == native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)).toLower());

    d.setFilter(QStringLiteral("no such action"));
    CHECK(shownRows(d).isEmpty());
    d.setFilter(QString());
    CHECK(int(shownRows(d).size()) == all);
}

TEST_CASE("an action added at runtime is on the sheet the next time it opens", "[shortcuts]") {
    MainWindow w;
    const QKeySequence key(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Q);
    ShortcutsDialog* d = showShortcuts(&w);
    CHECK_FALSE(row(*d, QStringLiteral("Colours"), QStringLiteral("Invert Colours")));

    QAction* added = menu(w, QStringLiteral("Colours"))->addAction(QStringLiteral("&Invert Colours..."));
    added->setShortcut(key);
    // An action in no menu goes under its dock.
    auto* layers = w.findChild<QDockWidget*>(QStringLiteral("layersDock"));
    REQUIRE(layers);
    auto* docked = new QAction(QStringLiteral("&Merge Down"), layers->widget());
    docked->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_W));
    layers->widget()->addAction(docked);

    d->close();
    d = showShortcuts(&w);
    QTreeWidgetItem* r = row(*d, QStringLiteral("Colours"), QStringLiteral("Invert Colours"));
    REQUIRE(r);
    CHECK(r->text(1) == native(key));
    r = row(*d, layers->windowTitle(), QStringLiteral("Merge Down"));
    REQUIRE(r);
    CHECK(r->text(1) == native(docked->shortcut()));

    // The reload keeps the filter.
    d->setFilter(QStringLiteral("invert"));
    added->setText(QStringLiteral("Invert &All"));
    d = showShortcuts(&w);
    CHECK(shownRows(*d) == QStringList{QStringLiteral("Invert All")});
    d->close();
}

// Disabled actions too: Cut and Copy wait for a selection, Undo for an
// edit, and each takes its key back when it is enabled.
TEST_CASE("no two of the main window's actions share a key, enabled or not", "[shortcuts]") {
    MainWindow w;
    for (const ShortcutClash& c : shortcutClashes(&w)) {
        const auto name = [](const QAction* a) {
            return (a->objectName().isEmpty() ? shortcutName(a->text()) : a->objectName()).toStdString();
        };
        FAIL_CHECK(native(c.key).toStdString() << ": " << name(c.first) << " and " << name(c.second));
    }
    int keys = 0;
    for (QAction* a : windowActions(&w))
        keys += int(a->shortcuts().size());
    CHECK(keys > 40);
}

// An action the window doesn't own, added to it or to one of its widgets,
// is the window's all the same: its key fires there (#195).
TEST_CASE("the sheet and the clash guard see an action the window has but doesn't own", "[shortcuts]") {
    MainWindow w;
    QAction extra(QStringLiteral("External command"), qApp);
    extra.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    w.addAction(&extra);
    auto* layers = w.findChild<QDockWidget*>(QStringLiteral("layersDock"));
    REQUIRE(layers);
    QAction docked(QStringLiteral("&Flatten"), qApp);
    docked.setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_W));
    layers->widget()->addAction(&docked);

    const QList<QAction*> all = windowActions(&w);
    CHECK(all.count(&extra) == 1);
    CHECK(all.count(&docked) == 1);
    CHECK(all.count(action(w, "keyboardTool")) == 1);

    ShortcutsDialog d(&w);
    QTreeWidgetItem* r = row(d, QStringLiteral("Other"), QStringLiteral("External command"));
    REQUIRE(r);
    CHECK(r->text(1) == native(QKeySequence(Qt::CTRL | Qt::Key_K)));
    r = row(d, layers->windowTitle(), QStringLiteral("Flatten"));
    REQUIRE(r);

    const QList<ShortcutClash> clashes = shortcutClashes(&w);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes[0].key == QKeySequence(Qt::CTRL | Qt::Key_K));
    CHECK(QSet<QAction*>{clashes[0].first, clashes[0].second} ==
          QSet<QAction*>{action(w, "keyboardTool"), &extra});
}

TEST_CASE("Tab takes the focus from the search field to the list, and Page Down moves through it (#195)",
          "[shortcuts]") {
    MainWindow w;
    ShortcutsDialog* d = showShortcuts(&w);
    REQUIRE(bwtest::showExposed(d));
    QLineEdit* search = d->filterEdit();
    QTreeWidget* list = d->tree();
    search->setFocus();
    QApplication::processEvents();
    REQUIRE(search->hasFocus());

    bwtest::systemKey(search, Qt::Key_Tab);
    REQUIRE(list->hasFocus());
    bwtest::systemKey(list, Qt::Key_Down);
    const QModelIndex first = list->currentIndex();
    REQUIRE(first.isValid());
    REQUIRE(list->verticalScrollBar()->maximum() > 0);
    bwtest::systemKey(list, Qt::Key_PageDown);
    const QModelIndex paged = list->currentIndex();
    CHECK(list->visualRect(paged).top() > list->visualRect(first).top());
    for (int i = 0; i < 3; ++i)
        bwtest::systemKey(list, Qt::Key_PageDown);
    CHECK(list->verticalScrollBar()->value() > 0);
    CHECK(list->viewport()->rect().intersects(list->visualRect(list->currentIndex())));
    bwtest::systemKey(list, Qt::Key_Tab);
    CHECK_FALSE(list->hasFocus());
    CHECK_FALSE(search->hasFocus());

    // Typing in the search field still filters.
    bwtest::systemKey(list, Qt::Key_Tab, Qt::ShiftModifier);
    REQUIRE(list->hasFocus());
    bwtest::systemKey(list, Qt::Key_Backtab, Qt::ShiftModifier);
    REQUIRE(search->hasFocus());
    for (const char c : {'z', 'o', 'o', 'm'})
        bwtest::systemKey(search, Qt::Key_A + (c - 'a'), Qt::NoModifier, QString(QLatin1Char(c)));
    CHECK(search->text() == QStringLiteral("zoom"));
    CHECK(shownRows(*d) == QStringList{QStringLiteral("Zoom In"), QStringLiteral("Zoom Out")});
    d->close();
}

TEST_CASE("the sheet opened again at once is not the one closing (#195)", "[shortcuts]") {
    MainWindow w;
    showShortcuts(&w)->close();
    QPointer<ShortcutsDialog> reopened = showShortcuts(&w);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(reopened);
    CHECK(reopened->isVisible());
    CHECK(w.findChildren<ShortcutsDialog*>().size() == 1);
    reopened->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(reopened);
    CHECK(showShortcuts(&w) == reopened.data());
    reopened->close();
}
