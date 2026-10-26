// The detail tools in the main window (PLAN §10, "Detail"): the outline,
// rim-light and texture brushes, the clone stamp, the glyph spray, the pencil's 8-connected mode and the
// symmetry modes,
// from the Tools and Edit menus and Tool Options, with the mouse delivered
// as the platform delivers it.

#include "tool_options.h"
#include "toolbox.h"
#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "select_tool.h"
#include "tool.h"
#include "toolbox.h"
#include "settings.h"

#include "scratch_settings.h"
#include "system_input.h"

#include "blockwright/brush.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/halfblock.h"
#include "blockwright/lint.h"
#include "blockwright/palette.h"
#include "blockwright/ramp.h"
#include "blockwright/shapes.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QToolButton>
#include <QSpinBox>
#include <QUndoStack>

#include <map>
#include <set>
#include <tuple>
#include <utility>

using namespace bw;

namespace {

using Point = std::pair<int, int>;

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

ToolOptions* dock(MainWindow& w) {
    auto* d = w.findChild<ToolOptions*>();
    REQUIRE(d);
    return d;
}

const Cell& at(MainWindow& w, int x, int y) { return w.document()->canvas().at(x, y); }

DragTool* dragTool(MainWindow& w) {
    auto* t = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(t);
    return t;
}

// The view pixel in the middle of half-block pixel (x, py).
QPointF pixel(CanvasView* v, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    return QPointF(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
}

// A left-button drag through half-block pixels, through the window system.
// Returns the edit the preview showed just before the release.
CellEdit drag(MainWindow& w, std::initializer_list<QPoint> points) {
    CanvasView* v = w.canvasView();
    const QPoint* p = points.begin();
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, p->x(), p->y()), Qt::LeftButton, Qt::LeftButton);
    for (++p; p != points.end(); ++p)
        bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, p->x(), p->y()), Qt::NoButton, Qt::LeftButton);
    const CellEdit shown = dragTool(w)->pending();
    const QPoint last = *(points.end() - 1);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, last.x(), last.y()), Qt::LeftButton, Qt::NoButton);
    return shown;
}

// The cells an edit wrote, and what it wrote there.
std::map<Point, Cell> written(const CellEdit& e) {
    std::map<Point, Cell> out;
    for (const CellChange& c : e.changes)
        out[{c.x, c.y}] = c.after;
    return out;
}

// Checks the canvas holds what the preview showed, as one undo step.
void landed(MainWindow& w, const CellEdit& shown, int stepsBefore) {
    REQUIRE_FALSE(shown.empty());
    for (const CellChange& c : shown.changes)
        CHECK(at(w, c.x, c.y) == c.after);
    CHECK(w.document()->undoStack()->index() == stepsBefore + 1);
}

Ramp steel() { return *findRamp("steel"); }  // k K w W

// A steel shape at rows 1-4, columns 2-6, darker on the left: rungs 8 to 12
// (w to W), as one undo step.
void paintBlob(MainWindow& w) {
    CellEdit e;
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 6; ++x)
            e.changes.push_back({x, y, at(w, x, y), rungCell(steel(), 8 + (x - 2))});
    REQUIRE(w.canvasView()->applyEdit(e));
}

std::set<Point> cells(const CellEdit& e) {
    std::set<Point> out;
    for (const CellChange& c : e.changes)
        out.insert({c.x, c.y});
    return out;
}

// A click on cell (x, y) with Ctrl held, through the window system.
void ctrlClick(CanvasView* v, int x, int y) {
    QWidget* top = v->window();
    const QPointF local = v->mapTo(top, QPointF(v->cellRect(QPoint(x, y)).center()));
    for (const auto& [type, held] : {std::pair{QEvent::MouseButtonPress, Qt::MouseButtons(Qt::LeftButton)},
                                     std::pair{QEvent::MouseButtonRelease, Qt::MouseButtons(Qt::NoButton)}})
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            top->windowHandle(), local, top->mapToGlobal(local), held, Qt::LeftButton, type, Qt::ControlModifier);
}

CloneTool* cloneTool(MainWindow& w) {
    auto* t = qobject_cast<CloneTool*>(w.tools()->find(QStringLiteral("clone")));
    REQUIRE(t);
    return t;
}

// The clone stamp's settings, forgotten before and after a test, from the
// scratch settings only.
struct FreshCloneSettings {
    static QString dir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }
    FreshCloneSettings() {
        REQUIRE(bwtest::settingsInScratch(dir()));
        appSettings()->remove(QStringLiteral("clone"));
    }
    ~FreshCloneSettings() {
        if (bwtest::settingsInScratch(dir()))
            appSettings()->remove(QStringLiteral("clone"));
    }
};

// The texture stamp's settings, forgotten before and after a test, from the
// scratch settings only.
struct FreshTextureSettings {
    static QString dir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }
    FreshTextureSettings() {
        REQUIRE(bwtest::settingsInScratch(dir()));
        appSettings()->remove(QStringLiteral("texture"));
    }
    ~FreshTextureSettings() {
        if (bwtest::settingsInScratch(dir()))
            appSettings()->remove(QStringLiteral("texture"));
    }
};

// Letters A, B, C at (1..3, 1) and a, b, c under them, as one undo step.
void paintLetters(MainWindow& w) {
    CellEdit e;
    for (int x = 1; x <= 3; ++x) {
        Cell top;
        top.ch = char32_t(U'A' + x - 1);
        top.fg = Color::indexed(Yellow);
        top.bg = Color::indexed(Blue);
        Cell under = top;
        under.ch = char32_t(U'a' + x - 1);
        e.changes.push_back({x, 1, at(w, x, 1), top});
        e.changes.push_back({x, 2, at(w, x, 2), under});
    }
    REQUIRE(w.canvasView()->applyEdit(e));
}

} // namespace

