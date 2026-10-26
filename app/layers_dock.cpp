#include "layers_dock.h"

#include "document.h"
#include "generator_form.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace bw {

namespace {

constexpr int kIdRole = Qt::UserRole;

QString kindLabel(LayerKind kind) {
    switch (kind) {
    case LayerKind::Cells: return LayersDock::tr("Cells");
    case LayerKind::Pixels: return LayersDock::tr("Pixels");
    case LayerKind::Text: return LayersDock::tr("Text");
    case LayerKind::Generator: return LayersDock::tr("Generator");
    case LayerKind::Reference: return LayersDock::tr("Reference");
    }
    return {};
}

QString kindToolTip(LayerKind kind) {
    switch (kind) {
    case LayerKind::Cells:
        return LayersDock::tr(
            "A Cells layer holds a glyph plus foreground and background colours in each cell.\n"
            "Use Keyboard (typing), Glyph Brush, shade and ramp brushes, detail brushes, or pixel tools.\n"
            "Opaque cells normally replace the cells below; transparent cells show through. "
            "Exports flatten the visible layers into character cells.");
    case LayerKind::Pixels:
        return LayersDock::tr(
            "A Pixels layer holds two half-block colours per cell (top and bottom), "
            "each either a palette colour or transparent.\n"
            "Paint with Half-Block Brush, Line, Rectangle, Ellipse or Fill. Other tools can only write "
            "space, █, ▀ or ▄; shade-producing tools and other glyphs are refused.\n"
            "Set halves cover what is below; transparent halves show through. Text layers stay in front of pixels. "
            "Exports flatten these pixels into half-block glyphs and colours in the visible composite.");
    case LayerKind::Text:
        return LayersDock::tr(
            "A Text layer holds text rendered as glyph cells with foreground and background colours.\n"
            "Edit live text with the Text tool and Tool Options; painting tools cannot change it. "
            "Without a live text source, the cells can be painted like a Cells layer.\n"
            "Opaque text cells normally replace cells below and cannot be covered by Pixels layers; "
            "transparent cells show through. Exports flatten the visible rendered text into character cells.");
    case LayerKind::Generator:
        return LayersDock::tr(
            "A Generator layer holds a program's cached output as glyph cells with foreground and background colours.\n"
            "Change parameters and Run below; painting tools cannot edit the output.\n"
            "The cached cells composite like a Cells layer and export in the visible composite. "
            "Opening or exporting does not run the generator.");
    case LayerKind::Reference:
        return LayersDock::tr(
            "Reference layers are reserved for a reference image.\n"
            "Painting tools cannot edit them. They are currently skipped by the compositor and never exported.");
    }
    return {};
}

Qt::CheckState checkState(bool on) { return on ? Qt::Checked : Qt::Unchecked; }

} // namespace

