#include "file_open.h"

#include "main_window.h"

#include <QFileOpenEvent>

namespace bw {

FileOpenForwarder::FileOpenForwarder(MainWindow* window) : QObject(window), window_(window) {}

bool FileOpenForwarder::eventFilter(QObject* watched, QEvent* e) {
    if (e->type() == QEvent::FileOpen) {
        const QString path = static_cast<QFileOpenEvent*>(e)->file();
        if (!path.isEmpty()) {
            window_->openRequested(path);
            return true;
        }
    }
    return QObject::eventFilter(watched, e);
}

} // namespace bw
