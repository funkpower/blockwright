#include "blockwright/edit.h"

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/width.h"

#include <QList>
#include <QString>

#include <algorithm>
#include <climits>
#include <map>
#include <utility>
#include <vector>

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

CellEdit settleMarks(const Canvas& canvas, CellEdit edit) {
    if (edit.moves || std::none_of(edit.changes.begin(), edit.changes.end(), [&](const CellChange& c) {
            return c.before.lostBlink || c.after.lostBlink ||
                   (!edit.dithers && (c.before.dithered || c.after.dithered));
        }))
        return edit;
    using At = std::pair<int, int>;  // (y, x)
    std::map<At, Cell> was;  // the cell before the edit, its first change's before
    std::map<At, std::size_t> last;  // its last change
    for (std::size_t i = 0; i < edit.changes.size(); ++i) {
        const CellChange& c = edit.changes[i];
        was.emplace(At{c.y, c.x}, c.before);
        last[{c.y, c.x}] = i;
    }
    // A cell's right neighbour, before and after the edit: the edit's where
    // it changes it, the canvas's where it doesn't, none past the edge.
    const auto before = [&](int x, int y) -> Glyph {
        if (const auto it = was.find({y, x}); it != was.end())
            return it->second.ch;
        return canvas.contains(x, y) ? canvas.at(x, y).ch : Glyph();
    };
    const auto after = [&](int x, int y) -> Glyph {
        if (const auto it = last.find({y, x}); it != last.end())
            return edit.changes[it->second].after.ch;
        return canvas.contains(x, y) ? canvas.at(x, y).ch : Glyph();
    };
    for (CellChange& c : edit.changes) {
        const bool dithered = c.after.dithered;
        c.after.lostBlink = false;
        c.after.dithered = false;
        c.after = writtenOver(was.at({c.y, c.x}), c.after, before(c.x + 1, c.y), after(c.x + 1, c.y));
        if (edit.dithers)
            c.after.dithered = dithered;
    }
    // A pair is one glyph: its continuation takes its lead's marks.
    std::vector<CellChange> added;
    for (const auto& [at, i] : last) {
        const auto [y, x] = at;
        if (!edit.changes[i].after.ch.isContinuation() && after(x + 1, y).isContinuation() &&
            canvas.contains(x + 1, y)) {
            const Cell& lead = edit.changes[i].after;
            if (const auto it = last.find({y, x + 1}); it != last.end()) {
                Cell& cont = edit.changes[it->second].after;
                cont.lostBlink = lead.lostBlink;
                cont.dithered = lead.dithered;
            } else if (const Cell& cont = canvas.at(x + 1, y);
                       cont.lostBlink != lead.lostBlink || cont.dithered != lead.dithered) {
                Cell c = cont;
                c.lostBlink = lead.lostBlink;
                c.dithered = lead.dithered;
                added.push_back({x + 1, y, cont, c});
            }
        } else if (edit.changes[i].after.ch.isContinuation() && x > 0 && !last.contains({y, x - 1}) &&
                   canvas.contains(x - 1, y)) {
            const Cell& lead = canvas.at(x - 1, y);
            Cell& cont = edit.changes[i].after;
            cont.lostBlink = lead.lostBlink;
            cont.dithered = lead.dithered;
        }
    }
    edit.changes.insert(edit.changes.end(), added.begin(), added.end());
    // and a change the marks leave as it was is none
    std::erase_if(edit.changes, [](const CellChange& c) { return identical(c.before, c.after); });
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
                // own glyph left (recoloured or not): a space in its colours,
                // unless typing wrote it (a narrow cluster over its own base)
                if (onCanvasCont(x + 1, y) && !cont(canvas.at(x, y)) && here.ch == canvas.at(x, y).ch &&
                    edited(x + 1, y) && !cont(after(x + 1, y)) && !(edit.typed && last.contains({y, x})))
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

EditedCells::EditedCells(const CellEdit& edit, Under under) : under_(std::move(under)) {
    for (const CellChange& c : edit.changes)
        cells_[{c.y, c.x}] = c.after;
}

EditedCells::EditedCells(const Canvas& canvas, const CellEdit& edit)
    : EditedCells(edit, [&canvas](int x, int y) { return canvas.contains(x, y) ? &canvas.at(x, y) : nullptr; }) {}

const Cell* EditedCells::at(int x, int y) const {
    const auto it = cells_.find({y, x});
    return it != cells_.end() ? &it->second : under_(x, y);
}

std::optional<int> EditedCells::pairLead(int x, int y) const {
    const Cell* here = at(x, y);
    if (!here)
        return std::nullopt;
    if (here->ch.isContinuation()) {
        const Cell* left = x > INT_MIN ? at(x - 1, y) : nullptr;
        return left && !left->ch.isContinuation() ? std::optional<int>(x - 1) : std::nullopt;
    }
    const Cell* right = x < INT_MAX ? at(x + 1, y) : nullptr;
    return right && right->ch.isContinuation() ? std::optional<int>(x) : std::nullopt;
}

namespace {

bool isCont(const Canvas& canvas, int x, int y) { return canvas.contains(x, y) && canvas.at(x, y).ch.isContinuation(); }

// The column of the cluster holding column `x`: the lead when `x` is a
// continuation.
int leadOf(const Canvas& canvas, int x, int y) { return x > 0 && isCont(canvas, x, y) ? x - 1 : x; }

// The cells the cluster starting at `x` takes: two for a lead, else one.
int cellsAt(const Canvas& canvas, int x, int y) { return isCont(canvas, x + 1, y) ? 2 : 1; }

std::u32string toU32(const QString& s) {
    const QList<uint> ucs = s.toUcs4();
    return {ucs.begin(), ucs.end()};
}

std::u32string nfc(std::u32string_view cps) {
    return toU32(QString::fromUcs4(cps.data(), static_cast<qsizetype>(cps.size()))
                     .normalized(QString::NormalizationForm_C));
}

std::string utf8(std::u32string_view cps) {
    return QString::fromUcs4(cps.data(), static_cast<qsizetype>(cps.size())).toStdString();
}

// A cluster named for a notice: its code points, each one that doesn't show
// on its own (a mark) by its code point and set off by spaces.
std::string named(std::u32string_view cluster) {
    std::string out;
    bool lastNamed = false;
    for (const char32_t cp : cluster) {
        const bool n = unprintableCodePoint(cp);
        if (!out.empty() && (n || lastNamed))
            out += ' ';
        out += quotedGlyph(cp);
        lastNamed = n;
    }
    return out;
}

// The glyph a typed cluster is written as: its code point, or the pool's
// cluster, or past the pool's cap its base, counted in `*capped`.
Glyph glyphOf(std::u32string_view cluster, const TypingOptions& opts, int* capped) {
    if (cluster.size() == 1)
        return Glyph(cluster[0]);
    if (const std::optional<Glyph> g = clusterPool().intern(std::span(cluster.data(), cluster.size()), opts.poolCap))
        return *g;
    ++*capped;
    return Glyph(cluster[0]);
}

std::string cappedNotice(int capped) {
    return std::to_string(capped) + (capped == 1 ? " cluster" : " clusters") +
           " typed as " + (capped == 1 ? "its base" : "their base") + ": too many distinct clusters";
}

// The cells of `row` as the edit should leave them, as changes from the
// canvas's row `y`. The continuation of each lead in `whole` (the pairs the
// edit writes wide, or joins a mark to) is kept even when the canvas held
// that cell already, since the settle keeps a pair over an old one whole
// only when the edit writes both halves (PLAN §16, Editing). So is a lead
// whose continuation the row makes a space, written with the glyph it had
// (a narrow cluster typed over its own base), so the settle sees it typed
// rather than left. The edit is typed (CellEdit::typed).
CellEdit rowEdit(const Canvas& canvas, int y, const std::vector<Cell>& row, const std::vector<int>& whole = {}) {
    CellEdit e;
    e.typed = true;
    const int w = canvas.width();
    for (int x = 0; x < w; ++x) {
        const bool kept = x > 0 && row[x].ch.isContinuation() && !identical(row[x - 1], canvas.at(x - 1, y)) &&
                          std::find(whole.begin(), whole.end(), x - 1) != whole.end();
        const bool spaced = x + 1 < w && !row[x].ch.isContinuation() && !canvas.at(x, y).ch.isContinuation() &&
                            canvas.at(x + 1, y).ch.isContinuation() && !row[x + 1].ch.isContinuation();
        if (kept || spaced || !identical(row[x], canvas.at(x, y)))
            e.changes.push_back({x, y, canvas.at(x, y), row[x]});
    }
    return e;
}

std::vector<Cell> rowOf(const Canvas& canvas, int y) {
    std::vector<Cell> row(static_cast<size_t>(canvas.width()));
    for (int x = 0; x < canvas.width(); ++x)
        row[x] = canvas.at(x, y);
    return row;
}

// Clears the cluster at `x` (both cells of a pair) to `blank`.
CellEdit clearCluster(const Canvas& canvas, int x, int y, const Cell& blank) {
    if (!canvas.contains(x, y))
        return {};
    std::vector<Cell> row = rowOf(canvas, y);
    const int n = cellsAt(canvas, x, y);
    for (int i = x; i < x + n; ++i)
        row[i] = blank;
    return rowEdit(canvas, y, row);
}

} // namespace

Cursor moveCursor(const Canvas& canvas, Cursor c, int dx, int dy) {
    if (canvas.width() <= 0 || canvas.height() <= 0)
        return {};
    const int w = canvas.width(), h = canvas.height();
    // a step right from a lead goes past its continuation
    const int from = std::clamp(c.x, 0, w - 1), row = std::clamp(c.y, 0, h - 1);
    const int skip = dx > 0 && isCont(canvas, from + 1, row) ? 1 : 0;
    const int x = std::clamp(c.x + dx + skip, 0, w - 1), y = std::clamp(c.y + dy, 0, h - 1);
    return {leadOf(canvas, x, y), y};
}

bool atRowEnd(const Canvas& canvas, Cursor c) {
    const int w = canvas.width();
    return canvas.contains(c.x, c.y) && (c.x == w - 1 || (c.x == w - 2 && isCont(canvas, w - 1, c.y)));
}

bool isTypeable(char32_t ch) { return ch >= 0x20 && ch != 0x7f && isWritableGlyph(ch); }

bool isTypeable(std::u32string_view cluster, TypingPolicy policy) {
    if (cluster.empty() || cluster.size() > static_cast<size_t>(kMaxClusterCodePoints))
        return false;
    if (policy == TypingPolicy::Cp437)
        return cluster.size() == 1 && isTypeable(cluster[0]);
    return std::all_of(cluster.begin(), cluster.end(), [&](char32_t cp) {
        return cp >= 0x20 && cp != 0x7f && utf8Writable(cp) && (cluster.size() == 1 || clusterMayHold(cp));
    });
}

std::vector<std::u32string> splitClusters(std::u32string_view text) {
    std::vector<std::u32string> out;
    bool open = false;  // whether a zero-width code point joins the last
    for (const char32_t cp : text) {
        const int w = codePointWidth(cp);
        if (w == 0 && open) {
            out.back() += cp;
            continue;
        }
        out.emplace_back(1, cp);
        open = w >= 0;  // a control takes no mark
    }
    return out;
}

EditResult typeCluster(const Canvas& canvas, Cursor c, std::u32string_view cluster, const Pen& pen,
                       const TypingOptions& opts) {
    if (!canvas.contains(c.x, c.y) || cluster.empty())
        return {{}, c};
    if (codePointWidth(cluster[0]) == 0)
        return typeMarks(canvas, c, cluster, opts);
    if (!isTypeable(cluster, opts.policy))
        return {{}, c, std::u32string(cluster)};
    c.x = leadOf(canvas, c.x, c.y);
    const int w = canvas.width();
    const bool wide = codePointWidth(cluster[0]) == 2;
    if (wide && c.x == w - 1)
        return {{}, c, {}, utf8(cluster) + " doesn't fit on the last column; not typed"};
    int capped = 0;
    const Cell cell{glyphOf(cluster, opts, &capped), pen.fg, pen.bg, pen.attrs, false};
    std::vector<Cell> row = rowOf(canvas, c.y);
    row[c.x] = cell;
    Cursor next{std::min(c.x + 1, w - 1), c.y, c.x == w - 1};
    std::vector<int> whole;
    if (wide) {
        Cell cont = cell;
        cont.ch = Glyph::continuation();
        row[c.x + 1] = cont;
        next = c.x + 2 < w ? Cursor{c.x + 2, c.y} : Cursor{c.x, c.y, true};
        whole.push_back(c.x);
    } else if (isCont(canvas, c.x + 1, c.y)) {
        // over a lead, its continuation a space in its colours, as the
        // settle leaves it
        row[c.x + 1].ch = U' ';
    }
    EditResult r{rowEdit(canvas, c.y, row, whole), next};
    if (capped)
        r.notice = cappedNotice(capped);
    return r;
}

EditResult typeGlyph(const Canvas& canvas, Cursor c, char32_t ch, const Pen& pen, TypingPolicy policy) {
    return typeCluster(canvas, c, std::u32string_view(&ch, 1), pen, {policy});
}

EditResult typeMarks(const Canvas& canvas, Cursor c, std::u32string_view marks, const TypingOptions& opts) {
    if (!canvas.contains(c.x, c.y) || marks.empty())
        return {{}, c};
    c.x = leadOf(canvas, c.x, c.y);
    // the cluster under the cursor past the end, else the one just left of it
    const bool past = c.pastEnd && atRowEnd(canvas, c);
    if (!past && c.x == 0)
        return {{}, c, {}, named(marks) + " has no character before it to join; not typed"};
    const int x = past ? c.x : leadOf(canvas, c.x - 1, c.y);
    const Cell& target = canvas.at(x, c.y);
    std::u32string joined;
    for (const char32_t cp : codePoints(target.ch))
        joined += cp;
    joined += marks;
    joined = nfc(joined);
    if (joined.size() > static_cast<size_t>(kMaxClusterCodePoints))
        return {{}, c, {}, named(marks) + " would be past the 16 code points a cell holds; not typed"};
    if (!isTypeable(joined, opts.policy) || !clusterMayHold(joined[0]))
        return {{}, c, joined};
    int capped = 0;
    std::vector<Cell> row = rowOf(canvas, c.y);
    row[x].ch = glyphOf(joined, opts, &capped);
    // a pair keeps the width it was stored with, whatever its new base
    EditResult r{rowEdit(canvas, c.y, row, {x}), c};
    if (capped)
        r.notice = cappedNotice(capped);
    return r;
}

EditResult replaceText(const Canvas& canvas, Cursor c, int start, int length, std::u32string_view text,
                       const Pen& pen, const Cell& blank, const TypingOptions& opts) {
    if (!canvas.contains(c.x, c.y))
        return {{}, c};
    const std::vector<std::u32string> clusters = splitClusters(text);
    for (const std::u32string& k : clusters)
        if (!isTypeable(k, opts.policy))
            return {{}, c, k};
    if (!clusters.empty() && codePointWidth(clusters.front()[0]) == 0)
        return {{}, c, {}, named(clusters.front()) + " starts text with more after it; not typed"};
    c.x = leadOf(canvas, c.x, c.y);
    const int w = canvas.width();
    // Past the end, the range counts back from the column after the cluster
    // under the cursor; the range is counted in clusters, a pair being one.
    const bool past = c.pastEnd && atRowEnd(canvas, c);
    // A cluster off the row is a column, so a range off it is clamped as
    // before.
    long long from = past ? w : c.x;
    long long back = -static_cast<long long>(start);  // clusters back; forward when negative
    for (; back > 0 && from > 0; --back)
        from = leadOf(canvas, static_cast<int>(from) - 1, c.y);
    for (; back < 0 && from < w; ++back)
        from += cellsAt(canvas, static_cast<int>(from), c.y);
    from -= back;
    const int first = static_cast<int>(std::clamp(from, 0LL, static_cast<long long>(w)));
    long long len = std::max(length, 0);
    long long stop = from;
    if (stop < 0) {
        const long long off = std::min(len, -stop);
        stop += off;
        len -= off;
    }
    for (; len > 0 && stop < w; --len)
        stop += cellsAt(canvas, static_cast<int>(stop), c.y);
    const int last = static_cast<int>(std::clamp(stop, static_cast<long long>(first), static_cast<long long>(w)));

    std::vector<Cell> row = rowOf(canvas, c.y);
    std::fill(row.begin() + first, row.begin() + last, blank);
    // A cluster past the end goes on the row's last cluster as the commit
    // has left it, a pair's lead when a pair ends the row, as typed there.
    const auto lastLead = [&] {
        return w >= 2 && row[w - 1].ch.isContinuation() && !row[w - 2].ch.isContinuation() ? w - 2 : w - 1;
    };
    int next = first;  // where the next cluster starts, past the end at w
    int wrote = -1;    // the column of the last cluster written
    int capped = 0;
    std::u32string dropped;
    std::vector<int> whole;  // the leads of the pairs written
    for (const std::u32string& k : clusters) {
        const int x = next < w ? next : lastLead();
        const Cell cell{glyphOf(k, opts, &capped), pen.fg, pen.bg, pen.attrs, false};
        if (codePointWidth(k[0]) == 2) {
            if (x == w - 1) {
                dropped += k;
                continue;
            }
            row[x] = cell;
            row[x + 1] = cell;
            row[x + 1].ch = Glyph::continuation();
            whole.push_back(x);
            next = x + 2;
        } else {
            row[x] = cell;
            // over a lead, its continuation a space in its colours, as the
            // settle leaves it, a pair this commit wrote too
            if (x + 1 < w && row[x + 1].ch.isContinuation())
                row[x + 1].ch = U' ';
            next = x + 1;
        }
        wrote = x;
    }
    std::erase_if(whole, [&](int x) { return row[x].ch.isContinuation() || !row[x + 1].ch.isContinuation(); });

    Cursor end{std::min(next, w - 1), c.y};
    if (next >= w && wrote >= 0)
        end = {wrote, c.y, true};
    else if (wrote < 0 && first == last)
        end = c;  // nothing written or cleared: the cursor as it was, past the end too
    EditResult r{rowEdit(canvas, c.y, row, whole), end};
    if (!dropped.empty())
        r.notice = utf8(dropped) + " would start on the last column; dropped";
    if (capped)
        r.notice += (r.notice.empty() ? "" : "; ") + cappedNotice(capped);
    return r;
}

ClusterRange clusterRange(const Canvas& canvas, Cursor c, int utf16Start, int utf16Length) {
    const int length = std::max(utf16Length, 0);
    if (!canvas.contains(c.x, c.y))
        return {utf16Start, length};
    const int w = canvas.width();
    c.x = leadOf(canvas, c.x, c.y);
    const int anchor = c.pastEnd && atRowEnd(canvas, c) ? w : c.x;
    // the row's cluster boundaries, in UTF-16 units from column 0
    std::vector<long long> at{0};
    std::size_t origin = 0;  // the boundary the range counts from
    for (int x = 0; x < w; x += cellsAt(canvas, x, c.y)) {
        if (x == anchor)
            origin = at.size() - 1;
        long long units = 0;
        for (const char32_t cp : codePoints(canvas.at(x, c.y).ch))
            units += cp > 0xFFFF ? 2 : 1;
        at.push_back(at.back() + std::max(units, 1LL));
    }
    const long long n = static_cast<long long>(at.size()) - 1;
    if (anchor == w)
        origin = static_cast<std::size_t>(n);
    // a boundary's number, past either end a column a unit: the last at or
    // before `u`, or the first at or after it
    const auto before = [&](long long u) -> long long {
        if (u < 0)
            return u;
        if (u >= at.back())
            return n + (u - at.back());
        return std::upper_bound(at.begin(), at.end(), u) - at.begin() - 1;
    };
    const auto after = [&](long long u) -> long long {
        if (u <= 0)
            return u;
        if (u > at.back())
            return n + (u - at.back());
        return std::lower_bound(at.begin(), at.end(), u) - at.begin();
    };
    const long long from = at[origin] + utf16Start;
    const long long first = before(from);
    const long long last = std::max(first, after(from + length));
    const auto clamped = [](long long v) { return static_cast<int>(std::clamp<long long>(v, INT_MIN, INT_MAX)); };
    return {clamped(first - static_cast<long long>(origin)), clamped(length > 0 ? last - first : 0)};
}

EditResult backspace(const Canvas& canvas, Cursor c, const Cell& blank) {
    if (!canvas.contains(c.x, c.y))
        return {{}, c};
    c.x = leadOf(canvas, c.x, c.y);
    if (c.pastEnd && atRowEnd(canvas, c))
        return {clearCluster(canvas, c.x, c.y, blank), {c.x, c.y}};
    if (c.x <= 0)
        return {{}, {c.x, c.y}};
    const Cursor left{leadOf(canvas, c.x - 1, c.y), c.y};
    return {clearCluster(canvas, left.x, left.y, blank), left};
}

EditResult deleteCell(const Canvas& canvas, Cursor c, const Cell& blank) {
    if (!canvas.contains(c.x, c.y))
        return {{}, {c.x, c.y}};
    c.x = leadOf(canvas, c.x, c.y);
    return {clearCluster(canvas, c.x, c.y, blank), {c.x, c.y}};
}

} // namespace bw
