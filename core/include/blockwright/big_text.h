#pragma once
// Whole-cell big text, compatible with /ascii big's four lint-clean pixel styles.
// The existing block font supplies the bits; bigTextFace adapts the
// glyph differences of the reference's 8x8 variant and doubles pixel rows.
#include "blockwright/text.h"

namespace bw {

const Face& bigTextFace();
std::vector<std::uint8_t> bigTextDefaultColours(BigTextStyle style);
// mIRC 0–15 numbers, colour names, or a reference palette name. No result
// for an empty, malformed or overlong list.
std::optional<std::vector<std::uint8_t>> parseBigTextColours(std::string_view text);
std::string bigTextColoursText(const std::vector<std::uint8_t>& colours);

struct BigTextLine {
    std::size_t first = 0, count = 0;
    int left = 0, top = 0;  // untrimmed font origin in the trimmed drawing, for the cursor
    int hitTop = 0;         // first occupied row of the line box, for hit testing
};
struct BigTextRender {
    TextRender drawing;
    // Letter origins and line hit-test boundaries after trimming the drawing.
    std::vector<BigTextLine> lines;
};
// Includes the one-cell effects, trims clear rows per line and clear
// columns around the drawing. Editor spacing, alignment and explicit new
// lines are retained; wrapped lines have a one-row gap as /ascii big does.
// Measures before allocating; throws TextError for an oversized box.
BigTextRender renderBigText(const TextSource& source);
TextExtent measureBigText(const TextSource& source);

} // namespace bw
