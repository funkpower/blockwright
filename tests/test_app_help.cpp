// In-app help (#153): every tool in the registry has help text, and the
// toolbox tooltips, the Tool Options help panel and Help > Manual all read
// it from the one store (help_text.h). main() is in test_app.cpp.

#include "help_text.h"
#include "main_window.h"
#include "manual_dialog.h"
#include "scratch_settings.h"
#include "shortcuts_dialog.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/layer.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QToolButton>

using namespace bw;

namespace {

QStringList registryIds(const MainWindow& w) {
    QStringList ids;
    for (const Tool* t : w.tools()->tools())
        ids << t->id();
    return ids;
}

QString plainName(const Tool* t) { return QString(t->name()).remove(QLatin1Char('&')); }

QString scratchDir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }

} // namespace

TEST_CASE("every tool in the registry has help, and no help is left over", "[help]") {
    MainWindow w;
    const QStringList ids = registryIds(w);
    REQUIRE(!ids.isEmpty());
    for (const QString& id : ids) {
        INFO("tool " << id.toStdString());
        const QString text = help::toolText(id);
        CHECK(!text.isEmpty());
        const QString summary = help::toolSummary(id);
        CHECK(!summary.isEmpty());
        // The summary is one plain sentence or two: it goes into tooltips.
        CHECK(!summary.contains(QLatin1Char('\n')));
        CHECK(!summary.contains(QLatin1Char('*')));
        CHECK(!summary.contains(QLatin1Char('`')));
        CHECK(text.startsWith(summary));
    }
    QStringList sorted = ids;
    sorted.sort();
    CHECK(help::toolIds() == sorted);
    // and every file in the source tree is bundled: one left out of the
    // resource list in app/CMakeLists.txt is help no tool shows
    QStringList sources;
    for (const QString& f :
         QDir(QStringLiteral(BW_HELP_SOURCE "/tools")).entryList({QStringLiteral("*.md")}, QDir::Files))
        sources << f.chopped(3);
    sources.sort();
    CHECK(sources.join(QLatin1Char(' ')).toStdString() == sorted.join(QLatin1Char(' ')).toStdString());
    CHECK(help::toolText(QStringLiteral("noSuchTool")).isEmpty());
    CHECK(help::toolText(QStringLiteral("../layers/cells")).isEmpty());
}

TEST_CASE("every kind of layer has help", "[help]") {
    for (LayerKind k : {LayerKind::Cells, LayerKind::Pixels, LayerKind::Text, LayerKind::Generator,
                        LayerKind::Reference}) {
        INFO("kind " << layerKindName(k));
        CHECK(!help::layerText(k).isEmpty());
    }
}

TEST_CASE("plain text drops the markup and keeps a paragraph a line", "[help]") {
    CHECK(help::plainText(QStringLiteral("One **bold**\nline.\n\nTwo.\n")) == QStringLiteral("One bold line.\nTwo."));
}

TEST_CASE("each tool's toolbox tooltip and status tip carry its summary", "[help]") {
    MainWindow w;
    for (const Tool* t : w.tools()->tools()) {
        INFO("tool " << t->id().toStdString());
        auto* a = w.findChild<QAction*>(t->id() + QStringLiteral("Tool"));
        REQUIRE(a);
        const QString summary = help::toolSummary(t->id());
        CHECK(a->toolTip().startsWith(plainName(t)));
        CHECK(a->toolTip().contains(summary));
        CHECK(a->statusTip() == summary);
    }
}

TEST_CASE("the Tool Options help panel follows the active tool and folds away", "[help]") {
    REQUIRE(bwtest::settingsInScratch(scratchDir()));
    appSettings()->remove(QStringLiteral("toolOptions/showHelp"));
    {
        MainWindow w;
        ToolOptions* o = w.toolOptions();
        QLabel* help = o->helpLabel();
        QToolButton* toggle = o->helpToggle();
        CHECK(help->textFormat() == Qt::MarkdownText);
        // Shown by default.
        CHECK(toggle->isChecked());
        for (const Tool* t : w.tools()->tools()) {
            INFO("tool " << t->id().toStdString());
            w.tools()->setActive(t->id());
            CHECK(help->text().startsWith(help::toolText(t->id())));
            CHECK(help->text().contains(QStringLiteral("](shortcuts)")));
            CHECK(help->isVisibleTo(o));
            CHECK(toggle->isVisibleTo(o));
        }
        toggle->setChecked(false);
        CHECK(!help->isVisibleTo(o));
        CHECK(toggle->isVisibleTo(o));
        // Hidden stays hidden across a change of tool.
        w.tools()->setActive(QStringLiteral("line"));
        CHECK(!help->isVisibleTo(o));
        CHECK(help->text().startsWith(help::toolText(QStringLiteral("line"))));
        CHECK(appSettings()->value(QStringLiteral("toolOptions/showHelp")).toBool() == false);
    }
    {
        // And across a restart.
        MainWindow w;
        CHECK(!w.toolOptions()->helpToggle()->isChecked());
        CHECK(!w.toolOptions()->helpLabel()->isVisibleTo(w.toolOptions()));
        w.toolOptions()->helpToggle()->setChecked(true);
        CHECK(w.toolOptions()->helpLabel()->isVisibleTo(w.toolOptions()));
    }
    appSettings()->remove(QStringLiteral("toolOptions/showHelp"));
}

TEST_CASE("the help panel's link opens the keyboard shortcuts", "[help]") {
    MainWindow w;
    CHECK(!w.findChild<ShortcutsDialog*>());
    emit w.toolOptions()->helpLabel()->linkActivated(QStringLiteral("shortcuts"));
    auto* d = w.findChild<ShortcutsDialog*>();
    REQUIRE(d);
    CHECK(d->isVisible());
    d->close();
}

TEST_CASE("Help > Manual shows every tool's and layer's help, offline", "[help]") {
    MainWindow w;
    auto* manual = w.findChild<QAction*>(QStringLiteral("manual"));
    REQUIRE(manual);
    QMenu* helpMenu = nullptr;
    for (QAction* a : w.menuBar()->actions())
        if (QMenu* m = QMenu::menuInAction(a); m && m->actions().contains(manual))
            helpMenu = m;
    REQUIRE(helpMenu);
    CHECK(helpMenu->title().remove(QLatin1Char('&')) == QStringLiteral("Help"));
    CHECK(manual->shortcuts().isEmpty());

    manual->trigger();
    auto* d = w.findChild<ManualDialog*>();
    REQUIRE(d);
    CHECK(d->isVisible());
    manual->trigger();
    CHECK(w.findChildren<ManualDialog*>().size() == 1);

    const QString md = manualMarkdown(*w.tools(), &w);
    const QString shown = d->text()->toPlainText();
    for (const Tool* t : w.tools()->tools()) {
        INFO("tool " << t->id().toStdString());
        CHECK(md.contains(QStringLiteral("## ") + plainName(t)));
        CHECK(md.contains(help::toolText(t->id()).trimmed()));
        CHECK(shown.contains(help::toolSummary(t->id())));
    }
    for (LayerKind k : {LayerKind::Cells, LayerKind::Pixels, LayerKind::Text, LayerKind::Generator,
                        LayerKind::Reference}) {
        INFO("kind " << layerKindName(k));
        CHECK(md.contains(help::layerText(k).trimmed()));
    }

    // Its button opens the keyboard shortcuts.
    d->shortcutsButton()->click();
    auto* s = w.findChild<ShortcutsDialog*>();
    REQUIRE(s);
    CHECK(s->isVisible());
    s->close();
    d->close();
}
