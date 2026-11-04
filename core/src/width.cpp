#include "blockwright/width.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace bw {

namespace {

struct WidthRun {
    std::uint32_t first, last;
    int width;
};

struct VariesRun {
    std::uint32_t first, last;
};

#include "width.inc"

constexpr bool sortedAndDisjoint() {
    for (std::size_t i = 1; i < std::size(kWidthRuns); ++i)
        if (kWidthRuns[i].first <= kWidthRuns[i - 1].last || kWidthRuns[i].first > kWidthRuns[i].last)
            return false;
    return true;
}
static_assert(sortedAndDisjoint());

constexpr bool variesSortedAndDisjoint() {
    for (std::size_t i = 1; i < std::size(kWidthVariesRuns); ++i)
        if (kWidthVariesRuns[i].first <= kWidthVariesRuns[i - 1].last ||
            kWidthVariesRuns[i].first > kWidthVariesRuns[i].last)
            return false;
    return true;
}
static_assert(variesSortedAndDisjoint());

// Box drawing, block elements and the geometric shapes before U+25FD, the
// glyphs ANSI art is mostly made of: one column each, looked up without the
// search. No run of the table may touch the range (a test checks each code
// point there too), so the fast path can never give another width than the
// table does.
constexpr char32_t kNarrowFirst = 0x2500, kNarrowLast = 0x25FC;

constexpr bool narrowRangeOutsideRuns() {
    for (const WidthRun& r : kWidthRuns)
        if (r.first <= kNarrowLast && r.last >= kNarrowFirst)
            return false;
    return true;
}
static_assert(narrowRangeOutsideRuns());

} // namespace

int codePointWidth(char32_t cp) {
    if (cp >= 0x20 && cp < 0x7F)
        return 1;
    if (cp >= kNarrowFirst && cp <= kNarrowLast)
        return 1;
    if (cp > 0x10FFFF)
        return -1;
    const auto it = std::upper_bound(std::begin(kWidthRuns), std::end(kWidthRuns), std::uint32_t(cp),
                                     [](std::uint32_t c, const WidthRun& r) { return c < r.first; });
    if (it == std::begin(kWidthRuns))
        return 1;
    const WidthRun& r = *(it - 1);
    return cp <= r.last ? r.width : 1;
}

const char* widthUnicodeVersion() { return kWidthUnicodeVersion; }

bool widthVaries(char32_t cp) {
    const auto it = std::upper_bound(std::begin(kWidthVariesRuns), std::end(kWidthVariesRuns), std::uint32_t(cp),
                                     [](std::uint32_t c, const VariesRun& r) { return c < r.first; });
    return it != std::begin(kWidthVariesRuns) && cp <= (it - 1)->last;
}

const char* widthVariesFloor() { return kWidthVariesFloor; }

} // namespace bw
