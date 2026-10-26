// The ramp gradient tool in the main window (PLAN §10, issue #92): in the
// Tools menu and the toolbox with its key, its settings in Tool Options and
// kept between runs, and linear and radial drags through the window system,
// previewed as they go and landing as one undo step inside the selection and
// the alpha lock, ladder-lint clean.

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "tool.h"
#include "tool_options.h"
#include "toolbox.h"

#include "scratch_settings.h"
#include "system_input.h"

#include "blockwright/ansi.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/guard.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QUndoStack>

#include <algorithm>
#include <utility>
#include <vector>

using namespace bw;

namespace {

constexpr int kW = 40, kH = 10;

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

GradientTool* useGradient(MainWindow& w) {
    act(w, QStringLiteral("gradientTool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QStringLiteral("gradient"));
    auto* t = qobject_cast<GradientTool*>(w.tools()->active());
    REQUIRE(t);
    return t;
}

// The gradient's settings, forgotten before and after a test, from the
// scratch settings only.
struct FreshGradientSettings {
    static QString dir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }
    static void clear() {
        REQUIRE(bwtest::settingsInScratch(dir()));
        appSettings()->remove(QStringLiteral("gradient"));
    }
    FreshGradientSettings() { clear(); }
    ~FreshGradientSettings() {
        if (bwtest::settingsInScratch(dir()))
            appSettings()->remove(QStringLiteral("gradient"));
    }
};

// A blank kW × kH document in a shown window, so a drag reaches every
// column.
void blank(MainWindow& w) {
    if (!w.isVisible())
        REQUIRE(bwtest::showExposed(&w));
    AnsiDocument doc;
    doc.canvas = Canvas(kW, kH);
    w.document()->replace(std::move(doc), QString());
    REQUIRE(w.document()->canvas().width() == kW);
}

QPointF centre(CanvasView* v, QPoint cell) { return QRectF(v->cellRect(cell)).center(); }

// A press at `from` and moves a cell at a time along the row, then down the
// column, to `to`, through the window system; released there unless `hold`.
void drag(CanvasView* v, QPoint from, QPoint to, Qt::MouseButton button = Qt::LeftButton, bool hold = false) {
    bwtest::systemMouse(v, QEvent::MouseButtonPress, centre(v, from), button, button);
    QPoint at = from;
    while (at != to) {
        if (at.x() != to.x())
            at.rx() += to.x() > at.x() ? 1 : -1;
        else
            at.ry() += to.y() > at.y() ? 1 : -1;
        bwtest::systemMouse(v, QEvent::MouseMove, centre(v, at), Qt::NoButton, button);
    }
    if (!hold)
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, centre(v, to), button, Qt::NoButton);
}

void release(CanvasView* v, QPoint at, Qt::MouseButton button = Qt::LeftButton) {
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, centre(v, at), button, Qt::NoButton);
}

// The rungs the core gives every cell of the canvas for this drag.
std::vector<int> expected(const Ramp& ramp, GradientShape shape, QPoint from, QPoint to, int fromRung, int toRung) {
    Gradient g;
    g.shape = shape;
    g.x0 = from.x(), g.y0 = from.y(), g.x1 = to.x(), g.y1 = to.y();
    g.fromRung = fromRung, g.toRung = toRung;
    return gradientRungs(ramp, g, kW, kH);
}

int rungAt(MainWindow& w, int x, int y) {
    const auto r = rungOf(w.canvasView()->ramp(), w.document()->canvas().at(x, y));
    REQUIRE(r);
    return *r;
}

Canvas applied(Canvas c, const CellEdit& e) {
    e.apply(c);
    return c;
}

bool sameCells(const Canvas& a, const Canvas& b) {
    if (a.width() != b.width() || a.height() != b.height())
        return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (!(a.at(x, y) == b.at(x, y)))
                return false;
    return true;
}

} // namespace