TEST_CASE("the clone stamp is in the Tools menu and the detail group, on Ctrl+Shift+K") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    QAction* a = act(w, QStringLiteral("cloneTool"));
    CHECK(a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
    CHECK(QString(cloneTool(w)->name()).remove(QLatin1Char('&')) == QStringLiteral("Clone Stamp"));
    bwtest::systemKey(w.canvasView(), Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(w.tools()->active()->id() == QStringLiteral("clone"));
    CHECK(a->isChecked());
    bool inDetail = false;
    for (const QStringList& g : Toolbox::groups())
        if (g.contains(QStringLiteral("outline")))
            inDetail = g.contains(QStringLiteral("clone"));
    CHECK(inDetail);
    // it reads the brush settings and Aligned, not a ramp or a strength
    w.tools()->setActive(cloneTool(w));
    const auto shown = dock(w)->shown();
    CHECK(shown.testFlag(ToolOptions::Option::Size));
    CHECK(shown.testFlag(ToolOptions::Option::Tip));
    CHECK(shown.testFlag(ToolOptions::Option::Symmetry));
    CHECK(shown.testFlag(ToolOptions::Option::AlphaLock));
    CHECK(shown.testFlag(ToolOptions::Option::CloneAligned));
    CHECK(dock(w)->cloneAlignedBox()->isVisibleTo(dock(w)));
}

TEST_CASE("the clone stamp: Ctrl+click sets the source and paints nothing; a drag copies from it") {
    FreshCloneSettings fresh;
    MainWindow w;
    // Keep the system-input targets visible, independent of the screen-sized default.
    w.resize(1400, 900);
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    REQUIRE(v->visibleRegion().contains(pixel(v, 41, 12).toPoint()));
    QUndoStack* undo = w.document()->undoStack();
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    paintLetters(w);
    use(w, "clone");
    CloneTool* t = cloneTool(w);
    CHECK(t->aligned());
    CHECK(t->status().contains(QStringLiteral("Ctrl+click")));
    const Canvas before = w.document()->canvas();
    const int steps = undo->index();

    // no source: a drag paints nothing
    drag(w, {{10, 8}, {12, 8}});
    CHECK(undo->index() == steps);
    CHECK(w.document()->canvas() == before);
    CHECK_FALSE(t->dragging());

    // Ctrl+click: the source, no paint, no drag, the cell cursor stays
    const Cursor cursor = v->cellCursor();
    ctrlClick(v, 1, 1);
    CHECK(t->source().source == std::pair{1, 1});
    CHECK_FALSE(t->dragging());
    CHECK(undo->index() == steps);
    CHECK(w.document()->canvas() == before);
    CHECK(v->cellCursor().x == cursor.x);
    CHECK(v->cellCursor().y == cursor.y);
    CHECK(t->mark() == QPoint(1, 1));
    CHECK(t->status().contains(QStringLiteral("aligned")));

    // a drag copies the cells at the distance to its first cell, one step
    const CellEdit shown = drag(w, {{10, 8}, {12, 8}});
    landed(w, shown, steps);
    CHECK(at(w, 10, 4) == before.at(1, 1));
    CHECK(at(w, 11, 4) == before.at(2, 1));
    CHECK(at(w, 12, 4) == before.at(3, 1));

    // aligned: the next stroke keeps that distance; the mark is the cell it
    // copies from
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 10), Qt::LeftButton, Qt::LeftButton);
    CHECK(t->mark() == QPoint(1, 2));
    bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, 11, 10), Qt::NoButton, Qt::LeftButton);
    CHECK(t->mark() == QPoint(2, 2));
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 11, 10), Qt::LeftButton, Qt::NoButton);
    CHECK(at(w, 10, 5) == before.at(1, 2));
    CHECK(at(w, 11, 5) == before.at(2, 2));
    CHECK(undo->index() == steps + 2);
    CHECK(t->mark() == QPoint(1, 1));

    // undo takes a stroke back whole
    undo->undo();
    CHECK(at(w, 10, 5) == before.at(10, 5));
    CHECK(at(w, 11, 5) == before.at(11, 5));
    CHECK(at(w, 10, 4) == before.at(1, 1));

    // a new source starts over; a stroke dropped with Esc sets no distance
    ctrlClick(v, 2, 1);
    CHECK_FALSE(t->source().anchor);
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 20, 12), Qt::LeftButton, Qt::LeftButton);
    CHECK(t->dragging());
    CHECK_FALSE(t->pending().empty());
    bwtest::systemKey(v, Qt::Key_Escape);
    CHECK_FALSE(t->dragging());
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 20, 12), Qt::LeftButton, Qt::NoButton);
    CHECK(at(w, 20, 6) == before.at(20, 6));
    CHECK_FALSE(t->source().anchor);
    drag(w, {{30, 12}});
    CHECK(at(w, 30, 6) == before.at(2, 1));
    CHECK(t->source().anchor == std::pair{30, 6});

    // not aligned: each stroke copies from the source again
    dock(w)->cloneAlignedBox()->setChecked(false);
    CHECK_FALSE(t->aligned());
    drag(w, {{40, 12}, {41, 12}});
    CHECK(at(w, 40, 6) == before.at(2, 1));
    CHECK(at(w, 41, 6) == before.at(3, 1));
}

TEST_CASE("the clone stamp's Aligned is kept between runs", "[settings]") {
    FreshCloneSettings fresh;
    {
        MainWindow w;
        CHECK(cloneTool(w)->aligned());
        CHECK(dock(w)->cloneAlignedBox()->isChecked());
        dock(w)->cloneAlignedBox()->setChecked(false);
        CHECK_FALSE(cloneTool(w)->aligned());
    }
    REQUIRE(bwtest::settingsInScratch(FreshCloneSettings::dir()));
    CHECK(appSettings()->value(QStringLiteral("clone/aligned")).toBool() == false);
    {
        MainWindow w;
        CHECK_FALSE(cloneTool(w)->aligned());
        CHECK_FALSE(dock(w)->cloneAlignedBox()->isChecked());
        // and the dock follows the tool
        cloneTool(w)->setAligned(true);
        CHECK(dock(w)->cloneAlignedBox()->isChecked());
    }
    CHECK(appSettings()->value(QStringLiteral("clone/aligned")).toBool());
}

TEST_CASE("the clone stamp's mark follows an unaligned drag") {
    FreshCloneSettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    paintLetters(w);
    use(w, "clone");
    CloneTool* t = cloneTool(w);
    t->setAligned(false);
    ctrlClick(v, 1, 1);
    const Canvas before = w.document()->canvas();
    // the mark is the cell the brush copies to the dab under the pointer
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 8), Qt::LeftButton, Qt::LeftButton);
    CHECK(t->mark() == QPoint(1, 1));
    bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, 11, 8), Qt::NoButton, Qt::LeftButton);
    CHECK(t->mark() == QPoint(2, 1));
    CHECK(dragTool(w)->pending().changes.size() == 2);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 11, 8), Qt::LeftButton, Qt::NoButton);
    CHECK(at(w, 11, 4) == before.at(2, 1));
    CHECK(t->mark() == QPoint(1, 1));
    // a second stroke starts at the source again, and its mark with it
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 20, 12), Qt::LeftButton, Qt::LeftButton);
    CHECK(t->mark() == QPoint(1, 1));
    bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, 22, 14), Qt::NoButton, Qt::LeftButton);
    CHECK(t->mark() == QPoint(3, 2));
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 22, 14), Qt::LeftButton, Qt::NoButton);
    CHECK(at(w, 22, 7) == before.at(3, 2));
}

