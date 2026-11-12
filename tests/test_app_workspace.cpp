// The workspace (PLAN §11): the Toolbox, Tool Options and glyph bar docks,
// the pen's swap and reset, the Window menu and the saved layout.

#include "blockwright/glyph_sets.h"

#include "canvas_view.h"
#include "device_grid.h"
#include "document.h"
#include "draw_tools.h"
#include "flow_row.h"
#include "glyph_bar.h"
#include "lint_dock.h"
#include "main_window.h"
#include "screenshot.h"
#include "palette_widget.h"
#include "scratch_settings.h"
#include "select_tool.h"
#include "tool.h"
#include "tool_options.h"
#include "toolbox.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFontDatabase>
#include <QHelpEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QRawFont>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <tuple>
#include <utility>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

QAction* toolAction(MainWindow& w, const QString& id) { return act(w, id + QStringLiteral("Tool")); }

QString keyText(const QKeySequence& k) { return k.toString(QKeySequence::NativeText); }

void press(QWidget* w, QPoint at, Qt::MouseButton button = Qt::LeftButton) {
    QMouseEvent e(QEvent::MouseButtonPress, QPointF(at), w->mapToGlobal(QPointF(at)), button, button, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

// Ask the current style for the split button's arrow or icon hit area.
QRect toolboxButtonRect(QToolButton* button, QStyle::SubControl control) {
    QStyleOptionToolButton option;
    option.initFrom(button);
    option.features = QStyleOptionToolButton::HasMenu | QStyleOptionToolButton::MenuButtonPopup;
    option.iconSize = button->iconSize();
    return button->style()->subControlRect(QStyle::CC_ToolButton, &option, control, button);
}

// The Window menu, by its title.
QMenu* windowMenu(MainWindow& w) {
    for (QAction* a : w.menuBar()->actions())
        if (a->menu() && a->text().remove(QLatin1Char('&')) == QStringLiteral("Window"))
            return a->menu();
    return nullptr;
}

// The right column's native minimum width: its widest open dock's minimum,
// the palette's or a scrolling panel's with its vertical scrollbar.
int rightColumnMinimum(MainWindow& w) {
    int minimum = 0;
    for (QDockWidget* d : w.docks())
        if (w.dockWidgetArea(d) == Qt::RightDockWidgetArea && !d->isHidden() && !d->isFloating())
            minimum = std::max({minimum, d->minimumWidth(), d->minimumSizeHint().width()});
    return minimum;
}

// The narrowest the right column goes, by Qt's own clamp. Leaves it there.
int rightColumnFloor(MainWindow& w) {
    w.resizeDocks({w.libraryDock()}, {100}, Qt::Horizontal);
    QApplication::processEvents();
    return w.libraryDock()->width();
}

// Drags the separator between a side dock's column and the canvas with the
// mouse until the column is `width` wide, as the user does.
void dragColumn(MainWindow& w, QDockWidget* d, int width) {
    const Qt::DockWidgetArea area = w.dockWidgetArea(d);
    REQUIRE((area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea));
    const QRect dock = d->geometry();
    const QRect canvas = w.centralWidget()->geometry();
    const bool left = area == Qt::LeftDockWidgetArea;
    const QPoint from((left ? dock.right() + canvas.left() : canvas.right() + dock.left()) / 2 + 1, dock.center().y());
    const QPoint to = from + QPoint(left ? width - d->width() : d->width() - width, 0);
    const auto send = [&w](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(at), w.mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&w, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    QApplication::processEvents(); // Qt moves the separator on a timer
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
}

// The side columns' height: they span the canvas's rows.
int columnHeight(MainWindow& w) { return w.centralWidget()->height(); }

int separatorExtent(MainWindow& w) { return w.style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, &w); }

// The height a scrolling panel shows all of its content in, title bar
// included: Layers and Lint ask for a set number of list rows.
int panelTarget(QDockWidget* d) {
    auto* scroll = qobject_cast<QScrollArea*>(d->widget());
    REQUIRE(scroll);
    QWidget* content = scroll->widget();
    const int inner = d->width() - 2 * scroll->frameWidth();
    const int height = content->hasHeightForWidth() ? content->heightForWidth(inner) : content->sizeHint().height();
    return d->height() - scroll->height() + 2 * scroll->frameWidth() + height;
}

// Drags the separator under `upper` with the mouse by `dy`, as the user does.
void dragSeparatorBelow(MainWindow& w, QDockWidget* upper, int dy) {
    const QPoint from(upper->geometry().center().x(), upper->geometry().bottom() + 1 + separatorExtent(w) / 2);
    const auto send = [&w](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(at), w.mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&w, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, from + QPoint(0, dy), Qt::NoButton, Qt::LeftButton);
    QApplication::processEvents(); // Qt moves the separator on a timer
    send(QEvent::MouseButtonRelease, from + QPoint(0, dy), Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
}

// The scratch directory the run's settings file is in.
QString scratchDir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }

// Starts from no saved layout and forgets the one the windows here saved, from
// the scratch settings only.
struct ClearLayout {
    ClearLayout() { REQUIRE(bwtest::resetScratchSettings()); }
    ~ClearLayout() { bwtest::resetScratchSettings(); }
};

// The bundled Noto Sans, from the app's resources (third_party/noto-sans/).
QString bundledFont(bool bold) {
    return bold ? QStringLiteral(":/ui-font/NotoSans-Bold.ttf") : QStringLiteral(":/ui-font/NotoSans-Regular.ttf");
}

// The font revision in a 'head' table (16.16 fixed point at offset 4), or "?".
QString headRevision(const QByteArray& head) {
    if (head.size() < 8)
        return QStringLiteral("?");
    const auto* b = reinterpret_cast<const unsigned char*>(head.constData()) + 4;
    const quint32 fixed = (quint32(b[0]) << 24) | (quint32(b[1]) << 16) | (quint32(b[2]) << 8) | b[3];
    return QString::number(fixed / 65536.0, 'f', 3);
}

// Empty when `resolved` is the font in the file at `path` (their 'head' tables
// match); otherwise names the font that was resolved and the file.
QString fontFileMismatch(const QRawFont& resolved, const QString& path) {
    const QByteArray want = QRawFont(path, 10).fontTable("head");
    const QByteArray got = resolved.fontTable("head");
    if (!want.isEmpty() && got == want)
        return {};
    return QStringLiteral("%1 %2 resolves to a font of revision %3, not the bundled %4 (revision %5): "
                          "another font of that family on this machine shadows it")
        .arg(resolved.familyName(), resolved.styleName(), headRevision(got), path, headRevision(want));
}

// The bytes of a font file's table `tag`, as an offset and length into `font`,
// or {0, 0} when it has none.
std::pair<qsizetype, qsizetype> fontTableRange(const QByteArray& font, const char (&tag)[5]) {
    const auto u16 = [&](qsizetype at) { return (quint8(font[at]) << 8) | quint8(font[at + 1]); };
    const auto u32 = [&](qsizetype at) { return (quint32(u16(at)) << 16) | quint32(u16(at + 2)); };
    if (font.size() < 12)
        return {0, 0};
    const int tables = u16(4);
    for (int i = 0; i < tables && 12 + 16 * (i + 1) <= font.size(); ++i) {
        const qsizetype record = 12 + 16 * i;
        const qsizetype offset = u32(record + 8), length = u32(record + 12);
        if (font.mid(record, 4) == QByteArray(tag) && offset + length <= font.size())
            return {offset, length};
    }
    return {0, 0};
}

// The family the layout tests register the bundled Noto Sans under: a name no
// installed font has, as long as "Noto Sans" so the file's name records keep
// their lengths. Under its own name the bundled file shares Qt's slot for
// Noto Sans (family, foundry and style) with any other Noto Sans on the
// machine, and the font registered last holds it. A fresh process, as ctest
// runs each case in, adds the bundled file before the font database fills, so
// the system's files filled in after it took the slot (#253).
constexpr char kLayoutFamily[] = "Bw Layout";
static_assert(sizeof(kLayoutFamily) == sizeof("Noto Sans"));

// The file at `path` renamed to kLayoutFamily as --screenshot renames it
// (screenshot::renamedFont): every "Noto Sans" in its name table, and the
// PostScript name's "NotoSans", in the Mac Roman and the UTF-16 records
// alike. Nothing else in the font changes.
QByteArray renamedBundledFont(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return screenshot::renamedFont(file.readAll(), kLayoutFamily);
}

// Sets the application's font for a test and puts the old one back: Noto
// Sans at `pointSize`, from the app's resources, so the layout tests measure
// the same font on every machine (#236). 10 pt is the desktop default the
// panels are sized for; 9 pt is the test platform's own size.
struct UiFont {
    const QFont saved = QApplication::font();
    explicit UiFont(int pointSize) {
        static const QString family = [] {
            QString name;
            for (const bool bold : {false, true}) {
                const QStringList families = QFontDatabase::applicationFontFamilies(
                    QFontDatabase::addApplicationFontFromData(renamedBundledFont(bundledFont(bold))));
                if (!families.isEmpty())
                    name = families.first();
            }
            return name;
        }();
        REQUIRE(family == QLatin1String(kLayoutFamily));
        QFont f = saved;
        f.setFamily(family);
        f.setPointSize(pointSize);
        QApplication::setFont(f);
        REQUIRE(QFontInfo(QApplication::font()).family() == family);
        // The family is the bundled one's, but another font of that family
        // installed on the machine can still win a style (#253).
        for (const bool bold : {false, true}) {
            QFont styled = QApplication::font();
            styled.setBold(bold);
            const QString mismatch = fontFileMismatch(QRawFont::fromFont(styled), bundledFont(bold));
            if (!mismatch.isEmpty())
                FAIL(mismatch.toStdString());
        }
    }
    ~UiFont() { QApplication::setFont(saved); }
    UiFont(const UiFont&) = delete;
    UiFont& operator=(const UiFont&) = delete;
};

// The left column's default where the canvas has room, and what it keeps
// before the right column gives up any width (PLAN §11).
constexpr int kDefaultLeft = 360;
constexpr int kNarrowLeft = 256;

// The default widths at a window wide enough for both columns' defaults
// less the left's give-back: the right column is 360, and the left is 360
// or, when that would leave the canvas under 640, what leaves it 640.
void checkDefaultWidths(MainWindow& w) {
    const int left = w.toolboxDock()->isVisible() ? w.toolboxDock()->width() : w.toolOptionsDock()->width();
    const int viewport = qobject_cast<QScrollArea*>(w.centralWidget())->viewport()->width();
    CAPTURE(left, viewport);
    CHECK(w.libraryDock()->width() == 360);
    CHECK(left >= kNarrowLeft);
    CHECK(left <= kDefaultLeft);
    if (left < kDefaultLeft)
        CHECK(viewport == 640);
    else
        CHECK(viewport >= 640);
}

} // namespace

TEST_CASE("the layout tests name a font that isn't the bundled file", "[ui-font]") {
    const QRawFont regular(bundledFont(false), 10), bold(bundledFont(true), 10);
    REQUIRE(regular.isValid());
    REQUIRE(bold.isValid());
    CHECK(fontFileMismatch(regular, bundledFont(false)).isEmpty());
    CHECK(fontFileMismatch(bold, bundledFont(true)).isEmpty());
    const QString mismatch = fontFileMismatch(bold, bundledFont(false));
    INFO(mismatch.toStdString());
    CHECK(mismatch.contains(QStringLiteral("Noto Sans Bold resolves to a font of revision ")));
    CHECK(mismatch.contains(QStringLiteral("NotoSans-Regular.ttf (revision 2.015)")));
    CHECK(mismatch.contains(QStringLiteral("shadows it")));
    CHECK(fontFileMismatch(regular, QStringLiteral("no-such-font.ttf")).contains(QStringLiteral("revision ?")));
    CHECK(headRevision(QByteArray("\0\1\0\0\0\2\x80\0", 8)) == QStringLiteral("2.500"));
}

TEST_CASE("the bundled font is renamed in its name table and nowhere else", "[ui-font]") {
    for (const bool bold : {false, true}) {
        QFile file(bundledFont(bold));
        REQUIRE(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll();
        const QByteArray renamed = renamedBundledFont(bundledFont(bold));
        REQUIRE(renamed.size() == original.size());
        const auto [offset, length] = fontTableRange(original, "name");
        REQUIRE(length > 0);
        CHECK(renamed.left(offset) == original.left(offset));
        CHECK(renamed.mid(offset + length) == original.mid(offset + length));
        CHECK(renamed.mid(offset, length) != original.mid(offset, length));
        const QRawFont raw(renamed, 10);
        REQUIRE(raw.isValid());
        CHECK(raw.familyName() == QLatin1String(kLayoutFamily));
        CHECK(raw.styleName() == (bold ? QStringLiteral("Bold") : QStringLiteral("Regular")));
        CHECK(raw.fontTable("head") == QRawFont(bundledFont(bold), 10).fontTable("head"));
    }
    CHECK(renamedBundledFont(QStringLiteral("no-such-font.ttf")).isEmpty());
}

// Another Noto Sans on the machine, registered after the bundled one (as the
// system's are when the font database fills late, in a process of a test's
// own), still leaves the layout tests the bundled file: before the bundled
// file had a family of its own, this decoy took its place and UiFont failed
// with "Noto Sans Regular resolves to a font of revision 1.500".
TEST_CASE("another Noto Sans registered after the bundled one doesn't take its place", "[ui-font]") {
    { UiFont first(10); }
    // removed again however the case ends, so no later case in this process meets them
    struct Decoys {
        QList<int> ids;
        ~Decoys() {
            for (const int id : ids)
                QFontDatabase::removeApplicationFont(id);
        }
    } decoys;
    for (const bool bold : {false, true}) {
        QFile file(bundledFont(bold));
        REQUIRE(file.open(QIODevice::ReadOnly));
        QByteArray decoy = file.readAll();
        const auto [offset, length] = fontTableRange(decoy, "head");
        REQUIRE(length >= 8);
        decoy.replace(offset + 4, 4, QByteArray("\0\1\x80\0", 4)); // revision 1.500
        decoys.ids.append(QFontDatabase::addApplicationFontFromData(decoy));
        REQUIRE(QFontDatabase::applicationFontFamilies(decoys.ids.last()) == QStringList{QStringLiteral("Noto Sans")});
    }
    {
        UiFont font(10);
        for (const bool bold : {false, true}) {
            QFont styled = QApplication::font();
            styled.setBold(bold);
            CHECK(fontFileMismatch(QRawFont::fromFont(styled), bundledFont(bold)).isEmpty());
        }
    }
}

TEST_CASE("the toolbox has a button for every tool, with the Tools menu's actions and keys") {
    MainWindow w;
    Toolbox* box = w.toolbox();
    REQUIRE(box);
    CHECK(w.toolboxDock()->widget() == box);
    CHECK(w.toolboxDock()->objectName() == QStringLiteral("toolboxDock"));
    CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::LeftDockWidgetArea);
    CHECK(box->buttons().size() == Toolbox::groups().size());
    for (Tool* t : w.tools()->tools()) {
        INFO(t->id().toStdString());
        QAction* a = toolAction(w, t->id());
        QToolButton* b = box->buttonFor(t->id());
        REQUIRE(b);
        CHECK((b->defaultAction() == a || b->menu()->actions().contains(a)));
        CHECK_FALSE(a->icon().isNull());
        // the tooltip names the tool and its shortcut
        CHECK(a->toolTip().startsWith(QString(t->name()).remove(QLatin1Char('&'))));
        REQUIRE_FALSE(a->shortcut().isEmpty());
        CHECK(a->toolTip().contains(keyText(a->shortcut())));
        CHECK(b->focusPolicy() == Qt::NoFocus);
    }
    // a single tool's button has no menu; a group's has its tools, in order
    CHECK(box->buttonFor(QStringLiteral("keyboard"))->menu() == nullptr);
    QToolButton* shapes = box->buttonFor(QStringLiteral("line"));
    CHECK(shapes == box->buttonFor(QStringLiteral("rectangle")));
    CHECK(shapes == box->buttonFor(QStringLiteral("ellipse")));
    CHECK(shapes == box->buttonFor(QStringLiteral("frame")));
    REQUIRE(shapes->menu());
    CHECK(shapes->menu()->actions() == QList<QAction*>{toolAction(w, QStringLiteral("line")),
                                                       toolAction(w, QStringLiteral("rectangle")),
                                                       toolAction(w, QStringLiteral("ellipse")),
                                                       toolAction(w, QStringLiteral("frame"))});
    CHECK(shapes->popupMode() == QToolButton::MenuButtonPopup);
    CHECK(toolAction(w, QStringLiteral("ellipse"))->toolTip().contains(QStringLiteral("right-click")));
    CHECK_FALSE(toolAction(w, QStringLiteral("eyedropper"))->toolTip().contains(QStringLiteral("right-click")));
}

TEST_CASE("a toolbox button picks its tool, and a group's button shows the last one picked") {
    MainWindow w;
    Toolbox* box = w.toolbox();
    QToolButton* brushes = box->buttonFor(QStringLiteral("rampShade"));
    CHECK(brushes->defaultAction() == toolAction(w, QStringLiteral("rampShade")));

    box->buttonFor(QStringLiteral("eyedropper"))->click();
    CHECK(w.tools()->active()->id() == QStringLiteral("eyedropper"));
    CHECK(box->buttonFor(QStringLiteral("eyedropper"))->isChecked());

    // picked from the menu (or a shortcut): the group's button takes it
    toolAction(w, QStringLiteral("soft"))->trigger();
    CHECK(brushes->defaultAction() == toolAction(w, QStringLiteral("soft")));
    CHECK(brushes->isChecked());
    CHECK(brushes->menu());
    CHECK_FALSE(box->buttonFor(QStringLiteral("eyedropper"))->isChecked());
    // another tool, then a click on the group's button: the last one again
    box->buttonFor(QStringLiteral("fill"))->click();
    CHECK_FALSE(brushes->isChecked());
    brushes->click();
    CHECK(w.tools()->active()->id() == QStringLiteral("soft"));
}

TEST_CASE("clicking each toolbox group's arrow opens its flyout") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    w.show();
    QApplication::processEvents();
    auto* button = w.toolbox()->buttonFor(id);
    REQUIRE(button);
    auto* menu = button->menu();
    REQUIRE(menu);
    const QRect arrow = toolboxButtonRect(button, QStyle::SC_ToolButtonMenu);
    REQUIRE_FALSE(arrow.isEmpty());
    REQUIRE(button->rect().contains(arrow));
    const QString activeBefore = w.tools()->active()->id();
    bool shown = false;
    // QToolButton opens QMenu in a nested event loop. This timer closes it
    // whether the click blocks inside sendEvent or returns asynchronously.
    QTimer closeMenu;
    closeMenu.setInterval(0);
    QObject::connect(&closeMenu, &QTimer::timeout, menu, [&] {
        if (menu->isVisible()) {
            shown = true;
            menu->close();
        }
    });
    closeMenu.start();
    press(button, arrow.center());
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(arrow.center()),
                        button->mapToGlobal(QPointF(arrow.center())), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(button, &release);
    QApplication::processEvents();
    closeMenu.stop();
    CHECK(shown);
    CHECK_FALSE(menu->isVisible());
    CHECK(w.tools()->active()->id() == activeBefore);
}

