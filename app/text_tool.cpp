#include "text_tool.h"
#include "blockwright/big_text.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/text.h"

#include <QKeyEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QUndoStack>

#include <algorithm>
#include <climits>
#include <utility>
#include <vector>

namespace bw {

namespace {

// The layer's source when it is a live text layer.
const TextSource* liveText(const Layer* l) { return l && l->props.text ? &*l->props.text : nullptr; }

bool editableText(const Layer* l) { return liveText(l) && l->props.visible && !l->props.locked; }

// The top-left cell of a live text layer's letters: its offset, moved right
// and down by its style's margins (renderStyledText draws them there).
QPoint lettersAt(const Layer& l) {
    QPoint at(l.props.x, l.props.y);
    if (l.props.style) {
        const StyleMargins m = styleMargins(*l.props.style);
        at += QPoint(m.left, m.top);
    }
    return at;
}

// The face with the source's letter spacing on top of its own, as
// renderText lays the text out (text.cpp).
class SpacedFace final : public Face {
public:
    SpacedFace(const Face& face, int extra) : face_(face), extra_(extra) {}
    std::string_view name() const override { return face_.name(); }
    int height() const override { return face_.height(); }
    int letterSpacing() const override { return face_.letterSpacing() + extra_; }
    std::optional<Glyph> glyph(char32_t ch) const override { return face_.glyph(ch); }
    std::vector<ConfusablePair> confusables() const override { return face_.confusables(); }

private:
    const Face& face_;
    int extra_;
};

// The source laid out as renderText draws it: each line's place in the
// box, and the column of each character's left edge within its line.
struct Laid {
    struct Line {
        int first = 0, count = 0;  // characters of the text
        int shift = 0;             // its first column in the box
        int top = 0;               // its font origin row, for drawing the cursor
        int hitTop = 0;            // its first line-box row, for selecting the line
        std::vector<int> columns;  // count + 1 caret columns, from the line's start
    };
    std::vector<Line> lines;
    int rows = 1;  // a line's height in cell rows
    int width = 0, height = 0;

