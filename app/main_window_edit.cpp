// The selection, mirror, guide and reference-image parts of the main window:
// the Tools and Selection menus, and their items in File, Edit, View and Layer.

#include "main_window.h"

#include "action_list.h"
#include "canvas_view.h"
#include "draw_tools.h"
#include "help_text.h"
#include "whats_this.h"
#include "reference_scale.h"
#include "select_tool.h"
#include "text_tool.h"
#include "tool.h"
#include "tool_icon.h"
#include "toolbox.h"

#include <QAction>
#include <QActionGroup>
#include <QBuffer>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QImageReader>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

namespace bw {

bool MainWindow::loadReferenceImage(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    if (quint64(file.size()) > project::kMaxEntryBytes) {
        *error = tr("The picture is over %1 MiB.").arg(project::kMaxEntryBytes >> 20);
        return false;
    }
    QByteArray bytes = file.readAll();
    // Enforce the decode ceiling and retain at most the view's pixel budget.
    QSize original;
    const QImage image = readReferencePicture(bytes, CanvasView::kMaxReferencePixels, &original, error);
    if (image.isNull() || original.isEmpty()) {
        statusBar()->showMessage(tr("Could not open reference image: %1").arg(*error), 5000);
        return false;
    }
    if (!referenceImageType(std::string_view(bytes.constData(), std::size_t(bytes.size())))) {
        // kept as a PNG: the project holds PNG and JPEG pictures only
        QByteArray png;
        QBuffer out(&png);
        if (!out.open(QIODevice::WriteOnly) || !image.save(&out, "PNG")) {
            *error = tr("The picture could not be kept as a PNG.");
            return false;
        }
        bytes = png;
    }
    ReferenceImage picture;
    picture.bytes = std::make_shared<const std::string>(bytes.toStdString());
    picture.width = std::max(1, view_->canvasPixels().width());
    picture.height = int(std::clamp<double>(std::round(double(picture.width) * original.height() / original.width()),
                                            1, ReferenceImage::kMaxPlace));
    const auto kept = picture.bytes;
    if (!doc_.addReferenceLayer(std::move(picture))) {
        *error = tr("The picture can't be kept in a project.");
        return false;
    }
    view_->keepReferencePicture(kept, image);
    return true;
}

void MainWindow::createReferenceActions() {
    layerMenu_->addSeparator();
    QMenu* opacity = layerMenu_->addMenu(tr("Reference &Opacity"));
    opacity->setObjectName(QStringLiteral("referenceOpacity"));
    auto* group = new QActionGroup(this);
    for (const int percent : {25, 50, 75, 100}) {
        QAction* a = opacity->addAction(tr("%1%").arg(percent), this, [this, percent] {
            const Layer* l = doc_.active();
            if (!l || !l->props.reference || l->props.reference->opacity == percent)
                return;
            LayerProperties props = l->props;
            props.reference->opacity = percent;
            doc_.setLayerProperties(l->props.id, std::move(props), tr("Reference Opacity"));
        });
        a->setObjectName(QStringLiteral("referenceOpacity%1").arg(percent));
        a->setCheckable(true);
        group->addAction(a);
    }
    const auto update = [this, opacity, group] {
        const Layer* l = doc_.active();
        const bool on = l && l->props.reference;
        opacity->setEnabled(on);
        for (QAction* a : group->actions())
            a->setChecked(on && a->objectName() == QStringLiteral("referenceOpacity%1").arg(l->props.reference->opacity));
    };
    connect(&doc_, &Document::layersChanged, this, update);
    connect(&doc_, &Document::activeLayerChanged, this, update);
    update();
}

void MainWindow::createEditingActions(QMenu* file, QMenu* edit, QMenu* view) {
    tools_ = new ToolRegistry(this);
    tools_->add(new Tool(QStringLiteral("keyboard"), tr("&Keyboard")));
    selectTool_ = new SelectTool;
    tools_->add(selectTool_);
    tools_->add(new MoveTool);
    tools_->add(new HalfBlockTool);
    tools_->add(new ShadeTool);
    tools_->add(new RampShadeTool);
    tools_->add(new FormTool);
    tools_->add(new SoftTool);
    tools_->add(new DespeckleTool);
    tools_->add(new SmudgeTool);
    tools_->add(new GlyphTool);
    tools_->add(new ColorizeTool);
    QList<ShapeTool*> fillable;
    tools_->add(new ShapeTool(ShapeTool::Shape::Line));
    tools_->add(fillable.emplace_back(new ShapeTool(ShapeTool::Shape::Rectangle)));
    tools_->add(fillable.emplace_back(new ShapeTool(ShapeTool::Shape::Ellipse)));
    tools_->add(new FillTool);
    tools_->add(new GradientTool);
    tools_->add(new EyedropperTool);
    tools_->add(new OutlineTool);
    tools_->add(new RimLightTool);
    tools_->add(new TextureTool);
    auto* clone = new CloneTool;
    tools_->add(clone);
    // The clone source and its distance are cells of this document, so a
    // new one forgets them, the tool active or not.
    connect(&doc_, &Document::reset, clone, &CloneTool::forgetSource);
    tools_->add(new SprayTool);
    tools_->add(new TextTool);
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
    // Del is no key of the action's but the tool's, so the menu names it
    // after any key Preferences > Shortcuts gives the action.
    QAction* del = add(edit, tr("&Delete"), "deleteSelection", QKeySequence(), [selecting] { selecting()->clear(); });
    const auto delText = [del] {
        QStringList keys;
        for (const QKeySequence& k : del->shortcuts())
            keys << k.toString(QKeySequence::NativeText);
        keys << QKeySequence(Qt::Key_Delete).toString(QKeySequence::NativeText);
        del->setText(tr("&Delete") + QLatin1Char('\t') + keys.join(QStringLiteral(", ")));
    };
    delText();
    connect(del, &QAction::changed, del, delText);
    needSelection << del;
    edit->addSeparator();
    QAction* mirror = check(add(edit, tr("&Mirror Mode"), "mirrorMode", QKeySequence(Qt::CTRL | Qt::Key_M), [] {}),
                            nullptr, view_->mirror());
    connect(mirror, &QAction::toggled, view_, &CanvasView::setMirror);
    connect(view_, &CanvasView::mirrorChanged, mirror, &QAction::setChecked);
    // The mirror toggle's menu: what mirror mode repeats a stroke as (PLAN
    // §10). Picking one turns mirror mode on; Tool Options sets radial's
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

    // View: guides.
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
    connect(view_, &CanvasView::referenceFailed, this, [this] {
        statusBar()->showMessage(tr("A reference image can't be shown at this zoom"), 5000);
    });

    // Tools: one checkable item per tool in the registry.
    QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    auto* toolGroup = new QActionGroup(this);
    const QHash<QString, QKeySequence> toolKeys{
        {QStringLiteral("keyboard"), QKeySequence(Qt::CTRL | Qt::Key_K)},
        {QStringLiteral("select"), QKeySequence(Qt::CTRL | Qt::Key_E)},
        {QStringLiteral("move"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J)},
        {QStringLiteral("halfBlock"), QKeySequence(Qt::CTRL | Qt::Key_B)},
        {QStringLiteral("shade"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)},
        {QStringLiteral("rampShade"), QKeySequence(Qt::CTRL | Qt::Key_T)},
        {QStringLiteral("form"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)},
        {QStringLiteral("soft"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
        {QStringLiteral("despeckle"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)},
        {QStringLiteral("smudge"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M)},
        {QStringLiteral("glyph"), QKeySequence(Qt::CTRL | Qt::Key_G)},
        {QStringLiteral("colorize"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
        {QStringLiteral("line"), QKeySequence(Qt::CTRL | Qt::Key_L)},
        {QStringLiteral("rectangle"), QKeySequence(Qt::CTRL | Qt::Key_U)},
        {QStringLiteral("ellipse"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)},
        {QStringLiteral("fill"), QKeySequence(Qt::CTRL | Qt::Key_J)},
        {QStringLiteral("gradient"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G)},
        {QStringLiteral("eyedropper"), QKeySequence(Qt::CTRL | Qt::Key_I)},
        {QStringLiteral("outline"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)},
        {QStringLiteral("rimLight"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L)},
        {QStringLiteral("texture"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)},
        {QStringLiteral("clone"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K)},
        {QStringLiteral("spray"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)},
        {QStringLiteral("text"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X)}};
    QHash<Tool*, QAction*> toolActions;
    for (Tool* t : tools_->tools()) {
        const QKeySequence key = toolKeys.value(t->id());
        QAction* a = add(toolsMenu, t->name(), "", key, [this, t] { tools_->setActive(t); });
        a->setObjectName(t->id() + QStringLiteral("Tool"));
        a->setProperty(kToolIdProperty, t->id());  // for --list-actions (action_list.h)
        a->setIcon(toolIcon(t->id()));
        const QString name = QString(t->name()).remove(QLatin1Char('&'));
        const QString summary = help::toolSummary(t->id());
        // the name and key, then what the tool does (help_text.h); the first
        // line again when the key changes (Preferences > Shortcuts), keeping
        // the lines after it (the toolbox adds one to a group's tools), which
        // setToolTip's own changed() leaves alone, the tip being the same
        a->setToolTip(summary.isEmpty() ? name : name + QLatin1Char('\n') + summary);
        const auto setTip = [a, name] {
            const QKeySequence k = a->shortcut();
            QStringList lines = a->toolTip().split(QLatin1Char('\n'));
            lines[0] = k.isEmpty() ? name : tr("%1 (%2)").arg(name, k.toString(QKeySequence::NativeText));
            a->setToolTip(lines.join(QLatin1Char('\n')));
        };
        setTip();
        connect(a, &QAction::changed, a, setTip);
        a->setStatusTip(summary);
        // and all of it for What's This (whats_this.h)
        a->setWhatsThis(toolWhatsThis(t->id()));
        check(a, toolGroup, t == tools_->active());
        toolActions.insert(t, a);
        toolActions_.insert(t->id(), a);
    }
    connect(tools_, &ToolRegistry::activeChanged, this, [toolActions](Tool* t) {
        if (QAction* a = toolActions.value(t))
            a->setChecked(true);
    });
    // Rectangles and ellipses: outlines, or filled.
    toolsMenu->addSeparator();
    QAction* filled = check(add(toolsMenu, tr("Filled Sha&pes"), "filledShapes",
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), [] {}),
                            nullptr, false);
    connect(filled, &QAction::toggled, this, [fillable](bool on) {
        for (ShapeTool* t : fillable)
            t->setFilled(on);
    });
    // The brushes' size, a cell at a time (Tool Options has the rest).
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
    // the glyph brush's refusal of a Pixels layer reads the symmetry
    connect(view_, &CanvasView::symmetryChanged, this, updateToolStatus);
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
}

} // namespace bw
