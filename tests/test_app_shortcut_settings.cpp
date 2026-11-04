// Edit > Preferences > Shortcuts (#149): an action can be given another
// key or none, kept in the settings; a key keyboard mode needs, or one the
// desktop takes, is refused; the tooltips that name a key follow it; and a
// middle drag pans the canvas. main() is in test_app.cpp.

#include "canvas_view.h"
#include "main_window.h"
#include "preferences.h"
#include "settings.h"
#include "shortcut_settings.h"
#include "shortcuts_dialog.h"

#include "system_input.h"

#include "blockwright/canvas.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

using namespace bw;

namespace {

// The settings keep no shortcuts before or after a test.
struct ShortcutsCleared {
    ShortcutsCleared() { clear(); }
    ~ShortcutsCleared() { clear(); }
    static void clear() { appSettings()->remove(QLatin1String(kShortcutsGroup)); }
};

QAction* action(QWidget& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

QKeySequence key(QKeyCombination c) { return QKeySequence(c); }

QString kept(const char* name) {
    return appSettings()->value(QLatin1String(kShortcutsGroup) + QLatin1Char('/') + QLatin1String(name)).toString();
}
bool isKept(const char* name) {
    return appSettings()->contains(QLatin1String(kShortcutsGroup) + QLatin1Char('/') + QLatin1String(name));
}
void keep(const char* name, const QString& keys) {
    appSettings()->setValue(QLatin1String(kShortcutsGroup) + QLatin1Char('/') + QLatin1String(name), keys);
}

// The actions the settings keep keys for, sorted.
QStringList keptNames() {
    const auto settings = appSettings();
    settings->beginGroup(QLatin1String(kShortcutsGroup));
    QStringList names = settings->childKeys();
    names.sort();
    return names;
}

// A key no action of `w` has, and that may be one.
QKeySequence freeKey(QWidget& w, int nth = 0) {
    for (int f = Qt::Key_F11; f <= Qt::Key_F24; ++f) {
        const QKeySequence k(Qt::CTRL | Qt::SHIFT | Qt::Key(f));
        bool taken = false;
        for (QAction* a : windowActions(&w))
            taken |= a->shortcuts().contains(k);
        if (!taken && shortcutRefusal(k).isEmpty() && nth-- == 0)
            return k;
    }
    FAIL("no free key");
    return {};
}

QTreeWidgetItem* rowOf(const ShortcutsEditor& e, const QString& name) {
    for (int i = 0; i < e.tree()->topLevelItemCount(); ++i) {
        QTreeWidgetItem* g = e.tree()->topLevelItem(i);
        for (int j = 0; j < g->childCount(); ++j)
            if (g->child(j)->text(0) == name)
                return g->child(j);
    }
    return nullptr;
}

QString native(const QKeySequence& k) { return k.toString(QKeySequence::NativeText); }

// A mouse event for `v` at `global`: where the pointer is on the screen,
// which a pan doesn't move, though it moves the view under it.
void mouse(QWidget* v, QEvent::Type type, QPointF global, Qt::MouseButton button, Qt::MouseButtons held) {
    QMouseEvent e(type, v->mapFromGlobal(global), global, button, held, Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

// The global point at the middle of `cell` in `v`.
QPointF globalAt(CanvasView* v, QPoint cell) { return v->mapToGlobal(v->toWidget(v->cellRect(cell)).center()); }

// A tablet event of the pen's tip at `cell`.
void tablet(CanvasView* v, QEvent::Type type, QPoint cell) {
    const QPointF at = v->toWidget(v->cellRect(cell)).center();
    QTabletEvent e(type, bwtest::systemStylus(), at, v->mapToGlobal(at), type == QEvent::TabletRelease ? 0.0 : 1.0, 0,
                   0, 0, 0, 0, Qt::NoModifier, type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                   type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
    e.setAccepted(false);
    QApplication::sendEvent(v, &e);
}

void sendKey(QWidget* w, int k, Qt::KeyboardModifiers mods, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, k, mods, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, mods, text);
    QApplication::sendEvent(w, &release);
}

} // namespace

TEST_CASE("a shortcut must leave keyboard mode and the desktop's keys alone", "[shortcuts][preferences]") {
    // can
    CHECK(shortcutRefusal(QKeySequence()).isEmpty());
    CHECK(shortcutRefusal(key(Qt::CTRL | Qt::Key_K)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::CTRL | Qt::SHIFT | Qt::Key_K)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::META | Qt::Key_K)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::CTRL | Qt::Key_BracketLeft)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::CTRL | Qt::ALT | Qt::Key_Left)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::CTRL | Qt::SHIFT | Qt::Key_F5)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::SHIFT | Qt::Key_F1)).isEmpty());  // What's This
    CHECK(shortcutRefusal(key(Qt::Key_F11)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::Key_F12)).isEmpty());
    CHECK(shortcutRefusal(key(Qt::SHIFT | Qt::Key_F12)).isEmpty());

    // can't: keyboard mode types or edits with them
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_X)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::SHIFT | Qt::Key_X)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::ALT | Qt::Key_X)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_Space)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_BracketLeft)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_Left)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_Delete)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_Escape)).isEmpty());
    // the glyph keys
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_F1)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::Key_F10)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::SHIFT | Qt::Key_F2)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::SHIFT | Qt::Key_F10)).isEmpty());
    // AltGr on Windows
    CHECK_FALSE(shortcutRefusal(key(Qt::CTRL | Qt::ALT | Qt::Key_X)).isEmpty());
    // the desktop's
    CHECK_FALSE(shortcutRefusal(key(Qt::ALT | Qt::Key_F4)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::CTRL | Qt::Key_F2)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::CTRL | Qt::ALT | Qt::Key_F1)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::CTRL | Qt::ALT | Qt::Key_Delete)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::CTRL | Qt::ALT | Qt::Key_Backspace)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Delete)).isEmpty());
    // not one key
    CHECK_FALSE(shortcutRefusal(QKeySequence(Qt::CTRL | Qt::Key_K, Qt::CTRL | Qt::Key_C)).isEmpty());
    CHECK_FALSE(shortcutRefusal(key(Qt::CTRL | Qt::Key_Control)).isEmpty());
}