TEST_CASE("clicking each toolbox group's icon picks its current tool without opening the flyout") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    w.show();
    QApplication::processEvents();
    auto* button = w.toolbox()->buttonFor(id);
    REQUIRE(button);
    auto* menu = button->menu();
    REQUIRE(menu);
    // Pick another member first, then leave the group. The icon must select
    // that remembered member, rather than reverting to the group's first.
    QAction* current = menu->actions().last();
    current->trigger();
    REQUIRE(button->defaultAction() == current);
    toolAction(w, QStringLiteral("keyboard"))->trigger();
    const QRect icon = toolboxButtonRect(button, QStyle::SC_ToolButton);
    REQUIRE_FALSE(icon.isEmpty());
    REQUIRE_FALSE(toolboxButtonRect(button, QStyle::SC_ToolButtonMenu).contains(icon.center()));
    bool opened = false;
    QObject::connect(menu, &QMenu::aboutToShow, menu, [&] { opened = true; });
    QTimer closeMenu;
    QObject::connect(&closeMenu, &QTimer::timeout, menu, &QMenu::close);
    closeMenu.start(0); // also bounds an accidental popup's nested event loop
    press(button, icon.center());
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(icon.center()),
                        button->mapToGlobal(QPointF(icon.center())), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(button, &release);
    QApplication::processEvents();
    closeMenu.stop();
    CHECK_FALSE(opened);
    CHECK_FALSE(menu->isVisible());
    CHECK(button->defaultAction() == current);
    CHECK(toolAction(w, w.tools()->active()->id()) == current);
    CHECK(button->isChecked());
}

TEST_CASE("the toolbox lays its buttons in rows as wide as its dock fits") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    // The number of buttons in each full row, checked to be a grid.
    const auto columnsOf = [](Toolbox* box) {
        const auto buttons = box->buttons();
        int columns = 0;
        while (columns < buttons.size() && buttons[columns]->y() == buttons[0]->y())
            ++columns;
        for (int i = 0; i < buttons.size(); ++i) {
            INFO(buttons[i]->objectName().toStdString());
            CHECK(box->rect().contains(buttons[i]->geometry()));
            CHECK(buttons[i]->width() >= buttons[i]->sizeHint().width());
            if (i % columns > 0) {
                CHECK(buttons[i]->y() == buttons[i - 1]->y());
                CHECK(buttons[i]->x() > buttons[i - 1]->geometry().right());
            } else if (i >= columns) {
                CHECK(buttons[i]->x() == buttons[i - columns]->x());
                CHECK(buttons[i]->y() > buttons[i - columns]->geometry().bottom());
            }
        }
        return columns;
    };
    const int columns = columnsOf(w.toolbox());
    CHECK(columns >= 4);
    CHECK(columns <= 6);
    // Its height is its rows, with nothing to spare.
    CHECK(w.toolbox()->height() == w.toolbox()->sizeHint().height());
    CHECK(w.toolbox()->buttons().back()->geometry().bottom() < w.toolbox()->height());

    QHash<QString, QAction*> actions;
    for (Tool* tool : w.tools()->tools())
        actions.insert(tool->id(), toolAction(w, tool->id()));
    Toolbox narrow(w.tools(), actions);
    CHECK(narrow.minimumSizeHint().width() <= 240);
    CHECK(narrow.sizeHint().width() <= 240);
    narrow.resize(narrow.minimumSizeHint().width(), 600);
    narrow.show();
    QApplication::processEvents();
    // At its narrowest it is two buttons wide, as tall as its rows ask.
    narrow.resize(narrow.width(), narrow.sizeHint().height());
    QApplication::processEvents();
    CHECK(columnsOf(&narrow) == 2);
    // Widening it takes the rows up, and its hint follows.
    int rowsChanged = 0;
    QObject::connect(&narrow, &Toolbox::rowsChanged, [&rowsChanged] { ++rowsChanged; });
    narrow.resize(240, narrow.height());
    QApplication::processEvents();
    CHECK(columnsOf(&narrow) > 2);
    CHECK(rowsChanged == 1);
    CHECK(narrow.sizeHint().height() < narrow.height());
}

TEST_CASE("a right click on a group's toolbox button opens the group") {
    MainWindow w;
    w.show();
    QToolButton* shapes = w.toolbox()->buttonFor(QStringLiteral("line"));
    QMenu* opened = nullptr;
    QTimer::singleShot(0, [&opened] {
        opened = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (opened)
            opened->close();
    });
    emit shapes->customContextMenuRequested(shapes->rect().center());
    CHECK(opened == shapes->menu());
    CHECK(shapes->contextMenuPolicy() == Qt::CustomContextMenu);
}

TEST_CASE("every tool has a Tool Options entry") {
    MainWindow w;
    for (Tool* t : w.tools()->tools()) {
        INFO(t->id().toStdString());
        bool known = false;
        ToolOptions::optionsFor(t->id(), &known);
        CHECK(known);
    }
    bool known = true;
    CHECK(ToolOptions::optionsFor(QStringLiteral("noSuchTool"), &known).testFlag(ToolOptions::Option::Size));
    CHECK_FALSE(known);
}

