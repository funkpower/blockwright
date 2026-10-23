#include "document.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUndoCommand>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

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

// One format change (SAUCE, iCE, letter spacing) on the stack.
class FormatCommand : public QUndoCommand {
public:
    FormatCommand(Document* doc, DocumentFormat before, DocumentFormat after, const QString& text)
        : QUndoCommand(text), doc_(doc), before_(std::move(before)), after_(std::move(after)) {}

    void redo() override { doc_->applyFormat(after_); }
    void undo() override { doc_->applyFormat(before_); }

private:
    Document* doc_;
    DocumentFormat before_, after_;
};

Document::Document(QObject* parent) : QObject(parent), doc_(blank()) {
    connect(&undo_, &QUndoStack::cleanChanged, this, [this](bool clean) { setModified(!clean); });
}

bool Document::load(const QString& path, QString* error) {
    AnsiDocument doc;
    int blinkDropped = 0;
    if (!read(path, &doc, error, &blinkDropped))
        return false;
    replace(std::move(doc), path, blinkDropped);
    return true;
}

bool Document::read(const QString& path, AnsiDocument* out, QString* error, int* blinkDropped) {
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
    // A file without iCE colours can still hold bright backgrounds (SGR
    // 100-107, 24-bit colours): open it with them on, as writeAnsi writes it
    // and the CLI renders it, and its blinking cells as writeAnsi writes them
    // then. `cli shade` opens files the same way.
    *blinkDropped = openAsEditor(*out).blinkDropped;
    return true;
}

AnsiOutput Document::encode() const {
    AnsiDocument d = doc_;
    for (int y = 0; y < d.canvas.height(); ++y)
        for (int x = 0; x < d.canvas.width(); ++x)
            if (!d.canvas.at(x, y).transparent)
                d.canvas.at(x, y) = iceCell(d.canvas.at(x, y), d.ice);
    AnsiOutput out = writeAnsi(d);
    out.blinkDropped += blinkDropped_;
    return out;
}

bool Document::setFormat(const DocumentFormat& format, const QString& text) {
    const DocumentFormat before = this->format();
    if (format == before)
        return false;
    undo_.push(new FormatCommand(this, before, format, text));
    return true;
}

bool Document::setSauceFields(const Sauce& fields) {
    DocumentFormat f = format();
    if (!f.sauce) {
        f.sauce = Sauce::fresh(doc_.canvas.width(), doc_.canvas.height());
        f.sauce->setIce(f.ice);
    }
    Sauce& s = *f.sauce;
    if (s.title == fields.title && s.author == fields.author && s.group == fields.group && s.date == fields.date &&
        s.comments == fields.comments)
        return false;  // the dialog accepted as it was shown: no record added either
    s.title = fields.title;
    s.author = fields.author;
    s.group = fields.group;
    s.date = fields.date;
    s.comments = fields.comments;
    return setFormat(f, tr("SAUCE"));
}

bool Document::setIce(bool on) {
    DocumentFormat f = format();
    f.ice = on;
    if (f.sauce)
        f.sauce->setIce(on);
    return setFormat(f, on ? tr("iCE Colours On") : tr("iCE Colours Off"));
}

bool Document::setLetterSpacing(int px) {
    DocumentFormat f = format();
    // 8 px is also what an unset flag (or no SAUCE) means: leave that alone
    if ((px == 9) == (f.sauce && f.sauce->letterSpacing() == 2))
        return false;
    if (!f.sauce) {
        f.sauce = Sauce::fresh(doc_.canvas.width(), doc_.canvas.height());
        f.sauce->setIce(f.ice);
    }
    f.sauce->setLetterSpacing(px == 9 ? 2 : 1);
    return setFormat(f, tr("%1 px Letter Spacing").arg(px == 9 ? 9 : 8));
}

void Document::applyFormat(const DocumentFormat& format) {
    doc_.sauce = format.sauce;
    doc_.ice = format.ice;
    emit formatChanged();
}

void Document::replace(AnsiDocument doc, const QString& path, int blinkDropped) {
    undo_.clear();
    doc_ = std::move(doc);
    blinkDropped_ = blinkDropped;
    path_ = path;
    emit reset();
    emit pathChanged(path_);
    setModified(false);
}

void Document::clear() {
    undo_.clear();
    doc_ = blank();
    blinkDropped_ = 0;
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
    blinkDropped_ = 0;  // the file written has none left to lose
    undo_.setClean();
    setModified(false);
    return true;
}

QRegion Document::changedCells(const CellEdit& edit) {
    // One rect per run of cells along a row, unioned pairwise: adding cells
    // to one region one at a time is quadratic in a big fill.
    std::vector<QPoint> cells;
    cells.reserve(edit.changes.size());
    for (const CellChange& c : edit.changes)
        if (c.before != c.after)
            cells.emplace_back(c.x, c.y);
    std::sort(cells.begin(), cells.end(), [](const QPoint& a, const QPoint& b) {
        return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x();
    });
    std::vector<QRegion> rows;
    for (size_t i = 0; i < cells.size();) {
        QRegion row;
        const int y = cells[i].y();
        while (i < cells.size() && cells[i].y() == y) {
            const int x0 = cells[i].x();
            int x1 = x0;
            for (++i; i < cells.size() && cells[i].y() == y && cells[i].x() <= x1 + 1; ++i)
                x1 = cells[i].x();
            row |= QRect(x0, y, x1 - x0 + 1, 1);
        }
        rows.push_back(row);
    }
    for (size_t step = 1; step < rows.size(); step *= 2)
        for (size_t i = 0; i + step < rows.size(); i += 2 * step)
            rows[i] |= rows[i + step];
    return rows.empty() ? QRegion() : rows.front();
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
