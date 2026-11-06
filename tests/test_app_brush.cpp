// The brushes' common behaviour in the main window (PLAN §4, phase 2b.1):
// Tool Options' size, tip, strength, pressure mapping, ramp and alpha
// lock, the ramp shade brush, the tools that write shades or glyphs on a
// Pixels layer, and a stylus's pressure through QTabletEvent.

#include "tool_options.h"
#include "portable_random.h"
#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "glyph_bar.h"
#include "main_window.h"
#include "settings.h"
#include "tool.h"
#include "replay.h"

#include "scratch_settings.h"
#include "system_input.h"

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/steps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabletEvent>
#include <QToolButton>
#include <QUndoStack>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

// The view pixel in the middle of half-block pixel (x, py).
QPointF pixel(CanvasView* v, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    return QPointF(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
}

// `device` is the mouse by default; a stylus device makes the mouse event
// Qt synthesizes from a tablet event no widget took.
void mouse(CanvasView* v, QEvent::Type type, int x, int py, Qt::MouseButton button = Qt::LeftButton,
           const QPointingDevice* device = QPointingDevice::primaryPointingDevice()) {
    const QPointF at = pixel(v, x, py);
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held,
                  Qt::NoModifier, device);
    QApplication::sendEvent(v, &e);
}

void drag(CanvasView* v, std::initializer_list<QPoint> points, Qt::MouseButton button = Qt::LeftButton) {
    const QPoint* p = points.begin();
    mouse(v, QEvent::MouseButtonPress, p->x(), p->y(), button);
    for (++p; p != points.end(); ++p)
        mouse(v, QEvent::MouseMove, p->x(), p->y(), button);
    const QPoint last = *(points.end() - 1);
    mouse(v, QEvent::MouseButtonRelease, last.x(), last.y(), button);
}

// A pen on a tablet: position and pressure.
const QPointingDevice* stylus() {
    static const QPointingDevice dev(QStringLiteral("test stylus"), 4242, QInputDevice::DeviceType::Stylus,
                                     QPointingDevice::PointerType::Pen,
                                     QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                                     3);
    return &dev;
}

// Sends a tablet event at half-block pixel (x, py) with the buttons `held`
// after it (a barrel button among them); returns whether the view accepted
// it (so Qt makes no mouse event of it).
bool tabletHeld(CanvasView* v, QEvent::Type type, int x, int py, double pressure, Qt::MouseButton button,
                Qt::MouseButtons held) {
    const QPointF at = pixel(v, x, py);
    QTabletEvent e(type, stylus(), at, v->mapToGlobal(at), pressure, 0, 0, 0, 0, 0, Qt::NoModifier,
                   type == QEvent::TabletMove ? Qt::NoButton : button, held);
    e.setAccepted(false);
    QApplication::sendEvent(v, &e);
    return e.isAccepted();
}

bool tablet(CanvasView* v, QEvent::Type type, int x, int py, double pressure,
            Qt::MouseButton button = Qt::LeftButton) {
    return tabletHeld(v, type, x, py, pressure, button,
                      type == QEvent::TabletRelease ? Qt::NoButton : Qt::MouseButtons(button));
}

// A mouse button event with the buttons `held` after it given outright.
void mouseHeld(CanvasView* v, QEvent::Type type, int x, int py, Qt::MouseButton button, Qt::MouseButtons held) {
    const QPointF at = pixel(v, x, py);
    QMouseEvent e(type, at, v->mapToGlobal(at), button, held, Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}


void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

const Cell& at(MainWindow& w, int x, int y) { return w.document()->canvas().at(x, y); }

DragTool* dragTool(MainWindow& w) {
    auto* t = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(t);
    return t;
}

// The cells an edit changes.
std::set<std::pair<int, int>> cellsOf(const CellEdit& e) {
    std::set<std::pair<int, int>> s;
    for (const CellChange& c : e.changes)
        s.insert({c.x, c.y});
    return s;
}

// The cells that differ from the mirror image of their mirror cell.
std::set<std::pair<int, int>> asymmetric(MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    CellEdit left;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < (c.width() + 1) / 2; ++x)
            left.changes.push_back({x, y, c.at(x, y), c.at(x, y)});
    return cellsOf(mirrorEdit(c, left));
}

// The cells that differ from a blank canvas.
std::set<std::pair<int, int>> changed(MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    std::set<std::pair<int, int>> s;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (!(c.at(x, y) == Cell{}))
                s.insert({x, y});
    return s;
}

std::set<std::pair<int, int>> tipAt(int x, int y, int size, Tip tip, MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    const auto cells = footprint(x, y, size, tip, c.width(), c.height());
    return {cells.begin(), cells.end()};
}

Ramp ramp(const char* name) {
    const auto r = findRamp(name);
    REQUIRE(r);
    return *r;
}

// Fills the active layer's top 40 x 12 cells with whole cells (█, which a
// Pixels layer holds as two pixels), three columns of each of `colours` in
// turn, as one undo step.
void fillWhole(MainWindow& w, std::initializer_list<std::uint8_t> colours) {
    Document* doc = w.document();
    const std::vector<std::uint8_t> cs(colours);
    CellEdit e;
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 40; ++x) {
            Cell c;
            c.ch = kFullBlock;
            c.fg = c.bg = Color::indexed(cs[(x / 3) % cs.size()]);
            e.changes.push_back({x, y, doc->layerCanvas().at(x, y), c});
        }
    REQUIRE(doc->edit(e));
}

// The glyph spray's settings, forgotten before and after a test, from the
// scratch settings only.
struct FreshSpraySettings {
    static QString dir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }
    FreshSpraySettings() {
        REQUIRE(bwtest::settingsInScratch(dir()));
        appSettings()->remove(QStringLiteral("spray"));
    }
    ~FreshSpraySettings() {
        if (bwtest::settingsInScratch(dir()))
            appSettings()->remove(QStringLiteral("spray"));
    }
};

} // namespace

TEST_CASE("Tool Options is in the window and shows the view's settings") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QDockWidget* dock = w.toolOptionsDock();
    REQUIRE(dock);
    CHECK(dock->objectName() == QStringLiteral("toolOptionsDock"));
    CHECK(dock->isAncestorOf(w.toolOptions()));
    ToolOptions* d = w.toolOptions();
    // defaults: size 1, square, a quarter strength (a rung a pass), pressure
    // to size, steel, no lock
    CHECK(v->brushSettings().size == 1);
    CHECK(v->brushSettings().tip == Tip::Square);
    CHECK(v->brushSettings().strength == 0.25);
    CHECK(v->brushSettings().pressure == PressureTo::Size);
    CHECK(v->ramp().name == "steel");
    CHECK_FALSE(v->lockTransparent());
    CHECK(d->sizeBox()->value() == 1);
    CHECK(d->strengthBox()->value() == 25);
    CHECK(d->rampBox()->currentText() == QStringLiteral("steel"));
    CHECK(d->rampBox()->count() == int(namedRamps().size()));

    d->sizeBox()->setValue(4);
    d->tipBox()->setCurrentIndex(d->tipBox()->findData(int(Tip::Round)));
    d->strengthBox()->setValue(50);
    d->pressureBox()->setCurrentIndex(d->pressureBox()->findData(int(PressureTo::Strength)));
    d->rampBox()->setCurrentIndex(d->rampBox()->findText(QStringLiteral("fire")));
    d->alphaLockBox()->setChecked(true);
    CHECK(v->brushSettings() == BrushSettings{4, Tip::Round, 0.5, PressureTo::Strength});
    CHECK(v->ramp().name == "fire");
    CHECK(v->lockTransparent());

    // and follows the view
    BrushSettings s = v->brushSettings();
    s.size = 9;
    v->setBrushSettings(s);
    v->setRamp(ramp("ice"));
    v->setLockTransparent(false);
    CHECK(d->sizeBox()->value() == 9);
    CHECK(d->rampBox()->currentText() == QStringLiteral("ice"));
    CHECK_FALSE(d->alphaLockBox()->isChecked());
    CHECK(d->tipBox()->currentData().toInt() == int(Tip::Round));
}

TEST_CASE("the view clamps the brush size and strength and refuses a bad ramp") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    v->setBrushSettings(BrushSettings{0, Tip::Square, -1.0, PressureTo::Nothing});
    CHECK(v->brushSettings().size == 1);
    CHECK(v->brushSettings().strength == 0.0);
    v->setBrushSettings(BrushSettings{kMaxBrushSize + 5, Tip::Square, 3.0, PressureTo::Nothing});
    CHECK(v->brushSettings().size == kMaxBrushSize);
    CHECK(v->brushSettings().strength == 1.0);
    v->setRamp(Ramp{"one", {1}});
    CHECK(v->ramp().name == "steel");
}

TEST_CASE("Ctrl+[ and Ctrl+] step the brush size, and the dock follows") {
    MainWindow w;
    QAction* smaller = act(w, QStringLiteral("brushSmaller"));
    QAction* larger = act(w, QStringLiteral("brushLarger"));
    CHECK(smaller->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    CHECK(larger->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
    larger->trigger();
    larger->trigger();
    CHECK(w.canvasView()->brushSettings().size == 3);
    CHECK(w.toolOptions()->sizeBox()->value() == 3);
    for (int i = 0; i < 5; ++i)
        smaller->trigger();
    CHECK(w.canvasView()->brushSettings().size == 1);
}

TEST_CASE("the brushes paint with the dock's size and tip") {
    // (the half-block brush is a one-pixel pencil and keeps to it)
    for (const char* id : {"shade", "rampShade", "glyph", "colorize"}) {
        for (const Tip tip : {Tip::Square, Tip::Round}) {
            INFO(id << " tip " << int(tip));
            MainWindow w;
            CanvasView* v = w.canvasView();
            use(w, id);
            v->setForeground(LightGreen);
            v->setBackground(Blue);
            v->setBrushSettings(BrushSettings{5, tip, 1.0, PressureTo::Nothing});
            drag(v, {{10, 10}});
            CHECK(changed(w) == tipAt(10, 5, 5, tip, w));
            CHECK(w.document()->undoStack()->count() == 1);
        }
    }
}

TEST_CASE("ramp shade brush: left lightens along the dock's ramp, right darkens, one undo step a stroke") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    use(w, "rampShade");
    CHECK(act(w, QStringLiteral("rampShadeTool"))->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_T));
    CHECK(label->text() == QStringLiteral("Ramp steel"));
    const Ramp steel = ramp("steel");

    // a quarter strength: a rung a pass, blank (black) is rung 0
    drag(v, {{0, 0}, {6, 0}});
    for (int x = 0; x <= 6; ++x)
        CHECK(rungOf(steel, at(w, x, 0)) == 1);
    CHECK(undo->count() == 1);
    drag(v, {{0, 0}, {2, 0}});
    CHECK(rungOf(steel, at(w, 1, 0)) == 2);
    CHECK(rungOf(steel, at(w, 4, 0)) == 1);
    CHECK(undo->count() == 2);
    drag(v, {{1, 0}, {4, 0}}, Qt::RightButton);
    CHECK(rungOf(steel, at(w, 1, 0)) == 1);
    CHECK(rungOf(steel, at(w, 4, 0)) == 0);
    CHECK(undo->count() == 3);
    undo->undo();
    CHECK(rungOf(steel, at(w, 1, 0)) == 2);
    CHECK(rungOf(steel, at(w, 4, 0)) == 1);

    // full strength: a whole colour a pass
    w.toolOptions()->strengthBox()->setValue(100);
    drag(v, {{10, 0}});
    CHECK(rungOf(steel, at(w, 10, 0)) == 4);
    CHECK(at(w, 10, 0) == rungCell(steel, 4));

    // another ramp from the dock: off-ramp cells (steel's grey) are left
    // alone, the status says which
    w.toolOptions()->rampBox()->setCurrentIndex(w.toolOptions()->rampBox()->findText(QStringLiteral("fire")));
    CHECK(label->text() == QStringLiteral("Ramp fire"));
    const Cell grey = at(w, 10, 0);
    drag(v, {{10, 0}, {12, 0}});
    CHECK(at(w, 10, 0) == grey);
    CHECK(at(w, 12, 0) == rungCell(ramp("fire"), 4));
}

