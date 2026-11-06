// The workspace (PLAN §11): the Toolbox, Tool Options, glyph bar, Layers,
// Lint and IRC View docks, the Layer menu, the Window menu, and the dock
// layout saved between runs.

#include "main_window.h"

#include "canvas_view.h"
#include "device_grid.h"
#include "glyph_bar.h"
#include "irc_meter.h"
#include "layers_dock.h"
#include "lint_dock.h"
#include "palette_widget.h"
#include "select_tool.h"
#include "settings.h"
#include "shortcuts_dialog.h"
#include "tool.h"
#include "tool_options.h"
#include "toolbox.h"

#include <QAction>
#include <QCoreApplication>
#include <QDockWidget>
#include <QMenu>
#include <QLayout>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace bw {

namespace {

// Bumped when the docks change, so an older saved layout is dropped.
constexpr int kLayoutVersion = 5;
// The left column is as wide as the right where the canvas has room, so the
// tallest tools' help wraps in fewer lines; where it hasn't, it gives that
// back before the right column shrinks.
constexpr int kLeftColumnWidth = 360;
constexpr int kLeftColumnNarrow = 256;
constexpr int kRightColumnWidth = 360;
// An 80-column piece at 1x, excluding the frame, scrollbar and IRC gutter.
constexpr int kCanvasViewportFloor = 640;
// The preview's default shows an 80x25 piece at 1x, in at most a third of
// the column.
constexpr QSize kPreviewPiece{80, 25};
constexpr int kPreviewColumnShare = 3;

// The height a scrolling panel's content asks for at `width`: Layers and
// Lint ask for a set number of list rows (RowTree).
int panelHeight(const QScrollArea* scroll, int width) {
    const QWidget* content = scroll->widget();
    const int inner = width - 2 * scroll->frameWidth();
    return 2 * scroll->frameWidth() +
           (content->hasHeightForWidth() ? content->heightForWidth(inner) : content->sizeHint().height());
}

// A panel that scrolls up and down only: at least as wide as its content
// and the vertical scrollbar, so the column's minimum width shows it all.
class PanelScroll : public QScrollArea {
public:
    QSize minimumSizeHint() const override {
        QSize hint = QScrollArea::minimumSizeHint();
        if (const QWidget* content = widget()) {
            const int contentWidth = std::max(content->minimumWidth(), content->minimumSizeHint().width());
            hint.setWidth(std::max(hint.width(), contentWidth + verticalScrollBar()->sizeHint().width() + 2 * frameWidth()));
        }
        return hint;
    }

protected:
    // The content's minimum can change after it is set (a generator's form
    // shown or rebuilt): let the dock column ask again.
    bool eventFilter(QObject* watched, QEvent* e) override {
        if (watched == widget() && e->type() == QEvent::LayoutRequest)
            updateGeometry();
        return QScrollArea::eventFilter(watched, e);
    }
};

} // namespace

