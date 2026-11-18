// The tutorials and the manual in the app (#210, PLAN §11): Help > Getting
// Started, Tutorials and Manual, the start screen's Tutorials and Tool
// Options' "More in the manual" open the installed help bundle in the help
// window, or the same page on the website when none is installed. The
// bundle here, tests/data/help-bundle/, is a small stand-in for the one
// the docs repo builds: two tutorials pages, the manual's index and its
// tools page with every tool's section.

#include "util.h"
#include "url_recorder.h"

#include "branding.h"
#include "document.h"
#include "help_docs.h"
#include "help_text.h"
#include "help_window.h"
#include "main_window.h"
#include "manual_dialog.h"
#include "start_screen.h"
#include "tool.h"
#include "tool_options.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdlib>
#include <filesystem>

using namespace bw;

namespace {

QString fixture() { return QString::fromStdString((bwtest::dataDir() / "help-bundle").string()); }

// A copy of the fixture bundle in `dir`, to break.
QString copyFixture(const QTemporaryDir& dir) {
    namespace fs = std::filesystem;
    const fs::path to = fs::path(dir.path().toStdString()) / "help";
    fs::copy(bwtest::dataDir() / "help-bundle", to, fs::copy_options::recursive);
    return QString::fromStdString(to.string());
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

QByteArray readAll(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

// Whether the document shown has an anchor named `name`, as an id the page
// gives an element: what QTextBrowser scrolls to for a fragment.
bool hasAnchor(const QTextDocument* doc, const QString& name) {
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.charFormat().anchorNames().contains(name) || b.blockFormat().property(QTextFormat::AnchorName) == name)
            return true;
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().anchorNames().contains(name))
                return true;
    }
    return false;
}

QAction* action(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

} // namespace

TEST_CASE("A help bundle is read from its bundle.json", "[help-docs]") {
    QString error;
    const helpdocs::Bundle b = helpdocs::read(fixture(), &error);
    INFO(error.toStdString());
    REQUIRE(b.valid());
    CHECK(b.docsCommit == QStringLiteral("0000000"));
    CHECK(b.checkedWith == QStringLiteral("0.0.0 (0000000)"));
    REQUIRE(b.contents.size() == 2);
    CHECK(b.contents[0].title == QStringLiteral("Tutorials"));
    CHECK(b.contents[1].page == QStringLiteral("manual/index.html"));
    QStringList pages;
    for (const helpdocs::Page& p : b.pages())
        pages << p.page;
    CHECK(pages == QStringList{QStringLiteral("tutorials/index.html"),
                               QStringLiteral("tutorials/01-what-ansi-art-is.html"),
                               QStringLiteral("manual/index.html"), QStringLiteral("manual/04-tools.html")});
    CHECK(b.title(QStringLiteral("manual/04-tools.html")) == QStringLiteral("Tools and their options"));

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    SECTION("none there") {
        CHECK_FALSE(helpdocs::read(tmp.path(), &error).valid());
        CHECK(error.contains(QStringLiteral("bundle.json")));
    }
    const QString copy = copyFixture(tmp);
    const QString json = copy + QStringLiteral("/bundle.json");
    SECTION("another format") {
        writeFile(json, readAll(json).replace("\"format\": 1", "\"format\": 2"));
        CHECK_FALSE(helpdocs::read(copy, &error).valid());
        CHECK(error.contains(QStringLiteral("format 1")));
    }
    SECTION("no docs commit") {
        writeFile(json, readAll(json).replace("\"docsCommit\": \"0000000\"", "\"docsCommit\": \"\""));
        CHECK_FALSE(helpdocs::read(copy, &error).valid());
    }
    SECTION("a page outside the bundle") {
        for (const QByteArray& bad : {QByteArray("../outside.html"), QByteArray("/etc/passwd"),
                                      QByteArray("manual/./04-tools.html"), QByteArray("C:/x.html"),
                                      QByteArray("manual\\\\04-tools.html")}) {
            INFO(bad.toStdString());
            writeFile(json, readAll(QDir(fixture()).filePath(QStringLiteral("bundle.json")))
                                .replace("\"manual/04-tools.html\"", "\"" + bad + "\""));
            CHECK_FALSE(helpdocs::read(copy, &error).valid());
        }
    }
    SECTION("not JSON") {
        writeFile(json, "{");
        CHECK_FALSE(helpdocs::read(copy, &error).valid());
    }
    SECTION("no contents: missing, not an array, or empty") {
        const QByteArray good = readAll(QDir(fixture()).filePath(QStringLiteral("bundle.json")));
        const qsizetype at = good.indexOf(",\n  \"contents\"");
        REQUIRE(at > 0);
        for (const QByteArray& bad : {good.left(at) + "\n}\n", good.left(at) + ",\n  \"contents\": {}\n}\n",
                                      good.left(at) + ",\n  \"contents\": []\n}\n",
                                      good.left(at) + ",\n  \"contents\": \"Tutorials\"\n}\n"}) {
            INFO(bad.toStdString());
            writeFile(json, bad);
            error.clear();
            CHECK_FALSE(helpdocs::read(copy, &error).valid());
            CHECK(error.contains(QStringLiteral("has no contents")));
        }
    }
    SECTION("a section whose pages aren't a list") {
        writeFile(json, readAll(json).replace("\"pages\": [", "\"pages\": {\"x\": [").replace("]\n    }", "]}\n    }"));
        CHECK_FALSE(helpdocs::read(copy, &error).valid());
        CHECK(error.contains(QStringLiteral("pages aren't a list")));
    }
}