TEST_CASE("the clone stamp forgets its source and distance when the document is replaced") {
    FreshCloneSettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    CloneTool* t = cloneTool(w);
    REQUIRE(t->aligned());

    // ABCDEFGH on one row, as an .ans opens
    const auto letters = [] {
        AnsiDocument a;
        a.canvas = Canvas(8, 1);
        for (int x = 0; x < 8; ++x) {
            a.canvas.at(x, 0).ch = char32_t(U'A' + x);
            a.canvas.at(x, 0).fg = Color::indexed(Yellow);
        }
        return a;
    };
    // A source at (1, 0) and an aligned stroke from (4, 0) on the open
    // document: a source and a distance.
    const auto setUp = [&] {
        doc->replace(letters(), QString());
        use(w, "clone");
        ctrlClick(v, 1, 0);
        drag(w, {{4, 0}});
        REQUIRE(t->source().source == std::pair{1, 0});
        REQUIRE(t->source().anchor == std::pair{4, 0});
        REQUIRE(at(w, 4, 0).ch == U'B');
    };
    // After a replacement: no source, no distance, no mark, and a press paints
    // nothing until a new Ctrl+click.
    const auto forgotten = [&] {
        CHECK_FALSE(t->source().source);
        CHECK_FALSE(t->source().anchor);
        CHECK_FALSE(t->mark());
        CHECK(t->aligned());
        use(w, "clone");
        CHECK(t->status().contains(QStringLiteral("Ctrl+click")));
        const Canvas before = doc->canvas();
        const int steps = doc->undoStack()->index();
        drag(w, {{4, 0}, {5, 0}});
        CHECK(doc->canvas() == before);
        CHECK(doc->undoStack()->index() == steps);
        // a new source works as ever
        ctrlClick(v, 0, 0);
        drag(w, {{4, 0}});
        CHECK(at(w, 4, 0) == before.at(0, 0));
    };

    SECTION("open an .ans with the clone stamp active") {
        setUp();
        doc->replace(letters(), QString());
        forgotten();
    }
    SECTION("New with the clone stamp active") {
        setUp();
        doc->clear();
        forgotten();
    }
    SECTION("open a project with another tool active") {
        setUp();
        use(w, "halfBlock");
        project::Project p;
        p.layers = LayerStack::fromCanvas(letters().canvas);
        doc->replace(std::move(p), QString(), FileForm::ProjectDirectory);
        forgotten();
    }
    SECTION("open a narrower canvas with another tool active: no source left off it") {
        setUp();
        ctrlClick(v, 7, 0);
        REQUIRE(t->source().source == std::pair{7, 0});
        use(w, "keyboard");
        AnsiDocument narrow;
        narrow.canvas = Canvas(4, 1);
        doc->replace(std::move(narrow), QString());
        CHECK_FALSE(t->source().source);
        CHECK_FALSE(t->mark());
    }
}

TEST_CASE("the outline brush outlines the ramp shape one cell wide, in the selection only") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    v->setBrushSettings(BrushSettings{12, Tip::Square, 1.0, PressureTo::Nothing});
    paintBlob(w);
    const int steps = w.document()->undoStack()->index();
    use(w, "outline");
    CHECK(act(w, QStringLiteral("outlineTool"))->isChecked());
    const CellEdit shown = drag(w, {{4, 4}, {5, 5}});
    landed(w, shown, steps);
    std::set<Point> edge;
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 6; ++x)
            if (x == 2 || x == 6 || y == 1 || y == 4)
                edge.insert({x, y});
    CHECK(cells(shown) == edge);
    for (const CellChange& c : shown.changes)
        CHECK(c.after == rungCell(steel(), 4));  // K, the colour below w

    // undone, then again with a selection: only its cells change
    w.document()->undoStack()->undo();
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({2, 1, 1, 4});
    use(w, "outline");
    const CellEdit clipped = drag(w, {{4, 4}});
    // the selection's edge cells, the left side, as far as the lint allows
    // (a corner cell it allows alone is a lone fragment, left off)
    REQUIRE_FALSE(clipped.empty());
    for (const Point& p : cells(clipped))
        CHECK((p.first == 2 && p.second >= 1 && p.second <= 4));
    CHECK(lint::checkLadder(w.document()->canvas(), steel()).clean());
}


TEST_CASE("the outline brush lines the seam of the selected shape from Tool Options") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    v->setBrushSettings(BrushSettings{12, Tip::Square, 1.0, PressureTo::Nothing});
    // two White shapes touching at rows 1-4: columns 2-6 and 7-11
    CellEdit e;
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 11; ++x)
            e.changes.push_back({x, y, at(w, x, y), rungCell(steel(), 12)});
    REQUIRE(v->applyEdit(e));
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({2, 1, 5, 4});
    QCheckBox* box = dock(w)->outlineSelectionBox();
    CHECK_FALSE(box->isChecked());
    CHECK(v->outlineShape() == OutlineShape::Ramp);
    // the selection only clips: no seam
    use(w, "outline");
    const CellEdit clipped = drag(w, {{4, 4}});
    REQUIRE_FALSE(clipped.empty());
    CHECK_FALSE(cells(clipped).count({6, 2}));
    CHECK_FALSE(cells(clipped).count({6, 3}));
    w.document()->undoStack()->undo();
    // the selection as the shape: its side of the seam takes the line
    box->setChecked(true);
    CHECK(v->outlineShape() == OutlineShape::Selection);
    const int steps = w.document()->undoStack()->index();
    const CellEdit seam = drag(w, {{4, 4}});
    landed(w, seam, steps);
    const auto line = written(seam);
    for (int y = 1; y <= 4; ++y) {
        INFO("row " << y);
        REQUIRE(line.count({6, y}));
        CHECK(line.at({6, y}) == rungCell(steel(), 8));
    }
    for (const Point& p : cells(seam))
        CHECK(p.first <= 6);
    CHECK(lint::checkLadder(w.document()->canvas(), steel()).clean());
    // and the dock follows the view
    v->setOutlineShape(OutlineShape::Ramp);
    CHECK_FALSE(box->isChecked());
}

TEST_CASE("the rim-light brush lights the edges facing the dock's light by one rung") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    v->setBrushSettings(BrushSettings{12, Tip::Square, 1.0, PressureTo::Nothing});
    paintBlob(w);
    QComboBox* light = dock(w)->lightBox();
    REQUIRE(light->count() == 8);
    CHECK(light->currentData().toInt() == int(Compass::NW));
    light->setCurrentIndex(light->findData(int(Compass::N)));
    CHECK(v->brushSettings().lightDirection == Compass::N);
    // the other settings stay as they were
    CHECK(v->brushSettings().size == 12);
    const int steps = w.document()->undoStack()->index();
    use(w, "rimLight");
    const CellEdit shown = drag(w, {{4, 4}});
    landed(w, shown, steps);
    std::map<Point, int> lit;
    for (const auto& [p, cell] : written(shown)) {
        REQUIRE(rungOf(steel(), cell));
        lit[p] = *rungOf(steel(), cell) - (8 + (p.first - 2));
    }
    // the top row but its last cell, which is W already
    CHECK(lit == std::map<Point, int>{{{2, 1}, 1}, {{3, 1}, 1}, {{4, 1}, 1}, {{5, 1}, 1}});

    // from the west: the left column, and the cells beside its ends, whose
    // normals on screen turn about 55 degrees from the west (as the core's)
    light->setCurrentIndex(light->findData(int(Compass::W)));
    w.document()->undoStack()->undo();
    const CellEdit west = drag(w, {{4, 4}});
    CHECK(cells(west) == std::set<Point>{{2, 1}, {2, 2}, {2, 3}, {2, 4}, {3, 1}, {3, 4}});
}

namespace {

RimLightTool* rimTool(MainWindow& w) {
    auto* t = qobject_cast<RimLightTool*>(w.tools()->find(QStringLiteral("rimLight")));
    REQUIRE(t);
    return t;
}

// The rim light's settings, forgotten before and after a test, from the
// scratch settings only.
struct FreshRimSettings {
    static QString dir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }
    FreshRimSettings() {
        REQUIRE(bwtest::settingsInScratch(dir()));
        appSettings()->remove(QStringLiteral("rimLight"));
    }
    ~FreshRimSettings() {
        if (bwtest::settingsInScratch(dir()))
            appSettings()->remove(QStringLiteral("rimLight"));
    }
};

} // namespace