void MainWindow::createDocks() {
    const auto dock = [this](const QString& title, const char* name, QWidget* widget) {
        auto* d = new QDockWidget(title, this);
        d->setObjectName(QString::fromLatin1(name));
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                       QDockWidget::DockWidgetClosable);
        d->setWidget(widget);
        return d;
    };
    toolbox_ = new Toolbox(tools_, toolActions_);
    toolboxDock_ = dock(tr("Toolbox"), "toolboxDock", toolbox_);
    // Taller than its buttons only if the user drags it so: a taller window
    // gives its height to Tool Options.
    toolboxDock_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    toolOptions_ = new ToolOptions(view_, tools_, selectTool_, findChild<QAction*>(QStringLiteral("filledShapes")));
    toolOptions_->followKeys(findChild<QAction*>(QStringLiteral("brushSmaller")),
                             findChild<QAction*>(QStringLiteral("brushLarger")),
                             findChild<QAction*>(QStringLiteral("mirrorMode")));
    connect(toolOptions_, &ToolOptions::shortcutsRequested, this, [this] { showShortcuts(this); });
    auto* optionsScroll = new QScrollArea;
    optionsScroll->setWidget(toolOptions_);
    optionsScroll->setWidgetResizable(true);
    optionsScroll->setFrameShape(QFrame::NoFrame);
    toolOptionsDock_ = dock(tr("Tool Options"), "toolOptionsDock", optionsScroll);
    glyphBar_ = new GlyphBar(view_);
    glyphBarDock_ = dock(tr("Glyphs"), "glyphBarDock", glyphBar_);
    layers_ = new LayersDock(&doc_);
    layersDock_ = dock(tr("Layers"), "layersDock", layers_);

    // Layer: the Layers dock's actions, no shortcuts. Their tooltips show
    // here too, so the add items explain their kinds as the buttons do.
    layerMenu_->setToolTipsVisible(true);
    layerMenu_->addAction(layers_->newCellsAction());
    layerMenu_->addAction(layers_->newPixelsAction());
    layerMenu_->addAction(layers_->deleteAction());
    layerMenu_->addSeparator();
    layerMenu_->addAction(layers_->raiseAction());
    layerMenu_->addAction(layers_->lowerAction());
    // Layer Style…: the style dialog for a cells, pixels or live text layer
    // (PLAN §15). Bake: a live text layer's cells kept, its source dropped,
    // or a styled layer's styled cells kept, its style dropped; one undo step.
    layerMenu_->addSeparator();
    QAction* style = layerMenu_->addAction(tr("Layer &Style…"), this, [this] { editLayerStyle(doc_.activeLayer()); });
    style->setObjectName(QStringLiteral("layerStyle"));
    style->setToolTip(tr("Edit the active layer's style: plate, glow, shadow, outline, fill, shade and bevel"));
    connect(layers_, &LayersDock::styleRequested, this, &MainWindow::editLayerStyle);
    QAction* bake = layerMenu_->addAction(tr("&Bake Layer"), this, [this] { doc_.bakeLayer(doc_.activeLayer()); });
    bake->setObjectName(QStringLiteral("bakeLayer"));
    bake->setToolTip(tr("Make the layer a cells layer of what it shows: a text layer's text, or a layer's style, "
                        "can't be edited after"));
    const auto canBake = [this, bake, style] {
        const Layer* l = doc_.active();
        bake->setEnabled(Document::canBake(l));
        style->setEnabled(l && (l->props.kind == LayerKind::Cells || l->props.kind == LayerKind::Pixels ||
                                (l->props.kind == LayerKind::Text && l->props.text)));
    };
    connect(&doc_, &Document::layersChanged, bake, canBake);
    connect(&doc_, &Document::activeLayerChanged, bake, canBake);
    connect(&doc_, &Document::reset, bake, canBake);
    canBake();

    lint_ = new LintDock(&doc_);
    lintDock_ = dock(tr("Lint"), "lintDock", lint_);
    ircView_ = new IrcViewDock(&doc_, ircMeter_);
    ircViewDock_ = dock(tr("IRC View"), "ircViewDock", ircView_);
    // it scrolls with the canvas (#394)
    ircView_->followCanvas(view_, scroll_);
    // These panels' forms, lists and buttons otherwise add their minimum
    // heights across the right column. Keep the content's own minimums,
    // scrolling the whole panel when the column has less height to share.
    for (QDockWidget* d : {layersDock_, libraryDock_, lintDock_, ircViewDock_}) {
        QWidget* content = d->widget();
        auto* scroll = new PanelScroll;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        d->setWidget(scroll);
    }
    // a tab behind another is still visible to Qt, off-screen
    connect(lintDock_, &QDockWidget::visibilityChanged, lint_, &FollowsDocument::setShown);
    connect(ircViewDock_, &QDockWidget::visibilityChanged, ircView_, &FollowsDocument::setShown);
    // View > IRC: the dock's toggle under the row meter's
    if (auto* irc = findChild<QMenu*>(QStringLiteral("ircMenu")))
        irc->insertAction(findChild<QAction*>(QStringLiteral("ircBudget")), ircViewDock_->toggleViewAction());
    // a finding's cell: the cursor goes there, and the view scrolls to it
    connect(lint_, &LintDock::findingActivated, this, [this](QPoint cell) {
        view_->setCellCursor(Cursor{cell.x(), cell.y()});
        view_->setFocus();
    });

    // The pen's swap arrow and reset corner.
    QAction* swap = findChild<QAction*>(QStringLiteral("swapColours"));
    connect(palette_, &PaletteWidget::swapRequested, swap, &QAction::trigger);
    QAction* reset = findChild<QAction*>(QStringLiteral("defaultColours"));
    connect(palette_, &PaletteWidget::resetRequested, reset, &QAction::trigger);
    palette_->setKeyActions(swap, reset);

    // Window: every dock, then the reset. A dock that can't close (the
    // preview, the palette, the Library) is always shown: Qt lists it
    // checked and disabled.
    for (QDockWidget* d : docks()) {
        QAction* a = d->toggleViewAction();
        a->setObjectName(QStringLiteral("show_") + d->objectName());
        a->setToolTip(tr("Show or hide the %1 dock").arg(d->windowTitle()));
        windowMenu_->addAction(a);
    }
    windowMenu_->addSeparator();
    QAction* resetLayoutAction = windowMenu_->addAction(tr("&Reset Layout"), this, &MainWindow::resetLayout);
    resetLayoutAction->setObjectName(QStringLiteral("resetLayout"));
    resetLayoutAction->setToolTip(tr("Put every dock back where it starts"));

    for (QDockWidget* d : docks())
        d->installEventFilter(this);
    // Narrowing the left column can give the toolbox another row.
    connect(toolbox_, &Toolbox::rowsChanged, this, [this] {
        if (followDefaultDockHeights_.testFlag(Qt::LeftDockWidgetArea))
            queueDefaultDockSizes();
    });
    resetLayout();
}