LayersDock::LayersDock(Document* doc, QWidget* parent) : QWidget(parent), doc_(doc), tree_(new QTreeWidget) {
    tree_->setObjectName(QStringLiteral("layerList"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Layer"), tr("Alpha Lock"), tr("Kind")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(AlphaLockColumn, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(KindColumn, QHeaderView::ResizeToContents);
    tree_->setToolTip(tr("The layers, top first. The current row is the layer the tools edit; "
                         "its box shows or hides it, double-click its name to rename it"));

    const auto action = [this](const QString& text, const char* name, const QString& tip, auto slot) {
        auto* a = new QAction(text, this);
        a->setObjectName(QString::fromLatin1(name));
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    newCells_ = action(tr("New &Cells Layer"), "newCellsLayer",
                       tr("Add a Cells layer above the active layer.\n%1").arg(kindToolTip(LayerKind::Cells)),
                       [this] { doc_->addLayer(LayerKind::Cells); });
    newPixels_ = action(tr("New &Pixels Layer"), "newPixelLayer",
                        tr("Add a Pixels layer above the active layer.\n%1").arg(kindToolTip(LayerKind::Pixels)),
                        [this] { doc_->addLayer(LayerKind::Pixels); });
    delete_ = action(tr("&Delete Layer"), "deleteLayer", tr("Delete the active layer"),
                     [this] { doc_->removeLayer(doc_->activeLayer()); });
    raise_ = action(tr("&Raise Layer"), "raiseLayer", tr("Move the active layer up one"), [this] {
        if (const auto i = doc_->layers().indexOf(doc_->activeLayer()))
            doc_->moveLayer(doc_->activeLayer(), *i + 1);
    });
    lower_ = action(tr("&Lower Layer"), "lowerLayer", tr("Move the active layer down one"), [this] {
        if (const auto i = doc_->layers().indexOf(doc_->activeLayer()); i && *i > 0)
            doc_->moveLayer(doc_->activeLayer(), *i - 1);
    });

    // The hint's half for each kind, in the status bar while the item is
    // under the pointer in the Layer menu.
    newCells_->setStatusTip(tr("Add a Cells layer above the active layer: glyphs and shades"));
    newPixels_->setStatusTip(tr("Add a Pixels layer above the active layer: half-block drawing"));

    // Short labels and arrows, so the dock stays narrow.
    newCells_->setIconText(tr("+ Cells"));
    newPixels_->setIconText(tr("+ Pixels"));
    delete_->setIconText(tr("Delete"));
    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    for (QAction* a : {newCells_, newPixels_, delete_, raise_, lower_}) {
        auto* b = new QToolButton;
        b->setDefaultAction(a);
        b->setObjectName(a->objectName() + QStringLiteral("Button"));
        if (a == raise_)
            b->setArrowType(Qt::UpArrow);
        else if (a == lower_)
            b->setArrowType(Qt::DownArrow);
        buttons->addWidget(b);
    }
    buttons->addStretch();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(tree_);
    auto* hint = new QLabel(tr("Cells: glyphs and shades. Pixels: half-block drawing."));
    hint->setObjectName(QStringLiteral("layerKindHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addLayout(buttons);
    form_ = new GeneratorForm(doc_);
    layout->addWidget(form_);

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        const QScopedValueRollback<int> guard(handling_, handling_ + 1);
        if (const LayerId id = layerOf(item); id && id != doc_->activeLayer())
            doc_->setActiveLayer(id);
    });
    connect(tree_, &QTreeWidget::itemChanged, this, &LayersDock::itemChanged);
    connect(doc_, &Document::layersChanged, this, &LayersDock::sync);
    connect(doc_, &Document::activeLayerChanged, this, &LayersDock::sync);
    sync();
}

LayerId LayersDock::layerOf(const QTreeWidgetItem* item) {
    return item ? item->data(NameColumn, kIdRole).value<LayerId>() : 0;
}

void LayersDock::sync() {
    const QSignalBlocker block(tree_);
    const LayerStack& stack = doc_->layers();
    const auto n = stack.size();
    bool same = tree_->topLevelItemCount() == int(n);
    for (std::size_t row = 0; same && row < n; ++row)
        same = layerOf(tree_->topLevelItem(int(row))) == stack[n - 1 - row].props.id;
    if (!same && handling_) {
        // a change from one of the tree's own signals took a layer away or
        // added one (the Text tool's empty layer, taken back before the
        // change): the rows are rebuilt once the signal's handler is done
        // with its row
        if (!syncQueued_) {
            syncQueued_ = true;
            QMetaObject::invokeMethod(
                this,
                [this] {
                    syncQueued_ = false;
                    sync();
                },
                Qt::QueuedConnection);
        }
        return;
    }
    if (!same) {
        tree_->clear();
        for (std::size_t row = 0; row < n; ++row) {
            auto* item = new QTreeWidgetItem(tree_);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        }
    }
    for (std::size_t row = 0; row < n; ++row) {
        QTreeWidgetItem* item = tree_->topLevelItem(int(row));
        fill(item, stack[n - 1 - row]);
        if (layerOf(item) == doc_->activeLayer())
            tree_->setCurrentItem(item);
    }
    updateActions();
}

void LayersDock::fill(QTreeWidgetItem* item, const Layer& layer) {
    const LayerProperties& p = layer.props;
    item->setData(NameColumn, kIdRole, QVariant::fromValue<LayerId>(p.id));
    item->setText(NameColumn, QString::fromStdString(p.name));
    item->setCheckState(NameColumn, checkState(p.visible));
    item->setCheckState(AlphaLockColumn, checkState(p.alphaLocked));
    item->setText(KindColumn, kindLabel(p.kind));
    item->setToolTip(KindColumn, kindToolTip(p.kind));
    item->setToolTip(NameColumn, tr("Shown or hidden; double-click to rename"));
    item->setToolTip(AlphaLockColumn, p.kind == LayerKind::Pixels
                                          ? tr("Alpha lock: the tools paint only the layer's opaque pixels")
                                          : tr("Alpha lock: the tools paint only the layer's opaque cells"));
}

void LayersDock::updateActions() {
    const LayerStack& stack = doc_->layers();
    const auto i = stack.indexOf(doc_->activeLayer());
    delete_->setEnabled(i && stack.size() > 1);
    raise_->setEnabled(i && *i + 1 < stack.size());
    lower_->setEnabled(i && *i > 0);
}

void LayersDock::itemChanged(QTreeWidgetItem* item, int column) {
    const QScopedValueRollback<int> guard(handling_, handling_ + 1);
    const LayerId id = layerOf(item);
    const Layer* layer = doc_->layers().find(id);
    if (!layer)
        return;
    if (column == NameColumn) {
        const bool visible = item->checkState(NameColumn) == Qt::Checked;
        if (visible != layer->props.visible)
            doc_->setLayerVisible(id, visible);
        else if (item->text(NameColumn) != QString::fromStdString(layer->props.name))
            doc_->renameLayer(id, item->text(NameColumn));
    } else if (column == AlphaLockColumn) {
        doc_->setLayerAlphaLocked(id, item->checkState(AlphaLockColumn) == Qt::Checked);
    }
    // A refused change (an empty name) puts the row back.
    if (const Layer* now = doc_->layers().find(id)) {
        const QSignalBlocker block(tree_);
        fill(item, *now);
    }
}

} // namespace bw
