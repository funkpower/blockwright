#pragma once
// Files the system hands the app to open. On macOS a document opened from the
// Finder, or dropped on the Dock icon, reaches the application as a
// QFileOpenEvent, not on the command line; main() installs a forwarder on the
// QApplication so the window opens it as File > Open would.

#include <QObject>

namespace bw {

class MainWindow;

class FileOpenForwarder : public QObject {
public:
    // Owned by `window`, so it stops filtering when the window goes.
    explicit FileOpenForwarder(MainWindow* window);

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    MainWindow* window_;
};

} // namespace bw
