// blockwright --list-actions (#209): the JSON every action is dumped as is
// valid, names each action once by a stable id, lists every action in the
// menus, and has exactly the Keyboard Shortcuts sheet's actions as its keyed
// ones, in the sheet's order. Its standard keys are a generic Linux
// desktop's, whatever the platform theme. main() is in test_app.cpp.

#include "action_list.h"
#include "document.h"
#include "help_text.h"
#include "main_window.h"
#include "shortcut_settings.h"
#include "shortcuts_dialog.h"

#include "blockwright/glyph_sets.h"
#include "blockwright/target.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QTreeWidget>
#include <qpa/qplatformtheme.h>
#include <private/qguiapplication_p.h>

using namespace bw;

namespace {

QJsonArray dumped(MainWindow& w) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(actionListJson(&w), &error);
    INFO(error.errorString().toStdString());
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(doc.isArray());
    return doc.array();
}

QJsonObject byId(const QJsonArray& list, const QString& id) {
    for (const QJsonValue& v : list)
        if (v.toObject().value(QStringLiteral("id")).toString() == id)
            return v.toObject();
    FAIL("no action " << id.toStdString());
    return {};
}

// The menus' actions, but not the entries of the lists of files File > Open
// Recent and Help > Samples hold, which are no actions of the app's; those
// a policy of the document hides count (#400).
void menuActions(const QList<QAction*>& actions, QList<QAction*>* out) {
    for (QAction* a : actions) {
        if (QMenu* sub = QMenu::menuInAction(a)) {
            if (sub->objectName() != QStringLiteral("samplesMenu") && sub->objectName() != QStringLiteral("openRecent"))
                menuActions(sub->actions(), out);
        }
        else if (!a->isSeparator() && (a->isVisible() || offeredByPolicy(a)))
            out->push_back(a);
    }
}

// --list-actions' standard keys for as long as it lives.
struct LinuxStandardKeys {
    LinuxStandardKeys() { useLinuxStandardKeys(); }
    ~LinuxStandardKeys() { useLinuxStandardKeys(false); }
};

// The platform theme for as long as it lives is the one it was, but with
// the keyboard scheme of a generic Linux desktop (xcb or Wayland with no
// theme, or GNOME's): the keys a user there gets from Qt.
class X11KeysTheme : public QPlatformTheme {
public:
    X11KeysTheme() : base_(QGuiApplicationPrivate::platform_theme) { QGuiApplicationPrivate::platform_theme = this; }
    ~X11KeysTheme() override { QGuiApplicationPrivate::platform_theme = base_; }
    QVariant themeHint(ThemeHint h) const override {
        return h == KeyboardScheme ? QVariant(int(X11KeyboardScheme)) : base_->themeHint(h);
    }
    const QPalette* palette(Palette type) const override { return base_->palette(type); }
    const QFont* font(Font type) const override { return base_->font(type); }

private:
    QPlatformTheme* base_;
};

QString nativeKeys(const QJsonObject& o) {
    QStringList keys;
    for (const QJsonValue& k : o.value(QStringLiteral("shortcuts")).toArray())
        keys << QKeySequence::fromString(k.toString(), QKeySequence::PortableText).toString(QKeySequence::NativeText);
    if (o.contains(QStringLiteral("hint")))
        for (const QString& h : o.value(QStringLiteral("hint")).toString().split(QStringLiteral(", ")))
            keys << QKeySequence::fromString(h, QKeySequence::PortableText).toString(QKeySequence::NativeText);
    return keys.join(QStringLiteral(", "));
}

} // namespace

