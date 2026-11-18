#include "replay.h"

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/clone.h"
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
#include "blockwright/cp437.h"
#include "blockwright/edit.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/regular_file.h"
#include "blockwright/samples.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"
#include "blockwright/target.h"
#include "blockwright/tdf.h"
#include "blockwright/text.h"
#include "blockwright/width.h"
#include "blockwright/xbin.h"
#include "loss.h"
#include "read_file.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <set>

#ifndef _WIN32
#include <unistd.h>
#endif

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
    if (o.aligned)
        s.clone.aligned = *o.aligned;
    if (o.sampleMerged)
        s.sampleMerged = *o.sampleMerged;
    // "textOptions" is the text step's, which sets it (text())
}

namespace {

// What the editor holds between steps: its document, and Tool Options.
struct State : ToolSettings {
    project::Project p;
    // the bytes of the files the steps read, by their paths as written (the
    // load pass's Script::files)
    const std::map<std::string, std::string>* files = nullptr;
    const std::map<std::string, std::string>* presets = nullptr;  // Script::presets
    const std::map<std::pair<std::string, std::string>, fs::path>* overwrites = nullptr;  // Script::userOverwrites
    const std::map<std::string, steps::Script::SaveCollection>* collections = nullptr;  // Script::userCollections
    LayerId active = 0;
    Selection selection;
    std::optional<CellBlock> clipboard;
    steps::View view;  // what the view steps have set (#398)
    // the Library (#431): the bundled stamps (Script::bundledStamps), the
    // user library's as the steps so far left it, and what they saved
    const std::vector<library::Stamp>* bundled = nullptr;
    std::vector<library::Stamp> user;
    std::vector<SavedStamp> saved;

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

// A Pixels layer holds half blocks and blanks: the tools that paint shades,
// and the glyph brush or the stars spray with a glyph it can't hold, refuse
// one before they paint (DragTool::refusesLayer). The glyph brush's refusal
// names the glyph, or what radial symmetry turns it into
// (GlyphTool::unheldGlyphs).
void pixelsHold(State& s, const Step& st) {
    if (s.layer().props.kind != LayerKind::Pixels)
        return;
    const std::string& a = st.action;
    const std::string layer = "the active layer \"" + s.layer().props.name + "\" is a pixels layer, which holds ";
    std::string what;
    if (a == "spray")
        what = s.spray.kind == SprayKind::Stars ? "stars" : "shades";
    else if (a == "shade" || a == "rampShade" || a == "form" || a == "soft" || a == "smudge" || a == "gradient" ||
             a == "rimLight" || a == "texture")
        what = "shades";
    else if (a == "glyph" && st.button == Button::Left) {
        const std::vector<char32_t> unheld = pixelsUnheldGlyphs(s.glyph, s.symmetry, s.width(), s.height());
        if (unheld.empty())
            return;
        // a glyph that shows nothing on its own (a format control, a mark)
        // is named by its code point, as a conversion warning names it
        const auto text = [](char32_t ch) {
            if (codePointWidth(ch) < 1)
                return codePoints(std::u32string_view(&ch, 1));
            std::string out;
            appendUtf8(out, ch);
            return out;
        };
        const std::string only = layer + "only " + text(kFullBlock) + " " + text(kUpperHalf) + " " +
                                 text(kLowerHalf) + " and blanks";
        if (unheld.front() == s.glyph)
            fail(st, only + ", not " + text(s.glyph));
        std::string turned;
        for (std::size_t i = 0; i < unheld.size(); ++i)
            turned += (i == 0 ? "" : i + 1 == unheld.size() ? " and " : ", ") + text(unheld[i]);
        fail(st, only + ", and radial symmetry turns " + text(s.glyph) + " on its side, into " + turned);
    }
    if (!what.empty())
        fail(st, layer + "no " + what);
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
        if (richTarget(s.p.target) && s.layer().props.kind != LayerKind::Pixels)
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
    if (!bg && richTarget(s.p.target))
        s.pen.attrs = cell.attrs & attr::Formatting;
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
                       "; target: " + (s.p.target == Target::Irc ? "IRC" : s.p.target == Target::Micron ? "Micron" : "ANSI") + ", " +
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

// Which step saved a stamp that has no file yet, and where it went: the last
// save of its title into its collection, or at the top level when its
// category names none (saveToLibrary replaces an earlier one of the same place).
std::string savedBy(const State& s, const library::Stamp& st) {
    const std::string collection = s.collections->count(st.meta.category) ? st.meta.category : std::string();
    for (auto it = s.saved.rbegin(); it != s.saved.rend(); ++it)
        if (it->name == st.name && it->collection == collection)
            return "the one " + it->step + " saved " +
                   (collection.empty() ? std::string("at the top level") : "in the collection \"" + collection + "\"");
    return "the one an earlier step saved";
}

// The stamp a placeStamp step names (#431): the only one of that name in
// the library it says, or in either when it says none.
const library::Stamp& stampNamed(const State& s, const Step& step) {
    const std::string name = cp437ToUtf8(step.stamp);
    const auto named = [&](const std::vector<library::Stamp>& from) {
        std::vector<const library::Stamp*> found;
        for (const library::Stamp& st : from)
            if (st.name == step.stamp)
                found.push_back(&st);
        return found;
    };
    const std::vector<const library::Stamp*> bundled = named(*s.bundled), user = named(s.user);
    using Library = Step::Library;
    if (!step.library && !bundled.empty() && !user.empty())
        fail(step, "the bundled and the user library both have a stamp named \"" + name +
                       "\"; say which with \"library\": \"bundled\" or \"user\"");
    const bool inUser = step.library ? *step.library == Library::User : bundled.empty();
    const std::vector<const library::Stamp*>& found = inUser ? user : bundled;
    if (found.empty())
        fail(step, "no stamp named \"" + name + "\" in " +
                       (step.library ? (inUser ? std::string("the user library") : std::string("the bundled library"))
                                     : std::string("the bundled or the user library")));
    if (found.size() > 1) {
        std::string paths;
        for (const library::Stamp* st : found)
            paths += (paths.empty() ? "" : ", ") + (st->path.empty() ? savedBy(s, *st) : st->path.string());
        fail(step, std::to_string(found.size()) + " stamps in the " + (inUser ? "user" : "bundled") +
                       " library are named \"" + name + "\": " + paths);
    }
    return *found.front();
}

// A Library stamp placed as the dock places it (MainWindow::placeStamp):
// floated in transparent paste mode, for this float only, and committed.
void placeStamp(State& s, const Step& step, Outcome& out) {
    const CellBlock block = stampNamed(s, step).block();
    const Point p = step.points[0];
    const CellEdit e =
        stampBlock(canvasOf(s), block, p.x, p.y, PasteMode::Transparent, nullptr, blankCell(s), pastePixels(s));
    if (land(s, step, e, false, out))
        s.selection = Selection(CellRect{p.x, p.y, block.width, block.height}).clipped(canvasOf(s));
}

// Whether `path` is inside folder `dir`, lexically.
bool under(const fs::path& path, const fs::path& dir) {
    const fs::path rel = path.lexically_relative(dir);
    return !rel.empty() && *rel.begin() != "..";
}

// Selection > Save to Library... (MainWindow::saveSelectionToLibrary): the
// selection's cells as the canvas shows them, as a stamp in the user
// library, replacing the one of its name the save writes over: one an
// earlier step saved, or the one in the library's own folder
// (Script::userOverwrites). One of its name in a subfolder is kept beside
// it, as Save to Library keeps it, with a note. A category that names a
// collection (#513) saves into it: the same rule, within its folder, where
// the stamp it replaces is one of that title anywhere in the folder. A
// category two folders go by is refused.
void saveToLibrary(State& s, const Step& step, Outcome& out) {
    needSelection(s, step);
    const auto into = s.collections->find(step.category);
    if (into != s.collections->end() && !into->second.error.empty())
        fail(step, into->second.error);
    const bool inCollection = into != s.collections->end();
    const fs::path& folder = inCollection ? into->second.dir : fs::path();
    const CellBlock cells = copyBlock(compositeOf(s), s.selection);
    library::StampFiles files;
    library::Stamp stamp;
    try {
        files = library::encodeStamp(step.stamp, step.category, step.tags, cells, s.p.ice);
        stamp = library::parseStamp(files);
    } catch (const library::LibraryError& e) {
        fail(step, e.what());
    }
    const auto over = s.overwrites->find({inCollection ? step.category : std::string(), step.stamp});
    // a stamp an earlier step saved is in this save's place when it went to
    // the same collection, or both went to the top level
    const auto savedHere = [&](const library::Stamp& st) {
        return inCollection ? st.meta.category == step.category : s.collections->count(st.meta.category) == 0;
    };
    const auto had = std::remove_if(s.user.begin(), s.user.end(), [&](const library::Stamp& st) {
        return st.name == step.stamp &&
               (st.path.empty() ? savedHere(st) : over != s.overwrites->end() && st.path == over->second);
    });
    const std::string name = cp437ToUtf8(step.stamp);
    if (had != s.user.end())
        out.notes.push_back(steps::stepLabel(step) + ": replaces the stamp \"" + name + "\" in the user library");
    s.user.erase(had, s.user.end());
    for (const library::Stamp& st : s.user)
        if (st.name == step.stamp && !st.path.empty() && (!inCollection || under(st.path, folder)))
            out.notes.push_back(steps::stepLabel(step) + ": the user library's stamp \"" + name + "\" in " +
                                st.path.string() + " is kept beside it, as Save to Library keeps it");
    s.user.push_back(std::move(stamp));
    s.saved.push_back({steps::stepLabel(step), step.stamp, std::move(files), inCollection ? step.category : std::string()});
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

// The first "<word> N" no layer is named (Document::freeLayerName).
std::string freeLayerName(State& s, const std::string& word = "Layer") {
    for (int n = 1;; ++n) {
        const std::string name = word + " " + std::to_string(n);
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

// The Clone Stamp (CloneTool): the source set first, as a Ctrl+click on it
// (a cell off the canvas is refused: the editor's click there sets
// nothing), then the stroke from the first point, through the core brush
// CloneSource::beginStroke makes and the same stroke as every brush. A
// stroke that lands sets the distance aligned strokes keep, as the tool's
// release does (CloneTool::finish), even one that changes nothing.
void clone(State& s, const Step& st, Outcome& out) {
    if (st.source) {
        const Point p = *st.source;
        if (p.x < 0 || p.y < 0 || p.x >= s.width() || p.y >= s.height())
            fail(st, "source cell " + at(p) + " is off the " + std::to_string(s.width()) + " x " +
                         std::to_string(s.height()) + " canvas");
        s.clone.setSource(p.x, p.y);
    }
    if (st.points.empty())
        return;
    onCanvas(s, st, false);
    editable(s, st);
    CloneSource next = s.clone;
    const std::optional<CloneBrush> brush = next.beginStroke(st.points[0].x, st.points[0].y);
    if (!brush)
        fail(st, "clone has no source to copy from: give \"source\" on this step or an earlier clone step");
    std::vector<Dab> dabs;
    for (const Point& p : st.points) {
        const Dab d{p.x, p.y, p.pressure};
        if (dabs.empty() || takesDab(dabs.back(), d, s.brush.pressure))
            dabs.push_back(d);
    }
    land(s, st, paintStroke(canvasOf(s), *brush, s.brush, dabs, optionsOf(s)), true, out);
    s.clone.anchor = next.anchor;
}

// The face a text step's TheDraw face names (#422): the font in the file
// the load pass read, by its name, at its index when the step gives one.
// The project's copy of the same bytes, or the file an earlier step named,
// is the one used. The file is kept for later steps (ToolSettings::fonts),
// as the Text tool keeps the file of the face new text starts in. Refused
// naming the step: a file that isn't a TheDraw font file (readTdf's
// reason), a name the file doesn't hold, a name two of its fonts share with
// no index given, and an index past its fonts or naming another font, each
// listing the fonts it holds.
FaceRef tdfFace(State& s, const Step& st, const steps::Options::Text::Tdf& t) {
    const auto read = s.files ? s.files->find(t.path) : std::map<std::string, std::string>::const_iterator{};
    if (!s.files || read == s.files->end())
        fail(st, t.path + ": not read; the load pass reads a step's files before the replay runs");
    const std::string sha = sha256Hex(read->second);
    FontFilePtr file = FontSources{&s.p.fonts, nullptr, &s.fonts}.file(sha);
    if (!file) {
        try {
            file = loadFontFile(read->second);
        } catch (const TdfError& e) {
            fail(st, t.path + ": not a TheDraw font file: " + e.what());
        }
    }
    const std::vector<std::string>& names = file->names;
    const auto quoted = [](const std::string& n) { return "\"" + n + "\""; };
    std::string holds;
    for (std::size_t i = 0; i < names.size(); ++i)
        holds += (i ? ", " : "") + std::to_string(i) + " " + quoted(names[i]);
    holds = "it holds " + holds;
    int index = 0;
    if (t.index) {
        if (std::size_t(*t.index) >= names.size())
            fail(st, t.path + " has no font at index " + std::to_string(*t.index) + "; " + holds);
        if (names[std::size_t(*t.index)] != t.font)
            fail(st, "the font at index " + std::to_string(*t.index) + " in " + t.path + " is " +
                         quoted(names[std::size_t(*t.index)]) + ", not " + quoted(t.font) + "; " + holds);
        index = *t.index;
    } else {
        std::vector<int> found;
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == t.font)
                found.push_back(int(i));
        if (found.empty())
            fail(st, "font " + quoted(t.font) + " is not in " + t.path + "; " + holds);
        if (found.size() > 1)
            fail(st, std::to_string(found.size()) + " fonts in " + t.path + " are named " + quoted(t.font) +
                         "; give \"index\" to pick one: " + holds);
        index = found[0];
    }
    s.fonts.add(file);
    return FaceRef{FaceRef::Kind::Tdf, t.font, file->sha256, index};
}

// A TheDraw face a layer has taken: its file copied into the project
// (Document::keepFont), so the project draws it without the steps file's
// fonts, as the editor copies a fonts-folder font.
void keepFont(State& s, const FaceRef& face) {
    if (face.kind == FaceRef::Kind::Tdf)
        if (FontFilePtr file = FontSources{&s.p.fonts, nullptr, &s.fonts}.file(face.file))
            s.p.fonts.add(std::move(file));
}

// A text step that would leave the text layers using over kMaxFontFiles
// font files fails: a project can't hold them (project::zipBytes refuses
// it), whatever the replay writes. `replaced` is the layer the step
// restyles, its old face no longer in use.
void checkFontCount(State& s, const Step& st, const FaceRef& face, const Layer* replaced) {
    if (face.kind != FaceRef::Kind::Tdf)
        return;
    std::set<std::string> used{face.file};
    for (const Layer& l : s.stack().layers())
        if (&l != replaced && l.props.text && l.props.text->face.kind == FaceRef::Kind::Tdf)
            used.insert(l.props.text->face.file);
    if (used.size() > bw::project::kMaxFontFiles)
        fail(st, "the text layers would use over " + std::to_string(bw::project::kMaxFontFiles) +
                     " TheDraw font files, more than a project holds");
}

// The Text tool (TextTool). Tool Options are set as the dock sets them
// (TextOptions::apply, steps::applyTextOptions) on the settings it shows:
// the active layer's when it is a live text layer and no text cursor waits,
// else what a new layer starts with. As the dock applies only when a control
// changes, settings that change what it shows become what a new layer starts
// with, and a step with none, or with the ones shown, changes nothing. With "at", its click there comes first, off every text layer it
// would edit (a click on one edits it, which the step doesn't do), so the
// step's Tool Options set only what a new layer starts with; then the text
// is typed: a new live text layer of it in those settings, "Text N", just
// above the active layer and made active (Document::addTextLayer,
// makeTextLayer). With no "at", the active layer takes them too when it is
// a live text layer that isn't locked, one change of its source
// (TextTool::setSettings, textEdit).
void text(State& s, const Step& st, Outcome& out) {
    const FontSources fonts{&s.p.fonts, nullptr, &s.fonts};
    std::optional<FaceRef> tdf;
    if (st.options.text && st.options.text->tdf)
        tdf = tdfFace(s, st, *st.options.text->tdf);
    const auto set = [&](TextSource& source) {
        if (st.options.text)
            if (const auto problem = steps::applyTextOptions(source, *st.options.text, tdf ? &*tdf : nullptr))
                fail(st, *problem);
    };
    if (st.points.empty()) {
        // the dock applies only when a control changes: no settings, or
        // the ones it shows, leave the defaults and the layer alone
        if (!st.options.text)
            return;
        Layer& l = s.layer();
        const bool shown = bool(l.props.text);
        const TextSource base = shown ? *l.props.text : s.text;
        // as the controls show it: a big style's own colours where it has
        // none, none without a style
        TextSource dock = base;
        steps::applyTextOptions(dock, {});
        TextSource next = base;
        set(next);
        if (next == dock) {
            out.notes.push_back(steps::stepLabel(st) + ": changes nothing");
            return;
        }
        s.text = next;
        s.text.text.clear();
        if (!shown || l.props.locked)
            return;
        if (const auto problem = textSourceProblem(next, fonts))
            fail(st, "the active layer \"" + l.props.name + "\" can't be set so: " + *problem);
        const FaceRef face = next.face;
        checkFontCount(s, st, face, &l);
        applyEdit(s.stack(), textEdit(l, std::move(next), 0, s.p.ice), false, fonts);
        keepFont(s, face);
        return;
    }
    onCanvas(s, st, false);
    const Point p = st.points[0];
    if (const Layer* hit = textLayerAt(s.stack(), p.x, p.y))
        fail(st, "cell " + at(p) + " is on the text layer \"" + hit->props.name +
                     "\", which a Text tool click there edits; a text step makes a new text layer, so click off "
                     "every visible, unlocked text layer");
    set(s.text);
    TextSource source = s.text;
    source.text = st.text;
    if (const auto problem = textSourceProblem(source, fonts))
        fail(st, "the text can't be set: " + *problem);
    const FaceRef face = source.face;
    checkFontCount(s, st, face, nullptr);
    Layer layer = makeTextLayer(std::move(source), freeLayerName(s, "Text"), p.x, p.y, fonts);
    s.active = s.stack().add(std::move(layer), s.index() + 1);
    keepFont(s, face);
}

// The bytes of a file the load pass read for `st`, by its path as written.
const std::string& loaded(State& s, const Step& st, const std::string& path) {
    const auto read = s.files ? s.files->find(path) : std::map<std::string, std::string>::const_iterator{};
    if (!s.files || read == s.files->end())
        fail(st, path + ": not read; the load pass reads a step's files before the replay runs");
    return read->second;
}

// editText (#422 D3): the active live text layer's whole text replaced in
// one edit (textEdit), as the Text tool's typing would leave it. The layer
// must be one the tool's click could edit: a live text layer, visible and
// unlocked (textLayerAt). The same text changes nothing.
void editText(State& s, const Step& st, Outcome& out) {
    Layer& l = s.layer();
    const std::string name = "the active layer \"" + l.props.name + "\"";
    if (l.props.kind != LayerKind::Text || !l.props.text)
        fail(st, name + " is " +
                     (l.props.kind == LayerKind::Text ? std::string("a text layer with no source (cells read as text)")
                                                      : std::string("a ") + kindName(l.props.kind) + " layer") +
                     "; editText edits a live text layer's text");
    if (l.props.locked)
        fail(st, name + " is locked");
    if (!l.props.visible)
        fail(st, name + " is hidden");
    if (l.props.text->text == st.text) {
        out.notes.push_back(steps::stepLabel(st) + ": changes nothing");
        return;
    }
    const FontSources fonts{&s.p.fonts, nullptr, &s.fonts};
    TextSource next = *l.props.text;
    next.text = st.text;
    if (const auto problem = textSourceProblem(next, fonts))
        fail(st, "the text can't be set: " + *problem);
    applyEdit(s.stack(), textEdit(l, std::move(next), 0, s.p.ice), false, fonts);
}

// applyTextStyle (#422 D2): a Library text style on the active layer, as
// the Library dock's activation puts it there (MainWindow::applyTextStyle):
// refused when it needs iCE colours and they are off, on a layer that
// isn't live text, cells or pixels, and when the layer can't draw it
// (layerStyleProblem); the style the layer has already changes nothing.
void applyTextStyle(State& s, const Step& st, Outcome& out) {
    std::string what;
    library::TextStyleEntry entry;
    try {
        if (st.stylePreset) {
            what = "text style \"" + *st.stylePreset + "\"";
            const auto it = s.presets ? s.presets->find(*st.stylePreset)
                                      : std::map<std::string, std::string>::const_iterator{};
            if (!s.presets || it == s.presets->end())
                fail(st, what + ": not read; the load pass reads the presets before the replay runs");
            entry = library::parseTextStyleEntry(it->second);
        } else {
            what = st.files.at(0).path;
            entry = library::parseTextStyleEntry(loaded(s, st, what));
        }
    } catch (const library::LibraryError& e) {
        fail(st, what + ": " + e.what());
    }
    if (entry.iceRequired && !s.p.ice)
        fail(st, what + " (" + entry.name + ") needs iCE colours, which are off");
    Layer& l = s.layer();
    if (!(l.props.kind == LayerKind::Cells || l.props.kind == LayerKind::Pixels ||
          (l.props.kind == LayerKind::Text && l.props.text)))
        fail(st, "the active layer \"" + l.props.name + "\" is a " + kindName(l.props.kind) +
                     " layer; a text style goes on a live text, cells or pixels layer");
    LayerProperties props = l.props;
    props.style = entry.style;
    if (props.style == l.props.style) {
        out.notes.push_back(steps::stepLabel(st) + ": changes nothing (the layer has this style already)");
        return;
    }
    const FontSources fonts{&s.p.fonts, nullptr, &s.fonts};
    if (const auto problem = layerStyleProblem(props, fonts))
        fail(st, "the active layer \"" + l.props.name + "\" can't take " + what + ": " + *problem);
    applyEdit(s.stack(), propsEdit(l, std::move(props), 0, s.p.ice), false, fonts);
}

// The EXIF orientation (1-8) in a JPEG's APP1 segments joined in order, as
// Qt's JPEG reader finds it (the editor reads with auto-transform): the
// joined bytes start "Exif\0\0" and a TIFF header in either byte order, and
// the first Orientation tag (0x0112) in up to 10 linked IFDs, each starting
// at or past the end of the one before (the first at 8 or later), counts if
// it is one SHORT. 0 when there is none or the bytes run out, which the editor
// takes as no turn either.
unsigned exifOrientation(std::string_view e) {
    if (e.size() < 14 || e.substr(0, 6) != std::string_view("Exif\0\0", 6))
        return 0;
    const std::string_view t = e.substr(6);  // offsets count from the TIFF header
    bool little = false;
    if (t.substr(0, 2) == "II")
        little = true;
    else if (t.substr(0, 2) != "MM")
        return 0;
    const auto u16 = [&](std::size_t i) -> std::optional<unsigned> {
        if (i > t.size() || t.size() - i < 2)
            return std::nullopt;
        const unsigned a = static_cast<unsigned char>(t[i]), b = static_cast<unsigned char>(t[i + 1]);
        return little ? (b << 8) | a : (a << 8) | b;
    };
    const auto u32 = [&](std::size_t i) -> std::optional<std::uint32_t> {
        const auto a = u16(i), b = u16(i + 2);
        if (!a || !b)
            return std::nullopt;
        return little ? (std::uint32_t(*b) << 16) | *a : (std::uint32_t(*a) << 16) | *b;
    };
    if (u16(2) != 0x2Au)
        return 0;
    auto offset = u32(4);
    // Qt's reader reads the TIFF bytes as a stream that only seeks forward:
    // an IFD that starts before where the stream stands (8 after the header,
    // then just past the previous IFD's link) is refused, no turn.
    std::size_t next = 8;
    for (int ifd = 0; offset && *offset != 0 && ifd < 10; ++ifd) {
        if (*offset < next)
            return 0;
        const auto count = u16(*offset);
        if (!count)
            return 0;
        for (unsigned n = 0; n < *count; ++n) {
            const std::size_t entry = std::size_t(*offset) + 2 + std::size_t(n) * 12;
            const auto tag = u16(entry);
            if (!tag)
                return 0;
            if (*tag != 0x0112)
                continue;
            const auto type = u16(entry + 2);
            const auto values = u32(entry + 4);
            const auto value = u16(entry + 8);
            if (type != 3u || values != 1u || !value || *value < 1 || *value > 8)
                return 0;
            return *value;
        }
        const std::size_t link = std::size_t(*offset) + 2 + std::size_t(*count) * 12;
        offset = u32(link);
        next = link + 4;
    }
    return 0;
}

// A PNG's or JPEG's size in pixels as the editor shows it, from its header
// (IHDR, or the first JPEG start-of-frame marker with the EXIF orientation
// of the segments before the scan: 5-8 turn it a quarter, so width and
// height swap), or nothing when it can't be found there.
std::optional<std::pair<int, int>> pictureSize(std::string_view b, std::string_view type) {
    const auto byte = [&](std::size_t i) { return unsigned(static_cast<unsigned char>(b[i])); };
    const auto be16 = [&](std::size_t i) { return (byte(i) << 8) | byte(i + 1); };
    const auto be32 = [&](std::size_t i) { return (std::uint32_t(be16(i)) << 16) | be16(i + 2); };
    std::uint32_t w = 0, h = 0;
    bool turned = false;
    if (type == "png") {
        // Qt's PNG reader applies no orientation (an eXIf chunk is ignored)
        if (b.size() < 24 || b.substr(12, 4) != "IHDR")
            return std::nullopt;
        w = be32(16);
        h = be32(20);
    } else {
        std::string exif;  // the APP1 segments' bytes, joined
        bool frame = false;
        std::size_t i = 2;  // past SOI
        while (true) {
            while (i < b.size() && byte(i) != 0xFF)
                ++i;
            while (i < b.size() && byte(i) == 0xFF)
                ++i;
            if (i >= b.size())
                break;
            const unsigned m = byte(i++);
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD8))
                continue;  // markers with no length
            if (m == 0xD9 || m == 0xDA || i + 2 > b.size())
                break;  // the end, the scan, or a cut segment
            const unsigned len = be16(i);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && !frame) {
                if (len < 7 || i + 7 > b.size())
                    return std::nullopt;
                h = be16(i + 3);
                w = be16(i + 5);
                frame = true;
            }
            if (len < 2)
                break;
            if (m == 0xE1)
                exif.append(b.substr(i + 2, std::min<std::size_t>(len - 2, b.size() - (i + 2))));
            i += len;
        }
        if (!frame)
            return std::nullopt;
        turned = exifOrientation(exif) >= 5;
    }
    if (w == 0 || h == 0 || w > std::uint32_t(ReferenceImage::kMaxPlace) || h > std::uint32_t(ReferenceImage::kMaxPlace))
        return std::nullopt;
    if (turned)
        std::swap(w, h);
    return std::pair{int(w), int(h)};
}

// File > Open Reference Image... (#422 D5, MainWindow::loadReferenceImage):
// the picture kept byte for byte, PNG and JPEG only (the window would
// re-encode any other format as a PNG, which isn't the same bytes on every
// system), as a new reference layer "Reference N" on top of the stack, made
// active, scaled across the canvas's width in pixels with its aspect kept.
void openReferenceImage(State& s, const Step& st) {
    const std::string& path = st.files.at(0).path;
    const std::string& bytes = loaded(s, st, path);
    const auto type = referenceImageType(bytes);
    if (!type)
        fail(st, path + ": not a PNG or JPEG picture; save it as a PNG for a steps file");
    const auto size = pictureSize(bytes, *type);
    if (!size)
        fail(st, path + ": its size can't be read from its " + (*type == "png" ? "PNG" : "JPEG") + " header");
    // the editor decodes a PNG in full, so refuses one past its ceiling (a
    // JPEG it decodes scaled down)
    if (*type == "png" && std::int64_t(size->first) * size->second > ReferenceImage::kMaxDecodePixels)
        fail(st, path + ": too large to decode: " + std::to_string(size->first) + " x " +
                     std::to_string(size->second) + " pixels (limit " +
                     std::to_string(ReferenceImage::kMaxDecodePixels) + "), as File > Open Reference Image... refuses it");
    ReferenceImage picture;
    picture.bytes = std::make_shared<const std::string>(bytes);
    picture.width = std::max(1, s.width() * canvasCellWidth(s.p.sauce));
    picture.height = int(std::clamp<double>(std::round(double(picture.width) * size->second / size->first), 1,
                                            ReferenceImage::kMaxPlace));
    Layer layer = makeLayer(LayerKind::Reference, 0, 0, freeLayerName(s, "Reference"));
    layer.props.reference = std::move(picture);
    if (const auto problem = project::layerReferenceProblem(layer.props))
        fail(st, path + ": the picture can't be kept in a project: " + *problem);
    s.active = s.stack().add(std::move(layer), s.stack().size());
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

// The Colours menu's palette steps (#497), as Document::setPalette,
// fitToPalette and remapColours do them: the standard palette is none.
void paletteStep(State& s, const Step& st, Outcome& out) {
    const std::optional<Palette> to = st.palette == kPalette ? std::nullopt : std::optional(st.palette);
    std::vector<std::pair<LayerId, CellEdit>> edits;
    std::vector<std::pair<LayerId, TextStyle>> styles;
    int skipped = 0;
    if (st.action == "fitToPalette") {
        const ColourMap map = fitColourMap(s.p.palette.value_or(kPalette), st.palette);
        edits = remapLayers(s.stack(), map, &skipped);
        styles = remapStyles(s.stack(), map);
    }
    if (st.action == "remapColours") {
        if (st.remapScope == Step::RemapScope::Document) {
            edits = remapLayers(s.stack(), st.colourMap);
        } else {
            editable(s, st);
            if (st.remapScope == Step::RemapScope::Selection)
                needSelection(s, st);
            const Layer& l = s.layer();
            std::optional<CellRect> area;
            if (st.remapScope == Step::RemapScope::Selection) {
                const CellRect& r = s.selection.bounds();
                area = CellRect{r.x - l.props.x, r.y - l.props.y, r.width, r.height};
            }
            CellEdit e = remapEdit(l.cells, st.colourMap, area, l.props.kind == LayerKind::Pixels);
            if (!e.changes.empty())
                edits.emplace_back(l.props.id, std::move(e));
        }
    }
    const bool palette = st.action != "remapColours" && to != s.p.palette;
    if (!palette && edits.empty() && styles.empty()) {
        out.notes.push_back(steps::stepLabel(st) + ": changes nothing");
        return;
    }
    const Palette from = s.p.palette.value_or(kPalette);
    const Canvas before = st.action == "fitToPalette" ? compositeOf(s) : Canvas{0, 0};
    if (st.action != "remapColours")
        s.p.palette = to;
    for (const auto& [id, e] : edits)
        e.apply(s.stack().find(id)->cells);
    for (const auto& [id, style] : styles)
        s.stack().find(id)->props.style = style;
    if (st.action != "fitToPalette")
        return;
    // as the editor's status bar says it (MainWindow::fitToPalette)
    if (const int lost = lookChanged(before, from, compositeOf(s), st.palette))
        out.notes.push_back(steps::stepLabel(st) + ": " + std::to_string(lost) +
                            (lost == 1 ? " cell shows" : " cells show") + " another colour");
    if (skipped)
        out.notes.push_back(steps::stepLabel(st) + ": " + std::to_string(skipped) +
                            (skipped == 1 ? " layer" : " layers") + " left as they are");
}

// Colours > Snap to Micron Colours (#491 slice 3), as
// Document::snapToMicronColours does it: the selection on the active layer,
// or the layer without one. Micron only, as the menu item is.
void snapMicronStep(State& s, const Step& st, Outcome& out) {
    if (s.p.target != Target::Micron)
        fail(st, "Snap to Micron Colours needs the Micron target (Format > Target > Micron)");
    editable(s, st);
    Layer& l = s.layer();
    std::optional<CellRect> area;
    if (!s.selection.empty()) {
        const CellRect& r = s.selection.bounds();
        area = CellRect{r.x - l.props.x, r.y - l.props.y, r.width, r.height};
    }
    const CellEdit e = micronSnapEdit(l.cells, area, l.props.kind == LayerKind::Pixels);
    if (e.changes.empty()) {
        out.notes.push_back(steps::stepLabel(st) + ": changes nothing");
        return;
    }
    e.apply(l.cells);
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
    } else if (a == "placeStamp") {
        onCanvas(s, st, false);
        placeStamp(s, st, out);
    } else if (a == "saveToLibrary") {
        saveToLibrary(s, st, out);
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
    } else if (a == "clone") {
        clone(s, st, out);
    } else if (a == "canvasSize") {
        canvasSize(s, st, out);
    } else if (a == "palette" || a == "fitToPalette" || a == "remapColours") {
        paletteStep(s, st, out);
    } else if (a == "snapMicronColours") {
        snapMicronStep(s, st, out);
    } else if (a == "text") {
        text(s, st, out);
    } else if (a == "editText") {
        editText(s, st, out);
    } else if (a == "applyTextStyle") {
        applyTextStyle(s, st, out);
    } else if (a == "openReferenceImage") {
        openReferenceImage(s, st);
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
        if (v.guides)
            s.view.guides = v.guides;
    } else {
        fail(st, "replay can't apply " + a + " yet");
    }
}

// The regular file at `p`, read through its opened handle
// (library::readRegularFile), so a FIFO or device swapped in after the
// checks can't block or flood the read. Throws Failure "<p>: <why>": no such
// file, is a directory, not a regular file, larger than `limit` bytes, or
// the system's reason.
std::string readChecked(const fs::path& p, std::size_t limit) {
    const std::string name = p.string();
    const auto refuse = [&](const std::string& why) { return Failure{name + ": " + why}; };
    std::error_code ec;
    const fs::file_status status = fs::status(p, ec);
    if (status.type() == fs::file_type::not_found)
        throw refuse("no such file");
    if (ec)
        throw refuse(ec.message());
    if (status.type() == fs::file_type::directory)
        throw refuse("is a directory");
    if (status.type() != fs::file_type::regular)
        throw refuse("not a regular file");
    try {
        return library::readRegularFile(p, limit);
    } catch (const library::LibraryError& e) {
        const std::string what = e.what();
        if (what == "is not a regular file")
            throw refuse("not a regular file");
        if (what.rfind("is larger than ", 0) == 0)
            throw refuse("larger than " + std::to_string(limit) + " bytes, the most it may be");
        throw refuse(what);
    }
}

// Whether the regular file at `p` starts with a zip's magic, read through
// readRegularHead, so a FIFO swapped in can't block it; anything else (gone,
// not regular, unreadable) is false and left for readChecked to name.
bool zipMagic(const fs::path& p) {
    try {
        return readRegularHead(p, 4) == std::string_view("PK\x03\x04", 4);
    } catch (const RegularFileError&) {
        return false;
    }
}

project::Project readStartFile(const fs::path& path, std::vector<std::string>* notes) {
    const std::string name = path.string();
    std::error_code ec;
    if (!fs::exists(path, ec))
        throw Failure{"start: " + name + ": no such file"};
    const auto readProject = [&] {
        project::ReadNotes read;
        project::Project p = project::read(path, &read);
        if (notes)
            for (const std::string& n : projectReadNotes(read))
                notes->push_back("start: " + name + ": " + n);
        return p;
    };
    try {
        if (fs::is_directory(path, ec) && project::isProject(path))
            return readProject();
        // a file is sniffed and read through checked handles, a zip
        // project's reader included (#448)
        if (zipMagic(path))
            return readProject();
        const std::string data = readChecked(path, project::kMaxEntryBytes);
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

fs::path resolve(const fs::path& dir, const std::string& path) {
    return dir / fs::path(std::u8string(path.begin(), path.end()));
}

project::Project start(const steps::Script& script, const fs::path& dir, const fs::path& samplesDir,
                       std::vector<std::string>* notes) {
    const steps::Start& st = script.start;
    switch (st.kind) {
    case steps::Start::Kind::File:
        // the parser refuses these; a Script made in code meets the same rule
        if (const auto why = steps::pathProblem(st.file))
            throw Failure{"start: \"file\" " + *why};
        return readStartFile(resolve(dir, st.file), notes);
    case steps::Start::Kind::Sample: return readSample(st, samplesDir, notes);
    case steps::Start::Kind::Blank: break;
    }
    project::Project p;
    p.layers = LayerStack::fromCanvas(Canvas(st.width, st.height));
    p.ice = script.ice.value_or(true);
    return p;
}

namespace {

// --user-library refused before any step runs (#431): a path that isn't a
// directory (a regular file is no empty library), and, when a step saves
// to it (`written`), a directory, or the nearest one above it a save would
// make it in, that can't be written.
void checkUserLibrary(const fs::path& dir, bool written) {
    fs::path at = dir;
    std::error_code ec;
    while (!fs::exists(fs::symlink_status(at, ec))) {
        const fs::path up = at.parent_path();
        if (up.empty() || up == at) {
            at = up.empty() ? fs::path(".") : up;
            break;
        }
        at = up;
    }
    const std::string where = "--user-library " + dir.string() + ": " + (at == dir ? std::string() : at.string() + ": ");
    if (!fs::is_directory(fs::status(at, ec)))
        throw Failure{where + "not a directory"};
#ifndef _WIN32
    if (written && ::access(at.c_str(), W_OK | X_OK) != 0) {
        const int err = errno;
        throw Failure{where + "can't be written: " + std::strerror(err)};
    }
#else
    (void)written;
#endif
}

} // namespace

void load(steps::Script& script, const fs::path& dir, const fs::path& libraryDir, const fs::path& userLibraryDir) {
    const bool saves = std::any_of(script.steps.begin(), script.steps.end(),
                                   [](const Step& st) { return st.action == "saveToLibrary"; });
    if (!userLibraryDir.empty()) {
        checkUserLibrary(userLibraryDir, saves);
        if (saves) {
            // refused here, before a capture is written or a folder made
            try {
                library::refuseInsideBundled(userLibraryDir);
            } catch (const library::LibraryError& e) {
                throw Failure{std::string("saveToLibrary: ") + e.what()};
            }
        }
    }
    if (std::any_of(script.steps.begin(), script.steps.end(),
                    [](const Step& st) { return st.action == "placeStamp" || st.action == "saveToLibrary"; })) {
        // what the scan can't read is left out, as the Library dock leaves
        // it out (it lists it as a problem)
        const auto scanned = [](const fs::path& d, const std::string& which) {
            try {
                return library::scan(d).stamps;
            } catch (const std::exception& e) {
                throw Failure{which + " " + d.string() + ": " + e.what()};
            }
        };
        if (!libraryDir.empty())
            script.bundledStamps = scanned(libraryDir, "the bundled library");
        if (!userLibraryDir.empty())
            script.userStamps = scanned(userLibraryDir, "the user library");
    }
    // what each save writes over, as the library is before the replay: a
    // save only ever adds a stamp of a new title, on a slot no stamp of
    // another title sits on, so an earlier save in the replay can't change it
    if (saves && !userLibraryDir.empty())
        for (const Step& st : script.steps) {
            if (st.action != "saveToLibrary")
                continue;
            try {
                // a category that names a collection saves into it (#513);
                // one two folders go by is refused when its step runs
                if (!script.userCollections.count(st.category)) {
                    try {
                        if (const auto c = library::findCollection(userLibraryDir, st.category))
                            script.userCollections[st.category] = {c->dir, {}};
                    } catch (const library::LibraryError& e) {
                        script.userCollections[st.category] = {{}, e.what()};
                    }
                }
                const auto into = script.userCollections.find(st.category);
                const bool inCollection = into != script.userCollections.end();
                if (inCollection && !into->second.error.empty())
                    continue;
                const auto key = std::make_pair(inCollection ? st.category : std::string(), st.stamp);
                if (script.userOverwrites.count(key))
                    continue;
                if (inCollection) {
                    // the title held anywhere in the folder, a stamp
                    // renamed in place included
                    const auto held = std::find_if(script.userStamps.begin(), script.userStamps.end(),
                                                   [&](const library::Stamp& s) {
                                                       return s.name == st.stamp && under(s.path, into->second.dir);
                                                   });
                    if (held != script.userStamps.end()) {
                        script.userOverwrites[key] = held->path;
                        continue;
                    }
                }
                const library::StampTarget t =
                    library::stampPathFor(inCollection ? into->second.dir : userLibraryDir, st.stamp);
                if (t.exists)
                    script.userOverwrites[key] = t.path;
            } catch (const std::exception& e) {
                throw Failure{"the user library " + userLibraryDir.string() + ": " + e.what()};
            }
        }
    for (const Step& st : script.steps) {
        if (st.stylePreset && !script.presets.count(*st.stylePreset)) {
            const fs::path styles = libraryDir / "text-styles";
            const fs::path p = styles / (*st.stylePreset + ".style.json");
            std::error_code ec;
            if (libraryDir.empty() || !fs::exists(fs::status(p, ec))) {
                // the presets there, by name, sorted so the message is the same on any system
                std::set<std::string> names;
                if (!libraryDir.empty())
                    for (const auto& e : fs::directory_iterator(styles, ec))
                        if (const std::string f = e.path().filename().string(); library::isTextStyleName(f))
                            names.insert(f.substr(0, f.size() - std::string_view(".style.json").size()));
                std::string list;
                for (const std::string& n : names)
                    list += (list.empty() ? "" : ", ") + n;
                fail(st, "no bundled text style \"" + *st.stylePreset + "\"" +
                             (list.empty() ? std::string(" (no bundled library found)") : "; the bundled ones are " + list));
            }
            try {
                script.presets.emplace(*st.stylePreset, readChecked(p, library::kMaxSidecarBytes));
            } catch (const Failure& e) {
                fail(st, e.message);
            }
        }
        for (const steps::FileRef& f : st.files) {
            if (const auto why = steps::pathProblem(f.path))
                fail(st, "\"" + f.key + "\" " + *why);
            const fs::path p = resolve(dir, f.path);
            if (const auto had = script.files.find(f.path); had != script.files.end()) {
                if (had->second.size() > f.maxBytes)
                    fail(st, p.string() + ": larger than " + std::to_string(f.maxBytes) + " bytes, the most it may be");
                continue;
            }
            try {
                script.files.emplace(f.path, readChecked(p, f.maxBytes));
            } catch (const Failure& e) {
                fail(st, e.message);
            }
        }
    }
}

void writeSaved(const SavedStamp& saved, const fs::path& dir) {
    std::error_code ec;
    try {
        library::refuseInsideBundled(dir);  // before a folder is made there
    } catch (const library::LibraryError& e) {
        throw Failure{saved.step + ": " + e.what()};
    }
    if (!fs::is_directory(dir, ec) && !fs::create_directories(dir, ec))
        throw Failure{saved.step + ": " + dir.string() + ": " +
                      (ec ? ec.message() : std::string("not a directory"))};
    fs::path path = dir;
    try {
        fs::path folder = dir;
        if (!saved.collection.empty()) {
            // a stamp saved into a collection (#513) goes in its folder,
            // over the one of its title there, as Save to Library writes it
            library::refuseBundled(dir);
            const auto c = library::findCollection(dir, saved.collection);
            if (!c)
                throw library::LibraryError("no collection is called \"" + saved.collection + "\"");
            library::refuseBundled(c->dir);
            folder = c->dir;
            for (const library::Stamp& st : library::scan(folder).stamps)
                if (st.name == saved.name) {
                    path = st.path;
                    break;
                }
        }
        if (path == dir)
            path = library::stampPathFor(folder, saved.name).path;
        library::writeStamp(path, saved.files);
    } catch (const std::exception& e) {
        // the library's errors name the file or folder already
        const std::string why = e.what();
        throw Failure{saved.step + ": " + (why.find(dir.string()) == std::string::npos ? path.string() + ": " : "") +
                      why};
    }
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
    s.files = &script.files;
    s.presets = &script.presets;
    s.overwrites = &script.userOverwrites;
    s.collections = &script.userCollections;
    s.bundled = &script.bundledStamps;
    s.user = script.userStamps;
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
            observe({st, true, s.p, s.active, s.selection, set, s.view, s.saved});
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
            observe({st, false, s.p, s.active, s.selection, s, s.view, s.saved});
        out.captures.push_back({st.capture == steps::Capture::BeforeAfter ? stem + "-after" : stem, s.p});
    }
    out.saved = std::move(s.saved);
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
