#include "document.h"
#include "font_scanner.h"

#include "blockwright/style_draw.h"
#include "blockwright/text.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopedValueRollback>
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

std::filesystem::path fspath(const QString& s) {
    return std::filesystem::path(QFile::encodeName(s).toStdString());
}

} // namespace

FileForm fileFormFor(const QString& path) {
    const QFileInfo fi(path);
    if (fi.isDir())
        return FileForm::ProjectDirectory;
    if (fi.suffix().compare(QLatin1String("bwp"), Qt::CaseInsensitive) == 0)
        return FileForm::ProjectZip;
    return FileForm::Ansi;
}

// One edit on the stack: a layer's cell delta, in its own cells.
// QUndoStack::push calls redo() at once, so pushing is what applies the
// edit.
class LayerEditCommand : public QUndoCommand {
public:
    LayerEditCommand(Document* doc, LayerId id, CellEdit edit, Cursor before, Cursor after)
        : QUndoCommand(Document::tr("Edit")), doc_(doc), id_(id), edit_(std::move(edit)), before_(before),
          after_(after) {}

    void redo() override {
        doc_->applyLayerEdit(id_, edit_, false);
        emit doc_->cursorRequested(after_);
    }
    void undo() override {
        doc_->applyLayerEdit(id_, edit_, true);
        emit doc_->cursorRequested(before_);
    }

private:
    Document* doc_;
    LayerId id_;
    CellEdit edit_;
    Cursor before_, after_;
};

// Adding, deleting or moving a layer. The layer keeps its id through undo
// and redo, so the edits on the stack that name it still find it.
class LayerStackCommand : public QUndoCommand {
public:
    enum class Op { Add, Remove, Move };

    // Add `layer` (id 0) at `index` and make it active.
    static LayerStackCommand* add(Document* doc, Layer layer, std::size_t index, const QString& text) {
        auto* c = new LayerStackCommand(doc, Op::Add, text);
        c->layer_ = std::move(layer);
        c->index_ = index;
        c->activeBefore_ = doc->activeLayer();
        return c;
    }
    // Delete layer `id`; `activeAfter` is active once it has gone.
    static LayerStackCommand* remove(Document* doc, LayerId id, LayerId activeAfter, const QString& text) {
        auto* c = new LayerStackCommand(doc, Op::Remove, text);
        c->layer_.props.id = id;
        c->index_ = *doc->layers().indexOf(id);
        c->activeBefore_ = doc->activeLayer();
        c->activeAfter_ = activeAfter;
        return c;
    }
    static LayerStackCommand* move(Document* doc, LayerId id, std::size_t to, const QString& text) {
        auto* c = new LayerStackCommand(doc, Op::Move, text);
        c->layer_.props.id = id;
        c->from_ = *doc->layers().indexOf(id);
        c->index_ = to;
        return c;
    }

    LayerId layerId() const { return layer_.props.id; }
    bool adds(LayerId id) const { return op_ == Op::Add && layer_.props.id == id; }

    void redo() override {
        switch (op_) {
        case Op::Add: {
            const LayerId id = doc_->insertLayer(layer_, index_);
            layer_.props.id = id;
            doc_->makeActive(id);
            break;
        }
        case Op::Remove: {
            const LayerId id = layer_.props.id;
            layer_ = doc_->takeLayer(id);
            doc_->makeActive(activeAfter_);
            break;
        }
        case Op::Move: doc_->placeLayer(layer_.props.id, index_); break;
        }
    }
    void undo() override {
        switch (op_) {
        case Op::Add:
            layer_ = doc_->takeLayer(layer_.props.id);
            doc_->makeActive(activeBefore_);
            break;
        case Op::Remove:
            doc_->insertLayer(layer_, index_);
            doc_->makeActive(activeBefore_);
            break;
        case Op::Move: doc_->placeLayer(layer_.props.id, from_); break;
        }
    }

private:
    LayerStackCommand(Document* doc, Op op, const QString& text) : QUndoCommand(text), doc_(doc), op_(op) {}

    Document* doc_;
    Op op_;
    // The layer (Add, and Remove once done) or just its id (Move).
    Layer layer_;
    std::size_t index_ = 0, from_ = 0;
    LayerId activeBefore_ = 0, activeAfter_ = 0;
};

// A layer's properties (name, visibility, alpha lock, a text source),
// before and after. The keystrokes of one run of typing fold into one step
// (mergeEdits).
//
// On a generator layer the step can also carry its generator's params.json
// and cells, before and after (Document::setGeneratorParam): a run landing
// later amends the cells after, so redo gives back what it drew.
class LayerPropsCommand : public QUndoCommand {
public:
    struct GeneratorStep {
        std::uint64_t serial = 0;
        std::optional<std::string> paramsBefore, paramsAfter;
        Canvas cellsBefore{0, 0}, cellsAfter{0, 0};
    };

    LayerPropsCommand(Document* doc, LayerPropsEdit edit, const QString& text,
                      std::optional<GeneratorStep> generator = std::nullopt)
        : QUndoCommand(text), doc_(doc), edit_(std::move(edit)), generator_(std::move(generator)) {}

    void redo() override {
        doc_->putProperties(edit_, false);
        if (generator_)
            doc_->putGenerator(edit_.id, generator_->paramsAfter, generator_->cellsAfter);
    }
    void undo() override {
        doc_->putProperties(edit_, true);
        if (generator_)
            doc_->putGenerator(edit_.id, generator_->paramsBefore, generator_->cellsBefore);
    }

