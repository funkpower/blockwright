#include "blockwright/brushes/guard.h"

#include <algorithm>

namespace bw {
namespace {

using Kind = lint::LadderFinding::Kind;

bool across(const lint::LadderFinding& f) { return f.nx != f.x; }

} // namespace

StrokeLint::StrokeLint(const Canvas& start, const Ramp& ramp)
    : ramp_(ramp), index_(start, ramp), kept_(start), rowWide_(size_t(start.height()), 0),
      columnWide_(size_t(start.width()), 0), rowSkips_(size_t(start.height())),
      columnSkips_(size_t(start.width())) {
    for (int y = 0; y < start.height(); ++y)
        rowSkips_[size_t(y)] = index_.skips(true, y);
    for (int x = 0; x < start.width(); ++x)
        columnSkips_[size_t(x)] = index_.skips(false, x);
    for (const auto& f : index_.findings()) {
        switch (f.kind) {
        case Kind::WideRung: {
            const bool a = across(f);
            const int rung = rungOf(ramp_, start.at(f.x, f.y)).value_or(-1);
            wide_[{a, a ? f.y : f.x, rung}].emplace_back(a ? f.x : f.y, a ? f.nx : f.ny);
            (a ? rowWide_[size_t(f.y)] : columnWide_[size_t(f.x)]) = 1;
            break;
        }
        case Kind::OneGlyph:
            oneGlyph_ = true;
            break;
        case Kind::Noise:
        case Kind::RungSkip:
            break;
        default:
            places_.insert(f.place());
        }
    }
    noise_ = index_.noise();
}

bool StrokeLint::atStart(const lint::LadderFinding& f, const Canvas& now) const {
    switch (f.kind) {
    case Kind::WideRung: {
        const bool a = across(f);
        const int rung = rungOf(ramp_, now.at(f.x, f.y)).value_or(-1);
        const auto it = wide_.find({a, a ? f.y : f.x, rung});
        if (it == wide_.end())
            return false;
        const int first = a ? f.x : f.y, last = a ? f.nx : f.ny;
        for (const auto& [b, e] : it->second)
            if (b <= last && first <= e)
                return true;
        return false;
    }
    case Kind::OneGlyph:
        return oneGlyph_;
    case Kind::Noise:
        return index_.noise() <= noise_;
    default:
        return places_.count(f.place()) != 0;
    }
}

void StrokeLint::revert() {
    if (pending_.empty())
        return;
    (void)index_.apply(kept_, pending_);
    pending_.clear();
    pendingCells_.clear();
}

std::vector<lint::LadderFinding> StrokeLint::judge(const Canvas& after,
                                                   const std::vector<std::pair<int, int>>& changed) {
    revert();
    std::vector<lint::LadderFinding> out;
    if (changed.empty())
        return out;
    // What the change adds to the kept canvas, of which the start had none.
    // The kept canvas adds nothing to the start, so that is all it adds,
    // but for a skip or a wide rung on a line the change reaches: one there
    // that matches or shares cells with a kept one isn't added to the kept
    // canvas, and may still have moved off the start's (a level or a run the
    // stroke slid along its line a dab at a time). Skips are read from the
    // lines the index judged again (the rows and columns within two of a
    // changed cell) against the start's; wide rungs from the lines through
    // a changed cell, where the start had a wide rung on them. Elsewhere the
    // kept canvas had none to slide.
    std::set<std::tuple<int, int, int, int>> seen;
    std::vector<lint::LadderFinding> rest;
    for (const auto& f : index_.apply(after, changed)) {
        if (f.kind == Kind::RungSkip || atStart(f, after))
            continue;
        if (f.kind == Kind::WideRung)
            seen.insert({f.x, f.y, f.nx, f.ny});
        rest.push_back(f);
    }
    std::vector<int> rows, columns, skipRows, skipColumns;
    for (const auto& [x, y] : changed) {
        if (x < 0 || y < 0 || x >= kept_.width() || y >= kept_.height())
            continue;
        if (rowWide_[size_t(y)])
            rows.push_back(y);
        if (columnWide_[size_t(x)])
            columns.push_back(x);
        for (int d = -2; d <= 2; ++d) {
            if (y + d >= 0 && y + d < kept_.height())
                skipRows.push_back(y + d);
            if (x + d >= 0 && x + d < kept_.width())
                skipColumns.push_back(x + d);
        }
    }
    for (auto* lines : {&rows, &columns, &skipRows, &skipColumns}) {
        std::sort(lines->begin(), lines->end());
        lines->erase(std::unique(lines->begin(), lines->end()), lines->end());
    }
    for (const bool a : {true, false})
        for (const int line : a ? skipRows : skipColumns)
            for (auto& f : index_.skipsAdded(a, line, (a ? rowSkips_ : columnSkips_)[size_t(line)]))
                out.push_back(std::move(f));
    out.insert(out.end(), rest.begin(), rest.end());
    for (const bool a : {true, false})
        for (const int line : a ? rows : columns)
            for (const auto& f : index_.wideRungs(a, line))
                if (!atStart(f, after) && seen.insert({f.x, f.y, f.nx, f.ny}).second)
                    out.push_back(f);
    pending_ = changed;
    pendingCells_.clear();
    for (const auto& [x, y] : changed)
        pendingCells_.push_back(x >= 0 && y >= 0 && x < after.width() && y < after.height() ? after.at(x, y) : Cell{});
    return out;
}

void StrokeLint::keep() {
    for (size_t i = 0; i < pending_.size(); ++i) {
        const auto [x, y] = pending_[i];
        if (x >= 0 && y >= 0 && x < kept_.width() && y < kept_.height())
            kept_.at(x, y) = pendingCells_[i];
    }
    pending_.clear();
    pendingCells_.clear();
}

} // namespace bw
