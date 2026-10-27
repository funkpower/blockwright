// The toolbox's buttons to assistive tech (PLAN §11, #179): a group's split
// button offers Press, which picks its current tool as a click does, and Show
// Menu, each once, names the current tool and keeps the flyout as its child.
// A button of one tool keeps Qt's own interface.

#include "main_window.h"
#include "tool.h"
#include "toolbox.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QTimer>
#include <QToolButton>

#include <utility>

using namespace bw;

namespace {

QAction* toolAction(MainWindow& w, const QString& id) {
    auto* a = w.findChild<QAction*>(id + QStringLiteral("Tool"));
    REQUIRE(a);
    return a;
}

QAccessibleActionInterface* actionsOf(QAccessibleInterface* iface) {
    REQUIRE(iface);
    QAccessibleActionInterface* actions = iface->actionInterface();
    REQUIRE(actions);
    return actions;
}

// Runs `open`, which opens `menu` in a nested event loop or not at all, and
// says whether the menu was shown. A timer closes the menu either way.
template <typename F>
bool menuShownBy(QMenu* menu, F open) {
    bool shown = false;
    QObject::connect(menu, &QMenu::aboutToShow, menu, [&] { shown = true; });
    QTimer closeMenu;
    closeMenu.setInterval(0);
    QObject::connect(&closeMenu, &QTimer::timeout, menu, [menu] {
        if (menu->isVisible())
            menu->close();
    });
    closeMenu.start();
    open();
    QApplication::processEvents();
    closeMenu.stop();
    QObject::disconnect(menu, &QMenu::aboutToShow, menu, nullptr);
    return shown;
}

} // namespace

TEST_CASE("each toolbox group's button offers Press and Show Menu, once each", "[toolbox-access]") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    QToolButton* b = w.toolbox()->buttonFor(id);
    REQUIRE(b);
    REQUIRE(b->menu());
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(b);
    QStringList names = actionsOf(iface)->actionNames();
    INFO(names.join(QStringLiteral(", ")).toStdString());
    CHECK(names.count(QAccessibleActionInterface::pressAction()) == 1);
    CHECK(names.count(QAccessibleActionInterface::showMenuAction()) == 1);
    CHECK(names.removeDuplicates() == 0);
    CHECK(iface->role() == QAccessible::ButtonDropDown);
    CHECK(iface->state().hasPopup);
}

TEST_CASE("a toolbox group's accessible Press picks its current tool, as a click does", "[toolbox-access]") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    w.show();
    QApplication::processEvents();
    QToolButton* b = w.toolbox()->buttonFor(id);
    REQUIRE(b);
    QMenu* menu = b->menu();
    REQUIRE(menu);
    // a member other than the group's first, then another tool
    QAction* current = menu->actions().last();
    current->trigger();
    REQUIRE(b->defaultAction() == current);
    QAction* keyboard = toolAction(w, QStringLiteral("keyboard"));
    keyboard->trigger();
    REQUIRE_FALSE(b->isChecked());

    // what a click does
    CHECK_FALSE(menuShownBy(menu, [&] { b->click(); }));
    const QString clicked = w.tools()->active()->id();
    CHECK(b->isChecked());
    keyboard->trigger();
    REQUIRE_FALSE(b->isChecked());

    QAccessibleActionInterface* actions = actionsOf(QAccessible::queryAccessibleInterface(b));
    REQUIRE(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
    CHECK_FALSE(menuShownBy(menu, [&] { actions->doAction(QAccessibleActionInterface::pressAction()); }));
    CHECK(w.tools()->active()->id() == clicked);
    CHECK(toolAction(w, clicked) == current);
    CHECK(b->isChecked());
    CHECK(b->defaultAction() == current);
    // and says so: checked once Press picks the tool, unchecked once another is picked
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(b);
    CHECK(iface->state().checkable);
    CHECK(iface->state().checked);
    keyboard->trigger();
    CHECK_FALSE(iface->state().checked);
}

TEST_CASE("a toolbox group's accessible Show Menu opens the group", "[toolbox-access]") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    w.show();
    QApplication::processEvents();
    QToolButton* b = w.toolbox()->buttonFor(id);
    REQUIRE(b);
    QMenu* menu = b->menu();
    REQUIRE(menu);
    const QString activeBefore = w.tools()->active()->id();
    QAccessibleActionInterface* actions = actionsOf(QAccessible::queryAccessibleInterface(b));
    REQUIRE(actions->actionNames().count(QAccessibleActionInterface::showMenuAction()) == 1);
    CHECK(menuShownBy(menu, [&] { actions->doAction(QAccessibleActionInterface::showMenuAction()); }));
    CHECK_FALSE(menu->isVisible());
    CHECK(w.tools()->active()->id() == activeBefore);
}

