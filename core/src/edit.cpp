#include "blockwright/edit.h"

#include "blockwright/ansi.h"

#include <algorithm>

namespace bw {

void CellEdit::apply(Canvas& canvas) const {
    for (const CellChange& c : changes)
        if (canvas.contains(c.x, c.y))
            canvas.at(c.x, c.y) = c.after;
}

void CellEdit::revert(Canvas& canvas) const {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        if (canvas.contains(it->x, it->y))
            canvas.at(it->x, it->y) = it->before;
}

Cursor moveCursor(const Canvas& canvas, Cursor c, int dx, int dy) {
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return {};
    return {std::clamp(c.x + dx, 0, canvas.width() - 1), std::clamp(c.y + dy, 0, canvas.height() - 1)};
}

bool isTypeable(char32_t ch) { return ch >= 0x20 && ch != 0x7f && isWritableGlyph(ch); }

namespace {

// An edit setting one cell, or none if the cell already holds `after`.
CellEdit setCell(const Canvas& canvas, Cursor c, const Cell& after) {
    CellEdit e;
    if (canvas.contains(c.x, c.y) && canvas.at(c.x, c.y) != after)
        e.changes.push_back({c.x, c.y, canvas.at(c.x, c.y), after});
    return e;
}

} // namespace

EditResult typeGlyph(const Canvas& canvas, Cursor c, char32_t ch, const Pen& pen) {
    if (!isTypeable(ch) || !canvas.contains(c.x, c.y))
        return {{}, c};
    return {setCell(canvas, c, Cell{ch, pen.fg, pen.bg, 0, false}), moveCursor(canvas, c, 1, 0)};
}

EditResult replaceText(const Canvas& canvas, Cursor c, int start, int length, std::u32string_view text,
                       const Pen& pen) {
    if (!canvas.contains(c.x, c.y) || !std::all_of(text.begin(), text.end(), isTypeable))
        return {{}, c};
    const long long w = canvas.width();
    const long long from = c.x + static_cast<long long>(start);
    const long long first = std::clamp(from, 0LL, w);
    const long long last = std::clamp(from + std::max(length, 0), first, w);  // one past

    std::vector<Cell> row(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x)
        row[x] = canvas.at(x, c.y);
    std::fill(row.begin() + first, row.begin() + last, Cell{});
    long long x = std::min(first, w - 1);
    for (const char32_t ch : text) {
        row[x] = Cell{ch, pen.fg, pen.bg, 0, false};
        x = std::min(x + 1, w - 1);
    }

    EditResult r{{}, {static_cast<int>(std::min(first + static_cast<long long>(text.size()), w - 1)), c.y}};
    for (int i = 0; i < w; ++i)
        if (row[i] != canvas.at(i, c.y))
            r.edit.changes.push_back({i, c.y, canvas.at(i, c.y), row[i]});
    return r;
}

EditResult backspace(const Canvas& canvas, Cursor c) {
    if (c.x <= 0 || !canvas.contains(c.x, c.y))
        return {{}, c};
    const Cursor left{c.x - 1, c.y};
    return {setCell(canvas, left, Cell{}), left};
}

EditResult deleteCell(const Canvas& canvas, Cursor c) { return {setCell(canvas, c, Cell{}), c}; }

} // namespace bw