    // Whether line `l` ends where the next starts (a wrap that drops no
    // space), so a caret there is on either.
    bool joined(int l) const {
        return l + 1 < int(lines.size()) && lines[l].first + lines[l].count == lines[l + 1].first;
    }
    // The line holding caret `c`: the last that starts at or before it, or
    // the one before when `after` and `c` is where the two join.
    int lineOf(int c, bool after) const {
        int l = 0;
        while (l + 1 < int(lines.size()) && lines[l + 1].first <= c)
            ++l;
        if (after && l > 0 && lines[l].first == c && joined(l - 1))
            --l;
        return l;
    }
    // The caret's cell in the box: its column and its line's top row.
    QPoint place(int c, bool after) const {
        if (lines.empty())
            return {};
        const Line& line = lines[lineOf(c, after)];
        const int k = std::clamp(c - line.first, 0, line.count);
        return {line.shift + line.columns[k], line.top};
    }
    struct Caret {
        int at = 0;
        bool after = false;  // as lineOf takes it
    };
    // The caret nearest a cell of the box.
    Caret caretAt(QPoint cell) const {
        if (lines.empty())
            return {};
        int l = 0;
        while (l + 1 < int(lines.size()) && lines[l + 1].hitTop <= cell.y())
            ++l;
        const Line& line = lines[l];
        // the caret before the character the cell is in, or after it if the
        // cell is in its right half
        const int x = cell.x() - line.shift;
        int best = 0;
        for (int k = 0; k <= line.count; ++k)
            if (std::abs(2 * line.columns[k] - (2 * x + 1)) < std::abs(2 * line.columns[best] - (2 * x + 1)))
                best = k;
        return {line.first + best, best == line.count && joined(l)};
    }
};

Laid lay(const TextSource& s, const Face& face) {
    if (s.bigStyle != BigTextStyle::None) {
        const auto big = renderBigText(s);
        Laid out;
        out.rows = 8;
        out.width = big.drawing.cells.width();
        out.height = big.drawing.cells.height();
        for (const auto& line : big.lines) {
            Laid::Line placed;
            placed.first = int(line.first);
            placed.count = int(line.count);
            placed.shift = line.left;
            placed.top = line.top;
            placed.hitTop = line.hitTop;
            for (std::size_t i = 0; i <= line.count; ++i)
                placed.columns.push_back(int(i) * (8 + s.letterSpacing));
            out.lines.push_back(std::move(placed));
        }
        return out;
    }
    const SpacedFace spaced(face, s.letterSpacing);
    const std::u32string text = casedText(s);
    const TextLayout layout = layoutText(spaced, text, {s.wrapWidth});
    const std::optional<Glyph> space = face.glyph(U' ');
    const int spaceWidth = space ? space->mask.width : 0;
    Laid out;
    out.rows = std::max(1, face.height() / 2);
    out.width = std::max(s.wrapWidth, layout.mask.width);
    const int lines = std::max<int>(1, int(layout.lines.size()));
    out.height = lines * out.rows + (lines - 1) * s.lineSpacing;
    for (size_t l = 0; l < layout.lines.size(); ++l) {
        const TextLayout::Line& src = layout.lines[l];
        Laid::Line line;
        line.first = int(src.first);
        line.count = int(src.count);
        line.shift = s.align == TextAlign::Left    ? 0
                     : s.align == TextAlign::Right ? out.width - src.width
                                                   : (out.width - src.width) / 2;
        line.top = int(l) * (out.rows + s.lineSpacing);
        line.hitTop = line.top;
        line.columns.reserve(src.count + 1);
        int x = 0;
        line.columns.push_back(0);
        for (size_t i = 0; i < src.count; ++i) {
            const std::optional<Glyph> g = face.glyph(text[src.first + i]);
            x += (g ? g->mask.width : spaceWidth) + spaced.letterSpacing();
            line.columns.push_back(x);
        }
        out.lines.push_back(std::move(line));
    }
    if (out.lines.empty())
        out.lines.push_back({0, 0, 0, 0, 0, {0}});
    return out;
}

// `at`, a character index into `s`, moved by `units` UTF-16 code units (an
// input method's offsets), a character outside the BMP two of them; within
// the text.
int stepUtf16(const std::u32string& s, int at, int units) {
    while (units < 0 && at > 0)
        units += s[size_t(--at)] > 0xFFFF ? 2 : 1;
    while (units > 0 && at < int(s.size()))
        units -= s[size_t(at++)] > 0xFFFF ? 2 : 1;
    return at;
}

} // namespace

TextTool::TextTool(QObject* parent) : Tool(QStringLiteral("text"), tr("T&ext"), parent) {}

Document* TextTool::doc() const { return view() ? view()->document() : nullptr; }

FontSources TextTool::fonts() const {
    const Document* d = doc();
    FontSources from = d ? d->fontSources() : FontSources{};
    from.held = &kept_;
    return from;
}

void TextTool::trimKept() {
    trimQueued_ = false;
    FontStore next;
    if (defaults_.face.kind == FaceRef::Kind::Tdf)
        if (FontFilePtr file = kept_.file(defaults_.face.file))
            next.add(std::move(file));
    if (next == kept_)
        return;
    // the faces listed from the files dropped last until the picker lists
    // the faces again
    const FontStore dropped = std::exchange(kept_, std::move(next));
    emit keptChanged();
}

const Layer* TextTool::edited() const {
    const Document* d = doc();
    const Layer* l = d && editing_ ? d->layers().find(editing_) : nullptr;
    return liveText(l) ? l : nullptr;
}

const TextSource* TextTool::shown(QPoint* origin) const {
    if (spot_) {
        *origin = *spot_;
        return &defaults_;
    }
    const Layer* l = edited();
    if (!l)
        return nullptr;
    *origin = lettersAt(*l);
    return &*l->props.text;
}

void TextTool::setSpot(std::optional<QPoint> spot) {
    const bool was = spot_.has_value();
    spot_ = spot;
    if (was != spot_.has_value())
        emit spotChanged();
}

void TextTool::setSettings(const TextSource& s, const QString& undoText, Document* doc) {
    // the face's file is kept, from the document or the files already
    // kept; the others go once the face picker is out of the signal that
    // picked, its entries for them selectable until then
    if (s.face.kind == FaceRef::Kind::Tdf) {
        FontSources from = doc ? doc->fontSources() : FontSources{};
        from.held = &kept_;
        if (FontFilePtr file = from.file(s.face.file))
            kept_.add(std::move(file));
    }
    defaults_ = s;
    defaults_.text.clear();
    if (!kept_.empty() && !trimQueued_) {
        trimQueued_ = true;
        QMetaObject::invokeMethod(this, &TextTool::trimKept, Qt::QueuedConnection);
    }
    if (!doc)
        return;
    const Layer* l = doc->active();
    if (!liveText(l) || l->props.locked || spot_) {
        // the status and the text cursor at spot() show the new defaults
        emit changed();
        return;
    }
    TextSource next = s;
    next.text = l->props.text->text;
    const LayerId id = l->props.id;
    auto set = [&] { doc->setTextSource(id, next, undoText, 0, &kept_); };
    if (view() && view()->document() == doc)
        own(set);
    else
        set();
    newRun();
    emit changed();
}

void TextTool::own(const std::function<void()>& change) {
    const QScopedValueRollback<bool> guard(own_, true);
    view()->ownChange(change);
}

void TextTool::begin(LayerId id, int caret, bool after) {
    editing_ = id;
    caret_ = caret;
    after_ = after;
    newRun();
    emit changed();
}

void TextTool::stop() {
    editing_ = made_ = 0;
    setSpot({});
    after_ = false;
    newRun();
    emit changed();
}

void TextTool::endEdit() {
    const LayerId id = editing_, made = made_;
    stop();
    if (id && id == made)
        drop(id);
}

void TextTool::drop(LayerId id) {
    Document* d = doc();
    if (!d || !view())
        return;
    const LayerId active = d->activeLayer();
    bool dropped = false;
    own([&] { dropped = d->dropNewTextLayer(id); });
    // the layer's undone step makes the layer before it active
    if (dropped && active != id && d->layers().find(active))
        d->setActiveLayer(active);
    emit changed();
}

void TextTool::activate(CanvasView* view) {
    Tool::activate(view);
    Document* d = doc();
    if (!d)
        return;
    connections_ = {
        connect(d->undoStack(), &QUndoStack::indexChanged, this, &TextTool::undoChanged),
        connect(d->undoStack(), &QUndoStack::cleanChanged, this, &TextTool::undoChanged),
        connect(d, &Document::layersChanged, this, &TextTool::layersChanged),
        connect(d, &Document::reset, this, &TextTool::documentReset),
        connect(d, &Document::activeLayerAboutToChange, this, &TextTool::activeAboutToChange),
        connect(d, &Document::commandAboutToPush, this, &TextTool::commandAboutToPush),
    };
}

void TextTool::deactivate() {
    endEdit();
    for (const QMetaObject::Connection& c : std::as_const(connections_))
        disconnect(c);
    connections_.clear();
    Tool::deactivate();
}

void TextTool::undoChanged() {
    // undo, redo, a command from elsewhere or a save: a new run of typing,
    // the cursor kept within the text
    if (own_)
        return;
    newRun();
    const Document* d = doc();
    const Layer* l = d && editing_ ? d->layers().find(editing_) : nullptr;
    if (editing_ && !editableText(l)) {
        stop();
        return;
    }
    if (l && caret_ > int(l->props.text->text.size())) {
        caret_ = int(l->props.text->text.size());
        after_ = false;
    }
    emit changed();
}

void TextTool::layersChanged() {
    // locked, hidden, baked or gone: the edit is over, the layer left as it is
    const Document* d = doc();
    if (editing_ && (!d || !editableText(d->layers().find(editing_))))
        stop();
}

void TextTool::documentReset() {
    // another document, whatever its layer ids
    editing_ = made_ = 0;
    setSpot({});
    caret_ = 0;
    after_ = false;
    newRun();
    emit changed();
}

void TextTool::activeAboutToChange(LayerId id) {
    // another layer about to be made active ends the edit now, outside any
    // command, so an empty layer it made goes before anything else can
    // change the document or begin an edit on that layer
    if (editing_ && id != editing_)
        endEdit();
}

void TextTool::commandAboutToPush() {
    // a command from elsewhere (a new layer, a layer deleted or moved, the
    // SAUCE fields...) would be pushed after the steps of an empty layer the
    // edit made, where it could no longer be taken back: the edit ends first
    const Document* d = doc();
    const Layer* l = d && !own_ && editing_ && editing_ == made_ ? d->layers().find(editing_) : nullptr;
    if (liveText(l) && l->props.text->text.empty())
        endEdit();
}

LayerId TextTool::textLayerAt(QPoint cell) const {
    const Document* d = doc();
    if (!d)
        return 0;
    const std::vector<Layer>& layers = d->layers().layers();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
        if (editableText(&*it) && it->bounds().contains(cell.x(), cell.y()))
            return it->props.id;
    return 0;
}

bool TextTool::mousePress(const ToolEvent& e) {
    Document* d = doc();
    if (!d || e.button != Qt::LeftButton)
        return false;
    const LayerId hit = textLayerAt(e.cell);
    if (hit) {
        if (hit != editing_) {
            endEdit();
            d->setActiveLayer(hit);
        }
        const Layer* l = d->layers().find(hit);
        const Face* face = l ? findFace(l->props.text->face, d->fontSources()) : nullptr;
        if (!face)
            return true;
        const Laid::Caret at = lay(*l->props.text, *face).caretAt(e.cell - lettersAt(*l));
        if (hit == editing_) {
            moveCaret(at.at, at.after);
        } else {
            made_ = 0;
            begin(hit, at.at, at.after);
        }
        return true;
    }
    // the layer waits for the first character typed, so a stray click
    // leaves no layer and no undo step
    endEdit();
    caret_ = 0;
    after_ = false;
    setSpot(e.cell);
    newRun();
    emit changed();
    return true;
}

bool TextTool::mouseRelease(const ToolEvent& e) { return e.button == Qt::LeftButton && (editing_ || spot_); }

bool TextTool::replace(int at, int count, const std::u32string& text) {
    Document* d = doc();
    if (!d)
        return false;
    bool made = false;
    if (spot_) {
        if (text.empty() || text.size() > TextSource::kMaxLength)
            return false;
        const QPoint cell = *spot_;
        setSpot({});
        TextSource s = defaults_;
        s.text.clear();
        LayerId id = 0;
        own([&] { id = d->addTextLayer(s, cell.x(), cell.y(), &kept_); });
        if (!id) {
            stop();
            return false;
        }
        begin(id, 0);
        made_ = id;
        made = true;
        at = count = 0;
    }
    const Layer* l = edited();
    if (!editableText(l)) {
        stop();
        return false;
    }
    TextSource s = *l->props.text;
    at = std::clamp(at, 0, int(s.text.size()));
    count = std::clamp(count, 0, int(s.text.size()) - at);
    s.text.replace(size_t(at), size_t(count), text);
    if (s.text.size() > TextSource::kMaxLength)
        return false;
    // a replacement with the same text (an input method's) only moves the
    // cursor, and a cursor moved starts a new run
    if (s.text == l->props.text->text) {
        moveCaret(at + int(text.size()));
        return true;
    }
    bool done = false;
    const LayerId id = editing_;
    own([&] { done = d->setTextSource(id, s, tr("Typing"), run_, &kept_); });
    if (!done) {
        if (made)
            endEdit();
        return false;
    }
    caret_ = at + int(text.size());
    after_ = false;
    emit changed();
    return true;
}

LayerId TextTool::placeStyled(QPoint cell, const std::u32string& text, const TextStyle& style) {
    Document* d = doc();
    if (!d || !view() || text.empty() || text.size() > TextSource::kMaxLength)
        return 0;
    endEdit();
    TextSource s = defaults_;
    s.text = text;
    LayerId id = 0;
    own([&] { id = d->addTextLayer(s, cell.x(), cell.y(), &kept_, style); });
    if (!id)
        return 0;
    begin(id, int(text.size()));
    made_ = id;
    return id;
}

void TextTool::moveCaret(int to, bool after) {
    const Layer* l = edited();
    if (!l)
        return;
    to = std::clamp(to, 0, int(l->props.text->text.size()));
    // to another character, or to the other line where a wrap joins two:
    // a new run of typing
    const QPoint was = caretCell();
    const int from = caret_;
    caret_ = to;
    after_ = after;
    if (caret_ != from || caretCell() != was)
        newRun();
    emit changed();
}

const Layer* TextTool::resumable() const {
    const Document* d = doc();
    const Layer* l = d && !editing_ && !spot_ ? d->active() : nullptr;
    return editableText(l) ? l : nullptr;
}

bool TextTool::resume() {
    if (editing_ || spot_)
        return true;
    // a key or an input method's text with the tool on a text layer edits
    // it, from its end
    const Layer* l = resumable();
    if (!l)
        return false;
    made_ = 0;
    begin(l->props.id, int(l->props.text->text.size()));
    return true;
}

bool TextTool::keyPress(QKeyEvent* e) {
    const QString t = e->text();
    const bool printable =
        std::any_of(t.begin(), t.end(), [](QChar c) { return c.unicode() >= 0x20 && c.unicode() != 0x7f; });
    if (!editing_ && !spot_) {
        if (e->key() == Qt::Key_Escape || (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))
            return false;
        // only a key the edit acts on starts one: a modifier, a lock or any
        // other key that types nothing and isn't handled below leaves the
        // layer as it is
        bool acts = printable;
        switch (e->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Backspace:
        case Qt::Key_Delete: acts = true; break;
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_Up:
        case Qt::Key_Down: acts = !(e->modifiers() & Qt::AltModifier); break;
        default: break;
        }
        if (!acts)
            return false;
    }
    if (!resume())
        return false;
    // at spot() the text is empty and has the settings a new layer starts with
    const Layer* l = spot_ ? nullptr : edited();
    if (!spot_ && !editableText(l)) {
        stop();
        return false;
    }
    const TextSource& s = l ? *l->props.text : defaults_;
    const int size = int(s.text.size());
    const Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier);
    switch (e->key()) {
    case Qt::Key_Escape: endEdit(); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: replace(caret_, 0, U"\n"); return true;
    case Qt::Key_Backspace:
        if (caret_ > 0)
            replace(caret_ - 1, 1, {});
        return true;
    case Qt::Key_Delete:
        if (caret_ < size)
            replace(caret_, 1, {});
        return true;
    case Qt::Key_Left:
        if (mods)
            return false;
        moveCaret(caret_ - 1);
        return true;
    case Qt::Key_Right:
        if (mods)
            return false;
        moveCaret(caret_ + 1);
        return true;
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (mods)
            return false;
        const Face* face = findFace(s.face, fonts());
        if (!face)
            return true;
        const Laid laid = lay(s, *face);
        const int line = laid.lineOf(caret_, after_);
        const Laid::Line& here = laid.lines[line];
        if (e->key() == Qt::Key_Home)
            moveCaret(here.first);
        else if (e->key() == Qt::Key_End)
            moveCaret(here.first + here.count, laid.joined(line));
        else {
            const int to = line + (e->key() == Qt::Key_Up ? -1 : 1);
            if (to >= 0 && to < int(laid.lines.size())) {
                const QPoint at = laid.place(caret_, after_);
                const Laid::Caret c = laid.caretAt({at.x(), laid.lines[to].hitTop});
                moveCaret(c.at, c.after);
            }
        }
        return true;
    }
    default: break;
    }
    if ((e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) && !(e->modifiers() & Qt::AltModifier))
        return false;  // a shortcut
    if (!printable)
        return false;
    inputText(t);
    return true;
}