    LayerId layerId() const { return edit_.id; }
    int id() const override { return edit_.typingRun ? kTypingId : -1; }
    bool mergeWith(const QUndoCommand* other) override {
        return mergeEdits(edit_, static_cast<const LayerPropsCommand*>(other)->edit_);
    }
    const std::optional<GeneratorStep>& generator() const { return generator_; }
    void setCellsAfter(Canvas cells) { generator_->cellsAfter = std::move(cells); }

private:
    static constexpr int kTypingId = 0x5459;  // "TY"
    Document* doc_;
    LayerPropsEdit edit_;
    std::optional<GeneratorStep> generator_;
};

// A step that does nothing and leaves the stack when undone: pushed where
// the undo stack stands, it drops every step after it, and its undo then
// takes it away too, so nothing is left to redo (Document::dropNewTextLayer).
class DropRedoCommand : public QUndoCommand {
public:
    void redo() override {}
    void undo() override { setObsolete(true); }
};

// A snapshot added to the project's snapshots; undo takes it off.
class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(Document* doc, project::Snapshot snapshot)
        : QUndoCommand(Document::tr("Snapshot r%1").arg(snapshot.round)), doc_(doc), snapshot_(std::move(snapshot)) {}

    // A flat file keeps no snapshots: one redone after a save made the
    // document an .ans (#115) is dropped from the stack instead.
    void redo() override {
        if (doc_->form_ == FileForm::Ansi) {
            setObsolete(true);
            return;
        }
        doc_->extras_.snapshots.push_back(snapshot_);
    }
    // Snapshots change only through these commands and open/new (which
    // clear the stack), so the one to take off is the last.
    void undo() override {
        auto& v = doc_->extras_.snapshots;
        if (!v.empty() && v.back() == snapshot_)
            v.pop_back();
    }

private:
    Document* doc_;
    project::Snapshot snapshot_;
};

// A change of the project's IRC budget.
class IrcBudgetCommand : public QUndoCommand {
public:
    IrcBudgetCommand(Document* doc, project::IrcBudget before, project::IrcBudget after)
        : QUndoCommand(Document::tr("IRC Budget")), doc_(doc), before_(std::move(before)), after_(std::move(after)) {}

    void redo() override { doc_->applyIrcBudget(after_); }
    void undo() override { doc_->applyIrcBudget(before_); }

private:
    Document* doc_;
    project::IrcBudget before_, after_;
};

// One format change (SAUCE, iCE, letter spacing) on the stack. An iCE
// toggle also redraws each live text layer's style (iceEdit), its cells
// before and after kept, so undo and redo put back exactly what was shown.
class FormatCommand : public QUndoCommand {
public:
    FormatCommand(Document* doc, DocumentFormat before, DocumentFormat after, const QString& text,
                  std::vector<LayerPropsEdit> restyles = {})
        : QUndoCommand(text), doc_(doc), before_(std::move(before)), after_(std::move(after)),
          restyles_(std::move(restyles)) {}

    void redo() override {
        doc_->applyFormat(after_);
        for (const LayerPropsEdit& e : restyles_)
            doc_->putProperties(e, false);
    }
    void undo() override {
        doc_->applyFormat(before_);
        for (auto it = restyles_.rbegin(); it != restyles_.rend(); ++it)
            doc_->putProperties(*it, true);
    }

private:
    Document* doc_;
    DocumentFormat before_, after_;
    std::vector<LayerPropsEdit> restyles_;
};

Document::Document(QObject* parent) : QObject(parent), doc_(blank()) {
    resetLayers(LayerStack::fromCanvas(doc_.canvas));
    connect(&undo_, &QUndoStack::cleanChanged, this, [this](bool clean) { setModified(!clean); });
    connect(&undo_, &QUndoStack::indexChanged, this, [this] { ++undoMoves_; });
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
        const std::string_view bytes(data.constData(), size_t(data.size()));
        *out = parseArt(bytes, artFormatOf(QFile::encodeName(path).toStdString(), bytes));
    } catch (const AnsiError& e) {
        *error = QString::fromStdString(e.what());
        return false;
    }
    // A file without iCE colours can still hold bright backgrounds (SGR
    // 100-107, 24-bit colours): open it with them on, as writeAnsi writes it
    // and the CLI renders it, and its blinking cells as writeAnsi writes them
    // then, marked as lost. `cli shade` opens files the same way.
    openAsEditor(*out);
    return true;
}

bool Document::readProject(const QString& path, project::Project* out, QString* error) {
    try {
        *out = project::read(fspath(path));
    } catch (const std::exception& e) {
        *error = QString::fromStdString(e.what());
        return false;
    }
    return true;
}

void Document::replace(project::Project p, const QString& path, FileForm form) {
    undo_.clear();
    doc_ = AnsiDocument{};
    doc_.ice = p.ice;
    doc_.sauce = std::move(p.sauce);
    resetLayers(std::move(p.layers));
    extras_ = {std::move(p.ircBudget), std::move(p.notes), std::move(p.snapshots), std::move(p.files),
               std::move(p.fonts)};
    countLostBlink_ = false;
    path_ = path;
    form_ = form;
    emit reset();
    emit layersChanged();
    emit activeLayerChanged(active_);
    emit pathChanged(path_);
    setModified(false);
}

void Document::markUnsaved() {
    undo_.resetClean();
    setModified(true);
}

