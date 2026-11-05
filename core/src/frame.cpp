#include "blockwright/frame.h"

#include "blockwright/ansi.h"
#include "blockwright/selection.h"
#include "blockwright/width.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QString>

#include <algorithm>

namespace bw {

int FrameStyle::minimumWidth() const { return std::max(minWidth, left + right); }
int FrameStyle::minimumHeight() const { return std::max(minHeight, top + bottom); }

bool FrameStyle::keepsBackground(int x, int y) const {
    return !keepBg.empty() && keepBg[std::size_t(y) * std::size_t(source.width()) + std::size_t(x)];
}

bool FrameStyle::valid() const {
    // Each inset is bounded before two are added, so no sum overflows.
    const int sw = source.width(), sh = source.height();
    return sw <= kMaxFrameSourceColumns && sh <= kMaxFrameSourceRows && left >= 0 && right >= 0 && top >= 0 &&
           bottom >= 0 && left < sw && right < sw && top < sh && bottom < sh && sw > left + right &&
           sh > top + bottom && minWidth >= 0 && minHeight >= 0 && minWidth <= kMaxColumns &&
           minHeight <= kMaxRows &&
           (keepBg.empty() || keepBg.size() == std::size_t(source.width()) * std::size_t(source.height()));
}

namespace {

// The source column (or row) cell `i` of a strip `length` long takes: the
// corner `lead` cells before the repeat as they are, then the middle
// `middle` cells over and over, then the corner `trail` cells after it.
int sourceIndex(int i, int length, int lead, int middle, int trail) {
    if (i < lead)
        return i;
    if (i >= length - trail)
        return lead + middle + (i - (length - trail));
    return lead + (i - lead) % middle;
}

} // namespace

std::optional<Canvas> drawFrame(const FrameStyle& style, int width, int height, std::vector<bool>* keepBg) {
    if (!style.valid() || width < style.minimumWidth() || height < style.minimumHeight() || width > kMaxColumns ||
        height > kMaxRows)
        return std::nullopt;
    const int sw = style.source.width(), sh = style.source.height();
    const int midW = sw - style.left - style.right, midH = sh - style.top - style.bottom;
    Canvas out(width, height);
    if (keepBg)
        keepBg->assign(std::size_t(width) * std::size_t(height), false);
    for (int y = 0; y < height; ++y) {
        const int sy = sourceIndex(y, height, style.top, midH, style.bottom);
        const bool midRow = y >= style.top && y < height - style.bottom;
        for (int x = 0; x < width; ++x) {
            const bool midCol = x >= style.left && x < width - style.right;
            Cell& c = out.at(x, y);
            if (midRow && midCol && !style.fill) {
                c = Cell{};
                c.transparent = true;
                continue;
            }
            const int sx = sourceIndex(x, width, style.left, midW, style.right);
            c = style.source.at(sx, sy);
            if (keepBg && style.keepsBackground(sx, sy))
                (*keepBg)[std::size_t(y) * std::size_t(width) + std::size_t(x)] = true;
        }
    }
    return out;
}

Canvas recolourFrame(Canvas frame, Color fg, Color bg) {
    for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x) {
            Cell& c = frame.at(x, y);
            if (!c.transparent) {
                c.fg = fg;
                c.bg = bg;
            }
        }
    return frame;
}

