#include "replay.h"

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/brushes/smudge.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/canvas_size.h"
#include "blockwright/composite.h"
#include "blockwright/edit.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/samples.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"
#include "blockwright/target.h"
#include "blockwright/width.h"
#include "blockwright/xbin.h"
#include "loss.h"
#include "read_file.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <ostream>

namespace bwcli::replay {

namespace fs = std::filesystem;
using namespace bw;
using steps::Button;
using steps::Point;
using steps::Step;

// The longest named ramp's top rung.
int longestTopRung() {
    int top = 0;
    for (const Ramp& r : namedRamps())
        top = std::max(top, r.topRung());
    return top;
}

void take(ToolSettings& s, const steps::Options& o) {
    if (o.fg)
        s.pen.fg = Color::indexed(*o.fg);
    if (o.bg)
        s.pen.bg = Color::indexed(*o.bg);
    if (o.size)
        s.brush.size = *o.size;
    if (o.tip)
        s.brush.tip = *o.tip;
    if (o.strength)
        s.brush.strength = *o.strength;
    if (o.pressure)
        s.brush.pressure = *o.pressure;
    if (o.light)
        s.brush.lightDirection = *o.light;
    if (o.ramp)
        s.ramp = *o.ramp;
    if (o.symmetry)
        s.symmetry = *o.symmetry;
    if (o.form)
        s.form = *o.form;
    if (o.hardness)
        s.hardness = *o.hardness;
    if (o.outline)
        s.outline = *o.outline;
    if (o.keepLint)
        s.keepLint = *o.keepLint;
    if (o.glyph)
        s.glyph = *o.glyph;
    if (o.filled)
        s.filled = *o.filled;
    if (o.pencil)
        s.pencil = *o.pencil;
    if (o.gradientShape)
        s.gradientShape = *o.gradientShape;
    if (o.gradientBanding)
        s.gradientBanding = *o.gradientBanding;
    if (o.gradientFrom)
        s.gradientFrom = *o.gradientFrom;
    if (o.gradientTo)
        s.gradientTo = *o.gradientTo;
    if (o.texture)
        s.texture = *o.texture;
    if (o.spray)
        s.spray = *o.spray;
    if (o.pasteMode)
        s.pasteMode = *o.pasteMode;
    if (o.sampleMerged)
        s.sampleMerged = *o.sampleMerged;
}

namespace {

// What the editor holds between steps: its document, and Tool Options.
struct State : ToolSettings {
    project::Project p;
    LayerId active = 0;
    Selection selection;
    std::optional<CellBlock> clipboard;
    steps::View view;  // what the view steps have set (#398)