TEST_CASE("Every tool has a manual section, and the bundle's links resolve", "[help-docs]") {
    const QStringList ids = help::toolIds();
    REQUIRE(ids.size() >= 25);
    for (const QString& id : ids) {
        INFO("tool " << id.toStdString());
        CHECK(helpdocs::toolPage(id).startsWith(QStringLiteral("manual/04-tools.html#")));
    }
    CHECK(helpdocs::toolPage(QStringLiteral("noSuchTool")).isEmpty());

    const helpdocs::Bundle good = helpdocs::read(fixture());
    REQUIRE(good.valid());
    CHECK(helpdocs::check(good, ids) == QStringList());
    CHECK(helpdocs::check(helpdocs::Bundle(), ids) == QStringList{QStringLiteral("no bundle")});

    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString copy = copyFixture(tmp);
    const QString tools = copy + QStringLiteral("/manual/04-tools.html");
    const QString tutorial = copy + QStringLiteral("/tutorials/01-what-ansi-art-is.html");
    const auto problems = [&] { return helpdocs::check(helpdocs::read(copy), ids); };

    SECTION("a tool's section is gone") {
        writeFile(tools, readAll(tools).replace("id=\"ramp-gradient\"", "id=\"gradient\""));
        const QStringList p = problems();
        INFO(p.join(QLatin1Char('\n')).toStdString());
        CHECK(p.contains(QStringLiteral("the tool gradient's section manual/04-tools.html#ramp-gradient isn't in the "
                                        "bundle")));
    }
    SECTION("an image is missing") {
        QFile::remove(copy + QStringLiteral("/samples/01-what-ansi-art-is/final.png"));
        CHECK(problems() == QStringList{QStringLiteral(
                                "tutorials/01-what-ansi-art-is.html: src=\"../samples/01-what-ansi-art-is/final.png\" "
                                "doesn't resolve")});
    }
    SECTION("a link to an anchor that isn't there, on the page or another") {
        writeFile(tutorial, readAll(tutorial)
                                .replace("href=\"#half-blocks\"", "href=\"#quarter-blocks\"")
                                .replace("04-tools.html#half-block-brush", "04-tools.html#full-block-brush"));
        CHECK(problems() ==
              QStringList{QStringLiteral("tutorials/01-what-ansi-art-is.html: "
                                         "href=\"../manual/04-tools.html#full-block-brush\" doesn't resolve"),
                          QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"#quarter-blocks\" doesn't "
                                         "resolve")});
    }
    SECTION("a link out of the bundle") {
        writeFile(copy + QStringLiteral("/tutorials/index.html"),
                  readAll(copy + QStringLiteral("/tutorials/index.html"))
                      .replace("href=\"01-what-ansi-art-is.html\"", "href=\"../../help/tutorials/index.html\""));
        CHECK(problems().size() == 1);
    }
    SECTION("a link with a scheme other than http or https, which the window never opens") {
        writeFile(tutorial, readAll(tutorial) +
                                "<a href=\"https://example.org/a\">ok</a><a href='HTTP://example.org/b'>ok</a>"
                                "<a href=\"mailto:someone@example.org\">m</a><a href=\"javascript:void(0)\">j</a>"
                                "<a href=ftp://example.org/f>f</a><a href=\"//example.org/c\">s</a>"
                                "<img src=\"data:image/png;base64,AAAA\">");
        const QString page = QStringLiteral("tutorials/01-what-ansi-art-is.html: ");
        const QString why = QStringLiteral(" isn't an http or https link, the only schemes the help window opens");
        CHECK(problems() ==
              QStringList{page + QStringLiteral("href=\"mailto:someone@example.org\"") + why,
                          page + QStringLiteral("href=\"javascript:void(0)\"") + why,
                          page + QStringLiteral("href=\"ftp://example.org/f\"") + why,
                          page + QStringLiteral("href=\"//example.org/c\"") + why,
                          page + QStringLiteral("src=\"data:image/png;base64,AAAA\"") + why});
    }
    SECTION("a page in the contents is missing, and so is a page the app opens") {
        QFile::remove(copy + QStringLiteral("/manual/index.html"));
        const QStringList p = problems();
        CHECK(p.contains(QStringLiteral("the app opens manual/index.html, which isn't in the bundle")));
        CHECK(p.contains(QStringLiteral("manual/index.html is in the contents but not in the bundle")));
    }
    SECTION("links in single quotes, percent-encoded and with character references, as a browser reads them") {
        writeFile(copy + QStringLiteral("/tutorials/a b.html"), "<h1 id=\"x y\">A B</h1><p id='single'>s</p>");
        writeFile(copy + QStringLiteral("/tutorials/a&b.html"), "<h1 id=\"t\">A and B</h1>");
        writeFile(tutorial, readAll(tutorial) +
                                "<a href='../manual/04-tools.html#fill'>fill</a>"
                                "<a href=\"a%20b.html#x%20y\">ab</a><a href='a%20b.html#single'>s</a>"
                                "<a href=\"a&amp;b.html#t\">amp</a><a href=\"a&#38;b.html\">dec</a>"
                                "<a href=\"a&#x26;b.html\">hex</a><a href = 'a%26b.html'>pct</a>"
                                "<img src='../samples/01-what-ansi-art-is/final.png'>");
        CHECK(problems() == QStringList());
        // and each is still checked
        writeFile(tutorial, readAll(tutorial) + "<a href='../manual/04-tools.html#flood'>x</a>"
                                                "<a href=\"a%20c.html\">y</a><a href=\"a&amp;c.html\">z</a>"
                                                "<img src='../samples/none.png'>");
        CHECK(problems() ==
              QStringList{QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"../manual/04-tools.html#flood\" "
                                         "doesn't resolve"),
                          QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"a%20c.html\" doesn't resolve"),
                          QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"a&amp;c.html\" doesn't resolve"),
                          QStringLiteral("tutorials/01-what-ansi-art-is.html: src=\"../samples/none.png\" doesn't "
                                         "resolve")});
    }
    SECTION("uppercase attributes and unquoted values, as a browser reads them") {
        writeFile(tutorial, readAll(tutorial) + "<a HREF=\"upper.html\">u</a> <a href=bare.html>b</a>"
                                                "<IMG SRC=none.png><a href=../manual/04-tools.html#fill>ok</a>");
        CHECK(problems() ==
              QStringList{QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"upper.html\" doesn't resolve"),
                          QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"bare.html\" doesn't resolve"),
                          QStringLiteral("tutorials/01-what-ansi-art-is.html: src=\"none.png\" doesn't resolve")});
        // an unquoted or uppercase id is an anchor
        writeFile(copy + QStringLiteral("/tutorials/anch.html"), "<h1 ID=up>U</h1><p name=bare>b</p>");
        writeFile(tutorial, readAll(tutorial) + "<a href=anch.html#up>1</a><a href=anch.html#bare>2</a>");
        CHECK(problems().size() == 3);
    }
