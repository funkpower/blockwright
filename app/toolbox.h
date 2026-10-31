#pragma once
// The Toolbox dock (PLAN §11): a button per tool, or per group of tools. A
// group's button shows the tool of the group last picked; clicking its arrow
// or right-clicking it opens the rest of the group. Clicking the icon picks
// the current tool. The buttons are the Tools menu's actions, so a button
// has the tool's shortcut in its tooltip, shows the active tool pressed,
// and adds no shortcut of its own. A button's icon is its own, drawn in the
// button's palette, so a palette set on the toolbox reaches it.

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

    // The buttons fill the width in rows, at least two columns wide; the
    // hint is as high as those rows, so the toolbox takes no spare height.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // The width changed how many rows the buttons take.
    void rowsChanged();

protected:
    void changeEvent(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void follow(Tool* tool);
    // Gives `b` the icon of its current tool, in its own palette.
    void showIcon(QToolButton* b);
    // How many columns of buttons `width` holds, and how high their rows are.
    int columnsFor(int width) const;
    int heightFor(int columns) const;
    // Lays the buttons out in the columns the width holds.
    void place();

    QList<QToolButton*> buttons_;
    QHash<QString, QToolButton*> byTool_;
    QHash<QString, QAction*> actions_;
    QHash<QString, QIcon> icons_; // by tool id, each drawn in its button's palette
    QSize cell_;       // every button's size: the largest button's hint
    int columns_ = 0;  // as last placed
};

} // namespace bw
