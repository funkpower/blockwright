#include "blockwright/text.h"

#include "blockwright/project.h"
#include "blockwright/ramp.h"

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
    std::optional<Glyph> glyph(char32_t ch) const override { return face_.glyph(ch); }
    std::vector<ConfusablePair> confusables() const override { return face_.confusables(); }

private:
    const Face& face_;
    int extra_;
};

std::u32string casedText(const TextSource& source) {
    std::u32string text = source.text;
    if (source.textCase == TextCase::Capitals)
        for (char32_t& ch : text)
            if (ch >= U'a' && ch <= U'z')
                ch = ch - U'a' + U'A';
    return text;
}

// The size of the source's box in cells, measured without drawing it
// (measureText), in 64 bits.
struct Box {
    std::int64_t width = 0, height = 0;
};

Box measure(const TextSource& source, const Face& face) {
    const SpacedFace spaced(face, source.letterSpacing);
    const TextExtent e = measureText(spaced, casedText(source), {source.wrapWidth});
    return {std::max<std::int64_t>(source.wrapWidth, e.width),
            e.lines * (face.height() / 2) + std::max<std::int64_t>(0, e.lines - 1) * source.lineSpacing};
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

TextRender renderText(const TextSource& source, const Face& face) {
    // measured first, so nothing the size of a box too large is allocated
    const Box box = measure(source, face);
    if (const auto problem = boxProblem(box))
        throw TextError(*problem);
    const SpacedFace spaced(face, source.letterSpacing);
    const TextLayout layout = layoutText(spaced, casedText(source), {source.wrapWidth});
    const int lineRows = face.height() / 2;
    const int lines = static_cast<int>(layout.lines.size());
    const int width = static_cast<int>(box.width), height = static_cast<int>(box.height);
    TextRender out;
    out.missing = layout.missing;
    out.cells = Canvas(width, height);
    const Cell clear = transparentCell();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            out.cells.at(x, y) = clear;

    for (int l = 0; l < lines; ++l) {
        const int lineWidth = layout.lines[l].width;
        const int shift = source.align == TextAlign::Left    ? 0
                          : source.align == TextAlign::Right ? width - lineWidth
                                                             : (width - lineWidth) / 2;
        const int top = l * (lineRows + source.lineSpacing);
        for (int r = 0; r < lineRows; ++r)
            for (int x = 0; x < lineWidth; ++x) {
                const int py = l * face.height() + 2 * r;
                Cell& c = out.cells.at(x + shift, top + r);
                if (layout.cells) {
                    const Cell& sc = layout.cells->at(x, py / 2);
                    if (!sc.transparent) {
                        c = sc;
                        continue;
                    }
                }
                const bool upper = layout.mask.at(x, py), lower = layout.mask.at(x, py + 1);
                if (!upper && !lower)
                    continue;
                c = Cell{};
                c.ch = upper && lower ? kFullBlock : upper ? kUpperHalf : kLowerHalf;
                c.fg = Color::indexed(source.colour);
                c.bg = Color::indexed(Black);
            }
    }
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
    // a layer the project file holds, measured without drawing it
    return boxProblem(measure(source, *face));
}

namespace {

Canvas drawSource(const TextSource& source, const FontSources& fonts) {
    if (const auto problem = textSourceProblem(source, fonts))
        throw TextError(*problem);
    return renderText(source, *findFace(source.face, fonts)).cells;
}

} // namespace

Layer makeTextLayer(TextSource source, std::string name, int x, int y, const FontSources& fonts) {
    Layer l = makeLayer(LayerKind::Text, 0, 0, std::move(name), x, y);
    l.cells = drawSource(source, fonts);
    l.props.text = std::move(source);
    return l;
}

namespace {

// Gives the layer `props` (its id kept), drawing its cells from their source
// when `redraw`; refused before the layer is touched.
void put(Layer& layer, LayerProperties props, bool redraw, const FontSources& fonts) {
    props.id = layer.props.id;
    if (props.text && props.kind != LayerKind::Text)
        throw TextError("a " + std::string(layerKindName(props.kind)) + " layer has no text source");
    if (redraw && props.text)
        layer.cells = drawSource(*props.text, fonts);
    layer.props = std::move(props);
}

} // namespace

bool applyLayerProperties(Layer& layer, LayerProperties props, const FontSources& fonts) {
    const bool redraw = props.text && props.text != layer.props.text;
    put(layer, std::move(props), redraw, fonts);
    return redraw;
}

LayerPropsEdit propsEdit(const Layer& layer, LayerProperties after, std::uint64_t typingRun) {
    after.id = layer.props.id;
    LayerPropsEdit e{layer.props.id, layer.props, std::move(after), typingRun, std::nullopt};
    if (e.before.text != e.after.text)
        e.cellsBefore = layer.cells;
    return e;
}

LayerPropsEdit textEdit(const Layer& layer, TextSource source, std::uint64_t typingRun) {
    LayerProperties after = layer.props;
    after.text = std::move(source);
    return propsEdit(layer, std::move(after), typingRun);
}

LayerPropsEdit bakeEdit(const Layer& layer) {
    return propsEdit(layer, bake(Layer{layer.props, Canvas{0, 0}}).props);
}

bool applyEdit(Layer& layer, const LayerPropsEdit& edit, bool revert, const FontSources& fonts) {
    // a step that changes the source: redone, it draws its after (a run of
    // typing may end on the source it began with); undone, it puts back the
    // cells it replaced as they were
    if (!edit.cellsBefore) {
        put(layer, revert ? edit.before : edit.after, false, fonts);
        return false;
    }
    if (!revert) {
        put(layer, edit.after, true, fonts);
        return true;
    }
    put(layer, edit.before, false, fonts);
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
        !into.after.text || !next.after.text)
        return false;
    // into's cells before, if it kept any, are still the merged step's; if
    // it kept none it changed no cell, so next's are into's
    if (!into.cellsBefore)
        into.cellsBefore = next.cellsBefore;
    into.after = next.after;
    return true;
}

} // namespace bw
