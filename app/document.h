#pragma once
// An open .ans file: the parsed document, where it lives on disk, its undo
// stack and whether it has unsaved edits. Every edit is pushed with edit()
// as one undo command; the commands change cells through apply(), which
// reports the cells it changed so the views repaint only those. The
// modified flag is the stack's clean state: saving marks the stack clean,
// so undoing back to the saved cells clears it.

#include "blockwright/ansi.h"
#include "blockwright/edit.h"

#include <QObject>
#include <QRegion>
#include <QString>
#include <QUndoStack>

namespace bw {

// What the SAUCE dialog and the iCE and letter-spacing toggles change: the
// SAUCE record (none until something sets one; writeAnsi then writes a
// fresh one) and whether the canvas uses iCE colours.
struct DocumentFormat {
    std::optional<Sauce> sauce;
    bool ice = true;

    bool operator==(const DocumentFormat&) const = default;
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
    // dark background and lose the blink, and `blinkDropped` counts them.
    static bool read(const QString& path, AnsiDocument* out, QString* error, int* blinkDropped);
    // Replaces the document with `doc`, read from `path`, unmodified and
    // with an empty undo stack. `blinkDropped` (from read) is reported by
    // encode() until the document is next saved.
    void replace(AnsiDocument doc, const QString& path, int blinkDropped = 0);
    // Starts over on a blank default-size canvas with no file and an empty
    // undo stack.
    void clear();

    // The bytes save() writes, and what they lose (see writeAnsi). The cells
    // are written as they show with the document's iCE setting (iceCell):
    // with iCE off a bright background is saved as blink, with it on blink
    // is saved as a bright background. So writeAnsi itself drops no blink, and
    // `blinkDropped` is only the blink lost on open (read), whatever has been
    // done to those cells since.
    AnsiOutput encode() const;
    // Writes `out` (from encode()) to `path` all or nothing, makes `path`
    // the document's file and marks the undo stack clean.
    bool save(const QString& path, const AnsiOutput& out, QString* error);

    const AnsiDocument& ansi() const { return doc_; }
    const Canvas& canvas() const { return doc_.canvas; }
    QString path() const { return path_; }
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

    // Pushes `edit` as one undo command, which applies it now. Undo reverts
    // it and emits cursorRequested(before); redo applies it again and emits
    // cursorRequested(after). An edit that changes no cell on the canvas is
    // not pushed, and false is returned.
    bool edit(const CellEdit& edit, Cursor before = {}, Cursor after = {});

    // Applies an edit forwards, or backwards with `revert`, and emits
    // cellsChanged with the cells whose value changed. The undo commands
    // call this; it does not touch the stack or the modified flag.
    void apply(const CellEdit& edit, bool revert = false);

    // The cells an edit changes: those whose before and after differ.
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
    void pathChanged(const QString& path);
    // The SAUCE record or the iCE setting changed (not on open or new; see
    // reset). The cell width may have changed with it.
    void formatChanged();

private:
    void setModified(bool on);

    AnsiDocument doc_;
    QString path_;
    QUndoStack undo_;
    int blinkDropped_ = 0;  // blink the file had and the document lost on open
    bool modified_ = false;
};

} // namespace bw