TEST_CASE("every keyed action can be given a key, under a name of its own", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    const QList<QAction*> bindable = bindableActions(&w);
    QSet<QString> names;
    for (QAction* a : bindable) {
        CHECK_FALSE(names.contains(a->objectName()));
        names.insert(a->objectName());
    }
    for (QAction* a : windowActions(&w))
        if (!a->shortcuts().isEmpty() && a->isVisible())
            CHECK(bindable.contains(a));  // FAIL names the action without an objectName
    CHECK(bindable.contains(action(w, "swapColours")));
    CHECK(bindable.contains(action(w, "brushSmaller")));
    CHECK(bindable.contains(action(w, "preferences")));
    CHECK(bindable.contains(action(w, "newFile")));
    CHECK(bindable.contains(action(w, "quit")));
}

// But Undo and Redo, which take every key the platform has for them
// (Alt+Backspace and the Undo key on some), and Default gives them back.
TEST_CASE("the keys an action has by default are ones the rules allow", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    QList<QKeySequence> platform = QKeySequence::keyBindings(QKeySequence::Undo);
    platform += QKeySequence::keyBindings(QKeySequence::Redo);
    for (QAction* a : bindableActions(&w))
        for (const QKeySequence& k : defaultShortcuts(a)) {
            const QString why = shortcutRefusal(k);
            if (!why.isEmpty() && !platform.contains(k))
                FAIL_CHECK(a->objectName().toStdString() << " " << native(k).toStdString() << ": "
                                                         << why.toStdString());
        }
}