TEST_CASE("changing the brush mid-drag redoes the preview, and release lands it") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    mouse(v, QEvent::MouseButtonPress, 10, 10);
    mouse(v, QEvent::MouseMove, 14, 10);
    CHECK(dragTool(w)->pending().changes.size() == 5);
    w.toolOptions()->sizeBox()->setValue(3);
    CHECK(cellsOf(dragTool(w)->pending()).size() == 3 * 7);
    w.toolOptions()->rampBox()->setCurrentIndex(w.toolOptions()->rampBox()->findText(QStringLiteral("fire")));
    for (const CellChange& c : dragTool(w)->pending().changes)
        CHECK(c.after == rungCell(ramp("fire"), 1));
    const CellEdit shown = dragTool(w)->pending();
    mouse(v, QEvent::MouseButtonRelease, 14, 10);
    for (const CellChange& c : shown.changes)
        CHECK(at(w, c.x, c.y) == c.after);
    CHECK(changed(w) == cellsOf(shown));
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("the ramp shade brush refuses a Pixels layer and says why in the status bar", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    QUndoStack* undo = doc->undoStack();
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    use(w, "rampShade");
    CHECK(label->text() == QStringLiteral("Ramp steel"));
    const LayerId cells = doc->activeLayer();
    // made active with the brush already chosen: the status follows
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
    // opaque black pixels: steel's rung 0
    fillWhole(w, {Black});
    REQUIRE(rungOf(ramp("steel"), doc->active()->cells.at(2, 4)) == 0);
    CHECK(label->isVisibleTo(&w));
    CHECK(label->text() == QStringLiteral("The ramp shade brush can't paint a Pixels layer: it holds no shades"));
    CHECK(dragTool(w)->status() == label->text());
    const Canvas layer = doc->active()->cells, shown = doc->canvas();
    const int steps = undo->count();

    // a quarter strength steps a rung, a shade the layer can't hold (it was
    // dropped without a word); full strength steps a whole colour, which it
    // can (it landed, a solid grey fill)
    for (const int strength : {25, 100}) {
        INFO("strength " << strength);
        w.toolOptions()->strengthBox()->setValue(strength);
        mouse(v, QEvent::MouseButtonPress, 2, 8);
        mouse(v, QEvent::MouseMove, 30, 8);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(dragTool(w)->pending().empty());
        mouse(v, QEvent::MouseButtonRelease, 30, 8);
        CHECK(doc->active()->cells == layer);
        CHECK(doc->canvas() == shown);
        CHECK(undo->count() == steps);
        CHECK(label->text().contains(QStringLiteral("Pixels layer")));
    }

    // back on the cells layer the brush paints, and the status says so
    w.toolOptions()->strengthBox()->setValue(25);
    REQUIRE(doc->setActiveLayer(cells));
    CHECK(label->text() == QStringLiteral("Ramp steel"));
    drag(v, {{2, 8}, {30, 8}});
    CHECK(rungOf(ramp("steel"), doc->active()->cells.at(2, 4)) == 1);
    CHECK(undo->count() == steps + 1);
}

TEST_CASE("the ramp shade brush shades a Text layer as it does a cells layer", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    QUndoStack* undo = doc->undoStack();
    use(w, "rampShade");
    doc->addLayer(LayerKind::Text);
    REQUIRE(doc->active()->props.kind == LayerKind::Text);
    fillWhole(w, {Black});
    CHECK(dragTool(w)->status() == QStringLiteral("Ramp steel"));
    const Ramp steel = ramp("steel");
    const int steps = undo->count();

    // a rung a pass at a quarter strength, a whole colour at full
    drag(v, {{2, 8}, {30, 8}});
    for (int x = 2; x <= 30; ++x) {
        INFO(x);
        CHECK(rungOf(steel, doc->active()->cells.at(x, 4)) == 1);
    }
    CHECK(undo->count() == steps + 1);
    w.toolOptions()->strengthBox()->setValue(100);
    drag(v, {{2, 12}, {30, 12}});
    for (int x = 2; x <= 30; ++x) {
        INFO(x);
        CHECK(doc->active()->cells.at(x, 6) == rungCell(steel, 4));
    }
    CHECK(undo->count() == steps + 2);
}

TEST_CASE("the ramp tools that write shades refuse a Pixels layer; outline and despeckle paint it", "[layers]") {
    FreshSpraySettings fresh;
    struct Case {
        const char* id;
        SprayKind spray;
        bool refuses;
    };
    const Case cases[] = {
        {"rampShade", SprayKind::Stars, true}, {"form", SprayKind::Stars, true},
        {"soft", SprayKind::Stars, true},      {"smudge", SprayKind::Stars, true},
        {"rimLight", SprayKind::Stars, true},  {"texture", SprayKind::Stars, true},
        {"spray", SprayKind::Haze, true},      {"spray", SprayKind::Smoke, true},
        // stars are glyphs, which it doesn't hold either (#164)
        {"spray", SprayKind::Stars, true},
        // whole cells and half blocks, which a pixel layer holds (the outline
        // leaves its shade rungs out there)
        {"outline", SprayKind::Stars, false},  {"despeckle", SprayKind::Stars, false},
    };
    for (const Case& c : cases) {
        INFO(c.id << " " << sprayKindName(c.spray).toStdString());
        MainWindow w;
        Document* doc = w.document();
        CanvasView* v = w.canvasView();
        act(w, QStringLiteral("newPixelLayer"))->trigger();
        // steel's colours in stripes, each tool something to work on
        fillWhole(w, {Black, DarkGrey, Grey, White});
        w.toolOptions()->sizeBox()->setValue(3);
        use(w, c.id);
        if (auto* spray = qobject_cast<SprayTool*>(w.tools()->active()))
            spray->setKind(c.spray);
        DragTool* t = dragTool(w);
        CHECK(t->status().contains(QStringLiteral("can't paint a Pixels layer")) == c.refuses);
        const Canvas layer = doc->active()->cells;
        const int steps = doc->undoStack()->count();
        mouse(v, QEvent::MouseButtonPress, 2, 8);
        mouse(v, QEvent::MouseMove, 30, 8);
        CHECK(t->dragging() == !c.refuses);
        mouse(v, QEvent::MouseButtonRelease, 30, 8);
        if (c.refuses) {
            CHECK(doc->active()->cells == layer);
            CHECK(doc->undoStack()->count() == steps);
        }

        // a text layer is painted as a cells layer
        doc->addLayer(LayerKind::Text);
        CHECK_FALSE(t->status().contains(QStringLiteral("Pixels layer")));
        mouse(v, QEvent::MouseButtonPress, 2, 8);
        CHECK(t->dragging());
        mouse(v, QEvent::MouseButtonRelease, 2, 8);
    }
}

// #164: the shade brush (░▒▓), the glyph brush with a glyph a Pixels layer
// can't hold and the spray's stars took the press there, showed the stroke,
// and landed nothing on release, no undo step and no word why: the layer
// refused the edit (fitLayerEdit). Now each declines the press and says why,
// and a cells or text layer still takes the stroke.
TEST_CASE("the shade brush, the glyph brush and the spray's stars refuse a Pixels layer and say why", "[layers]") {
    FreshSpraySettings fresh;
    struct Case {
        const char* id;
        char32_t glyph;  // the glyph brush's
        QPoint from, to;  // half-block pixels
        const char* message;
    };
    // the shade brush on blank cells, which it builds up to ░; the others on
    // black whole cells (fillWhole), open ground for the stars
    const Case cases[] = {
        {"shade", 0, {50, 30}, {60, 30}, "The shade brush can't paint a Pixels layer: it holds no shades"},
        {"glyph", U'░', {2, 8}, {30, 8},
         "The glyph brush can't paint a Pixels layer: it holds no ░; right button erases"},
        {"glyph", U'☺', {2, 8}, {30, 8},
         "The glyph brush can't paint a Pixels layer: it holds no ☺; right button erases"},
        {"spray", 0, {2, 8}, {30, 8}, "The glyph spray can't paint a Pixels layer: it holds no stars"},
    };
    for (const Case& c : cases) {
        INFO(c.id << " " << QString::fromUcs4(&c.glyph, 1).toStdString());
        MainWindow w;
        Document* doc = w.document();
        CanvasView* v = w.canvasView();
        QUndoStack* undo = doc->undoStack();
        auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
        REQUIRE(label);
        w.toolOptions()->sizeBox()->setValue(9);
        use(w, c.id);
        if (auto* glyph = qobject_cast<GlyphTool*>(w.tools()->active()))
            glyph->setGlyph(c.glyph);
        if (auto* spray = qobject_cast<SprayTool*>(w.tools()->active())) {
            spray->setKind(SprayKind::Stars);
            spray->setDensity(kMaxSprayDensity);
            spray->setSeed(17);
        }
        DragTool* t = dragTool(w);
        const LayerId cells = doc->activeLayer();
        fillWhole(w, {Black});
        CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));

        act(w, QStringLiteral("newPixelLayer"))->trigger();
        REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
        fillWhole(w, {Black});
        CHECK(label->isVisibleTo(&w));
        CHECK(label->text() == QString::fromUtf8(c.message));
        CHECK(t->status() == label->text());
        const Canvas layer = doc->active()->cells, shown = doc->canvas();
        const int steps = undo->count();
        mouse(v, QEvent::MouseButtonPress, c.from.x(), c.from.y());
        mouse(v, QEvent::MouseMove, c.to.x(), c.to.y());
        CHECK_FALSE(t->dragging());
        CHECK(t->pending().empty());
        mouse(v, QEvent::MouseButtonRelease, c.to.x(), c.to.y());
        CHECK(doc->active()->cells == layer);
        CHECK(doc->canvas() == shown);
        CHECK(undo->count() == steps);
        CHECK(label->text() == QString::fromUtf8(c.message));

        // a cells layer and a text layer take the stroke
        const auto paints = [&] {
            CHECK_FALSE(t->status().contains(QStringLiteral("Pixels layer")));
            CHECK(label->text() == t->status());
            const Canvas before = doc->active()->cells;
            const int n = undo->count();
            drag(v, {c.from, c.to});
            CHECK_FALSE(doc->active()->cells == before);
            CHECK(undo->count() == n + 1);
        };
        REQUIRE(doc->setActiveLayer(cells));
        paints();
        doc->addLayer(LayerKind::Text);
        REQUIRE(doc->active()->props.kind == LayerKind::Text);
        fillWhole(w, {Black});
        paints();
    }
}

