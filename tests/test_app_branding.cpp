// The logos and the app icon (#205): branding/'s .ans files parse, keep to
// their sizes, fit the IRC budget, lint clean with iCE on and off and are
// their projects flattened; the plain-text logo is 7-bit and at most 72
// columns; the PNGs are what blockwright-cli renders; and the app loads the
// logos and the icon from :/branding. main() is in test_app.cpp.

#include "branding.h"
#include "lint.h"
#include "main_window.h"

#include "blockwright/ansi.h"
#include "blockwright/irc.h"
#include "blockwright/logo_text.h"
#include "blockwright/project.h"
#include "blockwright/render.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QLabel>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace bw;

namespace {

const std::string kBranding = BW_BRANDING_DIR;
const std::string kPackaging = BW_PACKAGING_DIR;

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

AnsiDocument parseFile(const std::string& path) {
    const std::string bytes = slurp(path);
    return parseAnsi(bytes);
}

struct Piece {
    const char* name;
    int maxWidth, maxHeight;  // at most
    bool exact;               // exactly that size (the icons)
};

constexpr Piece kPieces[] = {
    {"logo-large", 80, 20, false},
    {"logo-small", 40, 6, false},
    {"icon", 32, 16, true},
    {"icon-small", 16, 8, true},
};

// The image as RGB32, for comparing pixels whatever format a PNG loads as
QImage rgb(const QImage& im) { return im.convertToFormat(QImage::Format_RGB32); }
QImage argb(const QImage& im) { return im.convertToFormat(QImage::Format_ARGB32); }

// Whether `icon` is `render` with only the black reached from its corners
// made transparent: each pixel is the render's, opaque, or transparent where
// the render is black, and the corners are transparent.
bool keyedRender(const QImage& icon, const QImage& render) {
    const QImage a = argb(icon), r = rgb(render);
    if (a.size() != r.size())
        return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = r.pixel(x, y);
            const bool same = qAlpha(p) == 255 && (p & 0xffffff) == (q & 0xffffff);
            const bool keyed = qAlpha(p) == 0 && (q & 0xffffff) == 0;
            if (!same && !keyed)
                return false;
        }
    const int w = a.width() - 1, h = a.height() - 1;
    return qAlpha(a.pixel(0, 0)) == 0 && qAlpha(a.pixel(w, 0)) == 0 && qAlpha(a.pixel(0, h)) == 0 &&
           qAlpha(a.pixel(w, h)) == 0 && qAlpha(a.pixel(w / 2, h / 2)) == 255;
}

} // namespace

TEST_CASE("each branding .ans parses, is signed, keeps to its size and fits the IRC budget", "[branding]") {
    for (const Piece& p : kPieces) {
        INFO(p.name);
        const AnsiDocument doc = parseFile(kBranding + "/" + p.name + ".ans");
        const Canvas& c = doc.canvas;
        if (p.exact) {
            CHECK(c.width() == p.maxWidth);
            CHECK(c.height() == p.maxHeight);
        } else {
            CHECK(c.width() <= p.maxWidth);
            CHECK(c.height() <= p.maxHeight);
        }
        REQUIRE(doc.sauce);
        CHECK(doc.sauce->author == "funkpower");
        CHECK(!doc.sauce->title.empty());

        // No bright background and no blink: the cells show the same with
        // iCE on and off
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x) {
                const Cell& cell = c.at(x, y);
                INFO(x << "," << y);
                CHECK(cell.bg.kind == Color::Kind::Index);
                CHECK(cell.bg.index < 8);
                CHECK(cell.fg.kind == Color::Kind::Index);
                CHECK(!(cell.attrs & attr::Blink));
            }

        // Every row as `blockwright-cli export --irc` writes it fits one
        // EFnet message
        for (const irc::Line& line : irc::exportLines(c, {})) {
            INFO(line.text);
            CHECK(line.text.size() <= irc::kEfnetWeechatBudget);
            CHECK(line.merged == 0);
        }

        // Lint, as `blockwright-cli lint` runs it, with iCE on and off
        for (const bool ice : {true, false}) {
            INFO("iCE " << ice);
            AnsiDocument d = doc;
            d.ice = ice;
            const lint::LintReport r = bwcli::lintDocument(d, {});
            INFO(bwcli::lintText(r));
            CHECK(r.findings.empty());
        }
    }
}

