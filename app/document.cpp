#include "document.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

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

Document::Document(QObject* parent) : QObject(parent), doc_(blank()) {}

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
    doc_ = std::move(doc);
    path_ = path;
    emit reset();
    emit pathChanged(path_);
    setModified(false);
}

void Document::clear() {
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

void Document::apply(const CellEdit& edit, bool revert) {
    const QRegion cells = changedCells(edit) & QRect(0, 0, doc_.canvas.width(), doc_.canvas.height());
    if (cells.isEmpty())
        return;
    if (revert)
        edit.revert(doc_.canvas);
    else
        edit.apply(doc_.canvas);
    emit cellsChanged(cells);
    setModified(true);
}

void Document::setModified(bool on) {
    if (modified_ == on)
        return;
    modified_ = on;
    emit modifiedChanged(on);
}

} // namespace bw
