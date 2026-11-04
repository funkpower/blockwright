#include "blockwright/edit.h"

#include "blockwright/ansi.h"

#include <algorithm>
#include <map>
#include <utility>

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

Cell writtenOver(const Cell& under, Cell c, Glyph underRight, Glyph cRight) {
    c.lostBlink = c.lostBlink || (under.lostBlink && writesAlike(under, underRight, c, cRight));
    c.dithered = c.dithered || (under.dithered && under == c);
    return c;
}

CellEdit settleMarks(CellEdit edit) {
    if (edit.moves || std::none_of(edit.changes.begin(), edit.changes.end(), [&](const CellChange& c) {
            return c.before.lostBlink || c.after.lostBlink ||
                   (!edit.dithers && (c.before.dithered || c.after.dithered));
        }))
        return edit;
    std::map<std::pair<int, int>, Cell> was, now;  // (y, x) -> the cell before and after the edit
    for (const CellChange& c : edit.changes) {
        was.emplace(std::pair{c.y, c.x}, c.before);
        now[{c.y, c.x}] = c.after;
    }
    // A cell's right neighbour, before and after, as far as the edit knows
    // it: a cell it leaves alone counts as none, so a lead it doesn't touch
    // the continuation of is judged as a single cell.
    const auto right = [](const std::map<std::pair<int, int>, Cell>& cells, int x, int y) {
        const auto it = cells.find({y, x + 1});
        return it != cells.end() ? it->second.ch : Glyph();
    };
    for (CellChange& c : edit.changes) {
        const bool dithered = c.after.dithered;
        c.after.lostBlink = false;
        c.after.dithered = false;
        c.after = writtenOver(was.at({c.y, c.x}), c.after, right(was, c.x, c.y), right(now, c.x, c.y));
        if (edit.dithers)
            c.after.dithered = dithered;
    }
    return edit;
}

namespace {

// One half of a pair given the other's colours, attributes and marks.
void takeValues(Cell& c, const Cell& from) {
    c.fg = from.fg;
    c.bg = from.bg;
    c.attrs = from.attrs;
    c.lostBlink = from.lostBlink;
    c.dithered = from.dithered;
}

bool sameValues(const Cell& a, const Cell& b) {
    return a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs && a.lostBlink == b.lostBlink &&
           a.dithered == b.dithered;
}

} // namespace

CellEdit settleWide(const Canvas& canvas, CellEdit edit) {
    const auto cont = [](const Cell& c) { return c.ch.isContinuation(); };
    const auto onCanvasCont = [&](int x, int y) { return canvas.contains(x, y) && cont(canvas.at(x, y)); };
    if (std::none_of(edit.changes.begin(), edit.changes.end(), [&](const CellChange& c) {
            return cont(c.after) || onCanvasCont(c.x, c.y) || onCanvasCont(c.x + 1, c.y);
        }))
        return edit;

    // The cells as the edit leaves them, the last change to each winning,
    // and which of them it changes.
    std::map<std::pair<int, int>, std::size_t> last;  // (y, x) -> its last change
    for (std::size_t i = 0; i < edit.changes.size(); ++i)
        if (canvas.contains(edit.changes[i].x, edit.changes[i].y))
            last[{edit.changes[i].y, edit.changes[i].x}] = i;
    const auto edited = [&](int x, int y) {
        const auto it = last.find({y, x});
        return it != last.end() && !identical(edit.changes[it->second].after, canvas.at(x, y));
    };
    const auto after = [&](int x, int y) -> const Cell& {
        const auto it = last.find({y, x});
        return it != last.end() ? edit.changes[it->second].after : canvas.at(x, y);
    };

    // What the settle writes, judged on the edit's result before any of it.
    std::map<std::pair<int, int>, Cell> settle;
    const auto set = [&](int x, int y, const Cell& c) {
        if (!identical(c, after(x, y)))
            settle[{y, x}] = c;
    };
    const auto space = [](Cell c) {
        c.ch = U' ';
        return c;
    };
    // Each changed cell and its neighbours, by rows.
    std::map<int, std::vector<int>> region;
    for (const auto& [yx, i] : last)
        for (int x = yx.second - 1; x <= yx.second + 1; ++x)
            if (canvas.contains(x, yx.first))
                region[yx.first].push_back(x);
    for (auto& [y, xs] : region) {
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        for (const int x : xs) {
            const Cell& here = after(x, y);
            if (!cont(here)) {
                // a lead whose continuation the edit wrote a glyph on, its
                // own glyph left (recoloured or not): a space in its colours
                if (onCanvasCont(x + 1, y) && !cont(canvas.at(x, y)) && here.ch == canvas.at(x, y).ch &&
                    edited(x + 1, y) && !cont(after(x + 1, y)))
                    set(x, y, space(here));
                continue;
            }
            // a stray continuation: a space in its own colours
            if (x == 0 || cont(after(x - 1, y))) {
                if (edited(x, y) || edited(x - 1, y))
                    set(x, y, space(here));
                continue;
            }
            // the pair (x - 1, x)
            const bool leadEdited = edited(x - 1, y), contEdited = edited(x, y);
            if (!leadEdited && !contEdited)
                continue;
            const Cell& lead = after(x - 1, y);
            const bool wasPair = cont(canvas.at(x, y)) && !cont(canvas.at(x - 1, y));
            if (wasPair && lead.ch != canvas.at(x - 1, y).ch) {
                // its lead written over: the continuation a space in its
                // colours, unless the edit wrote a new pair there whole (a
                // continuation in the new lead's values)
                if (!last.contains({y, x}) || !sameValues(lead, here))
                    set(x, y, space(here));
            } else if (wasPair && !leadEdited) {
                // only the continuation recoloured: the lead takes it
                if (!sameValues(lead, here)) {
                    Cell l = lead;
                    takeValues(l, here);
                    set(x - 1, y, l);
                }
            } else if (!sameValues(lead, here)) {
                // the lead recoloured, both written, or a new pair: the
                // lead's values
                Cell r = here;
                takeValues(r, lead);
                set(x, y, r);
            }
        }
    }
    for (const auto& [yx, c] : settle) {
        if (const auto it = last.find(yx); it != last.end())
            edit.changes[it->second].after = c;
        else
            edit.changes.push_back({yx.second, yx.first, canvas.at(yx.second, yx.first), c});
    }
    return edit;
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
    if (canvas.contains(c.x, c.y) && !identical(canvas.at(c.x, c.y), after))
        e.changes.push_back({c.x, c.y, canvas.at(c.x, c.y), after});
    return e;
}

} // namespace

