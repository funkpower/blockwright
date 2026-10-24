#include "tool.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/ansi.h"
#include "blockwright/canvas_renderer.h"
#include "blockwright/render.h"

#include <QPainter>

#include <algorithm>
#include <exception>

namespace bw {

Tool::Tool(const QString& id, const QString& name, QObject* parent) : QObject(parent), id_(id), name_(name) {}

const CellEdit& Tool::pending() const {
    static const CellEdit none;
    return none;
}

void Tool::activate(CanvasView* view) { view_ = view; }

void Tool::deactivate() { view_ = nullptr; }

void EditPreview::clear() {
    changes_.clear();
    cells_ = {};
}

void EditPreview::set(const CanvasView& view, const CellEdit& edit) {
    clear();
    const Canvas& cv = view.document()->canvas();
    // The tool's edit is on the active layer; what it shows is the
    // composite with it applied.
    changes_ = view.document()->previewEdit(edit).changes;
    const auto rowMajor = [](const CellChange& a, const CellChange& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    };
    if (!std::is_sorted(changes_.begin(), changes_.end(), rowMajor)) {
        // A cell changed twice lands as its last change, as CellEdit applies.
        std::stable_sort(changes_.begin(), changes_.end(), rowMajor);
        std::vector<CellChange> once;
        once.reserve(changes_.size());
        for (const CellChange& c : changes_) {
            if (!once.empty() && once.back().x == c.x && once.back().y == c.y)
                once.back() = c;
            else
                once.push_back(c);
        }
        changes_.swap(once);
    }
    int x0 = cv.width(), x1 = -1;
    for (const CellChange& c : changes_)
        x0 = std::min(x0, c.x), x1 = std::max(x1, c.x);
    if (x1 >= x0)
        cells_ = QRect(QPoint(x0, changes_.front().y), QPoint(x1, changes_.back().y));
}

void EditPreview::paint(QPainter& p, const CanvasView& view, const QRect& exposed) const {
    if (changes_.empty())
        return;
    // The cells under `exposed` that the edit spans.
    const QRect cells = QRect(view.cellAt(exposed.topLeft()), view.cellAt(exposed.bottomRight())) & cells_;
    if (cells.isEmpty())
        return;
    // As the renderer draws the canvas: iCE applied, at 1x, scaled up as
    // it is drawn.
    const Canvas& cv = view.document()->canvas();
    const CanvasRenderer* r = view.renderer();
    const bool ice = r->iceColours();
    Canvas part(cells.width(), cells.height());
    for (int y = cells.top(); y <= cells.bottom(); ++y) {
        for (int x = cells.left(); x <= cells.right(); ++x)
            part.at(x - cells.left(), y - cells.top()) = iceCell(cv.at(x, y), ice);
        auto c = std::lower_bound(changes_.begin(), changes_.end(), std::pair{y, cells.left()},
                                  [](const CellChange& a, std::pair<int, int> b) {
                                      return a.y != b.first ? a.y < b.first : a.x < b.second;
                                  });
        for (; c != changes_.end() && c->y == y && c->x <= cells.right(); ++c)
            part.at(c->x - cells.left(), y - cells.top()) = iceCell(c->after, ice);
    }
    QImage image;
    try {
        image = renderCanvas(part, r->cellSize().width());
    } catch (const std::exception&) {
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(QRect(view.cellRect(cells.topLeft()).topLeft(), image.size() * view.zoom()), image);
}

void ToolRegistry::add(Tool* tool) {
    if (!tool || tools_.contains(tool))
        return;
    tool->setParent(this);
    tools_ << tool;
    if (!active_)
        setActive(tool);
}

Tool* ToolRegistry::find(const QString& id) const {
    for (Tool* t : tools_)
        if (t->id() == id)
            return t;
    return nullptr;
}

void ToolRegistry::setActive(Tool* tool) {
    if (!tool || tool == active_ || !tools_.contains(tool))
        return;
    active_ = tool;
    emit activeChanged(tool);
}

} // namespace bw
