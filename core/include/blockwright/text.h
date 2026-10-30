#pragma once
// Live text layers (PLAN §15): a text layer whose cells are drawn from a
// source in its properties (text_source.h) through a face (face.h).
//
// Drawing: the text, in capitals if the source asks, is laid out by
// layoutText with the face's letter spacing plus the source's, wrapped at
// the wrap width. Lines are the face's height apart plus the line spacing
// (whole cell rows, so lines start on a cell). The text's box is as wide as
// the wrap width, or the widest line when that is wider or the text doesn't
// wrap, and each line takes its place in it by the alignment. The layer is
// the box's size; its offset is where the box's top-left cell sits. Each
// cell is drawn from its two pixels of the letter mask: both ink a █, one a
// ▀ or ▄, in the source's colour on black, and none a transparent cell. A
// face built from cells (TheDraw) gives its own source cells where it has
// them.
//
// Cells and source: a live text layer's cells are redrawn only when its
// source changes. A change that leaves the source as it is (a rename, a
// lock, visibility, mode, offset) keeps the cells, and so does opening a
// project: the cells are read as saved, which is what a 1.0 reader shows
// (PLAN §6, §15), even where an edited file's source would draw others.
//
// Undo: a change of the source, of any setting or of the face is one step
// that stores the layer's properties before and after (LayerPropsEdit).
// Every step that changes the source (giving one, changing it, Bake taking
// it away) also keeps the cells it replaced: undoing it puts them back as
// they were rather than redrawing the source before, so undo gives back the
// exact cells whatever drew them, and redoing it draws the source after. A
// step that leaves the source as it is keeps no cells and touches none,
// either way. A run of typing is one step: each keystroke's edit carries
// the run it belongs to, and mergeEdits folds an edit into the one before it
// in the same run. Bake (bakeEdit) is one step the same way, and undoing it
// gives back the live layer.
//
// Styles (text_style.h, PLAN §15): a live text layer's style is drawn with
// its text (renderStyledText): the layer is the text's box grown by the
// style's margins (styleMargins), the text at (left, top) in it, so giving a
// layer a style grows it right and down from its offset. Its cells are
// redrawn when the source or the style changes, and a step changing either
// keeps the cells it replaced. A style is drawn with the iCE colours the
// step is made with (`ice`, the document's setting); the cells don't follow
// a later change of the setting until the next redraw. A cells or pixels
// layer's style is drawn by the compositor (style_draw.h): a step changing
// it touches no cell, and Bake of such a layer keeps the cells before and
// after it (LayerPropsEdit::cellsAfter).

#include "blockwright/face.h"
#include "blockwright/font_store.h"
#include "blockwright/layer.h"
#include "blockwright/style_draw.h"
#include "blockwright/text_source.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bw {

// The face a reference names, or null when there is none: a pixel face
// from this build, a TheDraw font from `fonts` (the project's copies, then
// the fonts folder; font_store.h).
//
// Every call below that draws or checks a source takes the same `fonts`; a
// source in a TheDraw font that none of them holds can't be drawn.
const Face* findFace(const FaceRef& ref, const FontSources& fonts = {});

struct TextRender {
    Canvas cells{0, 0};
    // The characters the face can't draw (indexes into the text), laid out
    // blank (layoutText).
    std::vector<std::size_t> missing;
};

// The source drawn in `face` (see above). Throws TextError when its box is
// larger than a layer the project file holds (project.h's limits), found by
// measuring the text (measureText) before anything its size is allocated.
// The source's text in its case (Capitals turns a-z to A-Z), as it is laid
// out and drawn.
std::u32string casedText(const TextSource& source);

TextRender renderText(const TextSource& source, const Face& face);

// The source drawn in `face` with `style` (see above), with iCE colours
// `ice`. Throws TextError for a style that can't be drawn
// (textStyleProblem) or a box with the style's margins larger than a layer
// the project file holds, measured first as renderText does.
TextRender renderStyledText(const TextSource& source, const TextStyle& style, const Face& face, bool ice = true);

// The source laid out as its style reads it (style_draw.h), the box at
// (0, 0): what renderStyledText draws the style on, with the style's
// margins. Throws TextError as renderText does.
StyleInput textStyleInput(const TextSource& source, const Face& face);

// Thrown for a source that can't be drawn (textSourceProblem).
class TextError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Why the source can't be drawn (its face unknown, the text too long, a
// setting out of range, or its box larger than a layer the project file
// holds: project.h's limits), or nothing. A TheDraw font no source holds
// is named with its file's hash. The box is measured, not drawn,
// so a source far too large is refused without allocating its size.
std::optional<std::string> textSourceProblem(const TextSource& source, const FontSources& fonts = {});
// The same for a source with a style: the source's problem, the style's
// (textStyleProblem), or a box with the style's margins too large.
std::optional<std::string> styledTextProblem(const TextSource& source, const TextStyle& style,
                                             const FontSources& fonts = {});