TEST_CASE("each branding .ans is its .bwp flattened", "[branding]") {
    for (const Piece& p : kPieces) {
        INFO(p.name);
        const project::Project proj = project::read(kBranding + "/" + p.name + ".bwp");
        CHECK(writeAnsi(toAnsi(proj)).bytes == slurp(kBranding + "/" + p.name + ".ans"));
    }
}

TEST_CASE("the branding PNGs are the .ans files as blockwright-cli renders them", "[branding]") {
    // The logos at 1x and 2x
    for (const char* name : {"logo-large", "logo-small"}) {
        INFO(name);
        const AnsiDocument doc = parseFile(kBranding + "/" + name + ".ans");
        for (const int scale : {1, 2}) {
            const std::string png = kBranding + "/png/" + name + (scale == 2 ? "@2x" : "") + ".png";
            INFO(png);
            const QImage committed(QString::fromStdString(png));
            REQUIRE(!committed.isNull());
            CHECK(rgb(committed) == rgb(renderDocument(doc, {.fontWidth = 0, .scale = scale})));
        }
    }
    // The icon at 1x, 2x and 4x
    const AnsiDocument icon = parseFile(kBranding + "/icon.ans");
    const struct {
        std::string png;
        int scale;
    } renders[] = {
        {kPackaging + "/linux/blockwright-256.png", 1},
        {kPackaging + "/linux/blockwright-512.png", 2},
        {kPackaging + "/macos/icon-1024.png", 4},
    };
    for (const auto& r : renders) {
        INFO(r.png);
        const QImage committed(QString::fromStdString(r.png));
        REQUIRE(!committed.isNull());
        CHECK(keyedRender(committed, renderDocument(icon, {.fontWidth = 8, .scale = r.scale})));
    }
    // Every Linux size is square at its size
    for (const int size : {16, 32, 48, 128, 256, 512}) {
        const QImage im(QString::fromStdString(kPackaging + "/linux/blockwright-" + std::to_string(size) + ".png"));
        INFO(size);
        CHECK(im.size() == QSize(size, size));
    }
    // 16 px is icon-small.ans with each half-block pixel taken at its
    // centre; 32 and 48 px repeat those pixels
    const QImage small = rgb(renderDocument(parseFile(kBranding + "/icon-small.ans"), {.fontWidth = 8, .scale = 1}));
    REQUIRE(small.size() == QSize(128, 128));
    QImage centres(16, 16, QImage::Format_RGB32);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            centres.setPixel(x, y, small.pixel(x * 8 + 4, y * 8 + 4));
    const QImage i16 = argb(QImage(QString::fromStdString(kPackaging + "/linux/blockwright-16.png")));
    CHECK(keyedRender(i16, centres));
    const QImage i32 = argb(QImage(QString::fromStdString(kPackaging + "/linux/blockwright-32.png")));
    const QImage i48 = argb(QImage(QString::fromStdString(kPackaging + "/linux/blockwright-48.png")));
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            INFO(x << "," << y);
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                    CHECK(i32.pixel(x * 2 + i, y * 2 + j) == i16.pixel(x, y));
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i)
                    CHECK(i48.pixel(x * 3 + i, y * 3 + j) == i16.pixel(x, y));
        }
    // The Windows icon holds the 16, 32, 48 and 256 px icons (a width of 0
    // is 256), each as a PNG
    const std::string ico = slurp(kPackaging + "/windows/blockwright.ico");
    REQUIRE(ico.size() > 6 + 4 * 16);
    const auto u16 = [&](std::size_t at) { return static_cast<unsigned char>(ico[at]) | static_cast<unsigned char>(ico[at + 1]) << 8; };
    const auto u32 = [&](std::size_t at) { return static_cast<std::size_t>(u16(at)) | static_cast<std::size_t>(u16(at + 2)) << 16; };
    CHECK(u16(0) == 0);
    CHECK(u16(2) == 1);
    REQUIRE(u16(4) == 4);
    std::vector<int> widths;
    for (std::size_t e = 0; e < 4; ++e) {
        const std::size_t entry = 6 + e * 16;
        widths.push_back(static_cast<unsigned char>(ico[entry]) == 0 ? 256 : static_cast<unsigned char>(ico[entry]));
        const std::size_t size = u32(entry + 8), offset = u32(entry + 12);
        REQUIRE(offset + size <= ico.size());
        CHECK(ico.compare(offset, 8, "\x89PNG\r\n\x1a\n") == 0);
        const QImage im = QImage::fromData(reinterpret_cast<const uchar*>(ico.data() + offset), static_cast<int>(size), "PNG");
        CHECK(argb(im) == argb(QImage(QString::fromStdString(kPackaging + "/linux/blockwright-" + std::to_string(widths.back()) + ".png"))));
    }
    std::sort(widths.begin(), widths.end());
    CHECK(widths == std::vector<int>{16, 32, 48, 256});
    // 128 px is the 256 halved: transparent at the corners, opaque inside
    const QImage i128 = argb(QImage(QString::fromStdString(kPackaging + "/linux/blockwright-128.png")));
    CHECK(qAlpha(i128.pixel(0, 0)) == 0);
    CHECK(qAlpha(i128.pixel(64, 64)) == 255);
}