TEST_CASE("a kept key replaces the default and wins over another action's default", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    const QKeySequence save = MainWindow().findChild<QAction*>(QStringLiteral("save"))->shortcut();
    REQUIRE_FALSE(save.isEmpty());
    keep("newFile", save.toString(QKeySequence::PortableText));
    keep("open", QString());                 // no key
    keep("quit", QStringLiteral("X"));       // refused: dropped
    keep("saveAs", QStringLiteral("Ctrl+Shift+F11"));
    keep("about", QStringLiteral("Ctrl+Shift+F11"));  // already kept by an earlier action, or this one is earlier

    MainWindow w;
    CHECK(action(w, "newFile")->shortcuts() == QList<QKeySequence>{save});
    CHECK(action(w, "save")->shortcuts().isEmpty());
    CHECK(action(w, "open")->shortcuts().isEmpty());
    CHECK(action(w, "quit")->shortcuts().isEmpty());
    const int both = int(action(w, "saveAs")->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Shift+F11")))) +
                     int(action(w, "about")->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Shift+F11"))));
    CHECK(both == 1);
    CHECK(shortcutClashes(&w).isEmpty());
    // the defaults are still known
    CHECK(defaultShortcuts(action(w, "save")) == QList<QKeySequence>{save});
    CHECK(defaultShortcuts(action(w, "newFile")) == QList<QKeySequence>{QKeySequence(QKeySequence::New)});
}

TEST_CASE("the Shortcuts page refuses a key keyboard mode needs or another action has", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    QAction* saveAs = action(w, "saveAs");
    QAction* save = action(w, "save");
    const QList<QKeySequence> before = saveAs->shortcuts();
    ShortcutsEditor e(&w);
    CHECK_FALSE(e.assign(key(Qt::CTRL | Qt::Key_K)));  // nothing picked
    e.select(saveAs);
    REQUIRE(e.selected() == saveAs);
    CHECK(e.keyEdit()->isEnabled());

    CHECK_FALSE(e.assign(key(Qt::Key_X)));
    CHECK(e.message()->text().contains(QStringLiteral("can't be a shortcut")));
    CHECK(e.keys(saveAs) == before);
    CHECK_FALSE(e.assign(save->shortcut()));
    CHECK(e.message()->text().contains(QStringLiteral("already the key for Save")));
    CHECK(e.keys(saveAs) == before);
    CHECK(e.keyEdit()->keySequence() == (before.isEmpty() ? QKeySequence() : before.first()));

    const QKeySequence k = freeKey(w);
    CHECK(e.assign(k));
    CHECK(e.message()->text().isEmpty());
    CHECK(e.keys(saveAs) == QList<QKeySequence>{k});
    CHECK(rowOf(e, QStringLiteral("Save As"))->text(1) == native(k));
    CHECK(rowOf(e, QStringLiteral("Save As"))->font(0).italic());
    CHECK(e.defaultButton()->isEnabled());
    CHECK(saveAs->shortcuts() == before);  // not until apply()

    // the key field assigns as it is typed into
    e.select(action(w, "about"));
    const QKeySequence k2 = freeKey(w, 1);
    e.keyEdit()->setKeySequence(k2);
    Q_EMIT e.keyEdit()->keySequenceChanged(k2);
    CHECK(e.keys(action(w, "about")) == QList<QKeySequence>{k2});
    // and the key it has here is taken, though not yet applied
    e.select(action(w, "aboutQt"));
    CHECK_FALSE(e.assign(k));
    CHECK(e.message()->text().contains(QStringLiteral("already the key for Save As")));

    // the search finds a row by the key it has here
    e.filterEdit()->setText(native(k));
    CHECK_FALSE(rowOf(e, QStringLiteral("Save As"))->isHidden());
    CHECK(rowOf(e, QStringLiteral("Save"))->isHidden());
    e.filterEdit()->clear();
    CHECK_FALSE(rowOf(e, QStringLiteral("Save"))->isHidden());
}

TEST_CASE("Shortcuts keeps only the keys that differ from the default", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    QKeySequence k, saveKey;
    {
        MainWindow w;
        k = freeKey(w);
        saveKey = action(w, "save")->shortcut();
        ShortcutsEditor e(&w);
        e.select(action(w, "save"));
        e.noKeyButton()->click();
        CHECK(e.keys(action(w, "save")).isEmpty());
        CHECK_FALSE(e.noKeyButton()->isEnabled());
        e.select(action(w, "newFile"));
        REQUIRE(e.assign(saveKey));  // free here now
        e.select(action(w, "swapColours"));
        REQUIRE(e.assign(k));
        e.apply();

        CHECK(action(w, "save")->shortcuts().isEmpty());
        CHECK(action(w, "newFile")->shortcuts() == QList<QKeySequence>{saveKey});
        CHECK(action(w, "swapColours")->shortcuts() == QList<QKeySequence>{k});
        CHECK(shortcutClashes(&w).isEmpty());
        CHECK(isKept("save"));
        CHECK(kept("save").isEmpty());
        CHECK(kept("newFile") == saveKey.toString(QKeySequence::PortableText));
        CHECK(kept("swapColours") == k.toString(QKeySequence::PortableText));
        CHECK_FALSE(isKept("open"));
        CHECK(keptNames() == QStringList{QStringLiteral("newFile"), QStringLiteral("save"),
                                         QStringLiteral("swapColours")});
    }
    {
        // the next window has them
        MainWindow w;
        CHECK(action(w, "save")->shortcuts().isEmpty());
        CHECK(action(w, "newFile")->shortcuts() == QList<QKeySequence>{saveKey});
        CHECK(action(w, "swapColours")->shortcuts() == QList<QKeySequence>{k});

        // Default gives Save back only what no other action has here
        ShortcutsEditor e(&w);
        e.select(action(w, "save"));
        CHECK(e.defaultButton()->isEnabled());
        e.defaultButton()->click();
        CHECK(e.keys(action(w, "save")).isEmpty());
        CHECK(e.message()->text().contains(QStringLiteral("is the key for New now")));

        e.select(action(w, "newFile"));
        e.defaultButton()->click();
        CHECK_FALSE(e.defaultButton()->isEnabled());
        CHECK_FALSE(rowOf(e, QStringLiteral("New"))->font(0).italic());
        e.select(action(w, "save"));
        e.defaultButton()->click();
        CHECK(e.keys(action(w, "save")) == QList<QKeySequence>{saveKey});
        e.apply();
        CHECK(action(w, "save")->shortcuts() == QList<QKeySequence>{saveKey});
        CHECK_FALSE(isKept("save"));
        CHECK_FALSE(isKept("newFile"));
        CHECK(isKept("swapColours"));

        // Reset All gives every action its default and keeps nothing
        ShortcutsEditor again(&w);
        again.resetAllButton()->click();
        CHECK(again.keys(action(w, "swapColours")).isEmpty());
        again.apply();
        CHECK(action(w, "swapColours")->shortcuts().isEmpty());
        CHECK(keptNames().isEmpty());
    }
}

TEST_CASE("Default gives an action the platform's keys the rules would refuse", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    QAction* undo = action(w, "undo");
    const QList<QKeySequence> keys = undo->shortcuts();
    REQUIRE_FALSE(keys.isEmpty());
    ShortcutsEditor e(&w);
    e.select(undo);
    e.noKeyButton()->click();
    e.apply();
    CHECK(undo->shortcuts().isEmpty());
    e.defaultButton()->click();
    e.apply();
    CHECK(undo->shortcuts() == keys);
    CHECK(keptNames().isEmpty());
    // and a kept setting with them is read back
    keep("undo", QKeySequence::listToString(keys, QKeySequence::PortableText));
    MainWindow next;
    CHECK(action(next, "undo")->shortcuts() == keys);
}

TEST_CASE("Edit > Preferences has a Shortcuts page, and OK applies it", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    QAction* swap = action(w, "swapColours");
    const QKeySequence k = freeKey(w);
    bool shown = false;
    QTimer::singleShot(0, &w, [&] {
        auto* d = qobject_cast<PreferencesDialog*>(QApplication::activeModalWidget());
        if (!d)
            return;
        shown = true;
        CHECK(d->pages()->count() == 3);
        CHECK(d->pages()->tabText(0) == QStringLiteral("Fonts"));
        CHECK(d->pages()->tabText(1) == QStringLiteral("Documents"));
        CHECK(d->pages()->tabText(2) == QStringLiteral("Shortcuts"));
        auto* e = d->findChild<ShortcutsEditor*>();
        REQUIRE(e);
        e->select(swap);
        CHECK(e->assign(k));
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    action(w, "preferences")->trigger();
    REQUIRE(shown);
    CHECK(swap->shortcuts() == QList<QKeySequence>{k});
    CHECK(kept("swapColours") == k.toString(QKeySequence::PortableText));

    // Cancel changes nothing
    shown = false;
    QTimer::singleShot(0, &w, [&] {
        auto* d = qobject_cast<PreferencesDialog*>(QApplication::activeModalWidget());
        if (!d)
            return;
        shown = true;
        auto* e = d->findChild<ShortcutsEditor*>();
        REQUIRE(e);
        e->resetAll();
        d->buttons()->button(QDialogButtonBox::Cancel)->click();
    });
    action(w, "preferences")->trigger();
    REQUIRE(shown);
    CHECK(swap->shortcuts() == QList<QKeySequence>{k});
    CHECK(isKept("swapColours"));
}

TEST_CASE("the tooltips that name a key follow it", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    const QKeySequence k = freeKey(w);

    QAction* eyedropper = action(w, "eyedropperTool");
    CHECK(eyedropper->toolTip().contains(native(QKeySequence(Qt::CTRL | Qt::Key_I))));
    eyedropper->setShortcut(k);
    CHECK(eyedropper->toolTip().contains(native(k)));
    CHECK_FALSE(eyedropper->toolTip().contains(native(QKeySequence(Qt::CTRL | Qt::Key_I))));
    eyedropper->setShortcut({});
    CHECK(eyedropper->toolTip().section(QLatin1Char('\n'), 0, 0) == QStringLiteral("Eyedropper"));
    // a group's tool keeps the toolbox's line about the rest of the group
    QAction* ellipse = action(w, "ellipseTool");
    REQUIRE(ellipse->toolTip().contains(QStringLiteral("right-click")));
    ellipse->setShortcut(k);
    CHECK(ellipse->toolTip().section(QLatin1Char('\n'), 0, 0).contains(native(k)));
    CHECK(ellipse->toolTip().contains(QStringLiteral("right-click")));

    auto* size = w.findChild<QSpinBox*>(QStringLiteral("brushSize"));
    REQUIRE(size);
    CHECK(size->toolTip().contains(native(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft))));
    action(w, "brushSmaller")->setShortcut(k);
    CHECK(size->toolTip().contains(native(k)));
    action(w, "brushSmaller")->setShortcut({});
    CHECK(size->toolTip().contains(QStringLiteral("Tools > Smaller Brush and Larger Brush")));

    auto* symmetry = w.findChild<QComboBox*>(QStringLiteral("brushSymmetry"));
    REQUIRE(symmetry);
    CHECK(symmetry->toolTip().contains(native(QKeySequence(Qt::CTRL | Qt::Key_M))));
    action(w, "mirrorMode")->setShortcut({});
    CHECK(symmetry->toolTip().endsWith(QStringLiteral("(mirror mode)")));

    auto* filled = w.findChild<QCheckBox*>(QStringLiteral("shapeFilled"));
    REQUIRE(filled);
    action(w, "filledShapes")->setShortcut(k);
    CHECK(filled->toolTip().endsWith(QStringLiteral("(Filled Shapes, ") + native(k) + QLatin1Char(')')));
}

TEST_CASE("a middle drag pans the canvas, and not while another drag is on", "[shortcuts][canvas]") {
    MainWindow w;
    w.resize(500, 400);
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setZoom(CanvasView::kMaxZoom);
    QApplication::processEvents();
    QAbstractScrollArea* area = nullptr;
    for (QWidget* p = v->parentWidget(); p && !area; p = p->parentWidget())
        area = qobject_cast<QAbstractScrollArea*>(p);
    REQUIRE(area);
    QScrollBar* h = area->horizontalScrollBar();
    QScrollBar* vert = area->verticalScrollBar();
    REQUIRE(h->maximum() > 100);
    REQUIRE(vert->maximum() > 100);
    h->setValue(50);
    vert->setValue(50);
    const Canvas before = w.document()->canvas();

    const QPointF at(area->viewport()->mapToGlobal(QPoint(100, 100)));
    mouse(v, QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton);
    CHECK(v->panning());
    CHECK(v->cursor().shape() == Qt::ClosedHandCursor);
    mouse(v, QEvent::MouseMove, at - QPointF(30, 20), Qt::NoButton, Qt::MiddleButton);
    CHECK(h->value() == 80);
    CHECK(vert->value() == 70);
    // the view has moved under the pointer; the drag is still from where it began
    mouse(v, QEvent::MouseMove, at + QPointF(10, 10), Qt::NoButton, Qt::MiddleButton);
    CHECK(h->value() == 40);
    CHECK(vert->value() == 40);
    // a left button pressed mid-pan doesn't draw
    mouse(v, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::MiddleButton | Qt::LeftButton);
    CHECK(v->panning());
    mouse(v, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::MiddleButton);
    mouse(v, QEvent::MouseButtonRelease, at, Qt::MiddleButton, Qt::NoButton);
    CHECK_FALSE(v->panning());
    CHECK(v->cursor().shape() != Qt::ClosedHandCursor);
    CHECK(w.document()->canvas() == before);

    // a middle press during a left drag doesn't start one
    mouse(v, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(v, QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::LeftButton | Qt::MiddleButton);
    CHECK_FALSE(v->panning());
    mouse(v, QEvent::MouseButtonRelease, at, Qt::MiddleButton, Qt::LeftButton);
    mouse(v, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    CHECK_FALSE(v->panning());
}

TEST_CASE("keyboard mode leaves its editing keys alone with Ctrl or Meta held", "[shortcuts][canvas]") {
    // The rules give those chords to actions (Ctrl+arrows step the pen by
    // default), so keyboard mode doesn't edit or move with one no action has.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    action(w, "keyboardTool")->trigger();
    v->setFocus();
    v->setCellCursor({3, 2});
    sendKey(v, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"));
    sendKey(v, Qt::Key_B, Qt::NoModifier, QStringLiteral("B"));
    const Canvas typed = w.document()->canvas();
    REQUIRE(v->cellCursor().x == 5);
    for (const int k : {Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Return, Qt::Key_Enter, Qt::Key_Home, Qt::Key_End,
                        Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down})
        for (const Qt::KeyboardModifiers mods :
             {Qt::KeyboardModifiers(Qt::ControlModifier), Qt::KeyboardModifiers(Qt::MetaModifier),
              Qt::ControlModifier | Qt::AltModifier, Qt::ControlModifier | Qt::ShiftModifier}) {
            const QKeySequence chord(QKeyCombination(mods, Qt::Key(k)));
            INFO(native(chord).toStdString());
            // an action may have it, but Ctrl+Alt+Del and Backspace (the desktop's)
            const bool desktop = (mods & Qt::AltModifier) && (k == Qt::Key_Delete || k == Qt::Key_Backspace);
            CHECK(shortcutRefusal(chord).isEmpty() != desktop);
            sendKey(v, k, mods);
            CHECK(w.document()->canvas() == typed);
            CHECK(v->cellCursor().x == 5);
            CHECK(v->cellCursor().y == 2);
        }
    // and without them it still does
    sendKey(v, Qt::Key_Backspace, Qt::NoModifier);
    CHECK(w.document()->canvas() != typed);
    CHECK(v->cellCursor().x == 4);
    sendKey(v, Qt::Key_Home, Qt::ShiftModifier);
    CHECK(v->cellCursor().x == 0);
}

TEST_CASE("the glyph-set line's tooltip follows every set key", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    auto* label = w.findChild<QLabel*>(QStringLiteral("glyphSetLabel"));
    REQUIRE(label);
    const auto tip = [label] { return label->toolTip(); };
    const QString f1 = native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F1));
    const QString f10 = native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F10));
    const QString prev = native(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    const QString next = native(QKeySequence(Qt::CTRL | Qt::Key_Period));
    const QString dflt = native(QKeySequence(Qt::CTRL | Qt::Key_Slash));
    CHECK(tip().contains(QStringLiteral("%1 and %2 step through the sets, %3 goes back to the default")
                             .arg(prev, next, dflt)));
    CHECK(tip().contains(QStringLiteral("%1 to %2 pick sets 1-10; every set is in the Glyphs menu").arg(f1, f10)));

    // a step key taken away
    action(w, "previousGlyphSet")->setShortcuts({});
    CHECK(tip().contains(QStringLiteral("\n%1 steps through the sets, %2 goes back to the default").arg(next, dflt)));
    CHECK_FALSE(tip().contains(QStringLiteral("\n and")));
    action(w, "nextGlyphSet")->setShortcuts({});
    action(w, "defaultGlyphSet")->setShortcuts({});
    CHECK_FALSE(tip().contains(QStringLiteral("step")));
    CHECK_FALSE(tip().contains(QStringLiteral("default")));

    // a set key moved, from the middle of the run
    const QKeySequence k = freeKey(w);
    action(w, "glyphSet5")->setShortcut(k);
    const QString f4 = native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F4));
    const QString f6 = native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F6));
    CHECK(tip().contains(QStringLiteral("%1 to %2 pick sets 1-4, %3 picks set 5, %4 to %5 pick sets 6-10")
                             .arg(f1, f4, native(k), f6, f10)));
    // a set past the tenth given one
    action(w, "glyphSet12")->setShortcut(freeKey(w));
    CHECK(tip().contains(QStringLiteral("%1 picks set 12").arg(native(action(w, "glyphSet12")->shortcut()))));
    // every set key taken away
    for (int i = 1; i <= 16; ++i)
        action(w, QStringLiteral("glyphSet%1").arg(i).toLatin1().constData())->setShortcuts({});
    CHECK(tip().endsWith(QStringLiteral("\nEvery set is in the Glyphs menu")));
    CHECK_FALSE(tip().contains(QStringLiteral("pick")));
}