TEST_CASE("a toolbox group's accessible name and description follow the tool picked from its menu", "[toolbox-access]") {
    MainWindow w;
    QToolButton* b = w.toolbox()->buttonFor(QStringLiteral("line"));
    REQUIRE(b);
    QMenu* menu = b->menu();
    REQUIRE(menu);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(b);
    REQUIRE(iface);
    CHECK(iface->text(QAccessible::Name) == QStringLiteral("Line"));
    CHECK(iface->text(QAccessible::Description).startsWith(QStringLiteral("Line (")));

    QAction* ellipse = toolAction(w, QStringLiteral("ellipse"));
    REQUIRE(menu->actions().contains(ellipse));
    ellipse->trigger();
    REQUIRE(b->defaultAction() == ellipse);
    // the same interface, now about the ellipse
    REQUIRE(QAccessible::queryAccessibleInterface(b) == iface);
    CHECK(iface->text(QAccessible::Name) == QStringLiteral("Ellipse"));
    const QString description = iface->text(QAccessible::Description);
    INFO(description.toStdString());
    CHECK(description.startsWith(QStringLiteral("Ellipse (")));
    for (const QString& member : {QStringLiteral("Line"), QStringLiteral("Rectangle"), QStringLiteral("Ellipse")})
        CHECK(description.contains(member));

    // and Press picks it
    toolAction(w, QStringLiteral("keyboard"))->trigger();
    QAccessibleActionInterface* actions = actionsOf(iface);
    REQUIRE(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
    actions->doAction(QAccessibleActionInterface::pressAction());
    CHECK(w.tools()->active()->id() == QStringLiteral("ellipse"));
}

TEST_CASE("a toolbox group's button keeps its flyout as its accessible child", "[toolbox-access]") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    QToolButton* b = w.toolbox()->buttonFor(id);
    REQUIRE(b);
    QMenu* menu = b->menu();
    REQUIRE(menu);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(b);
    REQUIRE(iface);
    REQUIRE(iface->childCount() == 1);
    QAccessibleInterface* menuIface = iface->child(0);
    REQUIRE(menuIface);
    CHECK(menuIface->object() == menu);
    CHECK(menuIface == QAccessible::queryAccessibleInterface(menu));
    CHECK(iface->indexOfChild(menuIface) == 0);
    CHECK_FALSE(iface->child(1));
    CHECK_FALSE(iface->child(-1));
    // the group's tools, through the flyout
    const QList<QAction*> tools = menu->actions();
    REQUIRE(tools.size() > 1);
    REQUIRE(menuIface->childCount() == tools.size());
    for (int i = 0; i < tools.size(); ++i) {
        QAccessibleInterface* item = menuIface->child(i);
        REQUIRE(item);
        CHECK(item->object() == tools.at(i));
        CHECK(item->text(QAccessible::Name) == tools.at(i)->iconText());
        CHECK(menuIface->indexOfChild(item) == i);
    }
}

TEST_CASE("a toolbox group's button announces its new description when the tool changes", "[toolbox-access]") {
    MainWindow w;
    QToolButton* b = w.toolbox()->buttonFor(QStringLiteral("line"));
    REQUIRE(b);
    QAction* ellipse = toolAction(w, QStringLiteral("ellipse"));
    REQUIRE(b->menu()->actions().contains(ellipse));

    // Offscreen there is no assistive tech, so Qt drops every update unless
    // accessibility is active; a handler stands in for the platform's.
    static QList<std::pair<QAccessible::Event, QObject*>> seen;
    seen.clear();
    struct Listen {
        bool wasActive = QAccessible::isActive();
        QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(
            [](QAccessibleEvent* e) { seen.append({e->type(), e->object()}); });
        Listen() { QAccessible::setActive(true); }
        ~Listen() {
            QAccessible::installUpdateHandler(previous);
            QAccessible::setActive(wasActive);
        }
    };
    {
        Listen listen;
        ellipse->trigger();
    }
    REQUIRE(b->defaultAction() == ellipse);
    CHECK(seen.contains(std::pair{QAccessible::DescriptionChanged, static_cast<QObject*>(b)}));
    // a tool the button already shows changes nothing
    seen.clear();
    {
        Listen listen;
        ellipse->trigger();
    }
    CHECK_FALSE(seen.contains(std::pair{QAccessible::DescriptionChanged, static_cast<QObject*>(b)}));
}

TEST_CASE("a toolbox button of one tool keeps Qt's accessible interface", "[toolbox-access]") {
    const auto id = QString::fromLatin1(GENERATE("keyboard", "select", "eyedropper", "text"));
    INFO(id.toStdString());
    MainWindow w;
    QToolButton* b = w.toolbox()->buttonFor(id);
    REQUIRE(b);
    REQUIRE_FALSE(b->menu());
    // a button the toolbox has nothing to do with, with the same action
    QToolButton plain;
    plain.setAutoRaise(true);
    plain.setFocusPolicy(Qt::NoFocus);
    plain.setToolButtonStyle(Qt::ToolButtonIconOnly);
    plain.setDefaultAction(b->defaultAction());
    QAccessibleInterface* ours = QAccessible::queryAccessibleInterface(b);
    QAccessibleInterface* qts = QAccessible::queryAccessibleInterface(&plain);
    REQUIRE(ours);
    REQUIRE(qts);
    CHECK(ours->role() == qts->role());
    CHECK(ours->text(QAccessible::Name) == qts->text(QAccessible::Name));
    CHECK(ours->text(QAccessible::Description) == qts->text(QAccessible::Description));
    CHECK(actionsOf(ours)->actionNames() == actionsOf(qts)->actionNames());
    CHECK(actionsOf(ours)->actionNames().contains(QAccessibleActionInterface::pressAction()));
    CHECK(ours->childCount() == qts->childCount());
}

TEST_CASE("a split button outside the toolbox keeps Qt's accessible interface", "[toolbox-access]") {
    MainWindow w; // the toolbox's accessibility factory is installed
    QToolButton other;
    other.setObjectName(QStringLiteral("toolbox_elsewhere"));
    auto* menu = new QMenu(&other);
    menu->addAction(QStringLiteral("One"));
    other.setMenu(menu);
    other.setPopupMode(QToolButton::MenuButtonPopup);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&other);
    REQUIRE(iface);
    CHECK(iface->role() == QAccessible::ButtonMenu);
}
