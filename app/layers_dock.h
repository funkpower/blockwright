#pragma once
// The Layers dock (PLAN §5, §11): the document's layers, top first, one row
// each with its visibility and name, its alpha lock and its kind. The
// current row is the active layer; picking a row makes it active. A name is
// edited in place; the boxes toggle visibility and alpha lock. Under the
// list, New Cells Layer, New Pixels Layer, Delete, Raise and Lower, the same
// actions as the Layer menu. Every change but the active layer is an undo
// command on the document (Document::addLayer and the rest). Under those, the
// active generator layer's parameters (GeneratorForm). The Style column
// shows "fx" for a layer with a layer style (PLAN §15), its effects in the
// tooltip; a double-click there on a layer that can hold one asks for the
// Layer Style dialog (styleRequested).

#include "blockwright/layer.h"

#include <QWidget>

class QAction;
class QTreeWidget;
class QTreeWidgetItem;

namespace bw {

class Document;
class GeneratorForm;

class LayersDock : public QWidget {
    Q_OBJECT
public:
    enum Column { NameColumn, AlphaLockColumn, KindColumn, StyleColumn };

    explicit LayersDock(Document* doc, QWidget* parent = nullptr);

    QTreeWidget* tree() const { return tree_; }
    // The panel's actions, shared with the Layer menu.
    QAction* newCellsAction() const { return newCells_; }
    QAction* newPixelsAction() const { return newPixels_; }
    QAction* deleteAction() const { return delete_; }
    QAction* raiseAction() const { return raise_; }
    QAction* lowerAction() const { return lower_; }
    GeneratorForm* generatorForm() const { return form_; }

    // The layer a row shows; 0 for none.
    static LayerId layerOf(const QTreeWidgetItem* item);

signals:
    // A double-click on the Style cell of a layer that can hold a style.
    void styleRequested(LayerId id);

private:
    // Brings the rows up to the document: in place when the same layers are
    // in the same order (so a row being edited stays put), rebuilt when not,
    // later when that would be inside one of the tree's signals.
    void sync();
    void fill(QTreeWidgetItem* item, const Layer& layer);
    void updateActions();
    void itemChanged(QTreeWidgetItem* item, int column);

    Document* doc_;
    QTreeWidget* tree_;
    QAction* newCells_;
    QAction* newPixels_;
    QAction* delete_;
    QAction* raise_;
    QAction* lower_;
    int handling_ = 0;  // in a handler of the tree's signals
    bool syncQueued_ = false;
    GeneratorForm* form_;
};

} // namespace bw