#ifndef Q_OS_WIN
    SECTION("a link through a symlink out of the bundle, which the window refuses") {
        writeFile(tmp.path() + QStringLiteral("/outside.html"), "<h1 id=\"x\">Outside</h1>");
        REQUIRE(QFile::link(tmp.path() + QStringLiteral("/outside.html"), copy + QStringLiteral("/tutorials/away.html")));
        REQUIRE(QFile::link(tmp.path(), copy + QStringLiteral("/tutorials/up")));
        REQUIRE(QFile::link(copy + QStringLiteral("/tutorials/index.html"),
                            copy + QStringLiteral("/tutorials/again.html")));
        writeFile(tutorial, readAll(tutorial) + "<a href=\"away.html#x\">a</a><a href=\"up/outside.html\">b</a>"
                                                "<a href=\"again.html\">inside</a>");
        CHECK(problems() ==
              QStringList{QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"away.html#x\" doesn't resolve"),
                          QStringLiteral("tutorials/01-what-ansi-art-is.html: href=\"up/outside.html\" doesn't "
                                         "resolve")});
    }
#endif
    SECTION("links to the web aren't the bundle's to check") {
        writeFile(tutorial, readAll(tutorial) + "<a href=\"https://example.org/x\">x</a>"
                                                "<a href=\"http://example.com/\">y</a>");
        CHECK(problems() == QStringList());
    }
}