    LayerStack& stack() { return p.layers; }
    int width() const { return p.layers.width(); }
    int height() const { return p.layers.height(); }
    Layer& layer() { return *p.layers.find(active); }
    std::size_t index() const { return *p.layers.indexOf(active); }
};

[[noreturn]] void fail(const Step& step, const std::string& why) { throw Failure{steps::stepLabel(step) + ": " + why}; }

std::string at(const Point& p) { return "(" + std::to_string(p.x) + ", " + std::to_string(p.y) + ")"; }

// Every point on the canvas: cells, or with `pixels` half-block pixels (two
// rows to a cell row).
void onCanvas(const State& s, const Step& step, bool pixels) {
    const int h = pixels ? 2 * s.height() : s.height();
    for (const Point& p : step.points)
        if (p.x < 0 || p.y < 0 || p.x >= s.width() || p.y >= h)
            fail(step, (pixels ? "pixel " : "cell ") + at(p) + " is off the " + std::to_string(s.width()) + " x " +
                           std::to_string(h) + (pixels ? " pixel canvas" : " canvas"));
}

// Mirror mode as the editor's view keeps it: on whenever a symmetry is.
bool mirror(const State& s) { return s.symmetry.mode != SymmetryMode::None; }

// The blank a cleared cell becomes (Document::blankCell).
Cell blankCell(State& s) {
    if (s.layer().props.kind != LayerKind::Pixels && s.index() == 0)
        return Cell{};
    return transparentCell();
}

PastePixels pastePixels(State& s) { return {s.layer().props.kind == LayerKind::Pixels, s.p.ice}; }

// What a tool reads: the active layer's cells as a canvas.
Canvas canvasOf(State& s) { return layerCanvas(s.layer(), s.width(), s.height()); }

// The options a tool takes when the drag begins (DragTool::takeSettings).
StrokeOptions optionsOf(State& s) {
    StrokeOptions o = layerStrokeOptions(s.layer());
    o.selection = selectionMask(s.selection, s.width(), s.height());
    o.ice = s.p.ice;
    o.symmetry = s.symmetry;
    return o;
}

const char* kindName(LayerKind k) {
    switch (k) {
    case LayerKind::Cells: return "cells";
    case LayerKind::Pixels: return "pixels";
    case LayerKind::Text: return "text";
    case LayerKind::Generator: return "generator";
    case LayerKind::Reference: return "reference";
    }
    return "?";
}

// Whether the active layer takes edits (Document::editable), or why not.
void editable(State& s, const Step& step) {
    const Layer& l = s.layer();
    const std::string name = "the active layer \"" + l.props.name + "\"";
    if (!l.props.visible)
        fail(step, name + " is hidden");
    if (l.props.locked)
        fail(step, name + " is locked");
    if (l.props.kind == LayerKind::Generator || l.props.kind == LayerKind::Reference ||
        (l.props.kind == LayerKind::Text && l.props.text))
        fail(step, name + " is a " + kindName(l.props.kind) + " layer, which no tool edits");
}

// A Pixels layer holds half blocks and blanks: the tools that paint shades,
// and the glyph brush or the stars spray with a glyph it can't hold, refuse
// one before they paint (DragTool::refusesLayer).
void pixelsHold(State& s, const Step& st) {
    if (s.layer().props.kind != LayerKind::Pixels)
        return;
    const std::string& a = st.action;
    const auto held = [](char32_t ch) {
        Cell c;
        c.ch = ch;
        return halfBlocks(c).has_value();
    };
    std::string what;
    if (a == "spray")
        what = s.spray.kind == SprayKind::Stars ? "stars" : "shades";
    else if (a == "shade" || a == "rampShade" || a == "form" || a == "soft" || a == "smudge" || a == "gradient" ||
             a == "rimLight" || a == "texture")
        what = "shades";
    else if (a == "glyph" && st.button == Button::Left) {
        const int w = s.width(), h = s.height();
        for (const Dab& d : symmetryDabs(s.symmetry, Dab{w / 2, h / 2}, w, h))
            if (!held(dabGlyph(s.glyph, d)))
                what = "glyph but half blocks";
    }
    if (!what.empty())
        fail(st, "the active layer \"" + s.layer().props.name + "\" is a pixels layer, which holds no " + what);
}

// A tool's canvas edit made to the active layer (Document::edit), mirrored
// first unless the tool did that itself (CanvasView::applyEdit). Returns
// whether it changed anything.
bool land(State& s, const Step& step, CellEdit edit, bool mirrored, Outcome& out) {
    editable(s, step);
    if (!mirrored && s.symmetry.mode == SymmetryMode::MirrorX)
        edit = mirrorEdit(canvasOf(s), edit);
    const std::size_t index = s.index();
    const CompositeOptions opts{s.p.ice, Cell{}};
    const auto below = [&](int x, int y) { return compositeCellBelow(s.stack(), index, x, y, opts); };
    Layer& layer = s.layer();
    std::optional<CellEdit> e = fitLayerEdit(layer, edit, below, s.p.ice);
    if (!e)
        fail(step, "the active layer \"" + layer.props.name + "\" (" + kindName(layer.props.kind) +
                       ") can't hold what " + step.action + " writes" +
                       (layer.props.kind == LayerKind::Pixels ? ": a pixels layer holds only half-block pixels" : ""));
    *e = settleMarks(layer.cells, settleWide(layer.cells, std::move(*e)));
    if (e->changes.empty()) {
        out.notes.push_back(steps::stepLabel(step) + ": changes nothing");
        return false;
    }
    e->apply(layer.cells);
    return true;
}

std::unique_ptr<Brush> brushFor(State& s, const Step& step) {
    const bool right = step.button == Button::Right;
    const ShadeDirection dir = right ? ShadeDirection::Darken : ShadeDirection::Lighten;
    const std::string& a = step.action;
    if (a == "shade")
        return std::make_unique<ShadeBrush>(s.pen, right);
    if (a == "rampShade")
        return std::make_unique<RampShadeBrush>(s.ramp, dir, kRungsPerStep);
    if (a == "form")
        return std::make_unique<FormShadeBrush>(s.ramp, s.form);
    if (a == "soft")
        return std::make_unique<SoftShadeBrush>(s.ramp, dir, s.hardness);
    if (a == "despeckle")
        return std::make_unique<DespeckleBrush>(s.ramp);
    if (a == "smudge")
        return std::make_unique<SmudgeBrush>(s.ramp);
    if (a == "glyph") {
        if (right)
            return std::make_unique<GlyphBrush>(blankCell(s));
        Cell c;
        c.ch = s.glyph;
        c.fg = s.pen.fg;
        c.bg = s.pen.bg;
        // the pen's formatting (an IRC eyedropper pick's), as GlyphTool::makeBrush
        if (s.p.target == Target::Irc && s.layer().props.kind != LayerKind::Pixels)
            c.attrs = s.pen.attrs;
        return std::make_unique<GlyphBrush>(c);
    }
    if (a == "colorize")
        return right ? std::make_unique<ColorizeBrush>(std::nullopt, s.pen.bg)
                     : std::make_unique<ColorizeBrush>(s.pen.fg, std::nullopt);
    if (a == "outline")
        return std::make_unique<OutlineBrush>(s.ramp, s.outline);
    if (a == "rimLight")
        return std::make_unique<RimLightBrush>(s.ramp, s.brush.lightDirection, s.keepLint);
    if (a == "texture")
        return std::make_unique<TextureBrush>(s.ramp, s.texture, s.brush.lightDirection);
    if (a == "spray")
        return std::make_unique<SprayBrush>(s.ramp, s.spray, s.pen.fg, dir);
    return nullptr;
}

std::uint8_t colourFor(const State& s, const Step& step) {
    return (step.button == Button::Right ? s.pen.bg : s.pen.fg).paletteIndex();
}

// What the canvas shows: every visible layer composited (Document::canvas),
// what Sample merged reads.
Canvas compositeOf(State& s) { return composite(s.stack(), CompositeOptions{s.p.ice, Cell{}}); }

// The eyedropper (EyedropperTool::mousePress): on the active layer alone
// (the editor's default) nothing from a transparent cell or pixel; with
// Sample merged, what the canvas shows. Under the IRC target a left pick
// also takes the cell's formatting into the pen.
void eyedropper(State& s, const Step& step, Outcome& out) {
    const Point p = step.points[0];
    const Canvas cv = s.sampleMerged ? compositeOf(s) : canvasOf(s);
    const Cell& cell = cv.at(p.x, p.y / 2);
    const bool bg = step.button == Button::Right;
    std::optional<Color> c;
    if (s.sampleMerged) {
        c = pickColour(cv, p.x, p.y, bg, s.p.ice);
    } else if (s.layer().props.kind == LayerKind::Pixels) {
        const LayerPixels px = layerPixels(cell);
        if (const std::optional<std::uint8_t> half = p.y % 2 ? px.bottom : px.top)
            c = Color::indexed(*half);
    } else if (!cell.transparent) {
        c = pickColour(cv, p.x, p.y, bg, s.p.ice);
    }
    if (!c) {
        out.notes.push_back(steps::stepLabel(step) + ": pixel " + at(p) + " is transparent; the pen is unchanged");
        return;
    }
    (bg ? s.pen.bg : s.pen.fg) = *c;
    if (!bg && s.p.target == Target::Irc)
        s.pen.attrs = cell.attrs & attr::Formatting;
}

// "U+2605", or "U+0071 U+0302" for a cluster.
std::string codePoints(std::u32string_view cluster) {
    std::string s;
    for (const char32_t cp : cluster) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "%sU+%04X", s.empty() ? "" : " ", unsigned(cp));
        s += hex;
    }
    return s;
}

