#pragma once
// An open .ans file: the parsed document, its layers, where it lives on
// disk, its undo stack and whether it has unsaved edits. The cells are the
// layer stack's (PLAN §5); canvas() is their composite, what the views draw
// and save() writes, and layerCanvas() the active layer's, what the tools
// edit. Every edit is pushed with edit() as one undo command holding the
// active layer's cell delta; the commands report the canvas cells whose
// composite changed so the views repaint only those. The layer panel's
// changes (add, delete, reorder, rename, visibility, alpha lock) are undo
// commands too; which layer is active is not. The modified flag is the
// stack's clean state: saving marks the stack clean, so undoing back to the
// saved cells clears it.
//
// The file is a flat .ans or a project (PLAN §6), a .bwp zip or a project
// directory; save() writes the first, saveProject() the others. A project
// opens with its layers, iCE and SAUCE fields, and keeps the rest of what
// it holds (ProjectExtras) to save back.

#include "blockwright/ansi.h"
#include "blockwright/composite.h"
#include "blockwright/edit.h"
#include "blockwright/layer.h"
#include "blockwright/project.h"

#include <QObject>
#include <QRegion>
#include <QString>
#include <QUndoStack>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bw {

// What the SAUCE dialog and the iCE and letter-spacing toggles change: the
// SAUCE record (none until something sets one; writeAnsi then writes a
// fresh one) and whether the canvas uses iCE colours.
struct DocumentFormat {
    std::optional<Sauce> sauce;
    bool ice = true;

    bool operator==(const DocumentFormat&) const = default;
};

// The form of the file behind a document: a flat .ans, or a project as a
// .bwp zip or a directory. Save writes the form it was opened or last saved
// in.
enum class FileForm { Ansi, ProjectZip, ProjectDirectory };

// The form a save to `path` writes: a directory that is there is a project
// directory, a name ending in .bwp (any case) a project zip, anything else a
// flat .ans.
FileForm fileFormFor(const QString& path);

// What a project holds that the editor keeps without showing it: the IRC
// budget, notes.md, the snapshots and the files kept byte for byte (refs/,
// scripts/, thumbnail.png, what a newer minor version added). Opened with a
// project and saved back with it; a flat file has the defaults.
struct ProjectExtras {
    project::IrcBudget ircBudget;
    std::string notes;
    std::vector<project::Snapshot> snapshots;
    std::map<std::string, std::string> files;

    bool operator==(const ProjectExtras&) const = default;
};

class Document : public QObject {
    Q_OBJECT
public:
    // A blank canvas of the default size with no file behind it.
    explicit Document(QObject* parent = nullptr);

    // Replaces the document with the file at `path`. On failure the
    // document is unchanged, `error` says why and false is returned.
    bool load(const QString& path, QString* error);
    // Reads and parses the file at `path` into `out` without touching any
    // document; false and `error` on failure. A file whose iCE flag is off
    // but which sets a bright background opens with iCE on, as writeAnsi
    // writes it and `cli render` draws it: its blinking cells keep their
    // dark background and lose the blink, and those whose blink showed (not
    // on a blank glyph), as writeAnsi counts them, are marked
    // (Cell::lostBlink; openAsEditor).
    static bool read(const QString& path, AnsiDocument* out, QString* error);
    // Replaces the document with `doc`, read from `path`, unmodified and
    // with an empty undo stack: one layer, "Background", holding its cells
    // (LayerStack::fromCanvas), and active. Its marked cells (from read) are
    // the blink encode() reports lost until the document is next saved.
    void replace(AnsiDocument doc, const QString& path);
    // Starts over on a blank default-size canvas (one blank Background
    // layer) with no file and an empty undo stack.
    void clear();

    // Reads the project (a .bwp zip or a project directory) at `path` into
    // `out` without touching any document; false and `error` (what
    // project::read refused) on failure.
    static bool readProject(const QString& path, project::Project* out, QString* error);
    // Replaces the document with `project`, read from `path` in `form`: its
    // layer stack (next layer id included; the bottom layer active), iCE,
    // SAUCE fields and extras, unmodified and with an empty undo stack. No
    // cell counts as lost blink: a project holds none to lose.
    void replace(project::Project project, const QString& path, FileForm form);
    // The document as a project: the layer stack, iCE, SAUCE and extras.
    project::Project project() const;