TEST_CASE("The installed bundle is found beside the samples", "[help-docs]") {
    const QString dir = helpdocs::installedDir();
    CHECK(dir.endsWith(QStringLiteral("share/blockwright/help")));
}

TEST_CASE("With no help bundle, Help's pages open on the website", "[help-docs]") {
    QTemporaryDir empty;
    REQUIRE(empty.isValid());
    UrlRecorder browser;
    MainWindow w;
    w.setHelpDir(empty.path());
    CHECK_FALSE(w.helpBundle().valid());

    action(w, "gettingStarted")->trigger();
    action(w, "tutorials")->trigger();
    action(w, "manual")->trigger();
    w.tools()->setActive(QStringLiteral("halfBlock"));
    emit w.toolOptions()->helpLabel()->linkActivated(QStringLiteral("manual"));
    CHECK(browser.urls ==
          QList<QUrl>{QUrl(QStringLiteral("https://blockwright.pumpin.online/tutorials/01-what-ansi-art-is.html")),
                      QUrl(QStringLiteral("https://blockwright.pumpin.online/tutorials/index.html")),
                      QUrl(QStringLiteral("https://blockwright.pumpin.online/manual/index.html")),
                      QUrl(QStringLiteral("https://blockwright.pumpin.online/manual/04-tools.html#half-block-brush"))});
    CHECK_FALSE(w.helpWindow());

    // About says nothing of a bundle.
    action(w, "about")->trigger();
    auto* about = w.findChild<AboutDialog*>();
    REQUIRE(about);
    CHECK(about->findChild<QLabel*>(QStringLiteral("aboutHelp"))->isHidden());
    about->close();
}

TEST_CASE("Help's menu: Getting Started, Tutorials, Manual and the Tool Reference", "[help-docs]") {
    MainWindow w;
    QMenu* help = nullptr;
    for (QAction* m : w.menuBar()->actions())
        if (m->menu() && m->menu()->actions().contains(action(w, "manual")))
            help = m->menu();
    REQUIRE(help);
    QStringList names;
    for (QAction* a : help->actions())
        if (!a->objectName().isEmpty())
            names << a->objectName();
    const qsizetype at = names.indexOf(QStringLiteral("gettingStarted"));
    REQUIRE(at >= 0);
    CHECK(names.mid(at, 4) == QStringList{QStringLiteral("gettingStarted"), QStringLiteral("tutorials"),
                                          QStringLiteral("manual"), QStringLiteral("toolReference")});
    // No keys: F1 types in keyboard mode, Shift+F1 is What's This?.
    for (const char* name : {"gettingStarted", "tutorials", "manual", "toolReference"})
        CHECK(action(w, name)->shortcuts().isEmpty());

    action(w, "toolReference")->trigger();
    auto* d = w.findChild<ManualDialog*>();
    REQUIRE(d);
    CHECK(d->isVisible());
    d->close();
}

