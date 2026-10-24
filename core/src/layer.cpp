#include "blockwright/layer.h"

#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"

#include <algorithm>
#include <array>
#include <climits>
#include <unordered_map>

namespace bw {

namespace {

constexpr std::array<std::pair<LayerKind, std::string_view>, 5> kKindNames = {{
    {LayerKind::Cells, "cells"},
    {LayerKind::Pixels, "pixels"},
    {LayerKind::Text, "text"},
    {LayerKind::Generator, "generator"},
    {LayerKind::Reference, "reference"},
}};
constexpr std::array<std::pair<LayerMode, std::string_view>, 3> kModeNames = {{
    {LayerMode::Normal, "normal"},
    {LayerMode::CharsOnly, "chars-only"},
    {LayerMode::ColoursOnly, "colours-only"},
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

// Clamps a 64-bit sum back into int (offsets near INT_MAX).
int clampInt(long long v) { return int(std::clamp<long long>(v, INT_MIN, INT_MAX)); }

} // namespace

std::string_view layerKindName(LayerKind kind) { return nameOf(kKindNames, kind); }
std::optional<LayerKind> layerKindFromName(std::string_view name) { return fromName(kKindNames, name); }
std::string_view layerModeName(LayerMode mode) { return nameOf(kModeNames, mode); }
std::optional<LayerMode> layerModeFromName(std::string_view name) { return fromName(kModeNames, name); }

CellRect intersect(CellRect a, CellRect b) {
    if (a.empty() || b.empty())
        return {};
    const long long x0 = std::max<long long>(a.x, b.x), y0 = std::max<long long>(a.y, b.y);
    const long long x1 = std::min<long long>(1LL * a.x + a.width, 1LL * b.x + b.width);
    const long long y1 = std::min<long long>(1LL * a.y + a.height, 1LL * b.y + b.height);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {int(x0), int(y0), clampInt(x1 - x0), clampInt(y1 - y0)};
}

CellRect unite(CellRect a, CellRect b) {
    if (a.empty())
        return b.empty() ? CellRect{} : b;
    if (b.empty())
        return a;
    const long long x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const long long x1 = std::max<long long>(1LL * a.x + a.width, 1LL * b.x + b.width);
    const long long y1 = std::max<long long>(1LL * a.y + a.height, 1LL * b.y + b.height);
    return {int(x0), int(y0), clampInt(x1 - x0), clampInt(y1 - y0)};
}

CellRect unite(CellRect a, CellRect b, CellRect clip) { return unite(intersect(a, clip), intersect(b, clip)); }

const Cell* Layer::cellAt(int cx, int cy) const {
    const long long lx = 1LL * cx - props.x, ly = 1LL * cy - props.y;
    if (lx < 0 || ly < 0 || lx >= width() || ly >= height())
        return nullptr;
    return &cells.at(int(lx), int(ly));
}

Cell transparentCell() {
    Cell c;
    c.transparent = true;
    return c;
}

Layer makeLayer(LayerKind kind, int width, int height, std::string name, int x, int y) {
    Layer l;
    l.props.kind = kind;
    l.props.name = std::move(name);
    l.props.x = x;
    l.props.y = y;
    l.cells = Canvas(width, height);
    const Cell t = transparentCell();
    for (int cy = 0; cy < l.cells.height(); ++cy)
        for (int cx = 0; cx < l.cells.width(); ++cx)
            l.cells.at(cx, cy) = t;
    return l;
}

Layer bake(Layer layer) {
    if (layer.props.kind == LayerKind::Text)
        layer.props.kind = LayerKind::Cells;
    return layer;
}

StrokeOptions layerStrokeOptions(const Layer& layer, StrokeOptions base) {
    base.lockTransparent = layer.props.alphaLocked && layer.props.kind != LayerKind::Pixels;
    return base;
}

bool layerPaintable(const Layer& layer, int cx, int cy) {
    const Cell* c = layer.cellAt(cx, cy);
    if (!c || layer.props.locked)
        return false;
    if (!layer.props.alphaLocked)
        return true;
    if (layer.props.kind == LayerKind::Pixels) {
        const LayerPixels p = layerPixels(*c);
        return p.top || p.bottom;
    }
    return !c->transparent;
}

bool layerPixelPaintable(const Layer& layer, int cx, int py) {
    // an arithmetic shift (C++20): a negative row floors
    const Cell* c = layer.cellAt(cx, py >> 1);
    if (!c || layer.props.locked)
        return false;
    if (!layer.props.alphaLocked)
        return true;
    const LayerPixels p = layerPixels(*c);
    return bool((py & 1) ? p.bottom : p.top);
}

LayerPixels layerPixels(const Cell& cell) {
    if (cell.transparent) {
        const std::uint8_t fg = cell.fg.paletteIndex();
        switch (cell.ch) {
        case kUpperHalf: return {fg, std::nullopt};
        case kLowerHalf: return {std::nullopt, fg};
        default: return {};
        }
    }
    if (const auto pair = halfBlocks(cell, true))
        return {pair->top, pair->bottom};
    const std::uint8_t bg = cell.bg.paletteIndex();
    return {bg, bg};
}

Cell pixelLayerCell(LayerPixels p) {
    if (p.top && p.bottom)
        return halfBlockCell(HalfPair{std::uint8_t(*p.top & 15), std::uint8_t(*p.bottom & 15)}, true);
    Cell c = transparentCell();
    if (p.top || p.bottom) {
        c.ch = p.top ? kUpperHalf : kLowerHalf;
        c.fg = Color::indexed(p.top ? *p.top : *p.bottom);
    }
    return c;
}

Cell setLayerPixel(const Cell& cell, bool bottom, std::optional<std::uint8_t> colour) {
    LayerPixels p = layerPixels(cell);
    (bottom ? p.bottom : p.top) = colour ? std::optional<std::uint8_t>(*colour & 15) : std::nullopt;
    return pixelLayerCell(p);
}

CellEdit toLayerEdit(const Layer& layer, const CellEdit& canvasEdit) {
    CellEdit out;
    // a cell named twice keeps its first place and takes its last change
    std::unordered_map<long long, std::size_t> seen;
    for (const CellChange& c : canvasEdit.changes) {
        const long long lx = 1LL * c.x - layer.props.x, ly = 1LL * c.y - layer.props.y;
        if (lx < 0 || ly < 0 || lx >= layer.width() || ly >= layer.height())
            continue;
        const auto [it, added] = seen.try_emplace(ly * layer.width() + lx, out.changes.size());
        if (added)
            // the layer's own cell, not what the tool saw there (the composite)
            out.changes.push_back({int(lx), int(ly), layer.cells.at(int(lx), int(ly)), c.after});
        else
            out.changes[it->second].after = c.after;
    }
    std::erase_if(out.changes, [](const CellChange& c) { return identical(c.after, c.before); });
    out.moves = canvasEdit.moves;
    return out;
}

CellEdit toCanvasEdit(const Layer& layer, const CellEdit& layerEdit) {
    CellEdit out;
    for (const CellChange& c : layerEdit.changes) {
        const long long cx = 1LL * c.x + layer.props.x, cy = 1LL * c.y + layer.props.y;
        if (cx < INT_MIN || cx > INT_MAX || cy < INT_MIN || cy > INT_MAX)
            continue;
        out.changes.push_back({int(cx), int(cy), c.before, c.after});
    }
    out.moves = layerEdit.moves;
    return out;
}

CellRect dirtyRect(const Layer& layer, const CellEdit& layerEdit) {
    CellRect r;
    for (const CellChange& c : toCanvasEdit(layer, layerEdit).changes)
        r = unite(r, CellRect{c.x, c.y, 1, 1});
    return r;
}

LayerStack::LayerStack(int width, int height) : w_(std::max(0, width)), h_(std::max(0, height)) {}

LayerStack LayerStack::fromCanvas(const Canvas& canvas, std::string name) {
    LayerStack s(canvas.width(), canvas.height());
    Layer l;
    l.props.name = std::move(name);
    l.cells = canvas;
    for (int y = 0; y < l.cells.height(); ++y)
        for (int x = 0; x < l.cells.width(); ++x)
            l.cells.at(x, y).transparent = false;
    s.add(std::move(l));
    return s;
}

void LayerStack::resize(int width, int height) {
    w_ = std::max(0, width);
    h_ = std::max(0, height);
}

LayerId LayerStack::add(Layer layer, std::optional<std::size_t> index) {
    if (layer.props.id == 0) {
        if (next_ == 0)
            throw LayerError("layer ids exhausted");
        layer.props.id = next_;
        if (find(next_))  // can't happen: next_ is above every id held
            throw LayerError("layer id " + std::to_string(next_) + " is already in the stack");
    } else if (find(layer.props.id)) {
        throw LayerError("layer id " + std::to_string(layer.props.id) + " is already in the stack");
    }
    const LayerId id = layer.props.id;
    // ids given later stay above every id held; once the last id is taken
    // next_ is 0 for good (id + 1 wraps to it), so nothing is given again
    if (next_ != 0 && id >= next_)
        next_ = id + 1;
    const std::size_t at = std::min(index.value_or(layers_.size()), layers_.size());
    layers_.insert(layers_.begin() + std::ptrdiff_t(at), std::move(layer));
    return id;
}

void LayerStack::reserveIds(LayerId next) {
    if (next_ != 0 && (next == 0 || next > next_))
        next_ = next;
}

std::optional<Layer> LayerStack::remove(LayerId id) {
    const auto i = indexOf(id);
    if (!i)
        return std::nullopt;
    Layer l = std::move(layers_[*i]);
    layers_.erase(layers_.begin() + std::ptrdiff_t(*i));
    return l;
}

bool LayerStack::move(LayerId id, std::size_t index) {
    const auto i = indexOf(id);
    if (!i)
        return false;
    const std::size_t to = std::min(index, layers_.size() - 1);
    if (to == *i)
        return true;
    Layer l = std::move(layers_[*i]);
    layers_.erase(layers_.begin() + std::ptrdiff_t(*i));
    layers_.insert(layers_.begin() + std::ptrdiff_t(to), std::move(l));
    return true;
}

Layer* LayerStack::find(LayerId id) {
    const auto i = indexOf(id);
    return i ? &layers_[*i] : nullptr;
}

const Layer* LayerStack::find(LayerId id) const {
    const auto i = indexOf(id);
    return i ? &layers_[*i] : nullptr;
}

std::optional<std::size_t> LayerStack::indexOf(LayerId id) const {
    if (id == 0)
        return std::nullopt;
    for (std::size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].props.id == id)
            return i;
    return std::nullopt;
}

} // namespace bw
