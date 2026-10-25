// The pointer grab on an X server (the xcb platform), where a grab is the
// server's and not only Qt's record: input comes through XTest, as a
// device's would, and a second client asks the server whether the pointer
// is grabbed. Its own binary, run on a virtual X server (xcb_test.sh); it
// skips without one.

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "select_tool.h"
#include "tool.h"

#include "blockwright/brush.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMenu>
#include <QTemporaryDir>
#include <QWindow>

#include <xcb/xcb.h>
#include <xcb/xtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace bw;

namespace {

// Another client on the same server: it moves the pointer and presses its
// first button through XTest, and tries a grab of its own to see whether
// some client holds one.
class XClient {
public:
    XClient() {
        int screen = 0;
        c_ = xcb_connect(nullptr, &screen);
        if (xcb_connection_has_error(c_))
            return;
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c_));
        for (; it.rem && screen > 0; --screen)
            xcb_screen_next(&it);
        if (it.rem)
            root_ = it.data->root;
    }
    ~XClient() { xcb_disconnect(c_); }
    XClient(const XClient&) = delete;
    XClient& operator=(const XClient&) = delete;

    bool ok() const { return !xcb_connection_has_error(c_) && root_ != XCB_NONE; }
    void moveTo(QPoint global) { fake(XCB_MOTION_NOTIFY, 0, global); }
    void press() { fake(XCB_BUTTON_PRESS, 1, {}); }
    void release() { fake(XCB_BUTTON_RELEASE, 1, {}); }
    // Whether a client holds the pointer grabbed: this one's grab fails.
    bool pointerGrabbed() {
        xcb_grab_pointer_reply_t* r = xcb_grab_pointer_reply(
            c_,
            xcb_grab_pointer(c_, 0, root_, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                             XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
            nullptr);
        const bool mine = r && r->status == XCB_GRAB_STATUS_SUCCESS;
        std::free(r);
        if (mine) {
            xcb_ungrab_pointer(c_, XCB_CURRENT_TIME);
            sync();
        }
        return !mine;
    }

private:
    void fake(std::uint8_t type, std::uint8_t button, QPoint at) {
        xcb_test_fake_input(c_, type, button, XCB_CURRENT_TIME, type == XCB_MOTION_NOTIFY ? root_ : XCB_NONE,
                            std::int16_t(at.x()), std::int16_t(at.y()), 0);
        sync();
    }
    // A round trip: the server has done everything sent before it.
    void sync() { std::free(xcb_get_input_focus_reply(c_, xcb_get_input_focus(c_), nullptr)); }

    xcb_connection_t* c_ = nullptr;
    xcb_window_t root_ = XCB_NONE;
};

// Runs the event loop until `done` holds, for at most `ms`.
template <class F>
bool waitFor(F done, int ms = 3000) {
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms)
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

void settle(int ms = 200) {
    waitFor([] { return false; }, ms);
}

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

// The window, shown where the server puts it (there is no window manager)
// and active.
void show(MainWindow& w) {
    w.move(0, 0);
    w.resize(900, 700);
    w.show();
    w.activateWindow();
    REQUIRE(waitFor([&] { return w.windowHandle() && w.windowHandle()->isExposed() && w.isActiveWindow(); }));
    settle();
}

// A point on the screen outside the window and anything it opens.
const QPoint outside(1500, 1100);

} // namespace