TEST_CASE("the rim light's Keep the lint clean, from the dock, leaves out the cells that would skip a rung", "[settings]") {
    FreshRimSettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    v->setBrushSettings(BrushSettings{12, Tip::Square, 1.0, PressureTo::Nothing});
    paintBlob(w);
    QCheckBox* keep = dock(w)->rimLintBox();
    // only the rim light shows it
    use(w, "form");
    CHECK_FALSE(keep->isVisibleTo(dock(w)));
    use(w, "rimLight");
    CHECK(dock(w)->shown().testFlag(ToolOptions::Option::RimLint));
    CHECK(keep->isVisibleTo(dock(w)));
    CHECK_FALSE(keep->isChecked());
    CHECK_FALSE(rimTool(w)->keepLint());
    CHECK_FALSE(rimTool(w)->status().contains(QStringLiteral("lint")));
    // from the east the accent lights the rounding at the top and bottom
    // right, rung 11 to 12 beside rung 10: two rung skips (as the core's)
    QComboBox* light = dock(w)->lightBox();
    light->setCurrentIndex(light->findData(int(Compass::E)));
    const int steps = w.document()->undoStack()->index();
    const CellEdit accent = drag(w, {{4, 4}});
    landed(w, accent, steps);
    CHECK(cells(accent) == std::set<Point>{{5, 1}, {5, 4}});
    w.document()->undoStack()->undo();
    // kept clean, neither lights
    keep->setChecked(true);
    CHECK(rimTool(w)->keepLint());
    CHECK(rimTool(w)->status().contains(QStringLiteral("keeping the lint clean")));
    CHECK(drag(w, {{4, 4}}).empty());
    CHECK(w.document()->undoStack()->index() == steps);
    CHECK(lint::checkLadder(w.document()->canvas(), steel()).clean());
    // from the north the top row lights as the accent lights it: it adds
    // nothing
    light->setCurrentIndex(light->findData(int(Compass::N)));
    const CellEdit north = drag(w, {{4, 4}});
    landed(w, north, steps);
    CHECK(cells(north) == std::set<Point>{{2, 1}, {3, 1}, {4, 1}, {5, 1}});
    CHECK(lint::checkLadder(w.document()->canvas(), steel()).clean());
    // and the dock follows the tool
    rimTool(w)->setKeepLint(false);
    CHECK_FALSE(keep->isChecked());
}

TEST_CASE("the rim light's Keep the lint clean, changed mid-stroke, redoes the stroke with it", "[settings]") {
    FreshRimSettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    v->setBrushSettings(BrushSettings{12, Tip::Square, 1.0, PressureTo::Nothing});
    paintBlob(w);
    use(w, "rimLight");
    QComboBox* light = dock(w)->lightBox();
    light->setCurrentIndex(light->findData(int(Compass::E)));
    RimLightTool* t = rimTool(w);
    const int steps = w.document()->undoStack()->index();
    // pressed as the accent, the two rung skips show; turned on before the
    // release, the stroke is kept clean and lands nothing
    REQUIRE_FALSE(t->keepLint());
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 4, 4), Qt::LeftButton, Qt::LeftButton);
    CHECK(cells(t->pending()) == std::set<Point>{{5, 1}, {5, 4}});
    dock(w)->rimLintBox()->setChecked(true);
    CHECK(t->keepLint());
    CHECK(t->pending().empty());
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 4, 4), Qt::LeftButton, Qt::NoButton);
    CHECK(w.document()->undoStack()->index() == steps);
    CHECK(lint::checkLadder(w.document()->canvas(), steel()).clean());
    // and back: pressed kept clean, turned off before the release, the
    // accent's cells land
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 4, 4), Qt::LeftButton, Qt::LeftButton);
    CHECK(t->pending().empty());
    t->setKeepLint(false);
    CHECK_FALSE(dock(w)->rimLintBox()->isChecked());
    const CellEdit accent = t->pending();
    CHECK(cells(accent) == std::set<Point>{{5, 1}, {5, 4}});
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 4, 4), Qt::LeftButton, Qt::NoButton);
    landed(w, accent, steps);
}

TEST_CASE("the rim light's Keep the lint clean is kept between runs", "[settings]") {
    FreshRimSettings fresh;
    {
        MainWindow w;
        CHECK_FALSE(rimTool(w)->keepLint());
        CHECK_FALSE(dock(w)->rimLintBox()->isChecked());
        dock(w)->rimLintBox()->setChecked(true);
        CHECK(rimTool(w)->keepLint());
    }
    REQUIRE(bwtest::settingsInScratch(FreshRimSettings::dir()));
    CHECK(appSettings()->value(QStringLiteral("rimLight/keepLint")).toBool());
    {
        MainWindow w;
        CHECK(rimTool(w)->keepLint());
        CHECK(dock(w)->rimLintBox()->isChecked());
        rimTool(w)->setKeepLint(false);
        CHECK_FALSE(dock(w)->rimLintBox()->isChecked());
    }
    CHECK(appSettings()->value(QStringLiteral("rimLight/keepLint")).toBool() == false);
}

TEST_CASE("the pencil is 4-connected by default and 8-connected from the dock") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    QCheckBox* four = dock(w)->pencil4Box();
    CHECK(four->isChecked());
    CHECK(v->pencilConnectivity() == Connectivity::Four);
    use(w, "halfBlock");
    v->setForeground(Red);
    const auto lit = [&] {
        std::set<Point> px;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 6; ++x) {
                const HalfPair h = halfBlocks(at(w, x, y)).value_or(HalfPair{});
                if (h.top == Red)
                    px.insert({x, 2 * y});
                if (h.bottom == Red)
                    px.insert({x, 2 * y + 1});
            }
        return px;
    };
    // one move from one corner to the other: the line joins them
    drag(w, {{0, 0}, {3, 3}});
    const std::set<Point> stepped = lit();
    CHECK(stepped.size() == 7);
    CHECK(stepped.count({0, 0}));
    CHECK(stepped.count({3, 3}));

    w.document()->undoStack()->undo();
    four->setChecked(false);
    CHECK(v->pencilConnectivity() == Connectivity::Eight);
    const int steps = w.document()->undoStack()->index();
    const CellEdit shown = drag(w, {{0, 0}, {3, 3}});
    landed(w, shown, steps);
    CHECK(lit() == std::set<Point>{{0, 0}, {1, 1}, {2, 2}, {3, 3}});
}

