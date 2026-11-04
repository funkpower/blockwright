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

#include "width.inc"

constexpr bool sortedAndDisjoint() {
    for (std::size_t i = 1; i < std::size(kWidthRuns); ++i)
        if (kWidthRuns[i].first <= kWidthRuns[i - 1].last || kWidthRuns[i].first > kWidthRuns[i].last)
            return false;
    return true;
}
static_assert(sortedAndDisjoint());

} // namespace

int codePointWidth(char32_t cp) {
    if (cp >= 0x20 && cp < 0x7F)
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

} // namespace bw
