// Text styles in the Library dock (PLAN §12, §15): every bundled preset is
// listed with a thumbnail of its sample, the category list and the search
// box find them as they find stamps, the tooltip says what the style is, and
// a malformed .style.json is skipped and reported like any other bad entry.
// A style that needs iCE colours is shown disabled while the document has
// them off.

#include "document.h"
#include "library_dock.h"
#include "main_window.h"

#include "blockwright/library.h"
#include "blockwright/render.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QFile>
#include <QUndoStack>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QTemporaryDir>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <vector>

using namespace bw;
namespace fs = std::filesystem;

namespace {

const fs::path kBundled = BW_BUNDLED_LIBRARY;

fs::path fspath(const QString& s) { return fs::path(QFile::encodeName(s).toStdString()); }

void put(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// The bundled presets, read straight from library/text-styles/.
std::vector<library::TextStyleEntry> presets() {
    std::vector<library::TextStyleEntry> out;
    for (const fs::directory_entry& f : fs::directory_iterator(kBundled / "text-styles"))
        if (library::isTextStyleName(f.path()))
            out.push_back(library::readTextStyleEntry(f.path()));
    return out;
}

QListWidgetItem* item(LibraryDock& dock, const QString& name) {
    for (int i = 0; i < dock.grid()->count(); ++i)
        if (dock.grid()->item(i)->text() == name)
            return dock.grid()->item(i);
    FAIL("nothing named " << name.toStdString() << " in the grid");
    return nullptr;
}

// A dock over the repo's library (bundled) and a scratch folder (user).
struct Fixture {
    QTemporaryDir user;
    LibraryDock dock;
    Fixture() {
        REQUIRE(user.isValid());
        dock.setDirectories(kBundled, fspath(user.path()));
    }
    fs::path userDir() const { return fspath(user.path()); }
};

} // namespace

TEST_CASE("every bundled text style is in the Library dock with a thumbnail of its sample rendered at 1x") {
    Fixture f;
    const std::vector<library::TextStyleEntry> want = presets();
    REQUIRE(want.size() == 8);
    CHECK(f.dock.styles().size() == want.size());
    CHECK(f.dock.problems().empty());
    const QStringList shown = f.dock.shownNames();
    for (const library::TextStyleEntry& t : want) {
        const QString name = QString::fromStdString(t.name);
        INFO(t.name);
        CHECK(shown.count(name) == 1);
        QListWidgetItem* it = item(f.dock, name);
        REQUIRE(it);
        // The sample `library check` draws, with iCE colours as `library
        // list` sizes it, rendered at 1x and, like a stamp too big for the
        // grid's box, scaled down to fit it (stampIcon).
        const Canvas sample = library::textStyleSample(t.style, true);
        const QSize oneX(sample.width() * 8, sample.height() * 16);
        REQUIRE(renderCanvas(sample, 8).size() == oneX);
        const QSize box = oneX.boundedTo(kIconBox) == oneX ? oneX : oneX.scaled(kIconBox, Qt::KeepAspectRatio);
        const QList<QSize> sizes = it->icon().availableSizes();
        REQUIRE(sizes.size() == 1);
        CHECK(sizes.first() == box);
        CHECK(box.width() <= kIconBox.width());
        CHECK(box.height() <= kIconBox.height());
        CHECK(box.width() >= kIconBox.width() / 2);  // a word, not a speck
        const QImage want = stampIcon(sample, 8, kIconBox);
        const QImage icon = it->icon().pixmap(box).toImage().convertToFormat(want.format());
        CHECK(icon == want);
        // It shows the lettering: more than one colour.
        std::set<QRgb> colours;
        for (int y = 0; y < icon.height(); ++y)
            for (int x = 0; x < icon.width(); ++x)
                colours.insert(icon.pixel(x, y));
        CHECK(colours.size() > 1);
        // A text style isn't dragged: it's no stamp.
        CHECK_FALSE(it->flags().testFlag(Qt::ItemIsDragEnabled));
        const QModelIndex index = f.dock.grid()->model()->index(f.dock.grid()->row(it), 0);
        CHECK(std::unique_ptr<QMimeData>(f.dock.grid()->model()->mimeData({index})) == nullptr);
    }
    // The categories cover the text styles as well as the stamps.
    for (const char* c : {"classic", "metal", "elements", "signs"})
        CHECK(f.dock.categories().contains(QString::fromLatin1(c)));

    // Cached like a stamp's: a rescan renders nothing again.
    const int renders = f.dock.thumbnailRenders();
    f.dock.rescan();
    CHECK(f.dock.thumbnailRenders() == renders);
    CHECK(f.dock.thumbnailCache().size() == qsizetype(f.dock.entries().size() + f.dock.styles().size()));
}

TEST_CASE("the category list and the search box find text styles by name and tag") {
    Fixture f;
    const auto styleNames = [&] {
        QStringList out;
        for (const QString& n : f.dock.shownNames())
            for (const LibraryDock::StyleEntry& e : f.dock.styles())
                if (QString::fromStdString(e.style.name) == n)
                    out << n;
        return out;
    };
    // By name, ignoring case; every term must match.
    f.dock.searchEdit()->setText(QStringLiteral("gold 3D"));
    CHECK(f.dock.shownNames() == QStringList{QStringLiteral("Gold 3D")});
    f.dock.searchEdit()->setText(QStringLiteral("CHROME"));
    CHECK(styleNames() == QStringList{QStringLiteral("Chrome")});
    // By tag.
    f.dock.searchEdit()->setText(QStringLiteral("extrude"));
    CHECK(f.dock.shownNames() == QStringList{QStringLiteral("Gold 3D")});
    f.dock.searchEdit()->setText(QStringLiteral("frost"));
    CHECK(f.dock.shownNames() == QStringList{QStringLiteral("Ice")});
    f.dock.searchEdit()->setText(QStringLiteral("sign plate"));
    CHECK(styleNames() == QStringList{QStringLiteral("Warning sign")});
    f.dock.searchEdit()->setText(QStringLiteral("nothing-like-this"));
    CHECK(f.dock.shownNames().isEmpty());
    f.dock.searchEdit()->clear();

    // By category: "signs" holds three styles and no stamp.
    const auto found = f.dock.categoryList()->findItems(QStringLiteral("signs"), Qt::MatchExactly);
    REQUIRE(found.size() == 1);
    f.dock.categoryList()->setCurrentItem(found.first());
    const QStringList signs{QStringLiteral("Masthead"), QStringLiteral("Neon"), QStringLiteral("Warning sign")};
    CHECK(f.dock.shownNames() == signs);
    f.dock.rescan();  // keeps the category
    CHECK(f.dock.shownNames() == signs);
    // The search narrows within it.
    f.dock.searchEdit()->setText(QStringLiteral("neon"));
    CHECK(f.dock.shownNames() == QStringList{QStringLiteral("Neon")});
}

TEST_CASE("a text style's tooltip gives its name, category, tags and iCE need") {
    Fixture f;
    // A user style that needs iCE colours (none of the bundled ones does).
    std::string json = library::textStyleEntryJson(library::readTextStyleEntry(kBundled / "text-styles" / "neon.style.json"));
    library::TextStyleEntry t = library::parseTextStyleEntry(json);
    t.name = "Hot Neon";
    t.category = "mine";
    t.tags = {"hot", "glow"};
    t.iceRequired = true;
    put(f.userDir() / "hot-neon.style.json", library::textStyleEntryJson(t));
    f.dock.rescan();
    REQUIRE(f.dock.problems().empty());

    const QString hot = item(f.dock, QStringLiteral("Hot Neon"))->toolTip();
    CHECK(hot.startsWith(QStringLiteral("Hot Neon\nText style\n")));
    CHECK(hot.contains(QStringLiteral("Category: mine")));
    CHECK(hot.contains(QStringLiteral("Tags: hot, glow")));
    CHECK(hot.contains(QStringLiteral("Needs iCE colours")));
    CHECK(hot.contains(QStringLiteral("User library: ")));

    const QString gold = item(f.dock, QStringLiteral("Gold 3D"))->toolTip();
    CHECK(gold.contains(QStringLiteral("Category: classic")));
    CHECK(gold.contains(QStringLiteral("Tags: gold, 3d, extrude")));
    CHECK(gold.contains(QStringLiteral("Works with iCE colours on or off")));
    CHECK_FALSE(gold.contains(QStringLiteral("Needs iCE")));
    CHECK(gold.contains(QStringLiteral("Bundled library: ")));

    // Search finds the user style by a tag too.
    f.dock.searchEdit()->setText(QStringLiteral("glow"));
    CHECK(f.dock.shownNames().contains(QStringLiteral("Hot Neon")));
}

// A copy of Neon that needs iCE colours, as a user style in `dir`.
fs::path putHotNeon(const fs::path& dir, const char* file) {
    auto t = library::readTextStyleEntry(kBundled / "text-styles" / "neon.style.json");
    t.name = "Hot Neon";
    t.iceRequired = true;
    const fs::path p = dir / file;
    put(p, library::textStyleEntryJson(t));
    return p;
}

TEST_CASE("with iCE off the dock shows a style that needs iCE disabled, and says why") {
    Fixture f;
    putHotNeon(f.userDir(), "hot-neon.style.json");
    f.dock.rescan();
    REQUIRE(f.dock.iceColours());
    const int renders = f.dock.thumbnailRenders();
    CHECK(item(f.dock, QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));

    f.dock.setIceColours(false);
    QListWidgetItem* hot = item(f.dock, QStringLiteral("Hot Neon"));
    CHECK_FALSE(hot->flags().testFlag(Qt::ItemIsEnabled));
    CHECK(hot->toolTip().contains(QStringLiteral("Needs iCE colours, which are off: turn on View > iCE Colours")));
    // The rest are untouched.
    for (const library::TextStyleEntry& t : presets()) {
        INFO(t.name);
        QListWidgetItem* it = item(f.dock, QString::fromStdString(t.name));
        CHECK(it->flags().testFlag(Qt::ItemIsEnabled));
        CHECK(it->toolTip().contains(QStringLiteral("Works with iCE colours on or off")));
    }
    // The thumbnail is still the iCE look it needs, from the cache.
    CHECK(f.dock.thumbnailRenders() == renders);
    const auto style = std::find_if(f.dock.styles().begin(), f.dock.styles().end(),
                                    [](const LibraryDock::StyleEntry& e) { return e.style.name == "Hot Neon"; });
    REQUIRE(style != f.dock.styles().end());
    CHECK(f.dock.thumbnailCache().value(style->thumbKey) ==
          stampIcon(library::textStyleSample(style->style.style, true), 8, kIconBox));

    // Search and category refills keep it disabled.
    f.dock.searchEdit()->setText(QStringLiteral("hot"));
    CHECK_FALSE(item(f.dock, QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));
    f.dock.searchEdit()->clear();

    f.dock.setIceColours(true);
    hot = item(f.dock, QStringLiteral("Hot Neon"));
    CHECK(hot->flags().testFlag(Qt::ItemIsEnabled));
    CHECK_FALSE(hot->toolTip().contains(QStringLiteral("which are off")));
    CHECK(hot->toolTip().contains(QStringLiteral("Needs iCE colours")));
}

TEST_CASE("probe: an iCE-required style with iCE off in the window") {
    const fs::path userDir = fspath(QFile::decodeName(qgetenv("BLOCKWRIGHT_USER_LIBRARY")));
    fs::create_directories(userDir);
    const fs::path file = putHotNeon(userDir, "hot-neon-probe.style.json");
    {
        MainWindow w;
        w.setIceColours(false);
        REQUIRE_FALSE(w.document()->format().ice);
        w.libraryWidget()->rescan();
        QListWidgetItem* it = item(*w.libraryWidget(), QStringLiteral("Hot Neon"));
        REQUIRE(it);
        // PLAN §15: "with iCE off it is shown disabled in the Library".
        CHECK_FALSE(it->flags().testFlag(Qt::ItemIsEnabled));

        // Turned back on (the View menu's toggle), it is enabled again.
        w.findChild<QAction*>(QStringLiteral("iceColours"))->trigger();
        REQUIRE(w.document()->format().ice);
        CHECK(item(*w.libraryWidget(), QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));
        // Undo puts iCE off again, and the dock follows.
        w.document()->undoStack()->undo();
        REQUIRE_FALSE(w.document()->format().ice);
        CHECK_FALSE(item(*w.libraryWidget(), QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));
    }
    fs::remove(file);
}

TEST_CASE("the window's Library follows the iCE setting of the document it opens") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const fs::path userDir = fspath(tmp.filePath(QStringLiteral("user")));
    fs::create_directories(userDir);
    putHotNeon(userDir, "hot-neon.style.json");
    QString off, on, error;
    {
        MainWindow w;
        w.setIceColours(false);
        off = tmp.filePath(QStringLiteral("ice-off.ans"));
        REQUIRE(w.saveFile(off, &error));
        w.setIceColours(true);
        on = tmp.filePath(QStringLiteral("ice-on.ans"));
        REQUIRE(w.saveFile(on, &error));
    }
    MainWindow w;
    LibraryDock* dock = w.libraryWidget();
    dock->setDirectories(kBundled, userDir);
    // A new document has iCE colours.
    CHECK(dock->iceColours());
    CHECK(item(*dock, QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));
    REQUIRE(w.openFile(off, &error));
    CHECK_FALSE(dock->iceColours());
    CHECK_FALSE(item(*dock, QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));
    CHECK(item(*dock, QStringLiteral("Neon"))->flags().testFlag(Qt::ItemIsEnabled));
    REQUIRE(w.openFile(on, &error));
    CHECK(dock->iceColours());
    CHECK(item(*dock, QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));
}