TEST_CASE("the plain-text logo is 7-bit ASCII at most 72 columns, and the binaries print it", "[branding]") {
    const std::string text = slurp(kBranding + "/logo.txt");
    REQUIRE(!text.empty());
    CHECK(text.back() == '\n');
    std::istringstream lines(text);
    std::string line;
    int n = 0;
    while (std::getline(lines, line)) {
        ++n;
        INFO("line " << n << ": " << line);
        CHECK(line.size() <= 72);
        CHECK((line.empty() || line.back() != ' '));
        for (const char ch : line)
            CHECK((ch >= 0x20 && ch < 0x7f));
    }
    CHECK(n <= 12);
    // the header --help and --version print it from
    CHECK(std::string(kLogoText) == text);

    // the README's header: the large logo, with this as its alt text
    const std::string readme = slurp(kBranding + "/../README.md");
    const std::string open = "<img src=\"branding/png/logo-large@2x.png\" width=\"640\" alt=\"";
    const auto at = readme.find(open);
    REQUIRE(at != std::string::npos);
    const auto end = readme.find('"', at + open.size());
    REQUIRE(end != std::string::npos);
    QString alt = QString::fromStdString(readme.substr(at + open.size(), end - at - open.size()));
    alt.replace(QLatin1String("&lt;"), QLatin1String("<"))
        .replace(QLatin1String("&gt;"), QLatin1String(">"))
        .replace(QLatin1String("&quot;"), QLatin1String("\""))
        .replace(QLatin1String("&#x27;"), QLatin1String("'"))
        .replace(QLatin1String("&amp;"), QLatin1String("&"));
    CHECK(alt.toStdString() + "\n" == text);
}

