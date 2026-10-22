#pragma once
// The main window: the canvas view in a scroll area, the 1x preview dock
// and the File, View and Help menus.

#include "blockwright/canvas_renderer.h"

#include "document.h"

#include <QMainWindow>

class QAction;
class QDockWidget;
class QFileDialog;
class QLabel;
class QScrollArea;

namespace bw {

class CanvasView;
class PreviewWidget;

// The file Save As writes for the name its dialog returned: that name, with
// ".ans" added if it has no suffix. The dialog only confirmed replacing the
// name it returned, so replacing a file under an added suffix needs asking.
struct SavePath {
    QString path;
    bool confirmOverwrite = false;
};
SavePath resolveSavePath(const QString& chosen);

// Sets up `dialog` for Save As starting at `path`. It adds ".ans" itself, so
// its own overwrite check sees the name that will be written.
void configureSaveDialog(QFileDialog& dialog, const QString& path);

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    Document* document() { return &doc_; }
    CanvasView* canvasView() const { return view_; }
    PreviewWidget* preview() const { return preview_; }
    QDockWidget* previewDock() const { return previewDock_; }
    const CanvasRenderer& renderer() const { return renderer_; }

    // Opens `path` without asking anything; false and `error` on failure.
    bool openFile(const QString& path, QString* error);
    // Saves to `path` without asking anything. A save that would lose data
    // (glyphs written as '?', blink dropped) is refused unless `allowLossy`.
    bool saveFile(const QString& path, QString* error, bool allowLossy = false);

public slots:
    // The menu actions, with dialogs.
    void newFile();
    void open();
    // Opens `path`, reporting a failure in a message box.
    void openInteractive(const QString& path);
    bool save();
    bool saveAs();
    void zoomIn();
    void zoomOut();
    void zoomReset();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void createActions();
    void canvasReset();
    void updateTitle();
    void updateStatus();
    // Asks to save unsaved edits; false means cancel.
    bool maybeSave();
    bool saveInteractive(const QString& path);

    Document doc_;
    AtlasRenderer renderer_;
    QScrollArea* scroll_ = nullptr;
    CanvasView* view_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    QDockWidget* previewDock_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace bw