TEST_CASE("Tool Options shows only the active tool's settings") {
    MainWindow w;
    ToolOptions* o = w.toolOptions();
    REQUIRE(o);
    CHECK(w.toolOptionsDock()->objectName() == QStringLiteral("toolOptionsDock"));
    CHECK(w.dockWidgetArea(w.toolOptionsDock()) == Qt::LeftDockWidgetArea);
    auto* title = o->findChild<QLabel*>(QStringLiteral("toolOptionsTitle"));
    auto* none = o->findChild<QLabel*>(QStringLiteral("toolOptionsNone"));
    REQUIRE(title);
    REQUIRE(none);
    const auto shows = [o](QWidget* control) { return control->isVisibleTo(o); };
    for (Tool* t : w.tools()->tools()) {
        INFO(t->id().toStdString());
        w.tools()->setActive(t);
        const ToolOptions::Options want = ToolOptions::optionsFor(t->id());
        CHECK(o->shown() == want);
        CHECK(title->text() == QString(t->name()).remove(QLatin1Char('&')));
        CHECK(shows(none) == !want);
        CHECK(shows(o->sizeBox()) == want.testFlag(ToolOptions::Option::Size));
        CHECK(shows(o->tipBox()) == want.testFlag(ToolOptions::Option::Tip));
        CHECK(shows(o->strengthBox()) == want.testFlag(ToolOptions::Option::Strength));
        CHECK(shows(o->pressureBox()) == want.testFlag(ToolOptions::Option::Pressure));
        CHECK(shows(o->rampBox()) == want.testFlag(ToolOptions::Option::Ramp));
        CHECK(shows(o->alphaLockBox()) == want.testFlag(ToolOptions::Option::AlphaLock));
        CHECK(shows(o->lightBox()) == want.testFlag(ToolOptions::Option::Light));
        CHECK(shows(o->formBox()) == want.testFlag(ToolOptions::Option::Form));
        CHECK(shows(o->rimBox()) == want.testFlag(ToolOptions::Option::Form));
        CHECK(shows(o->hardnessBox()) == want.testFlag(ToolOptions::Option::Hardness));
        CHECK(shows(o->outlineSelectionBox()) == want.testFlag(ToolOptions::Option::OutlineSelection));
        CHECK(shows(o->symmetryBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->foldsBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->centreBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->centreXBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->pencil4Box()) == want.testFlag(ToolOptions::Option::Pencil));
        CHECK(shows(o->glyphLabel()) == want.testFlag(ToolOptions::Option::Glyph));
        CHECK(shows(o->filledBox()) == want.testFlag(ToolOptions::Option::Filled));
        CHECK(shows(o->pasteModeBox()) == want.testFlag(ToolOptions::Option::PasteMode));
        CHECK(shows(o->sampleMergedBox()) == want.testFlag(ToolOptions::Option::SampleMerged));
        CHECK(shows(o->gradientShapeBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->gradientFromBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->gradientToBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->gradientBandingBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->cloneAlignedBox()) == want.testFlag(ToolOptions::Option::CloneAligned));
        CHECK(shows(o->sprayKindBox()) == want.testFlag(ToolOptions::Option::Spray));
        CHECK(shows(o->sprayDensityBox()) == want.testFlag(ToolOptions::Option::Spray));
        CHECK(shows(o->spraySeedBox()) == want.testFlag(ToolOptions::Option::Spray));
        CHECK(shows(o->rimLintBox()) == want.testFlag(ToolOptions::Option::RimLint));
    }
    // spot checks against the tools themselves
    using O = ToolOptions::Option;
    CHECK(ToolOptions::optionsFor(QStringLiteral("keyboard")) == ToolOptions::Options());
    CHECK(ToolOptions::optionsFor(QStringLiteral("eyedropper")) == ToolOptions::Options(O::SampleMerged));
    CHECK(ToolOptions::optionsFor(QStringLiteral("fill")).testFlag(O::SampleMerged));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("halfBlock")).testFlag(O::SampleMerged));
    CHECK(ToolOptions::optionsFor(QStringLiteral("select")) == ToolOptions::Options(O::PasteMode));
    CHECK(ToolOptions::optionsFor(QStringLiteral("rampShade")).testFlag(O::Strength));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("shade")).testFlag(O::Strength));
    CHECK(ToolOptions::optionsFor(QStringLiteral("rectangle")).testFlag(O::Filled));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("line")).testFlag(O::Filled));
    CHECK(ToolOptions::optionsFor(QStringLiteral("halfBlock")).testFlag(O::Pencil));
    CHECK(ToolOptions::optionsFor(QStringLiteral("outline")).testFlag(O::OutlineSelection));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("rimLight")).testFlag(O::OutlineSelection));
    CHECK(ToolOptions::optionsFor(QStringLiteral("rimLight")).testFlag(O::RimLint));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("form")).testFlag(O::RimLint));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("unknown")).testFlag(O::RimLint));
    CHECK(ToolOptions::optionsFor(QStringLiteral("gradient")) == (O::AlphaLock | O::Ramp | O::Gradient));
    CHECK(ToolOptions::optionsFor(QStringLiteral("clone")).testFlag(O::CloneAligned));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("clone")).testFlag(O::Ramp));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("clone")).testFlag(O::Strength));
    CHECK(ToolOptions::optionsFor(QStringLiteral("spray")) == (ToolOptions::optionsFor(QStringLiteral("clone")) &
                                                               ~ToolOptions::Options(O::CloneAligned) | O::Ramp |
                                                               O::Spray));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("texture")).testFlag(O::Spray));
}

TEST_CASE("every Tool Options control has a tooltip, with its shortcut where it has one") {
    MainWindow w;
    ToolOptions* o = w.toolOptions();
    QList<QWidget*> controls;
    for (QWidget* c : o->findChildren<QSpinBox*>())
        controls << c;
    for (QWidget* c : o->findChildren<QComboBox*>())
        controls << c;
    for (QWidget* c : o->findChildren<QCheckBox*>())
        controls << c;
    CHECK(controls.size() >= 23);
    for (QWidget* c : controls) {
        INFO(c->objectName().toStdString());
        CHECK_FALSE(c->toolTip().isEmpty());
    }
    CHECK(o->sizeBox()->toolTip().contains(keyText(act(w, QStringLiteral("brushSmaller"))->shortcut())));
    CHECK(o->sizeBox()->toolTip().contains(keyText(act(w, QStringLiteral("brushLarger"))->shortcut())));
    CHECK(o->symmetryBox()->toolTip().contains(keyText(act(w, QStringLiteral("mirrorMode"))->shortcut())));
    CHECK(o->filledBox()->toolTip().contains(keyText(act(w, QStringLiteral("filledShapes"))->shortcut())));
}

TEST_CASE("Tool Options' Filled follows Filled Shapes both ways") {
    MainWindow w;
    QAction* filled = act(w, QStringLiteral("filledShapes"));
    auto* rect = qobject_cast<ShapeTool*>(w.tools()->find(QStringLiteral("rectangle")));
    REQUIRE(rect);
    QCheckBox* box = w.toolOptions()->filledBox();
    CHECK_FALSE(box->isChecked());
    box->setChecked(true);
    CHECK(filled->isChecked());
    CHECK(rect->filled());
    filled->trigger();
    CHECK_FALSE(box->isChecked());
    CHECK_FALSE(rect->filled());
}

TEST_CASE("Tool Options' paste mode follows the select tool both ways") {
    MainWindow w;
    QComboBox* box = w.toolOptions()->pasteModeBox();
    CHECK(box->currentData().toInt() == int(PasteMode::Normal));
    box->setCurrentIndex(box->findData(int(PasteMode::Transparent)));
    emit box->activated(box->currentIndex());
    CHECK(w.selectTool()->pasteMode() == PasteMode::Transparent);
    CHECK(act(w, QStringLiteral("pasteTransparent"))->isChecked());
    act(w, QStringLiteral("pasteUnder"))->trigger();
    CHECK(box->currentData().toInt() == int(PasteMode::Under));
}

TEST_CASE("Tool Options shows the glyph brush's glyph") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    toolAction(w, QStringLiteral("glyph"))->trigger();
    auto* glyph = qobject_cast<GlyphTool*>(w.tools()->active());
    REQUIRE(glyph);
    const auto shown = [glyph] {
        const char32_t g = glyph->glyph();
        return QString::fromUcs4(&g, 1);
    };
    CHECK(w.toolOptions()->glyphLabel()->text() == shown());
    QKeyEvent f4(QEvent::KeyPress, Qt::Key_F4, Qt::NoModifier);
    QApplication::sendEvent(v, &f4);
    CHECK(glyph->glyph() == glyphSetGlyph(v->glyphSet(), 3));
    CHECK(w.toolOptions()->glyphLabel()->text() == shown());
}

TEST_CASE("the glyph bar types its glyphs, picks the glyph brush's, and follows the set") {
    MainWindow w;
    GlyphBar* bar = w.glyphBar();
    REQUIRE(bar);
    CanvasView* v = w.canvasView();
    CHECK(w.glyphBarDock()->objectName() == QStringLiteral("glyphBarDock"));
    CHECK(w.dockWidgetArea(w.glyphBarDock()) == Qt::BottomDockWidgetArea);
    for (int k = 0; k < kGlyphSetKeys; ++k) {
        REQUIRE(bar->cell(k));
        CHECK(bar->cell(k)->toolTip().startsWith(QStringLiteral("F%1:").arg(k + 1)));
        CHECK_FALSE(bar->cell(k)->icon().isNull());
    }
    QComboBox* sets = bar->setBox();
    CHECK(sets->count() == kCp437GlyphSetCount);
    CHECK(sets->currentData().toInt() == v->glyphSet());

    // keyboard mode: a click types the glyph at the cursor, like its F-key
    REQUIRE(w.tools()->active()->id() == QStringLiteral("keyboard"));
    bar->cell(1)->click();
    CHECK(w.document()->canvas().at(0, 0).ch == glyphSetGlyph(v->glyphSet(), 1));
    CHECK(v->cellCursor().x == 1);

    // another set from the list, and from the view
    emit sets->activated(sets->findData(4));
    CHECK(v->glyphSet() == 4);
    v->setGlyphSet(6);
    CHECK(sets->currentData().toInt() == 6);
    const char32_t ch = glyphSetGlyph(6, 2);
    CHECK(bar->cell(2)->toolTip().contains(QString::fromUcs4(&ch, 1)));

    // the glyph brush: a click picks the glyph and types nothing
    toolAction(w, QStringLiteral("glyph"))->trigger();
    const Canvas before = w.document()->canvas();
    bar->cell(2)->click();
    CHECK(qobject_cast<GlyphTool*>(w.tools()->active())->glyph() == ch);
    CHECK(w.document()->canvas() == before);
}

TEST_CASE("the pen's swap arrow and reset corner") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    PaletteWidget* p = w.paletteWidget();
    // the parts of the pen don't overlap the swatches or each other's corners
    for (const QRect& r : {p->foregroundRect(), p->backgroundRect(), p->swapRect(), p->resetRect()}) {
        CHECK_FALSE(r.isEmpty());
        CHECK(p->penRect().contains(r));
        CHECK(p->indexAt(r.center()) == -1);
    }
    CHECK_FALSE(p->swapRect().intersects(p->foregroundRect()));
    CHECK_FALSE(p->swapRect().intersects(p->backgroundRect()));
    CHECK_FALSE(p->resetRect().intersects(p->foregroundRect()));
    CHECK_FALSE(p->resetRect().intersects(p->backgroundRect()));

    v->setForeground(Yellow);
    v->setBackground(Blue);
    press(p, p->swapRect().center());
    CHECK(v->pen().fg == Color::indexed(Blue));
    CHECK(v->pen().bg == Color::indexed(Yellow));
    CHECK(p->pen().fg == Color::indexed(Blue));
    act(w, QStringLiteral("swapColours"))->trigger();
    CHECK(v->pen().fg == Color::indexed(Yellow));
    press(p, p->resetRect().center());
    CHECK(v->pen().fg == Color::indexed(Grey));
    CHECK(v->pen().bg == Color::indexed(Black));
    // a right click on them does nothing
    v->setForeground(Red);
    press(p, p->swapRect().center(), Qt::RightButton);
    CHECK(v->pen().fg == Color::indexed(Red));

    // the foreground draws over the background, top left
    const QImage img = p->grab().toImage();
    CHECK(img.pixelColor(p->foregroundRect().center()) == QColor(0xaa, 0x00, 0x00));
    CHECK(img.pixelColor(p->backgroundRect().bottomRight() - QPoint(3, 3)) == QColor(0, 0, 0));

    // the reset's tooltip names its shortcut
    w.show();
    QHelpEvent tip(QEvent::ToolTip, p->resetRect().center(), p->mapToGlobal(p->resetRect().center()));
    QApplication::sendEvent(p, &tip);
    CHECK(QToolTip::text().contains(keyText(act(w, QStringLiteral("defaultColours"))->shortcut())));
    QToolTip::hideText();
}

TEST_CASE("the Window menu lists every dock and hides and shows it") {
    MainWindow w;
    w.show();
    QMenu* menu = windowMenu(w);
    REQUIRE(menu);
    const QList<QDockWidget*> docks = w.docks();
    CHECK(docks.size() == 9);
    CHECK(w.findChildren<QDockWidget*>().size() == docks.size());
    for (QDockWidget* d : docks) {
        INFO(d->objectName().toStdString());
        QAction* a = act(w, QStringLiteral("show_") + d->objectName());
        CHECK(menu->actions().contains(a));
        CHECK(a->isCheckable());
        CHECK(a->isChecked());
        // the preview, the palette and the Library are always shown; the
        // Layers dock can close like the toolbox
        const bool closable = d->features().testFlag(QDockWidget::DockWidgetClosable);
        CHECK(closable == (d != w.previewDock() && d != w.paletteDock() && d != w.libraryDock()));
        CHECK(a->isEnabled() == closable);
        if (!closable)
            continue;
        a->trigger();
        CHECK_FALSE(d->isVisible());
        CHECK_FALSE(a->isChecked());
        a->trigger();
        CHECK(d->isVisible());
    }
    // closing a dock unchecks its item
    w.glyphBarDock()->close();
    CHECK_FALSE(act(w, QStringLiteral("show_glyphBarDock"))->isChecked());
    CHECK(menu->actions().contains(act(w, QStringLiteral("resetLayout"))));
    CHECK(act(w, QStringLiteral("resetLayout"))->shortcut().isEmpty());
}

