#include "toolbox.h"

#include "tool.h"
#include "tool_icon.h"

#include <QAccessibleWidget>
#include <QAction>
#include <QEvent>
#include <QMenu>
#include <QResizeEvent>
#include <QSet>
#include <QToolButton>

#include <algorithm>

namespace bw {

namespace {

// The fewest columns the grid narrows to; it takes as many more as the
// dock's width holds.
constexpr int kMinColumns = 2;
constexpr int kSpacing = 2;
constexpr int kMargin = 4;
constexpr int kIcon = 24;

// The hint a group's tools carry in their tooltips.
QString groupHint() { return Toolbox::tr("Click the arrow or right-click for the rest of the group"); }

// Marks a group's button for groupButtonAccessible().
constexpr char kGroupButton[] = "bwToolboxGroup";

// What a screen reader sees of a group's button. Qt's own interface makes a
// split button a ButtonMenu whose only action, listed twice, is Show Menu, so
// the group's current tool can't be picked through it (#179). This one offers
// Press, which clicks the button as the mouse does, and Show Menu, each once,
// names the current tool, and keeps the flyout as its child.
class GroupButtonAccessible : public QAccessibleWidget {
public:
    explicit GroupButtonAccessible(QToolButton* b) : QAccessibleWidget(b, QAccessible::ButtonDropDown) {}

    QAccessible::State state() const override {
        QAccessible::State s = QAccessibleWidget::state();
        s.checkable = button()->isCheckable();
        s.checked = button()->isChecked();
        s.pressed = button()->isDown();
        s.hasPopup = true;
        return s;
    }

    QString text(QAccessible::Text t) const override {
        if (t == QAccessible::Name && widget()->accessibleName().isEmpty())
            return button()->text(); // the current tool's, from its action's icon text
        if (t == QAccessible::Description && widget()->accessibleDescription().isEmpty()) {
            QAction* a = button()->defaultAction();
            QStringList group;
            if (button()->menu())
                for (QAction* m : button()->menu()->actions())
                    group << m->iconText();
            const QString tool = a ? a->toolTip().section(QLatin1Char('\n'), 0, 0) : button()->text();
            return Toolbox::tr("%1. Press picks it; Show Menu opens the group: %2.")
                .arg(tool, group.join(QStringLiteral(", ")));
        }
        return QAccessibleWidget::text(t);
    }

    QStringList actionNames() const override {
        if (!widget()->isEnabled())
            return {};
        return {pressAction(), showMenuAction()};
    }

    void doAction(const QString& action) override {
        if (!widget()->isEnabled())
            return;
        if (action == pressAction())
            button()->click();
        else if (action == showMenuAction())
            button()->showMenu();
    }

    // The flyout is the button's one child, as in Qt's interface for a split
    // button; QAccessibleWidget leaves menus out of a widget's children.
    int childCount() const override { return button()->menu() ? 1 : 0; }

    QAccessibleInterface* child(int index) const override {
        return index == 0 && button()->menu() ? QAccessible::queryAccessibleInterface(button()->menu()) : nullptr;
    }

    int indexOfChild(const QAccessibleInterface* child) const override {
        return child && button()->menu() && child->object() == button()->menu() ? 0 : -1;
    }

