#include "tool.h"

namespace bw {

Tool::Tool(const QString& id, const QString& name, QObject* parent) : QObject(parent), id_(id), name_(name) {}

void Tool::activate(CanvasView* view) { view_ = view; }

void Tool::deactivate() { view_ = nullptr; }

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
