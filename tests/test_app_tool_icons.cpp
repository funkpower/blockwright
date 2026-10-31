// The tools' icons (PLAN §11, #224): drawn in the application palette's
// colours, or the toolbox's own, so every tool's icon stands out from a button
// on a light and a dark scheme; drawn at the device pixel ratio asked for; and
// made anew for the toolbox and the Tools menu when the palette changes.

#include "main_window.h"
#include "tool.h"
#include "tool_icon.h"
#include "toolbox.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace bw;

namespace {

// WCAG relative luminance and contrast ratio.
double channel(int v) {
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double luminance(QColor c) {
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue());
}
double contrast(QColor a, QColor b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QPalette lightPalette() {
    std::unique_ptr<QStyle> fusion(QStyleFactory::create(QStringLiteral("Fusion")));
    return fusion->standardPalette();
}

// A dark scheme like the desktops' dark themes: light text on dark grey.
QPalette darkPalette() {
    QPalette p;
    const QColor window(53, 53, 53), base(35, 35, 35), text(230, 230, 230);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, QColor(42, 130, 218));
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
    return p;
}

// Sets the application palette for the life of the guard.
class PaletteGuard {
public:
    explicit PaletteGuard(const QPalette& p) : saved_(QApplication::palette()) { QApplication::setPalette(p); }
    ~PaletteGuard() { QApplication::setPalette(saved_); }

private:
    QPalette saved_;
};

// The icon drawn over `paper` as a button would show it.
QImage onPaper(const QIcon& icon, QColor paper, QIcon::Mode mode = QIcon::Normal, QIcon::State state = QIcon::Off,
               qreal dpr = 1) {
    QImage img(QSize(24, 24) * dpr, QImage::Format_ARGB32);
    img.fill(paper);
    QPainter p(&img);
    p.drawPixmap(0, 0, icon.pixmap(QSize(24, 24), dpr, mode, state));
    p.end();
    return img;
}

// How many pixels of `img` meet `ratio` against `paper`.
int pixelsAtContrast(const QImage& img, QColor paper, double ratio) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (contrast(img.pixelColor(x, y), paper) >= ratio)
                ++n;
    return n;
}

bool hasPixel(const QImage& img, QColor want, int tolerance) {
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (std::abs(c.red() - want.red()) <= tolerance && std::abs(c.green() - want.green()) <= tolerance &&
                std::abs(c.blue() - want.blue()) <= tolerance)
                return true;
        }
    return false;
}

QStringList registryIds(MainWindow& w) {
    QStringList ids;
    for (Tool* t : w.tools()->tools())
        ids << t->id();
    return ids;
}

// A stroke 2 units wide across most of a 24-unit icon covers well over this.
constexpr int kMinLegiblePixels = 24;

} // namespace

TEST_CASE("every tool's icon meets 3:1 against the button on a light and a dark palette", "[tool-icons]") {
    const bool dark = GENERATE(false, true);
    const PaletteGuard guard(dark ? darkPalette() : lightPalette());
    MainWindow w;
    const QStringList ids = registryIds(w);
    REQUIRE(ids.size() >= 20);
    const QColor button = QApplication::palette().color(QPalette::Active, QPalette::Button);
    for (const QString& id : ids) {
        INFO((dark ? "dark " : "light ") << id.toStdString());
        const QIcon icon = toolIcon(id);
        CHECK(pixelsAtContrast(onPaper(icon, button), button, 3.0) >= kMinLegiblePixels);
        CHECK(pixelsAtContrast(onPaper(icon, button, QIcon::Normal, QIcon::On), button, 3.0) >= kMinLegiblePixels);
        CHECK(pixelsAtContrast(onPaper(icon, button, QIcon::Active), button, 3.0) >= kMinLegiblePixels);
    }
}