// Typed as keyboard mode types it (CanvasView::typeText), each line as one
// commit under the document's typing policy, so the IRC target or a UTF-8
// .ans takes any cluster a UTF-8 .ans holds, wide ones as a pair, and ANSI
// with a CP437 .ans a CP437 glyph (PLAN §16, Cursor and typing). A line of
// marks alone joins the cluster before the cursor (typeMarks); any other
// line is written by replaceText, so a line that starts with a mark and has
// more after it is refused, as the editor refuses it.
void keyboard(State& s, const Step& step, Outcome& out) {
    const TypingOptions opts{typingPolicy(s.p.target, s.p.ansEncoding)};
    const auto refuse = [&](std::u32string_view cluster) {
        fail(step, "the text holds a character keyboard mode can't type (" + codePoints(cluster) +
                       "; target: " + (s.p.target == Target::Irc ? "IRC" : "ANSI") + ", " +
                       (s.p.ansEncoding == TextEncoding::Utf8 ? "UTF-8" : "CP437") + ")");
    };
    const auto isMark = [](char32_t cp) { return codePointWidth(cp) == 0 && cp >= 0x20 && cp != 0x7f; };
    Cursor c{step.points[0].x, step.points[0].y, false};
    bool changed = false;
    std::u32string_view text = step.text;
    for (;;) {
        const std::size_t end = text.find(U'\n');
        const std::u32string_view line = text.substr(0, end);
        const std::vector<std::u32string> clusters = splitClusters(line);
        if (!clusters.empty()) {
            EditResult r;
            if (std::all_of(line.begin(), line.end(), isMark)) {
                // judged once joined
                r = typeMarks(canvasOf(s), c, line, opts);
            } else {
                // a lone mark is judged once joined (typeMarks)
                for (const std::u32string& k : clusters)
                    if (!isMark(k[0]) && !isTypeable(k, opts.policy))
                        refuse(k);
                if (isMark(clusters.front()[0]))
                    fail(step, "the text starts with a mark (" + codePoints(clusters.front()) +
                                   ") and has more after it on its line; keyboard mode joins marks only typed alone");
                r = replaceText(canvasOf(s), c, 0, 0, line, s.pen, blankCell(s), opts);
            }
            if (!r.rejected.empty())
                refuse(r.rejected);
            if (!r.notice.empty())
                out.notes.push_back(steps::stepLabel(step) + ": " + r.notice);
            Outcome quiet;
            changed = land(s, step, r.edit, false, quiet) || changed;
            c = r.cursor;
        }
        if (end == std::u32string_view::npos)
            break;
        text.remove_prefix(end + 1);
        if (c.y + 1 >= s.height())
            fail(step, "the text runs off the bottom of the canvas");
        c = Cursor{step.points[0].x, c.y + 1, false};
    }
    if (!changed)
        out.notes.push_back(steps::stepLabel(step) + ": changes nothing");
}

