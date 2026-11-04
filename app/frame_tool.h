#pragma once
// The Frame tool (#345): drag a rectangle and a frame style (frame.h) is
// drawn to fit it, landing as one edit, one undo step, on release. A drag
// smaller than the style's minimum grows to it, away from the press. An
// empty middle is left as it is. The left button draws the style, in its
// own colours or recoloured to the pen, either way leaving the background
// under the cells the style says keep it (frame.h); the right button erases
// the cells the frame would cover. Like the other drawing tools it paints inside the
// selection, keeps the layer's alpha lock and repeats under mirror mode's
// symmetry, its glyphs mirrored for each image.

#include "draw_tools.h"

#include "blockwright/frame.h"

#include <QString>

namespace bw {

class FrameTool : public DragTool {
    Q_OBJECT
public:
    explicit FrameTool(QObject* parent = nullptr);

    // The style drawn, by id (bundledFrameStyles); an unknown id is ignored.
    const FrameStyle& style() const;
    QString styleId() const { return styleId_; }
    void setStyle(const QString& id);
    // Recolour the style to the pen (recolourFrame) rather than draw it in
    // its own colours.
    bool penColours() const { return penColours_; }
    void setPenColours(bool on);

    QString status() const override;

    // The rectangle a drag from `from` to `to` draws `style` in: the cells
    // between them, grown to the style's minimum away from `from`.
    static QRect frameRect(const FrameStyle& style, QPoint from, QPoint to);

protected:
    bool begin(const ToolEvent& e) override;
    bool extend(const ToolEvent& e) override;
    CellEdit edit() const override;
    void reset() override {}
    QString unheldGlyphs(Qt::MouseButton button) const override;

private:
    QString styleId_;
    bool penColours_ = false;
    QPoint from_, to_;
    Qt::MouseButton button_ = Qt::NoButton;
    Pen pen_;
};

} // namespace bw
