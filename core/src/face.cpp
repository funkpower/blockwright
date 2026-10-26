#include "blockwright/face.h"

#include "blockwright/layer.h"
#include "blockwright/pc_fonts.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace bw {

int InkMask::count() const { return static_cast<int>(std::count(ink.begin(), ink.end(), 1)); }

bool sourceCellInk(const Cell& c) { return !isBlank(c); }

InkMask maskOfCells(const CellBlock& cells) {
    InkMask m(cells.width, cells.height * 2);
    for (int y = 0; y < cells.height; ++y)
        for (int x = 0; x < cells.width; ++x)
            if (sourceCellInk(cells.at(x, y))) {
                m.set(x, 2 * y);
                m.set(x, 2 * y + 1);
            }
    return m;
}

namespace {

// A mask's ink as (column, row up from the bottom row), sorted: masks
// aligned left and on the baseline compare point for point.
std::vector<std::pair<int, int>> inkPoints(const InkMask& m, int dx = 0, int dy = 0) {
    std::vector<std::pair<int, int>> out;
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            if (m.at(x, y))
                out.emplace_back(x + dx, m.height - 1 - y + dy);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

bool confusableMasks(const InkMask& a, const InkMask& b) {
    const auto pa = inkPoints(a), pb = inkPoints(b);
    std::vector<std::pair<int, int>> diff;
    std::set_symmetric_difference(pa.begin(), pa.end(), pb.begin(), pb.end(), std::back_inserter(diff));
    if (diff.size() <= 2)
        return true;
    if (pa.size() != pb.size())
        return false;
    constexpr std::pair<int, int> kShifts[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (auto [dx, dy] : kShifts)
        if (inkPoints(a, dx, dy) == pb)
            return true;
    return false;
}

std::vector<ConfusablePair> screenConfusables(const Face& face) {
    std::vector<ConfusablePair> out;
    for (std::u32string_view set : kConfusableSets)
        for (size_t i = 0; i < set.size(); ++i)
            for (size_t j = i + 1; j < set.size(); ++j) {
                const auto ga = face.glyph(set[i]), gb = face.glyph(set[j]);
                if (ga && gb && confusableMasks(ga->mask, gb->mask))
                    out.push_back({set[i], set[j]});
            }
    return out;
}

std::vector<ConfusablePair> confusablesIn(const Face& face, std::u32string_view text) {
    std::vector<ConfusablePair> out;
    for (const ConfusablePair& p : face.confusables())
        if (text.find(p.a) != text.npos && text.find(p.b) != text.npos)
            out.push_back(p);
    return out;
}

TextLayout layoutText(const Face& face, std::u32string_view text, const LayoutOptions& opts) {
    const int spacing = face.letterSpacing(), height = face.height();
    const auto space = face.glyph(U' ');
    const int spaceWidth = space ? space->mask.width : 1;

    TextLayout out;
    std::vector<std::optional<Glyph>> glyphs(text.size());
    std::vector<int> widths(text.size(), 0);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == U'\n')
            continue;
        glyphs[i] = face.glyph(text[i]);
        if (!glyphs[i])
            out.missing.push_back(i);
        widths[i] = glyphs[i] ? glyphs[i]->mask.width : spaceWidth;
    }
    const auto lineWidth = [&](size_t first, size_t count) {
        int w = 0;
        for (size_t k = first; k < first + count; ++k)
            w += widths[k] + (k > first ? spacing : 0);
        return w;
    };

    // Each line of each paragraph: the characters from `pos` until one
    // would cross the wrap width, cut back to the last space after a word
    // when there is one. Only the spaces between two words are dropped at
    // a wrap; the paragraph's leading and ending ones are like any character.
    const int wrap = opts.wrapWidth;
    for (size_t start = 0;;) {
        const size_t end = std::min(text.find(U'\n', start), text.size());
        const std::u32string_view para = text.substr(start, end - start);
        const size_t firstInk = para.find_first_not_of(U' '), lastInk = para.find_last_not_of(U' ');
        const auto between = [&](size_t k) {
            return text[k] == U' ' && firstInk != para.npos && k > start + firstInk && k < start + lastInk;
        };
        size_t pos = start;
        do {
            size_t cut = end, next = end, lastSpace = text.npos;
            int x = 0;
            for (size_t j = pos; j < end; ++j) {
                if (wrap > 0 && j > pos && x + widths[j] > wrap) {
                    cut = next = text[j] != U' ' && lastSpace != text.npos ? lastSpace : j;
                    while (cut > pos && between(cut - 1))
                        --cut;
                    while (next < end && between(next))
                        ++next;
                    break;
                }
                if (text[j] == U' ' && j > pos && text[j - 1] != U' ')
                    lastSpace = j;
                x += widths[j] + spacing;
            }
            out.lines.push_back({pos, cut - pos, lineWidth(pos, cut - pos)});
            pos = next;
        } while (pos < end);
        if (end == text.size())
            break;
        start = end + 1;
    }

    int width = 0;
    for (const auto& line : out.lines)
        width = std::max(width, line.width);
    out.mask = InkMask(width, static_cast<int>(out.lines.size()) * height);
    const bool anyCells = std::any_of(glyphs.begin(), glyphs.end(), [](const auto& g) { return g && g->cells; });
    if (anyCells) {
        out.cells = CellBlock(width, static_cast<int>(out.lines.size()) * height / 2);
        std::fill(out.cells->cells.begin(), out.cells->cells.end(), transparentCell());
    }
    for (size_t l = 0; l < out.lines.size(); ++l) {
        const int top = static_cast<int>(l) * height;
        int x = 0;
        for (size_t k = out.lines[l].first; k < out.lines[l].first + out.lines[l].count; ++k) {
            if (const auto& g = glyphs[k]) {
                for (int y = 0; y < std::min(g->mask.height, height); ++y)
                    for (int gx = 0; gx < g->mask.width; ++gx)
                        if (g->mask.at(gx, y))
                            out.mask.set(x + gx, top + y);
                if (g->cells)
                    for (int y = 0; y < std::min(g->cells->height, height / 2); ++y)
                        for (int gx = 0; gx < g->cells->width; ++gx)
                            out.cells->at(x + gx, top / 2 + y) = g->cells->at(gx, y);
            }
            x += widths[k] + spacing;
        }
    }
    return out;
}

namespace {

// A face drawing printable ASCII from a table of masks.
class PixelFace final : public Face {
public:
    static constexpr char32_t kFirst = 0x20, kLast = 0x7e;
    using Masks = std::array<InkMask, kLast - kFirst + 1>;