void needSelection(const State& s, const Step& step) {
    if (s.selection.empty())
        fail(step, step.action + " needs a selection: select or selectAll first");
}

void transform(State& s, const Step& step, Transform t, Outcome& out) {
    needSelection(s, step);
    const Canvas cv = canvasOf(s);
    if ((t == Transform::RotateClockwise || t == Transform::RotateCounterClockwise) &&
        selectionHoldsPair(cv, s.selection))
        fail(step, "can't rotate a selection that holds a wide character");
    const SelectionEdit r = transformSelection(cv, s.selection, t, blankCell(s));
    if (land(s, step, r.edit, false, out))
        s.selection = r.selection;
}

CellBlock copied(State& s) {
    CellBlock b = copyBlock(canvasOf(s), s.selection);
    b.pixelLayer = pastePixels(s).pixelLayer;
    return b;
}

// The selection lifted, moved by (dx, dy) and stamped, as a drag of it with
// the Select tool lands (SelectTool::stamp).
void moveSelected(State& s, const Step& step, Outcome& out) {
    needSelection(s, step);
    const CellBlock block = copied(s);
    const CellRect from = s.selection.bounds();
    const int x = from.x + step.dx, y = from.y + step.dy;
    const CellEdit e = stampBlock(canvasOf(s), block, x, y, s.pasteMode, &s.selection, blankCell(s), pastePixels(s));
    if (land(s, step, e, false, out))
        s.selection = Selection(CellRect{x, y, block.width, block.height}).clipped(canvasOf(s));
}

void paste(State& s, const Step& step, Outcome& out) {
    if (!s.clipboard)
        fail(step, "nothing to paste: copy or cut first");
    const Point p = step.points[0];
    const CellEdit e = stampBlock(canvasOf(s), *s.clipboard, p.x, p.y, s.pasteMode, nullptr, blankCell(s), pastePixels(s));
    if (land(s, step, e, false, out))
        s.selection = Selection(CellRect{p.x, p.y, s.clipboard->width, s.clipboard->height}).clipped(canvasOf(s));
}

Layer* layerNamed(State& s, const Step& step, const std::string& name) {
    Layer* found = nullptr;
    for (std::size_t i = 0; i < s.stack().size(); ++i)
        if (s.stack()[i].props.name == name) {
            if (found)
                fail(step, "two layers are named \"" + name + "\"");
            found = &s.stack()[i];
        }
    if (!found)
        fail(step, "no layer is named \"" + name + "\"");
    return found;
}

std::string freeLayerName(State& s) {
    for (int n = 1;; ++n) {
        const std::string name = "Layer " + std::to_string(n);
        const auto& ls = s.stack().layers();
        if (std::none_of(ls.begin(), ls.end(), [&](const Layer& l) { return l.props.name == name; }))
            return name;
    }
}