TEST_CASE("a popup that opens during a mouse stroke keeps the pointer grab, so a click outside closes it") {
    // Qt grabs the pointer for a popup before it tells the focus widget
    // the popup took the focus, and the canvas ends the drag then. A grab
    // or release of the canvas's own would take the popup's away: on X a
    // release ungrabs the device whatever window holds it. The canvas takes
    // none.
    XClient x;
    REQUIRE(x.ok());
    MainWindow w;
    show(w);
    CanvasView* v = w.canvasView();
    act(w, "glyphTool")->trigger();
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    auto* tool = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(tool);
    const auto cell = [&](int cx) { return v->mapToGlobal(v->cellRect(QPoint(cx, 2)).center()); };
    REQUIRE_FALSE(x.pointerGrabbed());

    x.moveTo(cell(2));
    x.press();
    REQUIRE(waitFor([&] { return tool->dragging(); }));
    CHECK(QWidget::mouseGrabber() == nullptr);
    x.moveTo(cell(3));
    settle();

    QMenu m(v);
    m.addAction(QStringLiteral("x"));
    m.popup(cell(20) + QPoint(0, 40));
    REQUIRE(waitFor([&] { return m.isVisible() && m.windowHandle() && m.windowHandle()->isExposed(); }));
    settle();
    CHECK_FALSE(tool->dragging());
    CHECK(x.pointerGrabbed());
    // The stroke's button comes up; the popup stays, and so does its grab.
    x.release();
    settle();
    CHECK(m.isVisible());
    CHECK(x.pointerGrabbed());
    // A click outside the application reaches the popup, which closes.
    x.moveTo(outside);
    x.press();
    x.release();
    CHECK(waitFor([&] { return !m.isVisible(); }));
    settle();
    CHECK(QWidget::mouseGrabber() == nullptr);
    CHECK_FALSE(x.pointerGrabbed());
    CHECK(w.document()->undoStack()->count() == 0);
}

TEST_CASE("Paste during a mouse drag of a floating selection leaves no pointer grab behind") {
    // The drag ends in the action, not in the view: the grab must go with
    // it, or the window keeps the pointer through whatever opens next.
    XClient x;
    REQUIRE(x.ok());
    MainWindow w;
    show(w);
    CanvasView* v = w.canvasView();
    act(w, "selectTool")->trigger();
    w.selectTool()->select({0, 0, 2, 2});
    act(w, "copy")->trigger();
    const auto cell = [&](int cx, int cy) { return v->mapToGlobal(v->cellRect(QPoint(cx, cy)).center()); };

    x.moveTo(cell(0, 0));
    x.press();
    x.moveTo(cell(3, 2));
    REQUIRE(waitFor([&] { return w.selectTool()->floating() && w.selectTool()->dragging(); }));
    act(w, "paste")->trigger();
    CHECK_FALSE(w.selectTool()->dragging());
    CHECK(QWidget::mouseGrabber() == nullptr);
    x.release();
    settle();
    CHECK_FALSE(x.pointerGrabbed());
}

TEST_CASE("a mouse stroke let go out of the window lands, with no grab of the canvas's own") {
    // The server's implicit grab on the press brings the stroke's moves and
    // release to the window, and Qt's record of the press to the canvas.
    XClient x;
    REQUIRE(x.ok());
    MainWindow w;
    show(w);
    CanvasView* v = w.canvasView();
    act(w, "glyphTool")->trigger();
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    auto* tool = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(tool);
    const auto cell = [&](int cx) { return v->mapToGlobal(v->cellRect(QPoint(cx, 2)).center()); };

    x.moveTo(cell(2));
    x.press();
    REQUIRE(waitFor([&] { return tool->dragging(); }));
    CHECK(QWidget::mouseGrabber() == nullptr);
    x.moveTo(cell(3));
    x.moveTo(outside);
    settle();
    CHECK(tool->dragging());
    x.release();
    CHECK(waitFor([&] { return !tool->dragging(); }));
    CHECK(w.document()->undoStack()->count() == 1);
    CHECK(w.document()->canvas().at(2, 2) != w.document()->blankCell());
    settle();
    CHECK_FALSE(x.pointerGrabbed());
}

int main(int argc, char** argv) {
    QTemporaryDir libraries;
    qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("bundled"))));
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(libraries.filePath(QStringLiteral("user"))));
    if (qgetenv("QT_QPA_PLATFORM") != "xcb" || qEnvironmentVariableIsEmpty("DISPLAY")) {
        std::puts("no X server (QT_QPA_PLATFORM=xcb and DISPLAY): skipped");
        return 77;
    }
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