project::Project Document::project() const {
    project::Project p;
    p.layers = stack_;
    p.ice = doc_.ice;
    p.sauce = doc_.sauce;
    p.ircBudget = extras_.ircBudget;
    p.notes = extras_.notes;
    p.snapshots = extras_.snapshots;
    p.files = extras_.files;
    p.fonts = extras_.fonts;
    return p;
}

AnsiOutput Document::encode(ArtFormat format) const {
    project::Project p;
    p.layers = stack_;
    p.ice = doc_.ice;
    p.sauce = doc_.sauce;
    AnsiDocument flat = project::toAnsi(p);
    flat.palette = doc_.palette;
    flat.font = doc_.font;
    AnsiOutput out = writeArt(flat, format);
    out.blinkDropped += lostBlinkOnOpen();
    return out;
}

int Document::lostBlinkOnOpen() const {
    return countLostBlink_ ? lostBlinkCount(doc_) : 0;
}

bool Document::nextSnapshot(project::Snapshot* snapshot, AnsiOutput* out, QString* error) const {
    project::Project p;
    p.layers = stack_;
    p.ice = doc_.ice;
    p.sauce = doc_.sauce;
    p.snapshots = extras_.snapshots;
    try {
        *out = project::takeSnapshot(p);
    } catch (const std::exception& e) {
        *error = QString::fromStdString(e.what());
        return false;
    }
    *snapshot = std::move(p.snapshots.back());
    return true;
}

void Document::addSnapshot(project::Snapshot snapshot) {
    emit commandAboutToPush();
    undo_.push(new SnapshotCommand(this, std::move(snapshot)));
}

bool Document::setIrcBudget(const project::IrcBudget& budget) {
    emit commandAboutToPush();
    if (budget == extras_.ircBudget)
        return false;
    undo_.push(new IrcBudgetCommand(this, extras_.ircBudget, budget));
    return true;
}

void Document::applyIrcBudget(const project::IrcBudget& budget) {
    extras_.ircBudget = budget;
    emit ircBudgetChanged();
}

bool Document::setFormat(const DocumentFormat& format, const QString& text) {
    emit commandAboutToPush();
    const DocumentFormat before = this->format();
    if (format == before)
        return false;
    std::vector<LayerPropsEdit> restyles;
    if (format.ice != before.ice)
        for (const Layer& l : stack_.layers()) {
            try {
                if (auto e = iceEdit(l, format.ice, fontSources()))
                    restyles.push_back(std::move(*e));
            } catch (const TextError&) {
                // a source that can't be drawn now (its font gone) keeps
                // the cells it has
            }
        }
    if (!restyles.empty())
        emit layersAboutToChange();
    undo_.push(new FormatCommand(this, before, format, text, std::move(restyles)));
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
    const bool ice = doc_.ice != format.ice;
    doc_.ice = format.ice;
    // iCE changes how pixel layers fit over what is under them, and the
    // rungs a style the compositor draws writes.
    const bool pixels = std::any_of(stack_.layers().begin(), stack_.layers().end(), [](const Layer& l) {
        return l.props.kind == LayerKind::Pixels || styledAtComposite(l.props);
    });
    const QRegion cells = ice && pixels ? recomposite(stack_.canvasRect()) : QRegion();
    emit formatChanged();
    if (!cells.isEmpty())
        emit cellsChanged(cells);
}

void Document::replace(AnsiDocument doc, const QString& path) {
    undo_.clear();
    doc_ = std::move(doc);
    resetLayers(LayerStack::fromCanvas(doc_.canvas));
    countLostBlink_ = false;
    const Canvas& cv = doc_.canvas;
    for (int y = 0; y < cv.height() && !countLostBlink_; ++y)
        for (int x = 0; x < cv.width() && !countLostBlink_; ++x)
            countLostBlink_ = cv.at(x, y).lostBlink;
    extras_ = {};
    path_ = path;
    form_ = FileForm::Ansi;
    emit reset();
    emit layersChanged();
    emit activeLayerChanged(active_);
    emit pathChanged(path_);
    setModified(false);
}

void Document::clear() { clear(Canvas::kDefaultWidth, Canvas::kDefaultHeight, true); }

void Document::clear(int width, int height, bool ice) {
    undo_.clear();
    doc_ = AnsiDocument{};
    doc_.canvas = Canvas(width, height);
    doc_.ice = ice;
    resetLayers(LayerStack::fromCanvas(doc_.canvas));
    extras_ = {};
    countLostBlink_ = false;
    path_.clear();
    form_ = FileForm::Ansi;
    emit reset();
    emit layersChanged();
    emit activeLayerChanged(active_);
    emit pathChanged(path_);
    setModified(false);
}

