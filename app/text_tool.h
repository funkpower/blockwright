#pragma once
// The Text tool (PLAN §15): a click on a live text layer edits it, a click
// anywhere else puts a text cursor at the cell and the first character
// typed there makes a new text layer, and typing, Backspace, Delete, Enter
// and the arrows edit its text at the text cursor; Escape, another tool or
// another active layer ends the edit, and so does the layer being locked,
// hidden or baked, or a new document. A layer the edit made that is empty
// again goes when the edit ends, and before any other document command is
// pushed (Document::commandAboutToPush), so its steps can still be taken
// back. Each keystroke sets the layer's
// source (Document::setTextSource), and a run of typing, until the cursor
// is moved, the undo stack moves another way (undo, redo, another command,
// a save) or the edit ends, is one undo step. The settings a new layer
// starts with are the last set in Tool Options, which also sets them on the
// active text layer, one undo step each.

#include "tool.h"

#include "blockwright/font_store.h"
#include "blockwright/layer.h"
#include "blockwright/text_source.h"

#include <QList>
#include <QPoint>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace bw {

class Document;

class TextTool : public Tool {
    Q_OBJECT
public:
    explicit TextTool(QObject* parent = nullptr);

    // What a new text layer starts with (its text empty).
    const TextSource& defaults() const { return defaults_; }
    // The file of the defaults' TheDraw face, kept from where it was picked,
    // so text can be set in it after the fonts folder or the project that
    // held it is gone (Document::addTextLayer, Document::setTextSource). A
    // file kept for a face picked before goes in a queued call after the
    // pick, keptChanged() telling the face picker first, so its entry can be
    // picked again until then.
    const FontStore& keptFonts() const { return kept_; }
    // Takes `s`'s settings (all but its text) as the defaults and, when the
    // document's active layer is a live text layer that isn't locked and no
    // text cursor waits for a new layer (spot()), gives it them as one undo
    // step named `undoText`.
    void setSettings(const TextSource& s, const QString& undoText, Document* doc);

    // The layer being edited (0 for none) and the text cursor in it, an
    // index into its text.
    LayerId editing() const { return editing_; }
    int caret() const { return caret_; }
    // Where a click off every text layer put the text cursor: the first
    // character typed makes a new text layer there. Unset while a layer is
    // being edited.
    std::optional<QPoint> spot() const { return spot_; }
    // The canvas cell the text cursor is drawn at, the top of its line: at
    // a wrap, the end of the line End went to or the start of the next.
    QPoint caretCell() const;
    // Ends the edit; a layer the edit made that is empty again goes, and so
    // do its undo steps, with nothing left to redo.
    void endEdit();
    // A new text layer of `text` in the defaults' settings, with `style`,
    // its top-left cell at `cell`, as one undo step (Document::addTextLayer),
    // and the text cursor at the end of its text, as if it had been typed
    // there: a text style dropped from the Library. The tool must be active.
    // 0, and nothing made, when the styled text can't be drawn.
    LayerId placeStyled(QPoint cell, const std::u32string& text, const TextStyle& style);

    void activate(CanvasView* view) override;
    void deactivate() override;
    bool mousePress(const ToolEvent& e) override;
    bool mouseRelease(const ToolEvent& e) override;
    bool keyPress(QKeyEvent* e) override;
    bool inputText(const QString& text, int replaceStart = 0, int replaceLength = 0) override;
    // The text cursor's cells, or with no edit and no text cursor waiting
    // the end of the active text layer a key would edit (resume()).
    std::optional<QRect> caretRect() const override;
    // The text cursor of an edit or of a click waiting for a new layer.
    std::optional<QRect> drawnCaret() const override;
    void cancel() override;
    void paint(QPainter& p) const override;
    QString status() const override;

signals:
    // spot() was set or unset.
    void spotChanged();
    // keptFonts() dropped files: the faces listed from them, held until the
    // signal returns, go now.
    void keptChanged();

private:
    Document* doc() const;
    // Where the document finds a TheDraw face (Document::fontSources), and
    // keptFonts().
    FontSources fonts() const;
    // Drops the kept files but the defaults' face's.
    void trimKept();
    const Layer* edited() const;
    // The active layer when nothing is being edited, no text cursor waits
    // and it is a live text layer that can be edited, else null.
    const Layer* resumable() const;
    // With nothing being edited and no text cursor waiting, starts editing
    // resumable() from its end; false if there is none.
    bool resume();
    // The source the text cursor is in and the top-left cell of its
    // letters: the edited layer's (inside its style's margins), or the
    // settings a new layer starts with at spot(); null for neither.
    const TextSource* shown(QPoint* origin) const;
    // The cells of a text cursor before character `at` of `s`, its letters'
    // top-left cell at `origin`; unset when the face is missing.
    std::optional<QRect> caretCellsAt(const TextSource& s, QPoint origin, int at, bool after) const;
    void setSpot(std::optional<QPoint> spot);
    // Starts editing `id` with the cursor at `caret`, a new run of typing.
    void begin(LayerId id, int caret, bool after = false);
    // Ends the edit and leaves its layer as it is.
    void stop();
    // Takes back `id`, an empty layer an edit made (Document::dropNewTextLayer),
    // keeping the active layer if it is another.
    void drop(LayerId id);
    // Runs `change`, one of the tool's own changes to the document.
    void own(const std::function<void()>& change);
    // The document's undo stack moved, its layers changed, or it was
    // replaced.
    void undoChanged();
    void layersChanged();
    void documentReset();
    // CanvasView::layerShown: an Alt+drag shows layer `id` somewhere else.
    void layerShown(LayerId id);
    // Document::setActiveLayer is about to make `id` active.
    void activeAboutToChange(LayerId id);
    // A document command is about to be pushed: an empty layer the edit
    // made goes first.
    void commandAboutToPush();
    // The topmost visible, unlocked live text layer whose box holds the
    // canvas cell, or 0.
    LayerId textLayerAt(QPoint cell) const;
    // Replaces `count` characters at `at` with `text`; one keystroke of the
    // current run. At spot() it first makes the layer, if `text` isn't
    // empty. False if the source wouldn't take it.
    bool replace(int at, int count, const std::u32string& text);
    void moveCaret(int to, bool after = false);
    void newRun() { run_ = ++runs_; }

    TextSource defaults_;
    FontStore kept_;
    bool trimQueued_ = false;
    LayerId editing_ = 0;
    LayerId made_ = 0;  // the layer the edit made
    std::optional<QPoint> spot_;
    int caret_ = 0;
    // At a wrap the end of one line is the start of the next: set, the
    // cursor is at the end of the first.
    bool after_ = false;
    bool own_ = false;  // in one of the tool's own changes
    std::uint64_t run_ = 0, runs_ = 0;
    QList<QMetaObject::Connection> connections_;
};

} // namespace bw