// A radial centre on the canvas: the editor fits a set centre onto it
// (CanvasView::fitSymmetryCentre), so one off it is a picture no drag makes.
void centreOnCanvas(const State& s, const Step& step) {
    const auto& c = step.options.symmetry ? step.options.symmetry->centre : std::nullopt;
    if (c && (c->first < 0 || c->second < 0 || c->first >= s.width() || c->second >= s.height()))
        fail(step, "symmetry centre " + at(Point{c->first, c->second}) + " is off the " + std::to_string(s.width()) +
                       " x " + std::to_string(s.height()) + " canvas");
}

// A layer move as the Move tool's drag lands it (CanvasView::beginLayerMove,
// endLayerMove): refused on a locked or hidden layer, and on a reference
// layer the picture moved by canvas pixels rather than the cells.
void moveLayer(State& s, const Step& st) {
    Layer& l = s.layer();
    const std::string name = "the active layer \"" + l.props.name + "\"";
    if (l.props.locked)
        fail(st, name + " is locked");
    if (!l.props.visible)
        fail(st, name + " is hidden");
    if (l.props.kind != LayerKind::Reference) {
        l.props.x += st.dx;
        l.props.y += st.dy;
        return;
    }
    if (!l.props.reference)
        fail(st, name + " is a reference layer with no picture");
    const auto place = [](long long v) {
        return int(std::clamp<long long>(v, -ReferenceImage::kMaxPlace, ReferenceImage::kMaxPlace));
    };
    l.props.reference->x = place((long long)l.props.reference->x + st.dx);
    l.props.reference->y = place((long long)l.props.reference->y + st.dy);
}

// Format > Canvas Size... as the dialog applies it (Document::resizeCanvas):
// a size canvasSizeProblem refuses stops the replay, the canvas's own size
// changes nothing, and any other is every layer through resizeCanvas, with
// the selection moved and cut with the cells, and a set symmetry centre
// pulled onto the canvas. The active layer and the clipboard stay as they
// are.
void canvasSize(State& s, const Step& st, Outcome& out) {
    const int w = st.canvasWidth, h = st.canvasHeight;
    if (const auto problem = canvasSizeProblem(s.stack(), w, h))
        fail(st, "the canvas can't be made " + std::to_string(w) + " x " + std::to_string(h) + ": " + *problem);
    if (w == s.width() && h == s.height()) {
        // the dialog's OK at the same size does nothing either
        out.notes.push_back(steps::stepLabel(st) + ": changes nothing");
        return;
    }
    const CellShift shift = anchorShift(s.width(), s.height(), w, h, st.anchor);
    s.selection = resizedSelection(s.selection, shift, w, h);
    s.p.layers = resizeCanvas(s.p.layers, w, h, shift, canvasCellWidth(s.p.sauce));
    // a set symmetry centre stays on the canvas, as the view keeps it
    // (CanvasView::fitSymmetryCentre)
    if (s.symmetry.centre)
        s.symmetry.centre = std::pair{std::clamp(s.symmetry.centre->first, 0, w - 1),
                                      std::clamp(s.symmetry.centre->second, 0, h - 1)};
}