TEST_CASE("The help window shows the bundle's pages", "[help-docs]") {
    UrlRecorder browser;
    MainWindow w;
    w.setHelpDir(fixture());
    REQUIRE(w.helpBundle().valid());

    action(w, "manual")->trigger();
    HelpWindow* h = w.helpWindow();
    REQUIRE(h);
    CHECK(h->isVisible());
    CHECK(h->isWindow());
    CHECK(h->currentPage() == QStringLiteral("manual/index.html"));
    CHECK(h->windowTitle().startsWith(QStringLiteral("Manual")));
    CHECK(browser.urls.isEmpty());

    // The contents: each section with its pages, the page shown current.
    QTreeWidget* tree = h->contents();
    REQUIRE(tree->topLevelItemCount() == 2);
    CHECK(tree->topLevelItem(0)->text(0) == QStringLiteral("Tutorials"));
    REQUIRE(tree->topLevelItem(0)->childCount() == 1);
    CHECK(tree->topLevelItem(0)->child(0)->text(0) == QStringLiteral("What ANSI art is"));
    CHECK(tree->currentItem() == tree->topLevelItem(1));
    emit tree->itemClicked(tree->topLevelItem(0)->child(0), 0);
    CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html"));
    CHECK(tree->currentItem() == tree->topLevelItem(0)->child(0));

    // Its image loads from the bundle.
    const QVariant image = h->browser()->document()->resource(
        QTextDocument::ImageResource,
        QUrl::fromLocalFile(fixture() + QStringLiteral("/samples/01-what-ansi-art-is/final.png")));
    CHECK(image.isValid());

    SECTION("one window, a page at a time") {
        action(w, "gettingStarted")->trigger();
        CHECK(w.helpWindow() == h);
        CHECK(w.findChildren<HelpWindow*>().size() == 1);
        CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html"));
        action(w, "tutorials")->trigger();
        CHECK(h->currentPage() == QStringLiteral("tutorials/index.html"));
    }
    SECTION("internal links, anchors, back and forward") {
        h->followLink(QUrl(QStringLiteral("../manual/04-tools.html#half-block-brush")));
        CHECK(h->currentPage() == QStringLiteral("manual/04-tools.html#half-block-brush"));
        CHECK(hasAnchor(h->browser()->document(), QStringLiteral("half-block-brush")));
        CHECK(tree->currentItem() == tree->topLevelItem(1)->child(0));
        h->followLink(QUrl(QStringLiteral("../tutorials/01-what-ansi-art-is.html#half-blocks")));
        CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html#half-blocks"));
        h->followLink(QUrl(QStringLiteral("#try-it")));
        CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html#try-it"));
        REQUIRE(h->backButton()->isEnabled());
        CHECK_FALSE(h->forwardButton()->isEnabled());
        h->backButton()->click();
        CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html#half-blocks"));
        h->backButton()->click();
        CHECK(h->currentPage() == QStringLiteral("manual/04-tools.html#half-block-brush"));
        REQUIRE(h->forwardButton()->isEnabled());
        h->forwardButton()->click();
        CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html#half-blocks"));
        CHECK(browser.urls.isEmpty());
    }
    SECTION("links out of the bundle go to the browser") {
        h->followLink(QUrl(QStringLiteral("https://example.org/ansi")));
        CHECK(browser.urls == QList<QUrl>{QUrl(QStringLiteral("https://example.org/ansi"))});
        CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html"));
        // An image or another file in the bundle, too; nothing outside it.
        h->followLink(QUrl(QStringLiteral("../samples/01-what-ansi-art-is/final.png")));
        REQUIRE(browser.urls.size() == 2);
        CHECK(browser.urls[1] ==
              QUrl::fromLocalFile(fixture() + QStringLiteral("/samples/01-what-ansi-art-is/final.png")));
        h->followLink(QUrl(QStringLiteral("../../help-bundle.html")));
        CHECK(browser.urls.size() == 2);
        CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html"));
    }
    SECTION("a starting file opens as an untitled copy") {
        h->followLink(QUrl(QStringLiteral("../samples/01-what-ansi-art-is/try.ans")));
        const Document& doc = *w.document();
        CHECK(doc.path().isEmpty());
        CHECK_FALSE(doc.isModified());
        CHECK(doc.canvas().at(0, 0).ch == U'T');
        CHECK(doc.canvas().at(2, 0).ch == U'Y');
        CHECK(browser.urls.isEmpty());
    }
    SECTION("Tool Options' link opens the tool's section") {
        w.tools()->setActive(QStringLiteral("gradient"));
        CHECK(w.toolOptions()->helpLabel()->text().contains(QStringLiteral("[More in the manual](manual)")));
        emit w.toolOptions()->helpLabel()->linkActivated(QStringLiteral("manual"));
        CHECK(h->currentPage() == QStringLiteral("manual/04-tools.html#ramp-gradient"));
        CHECK(hasAnchor(h->browser()->document(), QStringLiteral("ramp-gradient")));
    }
    SECTION("search over titles and text") {
        // The page with it in its title first, then the pages with it in
        // their text, in contents order.
        CHECK(h->search(QStringLiteral("what ANSI")) ==
              QStringList{QStringLiteral("tutorials/01-what-ansi-art-is.html"), QStringLiteral("tutorials/index.html")});
        CHECK(h->search(QStringLiteral("half-block brush")) ==
              QStringList{QStringLiteral("tutorials/01-what-ansi-art-is.html"), QStringLiteral("manual/04-tools.html")});
        CHECK(h->search(QStringLiteral("RAMP gradient")) == QStringList{QStringLiteral("manual/04-tools.html")});
        CHECK(h->search(QStringLiteral("ramp nowhere")).isEmpty());
        CHECK(h->search(QStringLiteral("  ")).isEmpty());

        h->searchBox()->setText(QStringLiteral("eyedropper"));
        CHECK(h->results()->isVisible());
        CHECK_FALSE(tree->isVisible());
        REQUIRE(h->results()->count() == 1);
        CHECK(h->results()->item(0)->text() == QStringLiteral("Tools and their options"));
        emit h->results()->itemActivated(h->results()->item(0));
        CHECK(h->currentPage() == QStringLiteral("manual/04-tools.html"));
        h->searchBox()->setText(QStringLiteral("zzzz"));
        REQUIRE(h->results()->count() == 1);
        CHECK(h->results()->item(0)->text() == QStringLiteral("Nothing found"));
        CHECK(h->results()->item(0)->flags() == Qt::NoItemFlags);
        h->searchBox()->clear();
        CHECK(tree->isVisible());
    }
    SECTION("About names the docs commit and the Blockwright it was checked against") {
        action(w, "about")->trigger();
        auto* about = w.findChild<AboutDialog*>();
        REQUIRE(about);
        auto* line = about->findChild<QLabel*>(QStringLiteral("aboutHelp"));
        REQUIRE(line);
        CHECK_FALSE(line->isHidden());
        CHECK(line->text().contains(QStringLiteral("0000000")));
        CHECK(line->text().contains(QStringLiteral("0.0.0 (0000000)")));
        about->close();
    }
    h->close();
}

