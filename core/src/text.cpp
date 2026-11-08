#include "blockwright/text.h"
#include "blockwright/big_text.h"

#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/style_draw.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace bw {

namespace {

constexpr std::array<std::pair<FaceRef::Kind, std::string_view>, 2> kFaceKindNames = {{
    {FaceRef::Kind::Pixel, "pixel"},
    {FaceRef::Kind::Tdf, "tdf"},
}};
constexpr std::array<std::pair<TextCase, std::string_view>, 2> kCaseNames = {{
    {TextCase::AsTyped, "as-typed"},
    {TextCase::Capitals, "capitals"},
}};
constexpr std::array<std::pair<TextAlign, std::string_view>, 3> kAlignNames = {{
    {TextAlign::Left, "left"},
    {TextAlign::Centre, "centre"},
    {TextAlign::Right, "right"},
}};

template <typename E, std::size_t N>
std::string_view nameOf(const std::array<std::pair<E, std::string_view>, N>& names, E e) {
    for (const auto& [v, n] : names)
        if (v == e)
            return n;
    return {};
}

template <typename E, std::size_t N>
std::optional<E> fromName(const std::array<std::pair<E, std::string_view>, N>& names, std::string_view s) {
    for (const auto& [v, n] : names)
        if (n == s)
            return v;
    return std::nullopt;
}

// A face with extra blank columns between its letters, so layoutText wraps
// with the spacing the source asks for.
class SpacedFace final : public Face {
public:
    SpacedFace(const Face& face, int extra) : face_(face), extra_(extra) {}
    std::string_view name() const override { return face_.name(); }
    int height() const override { return face_.height(); }
    int letterSpacing() const override { return face_.letterSpacing() + extra_; }
    std::optional<FaceGlyph> glyph(char32_t ch) const override { return face_.glyph(ch); }
    std::vector<ConfusablePair> confusables() const override { return face_.confusables(); }

private:
    const Face& face_;
    int extra_;
};

// The size of the source's box in cells, measured without drawing it
// (measureText), in 64 bits.
struct Box {
    std::int64_t width = 0, height = 0;
};

Box measure(const TextSource& source, const Face& face) {
    if (source.bigStyle != BigTextStyle::None) {
        const auto e = measureBigText(source);
        return {e.width, e.height};
    }
    const SpacedFace spaced(face, source.letterSpacing);
    const TextExtent e = measureText(spaced, casedText(source), {source.wrapWidth});
    return {std::max<std::int64_t>(source.wrapWidth, e.width),
            e.lines * (face.height() / 2) + std::max<std::int64_t>(0, e.lines - 1) * source.lineSpacing};
}

// The box grown by a style's margins.
Box grown(Box box, const TextStyle* style) {
    if (style) {
        const StyleMargins m = styleMargins(*style);
        box.width += m.left + m.right;
        box.height += m.top + m.bottom;
    }
    return box;
}

// Why a box is larger than a layer the project file holds (project.h's
// limits), or nothing.
std::optional<std::string> boxProblem(const Box& box) {
    if (box.width > 65535 || box.height > 65535 ||
        static_cast<std::uint64_t>(box.width) * static_cast<std::uint64_t>(box.height) > project::kMaxLayerCells)
        return "the text draws " + std::to_string(box.width) + "x" + std::to_string(box.height) +
               " cells, over a layer's " + std::to_string(project::kMaxLayerCells);
    return std::nullopt;
}

} // namespace

bool typedInText(char32_t ch) { return ch >= 0x20 && !(ch >= 0x7f && ch <= 0x9f); }

std::u32string casedText(const TextSource& source) {
    std::u32string text = source.text;
    if (source.textCase == TextCase::Capitals)
        for (char32_t& ch : text)
            if (ch >= U'a' && ch <= U'z')
                ch = ch - U'a' + U'A';
    return text;
}

