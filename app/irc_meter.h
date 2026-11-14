#pragma once
// The IRC row meter (PLAN §8): a gutter right of the canvas giving every
// row's size as the ascii script sends the line the IRC encoder writes
// (irc::txtSentBytes), against the byte budget, and
// the scroll area that makes room for it. Rows over the budget show red;
// rows fit to the budget by merging colours, amber. The budget is the
// document's (Document::extras, saved with a project), the one the IRC View
// dock cuts at; the encoder options are the window's (View > IRC). File >
// Export IRC Text... writes the lines the meter measures. It measures what
// the canvas shows: the tool's edit in progress (a floating selection, a
// drag) over the document's cells, which Export lands before it writes.

#include "blockwright/edit.h"
#include "blockwright/irc.h"
#include "blockwright/project.h"

#include <QDialog>
#include <QScrollArea>
#include <QWidget>

#include <string>
#include <unordered_map>
#include <vector>

class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;

namespace bw {

class CanvasView;
class Document;

// A scroll area with room for a widget down its right edge, outside the
// viewport, so the canvas scrolls beside it.
class GutterScrollArea : public QScrollArea {
    Q_OBJECT
public:
    using QScrollArea::QScrollArea;

    // Puts `gutter` (or nothing) down the right edge, `width` pixels wide.
    void setGutter(QWidget* gutter, int width);
    QWidget* gutter() const { return gutter_; }

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void placeGutter();

    QWidget* gutter_ = nullptr;
    int width_ = 0;
};

class IrcMeter : public QWidget {
    Q_OBJECT
public:
    enum class RowState { Fits, Merged, Over };

    // The meter for `doc` drawn in `view`, in the gutter of `scroll`, which
    // holds the view.
    IrcMeter(Document* doc, CanvasView* view, GutterScrollArea* scroll);

    // The document's budget (Document::setIrcBudget changes it).
    const project::IrcBudget& budget() const;
    bool fit() const { return fit_; }
    void setFit(bool on);
    bool pad() const { return pad_; }
    void setPad(bool on);
    bool swapShades() const { return swapShades_; }
    void setSwapShades(bool on);
    // The encoder options: with fit on, fit to the budget. The IRC View dock
    // sends with these too.
    irc::EncodeOptions options() const;

    // Shows the meter in its gutter, or hides it and gives the room back.
    // Under the Micron target it stays hidden, whatever is asked, since a
    // page has no byte budget (PLAN §17), and shows again as asked when the
    // target changes back.
    void setShown(bool on);
    bool shownAsked() const { return shownAsked_; }

    // Row y's line as the encoder writes it now (bytes), its size as the
    // ascii script sends it (sentBytes: it writes the line again, with
    // two-digit colour codes, #364), the cells fit merged in it, and how it
    // stands against the budget, which the sent size decides. A row is
    // encoded when first asked for after it changed, and a row with the
    // same cells as one encoded before with the same options and budget
    // takes that row's line.
    std::size_t bytes(int y);
    std::size_t sentBytes(int y);
    int merged(int y);
    RowState state(int y);
    // Row y's band in the meter's own coordinates: the canvas row's top and
    // height wherever the view is scrolled and zoomed, the meter pixels
    // rowAt gives row y. Bands don't overlap: at a fractional device pixel
    // ratio a pixel two canvas rows share is the upper row's.
    QRect rowRect(int y) const;
    // The canvas row under meter pixel row y, as the canvas maps the pointer
    // to a cell (on its device grid); off the canvas, below 0 or past its
    // last row.
    int rowAt(int y) const;
    // What the tooltip over row y says.
    QString describe(int y);

    // The lines Export writes (irc::exportLines): every row as measured,
    // without the blank rows above and below the art. Export finishes the
    // tool first, so they are the cells as the document saves them (iCE as
    // set), with the options.
    std::vector<irc::Line> lines();

    QSize sizeHint() const override;

signals:
    // Fit, Pad or Swap Shades was turned on or off. (A new budget comes as
    // Document::ircBudgetChanged.)
    void optionsChanged();

protected:
    bool event(QEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void applyShown();
    bool shownAsked_ = true;
    // A row's line and its size as the ascii script sends it.
    struct Measured {
        irc::Line line;
        std::size_t sent = 0;
    };
    struct Row {
        Measured m;
        bool valid = false;
    };
    const Row& row(int y);
    // Encodes a row's cells and measures the line, or takes the line of the
    // same cells from the cache.
    Measured encoded(const std::vector<irc::MircCell>& cells);
    void resetRows();
    void invalidate(const QRegion& cells);
    void invalidateRow(int y);
    // The view's tool, or its edit in progress, changed.
    void toolChanged();
    void pendingChanged();
    // The first meter pixel row rowAt gives `row` or a row below it.
    int firstPixel(int row) const;

    Document* doc_;
    CanvasView* view_;
    GutterScrollArea* scroll_;
    bool fit_ = false;
    bool pad_ = false;
    bool swapShades_ = false;
    std::vector<Row> rows_;
    std::vector<CellChange> pending_;  // the tool's, on the canvas, by row
    QMetaObject::Connection toolConnection_;
    // Lines by their cells (glyph, colours and formatting), the options and
    // the budget.
    std::unordered_map<std::string, Measured> cache_;
};

// View > IRC > Budget...: a preset, or a byte count typed or worked out
// from a nick!user@host mask and a channel (irc::budgetFor).
class IrcBudgetDialog : public QDialog {
    Q_OBJECT
public:
    explicit IrcBudgetDialog(const project::IrcBudget& budget, QWidget* parent = nullptr);

    project::IrcBudget budget() const;

    QComboBox* presetBox() const { return preset_; }
    QSpinBox* bytesBox() const { return bytes_; }
    QLineEdit* maskEdit() const { return mask_; }
    QLineEdit* targetEdit() const { return target_; }

private:
    void presetPicked();
    void prefixEdited();

    QComboBox* preset_;
    QSpinBox* bytes_;
    QLineEdit* mask_;
    QLineEdit* target_;
};

} // namespace bw