// The glyph brush on a Pixels layer refuses only what the layer can't hold:
// █ ▀ ▄ paint it, the right button erases with any glyph, and ▀ is refused
// only where radial symmetry would write it on its side (▐ ▌), which
// dropped the whole stroke on release before (#164).
TEST_CASE("the glyph brush paints a Pixels layer with the glyphs it holds, as mirror mode writes them", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    QUndoStack* undo = doc->undoStack();
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
    fillWhole(w, {Black});
    use(w, "glyph");
    auto* t = qobject_cast<GlyphTool*>(w.tools()->active());
    REQUIRE(t);
    const auto lands = [&](QPoint from, QPoint to, Qt::MouseButton button = Qt::LeftButton) {
        const Canvas before = doc->active()->cells;
        const int n = undo->count();
        drag(v, {from, to}, button);
        CHECK_FALSE(doc->active()->cells == before);
        CHECK(undo->count() == n + 1);
    };
    const auto declined = [&](QPoint from, QPoint to) {
        const Canvas before = doc->active()->cells;
        const int n = undo->count();
        mouse(v, QEvent::MouseButtonPress, from.x(), from.y());
        CHECK_FALSE(t->dragging());
        mouse(v, QEvent::MouseMove, to.x(), to.y());
        mouse(v, QEvent::MouseButtonRelease, to.x(), to.y());
        CHECK(doc->active()->cells == before);
        CHECK(undo->count() == n);
    };

    // the right button erases whatever the glyph
    t->setGlyph(U'░');
    CHECK(label->text().contains(QStringLiteral("can't paint a Pixels layer")));
    lands({2, 2}, {10, 2}, Qt::RightButton);
    CHECK(doc->active()->cells.at(4, 1).transparent);
    for (const char32_t g : {kFullBlock, kUpperHalf, kLowerHalf}) {
        INFO(int(g));
        t->setGlyph(g);
        CHECK(label->text() == t->status());
        CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));
        lands({2, 4}, {10, 4});
        // the layer keeps its pixels (pixelLayerCell), not the glyph itself
        Cell written;
        written.ch = g;
        written.fg = v->pen().fg;
        written.bg = v->pen().bg;
        CHECK(halfBlocks(doc->active()->cells.at(4, 2)) == halfBlocks(written));
    }

    // ▀ mirrored is ▀ or ▄; turned a quarter it is ▐ or ▌
    t->setGlyph(kUpperHalf);
    v->setSymmetry({SymmetryMode::MirrorY, 4, std::nullopt});
    CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));
    lands({2, 6}, {10, 6});
    v->setSymmetry({SymmetryMode::Radial, 4, std::nullopt});
    CHECK(label->text() == QStringLiteral("The glyph brush can't paint a Pixels layer: it holds no ▀ turned on its "
                                          "side, which radial symmetry writes; right button erases"));
    CHECK(t->status() == label->text());
    declined({2, 10}, {10, 10});
    // a half turn only flips it
    v->setSymmetry({SymmetryMode::Radial, 2, std::nullopt});
    CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));
    lands({2, 12}, {10, 12});

    // radial symmetry turned on mid-drag ends the drag, as a press would be
    // declined, and nothing lands
    v->setMirror(false);
    const Canvas before = doc->active()->cells;
    const int n = undo->count();
    mouse(v, QEvent::MouseButtonPress, 2, 16);
    mouse(v, QEvent::MouseMove, 10, 16);
    REQUIRE(t->dragging());
    v->setSymmetry({SymmetryMode::Radial, 4, std::nullopt});
    CHECK_FALSE(t->dragging());
    CHECK(t->pending().empty());
    mouse(v, QEvent::MouseMove, 14, 16);
    mouse(v, QEvent::MouseButtonRelease, 14, 16);
    CHECK(doc->active()->cells == before);
    CHECK(undo->count() == n);
    CHECK(label->text().contains(QStringLiteral("can't paint a Pixels layer")));
}

// Codex round 1 on PR #165: on a Pixels layer, a glyph picked mid-drag
// (F1-F10, or a click on the glyph bar, which sends the F-key) that the
// layer can't hold left the drag and its preview, and the old glyph's
// stroke landed on release with an undo step, while the status said the
// layer refused the new one. Picking it ends the drag as the press would
// have been declined. A glyph set switch picks no glyph: the drag goes on.
TEST_CASE("a glyph the Pixels layer refuses, picked mid-drag, ends the glyph brush's drag", "[layers]") {
    const bool viaBar = GENERATE(false, true);
    INFO((viaBar ? "glyph bar" : "F-key"));
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    QUndoStack* undo = doc->undoStack();
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
    use(w, "glyph");
    auto* t = qobject_cast<GlyphTool*>(w.tools()->active());
    REQUIRE(t);
    // the sixth set, ░▒▓█▀▄▌▐■·
    v->setGlyphSet(5);
    const auto pick = [&](int k) {
        if (viaBar) {
            w.glyphBar()->cell(k)->click();
        } else {
            QKeyEvent f(QEvent::KeyPress, Qt::Key_F1 + k, Qt::NoModifier);
            QApplication::sendEvent(v, &f);
        }
    };
    pick(3);
    REQUIRE(t->glyph() == kFullBlock);
    CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));

    const Canvas layer = doc->active()->cells, shown = doc->canvas();
    const int steps = undo->count();
    mouse(v, QEvent::MouseButtonPress, 2, 8);
    mouse(v, QEvent::MouseMove, 30, 8);
    REQUIRE(t->dragging());
    REQUIRE_FALSE(t->pending().empty());
    // another set changes no glyph
    v->setGlyphSet(6);
    CHECK(t->dragging());
    CHECK_FALSE(t->pending().empty());
    v->setGlyphSet(5);
    pick(0);
    REQUIRE(t->glyph() == U'░');
    CHECK(label->text() == QStringLiteral("The glyph brush can't paint a Pixels layer: it holds no ░; right "
                                          "button erases"));
    CHECK_FALSE(t->dragging());
    CHECK(t->pending().empty());
    mouse(v, QEvent::MouseMove, 40, 8);
    mouse(v, QEvent::MouseButtonRelease, 40, 8);
    CHECK(doc->active()->cells == layer);
    CHECK(doc->canvas() == shown);
    CHECK(undo->count() == steps);
}

// A glyph picked mid-drag that the layer holds redoes the drag with it, as
// the other settings do: the stroke shown is the new glyph's, and it lands.
TEST_CASE("a glyph picked mid-drag redoes the glyph brush's drag with it", "[layers]") {
    const bool pixels = GENERATE(false, true);
    INFO((pixels ? "Pixels layer" : "cells layer"));
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    QUndoStack* undo = doc->undoStack();
    if (pixels)
        act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "glyph");
    auto* t = qobject_cast<GlyphTool*>(w.tools()->active());
    REQUIRE(t);
    t->setGlyph(kFullBlock);
    const int steps = undo->count();
    mouse(v, QEvent::MouseButtonPress, 2, 8);
    mouse(v, QEvent::MouseMove, 30, 8);
    REQUIRE(t->dragging());
    t->setGlyph(kUpperHalf);
    REQUIRE(t->dragging());
    const CellEdit e = t->pending();
    REQUIRE_FALSE(e.empty());
    Cell written;
    written.ch = kUpperHalf;
    written.fg = v->pen().fg;
    written.bg = v->pen().bg;
    for (const auto& c : e.changes)
        CHECK(halfBlocks(c.after) == halfBlocks(written));
    mouse(v, QEvent::MouseButtonRelease, 30, 8);
    CHECK(undo->count() == steps + 1);
    CHECK(halfBlocks(doc->active()->cells.at(4, 4)) == halfBlocks(written));
}

// A change of active layer, or of the active layer's kind, to one the glyph
// can't paint doesn't reach the refusal mid-drag: the drag lands on the
// layer it was editing first, which took it, and no drag is left.
TEST_CASE("a switch to a Pixels layer mid-drag lands the glyph brush's drag first", "[layers]") {
    const bool kind = GENERATE(false, true);
    INFO((kind ? "kind" : "active layer"));
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    QUndoStack* undo = doc->undoStack();
    const LayerId cells = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId pixels = doc->activeLayer();
    REQUIRE(doc->setActiveLayer(cells));
    use(w, "glyph");
    auto* t = qobject_cast<GlyphTool*>(w.tools()->active());
    REQUIRE(t);
    t->setGlyph(U'░');
    const int steps = undo->count();
    mouse(v, QEvent::MouseButtonPress, 2, 8);
    mouse(v, QEvent::MouseMove, 30, 8);
    REQUIRE(t->dragging());
    if (kind) {
        LayerProperties p = doc->active()->props;
        p.kind = LayerKind::Pixels;
        REQUIRE(doc->setLayerProperties(cells, p, QStringLiteral("Kind")));
        REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
        // the stroke, then the kind
        CHECK(undo->count() == steps + 2);
        CHECK(undo->text(steps + 1) == QStringLiteral("Kind"));
    } else {
        REQUIRE(doc->setActiveLayer(pixels));
        CHECK(undo->count() == steps + 1);
    }
    CHECK_FALSE(t->dragging());
    CHECK(t->pending().empty());
    const int landed = undo->count();
    mouse(v, QEvent::MouseMove, 40, 8);
    mouse(v, QEvent::MouseButtonRelease, 40, 8);
    CHECK(undo->count() == landed);
    if (!kind) {
        REQUIRE(doc->setActiveLayer(cells));
        CHECK(doc->active()->cells.at(4, 4).ch == U'░');
        CHECK(doc->active()->cells.at(30, 4).ch == U'░');
        CHECK(doc->active()->cells.at(35, 4).ch != U'░');
    }
}