TEST_CASE("Reset Layout puts every dock back, shown and docked") {
    MainWindow w;
    w.show();
    w.removeDockWidget(w.toolboxDock());
    w.addDockWidget(Qt::RightDockWidgetArea, w.toolboxDock());
    w.toolboxDock()->show();
    w.addDockWidget(Qt::TopDockWidgetArea, w.libraryDock());
    w.toolOptionsDock()->setFloating(true);
    w.glyphBarDock()->hide();
    w.layersDock()->setFloating(true);
    w.paletteDock()->hide();
    w.tabifyDockWidget(w.previewDock(), w.paletteDock());
    REQUIRE(w.dockWidgetArea(w.toolboxDock()) == Qt::RightDockWidgetArea);

    act(w, QStringLiteral("resetLayout"))->trigger();
    const std::pair<QDockWidget*, Qt::DockWidgetArea> want[] = {
        {w.toolboxDock(), Qt::LeftDockWidgetArea},  {w.toolOptionsDock(), Qt::LeftDockWidgetArea},
        {w.previewDock(), Qt::RightDockWidgetArea}, {w.paletteDock(), Qt::RightDockWidgetArea},
        {w.layersDock(), Qt::RightDockWidgetArea},  {w.libraryDock(), Qt::RightDockWidgetArea},
        {w.lintDock(), Qt::RightDockWidgetArea},    {w.ircViewDock(), Qt::RightDockWidgetArea},
        {w.glyphBarDock(), Qt::BottomDockWidgetArea},
    };
    for (const auto& [d, area] : want) {
        INFO(d->objectName().toStdString());
        CHECK(w.dockWidgetArea(d) == area);
        CHECK_FALSE(d->isHidden());
        CHECK_FALSE(d->isFloating());
        CHECK(act(w, QStringLiteral("show_") + d->objectName())->isChecked());
        // Lint and the IRC View share a place, as tabs, Lint in front
        if (d == w.lintDock())
            CHECK(w.tabifiedDockWidgets(d) == QList<QDockWidget*>{w.ircViewDock()});
        else if (d == w.ircViewDock())
            CHECK(w.tabifiedDockWidgets(d) == QList<QDockWidget*>{w.lintDock()});
        else
            CHECK(w.tabifiedDockWidgets(d).isEmpty());
    }
    QApplication::processEvents();
    CHECK(w.lintWidget()->shown());
    CHECK_FALSE(w.ircViewWidget()->shown());
    // the toolbox is above its options
    CHECK(w.toolboxDock()->geometry().bottom() < w.toolOptionsDock()->geometry().top());
}

TEST_CASE("the default window fits a 720-high screen including its frame", "[window-height]") {
    ClearLayout clear;
    bw::appSettings()->clear();
    const QSize screen = GENERATE(QSize(1280, 720), QSize(1366, 768));
    const bool reset = GENERATE(false, true);
    // Reserve 60 logical pixels for the title bar, borders and desktop panel.
    constexpr int frameAllowance = 60;
    const QSize client(screen.width(), screen.height() - frameAllowance);
    CAPTURE(screen.width(), screen.height(), reset);
    // The offscreen screen's size is fixed, so the startup rule is given
    // the screen's work area through the seam the constructor uses.
    const QSize start = MainWindow::initialSize(screen);
    CHECK(start == client);
    MainWindow w;
    w.show();
    QApplication::processEvents();
    CHECK(w.size() == MainWindow::initialSize(w.screen()->availableGeometry().size()));
    CHECK(w.height() <= w.screen()->availableGeometry().height() - frameAllowance);
    w.resize(start);
    QApplication::processEvents();
    if (reset) {
        act(w, QStringLiteral("resetLayout"))->trigger();
        QApplication::processEvents();
    }
    const int minimumHeight = w.minimumSizeHint().height();
    CAPTURE(minimumHeight, w.height());
    CHECK(minimumHeight <= 720 - frameAllowance);
    CHECK(w.height() <= client.height());
    CHECK(w.width() <= client.width());
    for (QDockWidget* d : w.docks()) {
        INFO(d->objectName().toStdString());
        CAPTURE(d->minimumSizeHint().height(), d->height());
        CHECK_FALSE(d->isHidden());
        CHECK_FALSE(d->isFloating());
        // The inactive IRC tab is deliberately placed offscreen by Qt.
        if (d != w.ircViewDock())
            CHECK(w.rect().contains(d->geometry()));
    }
}

TEST_CASE("short right docks scroll their whole contents into reach", "[window-height]") {
    ClearLayout clear;
    bw::appSettings()->clear();
    MainWindow w;
    w.resize(1280, 660);
    w.show();
    QApplication::processEvents();
    for (QDockWidget* d : {w.layersDock(), w.libraryDock(), w.lintDock(), w.ircViewDock()}) {
        INFO(d->objectName().toStdString());
        d->raise();
        w.resizeDocks({d}, {90}, Qt::Vertical);
        QApplication::processEvents();
        auto* scroll = qobject_cast<QScrollArea*>(d->widget());
        REQUIRE(scroll);
        REQUIRE(scroll->widget());
        QWidget* content = scroll->widget();
        CHECK(content->height() >= content->minimumSizeHint().height());
        CHECK(content->width() >= content->minimumSizeHint().width());
        QScrollBar* bar = scroll->verticalScrollBar();
        REQUIRE(bar->maximum() > 0);
        CHECK(bar->isVisible());
        bar->setValue(bar->maximum());
        QApplication::processEvents();
        const int bottom = content->mapTo(scroll->viewport(), content->rect().bottomLeft()).y();
        CHECK(bottom == scroll->viewport()->height() - 1);
        bar->setValue(bar->minimum());
        QApplication::processEvents();
        CHECK(content->mapTo(scroll->viewport(), QPoint()).y() == 0);
        CHECK(w.height() == 660);
    }
}

TEST_CASE("wrapped right panels need no horizontal scroll at the column's minimum width", "[window-height]") {
    ClearLayout clear;
    bw::appSettings()->clear();
    MainWindow w;
    w.resize(1024, 708); // a 1024x768 screen: the right column falls to its native minimum
    w.show();
    QApplication::processEvents();
    for (QDockWidget* d : {w.layersDock(), w.libraryDock(), w.lintDock()}) {
        INFO(d->objectName().toStdString());
        auto* scroll = qobject_cast<QScrollArea*>(d->widget());
        REQUIRE(scroll);
        CAPTURE(d->width(), scroll->viewport()->width(), scroll->widget()->minimumSizeHint().width());
        CHECK_FALSE(scroll->horizontalScrollBar()->isVisible());
        CHECK(scroll->widget()->width() <= scroll->viewport()->width());
    }
}

TEST_CASE("the right dock column can grow and shrink", "[dock-width]") {
    ClearLayout clear;
    MainWindow w;
    // Room for a 480-pixel column beside the left one and the canvas,
    // whatever size the screen gives the window.
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    for (int width : {480, 300}) {
        w.resizeDocks({w.libraryDock()}, {width}, Qt::Horizontal);
        QApplication::processEvents();
        for (QDockWidget* d : {w.previewDock(), w.paletteDock(), w.layersDock(), w.libraryDock(), w.lintDock()}) {
            INFO(d->objectName().toStdString());
            CHECK(d->width() == width);
        }
        w.ircViewDock()->raise();
        QApplication::processEvents();
        CHECK(w.ircViewDock()->width() == width);
        w.lintDock()->raise();
    }
}

TEST_CASE("the left dock column can grow and shrink", "[dock-width]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    for (int width : {320, 200}) {
        w.resizeDocks({w.toolboxDock()}, {width}, Qt::Horizontal);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == width);
        CHECK(w.toolOptionsDock()->width() == width);
    }
}

TEST_CASE("Reset Layout restores the default dock column widths", "[dock-width]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    checkDefaultWidths(w);
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {320, 480}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->width() == 320);
    REQUIRE(w.libraryDock()->width() == 480);
    act(w, QStringLiteral("resetLayout"))->trigger();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == kDefaultLeft);
    CHECK(w.toolOptionsDock()->width() == kDefaultLeft);
    CHECK(w.previewDock()->width() == 360);
    CHECK(w.paletteDock()->width() == 360);
    CHECK(w.layersDock()->width() == 360);
    CHECK(w.libraryDock()->width() == 360);
    CHECK(w.lintDock()->width() == 360);
}

TEST_CASE("default dock widths preserve room for the canvas", "[dock-width][dock-floor]") {
    ClearLayout clear;
    const QSize size = GENERATE(QSize(700, 500), QSize(1280, 720), QSize(1366, 768), QSize(1920, 1080));
    const bool reset = GENERATE(false, true);
    CAPTURE(size.width(), size.height(), reset);
    MainWindow w;
    if (reset) {
        // Reset must use the current size, rather than the size at first show.
        w.resize(1920, 1080);
        w.show();
        QApplication::processEvents();
        w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {320, 480}, Qt::Horizontal);
        QApplication::processEvents();
        w.resize(size);
        act(w, QStringLiteral("resetLayout"))->trigger();
    } else {
        w.resize(size);
        w.show();
    }
    QApplication::processEvents();
    auto* canvasScroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(canvasScroll);
    const int viewport = canvasScroll->viewport()->width();
    CAPTURE(viewport, w.toolboxDock()->width(), w.libraryDock()->width());
    if (viewport < 640) {
        // At 700px there isn't room even at the native dock minimums.
        CHECK(w.toolboxDock()->width() == w.toolboxDock()->minimumSizeHint().width());
        CHECK(w.libraryDock()->width() == rightColumnMinimum(w));
    } else {
        CHECK(viewport >= 640);
    }
    if (size.width() >= 1920) {
        CHECK(viewport >= 640);
        CHECK(w.toolboxDock()->width() == kDefaultLeft);
        CHECK(w.libraryDock()->width() == 360);
    } else if (size.width() == 1366) {
        // The left column gives back part of its width above 256.
        checkDefaultWidths(w);
        CHECK(w.toolboxDock()->width() < kDefaultLeft);
    } else if (size.width() == 1280) {
        // The left column's width above 256, then the right column's, can
        // surrender enough space for the floor.
        CHECK(viewport >= 640);
        CHECK(w.toolboxDock()->width() == kNarrowLeft);
        CHECK(w.libraryDock()->width() < 360);
    }
}

TEST_CASE("the first-run window width fits the available screen", "[dock-width][dock-screen]") {
    ClearLayout clear;
    MainWindow w;
    w.show();
    QApplication::processEvents();
    const QSize available = w.screen()->availableGeometry().size();
    CAPTURE(w.width(), available.width());
    CHECK(w.width() <= available.width());
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    if (scroll->viewport()->width() < 640) {
        CHECK(w.toolboxDock()->width() == w.toolboxDock()->minimumSizeHint().width());
        CHECK(w.libraryDock()->width() == rightColumnMinimum(w));
    }
}

TEST_CASE("first-run dock widths follow window sizing until a column is resized", "[dock-width][dock-settle]") {
    ClearLayout clear;
    MainWindow w;
    // Exercise both main()'s construct/show path and a larger starting window
    // followed by the WM fitting it to a 1280px work area after first show.
    const bool largeStart = GENERATE(false, true);
    CAPTURE(largeStart);
    if (largeStart)
        w.resize(1400, 900);
    w.show();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    for (QSize size : {QSize(1280, 720), QSize(1366, 768), QSize(1280, 720)}) {
        CAPTURE(size.width(), size.height());
        w.resize(size);
        QApplication::processEvents();
        CHECK(scroll->viewport()->width() >= 640);
        if (size.width() == 1280)
            CHECK(w.toolboxDock()->width() == kNarrowLeft);
        else
            checkDefaultWidths(w);
        CHECK(w.libraryDock()->width() >= w.paletteDock()->minimumSizeHint().width());
    }
    // Manual widths take precedence, even if the canvas then falls below
    // the default floor. Returning to an earlier size must keep them too.
    const QSize manual = GENERATE(QSize(300, 400), QSize(230, 280));
    CAPTURE(manual.width(), manual.height());
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {manual.width(), manual.height()}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->width() == manual.width());
    REQUIRE(w.libraryDock()->width() == manual.height());
    for (QSize size : {QSize(1200, 720), QSize(1366, 768), QSize(1280, 720)}) {
        w.resize(size);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == manual.width());
        CHECK(w.libraryDock()->width() == manual.height());
    }
}

