#pragma once
// The main window: the canvas view in a scroll area, the 1x preview and
// palette docks, the F-key glyph set in the status bar, and the File, Edit,
// View, Colours, Glyphs, Tools, Selection and Help menus. File > SAUCE... and
// the View menu's iCE and letter-spacing toggles live in main_window_format.cpp.

#include "blockwright/canvas_renderer.h"

#include "document.h"

#include <QMainWindow>

#include <filesystem>

class QAction;
class QDockWidget;
class QFileDialog;
class QLabel;
class QMenu;
class QScrollArea;

namespace bw {

class CanvasView;
class PaletteWidget;
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

class SelectTool;
class BrushDock;
class LibraryDock;
class ToolRegistry;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    Document* document() { return &doc_; }
    CanvasView* canvasView() const { return view_; }
    PreviewWidget* preview() const { return preview_; }
    QDockWidget* previewDock() const { return previewDock_; }
    PaletteWidget* paletteWidget() const { return palette_; }
    QDockWidget* paletteDock() const { return paletteDock_; }
    BrushDock* brushWidget() const { return brush_; }
    QDockWidget* brushDock() const { return brushDock_; }
    QLabel* glyphSetLabel() const { return glyphSetLabel_; }
    const CanvasRenderer& renderer() const { return renderer_; }
    ToolRegistry* tools() const { return tools_; }
    SelectTool* selectTool() const { return selectTool_; }
    LibraryDock* libraryWidget() const { return library_; }
    QDockWidget* libraryDock() const { return libraryDock_; }

    // The Library (main_window_library.cpp). Floats the stamp at `path` (a
    // .ans in the Library dock, or any stamp on disk) with its top-left at
    // `cell`, through the select tool's paste in transparent paste mode;
    // false and `error` if it can't be read.
    bool placeStamp(const std::filesystem::path& path, QPoint cell, QString* error);
    bool placeStamp(const QString& path, QPoint cell, QString* error);  // a UTF-8 path
    // Saves the selection (or the floating selection) as a stamp in the
    // user library and rescans the dock, at libraryPathFor(name). Refuses
    // to replace the stamp of that name already there unless `replace`. The
    // path written, or empty and `error`.
    QString saveSelectionToLibrary(const QString& name, const QString& category, const QStringList& tags,
                                   QString* error, bool replace = false);
    // The .ans a stamp called `name` is saved as in the user library
    // (library::stampPathFor): a stamp with another title is never
    // replaced. `exists` says whether a stamp of that name is there.
    // Empty, with `error`, if the library folder can't be listed. The
    // QString is for showing; saving uses the native path.
    QString libraryPathFor(const QString& name, bool* exists = nullptr, QString* error = nullptr) const;

    // Shows the picture at `path` over the canvas to trace (View > Reference
    // Image). It is never saved. False and `error` if it can't be read.
    bool loadReferenceImage(const QString& path, QString* error);

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
    // File > SAUCE...: edits the record's text fields as one undo step.
    void editSauce();
    // View > iCE Colours and 9 px Letter Spacing, each one undo step. A 9 px
    // canvas too large to display is refused with a status message.
    void setIceColours(bool on);
    void setNinePx(bool on);
    // Selection > Save to Library...: asks for the name, category and tags.
    void saveToLibrary();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void createActions();
    // The iCE and letter-spacing toggles (main_window_format.cpp).
    void createFormatActions(QMenu* view);
    // The renderer and the toggles follow the document's format.
    void formatChanged();
    void syncFormatActions();
    void canvasReset();
    void updateTitle();
    void updateStatus();
    void updateGlyphSet();
    // Asks to save unsaved edits; false means cancel.
    bool maybeSave();
    bool saveInteractive(const QString& path);
    // The tools, selection, mirror, guides and reference image
    // (main_window_edit.cpp).
    void createEditingActions(QMenu* file, QMenu* edit, QMenu* view);
    // The Library dock and stamp drops on the canvas.
    void createLibrary();

    Document doc_;
    AtlasRenderer renderer_;
    QScrollArea* scroll_ = nullptr;
    CanvasView* view_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    QDockWidget* previewDock_ = nullptr;
    PaletteWidget* palette_ = nullptr;
    QDockWidget* paletteDock_ = nullptr;
    BrushDock* brush_ = nullptr;
    QDockWidget* brushDock_ = nullptr;
    LibraryDock* library_ = nullptr;
    QDockWidget* libraryDock_ = nullptr;
    QLabel* status_ = nullptr;
    ToolRegistry* tools_ = nullptr;
    SelectTool* selectTool_ = nullptr;
    QLabel* glyphSetLabel_ = nullptr;
    QList<QAction*> glyphSetActions_;
    QAction* iceAction_ = nullptr;
    QAction* ninePxAction_ = nullptr;
};

} // namespace bw
