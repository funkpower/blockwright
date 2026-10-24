// The workspace (PLAN §11): the Toolbox, Tool Options and glyph bar docks,
// the Window menu, and the dock layout saved between runs.

#include "main_window.h"

#include "canvas_view.h"
#include "glyph_bar.h"
#include "palette_widget.h"
#include "select_tool.h"
#include "settings.h"
#include "tool.h"
#include "tool_options.h"
#include "toolbox.h"

#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QScrollArea>

namespace bw {

namespace {

// Bumped when the docks change, so an older saved layout is dropped.
constexpr int kLayoutVersion = 1;

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

    resetLayout();
}

QList<QDockWidget*> MainWindow::docks() const {
    return {toolboxDock_, toolOptionsDock_, previewDock_, paletteDock_, libraryDock_, glyphBarDock_};
}

void MainWindow::resetLayout() {
    // Left: the toolbox over its options. Right: the preview, the palette
    // and the library. Bottom: the glyph bar.
    const std::pair<QDockWidget*, Qt::DockWidgetArea> places[] = {
        {toolboxDock_, Qt::LeftDockWidgetArea},  {toolOptionsDock_, Qt::LeftDockWidgetArea},
        {previewDock_, Qt::RightDockWidgetArea}, {paletteDock_, Qt::RightDockWidgetArea},
        {libraryDock_, Qt::RightDockWidgetArea}, {glyphBarDock_, Qt::BottomDockWidgetArea},
    };
    for (const auto& [d, area] : places) {
        d->setFloating(false);
        removeDockWidget(d);
    }
    for (const auto& [d, area] : places) {
        addDockWidget(area, d);
        d->show();
    }
}

void MainWindow::saveLayout() const {
    const auto s = appSettings();
    s->setValue(QStringLiteral("window/geometry"), saveGeometry());
    s->setValue(QStringLiteral("window/state"), saveState(kLayoutVersion));
}

void MainWindow::restoreLayout() {
    const auto s = appSettings();
    restoreGeometry(s->value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(s->value(QStringLiteral("window/state")).toByteArray(), kLayoutVersion);
    // The docks that can't close are always shown, whatever was saved.
    for (QDockWidget* d : docks())
        if (!d->features().testFlag(QDockWidget::DockWidgetClosable))
            d->show();
}

} // namespace bw