TEST_CASE("first-run dock widths recover after a one-step window squeeze", "[dock-width][dock-squeeze]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->width() == kDefaultLeft);
    REQUIRE(w.libraryDock()->width() == 360);

    struct ResizeCounter : QObject {
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Resize)
                ++count;
            return false;
        }
    } counter;
    w.installEventFilter(&counter);
    for (QSize size : {QSize(600, 720), QSize(1920, 1080)}) {
        CAPTURE(size.width(), size.height());
        counter.count = 0;
        w.resize(size);
        QApplication::processEvents();
        CHECK(w.width() == size.width());
        CHECK(counter.count == 1);
    }
    checkDefaultWidths(w);
    toolAction(w, QStringLiteral("text"))->trigger();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.toolOptionsDock()->widget());
    REQUIRE(scroll);
    CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    CHECK_FALSE(scroll->horizontalScrollBar()->isVisible());
}

TEST_CASE("every tool's options fit the default left column without horizontal scrolling", "[dock-width][tool-options-width]") {
    ClearLayout clear;
    const QSize size = GENERATE(QSize(1280, 720), QSize(1366, 768), QSize(1920, 1080));
    const int points = GENERATE(9, 10);
    // With the help folded away the title and its toggle share a line.
    const bool help = GENERATE(true, false);
    UiFont font(points);
    CAPTURE(size.width(), size.height(), points, help);
    MainWindow w;
    w.resize(size);
    w.show();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.toolOptionsDock()->widget());
    REQUIRE(scroll);
    auto* toggle = w.findChild<QToolButton*>(QStringLiteral("toolHelpToggle"));
    REQUIRE(toggle);
    toggle->setChecked(help);
    // A short options panel also covers the vertical scrollbar taking space.
    w.resizeDocks({w.toolOptionsDock()}, {200}, Qt::Vertical);
    for (Tool* tool : w.tools()->tools()) {
        INFO(tool->id().toStdString());
        toolAction(w, tool->id())->trigger();
        QApplication::processEvents();
        CHECK(scroll->horizontalScrollBar()->maximum() == 0);
        CHECK_FALSE(scroll->horizontalScrollBar()->isVisible());
    }
}

TEST_CASE("resized dock column widths survive closing the window", "[dock-width]") {
    ClearLayout clear;
    {
        MainWindow w;
        // Fit the offscreen screen so restoreGeometry keeps the same size.
        w.resize(760, 740);
        w.show();
        QApplication::processEvents();
        w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {200, 300}, Qt::Horizontal);
        QApplication::processEvents();
        REQUIRE(w.toolboxDock()->width() == 200);
        REQUIRE(w.libraryDock()->width() == 300);
        REQUIRE(w.close());
    }
    MainWindow w;
    // Wide enough to leave the canvas its floor beside them, so the widths
    // stand as saved (#244).
    w.resize(1366, 768);
    w.show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 200);
    CHECK(w.toolOptionsDock()->width() == 200);
    CHECK(w.previewDock()->width() == 300);
    CHECK(w.paletteDock()->width() == 300);
    CHECK(w.layersDock()->width() == 300);
    CHECK(w.libraryDock()->width() == 300);
    CHECK(w.lintDock()->width() == 300);
    // A restored layout must never opt into the first-run sizing policy.
    for (QSize size : {QSize(1366, 768), QSize(1280, 720)}) {
        w.resize(size);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == 200);
        CHECK(w.libraryDock()->width() == 300);
    }
}

namespace {

// Saves the layout of a window `size` big whose columns are `left` and
// `right` wide, as closing it does.
void saveColumns(QSize size, int left, int right) {
    MainWindow w;
    w.resize(size);
    w.show();
    QApplication::processEvents();
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {left, right}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->width() == left);
    REQUIRE(w.libraryDock()->width() == right);
    REQUIRE(w.close());
}

int canvasViewport(MainWindow& w) { return qobject_cast<QScrollArea*>(w.centralWidget())->viewport()->width(); }

// The left column's native minimum width, as rightColumnMinimum's.
int leftColumnMinimum(MainWindow& w) {
    int minimum = 0;
    for (QDockWidget* d : w.docks())
        if (w.dockWidgetArea(d) == Qt::LeftDockWidgetArea && !d->isHidden() && !d->isFloating())
            minimum = std::max({minimum, d->minimumWidth(), d->minimumSizeHint().width()});
    return minimum;
}

// A restored layout that leaves the canvas at its floor gave up only what
// the floor needs: `left` and `right` shrank, each in proportion to its
// saved width. A column that reaches its minimum stops there and the other
// gives the rest.
void checkProportionalShrink(MainWindow& w, int left, int right) {
    const int viewport = canvasViewport(w);
    const int leftShrink = left - w.toolboxDock()->width();
    const int rightShrink = right - w.libraryDock()->width();
    const int leftRoom = left - leftColumnMinimum(w);
    const int rightRoom = right - rightColumnMinimum(w);
    CAPTURE(viewport, leftShrink, rightShrink, leftRoom, rightRoom);
    CHECK(viewport >= 640);
    // Not a pixel more than needed.
    CHECK(viewport <= 641);
    const int deficit = leftShrink + rightShrink;
    CHECK(deficit > 0);
    REQUIRE(deficit < leftRoom + rightRoom);
    const double share = std::clamp(double(deficit) * left / (left + right), double(deficit - rightRoom), double(leftRoom));
    CHECK(std::abs(leftShrink - share) <= 1.0);
    CHECK(leftShrink <= leftRoom);
    CHECK(rightShrink <= rightRoom);
    CHECK(w.toolOptionsDock()->width() == w.toolboxDock()->width());
    for (QDockWidget* d : {w.previewDock(), w.paletteDock(), w.layersDock(), w.lintDock()}) {
        INFO(d->objectName().toStdString());
        CHECK(d->width() == w.libraryDock()->width());
    }
}

} // namespace

TEST_CASE("a restored layout too wide for the canvas floor shrinks both columns in proportion",
          "[dock-width][dock-restore]") {
    ClearLayout clear;
    // A 360/360 layout saved from a wide window, and an uneven one, restored
    // on a 1280x720 screen: the saved widths would leave the canvas 484
    // pixels at 360/360 (#244).
    const auto [left, right] = GENERATE(std::pair(360, 360), std::pair(300, 420), std::pair(420, 300));
    CAPTURE(left, right);
    saveColumns({1920, 1080}, left, right);
    MainWindow w;
    w.resize(1280, 720);
    w.show();
    QApplication::processEvents();
    checkProportionalShrink(w, left, right);
    // The saved widths would have left the canvas under its floor.
    MainWindow sized;
    sized.resize(1280, 720);
    sized.show();
    QApplication::processEvents();
    sized.resizeDocks({sized.toolboxDock(), sized.libraryDock()}, {left, right}, Qt::Horizontal);
    QApplication::processEvents();
    CHECK(canvasViewport(sized) < 640);
    CHECK(w.toolboxDock()->width() >= leftColumnMinimum(w));
    CHECK(w.libraryDock()->width() >= rightColumnMinimum(w));
}

TEST_CASE("a restored layout that leaves the canvas its floor keeps its widths exactly", "[dock-width][dock-restore]") {
    ClearLayout clear;
    const auto [size, left, right] = GENERATE(std::tuple(QSize(1920, 1080), 360, 360), std::tuple(QSize(1366, 768), 300, 320),
                                              std::tuple(QSize(1280, 720), 200, 300));
    CAPTURE(size.width(), size.height(), left, right);
    saveColumns({1920, 1080}, left, right);
    MainWindow w;
    w.resize(size);
    w.show();
    QApplication::processEvents();
    CHECK(canvasViewport(w) >= 640);
    CHECK(w.toolboxDock()->width() == left);
    CHECK(w.toolOptionsDock()->width() == left);
    for (QDockWidget* d : {w.previewDock(), w.paletteDock(), w.layersDock(), w.libraryDock(), w.lintDock()}) {
        INFO(d->objectName().toStdString());
        CHECK(d->width() == right);
    }
}

TEST_CASE("a restored layout with no room for the canvas floor gives the canvas all the minimums leave",
          "[dock-width][dock-restore]") {
    ClearLayout clear;
    saveColumns({1920, 1080}, 360, 360);
    MainWindow w;
    // Under 640 for the canvas even with both columns at their minimums.
    w.resize(900, 600);
    w.show();
    QApplication::processEvents();
    CAPTURE(canvasViewport(w), w.toolboxDock()->width(), w.libraryDock()->width());
    CHECK(canvasViewport(w) < 640);
    CHECK(w.toolboxDock()->width() == leftColumnMinimum(w));
    CHECK(w.toolOptionsDock()->width() == leftColumnMinimum(w));
    CHECK(w.libraryDock()->width() == rightColumnMinimum(w));
    // Qt's own clamp agrees: neither column goes any narrower.
    CHECK(w.libraryDock()->width() == rightColumnFloor(w));
}

TEST_CASE("a window resized after its layout was restored keeps its columns", "[dock-width][dock-restore]") {
    ClearLayout clear;
    saveColumns({1920, 1080}, 360, 360);
    MainWindow w;
    w.resize(1280, 720);
    w.show();
    QApplication::processEvents();
    // The restore gave the canvas its floor...
    const int left = w.toolboxDock()->width();
    const int right = w.libraryDock()->width();
    CAPTURE(left, right);
    REQUIRE(canvasViewport(w) >= 640);
    REQUIRE(left < 360);
    REQUIRE(right < 360);
    // ...and a size the user gives the window later is no restore: the
    // columns stay as they are, wider or narrower than the canvas would like.
    for (QSize size : {QSize(1100, 720), QSize(1920, 1080), QSize(1280, 720)}) {
        CAPTURE(size.width(), size.height());
        w.resize(size);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == left);
        CHECK(w.libraryDock()->width() == right);
    }
    CHECK(canvasViewport(w) >= 640);
    w.resize(1100, 720);
    QApplication::processEvents();
    CHECK(canvasViewport(w) < 640);
}

TEST_CASE("a restored window sized again before it is on screen is still the restore", "[dock-width][dock-restore]") {
    ClearLayout clear;
    saveColumns({1920, 1080}, 360, 360);
    // The window manager fits or maximizes a window as it maps it, after
    // show() and before the window is exposed.
    const bool widens = GENERATE(false, true);
    CAPTURE(widens);
    MainWindow w;
    w.resize(widens ? QSize(1280, 720) : QSize(1920, 1080));
    w.show();
    REQUIRE_FALSE(w.windowHandle()->isExposed());
    w.resize(widens ? QSize(1920, 1080) : QSize(1280, 720));
    QApplication::processEvents();
    REQUIRE(w.windowHandle()->isExposed());
    CHECK(canvasViewport(w) >= 640);
    if (widens) {
        // The saved widths fit the final size: they come back exactly.
        CHECK(w.toolboxDock()->width() == 360);
        CHECK(w.libraryDock()->width() == 360);
    } else {
        CHECK(w.toolboxDock()->width() < 360);
        CHECK(w.libraryDock()->width() < 360);
        CHECK(canvasViewport(w) <= 641);
    }
}

TEST_CASE("a legacy narrow dock layout gives way to the default widths", "[dock-width]") {
    ClearLayout clear;
    {
        MainWindow w;
        w.resize(760, 740);
        w.show();
        QApplication::processEvents();
        const int narrowRight = rightColumnMinimum(w);
        w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {160, narrowRight}, Qt::Horizontal);
        w.glyphBarDock()->hide();
        QApplication::processEvents();
        REQUIRE(w.toolboxDock()->width() == 160);
        REQUIRE(w.libraryDock()->width() == narrowRight);
        // Version 3 is the layout before the column-width defaults. Its
        // narrow widths must not override the new first-run layout.
        const QByteArray legacy = w.saveState(3);
        REQUIRE(w.close());
        appSettings()->setValue(QStringLiteral("window/state"), legacy);
    }
    MainWindow w;
    // Give the migrated layout room for the full defaults.
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == kDefaultLeft);
    CHECK(w.toolOptionsDock()->width() == kDefaultLeft);
    CHECK(w.previewDock()->width() == 360);
    CHECK(w.paletteDock()->width() == 360);
    CHECK(w.layersDock()->width() == 360);
    CHECK(w.libraryDock()->width() == 360);
    CHECK(w.lintDock()->width() == 360);
    CHECK(w.glyphBarDock()->isVisible());
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {200, 300}, Qt::Horizontal);
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 200);
    CHECK(w.libraryDock()->width() == 300);
}

TEST_CASE("a closed dock doesn't set the right column's floor", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1100, 708);
    w.show();
    QApplication::processEvents();
    w.layersDock()->close();
    QApplication::processEvents();
    w.resize(1024, 708); // follows the defaults: the right column falls to its minimum
    QApplication::processEvents();
    const int after = w.libraryDock()->width();
    CHECK(after == rightColumnMinimum(w));
    CHECK(after == rightColumnFloor(w));
}