    // The bytes save() writes, and what they lose (see writeAnsi): the
    // visible composite, flattened through project::toAnsi with the
    // document's iCE and SAUCE fields, as an export or a snapshot writes
    // it. The cells are written as they show with the document's iCE setting (iceCell):
    // with iCE off a bright background is saved as blink, with it on blink
    // is saved as a bright background. So writeAnsi itself drops no blink, and
    // `blinkDropped` is the cells that lost their blink on open (read) and
    // that these bytes still write without one: the marked cells on the
    // canvas, wherever edits have taken them, that don't blink as saved (so
    // all of them with iCE on) and whose glyph shows. A marked cell that
    // blinks again is back; one moved or flipped is still lost; one erased or
    // drawn over is gone with its mark, and a copy carries a mark of its own,
    // so the count can run over what the file lost but never under what the
    // save loses. It gates the save (lossy()) until the next save.
    AnsiOutput encode() const;
    // Writes `out` (from encode()) to `path` all or nothing. Refuses to
    // write over a project (project::isProject), as the CLI does. When the
    // .ans holds the whole document (flatHoldsAll), `path` becomes the
    // document's file and the undo stack is marked clean. Otherwise a
    // project stays the document's file, as unsaved as it was (an export);
    // a flat document takes `path` as its file but stays modified, with no
    // clean state left in its undo stack, until a project save.
    bool save(const QString& path, const AnsiOutput& out, QString* error);
    // Whether a flat .ans holds all of the document: one visible cells
    // layer, at (0, 0) and the canvas's size, compositing normally, and no
    // IRC budget, notes, snapshots or kept files besides it. Its name, id
    // and locks are not art and don't count.
    bool flatHoldsAll() const;
    // Writes project() to `path` as a zip (project::writeZip) or a directory
    // (project::writeDirectory), as `form` says, makes `path` the document's
    // file and `form` its form and marks the undo stack clean. On failure
    // the document is unchanged, `error` says why and false is returned.
    bool saveProject(const QString& path, FileForm form, QString* error);
    // The cells that lost their blink when the file was opened and that a
    // project save would keep without it, as encode() counts them; 0 after
    // any save.
    int lostBlinkOnOpen() const;

    // The snapshot that would be taken now (project::takeSnapshot on
    // project()): the next round and the .ans bytes, and in `out` what
    // writing them lost. Changes nothing; false and `error` if there is no
    // round left.
    bool nextSnapshot(project::Snapshot* snapshot, AnsiOutput* out, QString* error) const;
    // Adds `snapshot` to the project's snapshots as one undo command
    // ("Snapshot rN"), so it sets the modified flag and the next project
    // save writes it; undo takes it off again.
    void addSnapshot(project::Snapshot snapshot);

    // The SAUCE record, iCE and the composite as one flat document.
    const AnsiDocument& ansi() const { return doc_; }
    // The visible composite of the layers.
    const Canvas& canvas() const { return doc_.canvas; }
    QString path() const { return path_; }
    FileForm form() const { return form_; }
    const ProjectExtras& extras() const { return extras_; }
    bool isModified() const { return modified_; }
    QUndoStack* undoStack() { return &undo_; }

    DocumentFormat format() const { return {doc_.sauce, doc_.ice}; }
    // Pushes a format change as one undo command named `text`, which applies
    // it now; false, and nothing pushed, if it changes nothing.
    bool setFormat(const DocumentFormat& format, const QString& text);
    // The SAUCE dialog's record: its text fields and date replace the
    // document's, the rest of the record (flags, font, sizes) stays.
    bool setSauceFields(const Sauce& fields);
    // iCE colours on or off, and the SAUCE iCE flag with them.
    bool setIce(bool on);
    // 8 or 9 px cells: the SAUCE letter-spacing flag, 8 px or 9 px. Adds a
    // fresh SAUCE record if there is none.
    bool setLetterSpacing(int px);
    // Sets the format without the stack; the undo commands call this.
    void applyFormat(const DocumentFormat& format);

    // ---- layers ------------------------------------------------------------

    const LayerStack& layers() const { return stack_; }
    // The layer the tools edit; 0 only while the stack is being replaced.
    LayerId activeLayer() const { return active_; }
    const Layer* active() const { return stack_.find(active_); }
    // Makes the layer active (not an undo step). Emits layersAboutToChange
    // first, so a floating selection lands on the layer it was lifted from,
    // then activeLayerChanged. False if the stack doesn't hold it.
    bool setActiveLayer(LayerId id);
    // The active layer's cells as a canvas the document's size
    // (bw::layerCanvas): what the tools read and build their edits on.
    const Canvas& layerCanvas() const { return layerCanvas_; }
    // What erasing leaves on the active layer: a blank cell on the bottom
    // layer of cells, so the art stays opaque as an .ans is, and a
    // transparent cell anywhere else, so the layer beneath shows.
    Cell blankCell() const;
    // How a paste writes a half pixel on the active layer (pasteCell): as
    // the half-block tools plot on it, with the document's iCE setting.
    PastePixels pastePixels() const;
    // What the visible layers under the active one show at canvas cell
    // (x, y) (compositeCellBelow): what a clear cell of the active layer
    // shows through to, without the layers above it.
    Cell shownBelow(int x, int y) const;
    // Whether edit() takes edits now: an active cells, text or pixel layer
    // that is visible and not locked.
    bool editable() const;

