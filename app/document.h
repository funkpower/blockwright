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

class Document : public QObject {
    Q_OBJECT
public:
    // A blank canvas of the default size with no file behind it.
    explicit Document(QObject* parent = nullptr);

    // Replaces the document with the file at `path`. On failure the
    // document is unchanged, `error` says why and false is returned.
    bool load(const QString& path, QString* error);
    // Reads and parses the file at `path` into `out` without touching any
    // document; false and `error` on failure.
    static bool read(const QString& path, AnsiDocument* out, QString* error);
    // Replaces the document with `doc`, read from `path`, unmodified and
    // with an empty undo stack.
    void replace(AnsiDocument doc, const QString& path);
    // Starts over on a blank default-size canvas with no file and an empty
    // undo stack.
    void clear();

    // The bytes save() writes, and what they lose (see writeAnsi).
    AnsiOutput encode() const { return writeAnsi(doc_); }
    // Writes `out` (from encode()) to `path` all or nothing, makes `path`
    // the document's file and marks the undo stack clean.
    bool save(const QString& path, const AnsiOutput& out, QString* error);

    const AnsiDocument& ansi() const { return doc_; }
    const Canvas& canvas() const { return doc_.canvas; }
    QString path() const { return path_; }
    bool isModified() const { return modified_; }
    QUndoStack* undoStack() { return &undo_; }

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

private:
    void setModified(bool on);

    AnsiDocument doc_;
    QString path_;
    QUndoStack undo_;
    bool modified_ = false;
};

} // namespace bw