bool Document::save(const QString& path, const AnsiOutput& out, QString* error) {
    if (project::isProject(fspath(path))) {
        *error = tr("%1 is a project; save it as a project or choose another name").arg(path);
        return false;
    }
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
    emit fileWritten(path);
    // An .ans holding less than the document doesn't save it: a project
    // stays the document's file, and a flat document stays modified, so
    // nothing discards the layers or snapshots on no disk without asking.
    const bool whole = flatHoldsAll();
    if (!whole && form_ != FileForm::Ansi)
        return true;
    if (path_ != path || form_ != FileForm::Ansi) {
        path_ = path;
        form_ = FileForm::Ansi;
        emit pathChanged(path_);
    }
    countLostBlink_ = false;  // the file written has no blink left to lose
    if (artFormatFor(QFile::encodeName(path).toStdString()) != ArtFormat::Xbin) {
        // nor a palette or font, now it is the document's file
        doc_.palette.reset();
        doc_.font.reset();
    } else if (doc_.sauce && (out.letterSpacingDropped || out.aspectDropped || !out.fontNameDropped.empty())) {
        // nor the SAUCE letter spacing, aspect or font name an XBIN left out
        if (out.letterSpacingDropped)
            doc_.sauce->setLetterSpacing(0);
        if (out.aspectDropped)
            doc_.sauce->setAspectRatio(0);
        if (!out.fontNameDropped.empty())
            doc_.sauce->font.clear();
        emit formatChanged();
    }
    if (whole) {
        undo_.setClean();
        setModified(false);
    } else {
        // Undoing back to how the file was opened no longer matches it.
        undo_.resetClean();
        setModified(true);
    }
    return true;
}

bool Document::flatHoldsAll() const {
    // the font copies count only through text layers that use them, and one
    // cells layer uses none: those left are kept for a redo, not art
    const ProjectExtras none;
    if (stack_.size() != 1 || !(extras_.ircBudget == none.ircBudget && extras_.notes == none.notes &&
                                extras_.snapshots == none.snapshots && extras_.files == none.files))
        return false;
    // an .ans holds a styled layer's look, not its cells and style
    const Layer& l = stack_[0];
    return l.props.kind == LayerKind::Cells && !l.props.style && l.props.visible && l.props.mode == LayerMode::Normal &&
           l.props.x == 0 && l.props.y == 0 && l.width() == stack_.width() && l.height() == stack_.height();
}

bool Document::saveProject(const QString& path, FileForm form, QString* error) {
    try {
        const project::Project p = project();
        if (form == FileForm::ProjectDirectory)
            project::writeDirectory(p, fspath(path));
        else
            project::writeZip(p, fspath(path));
    } catch (const std::exception& e) {
        *error = QString::fromStdString(e.what());
        return false;
    }
    emit fileWritten(path);
    if (path_ != path || form_ != form) {
        path_ = path;
        form_ = form;
        emit pathChanged(path_);
    }
    countLostBlink_ = false;
    doc_.palette.reset();  // a project keeps neither
    doc_.font.reset();
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
        if (!identical(c.before, c.after))
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
    if (!editable())
        return false;
    const std::optional<CellEdit> e = fitLayerEdit(*active(), settleMarks(edit), below(), doc_.ice);
    if (!e || e->changes.empty())
        return false;
    undo_.push(new LayerEditCommand(this, active_, *e, before, after));
    return true;
}

CellEdit Document::previewEdit(const CellEdit& edit, CellEdit* layerCells) {
    if (layerCells)
        *layerCells = {};
    if (!editable())
        return {};
    Layer& layer = *stack_.find(active_);
    const std::optional<CellEdit> e = fitLayerEdit(layer, settleMarks(edit), below(), doc_.ice);
    if (!e)
        return {};
    // Applied for the moment, to read the composite, and taken off again.
    e->apply(layer.cells);
    CellEdit out;
    const CompositeOptions opts = compositeOptions();
    // what the layer covers of a reference layer under it, as it will
    const bool cover = layerCells && showsReference();
    if (styledAtComposite(layer.props)) {
        // the style redrawn over the whole layer: each cell it changes
        const CellRect rect = intersect(dirtyRect(layer, *e), stack_.canvasRect());
        const Canvas fresh = compositeRect(stack_, rect, opts);
        for (int y = rect.y; y < rect.y + rect.height; ++y)
            for (int x = rect.x; x < rect.x + rect.width; ++x)
                if (const Cell& now = fresh.at(x - rect.x, y - rect.y); !identical(now, doc_.canvas.at(x, y)))
                    out.changes.push_back({x, y, doc_.canvas.at(x, y), now});
        if (cover && !rect.empty()) {
            const Canvas styled = styledCells(layer, doc_.ice);
            layerCells->changes.reserve(static_cast<std::size_t>(rect.width) * rect.height);
            for (int y = rect.y; y < rect.y + rect.height; ++y)
                for (int x = rect.x; x < rect.x + rect.width; ++x)
                    layerCells->changes.push_back(
                        {x, y, {}, styled.at(x - layer.props.x, y - layer.props.y)});
        }
    } else {
        out.changes.reserve(e->changes.size());
        for (const CellChange& c : e->changes) {
            const int x = c.x + layer.props.x, y = c.y + layer.props.y;
            if (!doc_.canvas.contains(x, y))
                continue;
            out.changes.push_back({x, y, doc_.canvas.at(x, y), compositeCell(stack_, x, y, opts)});
            if (cover)
                layerCells->changes.push_back({x, y, c.before, c.after});
        }
    }
    e->revert(layer.cells);
    return out;
}

// ---- layers -------------------------------------------------------------

bool Document::setActiveLayer(LayerId id) {
    if (!stack_.find(id))
        return false;
    if (id == active_)
        return true;
    emit layersAboutToChange();
    emit activeLayerAboutToChange(id);
    makeActive(id);
    return true;
}

Cell Document::blankCell() const {
    const Layer* l = active();
    if (l && l->props.kind != LayerKind::Pixels && stack_.indexOf(active_) == std::size_t(0))
        return Cell{};
    return transparentCell();
}