bool TextTool::inputText(const QString& text, int replaceStart, int replaceLength) {
    if (!resume())
        return false;
    const Layer* l = edited();
    if (!l && !spot_)
        return true;
    std::u32string typed;
    for (char32_t ch : text.toUcs4())
        if (ch >= 0x20 && ch != 0x7f)
            typed += ch;
    // the input method's offsets count UTF-16 units from the cursor
    const std::u32string& source = l ? l->props.text->text : defaults_.text;
    const int from = stepUtf16(source, caret_, replaceStart);
    const int to = stepUtf16(source, from, std::max(0, replaceLength));
    if (!typed.empty() || to > from)
        replace(from, to - from, typed);
    return true;
}

void TextTool::cancel() {
    // the document changed under the edit (undo, redo, another layer made
    // active): a new run of typing, the cursor kept within the text, and the
    // edit over if its layer is gone or no longer the active text layer; a
    // text cursor waiting for a new layer is put away. An empty layer the
    // edit made stays: this may run inside a command, where the stack can't
    // change, and the command took it away before it was pushed
    // (commandAboutToPush, activeAboutToChange)
    newRun();
    setSpot({});
    const Document* d = doc();
    const Layer* l = edited();
    if (editing_ && (!d || !editableText(l) || d->activeLayer() != editing_)) {
        editing_ = made_ = 0;
        after_ = false;
    } else if (l) {
        caret_ = std::clamp(caret_, 0, int(l->props.text->text.size()));
    }
    emit changed();
}