TEST_CASE("the app loads the logos and the icon from :/branding", "[branding]") {
    for (const char* name : {"logo-large", "logo-small"}) {
        const QString path = QStringLiteral(":/branding/%1.ans").arg(QLatin1String(name));
        INFO(path.toStdString());
        QFile f(path);
        REQUIRE(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll().toStdString() == slurp(kBranding + "/" + name + ".ans"));
        const auto doc = loadLogo(path);
        REQUIRE(doc);
        CHECK(doc->canvas.width() == parseFile(kBranding + "/" + name + ".ans").canvas.width());
    }
    CHECK(QString::fromLatin1(kLogoLarge) == QStringLiteral(":/branding/logo-large.ans"));
    CHECK(QString::fromLatin1(kLogoSmall) == QStringLiteral(":/branding/logo-small.ans"));
    CHECK(!loadLogo(QStringLiteral(":/branding/none.ans")));
    CHECK(logoPixmap(QStringLiteral(":/branding/none.ans")).isNull());

    // at 2 the logo is drawn at twice the size and keeps its size on screen
    const QPixmap pm = logoPixmap(QString::fromLatin1(kLogoSmall), 2.0);
    const AnsiDocument small = parseFile(kBranding + "/logo-small.ans");
    const int w = small.canvas.width() * glyphWidth(small), h = small.canvas.height() * 16;
    CHECK(pm.size() == QSize(2 * w, 2 * h));
    CHECK(pm.deviceIndependentSize() == QSizeF(w, h));

    const QIcon icon = applicationIcon();
    REQUIRE(!icon.isNull());
    for (const int size : {16, 32, 48, 128, 256, 512}) {
        INFO(size);
        CHECK(icon.availableSizes().contains(QSize(size, size)));
        CHECK(icon.pixmap(QSize(size, size), 1.0).size() == QSize(size, size));
    }
    // the 16 px icon is the PNG, not the 512 scaled down
    CHECK(argb(icon.pixmap(QSize(16, 16), 1.0).toImage()) ==
          argb(QImage(QString::fromStdString(kPackaging + "/linux/blockwright-16.png"))));
}

TEST_CASE("Help > About Blockwright shows the small logo drawn by the renderer", "[branding]") {
    MainWindow w;
    QAction* about = w.findChild<QAction*>(QStringLiteral("about"));
    REQUIRE(about);
    about->trigger();
    auto* d = w.findChild<AboutDialog*>(QStringLiteral("aboutDialog"));
    REQUIRE(d);
    CHECK(d->isVisible());
    auto* logo = d->findChild<QLabel*>(QStringLiteral("aboutLogo"));
    REQUIRE(logo);
    CHECK(logo->isVisibleTo(d));
    const AnsiDocument small = parseFile(kBranding + "/logo-small.ans");
    const qreal dpr = d->devicePixelRatioF();
    const int scale = std::max(1, static_cast<int>(std::ceil(dpr)));
    CHECK(rgb(logo->pixmap().toImage()) == rgb(renderDocument(small, {.fontWidth = 0, .scale = scale})));
    // a change of pixel ratio (a move to another screen) draws it again
    logo->setPixmap(QPixmap());
    QEvent change(QEvent::DevicePixelRatioChange);
    QApplication::sendEvent(d, &change);
    CHECK(rgb(logo->pixmap().toImage()) == rgb(renderDocument(small, {.fontWidth = 0, .scale = scale})));
    auto* text = d->findChild<QLabel*>(QStringLiteral("aboutText"));
    REQUIRE(text);
    CHECK(text->text().contains(QStringLiteral("GPL-3.0-or-later")));
    CHECK(text->text().contains(QStringLiteral("VileR")));
    CHECK(text->text().contains(QStringLiteral("Tabler Icons")));
    // a second time shows the same dialog
    about->trigger();
    CHECK(w.findChildren<AboutDialog*>().size() == 1);
    d->close();
}

TEST_CASE("the About logo keeps a 16 px black border round the art", "[branding]") {
    MainWindow w;
    AboutDialog* d = showAbout(&w);
    auto* logo = d->findChild<QLabel*>(QStringLiteral("aboutLogo"));
    REQUIRE(logo);
    REQUIRE_FALSE(logo->pixmap().isNull());
    // showing the dialog polishes the label, which must not drop the border
    CHECK(logo->size() == logo->pixmap().deviceIndependentSize().toSize() + QSize(32, 32));
    d->close();
}