QList<QDockWidget*> MainWindow::docks() const {
    return {toolboxDock_,  toolOptionsDock_, previewDock_,  paletteDock_, layersDock_,
            libraryDock_, lintDock_,       ircViewDock_, glyphBarDock_};
}

void MainWindow::resetLayout() {
    // Don't enforce the taller intermediate layout while the IRC View and
    // Lint are separate docks: only the finished, tabbed layout must fit.
    layout()->setEnabled(false);
    // Left: the toolbox over its options. Right: the preview, the palette,
    // the layers, the library, and Lint tabbed with the IRC View. Bottom: the
    // glyph bar.
    const std::pair<QDockWidget*, Qt::DockWidgetArea> places[] = {
        {toolboxDock_, Qt::LeftDockWidgetArea},  {toolOptionsDock_, Qt::LeftDockWidgetArea},
        {previewDock_, Qt::RightDockWidgetArea}, {paletteDock_, Qt::RightDockWidgetArea},
        {layersDock_, Qt::RightDockWidgetArea},  {libraryDock_, Qt::RightDockWidgetArea},
        {lintDock_, Qt::RightDockWidgetArea},    {ircViewDock_, Qt::RightDockWidgetArea},
        {glyphBarDock_, Qt::BottomDockWidgetArea},
    };
    for (const auto& [d, area] : places) {
        d->setFloating(false);
        removeDockWidget(d);
    }
    for (const auto& [d, area] : places) {
        addDockWidget(area, d);
        d->show();
    }
    // the second goes on top
    tabifyDockWidget(ircViewDock_, lintDock_);
    layout()->setEnabled(true);
    layout()->activate();
    defaultDockWidthsPending_ = true;
    endRestoredDockWidthClamp();
    if (isVisible())
        applyDefaultDockSizes();
}