TEST_CASE("the ramp gradient is in the Tools menu and with fill in the toolbox, on Ctrl+Shift+G", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    QAction* a = act(w, QStringLiteral("gradientTool"));
    CHECK(a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    CHECK(a->toolTip().contains(a->shortcut().toString(QKeySequence::NativeText)));
    // in the fill tool's group
    QToolButton* b = w.toolbox()->buttonFor(QStringLiteral("gradient"));
    REQUIRE(b);
    CHECK(b == w.toolbox()->buttonFor(QStringLiteral("fill")));
    REQUIRE(b->menu());
    CHECK(b->menu()->actions().contains(a));
    useGradient(w);
    CHECK(a->isChecked());
    // the group's button takes it
    CHECK(b->defaultAction() == a);
}

TEST_CASE("a linear gradient drag fills the canvas along the ramp as the core does, one undo step", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    const Ramp ramp = v->ramp();
    REQUIRE(ramp.name == "steel");
    const int top = ramp.topRung();
    CHECK(t->shape() == GradientShape::Linear);
    CHECK(t->fromRung() == 0);
    CHECK(t->toRung() >= top);  // the whole ramp

    const QPoint from(2, 4), to(37, 4);
    drag(v, from, to);
    const std::vector<int> want = expected(ramp, GradientShape::Linear, from, to, 0, top);
    REQUIRE(want.size() == size_t(kW * kH));
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            INFO(x << "," << y);
            CHECK(rungAt(w, x, y) == want[size_t(y * kW + x)]);
        }
    // dark at the press, light at the release, never back down along the
    // drag, the same down every column
    CHECK(rungAt(w, from.x(), 0) == 0);
    CHECK(rungAt(w, to.x(), 9) == top);
    for (int x = 1; x < kW; ++x)
        CHECK(rungAt(w, x - 1, 0) <= rungAt(w, x, 0));
    for (int y = 1; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            CHECK(rungAt(w, x, y) == rungAt(w, x, 0));
    // every rung of the ladder is there
    for (int r = 0; r <= top; ++r) {
        INFO("rung " << r);
        bool found = false;
        for (int x = 0; x < kW; ++x)
            found = found || rungAt(w, x, 0) == r;
        CHECK(found);
    }

    CHECK(w.document()->undoStack()->count() == 1);
    w.document()->undoStack()->undo();
    CHECK(sameCells(w.document()->canvas(), Canvas(kW, kH)));
}

TEST_CASE("a radial gradient drag rings out from the press", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);
    dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(GradientShape::Radial)));
    REQUIRE(t->shape() == GradientShape::Radial);
    const Ramp ramp = v->ramp();
    const int top = ramp.topRung();

    const QPoint from(20, 5), to(34, 5);
    drag(v, from, to);
    const std::vector<int> want = expected(ramp, GradientShape::Radial, from, to, 0, top);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            INFO(x << "," << y);
            CHECK(rungAt(w, x, y) == want[size_t(y * kW + x)]);
        }
    // darkest at the centre, lighter outward, the same either side
    CHECK(rungAt(w, from.x(), from.y()) == 0);
    for (int d = 1; d < 19; ++d) {
        INFO("d " << d);
        CHECK(rungAt(w, from.x() - d, from.y()) == rungAt(w, from.x() + d, from.y()));
        CHECK(rungAt(w, from.x() + d - 1, from.y()) <= rungAt(w, from.x() + d, from.y()));
    }
    CHECK(rungAt(w, 0, 0) > rungAt(w, from.x() + 2, from.y()));
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("the gradient's right button runs the rungs the other way", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    t->setRungs(2, 9);
    const QPoint from(2, 4), to(37, 4);
    drag(v, from, to, Qt::RightButton);
    const std::vector<int> want = expected(v->ramp(), GradientShape::Linear, from, to, 9, 2);
    for (int x = 0; x < kW; ++x)
        CHECK(rungAt(w, x, 0) == want[size_t(x)]);
    CHECK(rungAt(w, from.x(), 0) == 9);
    CHECK(rungAt(w, to.x(), 0) == 2);
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("the gradient's preview is the edit that lands, and nothing lands before release", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    const Canvas before = w.document()->canvas();

    const QPoint from(5, 1), to(30, 8);
    drag(v, from, to, Qt::LeftButton, /*hold=*/true);
    REQUIRE(t->dragging());
    const CellEdit shown = t->pending();
    REQUIRE_FALSE(shown.empty());
    // the preview isn't an edit
    CHECK(sameCells(w.document()->canvas(), before));
    CHECK(w.document()->undoStack()->count() == 0);
    // the document previews the same cells
    const CellEdit previewed = w.document()->previewEdit(shown);
    CHECK(previewed.changes.size() == shown.changes.size());

    release(v, to);
    CHECK_FALSE(t->dragging());
    CHECK(t->pending().empty());
    CHECK(sameCells(w.document()->canvas(), applied(before, shown)));
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("Esc drops a gradient drag; a change of shape or ramp mid-drag redoes it", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);

    const QPoint from(20, 5), to(30, 5);
    drag(v, from, to, Qt::LeftButton, /*hold=*/true);
    const CellEdit linear = t->pending();
    dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(GradientShape::Radial)));
    REQUIRE(t->dragging());
    const CellEdit radial = t->pending();
    CHECK(sameCells(applied(Canvas(kW, kH), radial),
                    [&] {
                        Canvas c(kW, kH);
                        const auto want = expected(v->ramp(), GradientShape::Radial, from, to, 0, t->toRung());
                        for (int y = 0; y < kH; ++y)
                            for (int x = 0; x < kW; ++x)
                                c.at(x, y) = rungCell(v->ramp(), want[size_t(y * kW + x)]);
                        return c;
                    }()));
    CHECK_FALSE(sameCells(applied(Canvas(kW, kH), linear), applied(Canvas(kW, kH), radial)));
    // a new ramp redoes it on that ramp
    v->setRamp(*findRamp("fire"));
    REQUIRE(t->dragging());
    for (const CellChange& c : t->pending().changes)
        CHECK(rungOf(*findRamp("fire"), c.after));

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    CHECK(t->keyPress(&esc));
    CHECK_FALSE(t->dragging());
    release(v, to);
    CHECK(sameCells(w.document()->canvas(), Canvas(kW, kH)));
    CHECK(w.document()->undoStack()->count() == 0);
}