TEST_CASE("the action dump is JSON with a unique stable id for each action", "[list-actions]") {
    MainWindow w;
    const QJsonArray list = dumped(w);
    REQUIRE(list.size() > 50);
    QSet<QString> ids;
    for (const QJsonValue& v : list) {
        REQUIRE(v.isObject());
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        INFO(id.toStdString());
        CHECK_FALSE(id.isEmpty());
        CHECK_FALSE(id.startsWith(QStringLiteral("_q_")));
        CHECK_FALSE(ids.contains(id));
        ids.insert(id);
        // the id is the action's objectName, as the shortcut settings key it
        const auto found = w.findChildren<QAction*>(id);
        CHECK_FALSE(found.isEmpty());
        CHECK(o.value(QStringLiteral("menu")).isString());
        CHECK(o.value(QStringLiteral("path")).toArray().size() >= 1);
        CHECK(o.value(QStringLiteral("shortcuts")).isArray());
        CHECK(o.value(QStringLiteral("checkable")).isBool());
        CHECK(o.value(QStringLiteral("checked")).isBool());
        CHECK(o.value(QStringLiteral("statusTip")).isString());
    }
    // Two windows dump the same thing.
    MainWindow other;
    CHECK(actionListJson(&other) == actionListJson(&w));
}

TEST_CASE("the action dump lists exactly the Keyboard Shortcuts sheet's actions as its keyed ones", "[list-actions]") {
    MainWindow w;
    const QJsonArray list = dumped(w);
    // The rows of a real sheet, in its order, against the dump's actions it
    // says the sheet lists, grouped by menu as the sheet groups them.
    ShortcutsDialog dialog(&w);
    QStringList sheet;
    for (int i = 0; i < dialog.tree()->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* g = dialog.tree()->topLevelItem(i);
        for (int j = 0; j < g->childCount(); ++j)
            sheet << g->text(0) + QStringLiteral(" | ") + g->child(j)->text(0) + QStringLiteral(" | ") + g->child(j)->text(1);
    }
    QStringList groups;
    QHash<QString, QStringList> rows;
    for (const QJsonValue& v : list) {
        const QJsonObject o = v.toObject();
        const bool keyed = !o.value(QStringLiteral("shortcuts")).toArray().isEmpty() || o.contains(QStringLiteral("hint"));
        CHECK(o.value(QStringLiteral("inSheet")).toBool() == keyed);
        if (!keyed)
            continue;
        const QString menu = o.value(QStringLiteral("menu")).toString();
        if (!groups.contains(menu))
            groups << menu;
        rows[menu] << menu + QStringLiteral(" | ") + o.value(QStringLiteral("text")).toString() + QStringLiteral(" | ") + nativeKeys(o);
    }
    QStringList inSheet;
    for (const QString& g : groups)
        inSheet << rows.value(g);
    CHECK(inSheet == sheet);
    REQUIRE(sheet.size() > 50);

    // Its keys are the actions' own, in portable text.
    QJsonObject o = byId(list, QStringLiteral("halfBlockTool"));
    CHECK(o.value(QStringLiteral("menu")).toString() == QStringLiteral("Tools"));
    CHECK(o.value(QStringLiteral("shortcuts")).toArray() == QJsonArray{QStringLiteral("Ctrl+B")});
    o = byId(list, QStringLiteral("deleteSelection"));
    CHECK(o.value(QStringLiteral("text")).toString() == QStringLiteral("Delete"));
    CHECK(o.value(QStringLiteral("shortcuts")).toArray().isEmpty());
    CHECK(o.value(QStringLiteral("hint")).toString() == QStringLiteral("Del"));
    o = byId(list, QStringLiteral("symmetryRadial"));
    CHECK(o.value(QStringLiteral("path")).toArray() == QJsonArray{QStringLiteral("Edit"), QStringLiteral("Symmetry")});
    CHECK(o.value(QStringLiteral("checkable")).toBool());
    // An action with no key is in the dump but not the sheet.
    o = byId(list, QStringLiteral("keyboardShortcuts"));
    CHECK(o.value(QStringLiteral("menu")).toString() == QStringLiteral("Help"));
    CHECK_FALSE(o.value(QStringLiteral("inSheet")).toBool());
}