PastePixels Document::pastePixels() const {
    const Layer* l = active();
    return {l && l->props.kind == LayerKind::Pixels, doc_.ice};
}

Cell Document::shownBelow(int x, int y) const {
    return compositeCellBelow(stack_, stack_.indexOf(active_).value_or(0), x, y, compositeOptions());
}

std::function<Cell(int, int)> Document::below() const {
    return [this](int x, int y) { return shownBelow(x, y); };
}

bool Document::editable() const {
    const Layer* l = active();
    if (!l || !l->props.visible || l->props.locked)
        return false;
    switch (l->props.kind) {
    case LayerKind::Cells:
    case LayerKind::Pixels: return true;
    // a live text layer's cells are drawn from its source
    case LayerKind::Text: return !l->props.text;
    case LayerKind::Generator:
    case LayerKind::Reference: return false;
    }
    return false;
}

std::string Document::freeLayerName(const QString& pattern) const {
    int n = 1;
    const auto taken = [this](const std::string& name) {
        return std::any_of(stack_.layers().begin(), stack_.layers().end(),
                           [&](const Layer& l) { return l.props.name == name; });
    };
    while (taken(pattern.arg(n).toStdString()))
        ++n;
    return pattern.arg(n).toStdString();
}

LayerId Document::addLayer(LayerKind kind) {
    emit commandAboutToPush();
    std::string name = freeLayerName(tr("Layer %1"));
    emit layersAboutToChange();
    const std::optional<std::size_t> at = stack_.indexOf(active_);
    Layer layer = makeLayer(kind, stack_.width(), stack_.height(), std::move(name));
    auto* c = LayerStackCommand::add(this, std::move(layer), at ? *at + 1 : stack_.size(),
                                     kind == LayerKind::Pixels ? tr("New Pixels Layer") : tr("New Cells Layer"));
    undo_.push(c);
    return c->layerId();
}

LayerId Document::addTextLayer(const TextSource& source, int x, int y, const FontStore* held,
                               const std::optional<TextStyle>& style) {
    emit commandAboutToPush();
    FontSources from = fontSources();
    from.held = held;
    if (style ? styledTextProblem(source, *style, from) : textSourceProblem(source, from))
        return 0;
    keepFont(source.face, held);
    Layer layer = makeTextLayer(source, freeLayerName(tr("Text %1")), x, y, fontSources());
    if (style) {
        LayerProperties props = layer.props;
        props.style = style;
        applyLayerProperties(layer, std::move(props), fontSources(), doc_.ice);
    }
    emit layersAboutToChange();
    const std::optional<std::size_t> at = stack_.indexOf(active_);
    auto* c = LayerStackCommand::add(this, std::move(layer), at ? *at + 1 : stack_.size(), tr("New Text Layer"));
    undo_.push(c);
    return c->layerId();
}

LayerId Document::addReferenceLayer(ReferenceImage picture) {
    emit commandAboutToPush();
    Layer layer = makeLayer(LayerKind::Reference, 0, 0, freeLayerName(tr("Reference %1")));
    layer.props.reference = std::move(picture);
    if (project::layerReferenceProblem(layer.props))
        return 0;
    emit layersAboutToChange();
    auto* c = LayerStackCommand::add(this, std::move(layer), stack_.size(), tr("New Reference Layer"));
    undo_.push(c);
    return c->layerId();
}

bool Document::dropNewTextLayer(LayerId id) {
    const Layer* l = stack_.find(id);
    if (!l || !l->props.text || !l->props.text->text.empty())
        return false;
    // the step that added the layer, with nothing after it, done or undone,
    // but changes to the layer's own properties (its typing)
    int added = -1;
    for (int i = undo_.count() - 1; i >= 0 && added < 0; --i) {
        const QUndoCommand* c = undo_.command(i);
        if (const auto* s = dynamic_cast<const LayerStackCommand*>(c); s && s->adds(id))
            added = i;
        else if (const auto* p = dynamic_cast<const LayerPropsCommand*>(c); !p || p->layerId() != id)
            return false;
    }
    if (added < 0 || undo_.index() <= added)
        return false;
    emit layersAboutToChange();
    undo_.setIndex(added);
    undo_.push(new DropRedoCommand);
    undo_.undo();
    return true;
}

bool Document::setTextSource(LayerId id, TextSource source, const QString& text, std::uint64_t typingRun,
                             const FontStore* held) {
    emit commandAboutToPush();
    FontSources from = fontSources();
    from.held = held;
    const Layer* l = stack_.find(id);
    if (!l || l->props.kind != LayerKind::Text || !l->props.text || *l->props.text == source ||
        textSourceProblem(source, from))
        return false;
    keepFont(source.face, held);
    emit layersAboutToChange();
    undo_.push(new LayerPropsCommand(this, textEdit(*l, std::move(source), typingRun, doc_.ice), text));
    return true;
}

bool Document::canBake(const Layer* l) {
    if (!l)
        return false;
    if (l->props.kind == LayerKind::Text)
        return bool(l->props.text);
    return (l->props.kind == LayerKind::Cells || l->props.kind == LayerKind::Pixels) && l->props.style;
}

bool Document::bakeLayer(LayerId id) {
    emit commandAboutToPush();
    const Layer* l = stack_.find(id);
    if (!canBake(l))
        return false;
    const bool text = l->props.kind == LayerKind::Text;
    emit layersAboutToChange();
    undo_.push(new LayerPropsCommand(this, bakeEdit(*l, doc_.ice), text ? tr("Bake Text") : tr("Bake Layer Style")));
    return true;
}