TEST_CASE("a selected tool icon meets 3:1 against the highlight", "[tool-icons]") {
    const bool dark = GENERATE(false, true);
    const PaletteGuard guard(dark ? darkPalette() : lightPalette());
    MainWindow w;
    const QColor highlight = QApplication::palette().color(QPalette::Active, QPalette::Highlight);
    for (const QString& id : registryIds(w)) {
        INFO((dark ? "dark " : "light ") << id.toStdString());
        CHECK(pixelsAtContrast(onPaper(toolIcon(id), highlight, QIcon::Selected), highlight, 3.0) >=
              kMinLegiblePixels);
    }
}

TEST_CASE("a tool icon's colours follow the palette for each mode", "[tool-icons]") {
    const QPalette p = darkPalette();
    const ToolIconColours normal = toolIconColours(p, QIcon::Normal);
    CHECK(normal.ink == p.color(QPalette::Active, QPalette::ButtonText));
    CHECK(normal.paper == p.color(QPalette::Active, QPalette::Button));
    CHECK(toolIconColours(p, QIcon::Active).ink == normal.ink);
    const ToolIconColours disabled = toolIconColours(p, QIcon::Disabled);
    CHECK(disabled.ink == p.color(QPalette::Disabled, QPalette::ButtonText));
    CHECK(disabled.ink != normal.ink);
    const ToolIconColours selected = toolIconColours(p, QIcon::Selected);
    CHECK(selected.ink == p.color(QPalette::Active, QPalette::HighlightedText));
    CHECK(selected.paper == p.color(QPalette::Active, QPalette::Highlight));
}

TEST_CASE("a disabled tool icon is drawn in the disabled text colour", "[tool-icons]") {
    const PaletteGuard guard(darkPalette());
    const QPalette& p = QApplication::palette();
    const QColor button = p.color(QPalette::Button);
    const QColor disabled = p.color(QPalette::Disabled, QPalette::ButtonText);
    const QColor normal = p.color(QPalette::Active, QPalette::ButtonText);
    const auto id = QString::fromLatin1(GENERATE("line", "halfBlock", "select"));
    INFO(id.toStdString());
    const QImage img = onPaper(toolIcon(id), button, QIcon::Disabled);
    CHECK(hasPixel(img, disabled, 4));
    CHECK_FALSE(hasPixel(img, normal, 4));
}

TEST_CASE("a tool icon at device pixel ratio 2 is 48x48 device pixels", "[tool-icons]") {
    const auto id = QString::fromLatin1(GENERATE("line", "move", "shade", "colorize", "rampShade"));
    INFO(id.toStdString());
    const QPixmap pm = toolIcon(id).pixmap(QSize(24, 24), 2.0);
    CHECK(pm.size() == QSize(48, 48));
    CHECK(pm.devicePixelRatio() == 2.0);
    // Drawn at that resolution, not scaled up: a 2-unit stroke at the edge
    // of a cell lands on single device pixels it wouldn't at 1x.
    const QImage hi = pm.toImage();
    const QImage up = toolIcon(id).pixmap(QSize(24, 24), 1.0).toImage().scaled(48, 48);
    CHECK(hi.convertToFormat(QImage::Format_ARGB32) != up.convertToFormat(QImage::Format_ARGB32));
}

// QIcon hands the engine the size in device pixels before Qt 6.8 and in
// logical pixels from 6.8; either way the pixmap has round(24 x dpr) device
// pixels at that dpr.
TEST_CASE("a tool icon at a fractional device pixel ratio is drawn at that ratio", "[tool-icons]") {
    const qreal dpr = GENERATE(1.25, 1.5, 1.75, 2.0);
    const auto id = QString::fromLatin1(GENERATE("line", "shade"));
    INFO(id.toStdString() << " @" << dpr);
    const QPixmap pm = toolIcon(id).pixmap(QSize(24, 24), dpr);
    CHECK(pm.size() == QSize(qRound(24 * dpr), qRound(24 * dpr)));
    CHECK(pm.devicePixelRatio() == dpr);
}

TEST_CASE("the colorize icon keeps its three hues", "[tool-icons]") {
    const bool dark = GENERATE(false, true);
    const PaletteGuard guard(dark ? darkPalette() : lightPalette());
    const QColor button = QApplication::palette().color(QPalette::Button);
    const QImage img = onPaper(toolIcon(QStringLiteral("colorize")), button, QIcon::Normal, QIcon::Off, 2);
    CHECK(hasPixel(img, QColor(200, 40, 40), 8));
    CHECK(hasPixel(img, QColor(40, 160, 40), 8));
    CHECK(hasPixel(img, QColor(40, 80, 200), 8));
}

