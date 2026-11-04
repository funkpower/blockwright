#include "blockwright/tdf.h"

#include "blockwright/cp437.h"
#include "blockwright/layer.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

// The format, from Roy/SAC's "TheDraw Fonts File (.TDF) Specifications"
// (2014, roysac.com), checked against the published fonts tdfiglet and
// retrofont collect:
//
//   file   13, "TheDraw FONTS file", 1A (20 bytes), then the fonts
//   font   55 AA 00 FF; the name's length (1) and the name (12, padded);
//          4 unused; type (0 outline, 1 block, 2 colour); letter spacing;
//          the size of its glyph data (u16, little-endian); 94 offsets into
//          that data (u16, FFFF none) for '!' to '~'; the data. The next
//          font starts where the data ends.
//   glyph  width (1-30), height (1-12), then the rows, 0D between two, 00
//          at the end. Outline and block fonts store a byte a cell, colour
//          fonts a byte and an attribute (foreground the low nibble,
//          background bits 4-6, blink bit 7). A row shorter than the width
//          leaves the rest of it empty. '&' marks a descender row, one below
//          the height, and is not drawn. Glyphs may share data.
//
// What the published fonts settle that the spec leaves open or gets wrong:
// - Nothing follows the last font: the file ends with its data, or SAUCE
//   (1A on) follows. After a font, the end of the file, 00 or 1A ends them.
// - The letter spacing is a count of columns (fonts with 0 exist), not the
//   spec's 1-40 scale.
// - The width and height bytes are often wrong (rows past the width,
//   heights of 0): the rows are read as they are, and a row wider than the
//   width widens the glyph.
// - Colour fonts mostly store '&' as TheDraw does, as a character and an
//   attribute, which tdfiglet draws (an attribute of 00 is black on black,
//   as AtlantisSmB's Q has it). Some (Fraktur, Metal, retrofont's own) store
//   the marker alone, before 0D or 00, as outline and block fonts do. A
//   colour glyph is read in pairs; when that reading has no end, breaks the
//   limits, or runs on past the start of the next glyph's data (or the end
//   of the font's), it is read again with a '&' before 0D or 00 alone. Of
//   the 95 published colour glyphs with a '&', 50 are pairs and 45 have it
//   alone, and the reading kept ends where the next glyph's data starts.
// A glyph past the format's limits (wider than 30, over 12 rows, no end) is
// left out and the rest of the font read; a broken file or font header is
// an error, and so is a file over kMaxTdfFonts fonts or kMaxTdfCells cells
// of glyphs, each counted as tall as its font's tallest glyph (as a face
// draws it), found before the glyph past the limit is made.