bool Document::removeLayer(LayerId id) {
    emit commandAboutToPush();
    const std::optional<std::size_t> at = stack_.indexOf(id);
    if (!at || stack_.size() < 2)
        return false;
    emit layersAboutToChange();
    LayerId after = active_;
    if (id == active_)
        after = stack_[*at > 0 ? *at - 1 : 1].props.id;
    undo_.push(LayerStackCommand::remove(this, id, after, tr("Delete Layer")));
    return true;
}

bool Document::moveLayer(LayerId id, std::size_t index) {
    emit commandAboutToPush();
    const std::optional<std::size_t> at = stack_.indexOf(id);
    if (!at || stack_.empty())
        return false;
    index = std::min(index, stack_.size() - 1);
    if (index == *at)
        return false;
    emit layersAboutToChange();
    undo_.push(LayerStackCommand::move(this, id, index, index > *at ? tr("Raise Layer") : tr("Lower Layer")));
    return true;
}

void Document::previewLayerOffset(LayerId id, CellRect from, QPoint to) {
    Layer* layer = stack_.find(id);
    QRegion cells;
    CellRect moved = from;
    if (layer) {
        const QScopedValueRollback<int> x(layer->props.x, to.x()), y(layer->props.y, to.y());
        moved = unite(from, layer->bounds(), stack_.canvasRect());
        cells = recomposite(moved);
    } else
        cells = recomposite(from);
    if (!cells.isEmpty())
        emit cellsChanged(cells);
    if (!showsReference())
        return;
    // what it covers moved: the cells it shows otherwise at the two offsets
    moved = intersect(moved, stack_.canvasRect());
    if (!layer || styledAtComposite(layer->props))
        touchLayerCells(QRect(moved.x, moved.y, moved.width, moved.height));
    else
        touchLayerCells(otherCells(layer->cells, {from.x, from.y}, layer->cells, to, moved));
}

bool Document::setLayerProperties(LayerId id, LayerProperties props, const QString& text) {
    emit commandAboutToPush();
    const Layer* l = stack_.find(id);
    if (!l)
        return false;
    const LayerProperties before = l->props;
    props.id = id;
    if (props == before)
        return false;
    // a text source only on a text layer, and one that can be drawn
    if (props.text && (props.kind != LayerKind::Text || textSourceProblem(*props.text, fontSources())))
        return false;
    // a style the layer can hold and can draw, the box it grows included
    if (layerStyleProblem(props, fontSources()))
        return false;
    if (props.text)
        keepFont(props.text->face);
    // A name or the alpha lock changes no cell: a tool keeps what it holds
    // (a drag redoes itself with the new lock).
    LayerProperties same = props;
    same.name = before.name;
    same.alphaLocked = before.alphaLocked;
    if (same != before)
        emit layersAboutToChange();
    undo_.push(new LayerPropsCommand(this, propsEdit(*l, std::move(props), 0, doc_.ice), text));
    return true;
}

std::optional<std::string> Document::keptFile(const std::string& path) const {
    const auto it = extras_.files.find(path);
    return it == extras_.files.end() ? std::nullopt : std::optional(it->second);
}

std::optional<generator::Spec> Document::generatorSpec(LayerId id, QString* error) const {
    project::Project p;
    p.layers = stack_;
    p.files = extras_.files;
    try {
        return generator::layerSpec(p, id);
    } catch (const generator::GeneratorError& e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return std::nullopt;
    }
}

namespace {

std::string paramsPath(LayerId id) { return generator::directory(id) + std::string(generator::kParamsFile); }
std::string specPath(LayerId id) { return generator::directory(id) + std::string(generator::kSpecFile); }

} // namespace

bool Document::setGeneratorParam(LayerId id, const std::string& name, const generator::ParamValue& value,
                                 QString* error) {
    const std::optional<generator::Spec> spec = generatorSpec(id, error);
    if (!spec)
        return false;
    const std::optional<std::string> before = keptFile(paramsPath(id));
    std::string after;
    try {
        after = generator::withParam(*spec, before.value_or(std::string()), name, value);
    } catch (const generator::GeneratorError& e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return false;
    }
    if (before == after)
        return false;
    const Layer& l = *stack_.find(id);
    const std::uint64_t serial = ++generatorSerial_;
    undo_.push(new LayerPropsCommand(this, propsEdit(l, l.props), tr("Generator Parameter"),
                                     LayerPropsCommand::GeneratorStep{serial, before, after, l.cells, l.cells}));
    emit generatorRunWanted(id, serial);
    return true;
}

bool Document::requestGeneratorRun(LayerId id) {
    const Layer* l = stack_.find(id);
    if (!l || l->props.kind != LayerKind::Generator)
        return false;
    runAsked_[id] = undoMoves_;
    emit generatorRunWanted(id, 0);
    return true;
}