void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (defaultDockWidthsPending_) {
        applyDefaultDockSizes();
    } else if (clampRestoredDockWidths_) {
        // The window manager can still size the window until it is on
        // screen: until then each size is the restore's (#244).
        clampRestoredDockWidths();
        QWindow* const window = windowHandle();
        if (window && !window->isExposed())
            window->installEventFilter(this);
        else
            endRestoredDockWidthClamp();
    }
}

bool MainWindow::event(QEvent* e) {
    // Qt moves a dragged separator on a timer: a column whose width changes
    // then is the user's.
    const bool moving = separatorDrag_ && e->type() == QEvent::Timer;
    const QSize columns = moving ? dockColumnWidths() : QSize();
    const QList<int> heights = moving ? dockHeights(separatorArea_) : QList<int>();
    const bool handled = QMainWindow::event(e);
    if (e->type() == QEvent::MouseButtonPress) {
        // QMainWindow takes a left press on a separator, beside the canvas or
        // between docks; QWidget ignores a press anywhere else.
        auto* press = static_cast<QMouseEvent*>(e);
        separatorDrag_ = handled && e->isAccepted() && press->button() == Qt::LeftButton;
        separatorArea_ =
            separatorDrag_ ? heightSeparatorArea(press->position().toPoint()) : Qt::NoDockWidgetArea;
    } else if (e->type() == QEvent::MouseButtonRelease) {
        // A drag moves the other areas' docks too (the glyph bar's shortens
        // both columns): the ones still following go back to their defaults.
        if (std::exchange(separatorDrag_, false) && followDefaultDockHeights_)
            applyDefaultDockHeights();
        separatorArea_ = Qt::NoDockWidgetArea;
    } else if (moving) {
        if (dockColumnWidths() != columns)
            keepDockWidths();
        if (separatorArea_ != Qt::NoDockWidgetArea && dockHeights(separatorArea_) != heights)
            followDefaultDockHeights_.setFlag(separatorArea_, false);
    }
    return handled;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* e) {
    // A restored layout's window is on screen: a size from now on is the
    // user's, not the restore's.
    if (e->type() == QEvent::Expose && watched == windowHandle() && windowHandle()->isExposed())
        endRestoredDockWidthClamp();
    // A dock opening or closing changes its column's minimum and the room
    // the others share: apply the defaults again once the layout has caught up.
    if ((e->type() == QEvent::ShowToParent || e->type() == QEvent::HideToParent) &&
        docks().contains(qobject_cast<QDockWidget*>(watched)))
        queueDefaultDockSizes();
    return QMainWindow::eventFilter(watched, e);
}

void MainWindow::queueDefaultDockSizes() {
    if (!isVisible() || (!followDefaultDockWidths_ && !followDefaultDockHeights_))
        return;
    const bool queued = dockColumnChangePending_ || dockHeightChangePending_;
    dockColumnChangePending_ = dockColumnChangePending_ || followDefaultDockWidths_;
    dockHeightChangePending_ = dockHeightChangePending_ || followDefaultDockHeights_;
    if (queued)
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (dockColumnChangePending_ && followDefaultDockWidths_ && isVisible())
                applyDefaultDockWidths();
            dockColumnChangePending_ = false;
            if (dockHeightChangePending_ && followDefaultDockHeights_ && isVisible())
                applyDefaultDockHeights();
            dockHeightChangePending_ = false;
        },
        Qt::QueuedConnection);
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    // A window manager can fit the window after showEvent. Only the user
    // sizing a column ends this; Qt squeezing or widening one doesn't.
    if (followDefaultDockWidths_ && isVisible())
        applyDefaultDockWidths();
    else if (clampRestoredDockWidths_ && isVisible())
        clampRestoredDockWidths();
    if (followDefaultDockHeights_ && isVisible())
        applyDefaultDockHeights();
}