    QStringList keyBindingsForAction(const QString& action) const override {
        const QAction* a = button()->defaultAction();
        if (action == pressAction() && a && !a->shortcut().isEmpty())
            return {a->shortcut().toString(QKeySequence::NativeText)};
        return {};
    }

private:
    QToolButton* button() const { return static_cast<QToolButton*>(widget()); }
};

// The accessibility factory for the buttons marked kGroupButton; every other
// object keeps Qt's interface.
QAccessibleInterface* groupButtonAccessible(const QString&, QObject* o) {
    auto* b = qobject_cast<QToolButton*>(o);
    return b && b->property(kGroupButton).toBool() ? new GroupButtonAccessible(b) : nullptr;
}

} // namespace

const QList<QStringList>& Toolbox::groups() {
    static const QList<QStringList> g{
        {QStringLiteral("keyboard")},
        {QStringLiteral("select")},
        {QStringLiteral("move")},
        // The brushes that lay a colour or a glyph down.
        {QStringLiteral("halfBlock"), QStringLiteral("shade"), QStringLiteral("glyph"), QStringLiteral("colorize")},
        // The brushes that walk a ramp.
        {QStringLiteral("rampShade"), QStringLiteral("form"), QStringLiteral("soft"), QStringLiteral("despeckle"),
         QStringLiteral("smudge")},
        // The detail brushes: those that find an edge, the texture stamp, the
        // clone stamp and the glyph spray.
        {QStringLiteral("outline"), QStringLiteral("rimLight"), QStringLiteral("texture"),
         QStringLiteral("clone"), QStringLiteral("spray")},
        {QStringLiteral("line"), QStringLiteral("rectangle"), QStringLiteral("ellipse")},
        // The fills: the bucket and the ramp gradient.
        {QStringLiteral("fill"), QStringLiteral("gradient")},
        {QStringLiteral("eyedropper")},
        {QStringLiteral("text")},
    };
    return g;
}

Toolbox::Toolbox(ToolRegistry* tools, const QHash<QString, QAction*>& actions, QWidget* parent)
    : QWidget(parent), actions_(actions) {
    static const bool factoryInstalled = (QAccessible::installFactory(groupButtonAccessible), true);
    Q_UNUSED(factoryInstalled);
    QList<QStringList> layout;
    QSet<QString> placed;
    for (const QStringList& group : groups()) {
        QStringList present;
        for (const QString& id : group)
            if (tools->find(id) && actions.contains(id))
                present << id;
        if (!present.isEmpty())
            layout << present;
        for (const QString& id : present)
            placed.insert(id);
    }
    for (Tool* t : tools->tools())
        if (!placed.contains(t->id()) && actions.contains(t->id()))
            layout << QStringList{t->id()};

    for (const QStringList& group : layout) {
        auto* b = new QToolButton(this);
        // before anything asks for its accessible interface, which Qt caches
        b->setProperty(kGroupButton, group.size() > 1);
        b->setObjectName(QStringLiteral("toolbox_") + group.front());
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIconSize(QSize(kIcon, kIcon));
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setDefaultAction(actions.value(group.front()));
        if (group.size() > 1) {
            auto* menu = new QMenu(b);
            menu->setObjectName(b->objectName() + QStringLiteral("Menu"));
            for (const QString& id : group) {
                QAction* a = actions.value(id);
                menu->addAction(a);
                if (!a->toolTip().contains(groupHint()))
                    a->setToolTip(a->toolTip() + QLatin1Char('\n') + groupHint());
            }
            b->setMenu(menu);
            b->setPopupMode(QToolButton::MenuButtonPopup);
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QWidget::customContextMenuRequested, b, &QToolButton::showMenu);
        }
        buttons_ << b;
        for (const QString& id : group) {
            byTool_.insert(id, b);
            icons_.insert(id, toolIcon(id, b));
            // A changed action (checked, a new icon) puts its own icon back on
            // the button before this runs.
            QAction* a = actions.value(id);
            connect(a, &QAction::changed, b, [this, a, b] {
                if (b->defaultAction() == a)
                    showIcon(b);
            });
        }
        showIcon(b);
    }
    for (QToolButton* b : std::as_const(buttons_))
        cell_ = cell_.expandedTo(b->sizeHint());
    place();

    if (tools->active())
        follow(tools->active());
    connect(tools, &ToolRegistry::activeChanged, this, &Toolbox::follow);
}

void Toolbox::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    // The icons are drawn in the palette's colours (tool_icon.h): new ones
    // for the buttons, in theirs, and the Tools menu, in the application's,
    // when the palette or scheme changes.
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange ||
        e->type() == QEvent::ThemeChange) {
        for (auto it = icons_.begin(); it != icons_.end(); ++it)
            it.value() = toolIcon(it.key(), byTool_.value(it.key()));
        for (auto it = actions_.cbegin(); it != actions_.cend(); ++it)
            it.value()->setIcon(toolIcon(it.key()));
        for (QToolButton* b : std::as_const(buttons_))
            showIcon(b);
    }
}

int Toolbox::columnsFor(int width) const {
    const int fit = (width - 2 * kMargin + kSpacing) / (cell_.width() + kSpacing);
    return std::clamp(fit, std::min(kMinColumns, int(buttons_.size())), std::max(1, int(buttons_.size())));
}

int Toolbox::heightFor(int columns) const {
    const int rows = int((buttons_.size() + columns - 1) / columns);
    return 2 * kMargin + rows * cell_.height() + std::max(0, rows - 1) * kSpacing;
}

QSize Toolbox::sizeHint() const {
    // As high as the buttons need at the width the dock gives, and no more:
    // the rest of the column is Tool Options' (PLAN §11).
    const int narrowest = std::min(kMinColumns, int(buttons_.size()));
    return {2 * kMargin + narrowest * cell_.width() + std::max(0, narrowest - 1) * kSpacing,
            heightFor(columns_)};
}

QSize Toolbox::minimumSizeHint() const { return sizeHint(); }

void Toolbox::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    place();
}

void Toolbox::place() {
    const int columns = columnsFor(width());
    for (int i = 0; i < buttons_.size(); ++i)
        buttons_[i]->setGeometry(kMargin + (i % columns) * (cell_.width() + kSpacing),
                                 kMargin + (i / columns) * (cell_.height() + kSpacing), cell_.width(), cell_.height());
    if (columns != columns_) {
        const bool reflowed = columns_ == 0 || heightFor(columns) != heightFor(columns_);
        columns_ = columns;
        if (reflowed) {
            updateGeometry();
            emit rowsChanged();
        }
    }
}

void Toolbox::follow(Tool* tool) {
    // The group's button takes the tool picked, from its menu or anywhere.
    if (!tool)
        return;
    QToolButton* b = byTool_.value(tool->id());
    QAction* a = actions_.value(tool->id());
    if (b && a && b->defaultAction() != a) {
        QMenu* menu = b->menu();
        b->setDefaultAction(a);
        b->setMenu(menu);
        showIcon(b);
        // The name follows the button's text; the description names the tool too.
        QAccessibleEvent changed(b, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&changed);
    }
}

void Toolbox::showIcon(QToolButton* b) {
    if (QAction* a = b->defaultAction())
        b->setIcon(icons_.value(actions_.key(a)));
}

} // namespace bw