void step(State& s, const Step& st, Outcome& out) {
    centreOnCanvas(s, st);
    take(s, st.options);
    const std::string& a = st.action;
    // a tool step with no points only sets Tool Options, as picking the tool
    if (st.points.empty() && (brushFor(s, st) || steps::pixelAction(a) || a == "gradient" || a == "keyboard"))
        return;

    if (auto brush = brushFor(s, st)) {
        onCanvas(s, st, false);
        editable(s, st);
        pixelsHold(s, st);
        // each point's pressure scales what the "pressure" option says, as
        // a stylus's does in the editor (Stroke::stamp); a point the
        // editor's tool would drop (a move to the last dab's cell that
        // doesn't press harder with pressure on) is dropped by the same
        // rule, takesDab (BrushTool::extend)
        std::vector<Dab> dabs;
        for (const Point& p : st.points) {
            const Dab d{p.x, p.y, p.pressure};
            if (dabs.empty() || takesDab(dabs.back(), d, s.brush.pressure))
                dabs.push_back(d);
        }
        land(s, st, paintStroke(canvasOf(s), *brush, s.brush, dabs, optionsOf(s)), true, out);
    } else if (a == "halfBlock") {
        onCanvas(s, st, true);
        editable(s, st);
        // the stroke keeps a reference to the canvas: it must outlive it
        const Canvas cv = canvasOf(s);
        HalfBlockStroke stroke(cv, colourFor(s, st), optionsOf(s), false, s.pencil);
        for (const Point& p : st.points)
            stroke.addPoint(p.x, p.y);
        land(s, st, stroke.edit(), true, out);
    } else if (a == "line" || a == "rectangle" || a == "ellipse") {
        onCanvas(s, st, true);
        editable(s, st);
        const StrokeOptions o = optionsOf(s);
        const PixelRect clip = symmetryReach(o.symmetry, s.width(), s.height());
        const Point f = st.points[0], t = st.points[1];
        const Pixels px = a == "line"        ? lineShape(f.x, f.y, t.x, t.y, clip)
                          : a == "rectangle" ? rectShape(f.x, f.y, t.x, t.y, s.filled, clip)
                                             : ellipseShape(f.x, f.y, t.x, t.y, s.filled, clip);
        land(s, st, plotPixels(canvasOf(s), px, colourFor(s, st), o, mirror(s)), true, out);
    } else if (a == "fill") {
        onCanvas(s, st, true);
        editable(s, st);
        const StrokeOptions o = optionsOf(s);
        const Canvas cv = canvasOf(s);
        const Point press = st.points[0];
        if (!s.sampleMerged) {
            const auto px = floodPixels(cv, press.x, press.y, o);
            land(s, st, plotPixels(cv, px, colourFor(s, st), o, mirror(s)), true, out);
        } else {
            // FillTool with Sample merged: the region found in the
            // composite, which is flat cells (the layer's lock is for the
            // paint), painted into the active layer. On a cells layer, not
            // alpha-locked, a pixel lands in a clear cell over what that
            // cell shows through to (Document::shownBelow), so the half
            // outside the region keeps that and nothing is copied from the
            // layers above.
            StrokeOptions merged = o;
            merged.lockTransparent = false;
            merged.pixelLayer = false;
            const auto px = floodPixels(compositeOf(s), press.x, press.y, merged);
            Canvas base = cv;
            if (!o.pixelLayer && !o.lockTransparent) {
                const std::size_t index = s.index();
                const CompositeOptions opts{s.p.ice, Cell{}};
                for (int y = 0; y < base.height(); ++y)
                    for (int x = 0; x < base.width(); ++x)
                        if (base.at(x, y).transparent)
                            base.at(x, y) = compositeCellBelow(s.stack(), index, x, y, opts);
            }
            land(s, st, plotPixels(base, px, colourFor(s, st), o, mirror(s)), true, out);
        }
    } else if (a == "eyedropper") {
        onCanvas(s, st, true);
        eyedropper(s, st, out);
    } else if (a == "gradient") {
        onCanvas(s, st, false);
        editable(s, st);
        pixelsHold(s, st);
        Gradient g;
        g.shape = s.gradientShape;
        g.banding = s.gradientBanding;
        g.x0 = st.points[0].x;
        g.y0 = st.points[0].y;
        g.x1 = st.points[1].x;
        g.y1 = st.points[1].y;
        const bool reversed = st.button == Button::Right;
        g.fromRung = reversed ? s.gradientTo : s.gradientFrom;
        g.toRung = reversed ? s.gradientFrom : s.gradientTo;
        land(s, st, rampGradient(canvasOf(s), s.ramp, g, optionsOf(s)), true, out);
    } else if (a == "keyboard") {
        onCanvas(s, st, false);
        keyboard(s, st, out);
    } else if (a == "move") {
        moveLayer(s, st);
    } else if (a == "select") {
        const CellRect r = *st.rect;
        if (r.x < 0 || r.y < 0 || r.x + r.width > s.width() || r.y + r.height > s.height())
            fail(st, "the rectangle runs off the " + std::to_string(s.width()) + " x " + std::to_string(s.height()) +
                         " canvas");
        s.selection = Selection(r);
    } else if (a == "selectAll") {
        s.selection = Selection(s.stack().canvasRect());
    } else if (a == "deselect") {
        s.selection = {};
    } else if (a == "moveSelection") {
        moveSelected(s, st, out);
    } else if (a == "flipHorizontal") {
        transform(s, st, Transform::FlipHorizontal, out);
    } else if (a == "flipVertical") {
        transform(s, st, Transform::FlipVertical, out);
    } else if (a == "rotateClockwise") {
        transform(s, st, Transform::RotateClockwise, out);
    } else if (a == "rotateCounterClockwise") {
        transform(s, st, Transform::RotateCounterClockwise, out);
    } else if (a == "copy" || a == "cut") {
        needSelection(s, st);
        s.clipboard = copied(s);
        if (a == "cut")
            land(s, st, clearSelection(canvasOf(s), s.selection, blankCell(s)), false, out);
    } else if (a == "deleteSelection") {
        needSelection(s, st);
        land(s, st, clearSelection(canvasOf(s), s.selection, blankCell(s)), false, out);
    } else if (a == "paste") {
        onCanvas(s, st, false);
        paste(s, st, out);
    } else if (a == "newCellsLayer" || a == "newPixelLayer") {
        const LayerKind kind = a == "newPixelLayer" ? LayerKind::Pixels : LayerKind::Cells;
        if (st.rename)
            for (const Layer& l : s.stack().layers())
                if (l.props.name == *st.rename)
                    fail(st, "a layer is already named \"" + *st.rename + "\"");
        Layer layer = makeLayer(kind, s.width(), s.height(), st.rename ? *st.rename : freeLayerName(s));
        s.active = s.stack().add(std::move(layer), s.index() + 1);
    } else if (a == "deleteLayer") {
        if (s.stack().size() < 2)
            fail(st, "can't delete the only layer");
        const std::size_t i = s.index();
        const LayerId next = s.stack()[i > 0 ? i - 1 : 1].props.id;
        s.stack().remove(s.active);
        s.active = next;
    } else if (a == "raiseLayer" || a == "lowerLayer") {
        const std::size_t i = s.index();
        if (a == "raiseLayer" ? i + 1 >= s.stack().size() : i == 0)
            fail(st, "the active layer \"" + s.layer().props.name + "\" is already at the " +
                         (a == "raiseLayer" ? "top" : "bottom"));
        s.stack().move(s.active, a == "raiseLayer" ? i + 1 : i - 1);
    } else if (a == "setActiveLayer") {
        s.active = layerNamed(s, st, st.layer)->props.id;
    } else if (a == "layerProperties") {
        Layer& l = s.layer();
        if (st.rename && *st.rename != l.props.name)
            for (const Layer& other : s.stack().layers())
                if (other.props.name == *st.rename)
                    fail(st, "a layer is already named \"" + *st.rename + "\"");
        if (st.rename)
            l.props.name = *st.rename;
        if (st.visible)
            l.props.visible = *st.visible;
        if (st.locked)
            l.props.locked = *st.locked;
        if (st.alphaLocked)
            l.props.alphaLocked = *st.alphaLocked;
    } else if (a == "canvasSize") {
        canvasSize(s, st, out);
    } else if (a == "view") {
        // the window's, not the document's: no cell changes
        const steps::View& v = st.view;
        if (v.scrollTo && (v.scrollTo->x >= s.width() || v.scrollTo->y >= s.height()))
            fail(st, "scrollTo cell " + at(*v.scrollTo) + " is off the " + std::to_string(s.width()) + " x " +
                         std::to_string(s.height()) + " canvas");
        if (v.zoom)
            s.view.zoom = v.zoom;
        if (v.scrollTo)
            s.view.scrollTo = v.scrollTo;
        if (v.ircFit)
            s.view.ircFit = v.ircFit;
        if (v.ircPad)
            s.view.ircPad = v.ircPad;
        if (v.ircSwapShades)
            s.view.ircSwapShades = v.ircSwapShades;
    } else {
        fail(st, "replay can't apply " + a + " yet");
    }
}