TEST_CASE("each symmetry mode from the Edit menu repeats a brush stroke as its exact image") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    const int W = w.document()->canvas().width(), H = w.document()->canvas().height();
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    use(w, "glyph");
    auto* glyph = qobject_cast<GlyphTool*>(w.tools()->active());
    // an L of cells, (3,2), (4,2), (4,3), in `ch`
    const auto stroke = [&](char32_t ch) {
        glyph->setGlyph(ch);
        const int steps = w.document()->undoStack()->index();
        const CellEdit shown = drag(w, {{3, 4}, {4, 4}, {4, 6}});
        landed(w, shown, steps);
        std::map<Point, char32_t> out;
        for (const auto& [p, cell] : written(shown))
            out[p] = cell.ch;
        w.document()->undoStack()->undo();
        return out;
    };
    // the L in `ch` at its own cells and `images` of it
    const auto L = [](char32_t ch, std::map<Point, char32_t> images) {
        images.insert({{{3, 2}, ch}, {{4, 2}, ch}, {{4, 3}, ch}});
        return images;
    };
    const auto mirrorX = [&](char32_t ch) {
        return std::map<Point, char32_t>{{{W - 4, 2}, ch}, {{W - 5, 2}, ch}, {{W - 5, 3}, ch}};
    };
    const auto mirrorY = [&](char32_t ch) {
        return std::map<Point, char32_t>{{{3, H - 3}, ch}, {{4, H - 3}, ch}, {{4, H - 4}, ch}};
    };

    SECTION("mirror X") {
        act(w, QStringLiteral("symmetryMirrorX"))->trigger();
        CHECK(act(w, QStringLiteral("mirrorMode"))->isChecked());
        CHECK(stroke(U'▌') == L(U'▌', mirrorX(U'▐')));
    }
    SECTION("mirror Y") {
        act(w, QStringLiteral("symmetryMirrorY"))->trigger();
        CHECK(act(w, QStringLiteral("mirrorMode"))->isChecked());
        CHECK(act(w, QStringLiteral("symmetryMirrorY"))->isChecked());
        CHECK(dock(w)->symmetryBox()->currentData().toInt() == int(SymmetryMode::MirrorY));
        CHECK(stroke(U'▀') == L(U'▀', mirrorY(U'▄')));
    }
    SECTION("mirror X and Y") {
        act(w, QStringLiteral("symmetryMirrorXY"))->trigger();
        std::map<Point, char32_t> all = L(U'▌', mirrorX(U'▐'));
        all.merge(mirrorY(U'▌'));
        all.merge(std::map<Point, char32_t>{{{W - 4, H - 3}, U'▐'}, {{W - 5, H - 3}, U'▐'}, {{W - 5, H - 4}, U'▐'}});
        CHECK(stroke(U'▌') == all);
    }
    SECTION("radial, two folds about a set cell") {
        ToolOptions* d = dock(w);
        d->centreXBox()->setValue(10);
        d->centreYBox()->setValue(10);
        d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
        CHECK(act(w, QStringLiteral("symmetryRadial"))->isChecked());
        CHECK(d->foldsBox()->isEnabled());
        d->foldsBox()->setValue(2);
        d->centreBox()->setChecked(true);
        REQUIRE(v->symmetry() == Symmetry{SymmetryMode::Radial, 2, Point{10, 10}});
        // the half turn is exact, its glyph flipped both ways
        CHECK(stroke(U'▀') == L(U'▀', {{{17, 18}, U'▄'}, {{16, 18}, U'▄'}, {{16, 17}, U'▄'}}));
    }
    SECTION("radial, four folds: quarter turns on screen, glyphs turned with them") {
        ToolOptions* d = dock(w);
        d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
        d->foldsBox()->setValue(4);
        d->centreXBox()->setValue(6);
        d->centreYBox()->setValue(3);
        d->centreBox()->setChecked(true);
        REQUIRE(v->symmetry() == Symmetry{SymmetryMode::Radial, 4, Point{6, 3}});
        glyph->setGlyph(U'▀');
        const int steps = w.document()->undoStack()->index();
        const CellEdit shown = drag(w, {{8, 6}});
        landed(w, shown, steps);
        std::map<Point, char32_t> out;
        for (const auto& [p, cell] : written(shown))
            out[p] = cell.ch;
        // two cells across is one cell down: cells are twice as tall as
        // wide; a quarter turn turns the glyph as Rotate does (▀ to ▐)
        CHECK(out == std::map<Point, char32_t>{{{8, 3}, U'▀'}, {{6, 4}, U'▐'}, {{4, 3}, U'▄'}, {{6, 2}, U'▌'}});
    }
    SECTION("off from the dock: the stroke alone, and mirror mode off") {
        act(w, QStringLiteral("symmetryMirrorY"))->trigger();
        dock(w)->symmetryBox()->setCurrentIndex(0);
        CHECK_FALSE(v->mirror());
        CHECK_FALSE(act(w, QStringLiteral("mirrorMode"))->isChecked());
        CHECK(stroke(U'▀') == L(U'▀', {}));
        // mirror mode back on keeps the chosen mode
        act(w, QStringLiteral("mirrorMode"))->trigger();
        CHECK(v->symmetry().mode == SymmetryMode::MirrorY);
        CHECK(dock(w)->symmetryBox()->currentData().toInt() == int(SymmetryMode::MirrorY));
        CHECK(stroke(U'▀') == L(U'▀', mirrorY(U'▄')));
    }
}

TEST_CASE("the half-block pencil takes every symmetry mode") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    const int W = w.document()->canvas().width(), H = w.document()->canvas().height();
    use(w, "halfBlock");
    v->setForeground(Red);
    const auto pixels = [&](SymmetryMode mode) {
        act(w, mode == SymmetryMode::MirrorX    ? QStringLiteral("symmetryMirrorX")
               : mode == SymmetryMode::MirrorY  ? QStringLiteral("symmetryMirrorY")
               : mode == SymmetryMode::MirrorXY ? QStringLiteral("symmetryMirrorXY")
                                                : QStringLiteral("symmetryRadial"))
            ->trigger();
        const int steps = w.document()->undoStack()->index();
        const CellEdit shown = drag(w, {{1, 0}, {2, 0}});
        landed(w, shown, steps);
        std::set<Point> px;
        for (const auto& [p, cell] : written(shown)) {
            const HalfPair h = halfBlocks(cell).value_or(HalfPair{});
            if (h.top == Red)
                px.insert({p.first, 2 * p.second});
            if (h.bottom == Red)
                px.insert({p.first, 2 * p.second + 1});
        }
        w.document()->undoStack()->undo();
        return px;
    };
    const int R = 2 * H - 1;  // the last pixel row
    CHECK(pixels(SymmetryMode::MirrorX) == std::set<Point>{{1, 0}, {2, 0}, {W - 2, 0}, {W - 3, 0}});
    CHECK(pixels(SymmetryMode::MirrorY) == std::set<Point>{{1, 0}, {2, 0}, {1, R}, {2, R}});
    CHECK(pixels(SymmetryMode::MirrorXY) == std::set<Point>{{1, 0}, {2, 0}, {W - 2, 0}, {W - 3, 0}, {1, R}, {2, R},
                                                            {W - 2, R}, {W - 3, R}});
    // radial, the canvas centre, two folds: the half turn
    dock(w)->foldsBox()->setValue(2);
    CHECK(pixels(SymmetryMode::Radial) == std::set<Point>{{1, 0}, {2, 0}, {W - 2, R}, {W - 3, R}});
}