std::string_view faceKindName(FaceRef::Kind kind) { return nameOf(kFaceKindNames, kind); }
std::optional<FaceRef::Kind> faceKindFromName(std::string_view name) { return fromName(kFaceKindNames, name); }
std::string_view textCaseName(TextCase c) { return nameOf(kCaseNames, c); }
std::optional<TextCase> textCaseFromName(std::string_view name) { return fromName(kCaseNames, name); }
std::string_view textAlignName(TextAlign a) { return nameOf(kAlignNames, a); }
std::optional<TextAlign> textAlignFromName(std::string_view name) { return fromName(kAlignNames, name); }

const Face* findFace(const FaceRef& ref, const FontSources& fonts) {
    switch (ref.kind) {
    case FaceRef::Kind::Pixel: return bundledFace(ref.name);
    case FaceRef::Kind::Tdf: return fonts.face(ref);
    }
    return nullptr;
}

namespace {

// The source laid out in its box, as a style reads it (style_draw.h): the
// letter mask, the ink pixels in the source's colour, the face's source
// cells and the lines. The box is checked first (with `style`'s margins),
// so nothing the size of a box too large is allocated.
struct Laid {
    StyleInput in;
    std::vector<std::size_t> missing;
};

Laid laidOut(const TextSource& source, const Face& face, const TextStyle* style) {
    const Box box = measure(source, face);
    if (const auto problem = boxProblem(grown(box, style)))
        throw TextError(*problem);
    if (source.bigStyle != BigTextStyle::None) {
        const auto big = renderBigText(source);
        Laid out;
        out.missing = big.drawing.missing;
        Layer layer = makeLayer(LayerKind::Cells, 0, 0);
        layer.cells = big.drawing.cells;
        out.in = styleInputOf(layer);
        out.in.box = CellRect{0, 0, layer.width(), layer.height()};
        out.in.bands.clear();
        for (std::size_t i = 0; i < big.lines.size(); ++i)
            out.in.bands.emplace_back(2 * std::max(0, big.lines[i].top),
                                      i + 1 < big.lines.size() ? 2 * std::max(0, big.lines[i + 1].top)
                                                               : 2 * layer.cells.height());
        return out;
    }
    const SpacedFace spaced(face, source.letterSpacing);
    const TextLayout layout = layoutText(spaced, casedText(source), {source.wrapWidth});
    const int lineRows = face.height() / 2;
    const int lines = static_cast<int>(layout.lines.size());
    const int width = static_cast<int>(box.width), height = static_cast<int>(box.height);
    Laid out;
    out.missing = layout.missing;
    out.in.mask = InkMask(width, 2 * height);
    out.in.letters.assign(out.in.mask.ink.size(), 0);
    out.in.pixels = makeLayer(LayerKind::Pixels, width, height).cells;
    out.in.source = out.in.pixels;
    out.in.box = CellRect{0, 0, width, height};
    const std::uint8_t colour = source.colour;
    for (int l = 0; l < lines; ++l) {
        const int lineWidth = layout.lines[l].width;
        const int shift = source.align == TextAlign::Left    ? 0
                          : source.align == TextAlign::Right ? width - lineWidth
                                                             : (width - lineWidth) / 2;
        const int top = l * (lineRows + source.lineSpacing);
        out.in.bands.emplace_back(2 * top, 2 * (top + lineRows));
        for (int r = 0; r < lineRows; ++r)
            for (int x = 0; x < lineWidth; ++x) {
                const int py = l * face.height() + 2 * r;
                const int cx = x + shift, cy = top + r;
                const bool upper = layout.mask.at(x, py), lower = layout.mask.at(x, py + 1);
                if (upper) {
                    out.in.mask.set(cx, 2 * cy);
                    out.in.letters[static_cast<std::size_t>(2 * cy) * width + cx] =
                        layout.glyphs[static_cast<std::size_t>(py) * layout.mask.width + x];
                }
                if (lower) {
                    out.in.mask.set(cx, 2 * cy + 1);
                    out.in.letters[static_cast<std::size_t>(2 * cy + 1) * width + cx] =
                        layout.glyphs[static_cast<std::size_t>(py + 1) * layout.mask.width + x];
                }
                if (layout.cells) {
                    const Cell& sc = layout.cells->at(x, py / 2);
                    if (!sc.transparent) {
                        out.in.source.at(cx, cy) = sc;
                        continue;
                    }
                }
                Cell& pc = out.in.pixels.at(cx, cy);
                if (upper)
                    pc = setLayerPixel(pc, false, colour);
                if (lower)
                    pc = setLayerPixel(pc, true, colour);
            }
    }
    return out;
}

} // namespace

