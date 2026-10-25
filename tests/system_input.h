#pragma once

// Input delivered the way the platform delivers it, through
// QWindowSystemInterface: Qt routes it to an open popup and keeps
// QGuiApplication::mouseButtons() up to date, which QApplication::sendEvent
// does neither of.

#include <QApplication>
#include <QMenu>
#include <QPointingDevice>
#include <QWidget>
#include <QWindow>

#include <qpa/qwindowsysteminterface.h>

namespace bwtest {

// Shows the top-level widget and waits until its window is exposed.
inline bool showExposed(QWidget* top) {
    top->show();
    top->activateWindow();
    for (int i = 0; i < 100 && !(top->windowHandle() && top->windowHandle()->isExposed()); ++i)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    QApplication::processEvents();
    return top->windowHandle() && top->windowHandle()->isExposed();
}

// A mouse event at `at` in w's coordinates, through w's window, from
// `device` (the mouse by default; a stylus device makes the mouse events a
// platform synthesizes from the pen).
inline void systemMouse(QWidget* w, QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons held,
                        const QPointingDevice* device = nullptr) {
    QWidget* top = w->window();
    const QPointF local = w->mapTo(top, at);
    if (device)
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            top->windowHandle(), device, local, top->mapToGlobal(local), held, button, type, Qt::NoModifier,
            Qt::MouseEventSynthesizedBySystem);
    else
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            top->windowHandle(), local, top->mapToGlobal(local), held, button, type);
}

// Whether a click at `at` in w's coordinates, from `device` (the mouse by
// default), reaches w: its press, or the double click Qt makes of a second
// press at the same place.
inline bool clickReaches(QWidget* w, QPointF at, const QPointingDevice* device = nullptr) {
    struct Counter : QObject {
        int n = 0;
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick)
                ++n;
            return false;
        }
    } counter;
    w->installEventFilter(&counter);
    systemMouse(w, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, device);
    systemMouse(w, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, device);
    w->removeEventFilter(&counter);
    return counter.n > 0;
}

// A pen Qt knows about, so tablet events from it are delivered.
inline const QPointingDevice* systemStylus() {
    static const QPointingDevice* dev = [] {
        auto* d = new QPointingDevice(QStringLiteral("system test stylus"), 4343, QInputDevice::DeviceType::Stylus,
                                      QPointingDevice::PointerType::Pen,
                                      QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
        QWindowSystemInterface::registerInputDevice(d);
        return d;
    }();
    return dev;
}

// A tablet event at `at` in w's coordinates, through w's window.
inline void systemTablet(QWidget* w, QPointF at, Qt::MouseButtons held, double pressure) {
    QWidget* top = w->window();
    const QPointF local = w->mapTo(top, at);
    QWindowSystemInterface::handleTabletEvent(top->windowHandle(), systemStylus(), local, top->mapToGlobal(local),
                                              held, pressure, 0, 0, 0, 0, 0);
    QWindowSystemInterface::flushWindowSystemEvents();
}

// A key pressed and released through w's window, as the keyboard sends it:
// Qt tries it as a shortcut first, then gives it to the focus widget or an
// open popup.
inline void systemKey(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier,
                      const QString& text = QString()) {
    QWindow* win = w->window()->windowHandle();
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyPress, key,
                                                                                         mods, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyRelease, key,
                                                                                         mods, text);
    QApplication::processEvents();
}

// A menu pops up over w mid-drag. If `takeRelease`, `button` comes up over
// the menu, which gets the release (`held` still down after it); then the
// menu closes.
inline void popupOver(QWidget* w, bool takeRelease, Qt::MouseButton button = Qt::LeftButton,
                      Qt::MouseButtons held = Qt::NoButton) {
    QMenu m(w);
    m.addAction(QStringLiteral("x"));
    m.popup(w->mapToGlobal(QPoint(10, 10)));
    QApplication::processEvents();
    if (takeRelease)
        systemMouse(&m, QEvent::MouseButtonRelease, QPointF(5, 5), button, held);
    m.close();
    QApplication::processEvents();
}

} // namespace bwtest