namespace {

constexpr char kHex[] = "0123456789abcdef";

QJsonArray rowsOf(const Canvas& c, int what) {
    QJsonArray rows;
    for (int y = 0; y < c.height(); ++y) {
        QString row;
        for (int x = 0; x < c.width(); ++x) {
            const Cell& cell = c.at(x, y);
            if (what == 0) {
                const char32_t cp = cell.ch.codePoint().value_or(U'?');
                row += QString::fromUcs4(&cp, 1);
            } else if (what == 2 && cell.transparent) {
                row += QLatin1Char('.');
            } else {
                row += QLatin1Char(kHex[(what == 1 ? cell.fg : cell.bg).paletteIndex() & 15]);
            }
        }
        rows.append(row);
    }
    return rows;
}

std::optional<FrameStyle> fail(std::string* error, const std::string& why) {
    if (error)
        *error = why;
    return std::nullopt;
}

std::optional<std::vector<int>> ints(const QJsonValue& v, qsizetype n) {
    if (!v.isArray() || v.toArray().size() != n)
        return std::nullopt;
    std::vector<int> out;
    for (const QJsonValue& e : v.toArray()) {
        if (!e.isDouble() || e.toDouble() != double(e.toInt()) || e.toInt() < 0)
            return std::nullopt;
        out.push_back(e.toInt());
    }
    return out;
}

// A list of text, or nothing when it isn't one. Set `tooLarge` instead,
// before anything is copied, when it holds more than kMaxFrameSourceRows
// strings or one longer than kMaxFrameSourceColumns code points could be.
std::optional<std::vector<std::u32string>> strings(const QJsonValue& v, bool& tooLarge) {
    if (!v.isArray() || v.toArray().isEmpty())
        return std::nullopt;
    const QJsonArray a = v.toArray();
    if (a.size() > kMaxFrameSourceRows) {
        tooLarge = true;
        return std::nullopt;
    }
    for (const QJsonValue& e : a) {
        if (!e.isString())
            return std::nullopt;
        // two UTF-16 units at most a code point
        if (e.toString().size() > 2 * qsizetype(kMaxFrameSourceColumns)) {
            tooLarge = true;
            return std::nullopt;
        }
    }
    std::vector<std::u32string> out;
    out.reserve(std::size_t(a.size()));
    for (const QJsonValue& e : a)
        out.push_back(e.toString().toStdU32String());
    return out;
}

std::string sourceLimit() {
    return std::to_string(kMaxFrameSourceColumns) + "x" + std::to_string(kMaxFrameSourceRows);
}

int hexDigit(char32_t c) {
    if (c >= U'0' && c <= U'9')
        return int(c - U'0');
    if (c >= U'a' && c <= U'f')
        return int(c - U'a' + 10);
    if (c >= U'A' && c <= U'F')
        return int(c - U'A' + 10);
    return -1;
}

} // namespace

std::string frameStyleJson(const FrameStyle& s) {
    QJsonObject o;
    o[QStringLiteral("id")] = QString::fromStdString(s.id);
    o[QStringLiteral("name")] = QString::fromStdString(s.name);
    o[QStringLiteral("insets")] = QJsonArray{s.left, s.right, s.top, s.bottom};
    o[QStringLiteral("fill")] = s.fill;
    if (s.minWidth || s.minHeight)
        o[QStringLiteral("min")] = QJsonArray{s.minWidth, s.minHeight};
    o[QStringLiteral("rows")] = rowsOf(s.source, 0);
    o[QStringLiteral("fg")] = rowsOf(s.source, 1);
    o[QStringLiteral("bg")] = rowsOf(s.source, 2);
    if (std::find(s.keepBg.begin(), s.keepBg.end(), true) != s.keepBg.end()) {
        QJsonArray keep;
        for (int y = 0; y < s.source.height(); ++y) {
            QString row;
            for (int x = 0; x < s.source.width(); ++x)
                row += QLatin1Char(s.keepsBackground(x, y) ? '1' : '.');
            keep.append(row);
        }
        o[QStringLiteral("keepBg")] = keep;
    }
    return QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
}