TextRender renderText(const TextSource& source, const Face& face) {
    if (source.bigStyle != BigTextStyle::None)
        return renderBigText(source).drawing;
    Laid laid = laidOut(source, face, nullptr);
    const StyleInput& in = laid.in;
    const int width = in.mask.width, height = in.mask.height / 2;
    TextRender out;
    out.missing = std::move(laid.missing);
    out.cells = Canvas(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            Cell& c = out.cells.at(x, y);
            if (!in.source.at(x, y).transparent) {
                c = in.source.at(x, y);
                continue;
            }
            const LayerPixels p = layerPixels(in.pixels.at(x, y));
            if (!p.top && !p.bottom) {
                c = transparentCell();
                continue;
            }
            c = Cell{};
            c.ch = p.top && p.bottom ? kFullBlock : p.top ? kUpperHalf : kLowerHalf;
            c.fg = Color::indexed(source.colour);
            c.bg = Color::indexed(Black);
        }
    return out;
}

StyleInput textStyleInput(const TextSource& source, const Face& face) {
    return laidOut(source, face, nullptr).in;
}

TextRender renderStyledText(const TextSource& source, const TextStyle& style, const Face& face, bool ice) {
    if (const auto problem = textStyleProblem(style))
        throw TextError(*problem);
    Laid laid = laidOut(source, face, &style);
    TextRender out;
    out.missing = std::move(laid.missing);
    out.cells = drawStyle(laid.in, style, ice, styleMargins(style));
    return out;
}

std::optional<std::string> textSourceProblem(const TextSource& source, const FontSources& fonts) {
    const Face* face = findFace(source.face, fonts);
    if (!face && source.face.kind == FaceRef::Kind::Tdf) {
        const std::string font = "TheDraw font \"" + source.face.name + "\" (font " +
                                 std::to_string(source.face.index) + " of fonts/" + source.face.file + ".tdf)";
        if (fonts.file(source.face.file))
            return "no " + font + ": the file has no such font";
        return "no " + font + " in the project or the fonts folder";
    }
    if (!face)
        return "no " + std::string(faceKindName(source.face.kind)) + " face \"" + source.face.name + "\"";
    if (source.text.size() > TextSource::kMaxLength)
        return "the text is " + std::to_string(source.text.size()) + " characters, over " +
               std::to_string(TextSource::kMaxLength);
    const auto within = [](int v, int hi) { return v >= 0 && v <= hi; };
    if (!within(source.letterSpacing, TextSource::kMaxSpacing))
        return "letter spacing " + std::to_string(source.letterSpacing) + " is not 0 to " +
               std::to_string(TextSource::kMaxSpacing);
    if (!within(source.lineSpacing, TextSource::kMaxSpacing))
        return "line spacing " + std::to_string(source.lineSpacing) + " is not 0 to " +
               std::to_string(TextSource::kMaxSpacing);
    if (!within(source.wrapWidth, TextSource::kMaxWrapWidth))
        return "wrap width " + std::to_string(source.wrapWidth) + " is not 0 to " +
               std::to_string(TextSource::kMaxWrapWidth);
    if (source.colour > 15)
        return "colour " + std::to_string(source.colour) + " is not a palette index 0 to 15";
    if (bigTextStyleName(source.bigStyle).empty())
        return "unknown big text style";
    if (source.bigColours.size() > TextSource::kMaxBigColours)
        return "too many big text colours";
    for (const auto c : source.bigColours)
        if (c > 15)
            return "big text colour is not mIRC 0 to 15";
    if (source.bigStyle != BigTextStyle::None &&
        (source.face.kind != FaceRef::Kind::Pixel || source.face.name != "block"))
        return "a big text style needs the block face";
    // a layer the project file holds, measured without drawing it
    return boxProblem(measure(source, *face));
}