project::Project readStartFile(const fs::path& path, std::vector<std::string>* notes) {
    const std::string name = path.string();
    std::error_code ec;
    if (!fs::exists(path, ec))
        throw Failure{"start: " + name + ": no such file"};
    try {
        if (project::isProject(path)) {
            project::ReadNotes read;
            project::Project p = project::read(path, &read);
            if (notes)
                for (const std::string& n : projectReadNotes(read))
                    notes->push_back("start: " + name + ": " + n);
            return p;
        }
        const std::string data = readFile(name);
        if (data.empty())
            throw Failure{"start: " + name + ": empty file"};
        return project::fromAnsi(parseArt(data, artFormatOf(name, data)));
    } catch (const project::ProjectError& e) {
        throw Failure{"start: " + name + ": " + e.what()};
    } catch (const AnsiError& e) {
        throw Failure{"start: " + name + ": " + e.what()};
    } catch (const Failure& f) {
        throw Failure{f.message.rfind("start: ", 0) == 0 ? f.message : "start: " + f.message};
    }
}

project::Project readSample(const steps::Start& st, const fs::path& samplesDir, std::vector<std::string>* notes) {
    for (const samples::Sample& sample : samples::list(samplesDir)) {
        const fs::path& p = sample.path;
        if (p.filename().string() != st.sample && !(p.extension() == ".bwp" && p.stem().string() == st.sample))
            continue;
        if (!sample.problem.empty())
            throw Failure{"start: sample " + st.sample + ": " + sample.problem};
        project::Project proj = readStartFile(p, notes);
        if (st.variant.empty())
            return proj;
        try {
            const samples::Notes notes = samples::parseNotes(proj.notes);
            std::string names;
            for (const samples::Variant& v : notes.variants) {
                if (v.name == st.variant) {
                    samples::applyVariant(proj.layers, v);
                    return proj;
                }
                names += (names.empty() ? "" : ", ") + v.name;
            }
            throw Failure{"start: sample " + st.sample + " has no variant \"" + st.variant + "\"" +
                          (names.empty() ? std::string() : "; it has " + names)};
        } catch (const samples::SampleError& e) {
            throw Failure{"start: sample " + st.sample + ": " + e.what()};
        }
    }
    std::string names;
    for (const samples::Sample& sample : samples::list(samplesDir))
        names += (names.empty() ? "" : ", ") + sample.path.filename().string();
    throw Failure{"start: no sample \"" + st.sample + "\" in " + samplesDir.string() +
                  (names.empty() ? std::string() : "; the samples are " + names)};
}

} // namespace