TEST_CASE("the action dump lists every action in the menus, in menu order", "[list-actions]") {
    MainWindow w;
    const QJsonArray list = dumped(w);
    QList<QAction*> inMenus;
    menuActions(w.menuBar()->actions(), &inMenus);
    REQUIRE_FALSE(inMenus.isEmpty());
    QStringList menuIds;
    for (QAction* a : inMenus) {
        INFO(a->text().toStdString());
        CHECK_FALSE(a->objectName().isEmpty());
        if (!menuIds.contains(a->objectName()))
            menuIds << a->objectName();
    }
    QStringList dumpIds;
    for (const QJsonValue& v : list)
        dumpIds << v.toObject().value(QStringLiteral("id")).toString();
    // The menus' come first, as the menus show them; the docks' follow.
    CHECK(dumpIds.mid(0, menuIds.size()) == menuIds);
}

TEST_CASE("every keyed action in the dump has text and a tooltip, and each tool its tool and help ids", "[list-actions]") {
    MainWindow w;
    const QJsonArray list = dumped(w);
    int tools = 0;
    for (const QJsonValue& v : list) {
        const QJsonObject o = v.toObject();
        INFO(o.value(QStringLiteral("id")).toString().toStdString());
        if (o.value(QStringLiteral("inSheet")).toBool()) {
            CHECK_FALSE(o.value(QStringLiteral("text")).toString().trimmed().isEmpty());
            CHECK_FALSE(o.value(QStringLiteral("toolTip")).toString().trimmed().isEmpty());
        }
        if (!o.contains(QStringLiteral("tool")))
            continue;
        ++tools;
        const QString tool = o.value(QStringLiteral("tool")).toString();
        CHECK(o.value(QStringLiteral("id")).toString() == tool + QStringLiteral("Tool"));
        CHECK(o.value(QStringLiteral("menu")).toString() == QStringLiteral("Tools"));
        // #153's store has a file for every tool id
        CHECK(o.value(QStringLiteral("help")).toString() == QStringLiteral("tools/") + tool);
        CHECK(help::toolIds().contains(tool));
    }
    CHECK(tools == help::toolIds().size());
}

TEST_CASE("the action dump has a generic Linux desktop's standard keys and not the offscreen theme's", "[list-actions]") {
    LinuxStandardKeys linuxKeys;
    MainWindow w;
    const QJsonArray list = dumped(w);
    const auto keys = [&](const char* id) { return byId(list, QLatin1String(id)).value(QStringLiteral("shortcuts")).toArray(); };
    const auto strings = [](std::initializer_list<const char*> l) {
        QJsonArray out;
        for (const char* s : l)
            out.append(QLatin1String(s));
        return out;
    };
    // What the app gets on Linux with no theme, or with GNOME's (Qt 6.12, xcb)
    CHECK(keys("newFile") == strings({"Ctrl+N"}));
    CHECK(keys("open") == strings({"Ctrl+O"}));
    CHECK(keys("save") == strings({"Ctrl+S"}));
    CHECK(keys("saveAs") == strings({"Ctrl+Shift+S"}));
    CHECK(keys("quit") == strings({"Ctrl+Q"}));
    CHECK(keys("undo") == strings({"Ctrl+Z", "F14", "Undo"}));
    CHECK(keys("redo") == strings({"Ctrl+Shift+Z", "Redo"}));
    // Keys the window sets itself are its own on every platform.
    CHECK(keys("selectAll") == strings({"Ctrl+A"}));
    CHECK(byId(list, QStringLiteral("quit")).value(QStringLiteral("inSheet")).toBool());
    CHECK(byId(list, QStringLiteral("quit")).value(QStringLiteral("standardKey")).toString() == QStringLiteral("Quit"));
    CHECK(byId(list, QStringLiteral("redo")).value(QStringLiteral("standardKey")).toString() == QStringLiteral("Redo"));
    CHECK(byId(list, QStringLiteral("saveAs")).value(QStringLiteral("standardKey")).toString() == QStringLiteral("SaveAs"));
    CHECK_FALSE(byId(list, QStringLiteral("selectAll")).contains(QStringLiteral("standardKey")));
}