TEST_CASE("the gradient paints only inside the selection", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    CanvasView* v = w.canvasView();
    useGradient(w);
    const CellRect box{8, 2, 20, 5};
    v->setSelection(Selection(box));
    drag(v, QPoint(8, 4), QPoint(27, 4));
    const Canvas& c = w.document()->canvas();
    int painted = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const bool in = box.contains(x, y);
            INFO(x << "," << y);
            if (!in)
                CHECK(c.at(x, y) == Cell{});
            else
                painted += !(c.at(x, y) == Cell{});
        }
    CHECK(painted > box.width * box.height / 2);
    // the climb runs across the box
    CHECK(rungAt(w, box.x, 3) < rungAt(w, box.x + box.width - 1, 3));
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("the gradient leaves a locked layer's transparent cells alone", "[gradient][layers]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    REQUIRE(doc->active()->cells.at(0, 0).transparent);
    GradientTool* t = useGradient(w);
    // paint a box of the new layer opaque, unlocked
    const CellRect box{10, 3, 12, 4};
    v->setSelection(Selection(box));
    drag(v, QPoint(10, 4), QPoint(21, 4));
    v->setSelection(Selection());
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            REQUIRE(doc->active()->cells.at(x, y).transparent == !box.contains(x, y));
    const Canvas opaque = doc->active()->cells;

    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    t->setRungs(1, 7);
    drag(v, QPoint(0, 4), QPoint(39, 4), Qt::RightButton);
    REQUIRE(doc->undoStack()->count() >= 2);
    int changed = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            INFO(x << "," << y);
            const Cell& c = doc->active()->cells.at(x, y);
            CHECK(c.transparent == !box.contains(x, y));
            changed += box.contains(x, y) && !(c == opaque.at(x, y));
        }
    CHECK(changed > 0);
}

