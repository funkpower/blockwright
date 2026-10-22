#include "document.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUndoCommand>

#include <cerrno>
#include <cstring>
#include <utility>

namespace bw {

namespace {

AnsiDocument blank() {
    AnsiDocument d;
    d.canvas = Canvas(Canvas::kDefaultWidth, Canvas::kDefaultHeight);
    return d;
}

} // namespace

// One edit on the stack. QUndoStack::push calls redo() at once, so pushing
// is what applies the edit.
class EditCommand : public QUndoCommand {
public:
    EditCommand(Document* doc, CellEdit edit, Cursor before, Cursor after)
        : QUndoCommand(Document::tr("Edit")), doc_(doc), edit_(std::move(edit)), before_(before), after_(after) {}

    void redo() override {
        doc_->apply(edit_);
        emit doc_->cursorRequested(after_);
    }
    void undo() override {
        doc_->apply(edit_, true);
        emit doc_->cursorRequested(before_);
    }

private:
    Document* doc_;
    CellEdit edit_;
    Cursor before_, after_;
};

Document::Document(QObject* parent) : QObject(parent), doc_(blank()) {
    connect(&undo_, &QUndoStack::cleanChanged, this, [this](bool clean) { setModified(!clean); });
}

bool Document::load(const QString& path, QString* error) {
    AnsiDocument doc;
    if (!read(path, &doc, error))
        return false;
    replace(std::move(doc), path);
    return true;
}

bool Document::read(const QString& path, AnsiDocument* out, QString* error) {
    if (QFileInfo(path).isDir()) {
        *error = tr("%1 is a directory").arg(path);
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return false;
    }
    const QByteArray data = f.readAll();
    if (f.error() != QFileDevice::NoError) {
        *error = f.errorString();
        return false;
    }
    if (data.isEmpty()) {
        *error = tr("the file is empty");
        return false;
    }
    try {
        *out = parseAnsi(std::string_view(data.constData(), size_t(data.size())));
    } catch (const AnsiError& e) {
        *error = QString::fromStdString(e.what());
        return false;
    }
    return true;
}

void Document::replace(AnsiDocument doc, const QString& path) {
    undo_.clear();
    doc_ = std::move(doc);
    path_ = path;
    emit reset();
    emit pathChanged(path_);
    setModified(false);
}

void Document::clear() {
    undo_.clear();
    doc_ = blank();
    path_.clear();
    emit reset();
    emit pathChanged(path_);
    setModified(false);
}

bool Document::save(const QString& path, const AnsiOutput& out, QString* error) {
    // QSaveFile writes beside the destination and renames over it on commit,
    // so a failed save never leaves a half-written file.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        *error = f.errorString();
        return false;
    }
    const auto n = qint64(out.bytes.size());
    errno = 0;
    if (f.write(out.bytes.data(), n) != n || !f.flush()) {
        *error = errno ? QString::fromLocal8Bit(std::strerror(errno)) : f.errorString();
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        *error = f.errorString();
        return false;
    }
    if (path_ != path) {
        path_ = path;
        emit pathChanged(path_);
    }
    undo_.setClean();
    setModified(false);
    return true;
}

QRegion Document::changedCells(const CellEdit& edit) {
    QRegion r;
    for (const CellChange& c : edit.changes)
        if (c.before != c.after)
            r |= QRect(c.x, c.y, 1, 1);
    return r;
}

bool Document::edit(const CellEdit& edit, Cursor before, Cursor after) {
    if ((changedCells(edit) & QRect(0, 0, doc_.canvas.width(), doc_.canvas.height())).isEmpty())
        return false;
    undo_.push(new EditCommand(this, edit, before, after));
    return true;
}

void Document::apply(const CellEdit& edit, bool revert) {
    const QRegion cells = changedCells(edit) & QRect(0, 0, doc_.canvas.width(), doc_.canvas.height());
    if (cells.isEmpty())
        return;
    if (revert)
        edit.revert(doc_.canvas);
    else
        edit.apply(doc_.canvas);
    emit cellsChanged(cells);
}

void Document::setModified(bool on) {
    if (modified_ == on)
        return;
    modified_ = on;
    emit modifiedChanged(on);
}

} // namespace bw