std::optional<std::string> styledTextProblem(const TextSource& source, const TextStyle& style,
                                             const FontSources& fonts) {
    if (auto problem = textSourceProblem(source, fonts))
        return problem;
    if (auto problem = textStyleProblem(style))
        return problem;
    return boxProblem(grown(measure(source, *findFace(source.face, fonts)), &style));
}

std::optional<std::string> layerStyleProblem(const LayerProperties& props, const FontSources& fonts) {
    if (!props.style)
        return std::nullopt;
    if (props.kind == LayerKind::Text ? !props.text
                                      : props.kind != LayerKind::Cells && props.kind != LayerKind::Pixels)
        return "a " + std::string(layerKindName(props.kind)) + " layer" +
               (props.kind == LayerKind::Text ? " without a text source" : "") + " has no style";
    if (props.text)
        return styledTextProblem(*props.text, *props.style, fonts);
    return textStyleProblem(*props.style);
}

namespace {

Canvas drawSource(const TextSource& source, const TextStyle* style, bool ice, const FontSources& fonts) {
    if (const auto problem = style ? styledTextProblem(source, *style, fonts) : textSourceProblem(source, fonts))
        throw TextError(*problem);
    const Face& face = *findFace(source.face, fonts);
    return style ? renderStyledText(source, *style, face, ice).cells : renderText(source, face).cells;
}

} // namespace

Layer makeTextLayer(TextSource source, std::string name, int x, int y, const FontSources& fonts) {
    Layer l = makeLayer(LayerKind::Text, 0, 0, std::move(name), x, y);
    l.cells = drawSource(source, nullptr, true, fonts);
    l.props.text = std::move(source);
    return l;
}

const Layer* textLayerAt(const LayerStack& stack, int x, int y) {
    const std::vector<Layer>& layers = stack.layers();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
        if (it->props.text && it->props.visible && !it->props.locked && it->bounds().contains(x, y))
            return &*it;
    return nullptr;
}

namespace {

// Gives the layer `props` (its id kept), drawing its cells from their source
// when `redraw`; refused before the layer is touched.
void put(Layer& layer, LayerProperties props, bool redraw, bool ice, const FontSources& fonts) {
    props.id = layer.props.id;
    if (props.text && props.kind != LayerKind::Text)
        throw TextError("a " + std::string(layerKindName(props.kind)) + " layer has no text source");
    if (const auto problem = layerStyleProblem(props, fonts))
        throw TextError(*problem);
    if (redraw && props.text)
        layer.cells = drawSource(*props.text, props.style ? &*props.style : nullptr, ice, fonts);
    layer.props = std::move(props);
}

// Whether a step from `before` to `after` draws the cells again: a live
// text layer's source or style changes.
bool redraws(const LayerProperties& before, const LayerProperties& after) {
    return after.text && (after.text != before.text || after.style != before.style);
}

} // namespace

bool applyLayerProperties(Layer& layer, LayerProperties props, const FontSources& fonts, bool ice) {
    const bool redraw = redraws(layer.props, props);
    put(layer, std::move(props), redraw, ice, fonts);
    return redraw;
}

LayerPropsEdit propsEdit(const Layer& layer, LayerProperties after, std::uint64_t typingRun, bool ice) {
    after.id = layer.props.id;
    LayerPropsEdit e{layer.props.id, layer.props, std::move(after), typingRun, std::nullopt, std::nullopt, ice};
    if (e.before.text != e.after.text || redraws(e.before, e.after))
        e.cellsBefore = layer.cells;
    return e;
}