TEST_CASE("the gradient refuses a Pixels layer and says why in the status bar", "[gradient][layers]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    REQUIRE_FALSE(label->text().contains(QStringLiteral("Pixels layer")));
    const LayerId cells = doc->activeLayer();
    // made active with the gradient already chosen: the status follows
    // (CanvasView::brushChanged on a change of active layer)
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    REQUIRE(doc->active()->props.kind == LayerKind::Pixels);
    CHECK(label->isVisibleTo(&w));
    CHECK(label->text().contains(QStringLiteral("Pixels layer")));
    CHECK(t->status() == label->text());
    const Canvas layer = doc->active()->cells, shown = doc->canvas();
    const int undo = doc->undoStack()->count();

    // the whole ramp, shades and all, and a fill of whole colours only
    // (every rung a full block, which a pixel layer could hold)
    for (const auto& [from, to] : {std::pair{0, 99}, std::pair{kRungsPerStep, kRungsPerStep}}) {
        INFO("rungs " << from << " to " << to);
        t->setRungs(from, to);
        drag(v, QPoint(2, 4), QPoint(37, 4), Qt::LeftButton, /*hold=*/true);
        CHECK_FALSE(t->dragging());
        CHECK(t->pending().empty());
        release(v, QPoint(37, 4));
        CHECK(sameCells(doc->active()->cells, layer));
        CHECK(sameCells(doc->canvas(), shown));
        CHECK(doc->undoStack()->count() == undo);
        CHECK(label->text().contains(QStringLiteral("Pixels layer")));
    }

    // back on the cells layer the tool paints, and the status says so
    REQUIRE(doc->setActiveLayer(cells));
    CHECK_FALSE(label->text().contains(QStringLiteral("Pixels layer")));
    drag(v, QPoint(2, 4), QPoint(37, 4));
    CHECK(doc->undoStack()->count() == undo + 1);
}

TEST_CASE("the gradient paints a Text layer as it does a cells layer", "[gradient][layers]") {
    FreshGradientSettings fresh;
    MainWindow w;
    blank(w);
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    doc->addLayer(LayerKind::Text);
    REQUIRE(doc->active()->props.kind == LayerKind::Text);
    CHECK_FALSE(t->status().contains(QStringLiteral("Pixels layer")));
    const Canvas before = doc->active()->cells;
    const int undo = doc->undoStack()->count();

    const QPoint from(2, 4), to(37, 4);
    drag(v, from, to);
    Gradient g;
    g.x0 = from.x(), g.y0 = from.y(), g.x1 = to.x(), g.y1 = to.y();
    g.fromRung = t->fromRung(), g.toRung = t->toRung();
    const CellEdit want = rampGradient(before, v->ramp(), g, layerStrokeOptions(*doc->active()));
    REQUIRE_FALSE(want.empty());
    CHECK(sameCells(doc->active()->cells, applied(before, want)));
    CHECK(doc->undoStack()->count() == undo + 1);
}

TEST_CASE("a gradient drag is ladder-lint clean, and the tool says what a slanted fill adds", "[gradient][lint]") {
    FreshGradientSettings fresh;
    MainWindow w;
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);
    for (const char* name : {"steel", "fire", "sunset"}) {
        v->setRamp(*findRamp(name));
        const Ramp ramp = v->ramp();
        REQUIRE(ramp.name == name);
        for (const GradientShape shape : {GradientShape::Linear, GradientShape::Radial}) {
            dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(shape)));
            for (const auto& [from, to] : {std::pair{QPoint(2, 3), QPoint(36, 3)}, std::pair{QPoint(19, 5), QPoint(19, 9)},
                                           std::pair{QPoint(4, 1), QPoint(33, 8)}}) {
                blank(w);
                INFO(name << (shape == GradientShape::Radial ? " radial " : " linear ") << from.x() << ","
                          << from.y() << " to " << to.x() << "," << to.y());
                drag(v, from, to, Qt::LeftButton, /*hold=*/true);
                CHECK(t->added().empty());
                CHECK_FALSE(t->status().contains(QStringLiteral("lint")));
                release(v, to);
                const lint::LadderReport r = lint::checkLadder(w.document()->canvas(), ramp);
                for (const auto& f : r.findings)
                    UNSCOPED_INFO(f.message);
                CHECK(r.clean());
            }
        }
    }

    // In a selection on a field of one rung, a slanted fill can join a band
    // to the climb in an L (rampGradient's documented limit): the tool
    // reports it while dragging, as the lint finds it on release.
    dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(GradientShape::Linear)));
    v->setRamp(*findRamp("steel"));
    const Ramp steel = v->ramp();
    bool reported = false;
    for (int dy = 1; dy < 6 && !reported; ++dy)
        for (int x1 = 20; x1 < 36 && !reported; ++x1) {
            AnsiDocument field;
            field.canvas = Canvas(kW, kH);
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    field.canvas.at(x, y) = rungCell(steel, kRungsPerStep);
            w.document()->replace(std::move(field), QString());
            v->setSelection(Selection(CellRect{3, 1, 34, 8}));
            const Canvas start = w.document()->canvas();
            const QPoint from(4, 2), to(x1, 2 + dy);
            drag(v, from, to, Qt::LeftButton, /*hold=*/true);
            const std::vector<lint::LadderFinding> added = t->added();
            const QString status = t->status();
            release(v, to);
            if (added.empty())
                continue;
            reported = true;
            INFO("to " << x1 << "," << 2 + dy);
            CHECK(status.contains(QStringLiteral("adds %1 lint finding").arg(added.size())));
            std::vector<std::pair<int, int>> changed;
            const Canvas& after = w.document()->canvas();
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    if (!(after.at(x, y) == start.at(x, y)))
                        changed.push_back({x, y});
            CHECK(StrokeLint(start, steel).judge(after, changed) == added);
            for (const auto& f : added)
                CHECK(f.kind == lint::LadderFinding::Kind::WideRung);
        }
    CHECK(reported);
    // and forgets it once the drag is over
    CHECK(t->added().empty());
}