TEST_CASE("Edit > Delete's menu text names the key it is given", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    QAction* del = action(w, "deleteSelection");
    const QString delKey = native(QKeySequence(Qt::Key_Delete));
    CHECK(del->text() == QStringLiteral("&Delete\t") + delKey);
    const QKeySequence k = freeKey(w);
    {
        ShortcutsEditor e(&w);
        e.select(del);
        REQUIRE(e.assign(k));
        e.apply();
    }
    // the canvas still clears with Del, so the menu names both
    CHECK(del->text() == QStringLiteral("&Delete\t%1, %2").arg(native(k), delKey));
    bool listed = false;
    for (const ShortcutEntry& s : shortcutEntries(&w))
        if (s.action == del) {
            listed = true;
            CHECK(s.keys == QStringLiteral("%1, %2").arg(native(k), delKey));
        }
    CHECK(listed);
    del->setShortcuts({});
    CHECK(del->text() == QStringLiteral("&Delete\t") + delKey);
}

TEST_CASE("an open Keyboard Shortcuts sheet follows Preferences > Shortcuts", "[shortcuts][preferences]") {
    ShortcutsCleared cleared;
    MainWindow w;
    ShortcutsDialog* sheet = showShortcuts(&w);
    const auto shown = [sheet](const QString& name) {
        auto* tree = sheet->findChild<QTreeWidget*>(QStringLiteral("shortcutsTree"));
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            for (int j = 0; j < tree->topLevelItem(i)->childCount(); ++j)
                if (tree->topLevelItem(i)->child(j)->text(0) == name)
                    return tree->topLevelItem(i)->child(j)->text(1);
        return QStringLiteral("(none)");
    };
    REQUIRE(shown(QStringLiteral("Save")) == native(QKeySequence::Save));
    const QKeySequence k = freeKey(w);
    ShortcutsEditor e(&w);
    e.select(action(w, "save"));
    REQUIRE(e.assign(k));
    e.apply();
    CHECK(sheet->isVisible());
    CHECK(shown(QStringLiteral("Save")) == native(k));
    sheet->close();
}