    // The layer panel's changes, each one undo command (QUndoStack) that
    // applies it now; each emits layersAboutToChange before it is pushed.
    // A new layer is the canvas's size, all transparent, named "Layer N",
    // goes just above the active layer and becomes active; undo removes it
    // and makes the layer that was active active again. Returns its id.
    LayerId addLayer(LayerKind kind);
    // Deletes the layer; the one below it (or, for the bottom one, the new
    // bottom) becomes active if it was. The last layer can't be deleted.
    bool removeLayer(LayerId id);
    // Moves the layer to `index` (bottom first, clamped to the top).
    bool moveLayer(LayerId id, std::size_t index);
    // Replaces the layer's properties but its id, as one command named
    // `text`; false if it changes nothing. A change of visibility (or mode,
    // kind, offset) recomposites the cells the layer covers.
    bool setLayerProperties(LayerId id, LayerProperties props, const QString& text);
    bool renameLayer(LayerId id, const QString& name);
    bool setLayerVisible(LayerId id, bool visible);
    bool setLayerAlphaLocked(LayerId id, bool on);

    // Pushes `edit`, built on layerCanvas(), as one undo command on the
    // active layer, which applies it now. Undo reverts it and emits
    // cursorRequested(before); redo applies it again and emits
    // cursorRequested(after). The edit's lost-blink marks are settled first
    // (settleLostBlink): a drawn cell keeps the mark of the cell it is drawn
    // over only when the save writes the two alike, and a moved or copied
    // cell keeps its own. Then it is fitted to the layer (fitLayerEdit):
    // transparent cells stored plain, a half pixel pasted onto a cells
    // layer put over what shows below it (shownBelow), a pixel layer's
    // cells as pixels, and under the alpha lock no cell cleared or made
    // opaque. An edit is refused, and false returned, when the document
    // isn't editable(), when a pixel layer can't hold it (a glyph, a
    // shade), when it is a move the alpha lock would cut (it would clear a
    // cell) and when it changes no cell of the layer.
    bool edit(const CellEdit& edit, Cursor before = {}, Cursor after = {});
    // What `edit` (built on layerCanvas()) would make of the composite,
    // without applying it: each cell it changes, before and after, as the
    // canvas shows it. Empty when edit() would refuse it. The tools preview
    // their drags with it.
    CellEdit previewEdit(const CellEdit& edit);

    // The cells an edit changes: those whose before and after differ
    // (identical, so a mark moving counts).
    static QRegion changedCells(const CellEdit& edit);

signals:
    // Cells (in cell coordinates) that now hold something else.
    void cellsChanged(const QRegion& cells);
    // The whole canvas was replaced (open, new): sizes may have changed.
    void reset();
    void modifiedChanged(bool modified);
    // An undo command was done or undone: the cursor goes back to where it
    // was when the edit was made (undo) or where the edit left it (redo).
    void cursorRequested(bw::Cursor cursor);
    // The file behind the document or its form (form()) changed.
    void pathChanged(const QString& path);
    // The SAUCE record or the iCE setting changed (not on open or new; see
    // reset). The cell width may have changed with it.
    void formatChanged();
    // A layer panel change or a change of active layer is about to be made:
    // a tool lands what it holds on the layer it was editing.
    void layersAboutToChange();
    // The stack's layers or their properties changed (also on open, new,
    // and their undo and redo).
    void layersChanged();
    void activeLayerChanged(bw::LayerId id);

private:
    friend class LayerEditCommand;
    friend class LayerStackCommand;
    friend class LayerPropsCommand;
    friend class SnapshotCommand;

    void setModified(bool on);
    CompositeOptions compositeOptions() const { return {doc_.ice, Cell{}}; }
    // shownBelow, for fitLayerEdit
    std::function<Cell(int, int)> below() const;
    // Starts over with `stack`, its bottom layer active.
    void resetLayers(LayerStack stack);
    // The layer edit (Layer::cells coordinates) applied to layer `id`
    // forwards or backwards: the layer, the composite and layerCanvas()
    // updated and cellsChanged emitted.
    void applyLayerEdit(LayerId id, const CellEdit& layerEdit, bool revert);
    // Recomposites the canvas cells in `rect` (clipped to the canvas) and
    // returns those whose composite changed.
    QRegion recomposite(CellRect rect);
    // The layer commands' steps: no undo, no layersAboutToChange.
    LayerId insertLayer(Layer layer, std::size_t index);
    Layer takeLayer(LayerId id);
    void placeLayer(LayerId id, std::size_t index);
    void putProperties(LayerId id, const LayerProperties& props);
    void makeActive(LayerId id);
    void refreshLayerCanvas();

    AnsiDocument doc_;
    LayerStack stack_;
    LayerId active_ = 0;
    Canvas layerCanvas_{0, 0};
    QString path_;
    FileForm form_ = FileForm::Ansi;
    ProjectExtras extras_;
    QUndoStack undo_;
    // The marked cells count as lost (encode): from an open that marked
    // some until the next save, which leaves no blink of that file to lose.
    bool countLostBlink_ = false;
    bool modified_ = false;
};

} // namespace bw