QPoint TextTool::caretCell() const {
    QPoint origin;
    const TextSource* s = shown(&origin);
    const Face* face = s ? findFace(s->face, fonts()) : nullptr;
    if (!face)
        return {};
    return origin + lay(*s, *face).place(caret_, after_);
}

std::optional<QRect> TextTool::caretRect() const {
    QPoint origin;
    const TextSource* s = shown(&origin);
    int at = caret_;
    bool after = after_;
    if (const Layer* l = resumable()) {
        origin = lettersAt(*l);
        s = &*l->props.text;
        at = int(s->text.size());
        after = false;
    }
    const Face* face = s ? findFace(s->face, fonts()) : nullptr;
    if (!face || !view())
        return {};
    const Laid laid = lay(*s, *face);
    const QPoint cell = origin + laid.place(at, after);
    return view()->cellRect(cell).united(view()->cellRect(cell + QPoint(0, std::max(1, laid.rows) - 1)));
}

void TextTool::paint(QPainter& p) const {
    QPoint origin;
    const TextSource* s = shown(&origin);
    const Face* face = s ? findFace(s->face, fonts()) : nullptr;
    if (!face || !view())
        return;
    const Laid laid = lay(*s, *face);
    const QRect topLeft = view()->cellRect(origin);
    const QPoint far = origin + QPoint(std::max(1, laid.width), std::max(laid.rows, laid.height)) - QPoint(1, 1);
    const QRect box = topLeft.united(view()->cellRect(far));

    p.save();
    QPen dash(Qt::white, 1, Qt::DashLine);
    dash.setCosmetic(true);
    p.setPen(QPen(Qt::black, 1));
    p.drawRect(box.adjusted(0, 0, -1, -1));
    p.setPen(dash);
    p.drawRect(box.adjusted(0, 0, -1, -1));

    const QPoint at = origin + laid.place(caret_, after_);
    const QRect first = view()->cellRect(at);
    const QRect last = view()->cellRect(at + QPoint(0, laid.rows - 1));
    const int w = std::max(2, first.width() / 4);
    const QRect caret(first.left() - w / 2, first.top(), w, last.bottom() - first.top() + 1);
    p.fillRect(caret.adjusted(-1, 0, 1, 0), Qt::black);
    p.fillRect(caret, Qt::white);
    p.restore();
}

QString TextTool::status() const {
    const Layer* l = edited();
    const FaceRef& face = l ? l->props.text->face : defaults_.face;
    return tr("Face: %1").arg(QString::fromStdString(face.name));
}

} // namespace bw