namespace bw {

const std::optional<CellBlock>& TdfFont::glyph(char32_t ch) const {
    static const std::optional<CellBlock> none;
    return ch < kFirst || ch > kLast ? none : glyphs[ch - kFirst];
}

namespace {

constexpr std::string_view kSignature = "\x13TheDraw FONTS file\x1A";
constexpr std::string_view kIndicator{"\x55\xAA\x00\xFF", 4};
constexpr std::size_t kFontHeader = 213, kNameMax = 12;
constexpr std::size_t kTypeAt = 21, kSpacingAt = 22, kSizeAt = 23, kOffsetsAt = 25;
constexpr std::size_t kMaxWidth = 30, kMaxRows = 12;

// Outline fonts store the parts of an outline, not characters: 'A' to 'R'
// are its edges, corners and joins and a hard space, '@' is no cell. TheDraw
// draws them in one of 19 outline styles, picked when the text is typed.
// They are drawn here in the 10th, the style Roy/SAC's table gives and
// TheDraw's manual shows a font being designed in (retrofont's
// OUTLINE_CHAR_SET_UNICODE[9] is the same). Every style draws 'A' to 'N' as
// a line and 'O' to 'R' blank, so a glyph's letter mask is the same in all
// 19. Any other byte, which no published outline font has, is drawn as its
// CP437 character.
constexpr char32_t kOutlineStyle[] = {
    U'═', U'─', U'│', U'║', U'╒', U'╗', U'╓', U'┐', U'╚', U'╛', U'└', U'╜', U'╡', U'╟', U' ', U' ', U' ', U' ',
};

std::uint8_t byteAt(std::string_view s, std::size_t i) { return static_cast<std::uint8_t>(s[i]); }
unsigned u16At(std::string_view s, std::size_t i) { return byteAt(s, i) | unsigned(byteAt(s, i + 1)) << 8; }

[[noreturn]] void fail(std::size_t offset, const std::string& what) {
    throw TdfError("TheDraw font, byte " + std::to_string(offset) + ": " + what);
}

Cell outlineCell(std::uint8_t b) {
    if (b == '@')
        return transparentCell();
    Cell c;
    c.ch = b >= 'A' && b <= 'R' ? kOutlineStyle[b - 'A'] : cp437ToUnicode(b);
    return c;
}

Cell blockCell(std::uint8_t b) {
    Cell c;
    c.ch = cp437ToUnicode(b);
    return c;
}

Cell colourCell(std::uint8_t b, std::uint8_t attribute) {
    Cell c;
    c.ch = cp437ToUnicode(b);
    c.fg = Color::indexed(attribute & 15);
    c.bg = Color::indexed((attribute >> 4) & 7);
    c.attrs = attribute & 0x80 ? attr::Blink : 0;
    return c;
}

// A glyph's rows, read from `offset` in a font's data, and where its data
// ends; or why it can't be read. `loneMarker`: in a colour font, a '&'
// before 0D or 00 is the marker alone, not a character and its attribute.
struct GlyphRows {
    std::vector<std::vector<Cell>> rows;
    std::size_t width = 0, end = 0;
};

std::optional<GlyphRows> readRows(std::string_view data, std::size_t offset, TdfType type, bool loneMarker,
                                  std::string& why) {
    if (data.size() - offset < 2) {
        why = "the glyph is cut short";
        return std::nullopt;
    }
    GlyphRows g;
    g.width = byteAt(data, offset);
    const std::size_t height = byteAt(data, offset + 1);
    if (g.width < 1 || g.width > kMaxWidth || height > kMaxRows) {
        why = "the glyph is " + std::to_string(g.width) + "x" + std::to_string(height) + ", over 30x12 or 0 wide";
        return std::nullopt;
    }
    g.rows.resize(1);
    for (std::size_t i = offset + 2;;) {
        if (i >= data.size()) {
            why = "the glyph has no end";
            return std::nullopt;
        }
        const std::uint8_t b = byteAt(data, i++);
        if (b == 0x00) {
            g.end = i;
            break;
        }
        if (b == 0x0D) {
            if (g.rows.size() == kMaxRows) {
                why = "the glyph has over 12 rows";
                return std::nullopt;
            }
            g.rows.emplace_back();
            continue;
        }
        if (b == '&' && (type != TdfType::Colour ||
                         (loneMarker && (i >= data.size() || byteAt(data, i) == 0x00 || byteAt(data, i) == 0x0D))))
            continue;
        if (g.rows.back().size() == kMaxWidth) {
            why = "a row of the glyph is over 30 columns";
            return std::nullopt;
        }
        if (type == TdfType::Colour) {
            if (i >= data.size()) {
                why = "the glyph has no end";
                return std::nullopt;
            }
            g.rows.back().push_back(colourCell(b, byteAt(data, i++)));
        } else {
            g.rows.back().push_back(type == TdfType::Outline ? outlineCell(b) : blockCell(b));
        }
    }
    for (const auto& row : g.rows)
        g.width = std::max(g.width, row.size());
    return g;
}

// The glyph at `offset`, whose data should end by `bound`: the next glyph's
// data, or the end of the font's.
std::optional<GlyphRows> readGlyph(std::string_view data, std::size_t offset, std::size_t bound, TdfType type,
                                   std::string& why) {
    auto g = readRows(data, offset, type, false, why);
    if (type == TdfType::Colour && (!g || g->end > bound)) {
        std::string loneWhy;
        if (auto lone = readRows(data, offset, type, true, loneWhy))
            g = std::move(lone);
    }
    return g;
}

CellBlock cellsOf(const GlyphRows& g) {
    CellBlock cells(static_cast<int>(g.width), static_cast<int>(g.rows.size()));
    std::fill(cells.cells.begin(), cells.cells.end(), transparentCell());
    for (std::size_t y = 0; y < g.rows.size(); ++y)
        std::copy(g.rows[y].begin(), g.rows[y].end(), cells.cells.begin() + static_cast<std::ptrdiff_t>(y * g.width));
    return cells;
}

std::string quoted(char32_t ch) {
    std::string out = "'";
    appendUtf8(out, ch);
    return out + "'";
}

} // namespace

std::vector<TdfFont> readTdf(std::string_view file) {
    if (!file.starts_with(kSignature))
        fail(0, "no TheDraw font signature");
    std::vector<TdfFont> fonts;
    // cells of the fonts before this one, each glyph as tall as its font's
    // tallest (tdfFace pads them so)
    std::size_t pos = kSignature.size(), cellsBefore = 0;
    while (pos < file.size() && byteAt(file, pos) != 0x00 && byteAt(file, pos) != 0x1A) {
        if (fonts.size() == kMaxTdfFonts)
            fail(pos, "over " + std::to_string(kMaxTdfFonts) + " fonts in the file");
        if (file.size() - pos < kFontHeader)
            fail(pos, "the font header is cut short");
        if (file.substr(pos, kIndicator.size()) != kIndicator)
            fail(pos, "no font starts here");
        TdfFont font;
        const std::size_t nameLength = byteAt(file, pos + 4);
        if (nameLength > kNameMax)
            fail(pos + 4, "the font name is " + std::to_string(nameLength) + " long, over 12");
        const std::string_view name = file.substr(pos + 5, nameLength);
        font.name = cp437ToUtf8(name.substr(0, name.find('\0')));
        const unsigned type = byteAt(file, pos + kTypeAt);
        if (type > 2)
            fail(pos + kTypeAt, "font type " + std::to_string(type) + " is not 0, 1 or 2");
        font.type = static_cast<TdfType>(type);
        font.spacing = byteAt(file, pos + kSpacingAt);
        const std::size_t size = u16At(file, pos + kSizeAt), dataAt = pos + kFontHeader;
        if (file.size() - dataAt < size)
            fail(pos + kSizeAt, "the font's " + std::to_string(size) + " bytes of glyphs run past the end of the file");
        const std::string_view data = file.substr(dataAt, size);
        std::vector<std::size_t> starts;  // where each glyph's data starts, in order
        for (std::size_t k = 0; k < font.glyphs.size(); ++k)
            starts.push_back(u16At(file, pos + kOffsetsAt + 2 * k));
        std::sort(starts.begin(), starts.end());
        std::size_t widths = 0, rows = 1;  // the font's glyphs so far: their widths, the tallest
        for (std::size_t k = 0; k < font.glyphs.size(); ++k) {
            const char32_t ch = TdfFont::kFirst + static_cast<char32_t>(k);
            const std::size_t at = pos + kOffsetsAt + 2 * k;
            const unsigned offset = u16At(file, at);
            if (offset == 0xFFFF)
                continue;
            if (offset >= size)
                fail(at, "the glyph of " + quoted(ch) + " is at " + std::to_string(offset) + ", past the font's " +
                             std::to_string(size) + " bytes");
            const auto next = std::upper_bound(starts.begin(), starts.end(), offset);
            const std::size_t bound = next == starts.end() ? size : std::min<std::size_t>(*next, size);
            std::string why;
            const auto g = readGlyph(data, offset, bound, font.type, why);
            if (!g) {
                font.rejected.push_back({ch, why});
                continue;
            }
            // Glyphs that share data are each made, so each counts, as
            // tall as the font's tallest so far: a taller glyph adds rows to
            // every glyph before it. widths and rows stay under 94 * 30 and
            // 12, so nothing here overflows.
            widths += g->width;
            rows = std::max(rows, g->rows.size());
            if (widths * rows > kMaxTdfCells - cellsBefore)
                fail(dataAt + offset, "the glyphs, each as tall as its font's tallest, come to over " +
                                          std::to_string(kMaxTdfCells) + " cells");
            font.glyphs[k] = cellsOf(*g);
        }
        cellsBefore += widths * rows;
        fonts.push_back(std::move(font));
        pos = dataAt + size;
    }
    if (fonts.empty())
        fail(pos, "no font in the file");
    return fonts;
}

std::string readTdfBytes(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
        throw TdfError("can't read " + path.string() + ": " + ec.message());
    if (size > kMaxTdfBytes)
        throw TdfError(path.string() + " is over " + std::to_string(kMaxTdfBytes >> 20) + " MiB");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw TdfError("can't read " + path.string());
    // Read at most one byte over the limit, whatever the file grew to.
    std::string bytes(static_cast<std::size_t>(kMaxTdfBytes) + 1, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    if (bytes.size() > kMaxTdfBytes)
        throw TdfError(path.string() + " is over " + std::to_string(kMaxTdfBytes >> 20) + " MiB");
    return bytes;
}

std::vector<TdfFont> readTdfFile(const std::filesystem::path& path) { return readTdf(readTdfBytes(path)); }

namespace {

class TdfFace final : public Face {
public:
    explicit TdfFace(TdfFont font) : font_(std::move(font)) {
        int rows = 1;
        for (const auto& g : font_.glyphs)
            if (g)
                rows = std::max(rows, g->height);
        height_ = 2 * rows;
        for (std::size_t k = 0; k < font_.glyphs.size(); ++k)
            if (const auto& g = font_.glyphs[k]) {
                CellBlock cells(g->width, rows);
                std::fill(cells.cells.begin(), cells.cells.end(), transparentCell());
                std::copy(g->cells.begin(), g->cells.end(), cells.cells.begin());
                glyphs_[k] = FaceGlyph{maskOfCells(cells), std::move(cells)};
            }
        // The space, which TheDraw fonts don't have: half the letters'
        // average width, rounded up.
        int sum = 0, letters = 0;
        for (char32_t ch = U'A'; ch <= U'z'; ++ch)
            if ((ch <= U'Z' || ch >= U'a') && font_.glyph(ch)) {
                sum += font_.glyph(ch)->width;
                ++letters;
            }
        const int spaceWidth = letters ? std::max(1, (sum + 2 * letters - 1) / (2 * letters)) : 1;
        CellBlock blank(spaceWidth, rows);
        std::fill(blank.cells.begin(), blank.cells.end(), transparentCell());
        space_ = FaceGlyph{InkMask(spaceWidth, height_), std::move(blank)};
        // the glyphs as read are in glyphs_ now; only the name and spacing
        // are kept
        font_.glyphs = {};
    }

    void screen() { confusables_ = screenConfusables(*this); }

    std::string_view name() const override { return font_.name; }
    int height() const override { return height_; }
    int letterSpacing() const override { return font_.spacing; }
    std::optional<FaceGlyph> glyph(char32_t ch) const override {
        if (ch == U' ')
            return space_;
        if (ch < TdfFont::kFirst || ch > TdfFont::kLast)
            return std::nullopt;
        return glyphs_[ch - TdfFont::kFirst];
    }
    std::vector<ConfusablePair> confusables() const override { return confusables_; }

private:
    TdfFont font_;
    int height_ = 2;
    std::array<std::optional<FaceGlyph>, TdfFont::kLast - TdfFont::kFirst + 1> glyphs_;
    FaceGlyph space_;
    std::vector<ConfusablePair> confusables_;
};

} // namespace

std::unique_ptr<Face> tdfFace(TdfFont font) {
    auto face = std::make_unique<TdfFace>(std::move(font));
    face->screen();
    return face;
}

} // namespace bw