bool Document::applyGeneratorRun(std::uint64_t serial, LayerId id, const std::optional<std::string>& params,
                                 const std::optional<std::string>& spec, Canvas cells) {
    const Layer* l = stack_.find(id);
    if (!l || l->props.kind != LayerKind::Generator || keptFile(paramsPath(id)) != params ||
        keptFile(specPath(id)) != spec)
        return false;
    if (serial == 0) {
        // An edit undone since the Run was asked for (a parameter step put
        // back to the params the run read, or any other) stays to be
        // redone: the result, which would push over it, is dropped.
        const auto asked = runAsked_.find(id);
        if (undo_.canRedo() && (asked == runAsked_.end() || asked->second != undoMoves_))
            return false;
        if (cells != l->cells)
            undo_.push(new LayerPropsCommand(this, propsEdit(*l, l->props), tr("Run Generator"),
                                             LayerPropsCommand::GeneratorStep{0, params, params, l->cells, cells}));
        return true;
    }
    // The newest step on this layer's generator in the whole stack, done or
    // undone, and it must be done: a step that came after it, though undone
    // since, holds the cells this one has now as its cells before, so this
    // one's can't change under it.
    for (int i = undo_.count() - 1; i >= 0; --i) {
        const auto* c = dynamic_cast<const LayerPropsCommand*>(undo_.command(i));
        if (!c || !c->generator() || c->layerId() != id)
            continue;
        if (c->generator()->serial != serial || i >= undo_.index())
            return false;
        if (cells == c->generator()->cellsAfter)
            return true;
        const_cast<LayerPropsCommand*>(c)->setCellsAfter(cells);
        // a save holding this step holds the cells it had before
        if (undo_.cleanIndex() > i)
            undo_.resetClean();
        putGenerator(id, params, cells);
        return true;
    }
    return false;
}

void Document::putGenerator(LayerId id, const std::optional<std::string>& params, const Canvas& cells) {
    Layer* layer = stack_.find(id);
    if (!layer)
        return;
    if (params)
        extras_.files[paramsPath(id)] = *params;
    else
        extras_.files.erase(paramsPath(id));
    QRegion changed, touched;
    if (layer->cells != cells) {
        const CellRect before = layer->bounds();
        Canvas was = std::exchange(layer->cells, cells);
        const CellRect run = intersect(unite(before, layer->bounds(), stack_.canvasRect()), stack_.canvasRect());
        changed = recomposite(run);
        if (showsReference()) {
            const QPoint at(layer->props.x, layer->props.y);
            touched = styledAtComposite(layer->props) ? QRegion(QRect(run.x, run.y, run.width, run.height))
                                                      : otherCells(was, at, layer->cells, at, run);
        }
        if (id == active_)
            refreshLayerCanvas();
    }
    emit generatorChanged(id);
    if (!changed.isEmpty())
        emit cellsChanged(changed);
    touchLayerCells(touched);
}

bool Document::renameLayer(LayerId id, const QString& name) {
    const Layer* l = stack_.find(id);
    const QString trimmed = name.trimmed();
    if (!l || trimmed.isEmpty())
        return false;
    LayerProperties p = l->props;
    p.name = trimmed.toStdString();
    return setLayerProperties(id, p, tr("Rename Layer"));
}

bool Document::setLayerVisible(LayerId id, bool visible) {
    const Layer* l = stack_.find(id);
    if (!l)
        return false;
    LayerProperties p = l->props;
    p.visible = visible;
    return setLayerProperties(id, p, visible ? tr("Show Layer") : tr("Hide Layer"));
}

bool Document::setLayerAlphaLocked(LayerId id, bool on) {
    const Layer* l = stack_.find(id);
    if (!l)
        return false;
    LayerProperties p = l->props;
    p.alphaLocked = on;
    return setLayerProperties(id, p, on ? tr("Alpha Lock On") : tr("Alpha Lock Off"));
}

void Document::resetLayers(LayerStack stack) {
    stack_ = std::move(stack);
    active_ = stack_.empty() ? 0 : stack_[0].props.id;
    doc_.canvas = composite(stack_, compositeOptions());
    refreshLayerCanvas();
}

void Document::refreshLayerCanvas() {
    const Layer* l = active();
    layerCanvas_ = l ? bw::layerCanvas(*l, stack_.width(), stack_.height()) : Canvas(stack_.width(), stack_.height());
}

QRegion Document::otherCells(const Canvas& a, QPoint aAt, const Canvas& b, QPoint bAt, CellRect rect) {
    const Cell none = transparentCell();
    const auto at = [&](const Canvas& c, QPoint o, int x, int y) -> const Cell& {
        const long long lx = 1LL * x - o.x(), ly = 1LL * y - o.y();
        return lx >= 0 && ly >= 0 && lx < c.width() && ly < c.height() ? c.at(int(lx), int(ly)) : none;
    };
    CellEdit diff;
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x)
            if (const Cell &was = at(a, aAt, x, y), &now = at(b, bAt, x, y); !identical(was, now))
                diff.changes.push_back({x, y, was, now});
    return changedCells(diff);
}

QRegion Document::recomposite(CellRect rect) {
    rect = intersect(rect, stack_.canvasRect());
    const Canvas fresh = compositeRect(stack_, rect, compositeOptions());
    CellEdit diff;
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
            Cell& cell = doc_.canvas.at(x, y);
            const Cell& now = fresh.at(x - rect.x, y - rect.y);
            if (!identical(now, cell)) {
                diff.changes.push_back({x, y, cell, now});
                cell = now;
            }
        }
    return changedCells(diff);
}