std::optional<FrameStyle> frameStyleFromJson(const std::string& json, std::string* error) {
    if (json.size() > kMaxFrameStyleBytes)
        return fail(error, "larger than " + std::to_string(kMaxFrameStyleBytes) + " bytes");
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError)
        return fail(error, "not JSON: " + err.errorString().toStdString());
    if (!doc.isObject())
        return fail(error, "not a JSON object");
    const QJsonObject o = doc.object();
    static const QSet<QString> known{QStringLiteral("id"),   QStringLiteral("name"), QStringLiteral("insets"),
                                     QStringLiteral("fill"), QStringLiteral("min"),  QStringLiteral("rows"),
                                     QStringLiteral("fg"),   QStringLiteral("bg"),   QStringLiteral("keepBg")};
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!known.contains(it.key()))
            return fail(error, "unknown key \"" + it.key().toStdString() + "\"");

    FrameStyle s;
    if (!o.value(QStringLiteral("id")).isString() || o.value(QStringLiteral("id")).toString().isEmpty())
        return fail(error, "\"id\" is not a name");
    s.id = o.value(QStringLiteral("id")).toString().toStdString();
    if (o.contains(QStringLiteral("name")) && !o.value(QStringLiteral("name")).isString())
        return fail(error, "\"name\" is not text");
    s.name = o.value(QStringLiteral("name")).toString(QString::fromStdString(s.id)).toStdString();
    const auto insets = ints(o.value(QStringLiteral("insets")), 4);
    if (!insets)
        return fail(error, "\"insets\" is not four whole numbers");
    s.left = (*insets)[0], s.right = (*insets)[1], s.top = (*insets)[2], s.bottom = (*insets)[3];
    if (s.left >= kMaxFrameSourceColumns || s.right >= kMaxFrameSourceColumns || s.top >= kMaxFrameSourceRows ||
        s.bottom >= kMaxFrameSourceRows)
        return fail(error, "\"insets\" are larger than a source may be (" + sourceLimit() + ")");
    if (o.contains(QStringLiteral("fill"))) {
        if (!o.value(QStringLiteral("fill")).isBool())
            return fail(error, "\"fill\" is not true or false");
        s.fill = o.value(QStringLiteral("fill")).toBool();
    }
    if (o.contains(QStringLiteral("min"))) {
        const auto min = ints(o.value(QStringLiteral("min")), 2);
        if (!min)
            return fail(error, "\"min\" is not two whole numbers");
        s.minWidth = (*min)[0], s.minHeight = (*min)[1];
        if (s.minWidth > kMaxColumns || s.minHeight > kMaxRows)
            return fail(error, "\"min\" is larger than a canvas (" + std::to_string(kMaxColumns) + "x" +
                                   std::to_string(kMaxRows) + ")");
    }
    bool tooLarge = false;
    const auto rows = strings(o.value(QStringLiteral("rows")), tooLarge);
    const auto fg = tooLarge ? std::nullopt : strings(o.value(QStringLiteral("fg")), tooLarge);
    const auto bg = tooLarge ? std::nullopt : strings(o.value(QStringLiteral("bg")), tooLarge);
    if (tooLarge)
        return fail(error, "the source is larger than a frame style's (" + sourceLimit() + ")");
    if (!rows || !fg || !bg)
        return fail(error, "\"rows\", \"fg\" and \"bg\" are not lists of text");
    const std::size_t w64 = rows->front().size(), h64 = rows->size();
    if (w64 == 0)
        return fail(error, "\"rows\" has no cells");
    if (w64 > std::size_t(kMaxFrameSourceColumns) || h64 > std::size_t(kMaxFrameSourceRows))
        return fail(error, "the source is larger than a frame style's (" + sourceLimit() + ")");
    const int w = int(w64), h = int(h64);
    if (int(fg->size()) != h || int(bg->size()) != h)
        return fail(error, "\"rows\", \"fg\" and \"bg\" have different numbers of rows");
    std::optional<std::vector<std::u32string>> keep;
    if (o.contains(QStringLiteral("keepBg"))) {
        keep = strings(o.value(QStringLiteral("keepBg")), tooLarge);
        if (!keep || int(keep->size()) != h)
            return fail(error, "\"keepBg\" is not a list of text as long as \"rows\"");
        s.keepBg.assign(std::size_t(w) * std::size_t(h), false);
    }
    s.source = Canvas(w, h);
    for (int y = 0; y < h; ++y) {
        if (int((*rows)[y].size()) != w || int((*fg)[y].size()) != w || int((*bg)[y].size()) != w ||
            (keep && int((*keep)[y].size()) != w))
            return fail(error, "row " + std::to_string(y + 1) + " is not " + std::to_string(w) + " cells long");
        for (int x = 0; x < w; ++x) {
            const char32_t ch = (*rows)[y][x];
            if (ch < U' ' || codePointWidth(ch) != 1)
                return fail(error, "row " + std::to_string(y + 1) + " has a glyph that isn't one cell wide");
            const int f = hexDigit((*fg)[y][x]);
            const bool clear = (*bg)[y][x] == U'.';
            const int b = clear ? 0 : hexDigit((*bg)[y][x]);
            if (f < 0 || b < 0)
                return fail(error, "row " + std::to_string(y + 1) + " has a colour that isn't 0-f");
            Cell& c = s.source.at(x, y);
            c.ch = ch;
            c.fg = Color::indexed(std::uint8_t(f));
            c.bg = Color::indexed(std::uint8_t(b));
            c.transparent = clear;
            if (keep) {
                const char32_t k = (*keep)[y][x];
                if (k != U'1' && k != U'.')
                    return fail(error, "row " + std::to_string(y + 1) + " of \"keepBg\" has a cell that isn't 1 or .");
                s.keepBg[std::size_t(y) * std::size_t(w) + std::size_t(x)] = k == U'1';
            }
        }
    }
    if (!s.valid())
        return fail(error, "the insets leave no edge to repeat: the source is " + std::to_string(w) + "x" +
                               std::to_string(h));
    return s;
}