TEST_CASE("closing and reopening a dock re-applies the default widths", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1024, 708);
    w.show();
    QApplication::processEvents();
    const int withLayers = rightColumnMinimum(w);
    REQUIRE(w.libraryDock()->width() == withLayers);
    REQUIRE(w.layersDock()->minimumSizeHint().width() > w.paletteDock()->minimumSizeHint().width());

    // Closed, Layers gives its width to the canvas at once.
    w.layersDock()->close();
    QApplication::processEvents();
    const int withoutLayers = rightColumnMinimum(w);
    CAPTURE(withLayers, withoutLayers);
    CHECK(withoutLayers < withLayers);
    CHECK(w.libraryDock()->width() == withoutLayers);

    // Shown again, it brings its floor back, and the column still follows the window.
    w.layersDock()->show();
    QApplication::processEvents();
    CHECK(w.layersDock()->width() == withLayers);
    CHECK(w.libraryDock()->width() == withLayers);
    w.resize(1366, 708);
    QApplication::processEvents();
    checkDefaultWidths(w);
    w.resize(1024, 708);
    QApplication::processEvents();
    CHECK(w.libraryDock()->width() == withLayers);
    CHECK(w.libraryDock()->width() == rightColumnFloor(w));
}

TEST_CASE("Reset Layout brings a closed dock and its floor back", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1024, 708);
    w.show();
    QApplication::processEvents();
    const int withLayers = rightColumnMinimum(w);
    w.layersDock()->close();
    QApplication::processEvents();
    w.resize(1000, 708);
    QApplication::processEvents();
    REQUIRE(w.libraryDock()->width() < withLayers);
    w.resize(1024, 708);
    act(w, QStringLiteral("resetLayout"))->trigger();
    QApplication::processEvents();
    CHECK(w.layersDock()->isVisible());
    CHECK(w.layersDock()->width() == withLayers);
    CHECK(w.libraryDock()->width() == withLayers);
    CHECK(w.libraryDock()->width() == rightColumnFloor(w));
}

TEST_CASE("closing a dock keeps manually resized columns", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {230, 300}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.libraryDock()->width() == 300);
    for (bool open : {false, true}) {
        CAPTURE(open);
        w.layersDock()->setVisible(open);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == 230);
        CHECK(w.libraryDock()->width() == 300);
    }
}

TEST_CASE("closing Toolbox preserves the space-aware Tool Options width",
          "[dock-width][dock-closed]") {
    ClearLayout clear;
    bw::appSettings()->clear();
    MainWindow w;
    w.resize(1100, 708);
    w.show();
    QApplication::processEvents();

    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    const int left = w.toolOptionsDock()->width();
    REQUIRE(left < kNarrowLeft);
    REQUIRE(left > std::max(w.toolOptionsDock()->minimumWidth(),
                           w.toolOptionsDock()->minimumSizeHint().width()));
    REQUIRE(w.libraryDock()->width() == rightColumnMinimum(w));
    REQUIRE(scroll->viewport()->width() == 640);

    REQUIRE(w.toolboxDock()->close());
    QApplication::processEvents();

    CHECK(w.toolOptionsDock()->width() == left);
    CHECK(scroll->viewport()->width() == 640);
}

TEST_CASE("manual resizing after reopening a dock wins before queued defaults",
          "[dock-width][dock-closed]") {
    ClearLayout clear;
    bw::appSettings()->clear();
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();

    REQUIRE(w.layersDock()->close());
    QApplication::processEvents();

    w.layersDock()->show();
    // Deliberately resize before the queued dock-open callback runs.
    w.resizeDocks({w.toolboxDock(), w.libraryDock()},
                  {230, 300}, Qt::Horizontal);
    w.layout()->activate();
    REQUIRE(w.toolboxDock()->width() == 230);
    REQUIRE(w.libraryDock()->width() == 300);

    QApplication::processEvents();

    CHECK(w.toolboxDock()->width() == 230);
    CHECK(w.libraryDock()->width() == 300);
}

TEST_CASE("reopening Toolbox keeps following the window", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1100, 708);
    w.show();
    QApplication::processEvents();
    const int left = w.toolOptionsDock()->width();
    REQUIRE(w.toolboxDock()->close());
    QApplication::processEvents();
    w.toolboxDock()->show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == left);
    CHECK(w.toolOptionsDock()->width() == left);
    w.resize(1366, 708);
    QApplication::processEvents();
    checkDefaultWidths(w);
}

TEST_CASE("resizing the left column with Toolbox closed keeps its width", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->close());
    QApplication::processEvents();
    REQUIRE(w.toolOptionsDock()->width() >= kNarrowLeft);
    w.resizeDocks({w.toolOptionsDock()}, {230}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolOptionsDock()->width() == 230);
    w.resize(1300, 708);
    QApplication::processEvents();
    CHECK(w.toolOptionsDock()->width() == 230);
}

TEST_CASE("a drag of the options column with the Toolbox hidden is kept", "[dock-width][dock-follow]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    act(w, QStringLiteral("show_toolboxDock"))->trigger();
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->isHidden());
    const bool drag = GENERATE(false, true);
    CAPTURE(drag);
    if (drag)
        dragColumn(w, w.toolOptionsDock(), 200);
    else
        w.resizeDocks({w.toolOptionsDock()}, {200}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolOptionsDock()->width() == 200);
    w.resize(1280, 720);
    QApplication::processEvents();
    CHECK(w.toolOptionsDock()->width() == 200);
}

TEST_CASE("a drag of the right column with the Preview floating is kept", "[dock-width][dock-follow]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    w.previewDock()->setFloating(true);
    QApplication::processEvents();
    REQUIRE(w.previewDock()->isFloating());
    const bool drag = GENERATE(false, true);
    CAPTURE(drag);
    if (drag)
        dragColumn(w, w.libraryDock(), 300);
    else
        w.resizeDocks({w.libraryDock()}, {300}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.libraryDock()->width() == 300);
    w.resize(1280, 720);
    QApplication::processEvents();
    CHECK(w.libraryDock()->width() == 300);
}

TEST_CASE("dragging a column's separator keeps its width as the window changes", "[dock-width][dock-drag]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    checkDefaultWidths(w);
    const bool left = GENERATE(true, false);
    CAPTURE(left);
    QDockWidget* const d = left ? w.toolboxDock() : w.libraryDock();
    const int width = left ? 230 : 300;
    dragColumn(w, d, width);
    REQUIRE(d->width() == width);
    for (QSize size : {QSize(1300, 708), QSize(1400, 708)}) {
        CAPTURE(size.width());
        w.resize(size);
        QApplication::processEvents();
        CHECK(d->width() == width);
    }
}

TEST_CASE("a separator drag that leaves the column widths is no column resize", "[dock-width][dock-drag]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    // the separator between the Toolbox and Tool Options moves heights only
    const QRect box = w.toolboxDock()->geometry();
    const QRect options = w.toolOptionsDock()->geometry();
    const int leftWidth = box.width();
    const QPoint between(box.center().x(), (box.bottom() + options.top()) / 2 + 1);
    const QPoint lower = between + QPoint(0, 40);
    const auto send = [&w](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(at), w.mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&w, &e);
    };
    send(QEvent::MouseButtonPress, between, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, lower, Qt::NoButton, Qt::LeftButton);
    QApplication::processEvents();
    send(QEvent::MouseButtonRelease, lower, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->height() == box.height() + 40);
    REQUIRE(w.toolboxDock()->width() == leftWidth);
    w.resize(1024, 708);
    QApplication::processEvents();
    w.resize(1400, 708);
    QApplication::processEvents();
    checkDefaultWidths(w);
}

TEST_CASE("a column's displayed tab measures it, not one behind it", "[dock-width][dock-tabs]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    w.tabifyDockWidget(w.toolboxDock(), w.toolOptionsDock());
    w.toolOptionsDock()->raise();
    QApplication::processEvents();
    const bool drag = GENERATE(false, true);
    CAPTURE(drag);
    if (drag)
        dragColumn(w, w.toolOptionsDock(), 230);
    else
        w.resizeDocks({w.toolOptionsDock()}, {230}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolOptionsDock()->width() == 230);
    w.resize(1400, 708);
    QApplication::processEvents();
    CHECK(w.toolOptionsDock()->width() == 230);
}

TEST_CASE("choosing a tab is no column resize", "[dock-width][dock-tabs]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    w.tabifyDockWidget(w.toolboxDock(), w.toolOptionsDock());
    w.toolOptionsDock()->raise();
    QApplication::processEvents();
    w.resize(1100, 708);
    QApplication::processEvents();
    const int narrowed = w.toolOptionsDock()->width();
    REQUIRE(narrowed < kNarrowLeft);
    w.toolboxDock()->raise();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == narrowed);
    w.resize(1366, 708);
    QApplication::processEvents();
    checkDefaultWidths(w);
}

TEST_CASE("a window resize before queued defaults keeps following them", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1024, 708);
    w.show();
    QApplication::processEvents();
    const int withLayers = rightColumnMinimum(w);
    REQUIRE(w.layersDock()->close());
    QApplication::processEvents();
    w.layersDock()->show();
    w.layout()->activate();
    w.resize(1366, 708);
    QApplication::processEvents();
    checkDefaultWidths(w);
    w.resize(1024, 708);
    QApplication::processEvents();
    CHECK(w.libraryDock()->width() == withLayers);
}

TEST_CASE("reopening an empty column keeps a width set before queued defaults", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->close());
    REQUIRE(w.toolOptionsDock()->close());
    QApplication::processEvents();
    w.toolboxDock()->show();
    w.resizeDocks({w.toolboxDock()}, {230}, Qt::Horizontal);
    w.layout()->activate();
    REQUIRE(w.toolboxDock()->width() == 230);
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 230);
}

TEST_CASE("opening and closing a dock in one turn gives its width back", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1024, 708);
    w.show();
    QApplication::processEvents();
    REQUIRE(w.layersDock()->close());
    QApplication::processEvents();
    const int withoutLayers = w.libraryDock()->width();
    REQUIRE(withoutLayers == rightColumnMinimum(w));
    w.layersDock()->show();
    w.layout()->activate();
    w.layersDock()->hide();
    w.layout()->activate();
    QApplication::processEvents();
    CHECK(w.libraryDock()->width() == withoutLayers);
    w.resize(1366, 708);
    QApplication::processEvents();
    checkDefaultWidths(w);
}

TEST_CASE("Qt narrowing the other column for a wide dock is no column resize", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    REQUIRE(w.layersDock()->close());
    QApplication::processEvents();
    w.layersDock()->setMinimumWidth(1050);
    w.layersDock()->show();
    QApplication::processEvents();
    w.resize(2048, 708);
    QApplication::processEvents();
    // The left column gives back its width above 256 for the canvas floor.
    CHECK(w.toolboxDock()->width() >= kNarrowLeft);
    CHECK(w.toolboxDock()->width() < kDefaultLeft);
    CHECK(qobject_cast<QScrollArea*>(w.centralWidget())->viewport()->width() == 640);
    CHECK(w.libraryDock()->width() == 1050);
}

TEST_CASE("a column set to exactly a reopened dock's minimum is the user's", "[dock-width][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1366, 708);
    w.show();
    QApplication::processEvents();
    REQUIRE(w.layersDock()->close());
    QApplication::processEvents();
    w.resize(1024, 708);
    QApplication::processEvents();
    w.layersDock()->show();
    const int minimum = rightColumnMinimum(w);
    w.resizeDocks({w.libraryDock()}, {minimum}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.libraryDock()->width() == minimum);
    w.resize(1366, 708);
    QApplication::processEvents();
    CHECK(w.libraryDock()->width() == minimum);
}

// A window as large as a screen leaves after its frame and the desktop's
// panel, as the window-height tests reserve.
QSize windowOn(QSize screen) { return {screen.width(), screen.height() - 60}; }

TEST_CASE("Tool Options takes the left column below the Toolbox's rows", "[dock-height]") {
    ClearLayout clear;
    MainWindow w;
    const QSize screen = GENERATE(QSize(1920, 1080), QSize(2560, 1440));
    CAPTURE(screen.width(), screen.height());
    w.resize(windowOn(screen));
    w.show();
    QApplication::processEvents();
    act(w, QStringLiteral("resetLayout"))->trigger();
    QApplication::processEvents();
    QDockWidget* const toolbox = w.toolboxDock();
    QDockWidget* const options = w.toolOptionsDock();
    const int title = toolbox->height() - toolbox->widget()->height();
    CHECK(toolbox->height() == w.toolbox()->sizeHint().height() + title);
    CHECK(options->y() == toolbox->geometry().bottom() + 1 + separatorExtent(w));
    CHECK(options->geometry().bottom() == w.centralWidget()->geometry().bottom());
    CAPTURE(options->height(), columnHeight(w));
    CHECK(options->height() * 4 >= columnHeight(w) * 3);

    // A taller window gives all of its height to Tool Options.
    const int toolboxHeight = toolbox->height();
    const int optionsHeight = options->height();
    w.resize(w.width(), w.height() + 200);
    QApplication::processEvents();
    CHECK(toolbox->height() == toolboxHeight);
    CHECK(options->height() == optionsHeight + 200);
}