void Document::applyLayerEdit(LayerId id, const CellEdit& layerEdit, bool revert) {
    Layer* layer = stack_.find(id);
    if (!layer)
        return;
    if (revert)
        layerEdit.revert(layer->cells);
    else
        layerEdit.apply(layer->cells);
    if (styledAtComposite(layer->props)) {
        // any cell can restyle the whole layer
        if (id == active_)
            for (const CellChange& c : layerEdit.changes)
                if (doc_.canvas.contains(c.x + layer->props.x, c.y + layer->props.y))
                    layerCanvas_.at(c.x + layer->props.x, c.y + layer->props.y) = layer->cells.at(c.x, c.y);
        const CellRect dirty = intersect(dirtyRect(*layer, layerEdit), stack_.canvasRect());
        const QRegion cells = recomposite(dirty);
        if (!cells.isEmpty())
            emit cellsChanged(cells);
        if (!dirty.empty())
            touchLayerCells(QRect(dirty.x, dirty.y, dirty.width, dirty.height));
        return;
    }
    const CompositeOptions opts = compositeOptions();
    // the layer's cells changed, whether or not the composite did
    const bool cover = showsReference();
    CellEdit diff, touched;
    for (const CellChange& c : layerEdit.changes) {
        const int x = c.x + layer->props.x, y = c.y + layer->props.y;
        if (!doc_.canvas.contains(x, y))
            continue;
        if (id == active_)
            layerCanvas_.at(x, y) = layer->cells.at(c.x, c.y);
        if (cover)
            touched.changes.push_back({x, y, c.before, c.after});
        Cell& cell = doc_.canvas.at(x, y);
        const Cell now = compositeCell(stack_, x, y, opts);
        if (!identical(now, cell)) {
            diff.changes.push_back({x, y, cell, now});
            cell = now;
        }
    }
    const QRegion cells = changedCells(diff);
    if (!cells.isEmpty())
        emit cellsChanged(cells);
    if (cover)
        touchLayerCells(changedCells(touched));
}

bool Document::showsReference() const {
    return std::any_of(stack_.layers().begin(), stack_.layers().end(), [](const Layer& l) {
        return l.props.kind == LayerKind::Reference && l.props.visible && l.props.reference;
    });
}

void Document::touchLayerCells(const QRegion& cells) {
    if (!cells.isEmpty() && showsReference())
        emit layerCellsChanged(cells);
}

LayerId Document::insertLayer(Layer layer, std::size_t index) {
    const CellRect bounds = layer.bounds();
    const LayerId id = stack_.add(std::move(layer), index);
    const QRegion cells = recomposite(bounds);
    emit layersChanged();
    if (!cells.isEmpty())
        emit cellsChanged(cells);
    return id;
}

Layer Document::takeLayer(LayerId id) {
    Layer layer = *stack_.remove(id);
    const QRegion cells = recomposite(layer.bounds());
    emit layersChanged();
    if (!cells.isEmpty())
        emit cellsChanged(cells);
    return layer;
}

void Document::placeLayer(LayerId id, std::size_t index) {
    stack_.move(id, index);
    const QRegion cells = recomposite(stack_.find(id)->bounds());
    emit layersChanged();
    if (!cells.isEmpty())
        emit cellsChanged(cells);
}

void Document::scanFontsFolder(const QString& folder) {
    if (!fontScanner_) {
        fontScanner_ = new FontScanner(this);
        connect(fontScanner_, &FontScanner::changed, this, [this] {
            fontsFolder_ = fontScanner_->catalogue();
            emit fontsFolderChanged();
        });
    }
    fontScanner_->start(folder);
}

bool Document::fontsScanning() const { return fontScanner_ && fontScanner_->scanning(); }

void Document::setFontsFolder(std::shared_ptr<const FontCatalogue> catalogue) {
    if (fontScanner_)
        fontScanner_->cancel();
    fontsFolder_ = std::move(catalogue);
    emit fontsFolderChanged();
}

void Document::keepFont(const FaceRef& face, const FontStore* held) {
    // a TheDraw font from the folder (or held) becomes the project's copy
    FontSources from = fontSources();
    from.held = held;
    if (face.kind == FaceRef::Kind::Tdf)
        if (FontFilePtr file = from.file(face.file))
            extras_.fonts.add(std::move(file));
}

FontSources Document::fontSources() const {
    return {&extras_.fonts, fontsFolder_ ? &fontsFolder_->store : nullptr};
}

void Document::putProperties(const LayerPropsEdit& edit, bool revert) {
    const LayerId id = edit.id;
    Layer* layer = stack_.find(id);
    if (!layer)
        return;
    const LayerProperties was = layer->props;
    const CellRect before = layer->bounds();
    // a step changing a live text layer's source replaces its cells
    const bool redrawn = applyEdit(*layer, edit, revert, fontSources());
    const LayerProperties& now = layer->props;
    // Only these change the composite (locks and the name are the tools').
    const bool shows = was.visible != now.visible || was.kind != now.kind || was.mode != now.mode ||
                       was.style != now.style || redrawn || before != layer->bounds();
    const QRegion cells = shows ? recomposite(unite(before, layer->bounds(), stack_.canvasRect())) : QRegion();
    if (id == active_ && (before != layer->bounds() || redrawn))
        refreshLayerCanvas();
    emit layersChanged();
    if (!cells.isEmpty())
        emit cellsChanged(cells);
}

void Document::makeActive(LayerId id) {
    if (!stack_.find(id) || id == active_)
        return;
    active_ = id;
    refreshLayerCanvas();
    emit activeLayerChanged(id);
}

void Document::setModified(bool on) {
    if (modified_ == on)
        return;
    modified_ = on;
    emit modifiedChanged(on);
}

} // namespace bw
