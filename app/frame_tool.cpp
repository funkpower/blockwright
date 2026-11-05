#include "frame_tool.h"
#include "canvas_view.h"
#include "document.h"
#include "settings.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/layer.h"

#include <algorithm>

namespace bw {

namespace {

const QString kFrameStyleKey = QStringLiteral("frame/style");
const QString kFramePenColoursKey = QStringLiteral("frame/penColours");

// Writes the frame cell it is handed before each dab, as each symmetry image
// writes it (dabGlyph), as the glyph brush does. A cell that keeps the
// background takes the one already where it lands: the layer's own, or over
// a transparent cell the one the layers below show there (shownBelow, as the
// fill does), never one from a layer above. On a pixel layer that is the
// pixel the glyph leaves (a ▀'s bottom, a ▄'s top), set or not.
class FrameBrush : public Brush {
public:
    std::string_view name() const override { return "frame"; }
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double) const override {
        Cell c = cell;
        std::optional<char32_t> cp = c.ch.codePoint();
        if (cp) {
            cp = dabGlyph(*cp, stroke.currentDab());
            c.ch = *cp;
        }
        const std::optional<Cell> was = stroke.originalAt(x, y);
        if (!keepBg || !was)
            return c;
        if (stroke.options().pixelLayer) {
            // unheldGlyphs lets only these through
            LayerPixels p = layerPixels(*was);
            const std::uint8_t f = c.fg.paletteIndex();
            if (cp == U'█')
                p = {f, f};
            else if (cp == U'▀')
                p.top = f;
            else if (cp == U'▄')
                p.bottom = f;
            return pixelLayerCell(p);
        }
        if (!was->transparent)
            c.bg = was->bg;
        else if (doc)
            c.bg = doc->shownBelow(x, y).bg;
        return c;
    }
    Cell cell;
    bool keepBg = false;
    const Document* doc = nullptr;
};

// One axis of frameRect: the span from `from` to `to`, at least `minimum`
// long, grown on the side `to` lies.
std::pair<int, int> span(int from, int to, int minimum) {
    int lo = std::min(from, to), hi = std::max(from, to);
    if (hi - lo + 1 < minimum) {
        if (to < from)
            lo = hi - minimum + 1;
        else
            hi = lo + minimum - 1;
    }
    return {lo, hi};
}

} // namespace

FrameTool::FrameTool(QObject* parent) : DragTool(QStringLiteral("frame"), tr("Fra&me"), parent) {
    const auto s = appSettings();
    saved_ = s->value(kFrameStyleKey).toString();
    styleId_ = findStyle(saved_) ? saved_ : QString::fromStdString(bundledFrameStyles().front().id);
    penColours_ = s->value(kFramePenColoursKey, false).toBool();
}

const FrameStyle* FrameTool::findStyle(const QString& id) const {
    if (const FrameStyle* s = findFrameStyle(id.toStdString()))
        return s;
    for (const FrameStyle& s : library_)
        if (QString::fromStdString(s.id) == id)
            return &s;
    return nullptr;
}

const FrameStyle& FrameTool::style() const {
    const FrameStyle* s = findStyle(styleId_);
    return s ? *s : bundledFrameStyles().front();
}

void FrameTool::setStyle(const QString& id) {
    if (!findStyle(id) || (id == styleId_ && id == saved_))
        return;
    // Picking the style shown already (the fallback for one that went away)
    // still makes it the choice kept.
    if (id != saved_) {
        saved_ = id;
        appSettings()->setValue(kFrameStyleKey, id);
    }
    if (id == styleId_)
        return;
    styleId_ = id;
    // a drag under way is redone with it, or ends if the layer refuses it
    settingsChanged();
    emit changed();
}

void FrameTool::setLibraryStyles(std::vector<FrameStyle> styles) {
    library_ = std::move(styles);
    const QString was = styleId_;
    if (findStyle(saved_))
        styleId_ = saved_;
    else if (!findStyle(styleId_))
        styleId_ = QString::fromStdString(bundledFrameStyles().front().id);
    emit stylesChanged();
    // the style a drag under way draws may have changed, or gone
    settingsChanged();
    if (styleId_ != was)
        emit changed();
}

void FrameTool::setPenColours(bool on) {
    if (on == penColours_)
        return;
    penColours_ = on;
    appSettings()->setValue(kFramePenColoursKey, on);
    settingsChanged();
    emit changed();
}

QString FrameTool::status() const { return refusesLayer() ? pixelRefusal() : QString(); }

QRect FrameTool::frameRect(const FrameStyle& style, QPoint from, QPoint to) {
    const auto [x0, x1] = span(from.x(), to.x(), style.minimumWidth());
    const auto [y0, y1] = span(from.y(), to.y(), style.minimumHeight());
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

bool FrameTool::begin(const ToolEvent& e) {
    from_ = to_ = e.cell;
    button_ = e.button;
    pen_ = view()->pen();
    return true;
}

bool FrameTool::extend(const ToolEvent& e) {
    if (e.cell == to_)
        return false;
    to_ = e.cell;
    return true;
}

CellEdit FrameTool::edit() const {
    const FrameStyle& s = style();
    const QRect r = frameRect(s, from_, to_);
    std::vector<bool> keep;
    std::optional<Canvas> frame = drawFrame(s, r.width(), r.height(), &keep);
    if (!frame)
        return {};
    if (penColours_)
        frame = recolourFrame(std::move(*frame), pen_.fg, pen_.bg);
    const Cell blank = view()->document()->blankCell();
    const bool erase = button_ == Qt::RightButton;
    FrameBrush brush;
    brush.doc = view()->document();
    Stroke stroke(canvas(), brush, BrushSettings{}, options());
    for (int y = 0; y < frame->height(); ++y)
        for (int x = 0; x < frame->width(); ++x) {
            const Cell& c = frame->at(x, y);
            if (c.transparent)
                continue;
            brush.cell = erase ? blank : c;
            brush.keepBg = !erase && keep[std::size_t(y) * std::size_t(frame->width()) + std::size_t(x)];
            stroke.addDab(Dab{r.x() + x, r.y() + y}, false);
        }
    return stroke.edit();
}

QString FrameTool::unheldGlyphs(Qt::MouseButton button) const {
    if (button == Qt::RightButton || !view())
        return {};  // it erases, to a blank every layer holds
    // every glyph the style can draw: its corners, its edges and, when it
    // has one, its fill
    const FrameStyle& s = style();
    const Canvas& src = s.source;
    const auto held = [](char32_t ch) {
        Cell c;
        c.ch = ch;
        return halfBlocks(c).has_value();
    };
    const Canvas& c = canvas();
    const std::vector<Dab> images =
        symmetryDabs(view()->symmetry(), Dab{c.width() / 2, c.height() / 2}, c.width(), c.height());
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x) {
            const Cell& cell = src.at(x, y);
            const std::optional<char32_t> cp = cell.ch.codePoint();
            const bool middle =
                x >= s.left && x < src.width() - s.right && y >= s.top && y < src.height() - s.bottom;
            if (cell.transparent || !cp || (middle && !s.fill))
                continue;
            const char32_t g = *cp;
            if (!held(g))
                return QString::fromUcs4(&g, 1);
            for (const Dab& d : images)
                if (!held(dabGlyph(g, d)))
                    return tr("%1 turned on its side, which radial symmetry writes").arg(QString::fromUcs4(&g, 1));
        }
    return {};
}

} // namespace bw