TEST_CASE("on a 768-high screen every Toolbox button shows above Tool Options at its minimum", "[dock-height]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(windowOn({1366, 768}));
    w.show();
    QApplication::processEvents();
    const bool reset = GENERATE(false, true);
    CAPTURE(reset);
    if (reset) {
        act(w, QStringLiteral("resetLayout"))->trigger();
        QApplication::processEvents();
    }
    Toolbox* const box = w.toolbox();
    CHECK(box->height() >= box->sizeHint().height());
    for (QToolButton* b : box->buttons()) {
        INFO(b->objectName().toStdString());
        CHECK(b->isVisible());
        CHECK(box->rect().contains(b->geometry()));
    }
    QDockWidget* const options = w.toolOptionsDock();
    CHECK(options->height() >= std::max(options->minimumHeight(), options->minimumSizeHint().height()));
    CHECK(options->height() > w.toolboxDock()->height());
}

TEST_CASE("a narrower left column gives the Toolbox the rows it needs", "[dock-height]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(windowOn({1920, 1080}));
    w.show();
    QApplication::processEvents();
    const int rows = w.toolbox()->sizeHint().height();
    w.resizeDocks({w.toolboxDock()}, {w.toolbox()->minimumSizeHint().width()}, Qt::Horizontal);
    QApplication::processEvents();
    QApplication::processEvents();
    REQUIRE(w.toolbox()->sizeHint().height() > rows);
    CHECK(w.toolbox()->height() == w.toolbox()->sizeHint().height());
    CHECK(w.toolboxDock()->geometry().bottom() + 1 + separatorExtent(w) == w.toolOptionsDock()->y());
    for (QToolButton* b : w.toolbox()->buttons())
        CHECK(w.toolbox()->rect().contains(b->geometry()));
}

TEST_CASE("the right column's docks open at their default heights", "[dock-height]") {
    ClearLayout clear;
    // The bundled Noto Sans at the test platform's 9 pt, not whatever font the
    // machine defaults to: the targets below (Library and Lint give way at
    // 1080p) hold for the font the panels are sized for.
    UiFont font(9);
    MainWindow w;
    const QSize screen = GENERATE(QSize(1366, 768), QSize(1920, 1080), QSize(2560, 1440));
    const bool reset = GENERATE(false, true);
    CAPTURE(screen.width(), screen.height(), reset);
    w.resize(windowOn(screen));
    w.show();
    QApplication::processEvents();
    if (reset) {
        act(w, QStringLiteral("resetLayout"))->trigger();
        QApplication::processEvents();
    }
    QDockWidget* const preview = w.previewDock();
    QDockWidget* const palette = w.paletteDock();
    QDockWidget* const layers = w.layersDock();
    QDockWidget* const library = w.libraryDock();
    QDockWidget* const lint = w.lintDock();
    const auto minimum = [](QDockWidget* d) { return std::max(d->minimumHeight(), d->minimumSizeHint().height()); };
    constexpr int tolerance = 2;

    // Top to bottom, filling the column's height with nothing between.
    const int separator = separatorExtent(w);
    const QRect band = w.centralWidget()->geometry();
    CHECK(preview->y() == band.top());
    CHECK(palette->y() == preview->geometry().bottom() + 1 + separator);
    CHECK(layers->y() == palette->geometry().bottom() + 1 + separator);
    CHECK(library->y() == layers->geometry().bottom() + 1 + separator);
    CHECK(lint->y() == library->geometry().bottom() + 1 + separator);
    // Lint shares its slot with the IRC View's tab.
    const int tabs = band.bottom() - lint->geometry().bottom();
    CHECK(tabs > 0);

    // The palette and the glyph bar take their hints, no more.
    CHECK(palette->height() == palette->sizeHint().height());
    CHECK(w.glyphBarDock()->height() == w.glyphBarDock()->sizeHint().height());

    // The preview shows an 80x25 piece at 1x in up to a third of the column;
    // Layers its rows and buttons; Lint its rows.
    const int available = band.height() - 4 * separator;
    // The piece's height in whole device pixels, as the preview draws it
    // (DeviceGrid), so the target holds at a fractional device pixel ratio.
    const QSize cell = w.renderer().cellSize();
    const QSize piece = DeviceGrid::of(w.preview()).widgetSize({80 * cell.width(), 25 * cell.height()});
    const auto* previewScroll = static_cast<QScrollArea*>(preview->widget());
    const int frame = 2 * previewScroll->frameWidth();
    const int previewTitle = preview->height() - previewScroll->height();
    const int previewScrollBar =
        piece.width() + frame > preview->width() ? previewScroll->horizontalScrollBar()->sizeHint().height() : 0;
    const int previewTarget = std::min(previewTitle + piece.height() + frame + previewScrollBar, available / 3);
    const int layersTarget = panelTarget(layers);
    const int lintTarget = panelTarget(lint);
    CAPTURE(available, previewTarget, layersTarget, lintTarget);
    CAPTURE(preview->height(), layers->height(), library->height(), lint->height());
    const int others = palette->height() + previewTarget + layersTarget + lintTarget + tabs;
    if (available - others >= minimum(library)) {
        // Room for every target: the Library takes the rest.
        CHECK(std::abs(preview->height() - previewTarget) <= tolerance);
        CHECK(std::abs(layers->height() - layersTarget) <= tolerance);
        CHECK(std::abs(lint->height() - lintTarget) <= tolerance);
        CHECK(std::abs(library->height() - (available - others)) <= tolerance);
        CHECK(layers->widget()->findChild<QScrollBar*>()->maximum() == 0);
    } else {
        // Short of room, the Library gives way first, then Lint, then the
        // preview, each down to its minimum; Layers only when they all have.
        CHECK(library->height() == minimum(library));
        const int lintRoom = available - palette->height() - previewTarget - layersTarget - tabs - minimum(library);
        if (lintRoom >= minimum(lint)) {
            CHECK(std::abs(lint->height() - lintRoom) <= tolerance);
            CHECK(std::abs(preview->height() - previewTarget) <= tolerance);
            CHECK(std::abs(layers->height() - layersTarget) <= tolerance);
        } else {
            CHECK(lint->height() == minimum(lint));
            CHECK(preview->height() <= previewTarget);
            CHECK(preview->height() >= minimum(preview));
            CHECK(layers->height() >= minimum(layers));
            if (preview->height() > minimum(preview))
                CHECK(std::abs(layers->height() - layersTarget) <= tolerance);
        }
    }
    if (screen == QSize(1920, 1080)) {
#ifdef Q_OS_MACOS
        // A Mac's shorter menu bar leaves every target room at 1080p, which
        // the checks above hold it to.
        return;
#endif
        // The audit's numbers at 1080p: Library and Lint give way.
        CHECK(std::abs(preview->height() - previewTarget) <= tolerance);
        CHECK(library->height() == minimum(library));
        CHECK(lint->height() < lintTarget);
    } else if (screen == QSize(2560, 1440)) {
        CHECK(library->height() > minimum(library));
    }
}

TEST_CASE("a separator drag in a column keeps its heights until Reset Layout", "[dock-height]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(windowOn({2560, 1440}));
    w.show();
    QApplication::processEvents();
    const bool left = GENERATE(true, false);
    CAPTURE(left);
    QDockWidget* const upper = left ? w.toolboxDock() : w.layersDock();
    QDockWidget* const other = left ? w.layersDock() : w.toolboxDock();
    const int defaultHeight = upper->height();
    const int otherHeight = other->height();
    dragSeparatorBelow(w, upper, 60);
    const int dragged = upper->height();
    CAPTURE(defaultHeight, dragged);
    REQUIRE(dragged >= defaultHeight + 50);

    // The dragged column isn't set back to its defaults as the window
    // changes; the other column keeps following them.
    for (const int dy : {-100, 100}) {
        w.resize(w.width(), w.height() + dy);
        QApplication::processEvents();
        CHECK(upper->height() > defaultHeight + 20);
        if (left)
            CHECK(upper->height() == dragged);
        CHECK(other->height() == otherHeight);
    }
    act(w, QStringLiteral("resetLayout"))->trigger();
    QApplication::processEvents();
    CHECK(upper->height() == defaultHeight);
    w.resize(w.width(), w.height() + 100);
    QApplication::processEvents();
    CHECK(upper->height() == defaultHeight);
}

TEST_CASE("a vertical resizeDocks call is the user's height for that column", "[dock-height]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(windowOn({2560, 1440}));
    w.show();
    QApplication::processEvents();
    const int defaultHeight = w.layersDock()->height();
    w.resizeDocks({w.layersDock()}, {defaultHeight + 60}, Qt::Vertical);
    QApplication::processEvents();
    const int set = w.layersDock()->height();
    REQUIRE(set > defaultHeight + 30);
    w.resize(w.width(), w.height() + 100);
    QApplication::processEvents();
    CHECK(w.layersDock()->height() >= set);
    // Closing and opening a dock re-applies the heights only where they
    // are still followed.
    const int options = w.toolOptionsDock()->height();
    w.lintDock()->close();
    QApplication::processEvents();
    QApplication::processEvents();
    CHECK(w.layersDock()->height() >= set);
    CHECK(w.toolOptionsDock()->height() == options);
}

TEST_CASE("closing and reopening a right dock re-applies the default heights", "[dock-height][dock-closed]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(windowOn({2560, 1440}));
    w.show();
    QApplication::processEvents();
    const int preview = w.previewDock()->height();
    const int lint = w.lintDock()->height();
    const int library = w.libraryDock()->height();
    w.layersDock()->close();
    QApplication::processEvents();
    QApplication::processEvents();
    // Layers' height goes to the Library; the others keep theirs, the
    // preview's third of a column with one separator fewer.
    CHECK(std::abs(w.previewDock()->height() - preview) <= 2);
    CHECK(w.lintDock()->height() == lint);
    CHECK(w.libraryDock()->height() > library);
    w.layersDock()->show();
    QApplication::processEvents();
    QApplication::processEvents();
    CHECK(w.previewDock()->height() == preview);
    CHECK(w.lintDock()->height() == lint);
    CHECK(w.libraryDock()->height() == library);
}

TEST_CASE("the app's settings are an INI file in the test's scratch directory") {
    // Before any window here is built or any setting cleared. QSettings::setPath()
    // left macOS and Windows on the real settings; the factory doesn't.
    const auto s = bw::appSettings();
    CHECK(s->format() == QSettings::IniFormat);
    CHECK(s->fileName() == bwtest::scratchSettingsFile());
    CHECK(QFileInfo(s->fileName()).absolutePath() == scratchDir());
    CHECK(scratchDir() != QDir::homePath());
    CHECK(bwtest::settingsInScratch(scratchDir()));
}

TEST_CASE("Qt's own settings are in the test's scratch directory too") {
    // A QFileDialog reads and writes QtProject's native settings, outside the
    // factory. Where QSettings::setPath() can move them, the test main put
    // them in the scratch directory; elsewhere no test may build a QFileDialog.
    if (!bwtest::kQtSettingsRedirectable)
        SKIP("Qt's native settings can't be moved off the user's on this platform");
    const QSettings qt(QSettings::UserScope, QStringLiteral("QtProject"));
    CHECK(bwtest::inDir(qt.fileName(), scratchDir()));
    CHECK(bwtest::qtSettingsInScratch(scratchDir()));
}

TEST_CASE("the dock layout survives closing the window") {
    ClearLayout clear;
    {
        MainWindow w;
        w.show();
        w.addDockWidget(Qt::RightDockWidgetArea, w.toolboxDock());
        w.glyphBarDock()->hide();
        REQUIRE(w.close());
    }
    {
        // the window kept it in the scratch INI file
        QSettings ini(bwtest::scratchSettingsFile(), QSettings::IniFormat);
        CHECK(ini.contains(QStringLiteral("window/state")));
        CHECK(ini.contains(QStringLiteral("window/geometry")));
    }
    {
        MainWindow w;
        CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::RightDockWidgetArea);
        CHECK_FALSE(w.glyphBarDock()->isVisibleTo(&w));
        CHECK_FALSE(act(w, QStringLiteral("show_glyphBarDock"))->isChecked());
        CHECK(w.toolOptionsDock()->isVisibleTo(&w));
        w.show();
        act(w, QStringLiteral("resetLayout"))->trigger();
        CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::LeftDockWidgetArea);
        CHECK(w.glyphBarDock()->isVisible());
        w.close();
    }
    {
        // the reset layout is what the next window starts with
        MainWindow w;
        CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::LeftDockWidgetArea);
        CHECK(w.glyphBarDock()->isVisibleTo(&w));
    }
}