// The spray's refusal follows its kind: a Pixels layer holds neither stars
// nor the haze and smoke shades, and the status names which (#164). On a
// cells layer a kind changed mid-drag redoes the drag and it lands.
TEST_CASE("the glyph spray's refusal of a Pixels layer follows its kind", "[layers]") {
    FreshSpraySettings fresh;
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    QUndoStack* undo = doc->undoStack();
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    w.toolOptions()->sizeBox()->setValue(9);
    use(w, "spray");
    auto* t = qobject_cast<SprayTool*>(w.tools()->active());
    REQUIRE(t);
    t->setDensity(kMaxSprayDensity);
    t->setSeed(17);
    // open ground for the stars
    const LayerId cells = doc->activeLayer();
    fillWhole(w, {Black});
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    fillWhole(w, {Black});
    const Canvas layer = doc->active()->cells;
    const int steps = undo->count();
    const QString stars = QStringLiteral("The glyph spray can't paint a Pixels layer: it holds no stars");
    const QString shades = QStringLiteral("The glyph spray can't paint a Pixels layer: it holds no shades");
    for (const SprayKind k : {SprayKind::Stars, SprayKind::Haze, SprayKind::Stars, SprayKind::Smoke, SprayKind::Stars}) {
        INFO(sprayKindName(k).toStdString());
        t->setKind(k);
        CHECK(label->text() == (k == SprayKind::Stars ? stars : shades));
        CHECK(t->status() == label->text());
        for (const Qt::MouseButton b : {Qt::LeftButton, Qt::RightButton}) {
            mouse(v, QEvent::MouseButtonPress, 2, 8, b);
            CHECK_FALSE(t->dragging());
            mouse(v, QEvent::MouseMove, 30, 8, b);
            mouse(v, QEvent::MouseButtonRelease, 30, 8, b);
        }
    }
    CHECK(doc->active()->cells == layer);
    CHECK(undo->count() == steps);

    REQUIRE(doc->setActiveLayer(cells));
    t->setKind(SprayKind::Haze);
    CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));
    const Canvas before = doc->active()->cells;
    mouse(v, QEvent::MouseButtonPress, 2, 8);
    mouse(v, QEvent::MouseMove, 30, 8);
    REQUIRE(t->dragging());
    t->setKind(SprayKind::Stars);
    CHECK(t->dragging());
    CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));
    const CellEdit shown = t->pending();
    CHECK_FALSE(shown.empty());
    mouse(v, QEvent::MouseButtonRelease, 30, 8);
    CHECK_FALSE(doc->active()->cells == before);
    CHECK(undo->count() == steps + 1);
}

// Codex round 1 on PR #161: an outline click where the darkest line would
// add a lint finding at some cells, so they take the next rung lighter, a
// shade on a cells layer (K▓k, steel's rung 5). A Pixels layer can't hold
// that shade, and the whole stroke was dropped on release without a word,
// its whole-colour cells too. There the outline leaves the shade rungs out:
// a cell takes a whole colour or stays as it was, and what the stroke shows
// is what lands.
TEST_CASE("the outline writes no shade on a Pixels layer and its stroke lands as shown", "[layers]") {
    const Ramp steel = ramp("steel");
    // pixel cells only (top, bottom), so a Pixels layer holds them as they are
    const auto build = [](MainWindow& w, bool pixels) {
        if (pixels)
            act(w, QStringLiteral("newPixelLayer"))->trigger();
        Document* doc = w.document();
        CellEdit e;
        const auto px = [&](int x, int y, std::uint8_t top, std::uint8_t bottom) {
            e.changes.push_back({x, y, doc->layerCanvas().at(x, y), halfBlockCell(HalfPair{top, bottom}, true)});
        };
        px(3, 0, DarkGrey, Grey);
        for (int y = 1; y <= 2; ++y)
            for (int x = 2; x <= 4; ++x)
                px(x, y, Grey, Grey);
        px(2, 3, Grey, Grey);
        px(3, 3, Black, Grey);
        px(4, 3, Grey, White);
        px(7, 4, Grey, Grey);
        REQUIRE(doc->edit(e));
        w.canvasView()->setBrushSettings(BrushSettings{3, Tip::Square, 1.0, PressureTo::Nothing});
        use(w, "outline");
    };
    const auto press = [](MainWindow& w) {
        mouse(w.canvasView(), QEvent::MouseButtonPress, 3, 4);
        return dragTool(w)->pending();
    };
    const auto rungs = [&](const CellEdit& e) {
        std::map<std::pair<int, int>, int> out;
        for (const CellChange& c : e.changes)
            out[{c.x, c.y}] = rungOf(steel, c.after).value_or(-1);
        return out;
    };

    // on a cells layer the click lines the top in K and the left side in K▓k
    {
        MainWindow w;
        build(w, false);
        CHECK(rungs(press(w)) == std::map<std::pair<int, int>, int>{{{2, 1}, 4}, {{3, 1}, 4}, {{4, 1}, 4},
                                                                     {{2, 2}, 5}, {{2, 3}, 5}});
    }

    MainWindow w;
    Document* doc = w.document();
    QUndoStack* undo = doc->undoStack();
    build(w, true);
    const int steps = undo->count();
    const CellEdit shown = press(w);
    // the K the lint allows there, and no shade
    CHECK(rungs(shown) == std::map<std::pair<int, int>, int>{{{3, 1}, 4}, {{4, 1}, 4}, {{4, 2}, 4}});
    for (const CellChange& c : shown.changes) {
        INFO(c.x << "," << c.y);
        CHECK(halfBlocks(c.after, true));
    }
    mouse(w.canvasView(), QEvent::MouseButtonRelease, 3, 4);
    CHECK(undo->count() == steps + 1);
    for (const CellChange& c : shown.changes)
        CHECK(doc->active()->cells.at(c.x, c.y) == c.after);
}

TEST_CASE("the alpha lock leaves transparent cells alone") {
    for (const char* id : {"halfBlock", "shade", "rampShade", "glyph", "colorize"}) {
        INFO(id);
        MainWindow w;
        CanvasView* v = w.canvasView();
        Document* doc = w.document();
        // a cells layer over the background, opaque at 0 and 2, clear at 1
        doc->addLayer(LayerKind::Cells);
        const Cell clear = transparentCell();
        REQUIRE(doc->edit(CellEdit{{{0, 0, clear, Cell{}}, {2, 0, clear, Cell{}}}}));
        use(w, id);
        v->setForeground(LightGreen);
        w.toolOptions()->alphaLockBox()->setChecked(true);
        CHECK(doc->active()->props.alphaLocked);
        drag(v, {{0, 0}, {2, 0}});
        CHECK(doc->layerCanvas().at(1, 0) == clear);
        CHECK_FALSE(doc->layerCanvas().at(0, 0) == Cell{});
        CHECK_FALSE(doc->layerCanvas().at(2, 0) == Cell{});
        w.toolOptions()->alphaLockBox()->setChecked(false);
        drag(v, {{1, 0}});
        // colorize recolours what is there, and a hole has nothing to recolour
        CHECK((doc->layerCanvas().at(1, 0) == clear) == (std::string(id) == "colorize"));
    }
}

TEST_CASE("a stylus paints through the tool, its pressure scaling the size") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
    // 5 × 0.6 is 3 cells
    CHECK(tablet(v, QEvent::TabletPress, 10, 10, 0.6));
    CHECK(cellsOf(dragTool(w)->pending()) == tipAt(10, 5, 3, Tip::Square, w));
    // pressing harder where it is grows the dab
    CHECK(tablet(v, QEvent::TabletMove, 10, 10, 1.0));
    CHECK(cellsOf(dragTool(w)->pending()) == tipAt(10, 5, 5, Tip::Square, w));
    // a release reads no pressure (0): it lands what the stroke showed
    const CellEdit shown = dragTool(w)->pending();
    CHECK(tablet(v, QEvent::TabletRelease, 10, 10, 0.0));
    CHECK(changed(w) == cellsOf(shown));
    CHECK(w.document()->undoStack()->count() == 1);

    // a mouse presses fully
    drag(v, {{30, 10}});
    CHECK(changed(w).count({32, 5}));
    CHECK(changed(w).count({28, 5}));
}

TEST_CASE("a stylus's pressure can scale the ramp brush's strength instead") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength});
    const Ramp steel = ramp("steel");
    // full strength is four rungs; half the pressure two, a light touch one
    CHECK(tablet(v, QEvent::TabletPress, 0, 0, 0.5));
    CHECK(tablet(v, QEvent::TabletRelease, 0, 0, 0.0));
    CHECK(rungOf(steel, at(w, 0, 0)) == 2);
    tablet(v, QEvent::TabletPress, 2, 0, 0.05);
    tablet(v, QEvent::TabletRelease, 2, 0, 0.0);
    CHECK(rungOf(steel, at(w, 2, 0)) == 1);
    tablet(v, QEvent::TabletPress, 4, 0, 1.0);
    tablet(v, QEvent::TabletRelease, 4, 0, 0.0);
    CHECK(rungOf(steel, at(w, 4, 0)) == 4);
    CHECK(w.document()->undoStack()->count() == 3);
}

TEST_CASE("a stylus event the tool doesn't take is left for the mouse") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    w.tools()->setActive(QStringLiteral("keyboard"));
    CHECK_FALSE(tablet(v, QEvent::TabletPress, 4, 4, 0.5));
    CHECK_FALSE(tablet(v, QEvent::TabletRelease, 4, 4, 0.0));
    CHECK(w.document()->undoStack()->count() == 0);
}

