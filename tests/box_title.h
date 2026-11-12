#pragma once
// The title a QMessageBox shows. On macOS QMessageBox::setWindowTitle sets
// none (windowTitle() stays empty), so there a box shows a title only when it
// is the box's heading (bw::headingBoxes(), app/message_box.h).

#include <QString>

namespace bwtest {

inline QString shownWindowTitle(const QString& title) {
#ifdef Q_OS_MACOS
    Q_UNUSED(title);
    return QString();
#else
    return title;
#endif
}

// What bw::boxTitle() reads back from a box given `title`.
inline QString shownTitle(bool headings, const QString& title) {
    return headings ? title : shownWindowTitle(title);
}

} // namespace bwtest