std::optional<FrameStyle> frameStyleFromCells(const Canvas& cells, std::string id, std::string name, int left,
                                              int right, int top, int bottom, bool fill, bool keepBg,
                                              std::string* error) {
    FrameStyle s;
    s.id = std::move(id);
    s.name = std::move(name);
    s.left = left, s.right = right, s.top = top, s.bottom = bottom;
    s.fill = fill;
    if (cells.width() > kMaxFrameSourceColumns || cells.height() > kMaxFrameSourceRows)
        return fail(error, "the selection is " + std::to_string(cells.width()) + "x" +
                               std::to_string(cells.height()) + ", larger than a frame style's source (" +
                               sourceLimit() + ")");
    s.source = cells;
    if (!s.valid())
        return fail(error, "the corners leave no edge to repeat: the selection is " + std::to_string(cells.width()) +
                               "x" + std::to_string(cells.height()) + " and the corners " +
                               std::to_string(static_cast<long long>(left) + right) + "x" +
                               std::to_string(static_cast<long long>(top) + bottom) +
                               " (an edge needs a cell between them)");
    const int w = cells.width(), h = cells.height();
    if (keepBg)
        s.keepBg.assign(std::size_t(w) * std::size_t(h), false);
    bool draws = false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Cell& c = s.source.at(x, y);
            if (isBlank(c)) {
                c = Cell{};
                c.transparent = true;
                continue;
            }
            if (!fill && x >= left && x < w - right && y >= top && y < h - bottom)
                continue;  // the open middle: never drawn, so never checked
            const std::string at = "the cell at column " + std::to_string(x + 1) + ", row " + std::to_string(y + 1);
            const std::optional<char32_t> cp = c.ch.codePoint();
            if (!cp || *cp < U' ' || codePointWidth(*cp) != 1)
                return fail(error, at + " holds a glyph a frame style can't keep (one code point, one cell wide)");
            if (!c.fg.isIndexed() || c.fg.paletteIndex() > 15 || !c.bg.isIndexed() || c.bg.paletteIndex() > 15)
                return fail(error, at + " has a colour past the 16 a frame style keeps");
            if (c.attrs)
                return fail(error, at + " blinks, which a frame style doesn't keep");
            draws = true;
            if (keepBg)
                s.keepBg[std::size_t(y) * std::size_t(w) + std::size_t(x)] = true;
        }
    if (!draws)
        return fail(error, "every cell the frame would draw is blank");
    // The middle, when open, is written as it is drawn: transparent.
    if (!fill)
        for (int y = top; y < h - bottom; ++y)
            for (int x = left; x < w - right; ++x) {
                s.source.at(x, y) = Cell{};
                s.source.at(x, y).transparent = true;
            }
    return s;
}

namespace {

// A style from its rows of glyphs and of colours, as in the JSON.
FrameStyle style(std::string id, std::string name, std::vector<std::u32string> rows, std::vector<std::string> fg,
                 std::vector<std::string> bg, int inset = 1, bool fill = false, bool keepBg = false) {
    FrameStyle s;
    s.id = std::move(id);
    s.name = std::move(name);
    s.left = s.right = s.top = s.bottom = inset;
    s.fill = fill;
    const int w = int(rows.front().size()), h = int(rows.size());
    s.source = Canvas(w, h);
    if (keepBg)
        s.keepBg.assign(std::size_t(w) * std::size_t(h), false);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Cell& c = s.source.at(x, y);
            if (keepBg)
                s.keepBg[std::size_t(y) * std::size_t(w) + std::size_t(x)] = bg[y][x] != '.';
            c.ch = rows[y][x];
            c.fg = Color::indexed(std::uint8_t(hexDigit(char32_t(fg[y][x]))));
            c.transparent = bg[y][x] == '.';
            c.bg = Color::indexed(std::uint8_t(c.transparent ? 0 : hexDigit(char32_t(bg[y][x]))));
        }
    return s;
}

// A line one cell thick in `fg` round a transparent middle: three rows, the
// top and bottom edges' repeat as long as the middle row's. Every cell keeps
// the background under it (black on an empty canvas): a line's background is
// no part of it, nor is a dash's gap.
FrameStyle line(std::string id, std::string name, std::u32string top, std::u32string mid, std::u32string bottom,
                char fg) {
    const std::size_t w = top.size();
    const std::string f(w, fg), solid(w, '0'), open = "0" + std::string(w - 2, '.') + "0";
    return style(std::move(id), std::move(name), {std::move(top), std::move(mid), std::move(bottom)}, {f, f, f},
                 {solid, open, solid}, 1, false, true);
}