TEST_CASE("mirror mode with an even brush paints the exact mirror") {
    for (const char* id : {"shade", "rampShade", "glyph"}) {
        for (const Tip tip : {Tip::Square, Tip::Round}) {
            INFO(id << " tip " << int(tip));
            MainWindow w;
            CanvasView* v = w.canvasView();
            use(w, id);
            v->setForeground(LightGreen);
            v->setBrushSettings(BrushSettings{4, tip, 0.25, PressureTo::Nothing});
            act(w, QStringLiteral("mirrorMode"))->trigger();
            REQUIRE(v->mirror());
            drag(v, {{6, 10}});
            const int width = w.document()->canvas().width();
            const auto cells = changed(w);
            for (const auto& [x, y] : cells) {
                INFO("cell " << x << "," << y);
                CHECK(cells.count({width - 1 - x, y}));
            }
            CHECK(cells.size() == 2 * tipAt(6, 5, 4, tip, w).size());
        }
    }
}

TEST_CASE("a release lands with the stroke's last pressure, not the release's") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
    // The release moves to another cell and reads 0: the line out to it is
    // at the stroke's pressure (1.0, five rows), not at size 1.
    CHECK(tablet(v, QEvent::TabletPress, 10, 10, 1.0));
    CHECK(tablet(v, QEvent::TabletRelease, 20, 10, 0.0));
    CHECK(changed(w).size() == 5u * 15u);
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("pressing harder on the touch-down cell repaints it at the stronger strength") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength});
    const Ramp steel = ramp("steel");
    CHECK(tablet(v, QEvent::TabletPress, 0, 0, 0.05));
    CHECK(rungOf(steel, dragTool(w)->pending().changes.at(0).after) == 1);
    CHECK(tablet(v, QEvent::TabletMove, 0, 0, 0.5));
    CHECK(rungOf(steel, dragTool(w)->pending().changes.at(0).after) == 2);
    CHECK(tablet(v, QEvent::TabletMove, 0, 0, 1.0));
    // easing off leaves it at the peak
    CHECK(tablet(v, QEvent::TabletMove, 0, 0, 0.3));
    CHECK(tablet(v, QEvent::TabletRelease, 0, 0, 0.0));
    CHECK(rungOf(steel, at(w, 0, 0)) == 4);
    CHECK(w.document()->undoStack()->count() == 1);
    // A light touch-down, then a line out at full pressure: one pass, the
    // start cell as deep as the rest.
    CHECK(tablet(v, QEvent::TabletPress, 0, 2, 0.05));
    CHECK(tablet(v, QEvent::TabletMove, 0, 2, 1.0));
    CHECK(tablet(v, QEvent::TabletMove, 3, 2, 1.0));
    CHECK(tablet(v, QEvent::TabletRelease, 3, 2, 0.0));
    for (int x = 0; x <= 3; ++x)
        CHECK(rungOf(steel, at(w, x, 1)) == 4);
}

TEST_CASE("a softer move on the dab's own cell is no dab: the smudge after it paints as the replay's (#382)") {
    // A move the tool drops would still carry smudge's value on had it been
    // stamped: the last cell would take rung 7 for 6. Replay drops it by the
    // same rule (bw::takesDab), so the two agree.
    const char* steps = R"({"version": 1, "start": {"blank": [3, 1]}, "ice": true, "steps": [
        {"name": "light", "action": "glyph", "glyph": "█", "fg": 7, "points": [[0, 0]]},
        {"name": "shade", "action": "glyph", "glyph": "▒", "fg": 7, "bg": 8, "points": [[1, 0], [2, 0]]},
        {"name": "smear", "action": "smudge", "ramp": "steel", "size": 1, "strength": 1, "pressure": "strength",
         "points": [[0, 0, 1], [1, 0, 0.25], [1, 0, 0.125], [2, 0, 0.75]]}]})";
    const steps::Script script = steps::parse(steps);
    const bwcli::replay::Outcome out = bwcli::replay::run(script, bwcli::replay::start(script, {}, {}));
    REQUIRE(out.captures.size() == 3);
    const Canvas& smeared = out.captures[1].project.layers.layers()[0].cells;
    const Canvas& replayed = out.captures[2].project.layers.layers()[0].cells;

    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    doc->clear(3, 1, true);
    CellEdit e;
    for (int x = 0; x < 3; ++x)
        e.changes.push_back({x, 0, doc->layerCanvas().at(x, 0), smeared.at(x, 0)});
    REQUIRE(doc->edit(e));
    use(w, "smudge");
    const Ramp steel = ramp("steel");
    v->setRamp(steel);
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength});
    CHECK(rungOf(steel, at(w, 0, 0)) == 8);
    CHECK(rungOf(steel, at(w, 1, 0)) == 6);
    CHECK(rungOf(steel, at(w, 2, 0)) == 6);
    CHECK(tablet(v, QEvent::TabletPress, 0, 0, 1.0));
    CHECK(tablet(v, QEvent::TabletMove, 1, 0, 0.25));
    CHECK(tablet(v, QEvent::TabletMove, 1, 0, 0.125));
    CHECK(tablet(v, QEvent::TabletMove, 2, 0, 0.75));
    CHECK(tablet(v, QEvent::TabletRelease, 2, 0, 0.0));
    CHECK(rungOf(steel, at(w, 0, 0)) == 8);
    CHECK(rungOf(steel, at(w, 1, 0)) == 7);
    CHECK(rungOf(steel, at(w, 2, 0)) == 6);
    CHECK(doc->canvas() == replayed);
}

TEST_CASE("with iCE off the ramp brush writes no shade that needs a bright background") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("iceColours"))->trigger();  // iCE off, as a file without the flag opens
    REQUIRE_FALSE(w.document()->ansi().ice);
    use(w, "rampShade");
    const Ramp fire = ramp("fire");  // Black Red LightRed Yellow White
    v->setRamp(fire);
    BrushSettings s{1, Tip::Square, 1.0, PressureTo::Nothing};
    v->setBrushSettings(s);
    drag(v, {{0, 0}});
    drag(v, {{0, 0}});
    REQUIRE(rungOf(fire, at(w, 0, 0)) == 8);  // LightRed
    // A rung more is a LightRed/Yellow shade: two brights, a bright
    // background either way round, which iCE off would show blinking on Red.
    s.strength = 0.25;
    v->setBrushSettings(s);
    drag(v, {{0, 0}});
    CHECK(at(w, 0, 0).bg.paletteIndex() < 8);
    CHECK(rungOf(fire, iceCell(at(w, 0, 0), false)) == 8);
    // Every ramp, every strength, up then down: each cell written shows as a
    // rung of the ramp with iCE off.
    for (const Ramp& r : namedRamps()) {
        v->setRamp(r);
        for (const double strength : {0.25, 0.5, 0.75, 1.0}) {
            INFO(r.name << " strength " << strength);
            s.strength = strength;
            v->setBrushSettings(s);
            for (const Qt::MouseButton b : {Qt::LeftButton, Qt::RightButton})
                for (int pass = 0; pass < 2 * r.topRung(); ++pass) {
                    drag(v, {{2, 0}}, b);
                    const Cell& c = at(w, 2, 0);
                    CHECK(c.bg.paletteIndex() < 8);
                    CHECK(rungOf(r, iceCell(c, false)));
                }
        }
    }
}

TEST_CASE("the device that starts a drag owns it") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    const std::set<std::pair<int, int>> two = {{2, 2}, {3, 2}};

    SECTION("a stylus during a mouse drag neither extends nor lands it") {
        mouse(v, QEvent::MouseButtonPress, 2, 4);
        // taken (so Qt makes no mouse event of it), and ignored
        CHECK(tablet(v, QEvent::TabletPress, 20, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletMove, 21, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 21, 4, 0.0));
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 0);
        mouse(v, QEvent::MouseMove, 3, 4);
        mouse(v, QEvent::MouseButtonRelease, 3, 4);
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a mouse during a stylus drag neither extends nor lands it") {
        CHECK(tablet(v, QEvent::TabletPress, 2, 4, 1.0));
        mouse(v, QEvent::MouseButtonPress, 20, 4);
        mouse(v, QEvent::MouseMove, 21, 4);
        mouse(v, QEvent::MouseButtonRelease, 21, 4);
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 0);
        CHECK(tablet(v, QEvent::TabletMove, 3, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 3, 4, 0.0));
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a stylus drag ignores mouse events made from the stylus too") {
        // A platform that makes mouse events of every stylus event: they
        // don't start the drag again or add to it at full pressure.
        CHECK(tablet(v, QEvent::TabletPress, 2, 4, 1.0));
        mouse(v, QEvent::MouseButtonPress, 2, 4, Qt::LeftButton, stylus());
        mouse(v, QEvent::MouseMove, 6, 4, Qt::LeftButton, stylus());
        mouse(v, QEvent::MouseButtonRelease, 6, 4, Qt::LeftButton, stylus());
        CHECK(w.document()->undoStack()->count() == 0);
        CHECK(tablet(v, QEvent::TabletMove, 3, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 3, 4, 0.0));
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a drag Qt's mouse events from the stylus start is theirs, once") {
        // The tool took the mouse event Qt made of a stylus press: the
        // stylus's own events stay unaccepted, so Qt goes on making mouse
        // events of them, and those drive the drag.
        mouse(v, QEvent::MouseButtonPress, 2, 4, Qt::LeftButton, stylus());
        CHECK_FALSE(tablet(v, QEvent::TabletMove, 3, 4, 1.0));
        mouse(v, QEvent::MouseMove, 3, 4, Qt::LeftButton, stylus());
        // the real mouse is another device
        mouse(v, QEvent::MouseButtonPress, 20, 4);
        mouse(v, QEvent::MouseMove, 21, 4);
        mouse(v, QEvent::MouseButtonRelease, 21, 4);
        CHECK_FALSE(tablet(v, QEvent::TabletRelease, 3, 4, 0.0));
        CHECK(w.document()->undoStack()->count() == 0);
        mouse(v, QEvent::MouseButtonRelease, 3, 4, Qt::LeftButton, stylus());
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    // After any of them, either device starts the next drag.
    CHECK(tablet(v, QEvent::TabletPress, 10, 4, 1.0));
    CHECK(tablet(v, QEvent::TabletRelease, 10, 4, 0.0));
    drag(v, {{12, 4}});
    CHECK(changed(w).count({10, 2}));
    CHECK(changed(w).count({12, 2}));
    CHECK(w.document()->undoStack()->count() == 3);
}

