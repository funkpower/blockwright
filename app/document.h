#pragma once
// An open .ans file: the parsed document, where it lives on disk and whether
// it has unsaved edits. Every change goes through apply(), which reports the
// cells it changed so the views repaint only those.

#include "blockwright/ansi.h"
#include "blockwright/edit.h"

#include <QObject>
#include <QRegion>
#include <QString>

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
    // Replaces the document with `doc`, read from `path`, unmodified.
    void replace(AnsiDocument doc, const QString& path);
    // Starts over on a blank default-size canvas with no file.
    void clear();

    // The bytes save() writes, and what they lose (see writeAnsi).
    AnsiOutput encode() const { return writeAnsi(doc_); }
    // Writes `out` (from encode()) to `path` all or nothing and makes `path`
    // the document's file.
    bool save(const QString& path, const AnsiOutput& out, QString* error);

    const AnsiDocument& ansi() const { return doc_; }
    const Canvas& canvas() const { return doc_.canvas; }
    QString path() const { return path_; }
    bool isModified() const { return modified_; }

    // Applies an edit forwards, or backwards with `revert` (undo), and
    // emits cellsChanged with the cells whose value changed.
    void apply(const CellEdit& edit, bool revert = false);

    // The cells an edit changes: those whose before and after differ.
    static QRegion changedCells(const CellEdit& edit);

signals:
    // Cells (in cell coordinates) that now hold something else.
    void cellsChanged(const QRegion& cells);
    // The whole canvas was replaced (open, new): sizes may have changed.
    void reset();
    void modifiedChanged(bool modified);
    void pathChanged(const QString& path);

private:
    void setModified(bool on);

    AnsiDocument doc_;
    QString path_;
    bool modified_ = false;
};

} // namespace bw