void MainWindow::resizeDocks(const QList<QDockWidget*>& docks, const QList<int>& sizes, Qt::Orientation orientation) {
    if (orientation == Qt::Horizontal && std::any_of(docks.begin(), docks.end(), [this](QDockWidget* d) {
            const Qt::DockWidgetArea area = dockWidgetArea(d);
            return area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
        }))
        keepDockWidths();
    if (orientation == Qt::Vertical)
        for (QDockWidget* d : docks)
            if (!d->isFloating())
                followDefaultDockHeights_.setFlag(dockWidgetArea(d), false);
    QMainWindow::resizeDocks(docks, sizes, orientation);
}

void MainWindow::applyDefaultDockSizes() {
    applyDefaultDockWidths();
    followDefaultDockHeights_ = Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea;
    applyDefaultDockHeights();
}

void MainWindow::keepDockWidths() {
    followDefaultDockWidths_ = false;
    dockColumnChangePending_ = false;
    endRestoredDockWidthClamp();
}

void MainWindow::applyDefaultDockWidths() {
    defaultDockWidthsPending_ = false;
    resizeDockColumns(kLeftColumnWidth, kRightColumnWidth);
    keepCanvasFloor(ColumnShrink::DefaultOrder);
    followDefaultDockWidths_ = true;
    dockColumnChangePending_ = false;
}

void MainWindow::clampRestoredDockWidths() {
    // Each settling size starts from the widths the layout was saved with,
    // so a window the window manager then widens gets them back.
    if (!restoredColumns_.isValid())
        restoredColumns_ = dockColumnWidths();
    resizeDockColumns(restoredColumns_.width(), restoredColumns_.height());
    keepCanvasFloor(ColumnShrink::Proportional);
}

void MainWindow::endRestoredDockWidthClamp() {
    clampRestoredDockWidths_ = false;
    if (QWindow* const window = windowHandle())
        window->removeEventFilter(this);
}

void MainWindow::resizeDockColumns(int leftWidth, int rightWidth) {
    // One open dock per column sizes the whole vertical stack, including
    // tabs.
    QDockWidget* const left = columnDock(Qt::LeftDockWidgetArea);
    QDockWidget* const right = columnDock(Qt::RightDockWidgetArea);
    QList<QDockWidget*> columns;
    QList<int> widths;
    if (left) {
        columns << left;
        widths << leftWidth;
    }
    if (right) {
        columns << right;
        widths << rightWidth;
    }
    QMainWindow::resizeDocks(columns, widths, Qt::Horizontal);
    layout()->activate();
    // The canvas's scroll bars show or hide in a slot queued on their new
    // range (#238): run it now, so a bar the new size brings is measured.
    QCoreApplication::sendPostedEvents(scroll_, QEvent::MetaCall);
}

void MainWindow::keepCanvasFloor(ColumnShrink shrink) {
    // Measure the viewport so native separators, scrollbars and the IRC
    // gutter count as space the canvas can't use.
    const int deficit = kCanvasViewportFloor - scroll_->viewport()->width();
    if (deficit <= 0)
        return;
    const QSize columns = dockColumnWidths();
    const int leftMinimum = columnMinimum(Qt::LeftDockWidgetArea);
    const int leftRoom = std::max(0, columns.width() - leftMinimum);
    const int rightRoom = std::max(0, columns.height() - columnMinimum(Qt::RightDockWidgetArea));
    int leftShrink = 0;
    int rightShrink = 0;
    if (deficit >= leftRoom + rightRoom) {
        // When even the minimums can't leave 640px, give the canvas all we
        // can.
        leftShrink = leftRoom;
        rightShrink = rightRoom;
    } else if (shrink == ColumnShrink::Proportional) {
        // Each column gives in proportion to its width, and the other takes
        // on what one can't give above its minimum.
        const int total = columns.width() + columns.height();
        const int share = int((qint64(deficit) * columns.width() + total / 2) / total);
        leftShrink = std::clamp(share, deficit - rightRoom, leftRoom);
        rightShrink = deficit - leftShrink;
    } else {
        // The left column's width above 256 goes first, then the right column
        // shrinks while it has room, which keeps the options readable.
        const int leftExtra = std::max(0, columns.width() - std::max(kLeftColumnNarrow, leftMinimum));
        const int leftGive = std::min(deficit, leftExtra);
        rightShrink = std::min(deficit - leftGive, rightRoom);
        leftShrink = leftGive + std::min(deficit - leftGive - rightShrink, leftRoom - leftGive);
    }
    resizeDockColumns(columns.width() - leftShrink, columns.height() - rightShrink);
}