TEST_CASE("Only pictures, text and PDF in the bundle go to the system's opener", "[help-docs]") {
    UrlRecorder browser;
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString copy = copyFixture(tmp);
    const QString dir = copy + QStringLiteral("/samples/01-what-ansi-art-is/");
    for (const char* name : {"notes.txt", "sheet.PDF", "photo.jpg", "run.sh", "launch.desktop", "app.exe", "page.svg"})
        writeFile(dir + QString::fromLatin1(name), "x");
    HelpWindow h(helpdocs::read(copy), nullptr);
    h.open(QString::fromLatin1(helpdocs::kGettingStartedPage));
    for (const char* name : {"run.sh", "launch.desktop", "app.exe", "page.svg"})
        h.followLink(QUrl(QStringLiteral("../samples/01-what-ansi-art-is/") + QString::fromLatin1(name)));
    CHECK(browser.urls.isEmpty());
    for (const char* name : {"notes.txt", "sheet.PDF", "photo.jpg", "final.png"})
        h.followLink(QUrl(QStringLiteral("../samples/01-what-ansi-art-is/") + QString::fromLatin1(name)));
    CHECK(browser.urls == QList<QUrl>{QUrl::fromLocalFile(dir + QStringLiteral("notes.txt")),
                                      QUrl::fromLocalFile(dir + QStringLiteral("sheet.PDF")),
                                      QUrl::fromLocalFile(dir + QStringLiteral("photo.jpg")),
                                      QUrl::fromLocalFile(dir + QStringLiteral("final.png"))});
    CHECK(h.currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html"));
}

#ifndef Q_OS_WIN
TEST_CASE("A symlink in the bundle doesn't take a link out of it", "[help-docs]") {
    UrlRecorder browser;
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString copy = copyFixture(tmp);
    writeFile(tmp.path() + QStringLiteral("/outside.html"), "<h1>Outside</h1>");
    writeFile(tmp.path() + QStringLiteral("/outside.png"),
              readAll(fixture() + QStringLiteral("/samples/01-what-ansi-art-is/final.png")));
    writeFile(tmp.path() + QStringLiteral("/outside.ans"), "OUT");
    const QString dir = copy + QStringLiteral("/tutorials/");
    REQUIRE(QFile::link(tmp.path() + QStringLiteral("/outside.html"), dir + QStringLiteral("away.html")));
    REQUIRE(QFile::link(tmp.path() + QStringLiteral("/outside.png"), dir + QStringLiteral("away.png")));
    REQUIRE(QFile::link(tmp.path() + QStringLiteral("/outside.ans"), dir + QStringLiteral("away.ans")));
    REQUIRE(QFile::link(tmp.path(), dir + QStringLiteral("up")));
    // A symlink that stays inside is followed.
    REQUIRE(QFile::link(dir + QStringLiteral("index.html"), dir + QStringLiteral("again.html")));
    HelpWindow h(helpdocs::read(copy), nullptr);
    QStringList requested;
    QObject::connect(&h, &HelpWindow::fileRequested, [&](const QString& path) { requested << path; });
    h.open(QString::fromLatin1(helpdocs::kGettingStartedPage));
    for (const char* link : {"away.html", "away.png", "away.ans", "up/outside.html", "up/outside.png", "up/outside.ans"})
        h.followLink(QUrl(QString::fromLatin1(link)));
    CHECK(browser.urls.isEmpty());
    CHECK(requested.isEmpty());
    CHECK(h.currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html"));
    CHECK_FALSE(h.browser()->toPlainText().contains(QStringLiteral("Outside")));
    // Opened by name, it is no page of the bundle.
    h.open(QStringLiteral("tutorials/away.html"));
    CHECK_FALSE(h.browser()->toPlainText().contains(QStringLiteral("Outside")));
    h.followLink(QUrl(QStringLiteral("../samples/01-what-ansi-art-is/try.ans")));
    CHECK(requested.size() == 1);
    h.open(QStringLiteral("tutorials/again.html"));
    CHECK(h.currentPage() == QStringLiteral("tutorials/again.html"));
}
#endif

TEST_CASE("Only the web goes to the system's opener from a link, and a page name is never a scheme",
          "[help-docs]") {
    UrlRecorder browser({QStringLiteral("https"), QStringLiteral("http"), QStringLiteral("file"),
                         QStringLiteral("smb"), QStringLiteral("evil"), QStringLiteral("javascript")});
    HelpWindow h(helpdocs::read(fixture()), nullptr);
    h.open(QString::fromLatin1(helpdocs::kGettingStartedPage));
    for (const char* link : {"smb://example.invalid/share/run.desktop", "evil:payload.html", "javascript:alert(1)"})
        h.followLink(QUrl(QString::fromLatin1(link)));
    CHECK(browser.urls.isEmpty());
    h.followLink(QUrl(QStringLiteral("http://example.com/a")));
    h.followLink(QUrl(QStringLiteral("HTTPS://example.com/b")));
    CHECK(browser.urls.size() == 2);
    browser.urls.clear();
    // a missing page whose name has a colon lands on the website
    h.followLink(QUrl(QStringLiteral("../evil:payload.html")));
    CHECK(browser.urls == QList<QUrl>{QUrl(QStringLiteral("https://blockwright.pumpin.online/evil:payload.html"))});
    CHECK(helpdocs::websiteUrl(QStringLiteral("evil:payload.html")) ==
          QUrl(QStringLiteral("https://blockwright.pumpin.online/evil:payload.html")));
    CHECK(helpdocs::websiteUrl(QStringLiteral("smb://h/x#top")).scheme() == QStringLiteral("https"));
    CHECK(helpdocs::websiteUrl(QStringLiteral("manual/index.html#tools")) ==
          QUrl(QStringLiteral("https://blockwright.pumpin.online/manual/index.html#tools")));
}

TEST_CASE("A page missing from the bundle opens on the website, and the window says so", "[help-docs]") {
    UrlRecorder browser;
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString copy = copyFixture(tmp);
    QFile::remove(copy + QStringLiteral("/manual/index.html"));
    MainWindow w;
    w.setHelpDir(copy);
    action(w, "manual")->trigger();
    HelpWindow* h = w.helpWindow();
    REQUIRE(h);
    CHECK(h->isVisible());
    CHECK(browser.urls == QList<QUrl>{QUrl(QStringLiteral("https://blockwright.pumpin.online/manual/index.html"))});
    const QString text = h->browser()->toPlainText();
    CHECK(text.contains(QStringLiteral("Not in the installed help")));
    CHECK(text.contains(QStringLiteral("manual/index.html")));
    CHECK(text.contains(QStringLiteral("https://blockwright.pumpin.online/manual/index.html")));
    CHECK(h->currentPage().isEmpty());
    // A link to such a page from another page, with its fragment.
    action(w, "gettingStarted")->trigger();
    CHECK(h->currentPage() == QStringLiteral("tutorials/01-what-ansi-art-is.html"));
    h->followLink(QUrl(QStringLiteral("../manual/index.html#tools")));
    REQUIRE(browser.urls.size() == 2);
    CHECK(browser.urls.last() == QUrl(QStringLiteral("https://blockwright.pumpin.online/manual/index.html#tools")));
    CHECK(h->browser()->toPlainText().contains(QStringLiteral("Not in the installed help")));
    // The message's link goes to the website too.
    h->followLink(QUrl(QStringLiteral("https://blockwright.pumpin.online/manual/index.html#tools")));
    CHECK(browser.urls.size() == 3);
    h->open(QStringLiteral("tutorials/index.html"));
    CHECK(h->currentPage() == QStringLiteral("tutorials/index.html"));
    h->close();
}

TEST_CASE("The start screen's Tutorials gives a new canvas with the tutorials open", "[help-docs]") {
    UrlRecorder browser;
    MainWindow w;
    SECTION("from the bundle") {
        w.setHelpDir(fixture());
        StartScreen s(&w);
        s.show();
        auto* button = s.findChild<QPushButton*>(QStringLiteral("startTutorials"));
        REQUIRE(button);
        button->click();
        CHECK_FALSE(s.isVisible());
        REQUIRE(w.helpWindow());
        CHECK(w.helpWindow()->currentPage() == QStringLiteral("tutorials/index.html"));
        CHECK(w.document()->canvas().width() == Canvas::kDefaultWidth);
        CHECK(browser.urls.isEmpty());
        w.helpWindow()->close();
    }
    SECTION("from the website") {
        QTemporaryDir empty;
        REQUIRE(empty.isValid());
        w.setHelpDir(empty.path());
        StartScreen s(&w);
        s.show();
        s.openTutorials();
        CHECK_FALSE(s.isVisible());
        CHECK(browser.urls == QList<QUrl>{QUrl(QStringLiteral("https://blockwright.pumpin.online/tutorials/index.html"))});
    }
}

// The real bundle BW_HELP_BUNDLE installs (#210 slice 4, the help_bundle_check
// test, run when BW_HELP_BUNDLE is set): it reads, and helpdocs::check finds
// nothing wrong with it for the app's own tools.
TEST_CASE("The help bundle this build installs checks clean", "[.][help-bundle-check]") {
    const char* dir = std::getenv("BW_HELP_BUNDLE_CHECK");
    REQUIRE(dir);
    QString error;
    const helpdocs::Bundle b = helpdocs::read(QString::fromLocal8Bit(dir), &error);
    INFO(error.toStdString());
    REQUIRE(b.valid());
    CHECK_FALSE(b.contents.isEmpty());
    const QStringList problems = helpdocs::check(b, help::toolIds());
    INFO(problems.join(QLatin1Char('\n')).toStdString());
    CHECK(problems.isEmpty());
    // And the window opens each of the app's pages from it.
    UrlRecorder browser;
    MainWindow w;
    w.setHelpDir(QString::fromLocal8Bit(dir));
    for (const char* name : {"gettingStarted", "tutorials", "manual"}) {
        action(w, name)->trigger();
        REQUIRE(w.helpWindow());
        CHECK_FALSE(w.helpWindow()->currentPage().isEmpty());
        CHECK_FALSE(w.helpWindow()->browser()->toPlainText().trimmed().isEmpty());
    }
    CHECK(browser.urls.isEmpty());
    w.helpWindow()->close();
}
