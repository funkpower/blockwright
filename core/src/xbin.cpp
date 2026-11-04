#include "blockwright/xbin.h"

#include "blockwright/cp437.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <vector>

namespace bw {

namespace {

constexpr std::string_view kMagic = "XBIN\x1a";
constexpr size_t kHeaderSize = 11;
constexpr int kMaxFontHeight = 32;

// XBIN header flags.
constexpr std::uint8_t kPaletteFlag = 0x01, kFontFlag = 0x02, kCompressFlag = 0x04, kNonBlinkFlag = 0x08,
                       kFont512Flag = 0x10;

// XBIN run kinds, the top two bits of a run's first byte; the low six are
// its length - 1.
enum Run : std::uint8_t { Plain = 0x00, SameChar = 0x40, SameAttr = 0x80, SameBoth = 0xC0 };
constexpr int kMaxRun = 64;

// SAUCE DataTypes.
constexpr std::uint8_t kBinaryText = 5, kXBin = 6;

std::uint8_t byteAt(std::string_view s, size_t at) { return static_cast<std::uint8_t>(s[at]); }

std::uint16_t le16(std::string_view s, size_t at) {
    return static_cast<std::uint16_t>(byteAt(s, at) | (byteAt(s, at + 1) << 8));
}

void put16(std::string& out, int v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
}

// A cell from a character and attribute byte: the low nibble is the
// foreground, the next three bits the background, and the top bit a bright
// background with iCE colours or blink without, as parseAnsi keeps them.
Cell cellOf(std::uint8_t ch, std::uint8_t a, bool ice) {
    Cell c;
    c.ch = cp437ToUnicode(ch);
    c.fg = Color::indexed(a & 0x0F);
    if (ice) {
        c.bg = Color::indexed(static_cast<std::uint8_t>(a >> 4));
    } else {
        c.bg = Color::indexed(static_cast<std::uint8_t>((a >> 4) & 7));
        if (a & 0x80)
            c.attrs |= attr::Blink;
    }
    return c;
}

// Cells to character/attribute pairs, row by row, as writeAnsi would write
// them with iCE `ice`: transparent cells blank, '?' for a glyph CP437 hasn't
// got. `width` may be the canvas's width + 1, a blank column.
std::vector<std::uint8_t> pairsOf(const Canvas& cv, int width, bool ice, AnsiOutput& out) {
    std::vector<std::uint8_t> pairs;
    pairs.reserve(static_cast<size_t>(width) * cv.height() * 2);
    const Cell blank{};
    for (int y = 0; y < cv.height(); ++y)
        for (int x = 0; x < width; ++x) {
            const Cell& c = x >= cv.width() || cv.at(x, y).transparent ? blank : cv.at(x, y);
            const std::uint8_t fg = c.fg.paletteIndex(), bg = c.bg.paletteIndex();
            const bool high = bg >= 8 || (!ice && (c.attrs & attr::Blink));
            const Glyph right = x + 1 < cv.width() && !cv.at(x, y).transparent ? cv.at(x + 1, y).ch : Glyph();
            const std::optional<char32_t> cp = interimCodePoint(c.ch, right);
            std::optional<std::uint8_t> b = cp ? unicodeToCp437(*cp) : std::nullopt;
            if (!b) {
                out.unwritable.push_back({x, y, c.ch});
                b = '?';
            }
            pairs.push_back(*b);
            pairs.push_back(static_cast<std::uint8_t>(fg | ((bg & 7) << 4) | (high ? 0x80 : 0)));
        }
    return pairs;
}

void checkSize(int width, int height) {
    if (width < 1 || width > kMaxColumns)
        throw AnsiError("width " + std::to_string(width) + " is outside 1-" + std::to_string(kMaxColumns) + " columns");
    if (height < 1 || height > kMaxRows)
        throw AnsiError("height " + std::to_string(height) + " is outside 1-" + std::to_string(kMaxRows) + " rows");
}

std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

ArtFormat artFormatFor(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return ArtFormat::Ansi;
    const std::string ext = lowered(name.substr(dot));
    if (ext == ".xb" || ext == ".xbin")
        return ArtFormat::Xbin;
    if (ext == ".bin")
        return ArtFormat::Bin;
    return ArtFormat::Ansi;
}

ArtFormat artFormatOf(std::string_view path, std::string_view file) {
    return file.starts_with(kMagic) ? ArtFormat::Xbin : artFormatFor(path);
}

AnsiDocument parseArt(std::string_view file, ArtFormat format, const ParseOptions& ansi) {
    switch (format) {
    case ArtFormat::Xbin: return parseXbin(file);
    case ArtFormat::Bin: return parseBin(file);
    case ArtFormat::Ansi: break;
    }
    return parseAnsi(file, ansi);
}

AnsiOutput writeArt(const AnsiDocument& doc, ArtFormat format, TextEncoding ansi) {
    switch (format) {
    case ArtFormat::Xbin: return writeXbin(doc);
    case ArtFormat::Bin: return writeBin(doc);
    case ArtFormat::Ansi: break;
    }
    return writeAnsi(doc, ansi);
}

namespace {

// Decodes the header, palette, font and cells from `data` into `doc`;
// returns the offset just past the last byte the cells took. `complete` is
// whether the data held every cell.
size_t decodeXbin(std::string_view data, AnsiDocument& doc, bool& complete) {
    const int width = le16(data, 5), height = le16(data, 7), fontHeight = byteAt(data, 9);
    const std::uint8_t flags = byteAt(data, 10);
    checkSize(width, height);
    if (flags & kFont512Flag)
        throw AnsiError("512-character XBIN fonts are not supported");
    doc.ice = flags & kNonBlinkFlag;
    doc.palette.reset();
    doc.font.reset();
    size_t at = kHeaderSize;
    if (flags & kPaletteFlag) {
        if (data.size() < at + 48)
            throw AnsiError("the XBIN palette is cut short");
        std::array<Rgb, 16> pal;
        const auto widen = [&](size_t i) {
            const std::uint8_t v = byteAt(data, at + i) & 0x3F;
            return static_cast<std::uint8_t>(v << 2 | v >> 4);
        };
        for (size_t i = 0; i < 16; ++i)
            pal[i] = {widen(i * 3), widen(i * 3 + 1), widen(i * 3 + 2)};
        doc.palette = pal;
        at += 48;
    }
    if (flags & kFontFlag) {
        if (fontHeight < 1 || fontHeight > kMaxFontHeight)
            throw AnsiError("XBIN font height " + std::to_string(fontHeight) + " is outside 1-" +
                            std::to_string(kMaxFontHeight));
        const size_t n = size_t(256) * fontHeight;
        if (data.size() < at + n)
            throw AnsiError("the XBIN font is cut short");
        doc.font = FileFont{fontHeight, std::string(data.substr(at, n))};
        at += n;
    }
    Canvas cv(width, height);
    const size_t cells = size_t(width) * height;
    size_t i = 0;
    const auto put = [&](std::uint8_t ch, std::uint8_t a) {
        cv.at(int(i % width), int(i / width)) = cellOf(ch, a, doc.ice);
        ++i;
    };
    if (flags & kCompressFlag) {
        while (i < cells && at < data.size()) {
            const std::uint8_t head = byteAt(data, at++);
            const size_t n = std::min<size_t>((head & 0x3F) + 1, cells - i);
            switch (head & 0xC0) {
            case Plain:
                for (size_t k = 0; k < n && at + 1 < data.size(); ++k, at += 2)
                    put(byteAt(data, at), byteAt(data, at + 1));
                break;
            case SameChar: {
                if (at >= data.size())
                    break;
                const std::uint8_t ch = byteAt(data, at++);
                for (size_t k = 0; k < n && at < data.size(); ++k)
                    put(ch, byteAt(data, at++));
                break;
            }
            case SameAttr: {
                if (at >= data.size())
                    break;
                const std::uint8_t a = byteAt(data, at++);
                for (size_t k = 0; k < n && at < data.size(); ++k)
                    put(byteAt(data, at++), a);
                break;
            }
            default:  // SameBoth
                if (at + 1 >= data.size()) {
                    at = data.size();
                    break;
                }
                for (size_t k = 0; k < n; ++k)
                    put(byteAt(data, at), byteAt(data, at + 1));
                at += 2;
                break;
            }
        }
    } else {
        for (; i < cells && at + 1 < data.size(); at += 2)
            put(byteAt(data, at), byteAt(data, at + 1));
    }
    doc.canvas = std::move(cv);
    complete = i == cells;
    return std::min(at, data.size());
}

struct XbinRead {
    AnsiDocument doc;
    SauceSplit split;
};

// The cells come first: their bytes are the header's size, palette, font and
// width × height cells (or the runs that make them), and only what follows
// them can be SAUCE, so cells that spell "SAUCE" are cells. A file too short
// for its cells is cut short; then a SAUCE record at its end is taken as one,
// as for .ans, and the cells are read from what is before it and its EOF, so
// the EOF never completes a run or becomes a cell.
XbinRead readXbin(std::string_view file) {
    if (file.size() < kHeaderSize || !file.starts_with(kMagic))
        throw AnsiError("not an XBIN file (no XBIN header)");
    XbinRead r;
    bool complete = false;
    size_t end = decodeXbin(file, r.doc, complete);
    if (complete) {
        const SauceSplit after = splitSauce(file.substr(end));
        r.split.sauce = after.sauce;
        r.split.body = file.substr(0, end + after.body.size());
    } else {
        SauceSplit split = splitSauce(file);
        std::string_view data = split.body;
        if (split.sauce && data.ends_with('\x1a'))  // SAUCE's EOF
            data.remove_suffix(1);
        // the record is after the header: a file too short to hold both is all data
        if (split.sauce && data.size() >= kHeaderSize) {
            decodeXbin(data, r.doc, complete);
            end = data.size();  // all of it is the image, a last odd byte too
            r.split = std::move(split);
        } else {
            r.split.body = file;
        }
    }
    r.split.content = file.substr(0, end);
    r.doc.sauce = r.split.sauce;
    return r;
}

// A BIN has no header to bound its cells, so a record is SAUCE only if it
// could describe the bytes before it: a FileSize past them is cells.
SauceSplit splitBin(std::string_view file) {
    SauceSplit split = splitSauce(file);
    if (split.sauce && split.sauce->fileSize > split.body.size()) {
        split = {};
        split.body = file;
    }
    std::string_view data = split.body;
    if (data.size() % 2)  // SAUCE's EOF, or a stray last byte
        data.remove_suffix(1);
    split.content = data;
    return split;
}

} // namespace

SauceSplit splitArtSauce(std::string_view file, ArtFormat format) {
    switch (format) {
    case ArtFormat::Xbin: return readXbin(file).split;
    case ArtFormat::Bin: return splitBin(file);
    case ArtFormat::Ansi: break;
    }
    return splitSauce(file);
}

AnsiDocument parseXbin(std::string_view file) { return readXbin(file).doc; }

std::string xbinCompressRow(std::span<const std::uint8_t> pairs) {
    const size_t n = pairs.size() / 2;
    const auto ch = [&](size_t i) { return pairs[2 * i]; };
    const auto at = [&](size_t i) { return pairs[2 * i + 1]; };
    // How far the character, the attribute and both repeat from each cell.
    std::vector<int> sameCh(n + 1), sameAttr(n + 1), sameBoth(n + 1);
    for (size_t i = n; i-- > 0;) {
        const bool next = i + 1 < n;
        sameCh[i] = next && ch(i + 1) == ch(i) ? sameCh[i + 1] + 1 : 1;
        sameAttr[i] = next && at(i + 1) == at(i) ? sameAttr[i + 1] + 1 : 1;
        sameBoth[i] = next && ch(i + 1) == ch(i) && at(i + 1) == at(i) ? sameBoth[i + 1] + 1 : 1;
    }
    // cost[i]: the fewest bytes for cells i.., and the run that starts them.
    std::vector<size_t> cost(n + 1, 0);
    std::vector<std::pair<Run, int>> pick(n);
    for (size_t i = n; i-- > 0;) {
        size_t best = std::numeric_limits<size_t>::max();
        const auto tryRun = [&](Run kind, int longest) {
            longest = std::min<int>({longest, kMaxRun, int(n - i)});
            for (int len = longest; len >= 1; --len) {
                const size_t body = kind == SameBoth ? 2 : kind == Plain ? 2 * size_t(len) : 1 + size_t(len);
                const size_t c = 1 + body + cost[i + len];
                if (c < best) {
                    best = c;
                    pick[i] = {kind, len};
                }
            }
        };
        tryRun(SameBoth, sameBoth[i]);
        tryRun(SameChar, sameCh[i]);
        tryRun(SameAttr, sameAttr[i]);
        tryRun(Plain, kMaxRun);
        cost[i] = best;
    }
    std::string out;
    out.reserve(cost[0]);
    for (size_t i = 0; i < n;) {
        const auto [kind, len] = pick[i];
        out += static_cast<char>(kind | (len - 1));
        switch (kind) {
        case Plain:
            for (int k = 0; k < len; ++k) {
                out += static_cast<char>(ch(i + k));
                out += static_cast<char>(at(i + k));
            }
            break;
        case SameChar:
            out += static_cast<char>(ch(i));
            for (int k = 0; k < len; ++k)
                out += static_cast<char>(at(i + k));
            break;
        case SameAttr:
            out += static_cast<char>(at(i));
            for (int k = 0; k < len; ++k)
                out += static_cast<char>(ch(i + k));
            break;
        case SameBoth:
            out += static_cast<char>(ch(i));
            out += static_cast<char>(at(i));
            break;
        }
        i += len;
    }
    return out;
}

AnsiOutput writeXbin(const AnsiDocument& doc, const XbinOptions& opts) {
    AnsiOutput out;
    const Canvas& cv = doc.canvas;
    const bool ice = writtenIce(doc, &out.blinkDropped);
    const std::vector<std::uint8_t> pairs = pairsOf(cv, cv.width(), ice, out);
    std::string& s = out.bytes;
    s = kMagic;
    put16(s, cv.width());
    put16(s, cv.height());
    std::uint8_t flags = 0;
    if (doc.palette)
        flags |= kPaletteFlag;
    if (doc.font)
        flags |= kFontFlag;
    if (opts.compress)
        flags |= kCompressFlag;
    if (ice)
        flags |= kNonBlinkFlag;
    s += static_cast<char>(doc.font ? doc.font->height : 16);
    s += static_cast<char>(flags);
    if (doc.palette)
        for (const Rgb& c : *doc.palette) {
            s += static_cast<char>(c.r >> 2);
            s += static_cast<char>(c.g >> 2);
            s += static_cast<char>(c.b >> 2);
        }
    if (doc.font) {
        std::string bits = doc.font->bits;
        bits.resize(size_t(256) * doc.font->height, '\0');
        s += bits;
    }
    if (opts.compress) {
        const size_t row = size_t(cv.width()) * 2;
        for (int y = 0; y < cv.height(); ++y)
            s += xbinCompressRow(std::span(pairs).subspan(y * row, row));
    } else {
        s.append(pairs.begin(), pairs.end());
    }
    if (doc.sauce) {
        const Sauce& had = *doc.sauce;
        out.letterSpacingDropped = had.letterSpacing() == 2;
        if (had.aspectRatio() == 1 || had.aspectRatio() == 2)
            out.aspectDropped = had.aspectRatio();
        if (!had.font.empty() && had.font != "IBM VGA" && had.font != "IBM VGA 437")
            out.fontNameDropped = had.font;
    }
    Sauce sauce = doc.sauce ? *doc.sauce : Sauce::fresh(cv.width(), cv.height());
    sauce.dataType = kXBin;
    sauce.fileType = 0;
    sauce.tinfo1 = static_cast<std::uint16_t>(cv.width());
    sauce.tinfo2 = static_cast<std::uint16_t>(cv.height());
    sauce.tinfo3 = sauce.tinfo4 = 0;
    sauce.flags = 0;
    sauce.font.clear();
    const auto contentSize = static_cast<std::uint32_t>(s.size());
    s += encodeSauce(sauce, contentSize);
    return out;
}

AnsiDocument parseBin(std::string_view file, const BinOptions& opts) {
    SauceSplit split = splitBin(file);
    AnsiDocument doc;
    doc.sauce = std::move(split.sauce);
    int width = opts.defaultWidth;
    if (doc.sauce && doc.sauce->dataType == kBinaryText) {
        if (doc.sauce->fileType)
            width = doc.sauce->fileType * 2;
        doc.ice = doc.sauce->ice();
    }
    const std::string_view data = split.content;
    const size_t cells = data.size() / 2;
    if (!cells)
        throw AnsiError("no BIN content (no character/attribute pairs)");
    if (width < 1 || width > kMaxColumns)
        throw AnsiError("width " + std::to_string(width) + " is outside 1-" + std::to_string(kMaxColumns) + " columns");
    const size_t rows = (cells + width - 1) / width;
    if (rows > size_t(kMaxRows))
        throw AnsiError("height " + std::to_string(rows) + " is over " + std::to_string(kMaxRows) + " rows");
    Canvas cv(width, int(rows));
    for (size_t i = 0; i < cells; ++i)
        cv.at(int(i % width), int(i / width)) = cellOf(byteAt(data, 2 * i), byteAt(data, 2 * i + 1), doc.ice);
    doc.canvas = std::move(cv);
    return doc;
}

AnsiOutput writeBin(const AnsiDocument& doc) {
    AnsiOutput out;
    const Canvas& cv = doc.canvas;
    const bool ice = writtenIce(doc, &out.blinkDropped);
    out.paletteDropped = customPalette(doc);
    out.fontDropped = doc.font.has_value();
    const int width = cv.width() + cv.width() % 2;
    const std::vector<std::uint8_t> pairs = pairsOf(cv, width, ice, out);
    out.bytes.assign(pairs.begin(), pairs.end());
    Sauce sauce = doc.sauce ? *doc.sauce : Sauce::fresh(width, cv.height());
    sauce.dataType = kBinaryText;
    sauce.fileType = static_cast<std::uint8_t>(width / 2);
    sauce.tinfo1 = sauce.tinfo2 = sauce.tinfo3 = sauce.tinfo4 = 0;
    sauce.setIce(ice);
    const auto contentSize = static_cast<std::uint32_t>(out.bytes.size());
    out.bytes += encodeSauce(sauce, contentSize);
    return out;
}

} // namespace bw