TEST_CASE("a shape dragged past the canvas edge keeps the radial images that land on it") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    AnsiDocument doc;
    doc.canvas = Canvas(80, 25);
    w.document()->replace(std::move(doc), QString());
    CanvasView* v = w.canvasView();
    const Canvas blank = w.document()->canvas();
    v->setForeground(LightRed);
    ToolOptions* d = dock(w);
    d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
    d->foldsBox()->setValue(4);
    REQUIRE(v->symmetry() == Symmetry{SymmetryMode::Radial, 4});
    StrokeOptions o;
    o.symmetry = v->symmetry();
    // a line drawn entirely above the canvas: its quarter turns land on
    // the canvas's sides
    Pixels above;
    for (int x = 20; x <= 60; ++x)
        above.push_back({x, -5});
    const CellEdit want = plotPixels(blank, above, LightRed, o);
    REQUIRE_FALSE(want.empty());
    use(w, "line");
    Tool* t = w.tools()->active();
    const auto event = [](int x, int py, Qt::MouseButton b, Qt::MouseButtons held) {
        return ToolEvent{QPoint(x, py >> 1), py, b, held, Qt::NoModifier, 1.0};
    };
    REQUIRE(t->mousePress(event(20, -5, Qt::LeftButton, Qt::LeftButton)));
    t->mouseMove(event(60, -5, Qt::NoButton, Qt::LeftButton));
    CHECK(dragTool(w)->pending().changes == want.changes);
    t->mouseRelease(event(60, -5, Qt::LeftButton, Qt::NoButton));
    Canvas expect = blank;
    want.apply(expect);
    CHECK(w.document()->canvas() == expect);
}

TEST_CASE("the dock's radial centre stays on the canvas") {
    MainWindow w;
    ToolOptions* d = dock(w);
    const Canvas& c = w.document()->canvas();
    CHECK(d->centreXBox()->maximum() == c.width() - 1);
    CHECK(d->centreYBox()->maximum() == c.height() - 1);
    d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
    d->centreXBox()->setValue(30);
    d->centreYBox()->setValue(20);
    d->centreBox()->setChecked(true);
    REQUIRE(w.canvasView()->symmetry().centre == Point{30, 20});
    // a smaller canvas pulls the centre in to its edge
    AnsiDocument doc;
    doc.canvas = Canvas(12, 5);
    w.document()->replace(std::move(doc), QString());
    CHECK(d->centreXBox()->maximum() == 11);
    CHECK(d->centreYBox()->maximum() == 4);
    CHECK(w.canvasView()->symmetry().centre == Point{11, 4});
    // and a bigger one lets it out again
    AnsiDocument big;
    big.canvas = Canvas(200, 90);
    w.document()->replace(std::move(big), QString());
    CHECK(d->centreXBox()->maximum() == 199);
    CHECK(d->centreYBox()->maximum() == 89);
    d->centreXBox()->setValue(150);
    CHECK(w.canvasView()->symmetry().centre == Point{150, 4});
    // Codex round 2 #4: with symmetry off, a smaller canvas still pulls the
    // saved centre in, so turning symmetry back on turns about it
    d->centreXBox()->setValue(30);
    d->centreYBox()->setValue(20);
    REQUIRE(w.canvasView()->symmetry().centre == Point{30, 20});
    w.canvasView()->setMirror(false);
    AnsiDocument small;
    small.canvas = Canvas(12, 5);
    w.document()->replace(std::move(small), QString());
    CHECK(w.canvasView()->symmetryChoice().centre == Point{11, 4});
    w.canvasView()->setMirror(true);
    CHECK(w.canvasView()->symmetry().centre == Point{11, 4});
    CHECK(d->centreXBox()->value() == 11);
    CHECK(d->centreYBox()->value() == 4);
}

TEST_CASE("the texture stamp is in the Tools menu and the toolbox, with its key and its rows") {
    FreshTextureSettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    QAction* a = act(w, QStringLiteral("textureTool"));
    // Codex round 1 P3: Ctrl+Shift+X is the planned Text tool's (PLAN §15)
    CHECK(a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    QToolButton* b = w.toolbox()->buttonFor(QStringLiteral("texture"));
    REQUIRE(b);
    // with the other detail brushes
    CHECK(b == w.toolbox()->buttonFor(QStringLiteral("outline")));
    CHECK((b->defaultAction() == a || (b->menu() && b->menu()->actions().contains(a))));

    ToolOptions* d = dock(w);
    QComboBox* kind = d->textureKindBox();
    QDoubleSpinBox* density = d->textureDensityBox();
    QSpinBox* seed = d->textureSeedBox();
    // shown for the texture stamp only, with the ramp and the light
    use(w, "rimLight");
    CHECK_FALSE(kind->isVisibleTo(d));
    use(w, "texture");
    CHECK(a->isChecked());
    for (QWidget* row : {static_cast<QWidget*>(kind), static_cast<QWidget*>(density), static_cast<QWidget*>(seed),
                         static_cast<QWidget*>(d->rampBox()), static_cast<QWidget*>(d->lightBox())})
        CHECK(row->isVisibleTo(d));
    CHECK_FALSE(d->strengthBox()->isVisibleTo(d));

    CHECK(v->textureOptions() == TextureOptions{});
    REQUIRE(kind->count() == 5);
    CHECK(kind->currentData().toInt() == int(TextureKind::Scales));
    kind->setCurrentIndex(kind->findData(int(TextureKind::Hoard)));
    density->setValue(0.8);
    seed->setValue(42);
    CHECK(v->textureOptions() == TextureOptions{TextureKind::Hoard, 0.8, 42});
    CHECK(density->minimum() == kMinTextureDensity);
    CHECK(density->maximum() == kMaxTextureDensity);
    CHECK(seed->maximum() == kMaxTextureSeed);
    // and follows the view
    v->setTextureOptions(TextureOptions{TextureKind::Foliage, 2.0, 7});
    CHECK(kind->currentData().toInt() == int(TextureKind::Foliage));
    CHECK(density->value() == 2.0);
    CHECK(seed->value() == 7);
    // the view keeps its settings in range
    v->setTextureOptions(TextureOptions{TextureKind::Scales, 99.0, -5});
    CHECK(v->textureOptions() == TextureOptions{TextureKind::Scales, kMaxTextureDensity, 0});
}

TEST_CASE("the texture picker offers every kind by name, brick and noise last") {
    FreshTextureSettings fresh;
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "texture");
    QComboBox* kind = dock(w)->textureKindBox();
    const std::pair<TextureKind, QString> kinds[] = {{TextureKind::Scales, "Scales"},
                                                     {TextureKind::Foliage, "Foliage"},
                                                     {TextureKind::Hoard, "Hoard"},
                                                     {TextureKind::Brick, "Brick"},
                                                     {TextureKind::Noise, "Noise"}};
    REQUIRE(kind->count() == 5);
    for (int i = 0; i < 5; ++i) {
        INFO(i);
        CHECK(kind->itemData(i).toInt() == int(kinds[i].first));
        CHECK(kind->itemText(i) == kinds[i].second);
        CHECK(textureKindName(kinds[i].first) == kinds[i].second);
    }
    for (const auto& [k, name] : kinds) {
        kind->setCurrentIndex(kind->findData(int(k)));
        CHECK(v->textureOptions().kind == k);
        CHECK(w.tools()->active()->status().contains(name.toLower()));
    }
}

