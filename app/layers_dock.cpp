#include "layers_dock.h"

#include "document.h"
#include "generator_form.h"
#include "help_text.h"
#include "item_check_style.h"
#include "row_tree.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QScopedValueRollback>
#include <QStyledItemDelegate>
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

// What the kind holds, which tools paint it, how it composites and exports.
QString kindToolTip(LayerKind kind) { return help::plainText(help::layerText(kind)); }

Qt::CheckState checkState(bool on) { return on ? Qt::Checked : Qt::Unchecked; }

// The Style column has nothing to edit in place: a double-click there opens
// the Layer Style dialog instead.
class NoEditor : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget*, const QStyleOptionViewItem&, const QModelIndex&) const override { return nullptr; }
};

bool canHoldStyle(const Layer& layer) {
    const LayerKind k = layer.props.kind;
    return k == LayerKind::Cells || k == LayerKind::Pixels || (k == LayerKind::Text && layer.props.text);
}

QString effectsLabel(const TextStyle& style) {
    QStringList on;
    for (const Effect& e : style.effects)
        if (e.enabled)
            on << QString::fromLatin1(effectKindName(e.kind()));
    return on.isEmpty() ? LayersDock::tr("every effect off") : on.join(QStringLiteral(", "));
}

} // namespace

LayersDock::LayersDock(Document* doc, QWidget* parent) : QWidget(parent), doc_(doc), tree_(new RowTree(6)) {
    tree_->setObjectName(QStringLiteral("layerList"));
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("Layer"), tr("Alpha Lock"), tr("Kind"), tr("Style")});
    tree_->setItemDelegateForColumn(StyleColumn, new NoEditor(tree_));
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    fixItemChecks(tree_);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(AlphaLockColumn, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(KindColumn, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(StyleColumn, QHeaderView::ResizeToContents);
    tree_->setToolTip(tr("The layers, top first. The current row is the layer the tools edit; "
                         "its box shows or hides it, double-click its name to rename it"));

    const auto action = [this](const QString& text, const char* name, const QString& tip, auto slot) {
        auto* a = new QAction(text, this);
        a->setObjectName(QString::fromLatin1(name));
        a->setToolTip(tip);
        a->setStatusTip(tip);  // New Cells and New Pixels get one line of theirs below
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
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        const LayerId id = layerOf(item);
        if (column != StyleColumn)
            return;
        if (const Layer* l = doc_->layers().find(id); l && canHoldStyle(*l))
            emit styleRequested(id);
    });
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
    item->setText(StyleColumn, p.style ? QStringLiteral("fx") : QString());
    item->setToolTip(StyleColumn, p.style ? tr("Layer style: %1; double-click to edit it").arg(effectsLabel(*p.style))
                                  : canHoldStyle(layer) ? tr("No layer style; double-click to add one")
                                                        : tr("This layer can't have a layer style"));
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