EditResult typeGlyph(const Canvas& canvas, Cursor c, char32_t ch, const Pen& pen) {
    if (!isTypeable(ch) || !canvas.contains(c.x, c.y))
        return {{}, c};
    Cursor next = moveCursor(canvas, c, 1, 0);
    next.pastEnd = c.x == canvas.width() - 1;
    return {setCell(canvas, c, Cell{ch, pen.fg, pen.bg, 0, false}), next};
}

EditResult replaceText(const Canvas& canvas, Cursor c, int start, int length, std::u32string_view text,
                       const Pen& pen, const Cell& blank) {
    if (!canvas.contains(c.x, c.y) || !std::all_of(text.begin(), text.end(), isTypeable))
        return {{}, c};
    const long long w = canvas.width();
    // Past the end, the range counts back from the column after the last.
    const bool past = c.pastEnd && c.x == w - 1;
    const long long from = c.x + (past ? 1 : 0) + static_cast<long long>(start);
    const long long first = std::clamp(from, 0LL, w);
    const long long last = std::clamp(from + std::max(length, 0), first, w);  // one past

    std::vector<Cell> row(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x)
        row[x] = canvas.at(x, c.y);
    std::fill(row.begin() + first, row.begin() + last, blank);
    long long x = std::min(first, w - 1);
    for (const char32_t ch : text) {
        row[x] = Cell{ch, pen.fg, pen.bg, 0, false};
        x = std::min(x + 1, w - 1);
    }

    const long long end = first + static_cast<long long>(text.size());
    EditResult r{{}, {static_cast<int>(std::min(end, w - 1)), c.y, !text.empty() && end >= w}};
    for (int i = 0; i < w; ++i)
        if (!identical(row[i], canvas.at(i, c.y)))
            r.edit.changes.push_back({i, c.y, canvas.at(i, c.y), row[i]});
    return r;
}

EditResult backspace(const Canvas& canvas, Cursor c, const Cell& blank) {
    if (c.pastEnd && c.x == canvas.width() - 1 && canvas.contains(c.x, c.y))
        return {setCell(canvas, c, blank), {c.x, c.y}};
    if (c.x <= 0 || !canvas.contains(c.x, c.y))
        return {{}, c};
    const Cursor left{c.x - 1, c.y};
    return {setCell(canvas, left, blank), left};
}

EditResult deleteCell(const Canvas& canvas, Cursor c, const Cell& blank) {
    return {setCell(canvas, c, blank), {c.x, c.y}};
}

} // namespace bw