LayerPropsEdit textEdit(const Layer& layer, TextSource source, std::uint64_t typingRun, bool ice) {
    LayerProperties after = layer.props;
    after.text = std::move(source);
    return propsEdit(layer, std::move(after), typingRun, ice);
}

LayerPropsEdit styleEdit(const Layer& layer, std::optional<TextStyle> style, bool ice) {
    LayerProperties after = layer.props;
    after.style = std::move(style);
    return propsEdit(layer, std::move(after), 0, ice);
}

LayerPropsEdit bakeEdit(const Layer& layer, bool ice) {
    if (!styledAtComposite(layer.props))
        return propsEdit(layer, bake(Layer{layer.props, Canvas{0, 0}}).props, 0, ice);
    // the cells change to the styled ones, which the compositor drew from
    // the cells and the style: kept both ways
    LayerProperties after = layer.props;
    if (after.kind == LayerKind::Pixels)
        after.mode = LayerMode::Normal;  // what a styled pixel layer composites in
    after.kind = LayerKind::Cells;
    after.style.reset();
    LayerPropsEdit e = propsEdit(layer, std::move(after), 0, ice);
    e.cellsBefore = layer.cells;
    e.cellsAfter = styledCells(layer, ice);
    return e;
}

std::optional<LayerPropsEdit> iceEdit(const Layer& layer, bool ice, const FontSources& fonts) {
    if (!layer.props.text || !layer.props.style)
        return std::nullopt;
    Canvas after = drawSource(*layer.props.text, &*layer.props.style, ice, fonts);
    if (after == layer.cells)
        return std::nullopt;
    return LayerPropsEdit{layer.props.id, layer.props, layer.props, 0, layer.cells, std::move(after), ice};
}

bool applyEdit(Layer& layer, const LayerPropsEdit& edit, bool revert, const FontSources& fonts) {
    // a step that gives the cells after it (Bake of a style the compositor
    // draws) puts them, or those before it, in place as they were
    if (edit.cellsAfter && edit.cellsBefore) {
        put(layer, revert ? edit.before : edit.after, false, edit.ice, fonts);
        layer.cells = revert ? *edit.cellsBefore : *edit.cellsAfter;
        return true;
    }
    // a step that changes the source or a live text layer's style: redone,
    // it draws its after (a run of typing may end on the source it began
    // with); undone, it puts back the cells it replaced as they were
    if (!edit.cellsBefore) {
        put(layer, revert ? edit.before : edit.after, false, edit.ice, fonts);
        return false;
    }
    if (!revert) {
        put(layer, edit.after, true, edit.ice, fonts);
        return true;
    }
    put(layer, edit.before, false, edit.ice, fonts);
    layer.cells = *edit.cellsBefore;
    return true;
}

CellRect applyEdit(LayerStack& stack, const LayerPropsEdit& edit, bool revert, const FontSources& fonts) {
    Layer* l = stack.find(edit.id);
    if (!l)
        throw LayerError("no layer " + std::to_string(edit.id) + " in the stack");
    const CellRect before = l->bounds();
    applyEdit(*l, edit, revert, fonts);
    return unite(before, l->bounds(), stack.canvasRect());
}

bool mergeEdits(LayerPropsEdit& into, const LayerPropsEdit& next) {
    if (into.typingRun == 0 || next.typingRun != into.typingRun || next.id != into.id || next.before != into.after ||
        !into.after.text || !next.after.text || into.cellsAfter || next.cellsAfter || into.ice != next.ice)
        return false;
    // into's cells before, if it kept any, are still the merged step's; if
    // it kept none it changed no cell, so next's are into's
    if (!into.cellsBefore)
        into.cellsBefore = next.cellsBefore;
    into.after = next.after;
    return true;
}

} // namespace bw