TEST_CASE("the texture stamp's kind, density and seed are kept between runs", "[settings]") {
    FreshTextureSettings fresh;
    {
        MainWindow w;
        CHECK(w.canvasView()->textureOptions() == TextureOptions{});
        ToolOptions* d = dock(w);
        d->textureKindBox()->setCurrentIndex(d->textureKindBox()->findData(int(TextureKind::Noise)));
        d->textureDensityBox()->setValue(0.7);
        d->textureSeedBox()->setValue(13);
    }
    REQUIRE(bwtest::settingsInScratch(FreshTextureSettings::dir()));
    // the kind by name, so a kind added later doesn't shift a saved one
    CHECK(appSettings()->value(QStringLiteral("texture/kind")).toString() == QLatin1String("noise"));
    {
        MainWindow w;
        CHECK(w.canvasView()->textureOptions() == TextureOptions{TextureKind::Noise, 0.7, 13});
        ToolOptions* d = dock(w);
        CHECK(d->textureKindBox()->currentData().toInt() == int(TextureKind::Noise));
        CHECK(d->textureDensityBox()->value() == 0.7);
        CHECK(d->textureSeedBox()->value() == 13);
        w.canvasView()->setTextureOptions(TextureOptions{TextureKind::Brick, 2.5, 0});
    }
    {
        MainWindow w;
        CHECK(w.canvasView()->textureOptions() == TextureOptions{TextureKind::Brick, 2.5, 0});
    }
    // a kind not known, or a number, reads as scales; density and seed out of
    // range are taken into it, and not numbers at all leave the defaults
    for (const auto& [kind, density, seed, want] :
         {std::tuple{QStringLiteral("marble"), QVariant(9.0), QVariant(-4), TextureOptions{TextureKind::Scales, 2.5, 0}},
          std::tuple{QStringLiteral("4"), QVariant(0.01), QVariant(1000000), TextureOptions{TextureKind::Scales, 0.3, 99999}},
          std::tuple{QStringLiteral("hoard"), QVariant(QStringLiteral("thick")), QVariant(QStringLiteral("x")),
                     TextureOptions{TextureKind::Hoard, 1.6, 1}}}) {
        INFO(kind.toStdString());
        appSettings()->setValue(QStringLiteral("texture/kind"), kind);
        appSettings()->setValue(QStringLiteral("texture/density"), density);
        appSettings()->setValue(QStringLiteral("texture/seed"), seed);
        MainWindow w;
        CHECK(w.canvasView()->textureOptions() == want);
        CHECK(dock(w)->textureKindBox()->currentData().toInt() == int(want.kind));
    }
}

TEST_CASE("a texture stamp drag paints the dock's texture, one undo step") {
    FreshTextureSettings fresh;
    for (const TextureKind k : {TextureKind::Hoard, TextureKind::Brick, TextureKind::Noise}) {
        INFO(textureKindName(k).toStdString());
        MainWindow w;
        REQUIRE(bwtest::showExposed(&w));
        CanvasView* v = w.canvasView();
        v->setRamp(steel());
        v->setBrushSettings(BrushSettings{9, Tip::Square, 1.0, PressureTo::Nothing, Compass::NE});
        const TextureOptions t{k, 1.6, 9};
        v->setTextureOptions(t);
        const int steps = w.document()->undoStack()->index();
        use(w, "texture");
        const CellEdit shown = drag(w, {{10, 20}, {16, 20}, {20, 24}});
        landed(w, shown, steps);
        const Canvas& c = w.document()->canvas();
        int lit = 0;
        for (const auto& [p, cell] : written(shown)) {
            INFO(p.first << "," << p.second);
            const int r = textureRung(steel(), t, Compass::NE, p.first, p.second, true, c.width(), c.height());
            CHECK(cell == rungCell(steel(), r));
            lit += r > 0;
        }
        CHECK(lit > 10);
        // only the texture's cells: a second drag over the first changes nothing
        const int after = w.document()->undoStack()->index();
        const CellEdit again = dragTool(w)->pending();
        CHECK(again.empty());
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 12, 20), Qt::LeftButton, Qt::LeftButton);
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 12, 20), Qt::LeftButton, Qt::NoButton);
        CHECK(w.document()->undoStack()->index() == after);
    }
}