TEST_CASE("a drag's owner lets go when the drag ends, whatever buttons are still held") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    SECTION("the stylus lifted with its barrel button held") {
        CHECK(tabletHeld(v, QEvent::TabletPress, 2, 4, 1.0, Qt::LeftButton, Qt::LeftButton | Qt::RightButton));
        CHECK(tabletHeld(v, QEvent::TabletRelease, 2, 4, 0.0, Qt::LeftButton, Qt::RightButton));
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 1);
        drag(v, {{10, 4}});
        CHECK(changed(w).count({10, 2}));
    }
    SECTION("the mouse's left button let go with the right held") {
        mouseHeld(v, QEvent::MouseButtonPress, 2, 4, Qt::LeftButton, Qt::LeftButton);
        mouseHeld(v, QEvent::MouseButtonPress, 2, 4, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        mouseHeld(v, QEvent::MouseButtonRelease, 2, 4, Qt::LeftButton, Qt::RightButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(tablet(v, QEvent::TabletPress, 10, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 10, 4, 0.0));
        CHECK(changed(w).count({10, 2}));
    }
    CHECK(changed(w).count({2, 2}));
    CHECK(w.document()->undoStack()->count() == 2);
}

TEST_CASE("a popup that opens during a mouse drag ends it; a stylus stroke goes on") {
    // Through Qt's own delivery: a real QMenu takes the mouse's events, and
    // once it has had one Qt sends the drag's release to the canvas no more.
    MainWindow w;
    // Keep the system-input targets visible, independent of the screen-sized default.
    w.resize(1500, 900);
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    REQUIRE(v->visibleRegion().contains(pixel(v, 40, 4).toPoint()));
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    const auto sys = [&](QEvent::Type type, int x, int py, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, pixel(v, x, py), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                            held);
    };
    SECTION("a popup that takes the release") {
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        bwtest::popupOver(v, true);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
        // the stylus starts the next drag
        CHECK(tablet(v, QEvent::TabletPress, 20, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletMove, 21, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 21, 4, 0.0));
        CHECK(changed(w) == std::set<std::pair<int, int>>{{20, 2}, {21, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
        // and a click paints its own cell, joined to nothing
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{20, 2}, {21, 2}, {30, 2}});
        CHECK(w.document()->undoStack()->count() == 2);
    }
    SECTION("a popup that closes with the button still held") {
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        bwtest::popupOver(v, false);
        CHECK_FALSE(dragTool(w)->dragging());
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 3, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a popup that takes a move and closes before the release") {
        // Codex round 5: the release reaches the window but not the canvas.
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        m.close();
        QApplication::processEvents();
        sys(QEvent::MouseButtonRelease, 3, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a popup a right press outside closes, before either release") {
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 3, 4), Qt::RightButton,
                            Qt::LeftButton | Qt::RightButton);
        QApplication::processEvents();
        REQUIRE_FALSE(m.isVisible());
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 3, 4), Qt::RightButton, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 3, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a popup that takes the release outside it, dismissed by a click") {
        sys(QEvent::MouseButtonPress, 4, 4, Qt::LeftButton);
        sys(QEvent::MouseMove, 6, 4, Qt::LeftButton);
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        sys(QEvent::MouseButtonRelease, 6, 4, Qt::NoButton);  // over the canvas: Qt routes it to the popup
        REQUIRE(m.isVisible());                                // QMenu stays open
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);  // closes the popup; not replayed to the canvas
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(tablet(v, QEvent::TabletPress, 20, 8, 1.0));
        CHECK(tablet(v, QEvent::TabletMove, 21, 8, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 21, 8, 0.0));
        CHECK(w.document()->undoStack()->count() == 1);
        sys(QEvent::MouseButtonPress, 40, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 40, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{20, 4}, {21, 4}, {40, 2}});
    }
    SECTION("a popup that takes the release while another button is held") {
        // The right button still down doesn't make up for the left's release.
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 3, 4), Qt::RightButton,
                            Qt::LeftButton | Qt::RightButton);
        bwtest::popupOver(v, true, Qt::LeftButton, Qt::RightButton);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 3, 4), Qt::RightButton, Qt::NoButton);
        CHECK(changed(w).empty());
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a pen's mouse stroke a popup opens over") {
        // The pen's synthesized mouse events reach the popup as the mouse's
        // do: the drag ends at once, as a mouse drag does.
        const auto pen = [&](QEvent::Type type, int x, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                held, bwtest::systemStylus());
        };
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton);
        pen(QEvent::MouseMove, 3, Qt::LeftButton);
        bwtest::popupOver(v, false);
        CHECK_FALSE(dragTool(w)->dragging());
        pen(QEvent::MouseMove, 4, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
    SECTION("a pen's mouse stroke ignores the mouse's click") {
        // The mouse's click is another device's: the pen's drag goes on, the
        // click paints nothing, and the pen's own release lands it (#45).
        const auto pen = [&](QEvent::Type type, int x, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                held, bwtest::systemStylus());
        };
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton);
        pen(QEvent::MouseMove, 3, Qt::LeftButton);
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 4), Qt::RightButton, Qt::RightButton);
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 10, 4), Qt::RightButton, Qt::NoButton);
        CHECK(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
        pen(QEvent::MouseMove, 4, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(QWidget::mouseGrabber() == nullptr);
    }
    SECTION("a stylus stroke a popup opens and closes over") {
        // Qt sends the pen's events to the canvas, popup or not: the pen's
        // drag goes on and its own release ends it.
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            m.close();
            QApplication::processEvents();
        }
        CHECK(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    // Qt before 6.8 sends a tablet release under a popup to the canvas's
    // window as any other and records it, so the press after it is an
    // ordinary one with a tablet grab: a pen lifted off the canvas still
    // comes here and drags the stroke out to it, and a press of another
    // button is that button's. The cases that need Qt to keep the pen down
    // (#46) arise from Qt 6.8.
    const auto penKeptDownUnderPopup = [] {
        if (QT_VERSION < QT_VERSION_CHECK(6, 8, 0))
            SKIP("Qt before 6.8 records a tablet release under a popup (#46 doesn't arise)");
    };
    SECTION("a stylus press right after a release under a popup") {
        // #46: the release goes through the popup, so Qt keeps the pen down
        // and sends the next press, with no hover between, as a move.
        // A left stroke paints cells 2 and 3; a right one rubs them out.
        const auto penLiftedUnderPopup = [&](Qt::MouseButton button = Qt::LeftButton, double pressure = 1.0) {
            const int strokes = w.document()->undoStack()->count();
            bwtest::systemTablet(v, pixel(v, 2, 4), button, pressure);
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), button, pressure);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            m.close();
            QApplication::processEvents();
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(w.document()->undoStack()->count() == strokes + 1);
            if (button == Qt::LeftButton)
                CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}});
            else
                CHECK(changed(w).empty());
        };
        SECTION("starts a stroke") {
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
        }
        // Qt's held record makes a press of another button a press or a
        // release of the one let go (the lowest bit that changed): the view's
        // own record says which button it is.
        const auto settled = [&](std::set<std::pair<int, int>> cells, int strokes) {
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == cells);
            CHECK(w.document()->undoStack()->count() == strokes);
            // nothing is left pending for a hover to go on with
            bwtest::systemTablet(v, pixel(v, 30, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == cells);
            CHECK(w.document()->undoStack()->count() == strokes);
        };
        SECTION("with the right button after a left stroke") {
            // Codex round 1: the right stroke rubs out the left one's cells
            // and ends on its own release.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            settled({}, 2);
        }
        SECTION("with the left button after a right stroke") {
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            penLiftedUnderPopup(Qt::RightButton);
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            settled({{10, 2}, {11, 2}}, 3);
        }
        SECTION("with the middle button, which no brush takes") {
            // Nor does the mouse event Qt makes of it come as the left
            // button's press.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::MiddleButton, 1.0);
            CHECK_FALSE(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::MiddleButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}}, 1);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}, {40, 2}}, 2);
        }
        SECTION("pressed while another button is held") {
            // The right button's stroke: the tip pressed during it does
            // nothing, the right button's release ends it, and the tip held
            // after that paints nothing.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}});
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w).empty());
            CHECK(w.document()->undoStack()->count() == 2);
            bwtest::systemTablet(v, pixel(v, 13, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 13, 4), Qt::NoButton, 0.0);
            settled({}, 2);
        }
        SECTION("two buttons pressed at once") {
            // The lower one is the stroke's; the other's release leaves it
            // going.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 12, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 12, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}, {10, 2}, {11, 2}, {12, 2}}, 2);
        }
        SECTION("a different button pressed straight after a release") {
            // No hover between the left stroke's release and the right press.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::NoButton, 0.0);
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}});
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}}, 3);
        }
        // That press set no tablet grab, so a lift off the canvas goes to
        // another widget; the stroke lands where the canvas last saw the pen
        // once the pen is back.
        const QPoint offInWindow = w.statusBar()->geometry().center();
        REQUIRE(w.childAt(offInWindow) != v);
        const QPointF off = v->mapFrom(&w, QPointF(offInWindow));
        const auto strokeLiftedOffCanvas = [&] {
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, off, Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, off, Qt::NoButton, 0.0);
        };
        SECTION("lifted off the canvas, its stroke lands when the pen hovers back") {
            penKeptDownUnderPopup();
            strokeLiftedOffCanvas();
            bwtest::systemTablet(v, pixel(v, 30, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
            // and the next press is an ordinary one
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}, {40, 2}});
            CHECK(w.document()->undoStack()->count() == 3);
        }
        SECTION("lifted off the canvas, its stroke lands when the pen presses on it again") {
            penKeptDownUnderPopup();
            // No hover over the canvas between: the press lands the old
            // stroke and starts its own, joined to nothing.
            strokeLiftedOffCanvas();
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}, {40, 2}});
            CHECK(w.document()->undoStack()->count() == 3);
        }
        SECTION("lifted off the canvas, its stroke lands when another button presses on it") {
            penKeptDownUnderPopup();
            strokeLiftedOffCanvas();
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}, {10, 2}}, 3);
        }
        SECTION("a press Qt makes a release of reads its own pressure") {
            // Codex round 2: Qt still holds the right button, so it sends
            // the left press as a release; the press reads its pressure and
            // its release keeps it.
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            // 5 × 0.05 is 1 cell: the right stroke rubs out the left one
            v->setBrushSettings(BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
            penLiftedUnderPopup(Qt::RightButton, 0.05);
            bwtest::systemTablet(v, pixel(v, 20, 20), Qt::LeftButton, 1.0);
            CHECK(cellsOf(dragTool(w)->pending()) == tipAt(20, 10, 5, Tip::Square, w));
            bwtest::systemTablet(v, pixel(v, 20, 20), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == tipAt(20, 10, 5, Tip::Square, w));
            CHECK(w.document()->undoStack()->count() == 3);
        }
        SECTION("a click in keyboard mode with another button moves the cell cursor") {
            penKeptDownUnderPopup();
            // Codex round 2: Qt sends the right press as the left button's;
            // keyboard mode doesn't take it, so it moves the cell cursor as
            // a click does.
            penLiftedUnderPopup();
            w.tools()->setActive(QStringLiteral("keyboard"));
            REQUIRE(v->cellCursor() != Cursor{30, 4});
            bwtest::systemTablet(v, pixel(v, 30, 8), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 30, 8), Qt::NoButton, 0.0);
            CHECK(v->cellCursor() == Cursor{30, 4});
            CHECK(w.document()->undoStack()->count() == 1);
        }
        // Qt's record of the pen is up again for the tests after this one.
        bwtest::systemTablet(v, pixel(v, 50, 4), Qt::NoButton, 0.0);
    }
    SECTION("a right stroke lifted off the canvas ends where the view last saw it, not at the next press") {
        penKeptDownUnderPopup();
        // Codex round 3: Qt shows only the left button changing on the left
        // press, so nothing says the right one went up there.
        use(w, "halfBlock");
        v->setPen(Pen{Color::indexed(Grey), Color::indexed(Red)});
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            m.close();
            QApplication::processEvents();
        }
        REQUIRE(w.document()->undoStack()->count() == 1);
        const QPoint offInWindow = w.statusBar()->geometry().center();
        REQUIRE(w.childAt(offInWindow) != v);
        const QPointF off = v->mapFrom(&w, QPointF(offInWindow));
        bwtest::systemTablet(v, pixel(v, 10, 4), Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 11, 4), Qt::RightButton, 1.0);
        bwtest::systemTablet(v, off, Qt::RightButton, 1.0);
        bwtest::systemTablet(v, off, Qt::NoButton, 0.0);
        bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 2);
        bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}, {40, 2}});
        CHECK(w.document()->undoStack()->count() == 3);
    }
    SECTION("a stroke pressed under a popup after a release the view declined there stays whole") {
        // Codex round 3: the release the view declines comes again through
        // the window, and Qt records it; the press after it, which the view
        // takes through the popup, Qt doesn't, and sends as a press again.
        use(w, "halfBlock");
        v->setPen(Pen{Color::indexed(Grey), Color::indexed(Red)});
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(v, &esc);
        REQUIRE_FALSE(dragTool(w)->dragging());
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::MiddleButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 20, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 21, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(w.document()->undoStack()->count() == 0);
            m.close();
            QApplication::processEvents();
        }
        bwtest::systemTablet(v, pixel(v, 22, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 22, 4), Qt::MiddleButton, 1.0);
        CHECK_FALSE(dragTool(w)->dragging());
        std::set<std::pair<int, int>> row;
        for (int x = 10; x <= 22; ++x)
            row.insert({x, 2});
        CHECK(changed(w) == row);
        CHECK(w.document()->undoStack()->count() == 1);
        bwtest::systemTablet(v, pixel(v, 22, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == row);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a button the tool declines pressed under a popup leaves its drag going") {
        // The select tool declines the right press through the popup; Qt
        // passes it again through the window and the view, which has acted on
        // it, sees it again: a release of a button its drag doesn't use and
        // the same press, both declined again.
        use(w, "select");
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 5, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 8, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            m.close();
            QApplication::processEvents();
        }
        bwtest::systemTablet(v, pixel(v, 9, 6), Qt::LeftButton | Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 9, 6), Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 9, 6), Qt::NoButton, 0.0);
        CHECK(v->selection().bounds() == CellRect::fromCorners(2, 2, 9, 3));
        CHECK(changed(w).empty());
    }
    SECTION("a stylus button pressed under a popup leaves the stroke whole") {
        // Codex round 2: the canvas takes the right press through the popup,
        // so Qt doesn't record it and sends the next event as that press
        // again; the left stroke goes on to its own release.
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            m.close();
            QApplication::processEvents();
        }
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton | Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 5, 4), Qt::LeftButton | Qt::RightButton, 1.0);
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 0);
        bwtest::systemTablet(v, pixel(v, 5, 4), Qt::RightButton, 1.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}, {5, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
        bwtest::systemTablet(v, pixel(v, 5, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}, {5, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a stylus stroke Esc drops doesn't start again while the pen stays down") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(v, &esc);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
}

TEST_CASE("a menu opened from the keyboard ends a mouse drag; a stylus stroke goes on (#47)") {
    const bwtest::KeyboardMenus keyboardMenus;
    // Alt+F takes the focus from the canvas twice, for the popup and then for
    // the menu bar; a click on the menu bar takes it only for the popup.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    QMenu* file = w.menuBar()->actions().at(0)->menu();
    REQUIRE(file);
    REQUIRE(file->title() == QStringLiteral("&File"));
    const auto fileMenuByKeyboard = [&] {
        bwtest::systemKey(v, Qt::Key_F, Qt::AltModifier, QStringLiteral("f"));
        CHECK(file->isVisible());
        CHECK_FALSE(v->hasFocus());
        bwtest::systemKey(file, Qt::Key_Escape);
        CHECK_FALSE(file->isVisible());
        bwtest::systemKey(w.menuBar(), Qt::Key_Escape);
    };

    SECTION("a stylus stroke") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        CHECK(dragTool(w)->dragging());
        fileMenuByKeyboard();
        CHECK(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a stylus stroke the menu bar alone takes the focus from") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        QFocusEvent bar(QEvent::FocusOut, Qt::MenuBarFocusReason);
        QApplication::sendEvent(v, &bar);
        CHECK(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a mouse drag") {
        const auto mouseAt = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), button, held);
        };
        mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        mouseAt(QEvent::MouseMove, 3, Qt::NoButton, Qt::LeftButton);
        CHECK(dragTool(w)->dragging());
        fileMenuByKeyboard();
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(QWidget::mouseGrabber() == nullptr);
        mouseAt(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        mouseAt(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
    SECTION("a pen's mouse stroke") {
        const auto pen = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), button, held, bwtest::systemStylus());
        };
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        CHECK(dragTool(w)->dragging());
        fileMenuByKeyboard();
        CHECK_FALSE(dragTool(w)->dragging());
        pen(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
}

TEST_CASE("a window switch ends a stylus stroke a menu opened from the keyboard kept (#47)") {
    const bwtest::KeyboardMenus keyboardMenus;
    // The menu bar has the focus, not the canvas, so the canvas sees no
    // focus-out when the window goes inactive.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    QMenu* file = w.menuBar()->actions().at(0)->menu();
    const auto switchAway = [&](QWidget& other) {
        other.resize(100, 100);
        REQUIRE(bwtest::showExposed(&other));
        QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(
            other.windowHandle(), Qt::ActiveWindowFocusReason);
        QApplication::processEvents();
        REQUIRE(QApplication::activeWindow() == &other);
    };
    bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
    bwtest::systemKey(v, Qt::Key_F, Qt::AltModifier, QStringLiteral("f"));
    REQUIRE(file->isVisible());
    SECTION("popup still open") {
        QWidget other;
        switchAway(other);
        CHECK_FALSE(dragTool(w)->dragging());
    }
    SECTION("popup closed, menu bar holds the focus") {
        bwtest::systemKey(file, Qt::Key_Escape);
        REQUIRE(QApplication::focusWidget() == w.menuBar());
        {
            QWidget other;
            switchAway(other);
            CHECK_FALSE(dragTool(w)->dragging());
        }
        QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(
            w.windowHandle(), Qt::ActiveWindowFocusReason);
        QApplication::processEvents();
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 10), Qt::LeftButton, Qt::LeftButton);
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 10, 10), Qt::LeftButton, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{10, 5}});
        bwtest::systemTablet(v, pixel(v, 6, 4), Qt::NoButton, 0.0);  // the pen hovers back
        CHECK(changed(w) == std::set<std::pair<int, int>>{{10, 5}});
    }
    SECTION("popup closed, the focus moves on in the window") {
        bwtest::systemKey(file, Qt::Key_Escape);
        REQUIRE(QApplication::focusWidget() == w.menuBar());
        QWidget* other = nullptr;
        for (QWidget* c : w.findChildren<QWidget*>())
            if (c != v && c != w.menuBar() && c->isVisible() && c->isEnabled() && (c->focusPolicy() & Qt::ClickFocus)) {
                other = c;
                break;
            }
        REQUIRE(other);
        other->setFocus(Qt::MouseFocusReason);
        REQUIRE(QApplication::focusWidget() == other);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 6, 4), Qt::NoButton, 0.0);  // the pen hovers back
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
}