TEST_CASE("mirror mode adds nothing to a gradient, and Tool Options doesn't offer it", "[gradient]") {
    FreshGradientSettings fresh;
    const auto run = [](bool mirror, SymmetryMode mode) {
        MainWindow w;
        blank(w);
        CanvasView* v = w.canvasView();
        v->setSymmetry(Symmetry{mode});
        v->setMirror(mirror);
        useGradient(w);
        v->setSelection(Selection(CellRect{0, 0, 15, kH}));
        drag(v, QPoint(1, 2), QPoint(13, 7));
        return w.document()->canvas();
    };
    const Canvas plain = run(false, SymmetryMode::MirrorX);
    CHECK_FALSE(sameCells(plain, Canvas(kW, kH)));
    for (const SymmetryMode mode : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY,
                                    SymmetryMode::Radial}) {
        INFO(int(mode));
        CHECK(sameCells(run(true, mode), plain));
    }
    MainWindow w;
    useGradient(w);
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);
    CHECK_FALSE(dock->shown().testFlag(ToolOptions::Option::Symmetry));
}

TEST_CASE("Tool Options shows the gradient's shape, rungs on the ramp and banding", "[gradient]") {
    FreshGradientSettings fresh;
    MainWindow w;
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);
    const ToolOptions::Options shown = dock->shown();
    CHECK(shown == (ToolOptions::Option::AlphaLock | ToolOptions::Option::Ramp | ToolOptions::Option::Gradient));
    for (QWidget* c : {static_cast<QWidget*>(dock->gradientShapeBox()), static_cast<QWidget*>(dock->gradientFromBox()),
                       static_cast<QWidget*>(dock->gradientToBox()), static_cast<QWidget*>(dock->gradientBandingBox()),
                       static_cast<QWidget*>(dock->rampBox()), static_cast<QWidget*>(dock->alphaLockBox())}) {
        INFO(c->objectName().toStdString());
        CHECK_FALSE(c->isHidden());
        CHECK_FALSE(c->toolTip().isEmpty());
    }
    CHECK(dock->sizeBox()->isHidden());
    CHECK(dock->symmetryBox()->isHidden());

    // the rungs run 0 to the ramp's top; a rung past a short ramp's top
    // shows as the top and comes back on a longer ramp
    v->setRamp(*findRamp("sunset"));
    CHECK(dock->gradientToBox()->maximum() == findRamp("sunset")->topRung());
    t->setRungs(1, 20);
    CHECK(dock->gradientToBox()->value() == 20);
    v->setRamp(*findRamp("grass"));
    CHECK(dock->gradientToBox()->maximum() == findRamp("grass")->topRung());
    CHECK(dock->gradientToBox()->value() == findRamp("grass")->topRung());
    CHECK(t->toRung() == 20);
    CHECK(t->status() == QStringLiteral("Ramp grass, linear, rung 1 to 12"));
    v->setRamp(*findRamp("sunset"));
    CHECK(dock->gradientToBox()->value() == 20);

    // the dock sets the tool
    dock->gradientFromBox()->setValue(3);
    dock->gradientToBox()->setValue(11);
    CHECK(t->fromRung() == 3);
    CHECK(t->toRung() == 11);
    dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(GradientShape::Radial)));
    CHECK(t->shape() == GradientShape::Radial);
    // and follows it
    t->setShape(GradientShape::Linear);
    t->setRungs(6, 2);
    CHECK(dock->gradientShapeBox()->currentData().toInt() == int(GradientShape::Linear));
    CHECK(dock->gradientFromBox()->value() == 6);
    CHECK(dock->gradientToBox()->value() == 2);

    // cell-banded only: the Bayer dither waits on the speckle rule
    QComboBox* banding = dock->gradientBandingBox();
    REQUIRE(banding->count() == 2);
    CHECK(banding->currentIndex() == 0);
    auto* model = qobject_cast<QStandardItemModel*>(banding->model());
    REQUIRE(model);
    CHECK(model->item(0)->isEnabled());
    CHECK_FALSE(model->item(1)->isEnabled());
}

