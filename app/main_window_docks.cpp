// The workspace (PLAN §11): the Toolbox, Tool Options, glyph bar, Layers,
// Lint and IRC View docks, the Layer menu, the Window menu, and the dock
// layout saved between runs.

#include "main_window.h"

#include "canvas_view.h"
#include "glyph_bar.h"
#include "irc_meter.h"
#include "layers_dock.h"
#include "lint_dock.h"
#include "palette_widget.h"
#include "select_tool.h"
#include "settings.h"
#include "tool.h"
#include "tool_options.h"
#include "toolbox.h"

#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QLayout>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QResizeEvent>
#include <QShowEvent>

#include <algorithm>

namespace bw {

namespace {

// Bumped when the docks change, so an older saved layout is dropped.
constexpr int kLayoutVersion = 4;
constexpr int kLeftColumnWidth = 256;
constexpr int kRightColumnWidth = 360;
// An 80-column piece at 1x, excluding the frame, scrollbar and IRC gutter.
constexpr int kCanvasViewportFloor = 640;

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
    toolOptions_ = new ToolOptions(view_, tools_, selectTool_, findChild<QAction*>(QStringLiteral("filledShapes")));
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
    // Bake Text: a live text layer's cells kept, its source dropped (PLAN
    // §15), one undo step.
    layerMenu_->addSeparator();
    QAction* bake = layerMenu_->addAction(tr("&Bake Text"), this, [this] { doc_.bakeTextLayer(doc_.activeLayer()); });
    bake->setObjectName(QStringLiteral("bakeText"));
    bake->setToolTip(tr("Make the text layer a cells layer of what it shows; its text can't be edited after"));
    const auto canBake = [this, bake] {
        const Layer* l = doc_.active();
        bake->setEnabled(l && l->props.text);
    };
    connect(&doc_, &Document::layersChanged, bake, canBake);
    connect(&doc_, &Document::activeLayerChanged, bake, canBake);
    connect(&doc_, &Document::reset, bake, canBake);
    canBake();

    lint_ = new LintDock(&doc_);
    lintDock_ = dock(tr("Lint"), "lintDock", lint_);
    ircView_ = new IrcViewDock(&doc_);
    ircViewDock_ = dock(tr("IRC View"), "ircViewDock", ircView_);
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
    if (isVisible())
        applyDefaultDockWidths();
}

void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (defaultDockWidthsPending_)
        applyDefaultDockWidths();
}

bool MainWindow::event(QEvent* e) {
    // Qt moves a dragged separator on a timer: a column whose width changes
    // then is the user's.
    const bool moving = separatorDrag_ && e->type() == QEvent::Timer;
    const QSize columns = moving ? dockColumnWidths() : QSize();
    const bool handled = QMainWindow::event(e);
    if (e->type() == QEvent::MouseButtonPress) {
        // QMainWindow takes a left press on a separator, beside the canvas or
        // between docks; QWidget ignores a press anywhere else.
        separatorDrag_ =
            handled && e->isAccepted() && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton;
    } else if (e->type() == QEvent::MouseButtonRelease) {
        separatorDrag_ = false;
    } else if (moving && dockColumnWidths() != columns) {
        keepDockWidths();
    }
    return handled;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* e) {
    // A dock opening or closing changes its column's minimum: apply the
    // defaults again once the layout has caught up.
    if ((e->type() == QEvent::ShowToParent || e->type() == QEvent::HideToParent) && followDefaultDockWidths_ &&
        isVisible() && !dockColumnChangePending_ && docks().contains(qobject_cast<QDockWidget*>(watched))) {
        dockColumnChangePending_ = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (dockColumnChangePending_ && followDefaultDockWidths_ && isVisible())
                    applyDefaultDockWidths();
                dockColumnChangePending_ = false;
            },
            Qt::QueuedConnection);
    }
    return QMainWindow::eventFilter(watched, e);
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    // A window manager can fit the window after showEvent. Only the user
    // sizing a column ends this; Qt squeezing or widening one doesn't.
    if (followDefaultDockWidths_ && isVisible())
        applyDefaultDockWidths();
}

void MainWindow::resizeDocks(const QList<QDockWidget*>& docks, const QList<int>& sizes, Qt::Orientation orientation) {
    if (orientation == Qt::Horizontal && std::any_of(docks.begin(), docks.end(), [this](QDockWidget* d) {
            const Qt::DockWidgetArea area = dockWidgetArea(d);
            return area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
        }))
        keepDockWidths();
    QMainWindow::resizeDocks(docks, sizes, orientation);
}

void MainWindow::keepDockWidths() {
    followDefaultDockWidths_ = false;
    dockColumnChangePending_ = false;
}

void MainWindow::applyDefaultDockWidths() {
    defaultDockWidthsPending_ = false;
    // One open dock per column sizes the whole vertical stack, including
    // tabs. Measure the resulting viewport so native separators, scrollbars
    // and the IRC gutter count as space the canvas can't use.
    QDockWidget* const left = columnDock(Qt::LeftDockWidgetArea);
    QDockWidget* const right = columnDock(Qt::RightDockWidgetArea);
    const auto resizeColumns = [&](int leftWidth, int rightWidth) {
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
    };
    resizeColumns(kLeftColumnWidth, kRightColumnWidth);
    const int deficit = kCanvasViewportFloor - scroll_->viewport()->width();
    if (deficit > 0) {
        const QSize columns = dockColumnWidths();
        const int leftRoom = std::max(0, columns.width() - columnMinimum(Qt::LeftDockWidgetArea));
        const int rightRoom = std::max(0, columns.height() - columnMinimum(Qt::RightDockWidgetArea));
        // Keep the options readable while the right column has room to give.
        // When even the minimums can't leave 640px, give the canvas all we can.
        const int rightShrink = std::min(deficit, rightRoom);
        const int leftShrink = std::min(deficit - rightShrink, leftRoom);
        resizeColumns(columns.width() - leftShrink, columns.height() - rightShrink);
    }
    followDefaultDockWidths_ = true;
    dockColumnChangePending_ = false;
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
    // The docks that can't close are always shown, whatever was saved.
    for (QDockWidget* d : docks())
        if (!d->features().testFlag(QDockWidget::DockWidgetClosable))
            d->show();
}

} // namespace bw
