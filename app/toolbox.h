#pragma once
// The Toolbox dock (PLAN §11): a button per tool, or per group of tools. A
// group's button shows the tool of the group last picked; clicking its arrow
// or right-clicking it opens the rest of the group. Clicking the icon picks
// the current tool. The buttons are the Tools
// menu's actions, so a button has the tool's shortcut in its tooltip, shows
// the active tool pressed, and adds no shortcut of its own.

#include <QHash>
#include <QIcon>
#include <QStringList>
#include <QWidget>

class QAction;
class QToolButton;

namespace bw {

class Tool;
class ToolRegistry;

class Toolbox : public QWidget {
    Q_OBJECT
public:
    // The groups, by tool id, each one button. A registered tool in none of
    // them has a button of its own after these.
    static const QList<QStringList>& groups();

    // `actions` are the Tools menu's, by tool id.
    Toolbox(ToolRegistry* tools, const QHash<QString, QAction*>& actions, QWidget* parent = nullptr);

    QList<QToolButton*> buttons() const { return buttons_; }
    // The button that holds the tool with this id, or null.
    QToolButton* buttonFor(const QString& toolId) const { return byTool_.value(toolId); }

private:
    void follow(Tool* tool);

    QList<QToolButton*> buttons_;
    QHash<QString, QToolButton*> byTool_;
    QHash<QString, QAction*> actions_;
};

// The icon the toolbox and the Tools menu show for a tool.
QIcon toolIcon(const QString& toolId);

} // namespace bw