    PixelFace(std::string name, int height, int spacing, Masks masks, std::vector<ConfusablePair> confusables)
        : name_(std::move(name)), height_(height), spacing_(spacing), masks_(std::move(masks)),
          confusables_(std::move(confusables)) {}

    std::string_view name() const override { return name_; }
    int height() const override { return height_; }
    int letterSpacing() const override { return spacing_; }
    std::optional<Glyph> glyph(char32_t ch) const override {
        if (ch < kFirst || ch > kLast)
            return std::nullopt;
        return Glyph{masks_[ch - kFirst], std::nullopt};
    }
    std::vector<ConfusablePair> confusables() const override { return confusables_; }

private:
    std::string name_;
    int height_, spacing_;
    Masks masks_;
    std::vector<ConfusablePair> confusables_;
};

// One glyph a CP437 code, 8 px wide, one byte a row, bit 7 leftmost.
std::unique_ptr<PixelFace> bitmapFace(std::string name, std::span<const std::uint8_t> bits, int rows) {
    PixelFace::Masks masks;
    for (char32_t ch = PixelFace::kFirst; ch <= PixelFace::kLast; ++ch) {
        InkMask m(8, rows);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < 8; ++x)
                m.set(x, y, bits[ch * rows + y] & (0x80 >> x));
        masks[ch - PixelFace::kFirst] = std::move(m);
    }
    // Screened with screenConfusables: neither IBM font has a confusable
    // pair in kConfusableSets (test_face.cpp checks this).
    return std::make_unique<PixelFace>(std::move(name), rows, 0, std::move(masks), std::vector<ConfusablePair>{});
}

// funkpower's mini face, ascii.py's MINI_FONT (1.28.0): 4 cell
// rows, 8 pixel rows. Capitals and digits stand on rows 0-4, x-height
// letters on rows 1-4, descenders take rows 5 and 6, and row 7 is left blank
// as the line gap. Letters are 1 to 5 columns, the space 2. Rows from the
// top, '/' between rows, '#' ink; a row of all blanks may be written shorter.
constexpr std::pair<char, const char*> kMini[] = {
    {' ', ".."},
    {'!', "#/#/#/./#"},
    {'"', "#.#/#.#"},
    {'#', "#.#/###/#.#/###/#.#"},
    {'$', ".##/##./.#./.##/##."},
    {'%', "#.#/..#/.#./#../#.#"},
    {'&', ".#./#.#/.#./#.#/.##"},
    {'\'', "#/#"},
    {'(', ".#/#./#./#./.#"},
    {')', "#./.#/.#/.#/#."},
    {'*', ".../#.#/.#./#.#"},
    {'+', ".../.#./###/.#."},
    {',', "././././#/#"},
    {'-', "../../##"},
    {'.', "././././#"},
    {'/', "..#/..#/.#./#../#.."},
    {'0', ".##./#..#/#.##/##.#/.##."},
    {'1', ".#./##./.#./.#./###"},
    {'2', "##./..#/.#./#../###"},
    {'3', "##./..#/.#./..#/##."},
    {'4', "#.#/#.#/###/..#/..#"},
    {'5', "###/#../##./..#/##."},
    {'6', ".##/#../###/#.#/###"},
    {'7', "###/..#/.#./.#./.#."},
    {'8', "###/#.#/###/#.#/###"},
    {'9', "###/#.#/###/..#/##."},
    {':', "./#/./#"},
    {';', "./#/./#/#"},
    {'<', "..#/.#./#../.#./..#"},
    {'=', ".../###/.../###"},
    {'>', "#../.#./..#/.#./#.."},
    {'?', "##./..#/.#./.../.#."},
    {'@', ".##./#..#/#.##/#.##/#.../.###"},
    {'A', ".#./#.#/###/#.#/#.#"},
    {'B', "##./#.#/##./#.#/##."},
    {'C', ".##/#../#../#../.##"},
    {'D', "##./#.#/#.#/#.#/##."},
    {'E', "###/#../##./#../###"},
    {'F', "###/#../##./#../#.."},
    {'G', ".##/#../#.#/#.#/.##"},
    {'H', "#.#/#.#/###/#.#/#.#"},
    {'I', "###/.#./.#./.#./###"},
    {'J', "..#/..#/..#/#.#/.#."},
    {'K', "#.#/#.#/##./#.#/#.#"},
    {'L', "#../#../#../#../###"},
    {'M', "#...#/##.##/#.#.#/#...#/#...#"},
    {'N', "#..#/##.#/#.##/#..#/#..#"},
    {'O', ".##./#..#/#..#/#..#/.##."},
    {'P', "##./#.#/##./#../#.."},
    {'Q', ".##./#..#/#..#/#.##/.###"},
    {'R', "##./#.#/##./#.#/#.#"},
    {'S', ".##/#../.#./..#/##."},
    {'T', "###/.#./.#./.#./.#."},
    {'U', "#.#/#.#/#.#/#.#/###"},
    {'V', "#.#/#.#/#.#/#.#/.#."},
    {'W', "#...#/#...#/#.#.#/##.##/#...#"},
    {'X', "#.#/#.#/.#./#.#/#.#"},
    {'Y', "#.#/#.#/.#./.#./.#."},
    {'Z', "###/..#/.#./#../###"},
    {'[', "##/#./#./#./#./#./##"},
    {'\\', "#../#../.#./..#/..#"},
    {']', "##/.#/.#/.#/.#/.#/##"},
    {'^', ".#./#.#"},
    {'_', "../../../../../../##"},
    {'`', "#./.#"},
    {'a', ".../.##/#.#/#.#/.##"},
    {'b', "#../##./#.#/#.#/##."},
    {'c', ".../.##/#../#../.##"},
    {'d', "..#/.##/#.#/#.#/.##"},
    {'e', ".../.##/###/#../.##"},
    {'f', ".##/#../###/#../#.."},
    {'g', ".../.##/#.#/#.#/.##/..#/##."},
    {'h', "#../##./#.#/#.#/#.#"},
    {'i', "#/./#/#/#"},
    {'j', ".#/../.#/.#/.#/.#/#."},
    {'k', "#../#../#.#/##./#.#"},
    {'l', "#/#/#/#/#"},
    {'m', "...../####./#.#.#/#.#.#/#.#.#"},
    {'n', ".../##./#.#/#.#/#.#"},
    {'o', ".../.#./#.#/#.#/.#."},
    {'p', ".../##./#.#/#.#/##./#../#.."},
    {'q', ".../.##/#.#/#.#/.##/..#/..#"},
    {'r', "../##/#./#./#."},
    {'s', ".../.##/##./..#/##."},
    {'t', ".#./###/.#./.#./.##"},
    {'u', ".../#.#/#.#/#.#/.##"},
    {'v', ".../#.#/#.#/#.#/.#."},
    {'w', "...../#...#/#.#.#/#.#.#/.#.#."},
    {'x', ".../#.#/.#./.#./#.#"},
    {'y', ".../#.#/#.#/#.#/.##/..#/##."},
    {'z', ".../###/.##/##./###"},
    {'{', ".##/.#./.#./#../.#./.#./.##"},
    {'|', "#/#/#/#/#/#/#"},
    {'}', "##./.#./.#./..#/.#./.#./##."},
    {'~', ".../.../.##/##."},
};

std::unique_ptr<PixelFace> makeMini() {
    PixelFace::Masks masks;
    for (const auto& [ch, spec] : kMini) {
        std::vector<std::string> rows;
        for (std::string_view s = spec;;) {
            const size_t slash = s.find('/');
            rows.emplace_back(s.substr(0, slash));
            if (slash == s.npos)
                break;
            s.remove_prefix(slash + 1);
        }
        size_t width = 0;
        for (const auto& r : rows)
            width = std::max(width, r.size());
        InkMask m(static_cast<int>(width), 8);
        for (size_t y = 0; y < rows.size(); ++y)
            for (size_t x = 0; x < rows[y].size(); ++x)
                m.set(static_cast<int>(x), static_cast<int>(y), rows[y][x] == '#');
        masks[static_cast<char32_t>(ch) - PixelFace::kFirst] = std::move(m);
    }
    // Screened with screenConfusables (test_face.cpp checks this list).
    return std::make_unique<PixelFace>("mini", 8, 1, std::move(masks),
                                       std::vector<ConfusablePair>{{U'S', U'5'}, {U'O', U'0'}, {U'Z', U'2'}, {U'K', U'H'}});
}

} // namespace

const Face& blockFace() {
    static const auto face = bitmapFace("block", ibmBios8x8(), 8);
    return *face;
}

const Face& vgaFace() {
    static const auto face = bitmapFace("vga", ibmVga8x16(), 16);
    return *face;
}

const Face& miniFace() {
    static const auto face = makeMini();
    return *face;
}

std::span<const Face* const> bundledFaces() {
    static const std::array<const Face*, 3> faces = {&blockFace(), &vgaFace(), &miniFace()};
    return faces;
}

const Face* bundledFace(std::string_view name) {
    for (const Face* f : bundledFaces())
        if (f->name() == name)
            return f;
    return nullptr;
}

} // namespace bw