// Why a layer can't have its properties' style: a style on a layer that
// isn't a cells or pixels layer or a live text layer (a text layer without
// a source, a generator, a reference), or the style's or the styled text's
// problem; nothing when it has no style.
std::optional<std::string> layerStyleProblem(const LayerProperties& props, const FontSources& fonts = {});

// A live text layer of `source` at (x, y), its cells drawn. Throws
// TextError when the source can't be drawn.
Layer makeTextLayer(TextSource source, std::string name = {}, int x = 0, int y = 0,
                    const FontSources& fonts = {});

// Gives the layer `props` (its id kept) and, when they hold a text source
// other than the layer's, or a style other than its with a source, redraws
// its cells from them (a style with iCE colours `ice`), the layer taking the
// drawn size; otherwise the cells are kept. True when it redrew them.
// Throws TextError for a source or style that can't be drawn, before the
// layer is touched, for a source on a layer that isn't a text layer and for
// a style layerStyleProblem refuses.
bool applyLayerProperties(Layer& layer, LayerProperties props, const FontSources& fonts = {}, bool ice = true);

// One undo step on a layer's properties: the layer's properties before and
// after. A text edit with a non-zero `typingRun` is a keystroke of that
// run of typing; the tool starts a new run when the cursor is moved or the
// tool is left, and another command in between ends the run as well (an
// undo stack only merges into its top step).
struct LayerPropsEdit {
    LayerId id = 0;
    LayerProperties before, after;
    std::uint64_t typingRun = 0;
    // The layer's cells before, kept when the step changes the source
    // (`before.text` isn't `after.text`) or a live text layer's style: undo
    // puts them back as they were.
    std::optional<Canvas> cellsBefore;
    // The layer's cells after, kept by Bake of a layer whose style the
    // compositor draws: redo puts them in place (and undo cellsBefore).
    std::optional<Canvas> cellsAfter;
    // The iCE colours a redraw draws the style with.
    bool ice = true;

    bool operator==(const LayerPropsEdit&) const = default;
};

// The edit giving the layer the properties `after` (its id kept);
// `typingRun` as above. `ice` is the document's setting: a live text
// layer's style is drawn with it, and redrawn with it on redo.
LayerPropsEdit propsEdit(const Layer& layer, LayerProperties after, std::uint64_t typingRun = 0, bool ice = true);
// The edit setting the layer's text source to `source` (the text, a setting
// or the face); `typingRun` as above.
LayerPropsEdit textEdit(const Layer& layer, TextSource source, std::uint64_t typingRun = 0, bool ice = true);
// The edit setting the layer's style (none: dropping it), one step.
LayerPropsEdit styleEdit(const Layer& layer, std::optional<TextStyle> style, bool ice = true);
// The edit baking the layer (bake()): its properties as a cells layer
// without a source or style; its cells as they are, or, for a layer whose
// style the compositor draws, its styled cells (with iCE colours `ice`) and,
// for a pixel layer, the normal mode it composited in. Undo gives back the
// properties as they were, the mode with them.
LayerPropsEdit bakeEdit(const Layer& layer, bool ice = true);

// Applies the edit's `after` (`revert`: its `before`) to the layer. A step
// that kept cells redraws the source after it, and its revert puts the kept
// cells back without drawing; any other step leaves the cells alone. True
// when the layer's cells were replaced. Throws as applyLayerProperties does,
// before the layer is touched.
bool applyEdit(Layer& layer, const LayerPropsEdit& edit, bool revert = false, const FontSources& fonts = {});
// The same on its layer in the stack, returning the canvas cells to
// recomposite: the layer's bounds before and after, united clipped to the
// canvas. Throws LayerError when the stack doesn't hold the layer.
CellRect applyEdit(LayerStack& stack, const LayerPropsEdit& edit, bool revert = false,
                   const FontSources& fonts = {});

// Folds `next` into `into` when both are keystrokes of the same run of
// typing on the same layer, each leaving it a live text layer, and `next`
// starts where `into` ends: `into` then goes from its own before (and the
// cells kept with it, or `next`'s when it kept none) to `next`'s after.
// False, and `into` left as it is, otherwise.
bool mergeEdits(LayerPropsEdit& into, const LayerPropsEdit& next);

} // namespace bw