// Yellow and black stripes, two cells deep, at 45 degrees on screen (a
// cell is twice as tall as it is wide, so they step two columns a row).
FrameStyle hazard() {
    // The source: two rows or columns of corner, one period of stripe (eight
    // across, four down), two of corner. An edge cell is yellow when
    // (x + 2y) mod 8 < 4; the black ones are black blocks on yellow, so no
    // row ends in a black space, which IRC would drop. The corners are solid
    // yellow: an edge is cut wherever its length runs out, so a striped
    // corner would meet the right and bottom edges out of step (and leave a
    // lone yellow cell beside it), while a solid one meets any phase.
    std::vector<std::u32string> rows;
    std::vector<std::string> fg, bg;
    const int w = 12, h = 8;
    for (int y = 0; y < h; ++y) {
        std::u32string r;
        std::string f, b;
        for (int x = 0; x < w; ++x) {
            const bool across = x < 2 || x >= w - 2, down = y < 2 || y >= h - 2;
            const bool yellow = (across && down) || (x + 2 * y) % 8 < 4;
            r += (across || down) && !yellow ? U'█' : U' ';
            f += '0';
            b += across || down ? 'e' : '.';
        }
        rows.push_back(r);
        fg.push_back(f);
        bg.push_back(b);
    }
    return style("hazard", "Hazard stripes", rows, fg, bg, 2);
}

// ▓▒░ from the outside in, three cells deep on the sides and two on the top
// and bottom (▓▒), so the bands look as thick each way: a cell is twice as
// tall as it is wide. The shades keep the background under them, so the
// ring shades whatever it is dropped on.
FrameStyle shaded() {
    std::vector<std::u32string> rows;
    std::vector<std::string> fg, bg;
    const char32_t step[] = {U'▓', U'▒', U'░'};
    const int rowDepth[] = {0, 1, 3, 1, 0};
    for (int y = 0; y < 5; ++y) {
        std::u32string r;
        std::string b;
        for (int x = 0; x < 7; ++x) {
            const int depth = std::min({x, 6 - x, rowDepth[y]});
            r += depth < 3 ? step[depth] : U' ';
            b += depth < 3 ? '0' : '.';
        }
        rows.push_back(r);
        fg.push_back("bbbbbbb");
        bg.push_back(b);
    }
    FrameStyle s = style("shaded", "Shaded", rows, fg, bg, 3, false, true);
    s.top = s.bottom = 2;
    return s;
}

} // namespace

const std::vector<FrameStyle>& bundledFrameStyles() {
    static const std::vector<FrameStyle> all = [] {
        std::vector<FrameStyle> s;
        s.push_back(line("single", "Single line", U"┌─┐", U"│ │", U"└─┘", '7'));
        s.push_back(line("double", "Double line", U"╔═╗", U"║ ║", U"╚═╝", '7'));
        s.push_back(line("double-across", "Double across, single down", U"╒═╕", U"│ │", U"╘═╛", '7'));
        s.push_back(line("double-down", "Single across, double down", U"╓─╖", U"║ ║", U"╙─╜", '7'));
        s.push_back(line("heavy", "Heavy block", U"███", U"█ █", U"███", 'f'));
        // Half blocks: the top and bottom on the inner half of the cell, the
        // sides on the outer half, so the corners meet without a step. The
        // other half of each cell keeps the background under it.
        s.push_back(line("half-block", "Thin half-block", U"▄▄▄", U"▌ ▐", U"▀▀▀", 'b'));
        s.push_back(shaded());
        // Lit from the top left: white there, dark grey on the far sides.
        s.push_back(style("bevel", "Bevel", {U"██▀", U"█ █", U"▀██"}, {"fff", "f78", "f88"},
                          {"008", "0.0", "800"}));
        s.push_back(hazard());
        // Dashes of two with a one-cell gap across; the sides are whole,
        // since a gap at the right edge is a black space IRC drops.
        s.push_back(line("dashed", "Dashed line", U"┌── ┐", U"│   │", U"└── ┘", '7'));
        // A raised edge round a sunken one round a grey face, each a half
        // cell: white and dark grey in every edge cell, the face grey.
        s.push_back(style("button", "Double-bevel button", {U"▀▀▀", U"▌ ▌", U"▀▀▀"}, {"fff", "f7f", "fff"},
                          {"888", "878", "888"}, 1, true));
        return s;
    }();
    return all;
}

const FrameStyle* findFrameStyle(const std::string& id) {
    for (const FrameStyle& s : bundledFrameStyles())
        if (s.id == id)
            return &s;
    return nullptr;
}

} // namespace bw