TEST_CASE("the undo key during a stylus stroke undoes nothing while the menu bar has the focus (#47)") {
    const bwtest::KeyboardMenus keyboardMenus;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    QMenu* file = w.menuBar()->actions().at(0)->menu();
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 10), Qt::LeftButton, Qt::LeftButton);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 10, 10), Qt::LeftButton, Qt::NoButton);
    REQUIRE(w.document()->undoStack()->index() == 1);
    bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
    bwtest::systemKey(v, Qt::Key_F, Qt::AltModifier, QStringLiteral("f"));
    bwtest::systemKey(file, Qt::Key_Escape);
    REQUIRE(QApplication::focusWidget() == w.menuBar());
    REQUIRE(dragTool(w)->dragging());
    bwtest::systemKey(w.menuBar(), Qt::Key_Z, Qt::ControlModifier);
    CHECK_FALSE(dragTool(w)->dragging());
    CHECK(w.document()->undoStack()->index() == 1);
    CHECK(changed(w) == std::set<std::pair<int, int>>{{10, 5}});
}

TEST_CASE("another device's click mid-drag leaves the owner's drag to finish (#45)") {
    // Through Qt's own delivery. A release from another device ends Qt's
    // record of which widget the owner pressed, after which Qt drops the
    // owner's later moves and its release; the view keeps the other
    // device's events from Qt for the drag, and takes no mouse grab.
    MainWindow w;
    // Keep the system-input targets visible, independent of the screen-sized default.
    w.resize(1500, 900);
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    REQUIRE(v->visibleRegion().contains(pixel(v, 30, 4).toPoint()));
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    // Whether a click of the pen's (or the mouse's) on the status bar
    // reaches it.
    QStatusBar* bar = w.statusBar();
    REQUIRE(bar->isVisible());
    const auto barTakes = [&](bool byPen) {
        return bwtest::clickReaches(bar, QPointF(5, 5), byPen ? bwtest::systemStylus() : nullptr);
    };
    const auto mouseAt = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, pixel(v, x, 4), button, held);
    };
    const auto pen = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, pixel(v, x, 4), button, held, bwtest::systemStylus());
    };
    const std::set<std::pair<int, int>> stroke = {{2, 2}, {3, 2}, {4, 2}};

    SECTION("a pen's click during a mouse drag") {
        mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        mouseAt(QEvent::MouseMove, 3, Qt::NoButton, Qt::LeftButton);
        CHECK(QWidget::mouseGrabber() == nullptr);
        pen(QEvent::MouseButtonPress, 10, Qt::LeftButton, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 10, Qt::LeftButton, Qt::NoButton);
        CHECK(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        mouseAt(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        mouseAt(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == stroke);
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(QWidget::mouseGrabber() == nullptr);
        // the next click is a stroke of its own, joined to nothing
        mouseAt(QEvent::MouseButtonPress, 30, Qt::LeftButton, Qt::LeftButton);
        mouseAt(QEvent::MouseButtonRelease, 30, Qt::LeftButton, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}, {30, 2}});
        CHECK(w.document()->undoStack()->count() == 2);
    }
    SECTION("the mouse's click on another widget during a pen's mouse stroke") {
        // The click is the canvas's to ignore: it doesn't reach the widget
        // under it, which would otherwise keep the pen's moves too.
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        pen(QEvent::MouseMove, 3, Qt::NoButton, Qt::LeftButton);
        CHECK_FALSE(barTakes(false));
        CHECK(dragTool(w)->dragging());
        pen(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == stroke);
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(QWidget::mouseGrabber() == nullptr);
        // with the drag over, a click reaches the widget under it again
        CHECK(barTakes(false));
    }
    SECTION("the other device's clicks reach their widgets again once the drag ends, however it ends") {
        SECTION("Esc, the button still held") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE_FALSE(barTakes(true));
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(v, &esc);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(barTakes(true));
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("the undo key") {
            pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE_FALSE(barTakes(false));
            QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
            QApplication::sendEvent(v, &undo);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(barTakes(false));
            pen(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("a popup") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE_FALSE(barTakes(true));
            bwtest::popupOver(v, true);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(barTakes(true));
        }
        SECTION("a tool switch") {
            pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE_FALSE(barTakes(false));
            use(w, "line");
            CHECK(barTakes(false));
            pen(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("a new canvas") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE_FALSE(barTakes(true));
            w.document()->clear();
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(barTakes(true));
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("Select All, which starts the selection tool") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE_FALSE(barTakes(true));
            act(w, QStringLiteral("selectAll"))->trigger();
            CHECK(barTakes(true));
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("Deselect leaves the brush's drag going") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            act(w, QStringLiteral("deselect"))->trigger();
            CHECK(dragTool(w)->dragging());
            CHECK_FALSE(barTakes(true));
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("the focus going elsewhere") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE_FALSE(barTakes(true));
            v->clearFocus();
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(barTakes(true));
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        CHECK(w.document()->undoStack()->count() <= 1);
    }
    SECTION("a stylus stroke driven by tablet events takes no grab") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        CHECK(dragTool(w)->dragging());
        CHECK(QWidget::mouseGrabber() == nullptr);
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}});
    }
    CHECK(QWidget::mouseGrabber() == nullptr);
}