void MainWindow::applyDefaultDockHeights() {
    dockHeightChangePending_ = false;
    // The glyph bar first: its height sets the side columns'.
    if (followDefaultDockHeights_.testFlag(Qt::BottomDockWidgetArea) &&
        dockStack(Qt::BottomDockWidgetArea).contains(glyphBarDock_)) {
        QMainWindow::resizeDocks({glyphBarDock_}, {glyphBarDock_->sizeHint().height()}, Qt::Vertical);
        layout()->activate();
    }
    const int separator = style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, this);
    // The side columns share the canvas's rows.
    const QRect band = centralWidget()->geometry();
    for (const Qt::DockWidgetArea area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea}) {
        const QList<QDockWidget*> stack = dockStack(area);
        if (!followDefaultDockHeights_.testFlag(area) || stack.size() < 2)
            continue;
        const int available = band.height() - int(stack.size() - 1) * separator;
        // The dock that takes what the others leave, and the ones that give
        // up height first when the column is short (PLAN §11).
        QDockWidget* const rest = area == Qt::LeftDockWidgetArea ? toolOptionsDock_ : libraryDock_;
        const QDockWidget* const shrinkOrder[] = {lintDock_, ircViewDock_, previewDock_, layersDock_};
        QList<int> sizes;
        QList<int> minimums;
        for (int i = 0; i < stack.size(); ++i) {
            QDockWidget* const d = stack[i];
            // A tabbed group's tab bar is under its front dock, in its slot.
            const int end = i + 1 < stack.size() ? stack[i + 1]->y() - separator : band.bottom() + 1;
            const int tabs = std::max(0, end - d->geometry().bottom() - 1);
            const int title = d->height() - d->widget()->height();
            int target = d->height();
            if (d == toolboxDock_ || d == paletteDock_) {
                target = d->sizeHint().height();
            } else if (d == previewDock_) {
                const QSize cell = renderer_.cellSize();
                const QSize piece = DeviceGrid::of(preview_).widgetSize(
                    {kPreviewPiece.width() * cell.width(), kPreviewPiece.height() * cell.height()});
                const auto* scroll = static_cast<QScrollArea*>(previewDock_->widget());
                int content = piece.height() + 2 * scroll->frameWidth();
                if (piece.width() + 2 * scroll->frameWidth() > d->width())
                    content += scroll->horizontalScrollBar()->sizeHint().height();
                target = std::min(title + content, available / kPreviewColumnShare);
            } else if (d == layersDock_ || d == lintDock_ || d == ircViewDock_) {
                // The IRC View shares Lint's tab and its height.
                const QDockWidget* panel = d == ircViewDock_ ? lintDock_ : d;
                target = title + panelHeight(static_cast<const QScrollArea*>(panel->widget()), d->width());
            }
            minimums << tabs + std::max(d->minimumHeight(), d->minimumSizeHint().height());
            sizes << std::max(tabs + target, minimums.back());
        }
        if (const int r = int(stack.indexOf(rest)); r >= 0) {
            int others = 0;
            for (int i = 0; i < stack.size(); ++i)
                if (i != r)
                    others += sizes[i];
            sizes[r] = available - others;
            int deficit = minimums[r] - sizes[r];
            sizes[r] = std::max(sizes[r], minimums[r]);
            for (const QDockWidget* d : shrinkOrder) {
                const int i = int(stack.indexOf(d));
                if (deficit <= 0 || i < 0)
                    continue;
                const int give = std::min(deficit, sizes[i] - minimums[i]);
                sizes[i] -= give;
                deficit -= give;
            }
        }
        QMainWindow::resizeDocks(stack, sizes, Qt::Vertical);
        layout()->activate();
    }
}