TEST_CASE("the gradient's shape and rungs are kept between runs", "[gradient][settings]") {
    FreshGradientSettings fresh;
    {
        MainWindow w;
        useGradient(w);
        auto* dock = w.findChild<ToolOptions*>();
        REQUIRE(dock);
        dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(GradientShape::Radial)));
        dock->gradientFromBox()->setValue(2);
        dock->gradientToBox()->setValue(9);
    }
    REQUIRE(bwtest::settingsInScratch(FreshGradientSettings::dir()));
    {
        const auto s = appSettings();
        CHECK(s->value(QStringLiteral("gradient/shape")).toString() == QStringLiteral("radial"));
        CHECK(s->value(QStringLiteral("gradient/from")).toInt() == 2);
        CHECK(s->value(QStringLiteral("gradient/to")).toInt() == 9);
    }
    MainWindow w;
    GradientTool* t = useGradient(w);
    CHECK(t->shape() == GradientShape::Radial);
    CHECK(t->fromRung() == 2);
    CHECK(t->toRung() == 9);
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);
    CHECK(dock->gradientShapeBox()->currentData().toInt() == int(GradientShape::Radial));
    CHECK(dock->gradientFromBox()->value() == 2);
    CHECK(dock->gradientToBox()->value() == 9);

    // nonsense in the file reads as the defaults' kind of value
    appSettings()->setValue(QStringLiteral("gradient/shape"), QStringLiteral("spiral"));
    appSettings()->setValue(QStringLiteral("gradient/from"), -4);
    MainWindow again;
    auto* t2 = qobject_cast<GradientTool*>(again.tools()->find(QStringLiteral("gradient")));
    REQUIRE(t2);
    CHECK(t2->shape() == GradientShape::Linear);
    CHECK(t2->fromRung() == 0);
}

TEST_CASE("a change of shape or of one rung in Tool Options keeps the other rungs a short ramp clamps", "[gradient][settings]") {
    FreshGradientSettings fresh;
    MainWindow w;
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);
    const Ramp sunset = *findRamp("sunset"), grass = *findRamp("grass");
    REQUIRE(sunset.topRung() == 20);
    REQUIRE(grass.topRung() == 12);
    const auto kept = [](const char* key) { return appSettings()->value(QLatin1String("gradient/") + key); };

    // sunset To 20, then grass shows it as 12; a shape change there writes
    // the shape alone
    v->setRamp(sunset);
    dock->gradientFromBox()->setValue(18);
    dock->gradientToBox()->setValue(20);
    REQUIRE(t->fromRung() == 18);
    REQUIRE(t->toRung() == 20);
    v->setRamp(grass);
    CHECK(dock->gradientFromBox()->value() == 12);
    CHECK(dock->gradientToBox()->value() == 12);
    dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(GradientShape::Radial)));
    CHECK(t->shape() == GradientShape::Radial);
    CHECK(t->fromRung() == 18);
    CHECK(t->toRung() == 20);
    v->setRamp(sunset);
    CHECK(dock->gradientFromBox()->value() == 18);
    CHECK(dock->gradientToBox()->value() == 20);
    CHECK(kept("shape").toString() == QStringLiteral("radial"));
    CHECK(kept("from").toInt() == 18);
    CHECK(kept("to").toInt() == 20);

    // an edit of one rung on grass writes that rung alone
    v->setRamp(grass);
    dock->gradientToBox()->setValue(5);
    CHECK(t->fromRung() == 18);
    CHECK(t->toRung() == 5);
    dock->gradientFromBox()->setValue(2);
    CHECK(t->fromRung() == 2);
    CHECK(t->toRung() == 5);
    dock->gradientToBox()->setValue(12);
    v->setRamp(sunset);
    dock->gradientFromBox()->setValue(19);
    v->setRamp(grass);
    dock->gradientShapeBox()->setCurrentIndex(dock->gradientShapeBox()->findData(int(GradientShape::Linear)));
    v->setRamp(sunset);
    CHECK(dock->gradientFromBox()->value() == 19);
    CHECK(dock->gradientToBox()->value() == 12);
    CHECK(kept("shape").toString() == QStringLiteral("linear"));
    CHECK(kept("from").toInt() == 19);
    CHECK(kept("to").toInt() == 12);
}