TEST_CASE("a canvas that isn't shown takes no mouse grab") {
    // QWidget::grabMouse() wants a visible widget. The drag works all the
    // same.
    MainWindow w;
    CanvasView* v = w.canvasView();
    REQUIRE_FALSE(v->isVisible());
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    mouse(v, QEvent::MouseButtonPress, 2, 4);
    CHECK(dragTool(w)->dragging());
    CHECK(QWidget::mouseGrabber() == nullptr);
    mouse(v, QEvent::MouseMove, 3, 4);
    mouse(v, QEvent::MouseButtonRelease, 3, 4);
    CHECK_FALSE(dragTool(w)->dragging());
    CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}});
    CHECK(QWidget::mouseGrabber() == nullptr);
}

namespace {

// Every warning and error Qt or the app logs while one is alive.
class Warnings {
public:
    Warnings() : previous_(qInstallMessageHandler(&Warnings::handle)) { sink() = &messages_; }
    ~Warnings() {
        sink() = nullptr;
        qInstallMessageHandler(previous_);
    }
    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;
    const QStringList& messages() const { return messages_; }
    void clear() { messages_.clear(); }

private:
    static QStringList*& sink() {
        static QStringList* s = nullptr;
        return s;
    }
    static void handle(QtMsgType type, const QMessageLogContext&, const QString& msg) {
        if (type != QtDebugMsg && type != QtInfoMsg && sink())
            sink()->append(msg);
    }
    QtMessageHandler previous_;
    QStringList messages_;
};

} // namespace

TEST_CASE("a mouse stroke takes no mouse grab and logs nothing (#101)") {
    // A mouse grab is the window system's too: Wayland grants one only to a
    // popup and warns at every grab and release, and offscreen warns that it
    // can't. Qt's own record of the press brings the stroke's moves and
    // release to the canvas wherever they are, out of the view and out of
    // the window.
    Warnings warnings;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    // the mouse's events, then the pen's mouse events
    const QPointingDevice* device = GENERATE(false, true) ? bwtest::systemStylus() : nullptr;
    const auto at = [&](QWidget* target, QEvent::Type type, QPointF p, Qt::MouseButtons held) {
        bwtest::systemMouse(target, type, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                            device);
    };
    warnings.clear();

    SECTION("on the canvas") {
        at(v, QEvent::MouseButtonPress, pixel(v, 2, 4), Qt::LeftButton);
        at(v, QEvent::MouseMove, pixel(v, 3, 4), Qt::LeftButton);
        CHECK(dragTool(w)->dragging());
        CHECK(QWidget::mouseGrabber() == nullptr);
        at(v, QEvent::MouseButtonRelease, pixel(v, 3, 4), Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}});
    }
    SECTION("let go over another widget") {
        QStatusBar* bar = w.statusBar();
        REQUIRE(bar->isVisible());
        at(v, QEvent::MouseButtonPress, pixel(v, 2, 4), Qt::LeftButton);
        at(v, QEvent::MouseMove, pixel(v, 3, 4), Qt::LeftButton);
        at(bar, QEvent::MouseMove, QPointF(5, 5), Qt::LeftButton);
        CHECK(dragTool(w)->dragging());
        at(bar, QEvent::MouseButtonRelease, QPointF(5, 5), Qt::NoButton);
        CHECK(changed(w).count({2, 2}) == 1);
    }
    SECTION("let go out of the window") {
        const QPointF out = v->mapFrom(&w, QPointF(-40, -40));
        at(v, QEvent::MouseButtonPress, pixel(v, 2, 4), Qt::LeftButton);
        at(v, QEvent::MouseMove, pixel(v, 3, 4), Qt::LeftButton);
        at(v, QEvent::MouseMove, out, Qt::LeftButton);
        CHECK(dragTool(w)->dragging());
        at(v, QEvent::MouseButtonRelease, out, Qt::NoButton);
        CHECK(changed(w).count({2, 2}) == 1);
    }
    CHECK_FALSE(dragTool(w)->dragging());
    CHECK(w.document()->undoStack()->count() == 1);
    CHECK(QWidget::mouseGrabber() == nullptr);
    INFO(warnings.messages().join(QLatin1Char('\n')).toStdString());
    CHECK(warnings.messages().isEmpty());
}

TEST_CASE("mirror mode with Pressure->Strength: a cell both strokes reach takes the stronger dab") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    act(w, QStringLiteral("mirrorMode"))->trigger();
    REQUIRE(w.document()->canvas().width() == 80);
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength});
    const Ramp steel = ramp("steel");
    // columns 39 and 40 mirror each other: the stroke covers 39 lightly and
    // 40 fully, its mirror 40 lightly and 39 fully
    tablet(v, QEvent::TabletPress, 39, 0, 0.25);
    tablet(v, QEvent::TabletMove, 40, 0, 1.0);
    tablet(v, QEvent::TabletRelease, 40, 0, 0.0);
    CHECK(rungOf(steel, at(w, 39, 0)) == 4);
    CHECK(rungOf(steel, at(w, 40, 0)) == 4);
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("mirror mode: random stylus strokes leave a mirror-symmetric canvas") {
    for (const char* id : {"rampShade", "shade"}) {
        MainWindow w;
        CanvasView* v = w.canvasView();
        use(w, id);
        act(w, QStringLiteral("mirrorMode"))->trigger();
        REQUIRE(v->mirror());
        std::mt19937 rng(20261022);
        const auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
        const auto pressure = [&] { return bwtest::uniformReal(rng, 0.02, 1.0); };
        for (int stroke = 0; stroke < 60; ++stroke) {
            v->setBrushSettings(BrushSettings{pick(1, 5), pick(0, 1) ? Tip::Round : Tip::Square, pick(10, 100) / 100.0,
                                              pick(0, 1) ? PressureTo::Strength : PressureTo::Size});
            const Qt::MouseButton button = pick(0, 3) ? Qt::LeftButton : Qt::RightButton;
            int x = pick(30, 49), py = pick(0, 19);
            tablet(v, QEvent::TabletPress, x, py, pressure(), button);
            for (int n = pick(0, 4); n > 0; --n) {
                x = std::clamp(x + pick(-6, 6), 0, 79);
                py = std::clamp(py + pick(-4, 4), 0, 39);
                tablet(v, QEvent::TabletMove, x, py, pressure(), button);
            }
            tablet(v, QEvent::TabletRelease, x, py, 0.0, button);
            INFO(id << " stroke " << stroke);
            REQUIRE(asymmetric(w).empty());
        }
    }
}

TEST_CASE("a brush keeps the pen it was pressed with for the whole stroke") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    for (const char* id : {"shade", "glyph", "colorize"}) {
        INFO(id);
        use(w, id);
        v->setForeground(LightGreen);
        mouse(v, QEvent::MouseButtonPress, 0, 0);
        mouse(v, QEvent::MouseMove, 2, 0);
        v->setForeground(Yellow);
        // a brush change redoes the stroke, in the pen it started with
        BrushSettings s = v->brushSettings();
        s.tip = s.tip == Tip::Square ? Tip::Round : Tip::Square;
        v->setBrushSettings(s);
        act(w, QStringLiteral("mirrorMode"))->trigger();
        act(w, QStringLiteral("mirrorMode"))->trigger();
        mouse(v, QEvent::MouseMove, 3, 0);
        mouse(v, QEvent::MouseButtonRelease, 3, 0);
        for (int x = 0; x <= 3; ++x)
            CHECK(at(w, x, 0).fg.paletteIndex() == LightGreen);
        // the next stroke takes the new pen
        drag(v, {{0, 2}});
        CHECK(at(w, 0, 1).fg.paletteIndex() == Yellow);
    }
}