TEST_CASE("a malformed .style.json is skipped and reported, and the rest still shows") {
    Fixture f;
    const std::size_t bundled = f.dock.styles().size();
    put(f.userDir() / "broken.style.json", "{\"kind\": \"text-style\", \"name\": \"Broken\"");
    put(f.userDir() / "unknown.style.json",
        "{\"kind\": \"text-style\", \"name\": \"Unknown\", \"category\": \"x\", \"tags\": [],"
        " \"effects\": [{\"kind\": \"sparkle\", \"enabled\": true}]}\n");
    put(f.userDir() / "no-ramp.style.json",
        "{\"kind\": \"text-style\", \"name\": \"No Ramp\", \"category\": \"x\", \"tags\": [],"
        " \"effects\": [{\"kind\": \"fill\", \"enabled\": true, \"colour\": 6, \"mode\": \"flat\","
        " \"ramp\": \"no-such-ramp\"}]}\n");
    f.dock.rescan();

    CHECK(f.dock.styles().size() == bundled);
    REQUIRE(f.dock.problems().size() == 3);
    std::set<fs::path> bad;
    for (const library::Problem& p : f.dock.problems())
        bad.insert(p.path);
    CHECK(bad == std::set<fs::path>{f.userDir() / "broken.style.json", f.userDir() / "unknown.style.json",
                                    f.userDir() / "no-ramp.style.json"});
    CHECK(f.dock.problemLabel()->text() == QStringLiteral("3 file(s) skipped"));
    CHECK(f.dock.problemLabel()->toolTip().contains(QStringLiteral("broken.style.json")));
    for (const char* name : {"Broken", "Unknown", "No Ramp"})
        CHECK_FALSE(f.dock.shownNames().contains(QString::fromLatin1(name)));
    CHECK(f.dock.shownNames().contains(QStringLiteral("Gold 3D")));
}