TEST_CASE("a pen is refused while a middle drag pans, as for any other drag", "[shortcuts][canvas]") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    action(w, "halfBlockTool")->trigger();
    const Canvas before = w.document()->canvas();
    mouse(v, QEvent::MouseButtonPress, globalAt(v, {2, 2}), Qt::MiddleButton, Qt::MiddleButton);
    REQUIRE(v->panning());
    tablet(v, QEvent::TabletPress, {3, 3});
    tablet(v, QEvent::TabletMove, {6, 3});
    tablet(v, QEvent::TabletRelease, {6, 3});
    CHECK(v->panning());
    CHECK(w.document()->canvas() == before);
    // nor the stylus's mouse events, which reach no widget while it lasts
    mouse(v, QEvent::MouseButtonRelease, globalAt(v, {2, 2}), Qt::MiddleButton, Qt::NoButton);
    CHECK_FALSE(v->panning());
    // and the pen draws again once it has ended
    tablet(v, QEvent::TabletPress, {3, 3});
    tablet(v, QEvent::TabletRelease, {3, 3});
    CHECK(w.document()->canvas() != before);
}

TEST_CASE("the undo key ends a pan, and undoes nothing", "[shortcuts][canvas]") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    action(w, "halfBlockTool")->trigger();
    mouse(v, QEvent::MouseButtonPress, globalAt(v, {2, 2}), Qt::LeftButton, Qt::LeftButton);
    mouse(v, QEvent::MouseButtonRelease, globalAt(v, {2, 2}), Qt::LeftButton, Qt::NoButton);
    const Canvas drawn = w.document()->canvas();
    v->setFocus();
    mouse(v, QEvent::MouseButtonPress, globalAt(v, {4, 4}), Qt::MiddleButton, Qt::MiddleButton);
    REQUIRE(v->panning());
    const QKeySequence undo = QKeySequence::keyBindings(QKeySequence::Undo).value(0);
    REQUIRE_FALSE(undo.isEmpty());
    QKeyEvent over(QEvent::ShortcutOverride, undo[0].key(), undo[0].keyboardModifiers());
    QApplication::sendEvent(v, &over);
    CHECK(over.isAccepted());
    sendKey(v, undo[0].key(), undo[0].keyboardModifiers());
    CHECK_FALSE(v->panning());
    CHECK(v->cursor().shape() != Qt::ClosedHandCursor);
    CHECK(w.document()->canvas() == drawn);
    mouse(v, QEvent::MouseButtonRelease, globalAt(v, {4, 4}), Qt::MiddleButton, Qt::NoButton);
    CHECK(w.document()->canvas() == drawn);
}

TEST_CASE("a middle press during a tool's drag doesn't pan, with no other button held", "[shortcuts][canvas]") {
    // The drag's left release went where the view couldn't see it (#45):
    // the middle button is the only one held, and the brush's drag is on.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    action(w, "halfBlockTool")->trigger();
    const Canvas before = w.document()->canvas();
    mouse(v, QEvent::MouseButtonPress, globalAt(v, {2, 2}), Qt::LeftButton, Qt::LeftButton);
    mouse(v, QEvent::MouseButtonPress, globalAt(v, {5, 2}), Qt::MiddleButton, Qt::MiddleButton);
    CHECK_FALSE(v->panning());
    CHECK(v->cursor().shape() != Qt::ClosedHandCursor);
    mouse(v, QEvent::MouseButtonRelease, globalAt(v, {5, 2}), Qt::MiddleButton, Qt::NoButton);
    CHECK_FALSE(v->panning());
    // the brush's drag went on, and lands
    CHECK(w.document()->canvas() == before);
    mouse(v, QEvent::MouseButtonRelease, globalAt(v, {5, 2}), Qt::LeftButton, Qt::NoButton);
    CHECK(w.document()->canvas() != before);
}