TEST_CASE("FlowRow puts settings side by side while they fit and wraps them when they don't", "[flow-row]") {
    QWidget host;
    auto* row = new FlowRow(&host);
    row->setSpacing(6);
    auto* first = new QLineEdit;
    auto* second = new QLineEdit;
    auto* firstLabel = new QLabel(QStringLiteral("First"));
    auto* secondLabel = new QLabel(QStringLiteral("Second"));
    row->addPair(firstLabel, first);
    row->addPair(secondLabel, second);
    CHECK(firstLabel->buddy() == first);
    CHECK(secondLabel->buddy() == second);
    host.show();
    QApplication::processEvents();

    const int line = row->heightForWidth(row->sizeHint().width());
    CHECK(line == row->sizeHint().height());
    host.resize(row->sizeHint().width() + 100, line);
    QApplication::processEvents();
    CHECK(first->y() == second->y());
    CHECK(second->x() > first->x() + first->width());
    // the spare width goes to the fields
    CHECK(second->geometry().right() == host.width() - 1);

    const int narrow = row->minimumSize().width();
    CHECK(narrow < row->sizeHint().width());
    CHECK(row->heightForWidth(narrow) == 2 * line + 6);
    host.resize(narrow, 2 * line + 6);
    QApplication::processEvents();
    CHECK(second->y() >= first->y() + first->height());
    // each fills the row
    CHECK(first->geometry().right() == host.width() - 1);
    CHECK(second->geometry().right() == host.width() - 1);

    // a hidden setting takes no room
    secondLabel->hide();
    second->hide();
    CHECK(row->heightForWidth(narrow) == line);
}

TEST_CASE("FlowRow keeps an item at its minimum width when the row is narrower", "[flow-row]") {
    QWidget host;
    auto* row = new FlowRow(&host);
    auto* wide = new QLineEdit;
    wide->setMinimumWidth(200);
    row->addWidget(wide);
    host.show();
    // Narrower than the item's minimum: the item keeps it rather than being
    // squeezed to the row.
    row->setGeometry(QRect(0, 0, 120, 40));
    CHECK(wide->width() == 200);
    row->setGeometry(QRect(0, 0, 400, 40));
    CHECK(wide->width() == 400);
}

TEST_CASE("Tool Options pairs settings on a line where the column is wide enough", "[tool-options-height]") {
    ClearLayout clear;
    const int points = GENERATE(9, 10);
    UiFont font(points);
    CAPTURE(points);
    MainWindow w;
    w.resize(windowOn({1920, 1080}));
    w.show();
    QApplication::processEvents();
    const auto sameLine = [&w](const char* tool, const char* a, const char* b) {
        w.tools()->setActive(QString::fromLatin1(tool));
        QApplication::processEvents();
        auto* first = w.findChild<QWidget*>(QString::fromLatin1(a));
        auto* second = w.findChild<QWidget*>(QString::fromLatin1(b));
        REQUIRE(first);
        REQUIRE(second);
        CAPTURE(tool, a, b);
        CHECK(first->isVisible());
        CHECK(second->isVisible());
        // On one line: each one's middle is within the other's height (a
        // line centres its settings, which can round a pixel apart).
        const QRect ra(first->mapTo(&w, QPoint()), first->size()), rb(second->mapTo(&w, QPoint()), second->size());
        CAPTURE(ra.top(), ra.bottom(), rb.top(), rb.bottom());
        CHECK((ra.top() <= rb.center().y() && rb.center().y() <= ra.bottom()));
        CHECK((rb.top() <= ra.center().y() && ra.center().y() <= rb.bottom()));
    };
    sameLine("form", "brushSize", "brushTip");
    sameLine("form", "brushLightDirection", "brushForm");
    sameLine("form", "brushRim", "brushSpecular");
    sameLine("texture", "brushTextureDensity", "brushTextureSeed");
    sameLine("spray", "sprayDensity", "spraySeed");
    sameLine("gradient", "gradientShape", "gradientBanding");
    sameLine("gradient", "gradientFrom", "gradientTo");
    sameLine("outline", "brushOutlineSelection", "brushAlphaLock");
    sameLine("text", "textBigStyle", "textBigColours");
    sameLine("text", "textColour", "textCase");
    sameLine("text", "textWrapWidth", "textAlign");
    sameLine("text", "textLetterSpacing", "textLineSpacing");
    sameLine("text", "faceSearch", "faceType");

    // A setting a tool doesn't have hides with its label, wherever it sits.
    for (Tool* t : w.tools()->tools()) {
        w.tools()->setActive(t);
        QApplication::processEvents();
        for (QLabel* label : w.toolOptionsDock()->findChildren<QLabel*>())
            if (label->buddy() && label->buddy()->window() == &w) {
                CAPTURE(t->id().toStdString(), label->text().toStdString());
                CHECK(label->isVisible() == label->buddy()->isVisible());
            }
    }
}

// The rows Tool Options shows for the active tool, after any other tool:
// each FlowRow gets at least its height for its width, and no two settings
// on the same panel overlap (#236: the Text tool's pairs drew over each
// other after another tool at the 1366×768 column). Returns what's wrong.
QStringList optionsLaidOutWrong(MainWindow& w) {
    QStringList wrong;
    auto* options = w.findChild<ToolOptions*>();
    QList<QWidget*> panels{options};
    for (QWidget* page : options->findChildren<QWidget*>())
        if (page->layout() && page->isVisible())
            panels.append(page);
    for (QWidget* panel : panels) {
        const QString name = panel->objectName();
        for (QLayout* any : panel->layout()->findChildren<QLayout*>()) {
            auto* row = dynamic_cast<FlowRow*>(any);
            if (!row || row->isEmpty())
                continue;
            const QRect g = row->geometry();
            if (g.height() < row->heightForWidth(g.width()))
                wrong << QStringLiteral("%1: row at y %2 is %3 tall, needs %4 at width %5")
                             .arg(name).arg(g.y()).arg(g.height()).arg(row->heightForWidth(g.width())).arg(g.width());
        }
        QList<QWidget*> shown;
        for (QWidget* child : panel->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
            if (child->isVisible() && !child->isWindow())
                shown.append(child);
        for (int i = 0; i < shown.size(); ++i)
            for (int j = i + 1; j < shown.size(); ++j)
                if (shown[i]->geometry().intersects(shown[j]->geometry()))
                    wrong << QStringLiteral("%1: %2 (y %3) overlaps %4 (y %5)")
                                 .arg(name, shown[i]->objectName()).arg(shown[i]->y())
                                 .arg(shown[j]->objectName()).arg(shown[j]->y());
    }
    return wrong;
}

// Counts the layout passes asked of a widget and the widgets in it.
struct LayoutRequests : QObject {
    QWidget* within;
    int count = 0;
    explicit LayoutRequests(QWidget* w) : within(w) { qApp->installEventFilter(this); }
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::LayoutRequest && o->isWidgetType()
            && (o == within || within->isAncestorOf(static_cast<QWidget*>(o))))
            ++count;
        return false;
    }
};

TEST_CASE("every tool's settings lie out without overlapping after any other tool", "[tool-options-height]") {
    ClearLayout clear;
    const QSize size = GENERATE(QSize(1280, 720), QSize(1366, 768));
    const int points = GENERATE(9, 10);
    UiFont font(points);
    CAPTURE(size.width(), points);
    MainWindow w;
    w.resize(size);
    w.show();
    QApplication::processEvents();
    auto* options = w.findChild<ToolOptions*>();
    REQUIRE(options);
    const auto open = [&w](Tool* t) {
        w.tools()->setActive(t);
        QApplication::processEvents();
        QApplication::processEvents();
    };
    // Each tool opened first, then after every other tool.
    const QList<Tool*> tools = w.tools()->tools();
    for (Tool* t : tools) {
        open(t);
        CAPTURE(t->id().toStdString());
        CHECK(optionsLaidOutWrong(w).join(QLatin1Char('\n')).toStdString() == "");
    }
    for (Tool* before : tools)
        for (Tool* after : tools) {
            if (before == after)
                continue;
            open(before);
            open(after);
            CAPTURE(before->id().toStdString(), after->id().toStdString());
            CHECK(optionsLaidOutWrong(w).join(QLatin1Char('\n')).toStdString() == "");
            // Laid out, the panel settles: within a few rounds of events a
            // round asks for no more layout passes.
            LayoutRequests requests(options);
            int rounds = 0;
            for (int asked = -1; asked != requests.count && rounds < 5; ++rounds) {
                asked = requests.count;
                QApplication::processEvents();
            }
            CHECK(rounds < 5);
        }
}

TEST_CASE("a saved layout that hid a dock that can't close still shows it") {
    ClearLayout clear;
    {
        MainWindow w;
        w.show();
        w.previewDock()->hide();
        w.paletteDock()->hide();
        REQUIRE(w.close());
    }
    MainWindow w;
    CHECK(w.previewDock()->isVisibleTo(&w));
    CHECK(w.paletteDock()->isVisibleTo(&w));
}

TEST_CASE("Tool Options shows each tool's settings and help without scrolling where the column is tall enough",
          "[tool-options-height]") {
    ClearLayout clear;
    const QSize screen = GENERATE(QSize(1920, 1080), QSize(2560, 1440));
    // The test platform's 9 pt, and the 10 pt a desktop gives by default.
    const int points = GENERATE(9, 10);
    UiFont font(points);
    MainWindow w;
    w.resize(windowOn(screen));
    w.show();
    QApplication::processEvents();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.toolOptionsDock()->widget());
    REQUIRE(scroll);
    auto* help = w.findChild<QLabel*>(QStringLiteral("toolHelp"));
    REQUIRE(help);
    const auto check = [&](Tool* t) {
        w.tools()->setActive(t);
        QApplication::processEvents();
        const int overflow = scroll->verticalScrollBar()->maximum();
        CAPTURE(screen.width(), points, t->id().toStdString(), overflow);
        CHECK(overflow == 0);
        // The help is laid out at its height for the column's width, not cut
        // off at its preferred height.
        if (help->isVisible())
            CHECK(help->height() >= help->heightForWidth(help->width()));
    };
    // Every panel fits from 1080 up, the longest help included (#236).
    for (Tool* t : w.tools()->tools())
        check(t);
    // From the Text tool's tall settings to the Gradient's long help.
    check(w.tools()->find(QStringLiteral("text")));
    check(w.tools()->find(QStringLiteral("gradient")));
}

// #238: shrinking a full-size window leaves the canvas at its floor, as a
// fresh window of the shrunk size gets, without waiting for another resize.
// tests/CMakeLists.txt runs it at fractional scales too, where the canvas's
// vertical scroll bar used to appear only after the columns were sized.
TEST_CASE("shrinking a full-size window keeps the canvas at its floor", "[canvas-floor-shrink]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1020);
    w.show();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    const int width = GENERATE(1280, 1300, 1366, 1400, 1440);
    const QSize shrunk(width, 708);
    CAPTURE(shrunk.width(), shrunk.height());
    w.resize(shrunk);
    QApplication::processEvents();
    CAPTURE(w.toolboxDock()->width(), w.libraryDock()->width());
    CHECK(scroll->viewport()->width() >= 640);
    // The same columns and canvas as a window opened at that size.
    MainWindow fresh;
    fresh.resize(shrunk);
    fresh.show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == fresh.toolboxDock()->width());
    CHECK(w.libraryDock()->width() == fresh.libraryDock()->width());
    CHECK(scroll->viewport()->width() == qobject_cast<QScrollArea*>(fresh.centralWidget())->viewport()->width());
}

TEST_CASE("narrowing a full-size window keeps the canvas at its floor", "[canvas-floor-shrink]") {
    // Only the width changes: the narrower canvas brings the horizontal bar,
    // and that bar's height then brings the vertical one (#238, at scale 1.25).
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1160);
    w.show();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    const int width = GENERATE(1280, 1366, 1440);
    const QSize narrowed(width, 1160);
    CAPTURE(narrowed.width(), narrowed.height());
    w.resize(narrowed);
    QApplication::processEvents();
    CAPTURE(w.toolboxDock()->width(), w.libraryDock()->width());
    CHECK(scroll->viewport()->width() >= 640);
    MainWindow fresh;
    fresh.resize(narrowed);
    fresh.show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == fresh.toolboxDock()->width());
    CHECK(w.libraryDock()->width() == fresh.libraryDock()->width());
    CHECK(scroll->viewport()->width() == qobject_cast<QScrollArea*>(fresh.centralWidget())->viewport()->width());
}


