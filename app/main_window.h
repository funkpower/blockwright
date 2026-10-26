#pragma once
// The main window: the canvas view in a scroll area, the Toolbox and Tool
// Options docks on the left, the 1x preview, palette, Layers, Library, Lint
// and IRC View docks on the right, the glyph bar at the bottom, the F-key
// glyph set in the status bar, and the File, Edit, View, Colours, Glyphs,
// Tools, Selection, Layer, Window and Help menus. File > SAUCE... and the
// View menu's iCE and letter-spacing toggles live in main_window_format.cpp;
// the docks, the Layer and Window menus and the saved layout in
// main_window_docks.cpp; projects (opening and saving them, snapshots) in
// main_window_project.cpp; the IRC row meter beside the canvas, View > IRC
// and File > Export IRC Text... in main_window_irc.cpp; generator runs and
// the trust they ask for in main_window_generators.cpp.

#include "blockwright/canvas_renderer.h"

#include "document.h"

#include <QHash>
#include <QSet>
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
class GeneratorRunner;
class IrcMeter;
class PaletteWidget;
class PreviewWidget;

// The file Save As writes for the name its dialog returned: that name, with
// "." + `suffix` added if it has no suffix. The dialog only confirmed
// replacing the name it returned, so replacing a file under an added suffix
// needs asking.
struct SavePath {
    QString path;
    bool confirmOverwrite = false;
};
SavePath resolveSavePath(const QString& chosen, const QString& suffix = QStringLiteral("ans"));

// Sets up `dialog` for Save As starting at `path`, with filters for ANSI art
// and Blockwright projects: the project filter, and ".bwp", selected when
// `project`, else ANSI art and ".ans". It adds the selected filter's suffix
// itself, so its own overwrite check sees the name that will be written.
void configureSaveDialog(QFileDialog& dialog, const QString& path, bool project = false);

// What a lossy save or snapshot loses, one line per kind of loss.
QString lossDescription(const AnsiOutput& out);
// The question Save asks before a lossy save (AnsiOutput::lossy) as .ans,
// or, with `project`, as a project (which keeps every glyph, so only blink
// lost on open counts). Blink lost on open is stated as what happened on
// open, not as something this save loses, as the cells may have been put
// back.
QString saveWarning(const AnsiOutput& out, bool project = false);

class SelectTool;
class GlyphBar;
class IrcViewDock;
class LayersDock;
class LibraryDock;
class LintDock;
class ToolOptions;
class Toolbox;
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
    ToolOptions* toolOptions() const { return toolOptions_; }
    QDockWidget* toolOptionsDock() const { return toolOptionsDock_; }
    Toolbox* toolbox() const { return toolbox_; }
    QDockWidget* toolboxDock() const { return toolboxDock_; }
    GlyphBar* glyphBar() const { return glyphBar_; }
    QDockWidget* glyphBarDock() const { return glyphBarDock_; }
    LayersDock* layersWidget() const { return layers_; }
    QDockWidget* layersDock() const { return layersDock_; }
    // Every dock, in the Window menu's order.
    QList<QDockWidget*> docks() const;
    QLabel* glyphSetLabel() const { return glyphSetLabel_; }
    const CanvasRenderer& renderer() const { return renderer_; }
    ToolRegistry* tools() const { return tools_; }
    SelectTool* selectTool() const { return selectTool_; }
    LibraryDock* libraryWidget() const { return library_; }
    QDockWidget* libraryDock() const { return libraryDock_; }
    LintDock* lintWidget() const { return lint_; }
    QDockWidget* lintDock() const { return lintDock_; }
    IrcViewDock* ircViewWidget() const { return ircView_; }
    QDockWidget* ircViewDock() const { return ircViewDock_; }
    IrcMeter* ircMeter() const { return ircMeter_; }
    GeneratorRunner* generatorRunner() const { return runner_; }
    // Whether the document's generators may run without asking: its project
    // was trusted this session (main_window_generators.cpp).
    bool generatorsTrusted() const { return generatorsTrusted_; }

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
    // A project (project::isProject, or a name ending in .bwp, or a
    // directory) opens as one, with its layers (main_window_project.cpp).
    bool openFile(const QString& path, QString* error);
    // Saves to `path` without asking anything, in the document's form if
    // `path` is its file, else in the form the path asks for (fileFormFor):
    // a flat .ans (the layers flattened, project::toAnsi), a .bwp zip or a
    // project directory. A save that would lose data (glyphs written as '?'
    // in an .ans, or blink lost when the file was opened, see
    // Document::encode) is refused unless `allowLossy`. An .ans that can't
    // hold the whole document (Document::flatHoldsAll) leaves it unsaved:
    // from a project it is an export, and the project stays its file.
    bool saveFile(const QString& path, QString* error, bool allowLossy = false);
    // Writes the IRC lines the row meter measures to `path`, a floating
    // selection stamped first, as Save does. Refused, with `error`, if a
    // line is over the budget, unless `allowOverBudget`.
    bool exportIrcText(const QString& path, QString* error, bool allowOverBudget = false);
    // Takes a snapshot of a project (Document::nextSnapshot) and adds it as
    // an undo step, without asking anything: its round, or 0 and `error` for
    // a flat file, no round left, or a lossy snapshot (glyphs written as
    // '?') unless `allowLossy`.
    int snapshot(QString* error, bool allowLossy = false);

