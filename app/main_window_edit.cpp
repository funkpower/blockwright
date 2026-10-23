// The selection, mirror, guide and reference-image parts of the main window:
// the Tools and Selection menus, and their items in File, Edit and View.

#include "main_window.h"

#include "canvas_view.h"
#include "draw_tools.h"
#include "select_tool.h"
#include "tool.h"

#include <QAction>
#include <QActionGroup>
#include <QFileDialog>
#include <QLabel>
#include <QImageReader>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

namespace bw {

bool MainWindow::loadReferenceImage(const QString& path, QString* error) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        *error = reader.errorString();
        return false;
    }
    view_->setReferenceImage(image);
    view_->setReferenceVisible(true);
    return true;
}

void MainWindow::createEditingActions(QMenu* file, QMenu* edit, QMenu* view) {
    tools_ = new ToolRegistry(this);
    tools_->add(new Tool(QStringLiteral("keyboard"), tr("&Keyboard")));
    selectTool_ = new SelectTool;
    tools_->add(selectTool_);
    tools_->add(new HalfBlockTool);
    tools_->add(new ShadeTool);
    tools_->add(new RampShadeTool);
    tools_->add(new FormTool);
    tools_->add(new SoftTool);
    tools_->add(new DespeckleTool);
    tools_->add(new GlyphTool);
    tools_->add(new ColorizeTool);
    QList<ShapeTool*> fillable;
    tools_->add(new ShapeTool(ShapeTool::Shape::Line));
    tools_->add(fillable.emplace_back(new ShapeTool(ShapeTool::Shape::Rectangle)));
    tools_->add(fillable.emplace_back(new ShapeTool(ShapeTool::Shape::Ellipse)));
    tools_->add(new FillTool);
    tools_->add(new EyedropperTool);
    tools_->add(new RimLightTool);
    view_->setTool(tools_->active());
    connect(tools_, &ToolRegistry::activeChanged, view_, &CanvasView::setTool);

    const auto add = [this](QMenu* m, const QString& text, const char* name, const QKeySequence& key, auto slot) {
        QAction* act = m->addAction(text, this, slot);
        act->setObjectName(QString::fromLatin1(name));
        act->setShortcut(key);
        return act;
    };
    const auto check = [](QAction* a, QActionGroup* group, bool on) {
        a->setCheckable(true);
        a->setChecked(on);
        if (group)
            group->addAction(a);
        return a;
    };
    // The selection is the view's, so it can be there in any tool; the
    // actions that work on it start the selection tool.
    const auto selecting = [this] {
        tools_->setActive(selectTool_);
        return selectTool_;
    };

    // File: the reference image, above Quit.
    const QList<QAction*> fileActions = file->actions();
    QAction* quitSeparator = fileActions.isEmpty() ? nullptr : fileActions.back();
    if (fileActions.size() >= 2 && fileActions.at(fileActions.size() - 2)->isSeparator())
        quitSeparator = fileActions.at(fileActions.size() - 2);
    auto* openReference = new QAction(tr("Open &Reference Image..."), this);
    openReference->setObjectName(QStringLiteral("openReferenceImage"));
    connect(openReference, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Reference Image"), QString(),
                                                          tr("Images (*.png *.jpg *.jpeg);;All files (*)"));
        QString error;
        if (!path.isEmpty() && !loadReferenceImage(path, &error))
            QMessageBox::warning(this, tr("Open Reference Image"), tr("Could not open %1:\n%2").arg(path, error));
    });
    file->insertSeparator(quitSeparator);
    file->insertAction(quitSeparator, openReference);

    // Edit: clipboard and mirror mode. Delete is the tool's own key, so in
    // keyboard mode it still clears the cell under the cursor.
    edit->addSeparator();
    QList<QAction*> needSelection;
    needSelection << add(edit, tr("Cu&t"), "cut", QKeySequence(Qt::CTRL | Qt::Key_X), [selecting] { selecting()->cut(); })
                  << add(edit, tr("&Copy"), "copy", QKeySequence(Qt::CTRL | Qt::Key_C),
                         [selecting] { selecting()->copy(); });
    QAction* paste = add(edit, tr("&Paste"), "paste", QKeySequence(Qt::CTRL | Qt::Key_V),
                         [selecting] { selecting()->paste(); });
    needSelection << add(edit, tr("&Delete") + QStringLiteral("\t") + QKeySequence(Qt::Key_Delete).toString(
                                                                          QKeySequence::NativeText),
                         "deleteSelection", QKeySequence(), [selecting] { selecting()->clear(); });
    edit->addSeparator();
    QAction* mirror = check(add(edit, tr("&Mirror Mode"), "mirrorMode", QKeySequence(Qt::CTRL | Qt::Key_M), [] {}),
                            nullptr, view_->mirror());
    connect(mirror, &QAction::toggled, view_, &CanvasView::setMirror);
    connect(view_, &CanvasView::mirrorChanged, mirror, &QAction::setChecked);
    // The mirror toggle's menu: what mirror mode repeats a stroke as (PLAN
    // §10). Picking one turns mirror mode on; the Brush dock sets radial's
    // folds and centre.
    QMenu* symmetry = edit->addMenu(tr("S&ymmetry"));
    auto* symmetryGroup = new QActionGroup(this);
    struct SymmetryEntry {
        const char* name;
        QString text;
        SymmetryMode mode;
    };
    for (const SymmetryEntry& m : {SymmetryEntry{"symmetryMirrorX", tr("Mirror &X"), SymmetryMode::MirrorX},
                                   SymmetryEntry{"symmetryMirrorY", tr("Mirror &Y"), SymmetryMode::MirrorY},
                                   SymmetryEntry{"symmetryMirrorXY", tr("Mirror X &and Y"), SymmetryMode::MirrorXY},
                                   SymmetryEntry{"symmetryRadial", tr("&Radial"), SymmetryMode::Radial}}) {
        QAction* a = check(add(symmetry, m.text, m.name, QKeySequence(),
                               [this, mode = m.mode] {
                                   Symmetry s = view_->symmetryChoice();
                                   s.mode = mode;
                                   view_->setSymmetry(s);
                               }),
                           symmetryGroup, view_->symmetryChoice().mode == m.mode);
        connect(view_, &CanvasView::symmetryChanged, a,
                [this, a, mode = m.mode] { a->setChecked(view_->symmetryChoice().mode == mode); });
    }

    // View: guides and the reference image.
    view->addSeparator();
    QMenu* guides = view->addMenu(tr("&Guides"));
    auto* guideGroup = new QActionGroup(this);
    struct Guide {
        const char* name;
        QSize size;
    };
    for (const Guide& g : {Guide{"guideNone", {}}, Guide{"guide80x25", {80, 25}}, Guide{"guide80x40", {80, 40}},
                           Guide{"guide80x50", {80, 50}}}) {
        const QString text = g.size.isEmpty() ? tr("&None") : tr("%1x%2").arg(g.size.width()).arg(g.size.height());
        check(add(guides, text, g.name, QKeySequence(), [this, s = g.size] { view_->setGuide(s); }), guideGroup,
              g.size == view_->guide());
    }
    QMenu* reference = view->addMenu(tr("&Reference Image"));
    QAction* showReference = check(add(reference, tr("&Show"), "showReferenceImage", QKeySequence(), [] {}), nullptr,
                                   view_->referenceVisible());
    connect(showReference, &QAction::toggled, view_, &CanvasView::setReferenceVisible);
    QMenu* opacity = reference->addMenu(tr("&Opacity"));
    auto* opacityGroup = new QActionGroup(this);
    for (const int percent : {25, 50, 75, 100})
        check(add(opacity, tr("%1%").arg(percent), "", QKeySequence(),
                  [this, percent] { view_->setReferenceOpacity(percent / 100.0); }),
              opacityGroup, qRound(view_->referenceOpacity() * 100) == percent)
            ->setObjectName(QStringLiteral("referenceOpacity%1").arg(percent));
    QAction* clearReference = add(reference, tr("&Clear"), "clearReferenceImage", QKeySequence(),
                                  [this] { view_->setReferenceImage({}); });
    const auto updateReference = [this, showReference, opacity, clearReference] {
        const bool any = !view_->referenceImage().isNull();
        showReference->setEnabled(any);
        opacity->setEnabled(any);
        clearReference->setEnabled(any);
        showReference->setChecked(view_->referenceVisible());
    };
    connect(view_, &CanvasView::referenceChanged, this, updateReference);
    connect(view_, &CanvasView::referenceFailed, this, [this] {
        statusBar()->showMessage(tr("The reference image is too large to show at this zoom"), 5000);
    });

    // Tools: one checkable item per tool in the registry.
    QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    auto* toolGroup = new QActionGroup(this);
    const QHash<QString, QKeySequence> toolKeys{
        {QStringLiteral("keyboard"), QKeySequence(Qt::CTRL | Qt::Key_K)},
        {QStringLiteral("select"), QKeySequence(Qt::CTRL | Qt::Key_E)},
        {QStringLiteral("halfBlock"), QKeySequence(Qt::CTRL | Qt::Key_B)},
        {QStringLiteral("shade"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)},
        {QStringLiteral("rampShade"), QKeySequence(Qt::CTRL | Qt::Key_T)},
        {QStringLiteral("form"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)},
        {QStringLiteral("soft"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
        {QStringLiteral("despeckle"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)},
        {QStringLiteral("glyph"), QKeySequence(Qt::CTRL | Qt::Key_G)},
        {QStringLiteral("colorize"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
        {QStringLiteral("line"), QKeySequence(Qt::CTRL | Qt::Key_L)},
        {QStringLiteral("rectangle"), QKeySequence(Qt::CTRL | Qt::Key_U)},
        {QStringLiteral("ellipse"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)},
        {QStringLiteral("fill"), QKeySequence(Qt::CTRL | Qt::Key_J)},
        {QStringLiteral("eyedropper"), QKeySequence(Qt::CTRL | Qt::Key_I)},
        {QStringLiteral("rimLight"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L)}};
    QHash<Tool*, QAction*> toolActions;
    for (Tool* t : tools_->tools()) {
        QAction* a = add(toolsMenu, t->name(), "", toolKeys.value(t->id()), [this, t] { tools_->setActive(t); });
        a->setObjectName(t->id() + QStringLiteral("Tool"));
        check(a, toolGroup, t == tools_->active());
        toolActions.insert(t, a);
    }
    connect(tools_, &ToolRegistry::activeChanged, this, [toolActions](Tool* t) {
        if (QAction* a = toolActions.value(t))
            a->setChecked(true);
    });
    // The same tools on a toolbar.
    QToolBar* toolBar = addToolBar(tr("Tools"));
    toolBar->setObjectName(QStringLiteral("toolsToolBar"));
    for (Tool* t : tools_->tools())
        toolBar->addAction(toolActions.value(t));
    // Rectangles and ellipses: outlines, or filled.
    toolsMenu->addSeparator();
    QAction* filled = check(add(toolsMenu, tr("Filled Sha&pes"), "filledShapes",
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [] {}),
                            nullptr, false);
    connect(filled, &QAction::toggled, this, [fillable](bool on) {
        for (ShapeTool* t : fillable)
            t->setFilled(on);
    });
    // The brushes' size, a cell at a time (the Brush dock has the rest).
    const auto stepSize = [this](int by) {
        BrushSettings s = view_->brushSettings();
        s.size += by;
        view_->setBrushSettings(s);
    };
    add(toolsMenu, tr("&Smaller Brush"), "brushSmaller", QKeySequence(Qt::CTRL | Qt::Key_BracketLeft),
        [stepSize] { stepSize(-1); });
    add(toolsMenu, tr("&Larger Brush"), "brushLarger", QKeySequence(Qt::CTRL | Qt::Key_BracketRight),
        [stepSize] { stepSize(1); });
    // The active tool's settings (the glyph brush's glyph) in the status bar.
    auto* toolStatus = new QLabel;
    toolStatus->setObjectName(QStringLiteral("toolStatusLabel"));
    statusBar()->insertPermanentWidget(0, toolStatus);
    const auto updateToolStatus = [this, toolStatus] {
        Tool* t = tools_->active();
        toolStatus->setText(t ? t->status() : QString());
        toolStatus->setVisible(!toolStatus->text().isEmpty());
    };
    for (Tool* t : tools_->tools())
        connect(t, &Tool::changed, this, updateToolStatus);
    connect(tools_, &ToolRegistry::activeChanged, this, updateToolStatus);
    connect(view_, &CanvasView::brushChanged, this, updateToolStatus);
    updateToolStatus();

    // Selection.
    QMenu* sel = menuBar()->addMenu(tr("&Selection"));
    add(sel, tr("Select &All"), "selectAll", QKeySequence(Qt::CTRL | Qt::Key_A), [selecting] {
        selecting()->selectAll();
    });
    needSelection << add(sel, tr("&Deselect"), "deselect", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
                         [this] {
                             if (selectTool_->view())
                                 selectTool_->deselect();
                             else
                                 view_->setSelection({});
                         });
    sel->addSeparator();
    needSelection
        << add(sel, tr("Flip &Horizontally"), "flipHorizontal", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H),
               [selecting] { selecting()->transform(Transform::FlipHorizontal); })
        << add(sel, tr("Flip &Vertically"), "flipVertical", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V),
               [selecting] { selecting()->transform(Transform::FlipVertical); })
        << add(sel, tr("Rotate &Clockwise"), "rotateClockwise", QKeySequence(Qt::CTRL | Qt::Key_R),
               [selecting] { selecting()->transform(Transform::RotateClockwise); })
        << add(sel, tr("Rotate C&ounterclockwise"), "rotateCounterClockwise",
               QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R),
               [selecting] { selecting()->transform(Transform::RotateCounterClockwise); });
    sel->addSeparator();
    QMenu* modes = sel->addMenu(tr("&Paste Mode"));
    auto* modeGroup = new QActionGroup(this);
    struct Mode {
        const char* name;
        QString text;
        PasteMode mode;
    };
    for (const Mode& m : {Mode{"pasteNormal", tr("&Normal"), PasteMode::Normal},
                          Mode{"pasteTransparent", tr("&Transparent (blank cells don't paste)"),
                               PasteMode::Transparent},
                          Mode{"pasteUnder", tr("&Under (only into blank cells)"), PasteMode::Under}}) {
        QAction* a = check(
            add(modes, m.text, m.name, QKeySequence(), [this, mode = m.mode] { selectTool_->setPasteMode(mode); }),
            modeGroup, m.mode == selectTool_->pasteMode());
        // A Library stamp floats in a mode of its own; the menu shows it.
        connect(selectTool_, &Tool::changed, a, [this, a, mode = m.mode] {
            if (selectTool_->effectivePasteMode() == mode)
                a->setChecked(true);
        });
    }
    sel->addSeparator();
    needSelection << add(sel, tr("Save to &Library..."), "saveToLibrary", QKeySequence(), [this] { saveToLibrary(); });

    const auto updateSelection = [this, needSelection, paste] {
        const bool any = selectTool_->floating() || !view_->selection().empty();
        for (QAction* a : needSelection)
            a->setEnabled(any);
        paste->setEnabled(!selectTool_->clipboard().empty());
    };
    connect(selectTool_, &Tool::changed, this, updateSelection);
    connect(view_, &CanvasView::selectionChanged, this, updateSelection);
    updateSelection();
    updateReference();
}

} // namespace bw