TEST_CASE("activating a text style asks for it, not for a stamp") {
    Fixture f;
    QByteArray style, stamp;
    QObject::connect(&f.dock, &LibraryDock::textStyleActivated, [&](const QByteArray& k) { style = k; });
    QObject::connect(&f.dock, &LibraryDock::stampActivated, [&](const QByteArray& k) { stamp = k; });
    emit f.dock.grid()->itemActivated(item(f.dock, QStringLiteral("Chrome")));
    CHECK(stamp.isEmpty());
    REQUIRE(!style.isEmpty());
    const library::TextStyleEntry* t = f.dock.findStyleKey(style);
    REQUIRE(t);
    CHECK(t->name == "Chrome");
    CHECK(t->path == kBundled / "text-styles" / "chrome.style.json");
    // The key is no stamp's.
    CHECK(f.dock.findKey(style) == nullptr);
}

TEST_CASE("library::matches finds a text style by every term in its name or tags") {
    library::TextStyleEntry t;
    t.name = "Gold 3D";
    t.tags = {"gold", "extrude"};
    CHECK(library::matches(t, ""));
    CHECK(library::matches(t, "GOLD"));
    CHECK(library::matches(t, "3d extr"));
    CHECK_FALSE(library::matches(t, "gold chrome"));
}