public slots:
    // The menu actions, with dialogs.
    void newFile();
    void open();
    // Opens `path`, reporting a failure in a message box.
    void openInteractive(const QString& path);
    // File > Open Project Directory...: a directory picker.
    void openDirectory();
    bool save();
    // Save As in the document's form: a directory picker for a project
    // directory, else the file dialog (.ans or .bwp, by its filter).
    bool saveAs();
    // File > Save As Project Directory... (Save As File... for a project
    // directory): Save As in the other kind of form.
    bool saveAsOtherForm();
    // File > Take Snapshot, asking before a lossy one.
    void takeSnapshot();
    void zoomIn();
    void zoomOut();
    void zoomReset();
    // File > SAUCE...: edits the record's text fields as one undo step.
    void editSauce();
    // Edit > Preferences...: the fonts folder, kept in the settings and
    // scanned into the document's fonts folder when it changes.
    void editPreferences();
    // View > iCE Colours and 9 px Letter Spacing, each one undo step. A 9 px
    // canvas too large to display is refused with a status message.
    void setIceColours(bool on);
    void setNinePx(bool on);
    // Selection > Save to Library...: asks for the name, category and tags.
    void saveToLibrary();
    // Window > Reset Layout: every dock shown, docked where it starts.
    void resetLayout();
    // File > Export IRC Text..., asking before writing lines over the
    // budget, and View > IRC > Budget....
    void exportIrc();
    void editIrcBudget();

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void createActions();
    // The iCE and letter-spacing toggles (main_window_format.cpp).
    void createFormatActions(QMenu* view);
    // The renderer and the toggles follow the document's format.
    void formatChanged();
    void syncFormatActions();
    // File > Export IRC Text... and View > IRC (main_window_irc.cpp).
    void createIrcActions(QMenu* file, QMenu* view);
    void canvasReset();
    void updateTitle();
    void updateStatus();
    void updateGlyphSet();
    // Asks to save unsaved edits; false means cancel.
    bool maybeSave();
    bool saveInteractive(const QString& path, FileForm form);
    // Save As into the directory picked, or through the file dialog.
    bool saveAsDirectory();
    bool saveAsFile();
    // openFile for a project, and saveFile's write in `form`.
    bool openProject(const QString& path, QString* error);
    bool writeFile(const QString& path, FileForm form, QString* error, bool allowLossy);
    // What a save in `form` would lose, for the warning: encode() for .ans,
    // blink lost on open for a project.
    AnsiOutput saveLoss(FileForm form) const;
    // File > Save As Project Directory... and Take Snapshot, after Save As.
    void createProjectActions(QMenu* file);
    void syncProjectActions();
    // The tools, selection, mirror, guides and reference image
    // (main_window_edit.cpp).
    void createEditingActions(QMenu* file, QMenu* edit, QMenu* view);
    // The Library dock and stamp drops on the canvas.
    void createLibrary();
    // The Toolbox, Tool Options, glyph bar, Lint and IRC View docks and the Window menu
    // (main_window_docks.cpp), after the tools and the Library.
    void createDocks();
    // The window's size and dock layout, kept in the settings between runs.
    void saveLayout() const;
    void restoreLayout();
    void applyDefaultDockWidths();
    bool defaultDockWidthsPending_ = true;
    QSize defaultDockWidths_;
    QSize defaultDockWindowSize_;
    // Generator runs, after the trust check (main_window_generators.cpp).
    void createGenerators();
    // Asks, the first time in the session for a project, whether its
    // generators may run (layer `id`'s command shown); true if they may.
    bool trustGenerators(LayerId id);

    Document doc_;
    AtlasRenderer renderer_;
    QScrollArea* scroll_ = nullptr;
    CanvasView* view_ = nullptr;
    IrcMeter* ircMeter_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    QDockWidget* previewDock_ = nullptr;
    PaletteWidget* palette_ = nullptr;
    QDockWidget* paletteDock_ = nullptr;
    ToolOptions* toolOptions_ = nullptr;
    QDockWidget* toolOptionsDock_ = nullptr;
    Toolbox* toolbox_ = nullptr;
    QDockWidget* toolboxDock_ = nullptr;
    GlyphBar* glyphBar_ = nullptr;
    QDockWidget* glyphBarDock_ = nullptr;
    LayersDock* layers_ = nullptr;
    QDockWidget* layersDock_ = nullptr;
    QMenu* layerMenu_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QHash<QString, QAction*> toolActions_;  // the Tools menu's, by tool id
    LibraryDock* library_ = nullptr;
    QDockWidget* libraryDock_ = nullptr;
    LintDock* lint_ = nullptr;
    QDockWidget* lintDock_ = nullptr;
    IrcViewDock* ircView_ = nullptr;
    QDockWidget* ircViewDock_ = nullptr;
    QLabel* status_ = nullptr;
    ToolRegistry* tools_ = nullptr;
    SelectTool* selectTool_ = nullptr;
    QLabel* glyphSetLabel_ = nullptr;
    QList<QAction*> glyphSetActions_;
    QAction* iceAction_ = nullptr;
    QAction* ninePxAction_ = nullptr;
    QAction* otherFormAction_ = nullptr;
    QAction* snapshotAction_ = nullptr;
    GeneratorRunner* runner_ = nullptr;
    QSet<QString> trustedProjects_;  // canonical paths
    bool generatorsTrusted_ = false;
    bool trustFromPath_ = false;  // reset: pathChanged decides from trustedProjects_
};

} // namespace bw