project::Project start(const steps::Script& script, const fs::path& dir, const fs::path& samplesDir,
                       std::vector<std::string>* notes) {
    const steps::Start& st = script.start;
    switch (st.kind) {
    case steps::Start::Kind::File:
        return readStartFile(dir / fs::path(std::u8string(st.file.begin(), st.file.end())), notes);
    case steps::Start::Kind::Sample: return readSample(st, samplesDir, notes);
    case steps::Start::Kind::Blank: break;
    }
    project::Project p;
    p.layers = LayerStack::fromCanvas(Canvas(st.width, st.height));
    p.ice = script.ice.value_or(true);
    return p;
}

AnsiDocument document(const project::Project& project, const std::optional<std::string>& today) {
    AnsiDocument doc = project::toAnsi(project);
    if (!project.sauce && today && doc.sauce)
        doc.sauce->date = *today;
    return doc;
}

project::Project startSaying(const steps::Script& script, const fs::path& dir, const fs::path& samplesDir,
                             std::ostream& err, const std::string& program, const std::string& stepsPath) {
    std::vector<std::string> notes;
    const auto say = [&] {
        for (const std::string& n : notes)
            err << program << ": note: " << stepsPath << ": " << n << "\n";
    };
    try {
        project::Project p = start(script, dir, samplesDir, &notes);
        say();
        return p;
    } catch (const Failure&) {
        say();
        throw;
    }
}

Outcome run(const steps::Script& script, project::Project project, const Observer& observe) {
    State s;
    s.p = std::move(project);
    if (s.stack().empty())
        throw Failure{"start: it has no layers"};
    // the editor opens on the bottom layer
    s.active = s.stack()[0].props.id;
    Outcome out;
    const int count = int(script.steps.size());
    for (const Step& st : script.steps) {
        const std::string stem = stepNumber(st.number, count) + "-" + st.name;
        if (st.capture == steps::Capture::BeforeAfter)
            out.captures.push_back({stem + "-before", s.p});
        if (observe) {
            ToolSettings set = s;
            take(set, st.options);
            observe({st, true, s.p, s.active, s.selection, set, s.view});
        }
        try {
            step(s, st, out);
        } catch (const Failure&) {
            throw;
        } catch (const std::bad_alloc&) {
            throw;  // main's "out of memory"
        } catch (const std::exception& e) {
            // a core error a step's checks don't foresee (ids exhausted, say)
            // is still this step's error, not an abort
            fail(st, e.what());
        }
        if (observe)
            observe({st, false, s.p, s.active, s.selection, s, s.view});
        out.captures.push_back({st.capture == steps::Capture::BeforeAfter ? stem + "-after" : stem, s.p});
    }
    return out;
}

const steps::Step* viewStep(const steps::Script& script) {
    for (const Step& st : script.steps)
        if (st.action == "view")
            return &st;
    return nullptr;
}

std::string stepNumber(int number, int steps) {
    const int width = std::max(2, int(std::to_string(steps).size()));
    std::string n = std::to_string(number);
    return std::string(std::size_t(std::max(0, width - int(n.size()))), '0') + n;
}

} // namespace bwcli::replay
