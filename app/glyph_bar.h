#pragma once
// The glyph bar (PLAN §11): the current glyph set's ten glyphs, F1 to F10,
// and a list of the sixteen sets. A click on a glyph does what its F-key does
// in the canvas: types it at the cursor, or with the glyph brush picks it.

#include <QWidget>

class QComboBox;
class QToolButton;

namespace bw {

class CanvasView;

class GlyphBar : public QWidget {
    Q_OBJECT
public:
    explicit GlyphBar(CanvasView* view, QWidget* parent = nullptr);

    // The cell for F(key + 1), key 0-9.
    QToolButton* cell(int key) const { return cells_.value(key); }
    QComboBox* setBox() const { return set_; }

private:
    void sync();

    CanvasView* view_;
    QList<QToolButton*> cells_;
    QComboBox* set_;
};

} // namespace bw
