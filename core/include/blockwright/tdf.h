#pragma once
// TheDraw fonts (PLAN §15): .tdf files of one or more outline, block or
// colour fonts, read only. A font becomes a Face (face.h) whose glyphs keep
// their source cells.

#include "blockwright/face.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw {

class TdfError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class TdfType : std::uint8_t { Outline = 0, Block = 1, Colour = 2 };

struct TdfFont {
    static constexpr char32_t kFirst = U'!', kLast = U'~';

    std::string name;
    TdfType type = TdfType::Block;
    int spacing = 0;  // blank columns between two glyphs
    // The glyphs of '!' to '~', as cells, as wide as the glyph and as many
    // rows as it draws; empty when the font has none or it was rejected.
    std::array<std::optional<CellBlock>, kLast - kFirst + 1> glyphs;
    // Glyphs the font defines but that couldn't be read, and why.
    struct Rejected {
        char32_t ch = 0;
        std::string reason;
    };
    std::vector<Rejected> rejected;

    const std::optional<CellBlock>& glyph(char32_t ch) const;
};

// Over these a file isn't read. The published ones are at most 241 KiB,
// 34 fonts and 132,441 cells of glyphs (tdfonts-3.tdf), 159,786 with each
// glyph as tall as its font's tallest (colosall.tdf). The limit on cells
// bounds memory, which a file's size doesn't (glyphs can share data): it
// counts each glyph as tall as its font's tallest, as tdfFace pads it, so
// it bounds the faces of a file too, at kMaxTdfCells source cells and twice
// as many mask pixels, beside each font's space.
inline constexpr std::uintmax_t kMaxTdfBytes = 4u << 20;
inline constexpr std::size_t kMaxTdfFonts = 256, kMaxTdfCells = std::size_t{1} << 20;

// The fonts of a .tdf file, in file order. Throws TdfError, naming the byte
// offset, when the file or a font header is broken or the file is over
// kMaxTdfFonts or kMaxTdfCells (each glyph counted as tall as its font's
// tallest, before the glyph is made); a glyph that can't be read is left out and
// listed in its font's `rejected`.
std::vector<TdfFont> readTdf(std::string_view file);
// The bytes of the regular file at `path`, read through its opened handle
// (readRegular), so a FIFO or device there is refused at once. Throws
// TdfError when it isn't a regular file, can't be read or is over
// kMaxTdfBytes.
std::string readTdfBytes(const std::filesystem::path& path);
// readTdf on the file at `path` (readTdfBytes), throwing as both do.
std::vector<TdfFont> readTdfFile(const std::filesystem::path& path);

// A face drawing `font`: '!' to '~' where the font has the glyph, and a
// blank space. Its height is the font's tallest glyph, every glyph standing
// at the top; its confusables are screened (screenConfusables) here.
std::unique_ptr<Face> tdfFace(TdfFont font);

} // namespace bw