namespace {

// The lint's reads for a gradient drag on a blank 400 × h canvas, a press in
// a 20 × 10 selection and `moves` moves of the end back and forth between
// two cells, the tool's added() read each time as the status line reads it:
// the press and first move (the first preview), then each move after.
struct DragReads {
    long long first = 0;
    std::vector<long long> moves;
};

DragReads lintReads(int h, int moves) {
    MainWindow w;
    if (!w.isVisible())
        REQUIRE(bwtest::showExposed(&w));
    AnsiDocument doc;
    doc.canvas = Canvas(400, h);
    w.document()->replace(std::move(doc), QString());
    CanvasView* v = w.canvasView();
    GradientTool* t = useGradient(w);
    v->setSelection(Selection(CellRect{10, 5, 20, 10}));
    const QPoint from(12, 7), ends[] = {QPoint(26, 12), QPoint(27, 13)};
    DragReads reads;
    const long long start = lint::cellsRead();
    bwtest::systemMouse(v, QEvent::MouseButtonPress, centre(v, from), Qt::LeftButton, Qt::LeftButton);
    bwtest::systemMouse(v, QEvent::MouseMove, centre(v, ends[1]), Qt::NoButton, Qt::LeftButton);
    reads.first = lint::cellsRead() - start;
    REQUIRE(t->dragging());
    REQUIRE_FALSE(t->pending().empty());
    for (int i = 0; i < moves; ++i) {
        const long long before = lint::cellsRead();
        bwtest::systemMouse(v, QEvent::MouseMove, centre(v, ends[i & 1]), Qt::NoButton, Qt::LeftButton);
        (void)t->added();
        reads.moves.push_back(lint::cellsRead() - before);
        REQUIRE(t->pending().changes.size() <= 200);
    }
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, centre(v, ends[(moves - 1) & 1]), Qt::LeftButton,
                        Qt::NoButton);
    CHECK(w.document()->undoStack()->count() == 1);
    return reads;
}

} // namespace

TEST_CASE("a gradient drag reads the whole canvas for the lint once, at its first preview", "[gradient][lint]") {
    FreshGradientSettings fresh;
    // After the first preview a move judges the lint a change at a time
    // (StrokeLint), which costs the rows and columns through the 20 × 10
    // fill and the two either side of them (LadderIndex), read once to put
    // the last preview back and once for the next: 24 columns down and 14
    // rows across, some ten reads a cell. A whole-canvas pass a move is
    // 2.5 million cells.
    const DragReads shorter = lintReads(1000, 20), tall = lintReads(2000, 20);
    const long long most = *std::max_element(tall.moves.begin(), tall.moves.end());
    const long long mostShorter = *std::max_element(shorter.moves.begin(), shorter.moves.end());
    INFO("first preview " << tall.first << ", most a move " << most << " on 400x2000; first preview "
                          << shorter.first << ", most a move " << mostShorter << " on 400x1000");
    CHECK(most > 0);
    CHECK(most * 20 < tall.first);
    const auto lines = [](int h) { return 24LL * h + 14LL * 400; };
    CHECK(most < 16 * lines(2000));
    CHECK(mostShorter < 16 * lines(1000));
    // a thousand rows more add to a move only the fill's columns' length
    CHECK(most - mostShorter < 16 * (lines(2000) - lines(1000)));
    CHECK(most - mostShorter < 1000LL * 400);
}