TEST_CASE("every Tabler-sourced tool icon is in the resources", "[tool-icons]") {
    MainWindow w;
    int tabler = 0;
    for (const QString& id : registryIds(w)) {
        const QString name = toolIconSource(id);
        if (name.isEmpty())
            continue;
        INFO(id.toStdString() << " -> " << name.toStdString());
        ++tabler;
        QFile svg(QStringLiteral(":/tool-icons/%1.svg").arg(name));
        REQUIRE(svg.open(QIODevice::ReadOnly));
        CHECK(svg.readAll().contains("currentColor"));
    }
    CHECK(tabler == 11);
}

TEST_CASE("changing the application palette rebuilds the toolbox and Tools menu icons", "[tool-icons]") {
    const PaletteGuard guard(lightPalette());
    MainWindow w;
    Toolbox* box = w.toolbox();
    REQUIRE(box);
    const QStringList ids = registryIds(w);
    QHash<QString, qint64> actionKeys;
    for (const QString& id : ids) {
        auto* a = w.findChild<QAction*>(id + QStringLiteral("Tool"));
        REQUIRE(a);
        actionKeys.insert(id, a->icon().cacheKey());
    }
    QHash<QToolButton*, qint64> buttonKeys;
    for (QToolButton* b : box->buttons())
        buttonKeys.insert(b, b->icon().cacheKey());
    REQUIRE_FALSE(buttonKeys.isEmpty());

    QApplication::setPalette(darkPalette());
    QApplication::processEvents();

    const QColor button = darkPalette().color(QPalette::Button);
    const QColor ink = darkPalette().color(QPalette::ButtonText);
    for (const QString& id : ids) {
        INFO(id.toStdString());
        auto* a = w.findChild<QAction*>(id + QStringLiteral("Tool"));
        CHECK(a->icon().cacheKey() != actionKeys.value(id));
        CHECK(hasPixel(onPaper(a->icon(), button), ink, 4));
    }
    for (auto it = buttonKeys.cbegin(); it != buttonKeys.cend(); ++it) {
        INFO(it.key()->objectName().toStdString());
        CHECK(it.key()->icon().cacheKey() != it.value());
        CHECK(hasPixel(onPaper(it.key()->icon(), button), ink, 4));
    }
}

TEST_CASE("a palette set on the toolbox draws its buttons' icons in it", "[tool-icons]") {
    const PaletteGuard guard(lightPalette());
    MainWindow w;
    Toolbox* box = w.toolbox();
    REQUIRE(box);
    const QPalette dark = darkPalette();
    box->setPalette(dark);
    QApplication::processEvents();
    const QColor darkButton = dark.color(QPalette::Button);
    for (QToolButton* b : box->buttons()) {
        INFO(b->objectName().toStdString());
        CHECK(pixelsAtContrast(onPaper(b->icon(), darkButton), darkButton, 3.0) >= kMinLegiblePixels);
    }
    // A group's button takes another tool, and the active one is checked: its
    // icon stays in the toolbox's palette.
    auto* rectangle = w.findChild<QAction*>(QStringLiteral("rectangleTool"));
    REQUIRE(rectangle);
    rectangle->trigger();
    QApplication::processEvents();
    QToolButton* b = box->buttonFor(QStringLiteral("rectangle"));
    REQUIRE(b);
    REQUIRE(b->defaultAction() == rectangle);
    CHECK(pixelsAtContrast(onPaper(b->icon(), darkButton), darkButton, 3.0) >= kMinLegiblePixels);
    // The Tools menu keeps the application palette.
    const QColor lightButton = lightPalette().color(QPalette::Button);
    CHECK(pixelsAtContrast(onPaper(rectangle->icon(), lightButton), lightButton, 3.0) >= kMinLegiblePixels);
}