TEST_CASE("the action dump's standard keys are the ones Qt gives a generic Linux desktop", "[list-actions]") {
    // The window as it is made on Linux with no theme, against the window
    // --list-actions makes: every action with a standard key has the same keys.
    QHash<QString, QList<QKeySequence>> desktop;
    {
        X11KeysTheme theme;
        MainWindow w;
        for (QAction* a : w.findChildren<QAction*>())
            if (a->property(kStandardKeyProperty).isValid())
                desktop.insert(a->objectName(), a->shortcuts());
    }
    REQUIRE(desktop.size() >= 7);
    LinuxStandardKeys linuxKeys;
    MainWindow w;
    for (auto it = desktop.cbegin(); it != desktop.cend(); ++it) {
        INFO(it.key().toStdString());
        auto* a = w.findChild<QAction*>(it.key());
        REQUIRE(a);
        CHECK(QKeySequence::listToString(a->shortcuts(), QKeySequence::PortableText).toStdString() ==
              QKeySequence::listToString(it.value(), QKeySequence::PortableText).toStdString());
    }
}

TEST_CASE("the action dump lists the glyph sets an ANSI document hides, and where they are offered", "[list-actions]") {
    // #400: the dump's window has a new ANSI document, whose CP437 typing
    // policy hides sets 17-21; they are actions of the app all the same.
    MainWindow w;
    REQUIRE(w.document()->target() == Target::Ansi);
    for (int i = kCp437GlyphSetCount + 1; i <= kGlyphSetCount; ++i)
        REQUIRE_FALSE(w.findChild<QAction*>(QStringLiteral("glyphSet%1").arg(i))->isVisible());
    const QJsonArray list = dumped(w);
    QStringList sets, want;
    for (const QJsonValue& v : list) {
        const QString id = v.toObject().value(QStringLiteral("id")).toString();
        if (id.startsWith(QStringLiteral("glyphSet")) && id.back().isDigit())
            sets << id;
    }
    for (int i = 1; i <= kGlyphSetCount; ++i)
        want << QStringLiteral("glyphSet%1").arg(i);
    CHECK(sets == want);
    for (int i = 1; i <= kGlyphSetCount; ++i) {
        const QJsonObject o = byId(list, QStringLiteral("glyphSet%1").arg(i));
        INFO(i);
        CHECK(o.value(QStringLiteral("menu")).toString() == QStringLiteral("Glyphs"));
        CHECK(o.value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Set %1: ").arg(i)));
        if (i > kCp437GlyphSetCount)
            CHECK(o.value(QStringLiteral("offeredWhen")).toString() == QStringLiteral("in IRC and UTF-8 documents"));
        else
            CHECK_FALSE(o.contains(QStringLiteral("offeredWhen")));
    }
    // set 21, the sparkles (#399): last, offered as 17-20 are, with no key
    const QJsonObject set21 = byId(list, QStringLiteral("glyphSet21"));
    CHECK(sets.size() == 21);
    CHECK(sets.back() == QStringLiteral("glyphSet21"));
    CHECK(set21.value(QStringLiteral("text")).toString() == QStringLiteral("Set 21: ✦✧✶✷✸✹⋆✱✺❖"));
    CHECK(set21.value(QStringLiteral("offeredWhen")).toString() == QStringLiteral("in IRC and UTF-8 documents"));
    CHECK(set21.value(QStringLiteral("shortcuts")).toArray().isEmpty());
    CHECK_FALSE(set21.value(QStringLiteral("inSheet")).toBool());
    // The same dump from a window whose document offers them: only "checked"
    // follows the document.
    MainWindow irc;
    REQUIRE(irc.document()->setTarget(Target::Irc));
    REQUIRE(irc.findChild<QAction*>(QStringLiteral("glyphSet20"))->isVisible());
    CHECK(byId(dumped(irc), QStringLiteral("glyphSet20")) == byId(list, QStringLiteral("glyphSet20")));
    REQUIRE(irc.findChild<QAction*>(QStringLiteral("glyphSet21"))->isVisible());
    CHECK(byId(dumped(irc), QStringLiteral("glyphSet21")) == set21);
}