namespace {

SprayTool* sprayTool(MainWindow& w) {
    auto* t = qobject_cast<SprayTool*>(w.tools()->find(QStringLiteral("spray")));
    REQUIRE(t);
    return t;
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

TEST_CASE("the glyph spray is in the Tools menu and the toolbox, with its key and its rows") {
    FreshSpraySettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    QAction* a = act(w, QStringLiteral("sprayTool"));
    CHECK(a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y));
    CHECK(QString(sprayTool(w)->name()).remove(QLatin1Char('&')) == QStringLiteral("Glyph Spray"));
    bwtest::systemKey(w.canvasView(), Qt::Key_Y, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(w.tools()->active()->id() == QStringLiteral("spray"));
    CHECK(a->isChecked());
    QToolButton* b = w.toolbox()->buttonFor(QStringLiteral("spray"));
    REQUIRE(b);
    // with the other detail brushes
    CHECK(b == w.toolbox()->buttonFor(QStringLiteral("outline")));
    CHECK((b->defaultAction() == a || (b->menu() && b->menu()->actions().contains(a))));

    ToolOptions* d = dock(w);
    QComboBox* kind = d->sprayKindBox();
    QSpinBox* density = d->sprayDensityBox();
    QSpinBox* seed = d->spraySeedBox();
    // shown for the spray only, with the ramp and the brush settings
    use(w, "texture");
    CHECK_FALSE(kind->isVisibleTo(d));
    use(w, "spray");
    for (QWidget* row : {static_cast<QWidget*>(kind), static_cast<QWidget*>(density), static_cast<QWidget*>(seed),
                         static_cast<QWidget*>(d->rampBox()), static_cast<QWidget*>(d->sizeBox())})
        CHECK(row->isVisibleTo(d));
    CHECK_FALSE(d->strengthBox()->isVisibleTo(d));
    CHECK_FALSE(d->textureKindBox()->isVisibleTo(d));
    const auto shown = d->shown();
    CHECK(shown.testFlag(ToolOptions::Option::Symmetry));
    CHECK(shown.testFlag(ToolOptions::Option::AlphaLock));

    SprayTool* t = sprayTool(w);
    CHECK(t->options() == SprayOptions{});
    REQUIRE(kind->count() == 3);
    CHECK(kind->currentData().toInt() == int(SprayKind::Stars));
    CHECK(density->value() == SprayOptions{}.density);
    kind->setCurrentIndex(kind->findData(int(SprayKind::Smoke)));
    density->setValue(55);
    seed->setValue(321);
    CHECK(t->options() == SprayOptions{SprayKind::Smoke, 55, 321});
    CHECK(density->minimum() == kMinSprayDensity);
    CHECK(density->maximum() == kMaxSprayDensity);
    CHECK(seed->maximum() == kMaxSpraySeed);
    // and follows the tool
    t->setKind(SprayKind::Haze);
    t->setDensity(12);
    t->setSeed(8);
    CHECK(kind->currentData().toInt() == int(SprayKind::Haze));
    CHECK(density->value() == 12);
    CHECK(seed->value() == 8);
    // the tool keeps its settings in range
    t->setDensity(400);
    t->setSeed(-3);
    CHECK(t->options() == SprayOptions{SprayKind::Haze, kMaxSprayDensity, 0});
    CHECK(sprayKindName(SprayKind::Stars) == QStringLiteral("Stars"));
    CHECK(t->status().contains(QStringLiteral("haze")));
}

TEST_CASE("the glyph spray's kind, density and seed are kept between runs", "[settings]") {
    FreshSpraySettings fresh;
    {
        MainWindow w;
        CHECK(sprayTool(w)->options() == SprayOptions{});
        ToolOptions* d = dock(w);
        d->sprayKindBox()->setCurrentIndex(d->sprayKindBox()->findData(int(SprayKind::Haze)));
        d->sprayDensityBox()->setValue(70);
        d->spraySeedBox()->setValue(4242);
        CHECK(sprayTool(w)->options() == SprayOptions{SprayKind::Haze, 70, 4242});
    }
    REQUIRE(bwtest::settingsInScratch(FreshSpraySettings::dir()));
    CHECK(appSettings()->value(QStringLiteral("spray/kind")).toString() == QStringLiteral("haze"));
    CHECK(appSettings()->value(QStringLiteral("spray/density")).toInt() == 70);
    CHECK(appSettings()->value(QStringLiteral("spray/seed")).toInt() == 4242);
    {
        MainWindow w;
        CHECK(sprayTool(w)->options() == SprayOptions{SprayKind::Haze, 70, 4242});
        ToolOptions* d = dock(w);
        CHECK(d->sprayKindBox()->currentData().toInt() == int(SprayKind::Haze));
        CHECK(d->sprayDensityBox()->value() == 70);
        CHECK(d->spraySeedBox()->value() == 4242);
        sprayTool(w)->setKind(SprayKind::Smoke);
    }
    CHECK(appSettings()->value(QStringLiteral("spray/kind")).toString() == QStringLiteral("smoke"));
    // a value out of range or unknown reads as in range, or the default
    appSettings()->setValue(QStringLiteral("spray/kind"), QStringLiteral("fog"));
    appSettings()->setValue(QStringLiteral("spray/density"), 1000);
    appSettings()->setValue(QStringLiteral("spray/seed"), -1);
    {
        MainWindow w;
        CHECK(sprayTool(w)->options() == SprayOptions{SprayKind::Stars, kMaxSprayDensity, 0});
    }
}

TEST_CASE("a glyph spray drag lays the field's stars, one undo step; the right button takes them off") {
    FreshSpraySettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setBrushSettings(BrushSettings{9, Tip::Square, 1.0, PressureTo::Nothing});
    use(w, "spray");
    SprayTool* t = sprayTool(w);
    t->setDensity(100);
    t->setSeed(17);
    const int steps = w.document()->undoStack()->index();
    const CellEdit shown = drag(w, {{6, 20}, {30, 20}, {40, 30}});
    landed(w, shown, steps);
    const Canvas& c = w.document()->canvas();
    for (const auto& [p, cell] : written(shown)) {
        INFO(p.first << "," << p.second);
        CHECK(sprayStar(t->options(), p.first, p.second, c.width(), c.height()) == cell.ch);
        CHECK(cell.fg == v->pen().fg);
    }
    CHECK(written(shown).size() > 3);
    // over its own stars a drag changes nothing
    const int after = w.document()->undoStack()->index();
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 12, 20), Qt::LeftButton, Qt::LeftButton);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 12, 20), Qt::LeftButton, Qt::NoButton);
    CHECK(w.document()->undoStack()->index() == after);
    // the right button wipes them
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 6, 20), Qt::RightButton, Qt::RightButton);
    bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, 30, 20), Qt::NoButton, Qt::RightButton);
    bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, 40, 30), Qt::NoButton, Qt::RightButton);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 40, 30), Qt::RightButton, Qt::NoButton);
    CHECK(w.document()->undoStack()->index() == after + 1);
    for (const auto& [p, cell] : written(shown))
        CHECK(at(w, p.first, p.second).ch == U' ');
}

TEST_CASE("a haze drag lifts the ramp by the field and adds no lint finding") {
    FreshSpraySettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    v->setBrushSettings(BrushSettings{11, Tip::Square, 1.0, PressureTo::Nothing});
    CellEdit ground;
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            ground.changes.push_back({x, y, at(w, x, y), rungCell(steel(), 4)});
    REQUIRE(v->applyEdit(ground));
    const Canvas before = w.document()->canvas();
    use(w, "spray");
    SprayTool* t = sprayTool(w);
    t->setKind(SprayKind::Haze);
    t->setDensity(50);
    t->setSeed(6);
    const int steps = w.document()->undoStack()->index();
    const CellEdit shown = drag(w, {{4, 10}, {30, 10}, {30, 24}});
    landed(w, shown, steps);
    const Canvas& c = w.document()->canvas();
    for (const auto& [p, cell] : written(shown)) {
        INFO(p.first << "," << p.second);
        const auto r = rungOf(steel(), cell);
        REQUIRE(r);
        CHECK(*r > 4);
        CHECK(*r <= 4 + sprayLift(t->options(), p.first, p.second, c.width(), c.height()));
    }
    std::vector<std::pair<int, int>> changed;
    for (const auto& [p, cell] : written(shown))
        changed.push_back(p);
    CHECK(lint::addedFindingsOnce(before, c, steel(), changed).empty());
}

TEST_CASE("changing the glyph spray's kind, density or seed mid-drag redoes the stroke with them") {
    // Codex round 1, finding 4: the drag kept the brush it was pressed with.
    FreshSpraySettings fresh;
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    const BrushSettings settings{9, Tip::Square, 1.0, PressureTo::Nothing};
    v->setBrushSettings(settings);
    CellEdit ground;
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            ground.changes.push_back({x, y, at(w, x, y), rungCell(steel(), 4)});
    REQUIRE(v->applyEdit(ground));
    const Canvas before = w.document()->canvas();
    use(w, "spray");
    SprayTool* t = sprayTool(w);
    t->setKind(SprayKind::Stars);
    t->setDensity(100);
    t->setSeed(17);
    // what the same dabs paint with the options as they are now
    const auto replay = [&](std::vector<Dab> dabs) {
        return paintStroke(before, SprayBrush(steel(), t->options(), v->pen().fg), settings, dabs);
    };
    const int steps = w.document()->undoStack()->index();
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 4, 20), Qt::LeftButton, Qt::LeftButton);
    bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, 30, 20), Qt::NoButton, Qt::LeftButton);
    const std::vector<Dab> dabs{Dab{4, 10}, Dab{30, 10}};
    REQUIRE_FALSE(dragTool(w)->pending().empty());
    CHECK(dragTool(w)->pending().changes == replay(dabs).changes);
    t->setKind(SprayKind::Haze);
    CHECK(dragTool(w)->pending().changes == replay(dabs).changes);
    for (const CellChange& c : dragTool(w)->pending().changes)
        CHECK_FALSE(isSprayStar(c.after.ch));
    t->setDensity(50);
    CHECK(dragTool(w)->pending().changes == replay(dabs).changes);
    const CellEdit seeded = dragTool(w)->pending();
    t->setSeed(6);
    CHECK_FALSE(dragTool(w)->pending().changes == seeded.changes);
    CHECK(dragTool(w)->pending().changes == replay(dabs).changes);
    bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, 30, 32), Qt::NoButton, Qt::LeftButton);
    const CellEdit shown = dragTool(w)->pending();
    CHECK(shown.changes == replay({Dab{4, 10}, Dab{30, 10}, Dab{30, 16}}).changes);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 30, 32), Qt::LeftButton, Qt::NoButton);
    landed(w, shown, steps);
}