QList<QDockWidget*> MainWindow::dockStack(Qt::DockWidgetArea area) const {
    QList<QDockWidget*> stack;
    for (QDockWidget* d : docks())
        if (!d->isHidden() && !d->isFloating() && dockWidgetArea(d) == area && d->geometry().intersects(rect()))
            stack << d;
    std::stable_sort(stack.begin(), stack.end(), [](const QDockWidget* a, const QDockWidget* b) {
        return a->y() != b->y() ? a->y() < b->y() : a->x() < b->x();
    });
    return stack;
}

QList<int> MainWindow::dockHeights(Qt::DockWidgetArea area) const {
    QList<int> heights;
    if (area != Qt::NoDockWidgetArea)
        for (const QDockWidget* d : dockStack(area))
            heights << d->height();
    return heights;
}

Qt::DockWidgetArea MainWindow::heightSeparatorArea(QPoint pos) const {
    for (const Qt::DockWidgetArea area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea}) {
        QRect column;
        for (const QDockWidget* d : dockStack(area))
            column |= d->geometry();
        if (column.contains(pos))
            return area;
    }
    if (pos.y() > centralWidget()->geometry().bottom())
        return Qt::BottomDockWidgetArea;
    return Qt::NoDockWidgetArea;
}

QDockWidget* MainWindow::columnDock(Qt::DockWidgetArea area) const {
    // docks() lists the Toolbox and the preview first in their columns. Qt
    // moves a tab behind another out of the window.
    for (QDockWidget* d : docks())
        if (!d->isHidden() && !d->isFloating() && dockWidgetArea(d) == area && d->geometry().intersects(rect()))
            return d;
    return nullptr;
}

int MainWindow::columnMinimum(Qt::DockWidgetArea area) const {
    int minimum = 0;
    for (QDockWidget* d : docks())
        // A closed or floating dock doesn't hold its column open.
        if (!d->isHidden() && !d->isFloating() && dockWidgetArea(d) == area)
            minimum = std::max({minimum, d->minimumWidth(), d->minimumSizeHint().width()});
    return minimum;
}

QSize MainWindow::dockColumnWidths() const {
    const QDockWidget* left = columnDock(Qt::LeftDockWidgetArea);
    const QDockWidget* right = columnDock(Qt::RightDockWidgetArea);
    return {left ? left->width() : 0, right ? right->width() : 0};
}

void MainWindow::saveLayout() const {
    const auto s = appSettings();
    s->setValue(QStringLiteral("window/geometry"), saveGeometry());
    s->setValue(QStringLiteral("window/state"), saveState(kLayoutVersion));
}

void MainWindow::restoreLayout() {
    const auto s = appSettings();
    restoreGeometry(s->value(QStringLiteral("window/geometry")).toByteArray());
    defaultDockWidthsPending_ = !restoreState(s->value(QStringLiteral("window/state")).toByteArray(), kLayoutVersion);
    // The restored widths stand unless they would leave the canvas under its
    // floor, checked when the window first shows and settles (#244).
    clampRestoredDockWidths_ = !defaultDockWidthsPending_;
    restoredColumns_ = QSize();
    // The docks that can't close are always shown, whatever was saved.
    for (QDockWidget* d : docks())
        if (!d->features().testFlag(QDockWidget::DockWidgetClosable))
            d->show();
}

} // namespace bw
