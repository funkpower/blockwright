#include "blockwright/project.h"

#include "blockwright/composite.h"
#include "blockwright/cp437.h"
#include "blockwright/text.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <zip.h>
#include <zstd.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#define BW_PROJECT_POSIX 1
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define BW_PROJECT_POSIX 0
#endif

namespace fs = std::filesystem;

namespace bw::project {
namespace {

constexpr char kMagic[] = "BWCL";
constexpr std::uint16_t kCellsVersion = 1;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kCellSize = 14;
constexpr std::uint32_t kMaxSide = 65535;
constexpr int kZstdLevel = 3;
constexpr std::uint8_t kFlagTransparent = 0x01;
// What a zip entry's time says: 1980-01-01 00:00, the first DOS date.
constexpr std::uint16_t kDosTime = 0, kDosDate = (1 << 5) | 1;
constexpr int kMaxRoundDigits = 9;  // r999999999.ans: an int either way
constexpr std::size_t kMaxBudget = 512;  // an IRC line, CR LF included
constexpr std::uint8_t kReservedFlags = 0xE0;  // SAUCE TFlags bits 5-7
constexpr std::size_t kMaxNameBytes = 255;     // a name on most disks (NAME_MAX)
constexpr std::size_t kMaxPathBytes = 1024;    // a path in a project, leaving room for where it goes
constexpr std::size_t kTmpNameBytes = 64;      // writeTemporary's temporary name, at most

[[noreturn]] void fail(const std::string& what) { throw ProjectError(what); }

void put16(std::string& out, std::uint16_t v) {
    out += char(v & 0xff);
    out += char(v >> 8);
}
void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out += char((v >> (8 * i)) & 0xff);
}
std::uint16_t get16(const unsigned char* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::uint32_t get32(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

void putColour(std::string& out, const Color& c) {
    if (c.isIndexed()) {
        out += char(0);
        out += char(c.index & 15);
        out += char(0);
        out += char(0);
    } else {
        out += char(1);
        out += char(c.rgb.r);
        out += char(c.rgb.g);
        out += char(c.rgb.b);
    }
}

Color getColour(const unsigned char* p, std::size_t at) {
    if (p[0] == 0) {
        if (p[1] > 15 || p[2] || p[3])
            fail("layer cells: cell " + std::to_string(at) + " has a palette colour past index 15");
        return Color::indexed(p[1]);
    }
    if (p[0] == 1)
        return Color::fromRgb({p[1], p[2], p[3]});
    fail("layer cells: cell " + std::to_string(at) + " has an unknown colour kind " + std::to_string(p[0]));
}

// ---- paths ----

std::vector<std::string_view> parts(std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        out.push_back(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos)
            return out;
        start = slash + 1;
    }
}

bool hiddenPart(std::string_view part) { return !part.empty() && part[0] == '.' && part != "." && part != ".."; }

// A path a reader skips: one with a hidden part (.git/, .DS_Store) and no
// "." or ".." that would make it something else.
bool hiddenPath(std::string_view path) {
    bool hidden = false;
    for (std::string_view p : parts(path)) {
        if (p == "." || p == "..")
            return false;
        hidden = hidden || hiddenPart(p);
    }
    return hidden;
}

// A canonical number: digits, no leading zero, not 0, at most `digits` long.
std::optional<std::uint64_t> canonicalNumber(std::string_view s, std::size_t digits) {
    if (s.empty() || s.size() > digits || s[0] == '0')
        return std::nullopt;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return std::nullopt;
        v = v * 10 + std::uint64_t(c - '0');
    }
    return v;
}

// layers/<id>.json and .cells: the id, and whether it is the .cells file.
std::optional<std::pair<LayerId, bool>> layerFile(std::string_view path) {
    if (!path.starts_with("layers/"))
        return std::nullopt;
    std::string_view name = path.substr(7);
    bool cells;
    if (name.ends_with(".json")) {
        cells = false;
        name.remove_suffix(5);
    } else if (name.ends_with(".cells")) {
        cells = true;
        name.remove_suffix(6);
    } else {
        return std::nullopt;
    }
    const auto id = canonicalNumber(name, 10);
    if (!id || *id > UINT32_MAX)
        return std::nullopt;
    return std::pair{LayerId(*id), cells};
}

// snapshots/r<N>.ans: the round.
std::optional<int> snapshotFile(std::string_view path) {
    if (!path.starts_with("snapshots/r") || !path.ends_with(".ans"))
        return std::nullopt;
    const auto n = canonicalNumber(path.substr(11, path.size() - 15), kMaxRoundDigits);
    if (!n)
        return std::nullopt;
    return int(*n);
}

// fonts/<sha256>.tdf: the hash.
std::optional<std::string_view> fontFile(std::string_view path) {
    if (!path.starts_with("fonts/") || !path.ends_with(".tdf"))
        return std::nullopt;
    const std::string_view sha = path.substr(6, path.size() - 10);
    if (!isSha256Hex(sha))
        return std::nullopt;
    return sha;
}

bool ownedPath(std::string_view path) {
    return path == "manifest.json" || path == "notes.md" || layerFile(path) || snapshotFile(path) || fontFile(path);
}

std::string layerPath(LayerId id, bool cells) { return "layers/" + std::to_string(id) + (cells ? ".cells" : ".json"); }
std::string snapshotPath(int round) { return "snapshots/r" + std::to_string(round) + ".ans"; }
std::string fontPath(std::string_view sha) { return "fonts/" + std::string(sha) + ".tdf"; }

// A path as a case-insensitive, normalisation-insensitive disk may take it:
// Unicode's canonical caseless form, NFD(fold(NFD(path))), with full case
// folding ("ß" and "ẞ" are "ss", "ﬁ" is "fi", as Linux casefold takes them).
// Qt's toCaseFolded is simple folding, so the path is upper-cased in full
// between two simple folds: the first takes "ẞ", already upper-case, to "ß"
// so that it upper-cases to "SS". The upper-casing also makes "ı" one with
// "I", which over-refuses harmlessly.
QString foldedPath(std::string_view path) {
    return QString::fromUtf8(path.data(), qsizetype(path.size()))
        .normalized(QString::NormalizationForm_D)
        .toCaseFolded()
        .toUpper()
        .toCaseFolded()
        .normalized(QString::NormalizationForm_D);
}

// Refuses two paths samePath takes for one: each file and each directory on
// the way to one has a single spelling, and no path is both.
template <typename Paths>
void checkPaths(const Paths& paths) {
    std::map<QString, std::pair<std::string_view, bool>> seen;  // folded: as spelled, a file
    for (const auto& entry : paths) {
        const std::string_view path = entry.first;
        for (std::size_t end = 0;; ++end) {
            end = path.find('/', end);
            const bool file = end == std::string_view::npos;
            const std::string_view at = path.substr(0, end);
            const auto [it, fresh] = seen.try_emplace(foldedPath(at), at, file);
            if (!fresh && it->second.first != at)
                fail("\"" + std::string(it->second.first) + "\" and \"" + std::string(at) +
                     "\" are one path on some disks");
            if (!fresh && (file || it->second.second))
                fail("\"" + std::string(at) + "\" is a file and a directory");
            if (file)
                break;
        }
    }
}

// ---- JSON ----

void checkJsonSize(const std::string& name, std::size_t size) {
    if (size > kMaxJsonBytes)
        fail(name + " is over " + std::to_string(kMaxJsonBytes >> 10) + " KiB");
}

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
std::string utf8(const QString& s) { return s.toStdString(); }

std::string toJson(const QJsonObject& o) {
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Indented);
    return std::string(b.constData(), std::size_t(b.size()));
}

// A key some object in `json` holds twice, or nothing. `json` is valid JSON
// (Qt has parsed it); QJsonDocument keeps only the last of a repeated key,
// so repeats are found here, in every object at any depth.
std::optional<QString> repeatedKey(std::string_view json) {
    std::vector<std::optional<std::set<QString>>> open;  // nullopt: an array
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{') {
            open.emplace_back(std::set<QString>{});
        } else if (c == '[') {
            open.emplace_back(std::nullopt);
        } else if ((c == '}' || c == ']') && !open.empty()) {
            open.pop_back();
        } else if (c == '"') {
            const std::size_t start = i;
            for (++i; i < json.size() && json[i] != '"'; ++i)
                if (json[i] == '\\')
                    ++i;
            if (open.empty() || !open.back())
                continue;
            std::size_t j = i + 1;
            while (j < json.size() && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r'))
                ++j;
            if (j < json.size() && json[j] == ':') {
                const std::string raw = "[" + std::string(json.substr(start, i - start + 1)) + "]";
                const QString key = QJsonDocument::fromJson(QByteArray::fromStdString(raw)).array().at(0).toString();
                if (!open.back()->insert(key).second)
                    return key;
            }
        }
    }
    return std::nullopt;
}

// A JSON entry as an object; one over kMaxJsonBytes is refused before it is
// parsed (Qt's parse takes about ten times the text).
QJsonObject parseObject(const std::string& text, const std::string& what) {
    checkJsonSize(what, text.size());
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(text.data(), qsizetype(text.size())), &err);
    if (err.error != QJsonParseError::NoError)
        fail(what + " is not JSON: " + utf8(err.errorString()) + " at offset " + std::to_string(err.offset));
    if (!doc.isObject())
        fail(what + " is not a JSON object");
    if (const auto key = repeatedKey(text))
        fail(what + " has the key \"" + utf8(*key) + "\" more than once");
    return doc.object();
}

// Reads the keys a format version holds, each of one type; a key it
// doesn't know is a newer minor version's and is skipped.
class Reader {
public:
    Reader(const QJsonObject& o, std::string what) : o_(o), what_(std::move(what)) {}

    QJsonValue value(const char* key) const {
        const QJsonValue v = o_.value(QLatin1String(key));
        if (v.isUndefined())
            fail(what_ + " has no \"" + key + "\"");
        return v;
    }
    std::string string(const char* key) const {
        const QJsonValue v = value(key);
        if (!v.isString())
            fail(what_ + ": \"" + key + "\" is not a string");
        return utf8(v.toString());
    }
    bool boolean(const char* key) const {
        const QJsonValue v = value(key);
        if (!v.isBool())
            fail(what_ + ": \"" + key + "\" is not true or false");
        return v.toBool();
    }
    std::int64_t integer(const char* key, std::int64_t lo, std::int64_t hi) const {
        return integerOf(value(key), key, lo, hi);
    }
    std::int64_t integerOf(const QJsonValue& v, const std::string& key, std::int64_t lo, std::int64_t hi) const {
        const double d = v.toDouble(std::nan(""));
        if (!v.isDouble() || !std::isfinite(d) || d != std::floor(d) || d < double(lo) || d > double(hi))
            fail(what_ + ": \"" + key + "\" is not a whole number from " + std::to_string(lo) + " to " +
                 std::to_string(hi));
        return std::int64_t(d);
    }
    QJsonArray array(const char* key) const {
        const QJsonValue v = value(key);
        if (!v.isArray())
            fail(what_ + ": \"" + key + "\" is not a list");
        return v.toArray();
    }
    QJsonObject object(const char* key) const {
        const QJsonValue v = value(key);
        if (!v.isObject())
            fail(what_ + ": \"" + key + "\" is not an object");
        return v.toObject();
    }
    bool has(const char* key) const { return o_.contains(QLatin1String(key)); }
    const std::string& what() const { return what_; }

private:
    QJsonObject o_;
    std::string what_;
};

// ---- SAUCE in the manifest ----

// A SAUCE text field: CP437 bytes in the record, UTF-8 in the manifest.
QJsonValue sauceText(const std::string& cp437) { return qstr(cp437ToUtf8(cp437)); }

void checkSauceText(const std::string& cp437, const std::string& what, std::size_t maxLen) {
    if (cp437.size() > maxLen)
        fail("manifest: SAUCE " + what + " is over " + std::to_string(maxLen) + " characters");
}

void checkSauceComments(std::size_t lines) {
    if (lines > Sauce::kMaxComments)
        fail("manifest: SAUCE has over " + std::to_string(Sauce::kMaxComments) + " comment lines");
}

std::string sauceBytes(const QJsonValue& v, const std::string& what, std::size_t maxLen) {
    if (!v.isString())
        fail("manifest: SAUCE " + what + " is not a string");
    std::string out;
    for (char32_t cp : v.toString().toStdU32String()) {
        const auto b = unicodeToCp437(cp);
        if (!b)
            fail("manifest: SAUCE " + what + " has a character with no CP437 byte (U+" + [&] {
                char hex[16];
                std::snprintf(hex, sizeof hex, "%04X", unsigned(cp));
                return std::string(hex);
            }() + ")");
        out += char(*b);
    }
    checkSauceText(out, what, maxLen);
    return out;
}

// What sauceFromJson refuses of a record's text, refused before a writer
// writes it.
void checkSauce(const Sauce& s) {
    checkSauceText(s.title, "title", Sauce::kTitleLen);
    checkSauceText(s.author, "author", Sauce::kAuthorLen);
    checkSauceText(s.group, "group", Sauce::kGroupLen);
    checkSauceText(s.date, "date", 8);
    checkSauceText(s.font, "font", Sauce::kFontLen);
    checkSauceComments(s.comments.size());
    for (const std::string& c : s.comments)
        checkSauceText(c, "comment", Sauce::kCommentLen);
}

QJsonObject sauceJson(const Sauce& s) {
    QJsonArray comments;
    for (const std::string& c : s.comments)
        comments.append(sauceText(c));
    QJsonValue spacing = QJsonValue::Null;
    if (s.letterSpacing() == 1)
        spacing = 8;
    else if (s.letterSpacing() == 2)
        spacing = 9;
    else if (s.letterSpacing() == 3)
        spacing = "reserved";
    const int a = s.aspectRatio();
    const char* aspect = a == 1 ? "legacy" : a == 2 ? "square" : a == 3 ? "reserved" : "none";
    return QJsonObject{
        {"title", sauceText(s.title)},   {"author", sauceText(s.author)}, {"group", sauceText(s.group)},
        {"date", sauceText(s.date)},     {"font", sauceText(s.font)},     {"comments", comments},
        {"letterSpacing", spacing},      {"aspectRatio", QLatin1String(aspect)},
        {"tinfo3", s.tinfo3},            {"tinfo4", s.tinfo4},           {"reservedFlags", s.flags & kReservedFlags},
    };
}

Sauce sauceFromJson(const Reader& r) {
    Sauce s;
    s.title = sauceBytes(r.value("title"), "title", Sauce::kTitleLen);
    s.author = sauceBytes(r.value("author"), "author", Sauce::kAuthorLen);
    s.group = sauceBytes(r.value("group"), "group", Sauce::kGroupLen);
    s.date = sauceBytes(r.value("date"), "date", 8);
    s.font = sauceBytes(r.value("font"), "font", Sauce::kFontLen);
    const QJsonArray comments = r.array("comments");
    checkSauceComments(std::size_t(comments.size()));
    for (const QJsonValue& c : comments)
        s.comments.push_back(sauceBytes(c, "comment", Sauce::kCommentLen));
    const QJsonValue spacing = r.value("letterSpacing");
    if (spacing == QJsonValue("reserved")) {
        s.setLetterSpacing(3);
    } else if (!spacing.isNull()) {
        const auto px = r.integerOf(spacing, "letterSpacing", 8, 9);
        s.setLetterSpacing(px == 8 ? 1 : 2);
    }
    const std::string aspect = r.string("aspectRatio");
    if (aspect == "legacy")
        s.setAspectRatio(1);
    else if (aspect == "square")
        s.setAspectRatio(2);
    else if (aspect == "reserved")
        s.setAspectRatio(3);
    else if (aspect != "none")
        fail("manifest: SAUCE aspectRatio is not none, legacy, square or reserved");
    s.tinfo3 = std::uint16_t(r.integer("tinfo3", 0, 65535));
    s.tinfo4 = std::uint16_t(r.integer("tinfo4", 0, 65535));
    const auto reserved = r.integer("reservedFlags", 0, 255);
    if (reserved & ~kReservedFlags)
        fail("manifest: SAUCE reservedFlags holds more than TFlags bits 5 to 7");
    s.flags = std::uint8_t(s.flags | reserved);
    return s;
}

// The fields a project keeps of a SAUCE record, every one an .ans save
// keeps (writeAnsi), the rest as flatten writes them: ANSi, the canvas size,
// the project's iCE, FileSize 0 (writeAnsi sets it).
Sauce normalSauce(const Sauce& from, int width, int height, bool ice) {
    Sauce s = from;
    s.fileSize = 0;
    s.dataType = 1;
    s.fileType = 1;
    s.tinfo1 = std::uint16_t(width);
    s.tinfo2 = std::uint16_t(height);
    s.setIce(ice);
    return s;
}

// ---- layers ----

// A live text layer's source (text.h): the text, the Text tool's settings
// and the face, named by its kind and the fields that kind has. A source in
// a pixel face is the layer's "source"; one in a TheDraw font (1.2) is its
// "tdfSource", the same shape, so a 1.1 reader, which refuses a face kind
// it doesn't know, drops the key instead and shows the saved cells.
const char* sourceKey(const TextSource& t) { return t.face.kind == FaceRef::Kind::Tdf ? "tdfSource" : "source"; }

QJsonObject textSourceJson(const TextSource& t) {
    QJsonObject face{{"kind", qstr(faceKindName(t.face.kind))}};
    switch (t.face.kind) {
    case FaceRef::Kind::Pixel: face["name"] = qstr(t.face.name); break;
    case FaceRef::Kind::Tdf:
        face["file"] = qstr(t.face.file);
        face["index"] = t.face.index;
        face["name"] = qstr(t.face.name);
        break;
    }
    return QJsonObject{
        {"text", QString::fromStdU32String(t.text)},
        {"face", face},
        {"case", qstr(textCaseName(t.textCase))},
        {"letterSpacing", t.letterSpacing},
        {"lineSpacing", t.lineSpacing},
        {"align", qstr(textAlignName(t.align))},
        {"wrapWidth", t.wrapWidth},
        {"colour", int(t.colour)},
    };
}

TextSource textSourceFromJson(const QJsonObject& o, const std::string& what, const char* key,
                              const FontStore& fonts) {
    const Reader r(o, what + " " + key);
    TextSource t;
    const QJsonValue text = r.value("text");
    if (!text.isString())
        fail(r.what() + ": \"text\" is not a string");
    t.text = text.toString().toStdU32String();
    const Reader f(r.object("face"), r.what() + " face");
    const auto kind = faceKindFromName(f.string("kind"));
    if (!kind)
        fail(f.what() + ": unknown face kind \"" + f.string("kind") + "\"");
    t.face.kind = *kind;
    if (std::string_view(sourceKey(t)) != key)
        fail(f.what() + ": a " + f.string("kind") + " face under \"" + key + "\"");
    switch (*kind) {
    case FaceRef::Kind::Pixel: t.face.name = f.string("name"); break;
    case FaceRef::Kind::Tdf:
        t.face.file = f.string("file");
        if (!isSha256Hex(t.face.file))
            fail(f.what() + ": \"file\" is not a sha256 in lowercase hex");
        t.face.index = int(f.integer("index", 0, kMaxTdfFonts - 1));
        t.face.name = f.string("name");
        if (!fonts.file(t.face.file))
            fail(f.what() + ": no " + fontPath(t.face.file) + " in the project");
        break;
    }
    const auto textCase = textCaseFromName(r.string("case"));
    if (!textCase)
        fail(r.what() + ": unknown case \"" + r.string("case") + "\"");
    t.textCase = *textCase;
    t.letterSpacing = int(r.integer("letterSpacing", 0, TextSource::kMaxSpacing));
    t.lineSpacing = int(r.integer("lineSpacing", 0, TextSource::kMaxSpacing));
    const auto align = textAlignFromName(r.string("align"));
    if (!align)
        fail(r.what() + ": unknown alignment \"" + r.string("align") + "\"");
    t.align = *align;
    t.wrapWidth = int(r.integer("wrapWidth", 0, TextSource::kMaxWrapWidth));
    t.colour = std::uint8_t(r.integer("colour", 0, 15));
    if (const auto problem = textSourceProblem(t, {&fonts, nullptr}))
        fail(r.what() + ": " + *problem);
    return t;
}

QJsonObject layerJson(const Layer& l) {
    const LayerProperties& p = l.props;
    QJsonObject o{
        {"id", qint64(p.id)},
        {"name", qstr(p.name)},
        {"kind", qstr(layerKindName(p.kind))},
        {"x", p.x},
        {"y", p.y},
        {"width", l.width()},
        {"height", l.height()},
        {"visible", p.visible},
        {"locked", p.locked},
        {"alphaLocked", p.alphaLocked},
        {"mode", qstr(layerModeName(p.mode))},
    };
    if (p.text)
        o[QLatin1String(sourceKey(*p.text))] = textSourceJson(*p.text);
    return o;
}

Layer layerFromEntries(LayerId id, const std::string& json, const std::string& cells, const FontStore& fonts) {
    const std::string what = layerPath(id, false);
    const Reader r(parseObject(json, what), what);
    Layer l;
    LayerProperties& p = l.props;
    if (LayerId(r.integer("id", 1, UINT32_MAX)) != id)
        fail(what + ": \"id\" is not " + std::to_string(id));
    p.id = id;
    p.name = r.string("name");
    const auto kind = layerKindFromName(r.string("kind"));
    if (!kind)
        fail(what + ": unknown layer kind \"" + r.string("kind") + "\"");
    p.kind = *kind;
    const auto mode = layerModeFromName(r.string("mode"));
    if (!mode)
        fail(what + ": unknown layer mode \"" + r.string("mode") + "\"");
    p.mode = *mode;
    p.x = int(r.integer("x", INT_MIN, INT_MAX));
    p.y = int(r.integer("y", INT_MIN, INT_MAX));
    p.visible = r.boolean("visible");
    p.locked = r.boolean("locked");
    p.alphaLocked = r.boolean("alphaLocked");
    // a live text layer's source; the cells are its drawn text as saved
    if (r.has("source") && r.has("tdfSource"))
        fail(what + " has both a \"source\" and a \"tdfSource\"");
    for (const char* key : {"source", "tdfSource"})
        if (r.has(key)) {
            if (p.kind != LayerKind::Text)
                fail(what + ": a " + std::string(layerKindName(p.kind)) + " layer has a text source");
            p.text = textSourceFromJson(r.object(key), what, key, fonts);
        }
    const auto w = r.integer("width", 0, kMaxSide);
    const auto h = r.integer("height", 0, kMaxSide);
    try {
        l.cells = decodeCells(cells);
    } catch (const ProjectError& e) {
        fail(layerPath(id, true) + ": " + e.what());
    }
    if (l.width() != w || l.height() != h)
        fail(what + " says " + std::to_string(w) + "x" + std::to_string(h) + ", " + layerPath(id, true) +
             " holds " + std::to_string(l.width()) + "x" + std::to_string(l.height()));
    return l;
}

// "major.minor", each a number.
std::pair<int, int> parseVersion(const std::string& v) {
    const std::size_t dot = v.find('.');
    const auto number = [&](std::string_view s) -> int {
        if (s.empty() || s.size() > 4 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
            fail("manifest: version \"" + v + "\" is not major.minor");
        return std::stoi(std::string(s));
    };
    if (dot == std::string::npos)
        fail("manifest: version \"" + v + "\" is not major.minor");
    return {number(std::string_view(v).substr(0, dot)), number(std::string_view(v).substr(dot + 1))};
}

// ---- files ----

#if BW_PROJECT_POSIX
// A descriptor, closed when this goes unless it is let go. Every one the
// save and the read open is taken into one as the call returns it, before
// anything that can throw, so none is left open whatever fails.
class Descriptor {
public:
    explicit Descriptor(int fd) noexcept : fd_(fd) {}
    Descriptor(Descriptor&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Descriptor& operator=(Descriptor&&) = delete;
    ~Descriptor() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_;
};

// Reads the `size` bytes of `p`, opened as `fd` without following a link or
// waiting on a FIFO, if it is a plain file of that size.
std::string readOpened(Descriptor fd, const fs::path& p, std::size_t size) {
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode))
        fail(p.string() + " is not a file");
    if (std::uint64_t(st.st_size) != size)
        fail(p.string() + " changed while it was read");
    std::string out(size, '\0');
    if (testing::directoryStep)
        testing::directoryStep("reading", p);
    std::size_t got = 0;
    while (got < size) {
        const ssize_t n = ::read(fd.get(), out.data() + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += std::size_t(n);
    }
    char extra;
    ssize_t more;
    do
        more = ::read(fd.get(), &extra, 1);
    while (more < 0 && errno == EINTR);
    if (got != size || more != 0)
        fail(p.string() + " changed while it was read");
    return out;
}
#endif

// Reads the `size` bytes of the file a directory walk found at `p`. What is
// there now may not be what the walk saw, so it is opened without following
// a link or waiting on a FIFO, and read only if it is a plain file of that
// size.
std::string readWhole(const fs::path& p, std::size_t size) {
#if BW_PROJECT_POSIX
    Descriptor fd(::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0)
        fail(errno == ELOOP ? p.string() + " is a symbolic link" : "can't read " + p.string() + ": " + std::strerror(errno));
    return readOpened(std::move(fd), p, size);
#else
    std::string out(size, '\0');
    std::ifstream in(p, std::ios::binary);
    if (!in)
        fail("can't read " + p.string());
    in.read(out.data(), std::streamsize(size));
    if (std::size_t(in.gcount()) != size || in.peek() != std::char_traits<char>::eof())
        fail(p.string() + " changed while it was read");
    return out;
#endif
}

void checkSize(const std::string& name, std::uint64_t size, std::uint64_t& total) {
    if (size > kMaxEntryBytes)
        fail(name + " is over " + std::to_string(kMaxEntryBytes >> 20) + " MiB");
    total += size;
    if (total > kMaxTotalBytes)
        fail("the project is over " + std::to_string(kMaxTotalBytes >> 20) + " MiB");
}

void checkCount(std::size_t entries) {
    if (entries > kMaxEntries)
        fail("the project holds over " + std::to_string(kMaxEntries) + " files");
}

// The sizes and number of a project's entries, against the limits.
void checkEntries(const Entries& entries) {
    checkCount(entries.size());
    std::uint64_t total = 0;
    for (const auto& [name, bytes] : entries)
        checkSize(name, bytes.size(), total);
}

// A font copy's size, and the number of them so far with this one, against
// kMaxTdfBytes and kMaxFontFiles: what decode checks, checked by a reader
// from the size the archive or the disk gives, before the entry is read.
void checkFontEntry(const std::string& name, std::uint64_t size, std::size_t& fonts) {
    if (!fontFile(name))
        return;
    if (++fonts > kMaxFontFiles)
        fail("the project holds over " + std::to_string(kMaxFontFiles) + " font files");
    if (size > kMaxTdfBytes)
        fail(name + " is over " + std::to_string(kMaxTdfBytes >> 20) + " MiB");
}

// Takes an entry from a zip or a directory: skipped if hidden, refused if
// unsafe, repeated or one too many.
bool takeEntry(Entries& out, const std::string& name) {
    if (hiddenPath(name))
        return false;
    if (!safeEntryPath(name))
        fail("the project holds an unsafe path \"" + name + "\"");
    if (out.count(name))
        fail("the project holds \"" + name + "\" twice");
    checkCount(out.size() + 1);
    return true;
}

// What a zip entry is by its UNIX mode, if it has one: a file or directory
// as its name says, or refused (a link, FIFO, device, socket).
void checkEntryType(zip_t* z, zip_uint64_t i, const std::string& name) {
    zip_uint8_t system = 0;
    zip_uint32_t attributes = 0;
    if (zip_file_get_external_attributes(z, i, 0, &system, &attributes) != 0)
        fail("can't read entry " + name + ": " + zip_strerror(z));
    if (system != ZIP_OPSYS_UNIX)
        return;
    const zip_uint32_t type = (attributes >> 16) & 0170000;
    const bool directory = name.ends_with('/');
    if (type == 0 || type == (directory ? 0040000u : 0100000u))
        return;
    fail("the project holds \"" + name + "\", which is " +
         (type == 0120000 ? "a symbolic link" : directory ? "not a directory" : "not a file"));
}

// What libzip's error `code` says; the record it is read from (which can
// hold text libzip allocated) freed whatever copying it out throws.
std::string zipErrorText(int code) {
    zip_error_t e;
    zip_error_init_with_code(&e, code);
    const struct Fini {
        zip_error_t* e;
        ~Fini() { zip_error_fini(e); }
    } fini{&e};
    return zip_error_strerror(&e);
}

Entries readZip(const fs::path& path) {
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &err);
    if (!z)
        fail(path.string() + " is not a project zip: " + zipErrorText(err));
    struct Closer {
        zip_t* z;
        ~Closer() { zip_discard(z); }
    } closer{z};
    Entries out;
    std::uint64_t total = 0;
    std::size_t fonts = 0;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; ++i) {
        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(z, zip_uint64_t(i), ZIP_FL_ENC_GUESS, &st) != 0 || !(st.valid & ZIP_STAT_NAME) ||
            !(st.valid & ZIP_STAT_SIZE))
            fail(path.string() + ": can't read entry " + std::to_string(i) + ": " + zip_strerror(z));
        std::string name = st.name;
        checkEntryType(z, zip_uint64_t(i), name);
        if (name.ends_with('/')) {  // a directory: its files say what's in it
            name.pop_back();
            if (!name.empty() && !hiddenPath(name) && !safeEntryPath(name))
                fail("the project holds an unsafe path \"" + name + "/\"");
            continue;
        }
        if (!takeEntry(out, name))
            continue;
        if ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) && st.encryption_method != ZIP_EM_NONE)
            fail(path.string() + ": " + name + " is encrypted");
        checkSize(name, st.size, total);
        checkFontEntry(name, st.size, fonts);
        std::string bytes(std::size_t(st.size), '\0');
        zip_file_t* f = zip_fopen_index(z, zip_uint64_t(i), 0);
        if (!f)
            fail(path.string() + ": can't open " + name + ": " + zip_strerror(z));
        const struct FileCloser {
            zip_file_t* f;
            ~FileCloser() { zip_fclose(f); }
        } fileCloser{f};
        const zip_int64_t got = st.size ? zip_fread(f, bytes.data(), st.size) : 0;
        char extra;
        const zip_int64_t more = zip_fread(f, &extra, 1);
        if (got != zip_int64_t(st.size) || more != 0)
            fail(path.string() + ": " + name + " is damaged");
        out.emplace(std::move(name), std::move(bytes));
    }
    return out;
}

Entries readDirectory(const fs::path& root) {
    Entries out;
    std::uint64_t total = 0;
    std::size_t fonts = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec), end;
    if (ec)
        fail("can't read " + root.string() + ": " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec)
            fail("can't read " + root.string() + ": " + ec.message());
        const fs::directory_entry& e = *it;
        const std::string name = e.path().lexically_relative(root).generic_string();
        // never part of the project, whatever it is: not opened, followed or
        // looked into
        if (hiddenPath(name)) {
            it.disable_recursion_pending();
            continue;
        }
        if (e.is_symlink(ec))
            fail(root.string() + ": " + name + " is a symbolic link");
        if (e.is_directory(ec))
            continue;
        if (!e.is_regular_file(ec))
            fail(root.string() + ": " + name + " is not a file");
        takeEntry(out, name);
        const std::uint64_t size = e.file_size(ec);
        if (ec)
            fail("can't read " + e.path().string() + ": " + ec.message());
        checkSize(name, size, total);
        checkFontEntry(name, size, fonts);
        if (testing::directoryStep)
            testing::directoryStep("read", e.path());
        out.emplace(name, readWhole(e.path(), std::size_t(size)));
    }
    return out;
}

long processId() {
#if BW_PROJECT_POSIX
    return long(::getpid());
#else
    return 0;
#endif
}

// A file's identity, the same under every name it has: device and inode,
// or {0, 0} where the system has none.
using FileId = std::pair<std::uint64_t, std::uint64_t>;

// A directory a save works in. On POSIX systems it is held open, and every
// name in it is reached through that descriptor, so what the save does
// there, and what a failed save puts back, stays in this directory even if
// it is moved or a link is put where it was. Elsewhere it is reached by its
// path each time.
struct Folder {
    fs::path path;  // as the save spells it
#if BW_PROJECT_POSIX
    int fd = -1;
    Folder(fs::path p, Descriptor d) : path(std::move(p)), fd(d.release()) {}
    Folder(Folder&& o) noexcept : path(std::move(o.path)), fd(std::exchange(o.fd, -1)) {}
    Folder& operator=(Folder&&) = delete;
    ~Folder() {
        if (fd >= 0)
            ::close(fd);
    }
#else
    explicit Folder(fs::path p) : path(std::move(p)) {}
#endif
    fs::path operator/(const std::string& name) const { return path / fs::path(name); }
};

// The directory `path` itself, a link to one followed.
Folder openRoot(const fs::path& path) {
#if BW_PROJECT_POSIX
    Descriptor fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.get() < 0)
        fail("can't read " + path.string() + ": " + std::strerror(errno));
    return Folder(path, std::move(fd));
#else
    return Folder(path);
#endif
}

#if BW_PROJECT_POSIX
// The errno a test has the system call `call` on `name` in `in` fail with,
// or 0.
int injected(std::string_view call, const Folder& in, const std::string& name) {
    return testing::systemCall ? testing::systemCall(call, in / name) : 0;
}
#endif

enum class Kind { None, File, Directory, Link, Other };

// What is at `name` in `in`, a link not followed.
Kind kindAt(const Folder& in, const std::string& name) {
#if BW_PROJECT_POSIX
    struct stat st {};
    if (::fstatat(in.fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT)
            return Kind::None;
        fail("can't read " + (in / name).string() + ": " + std::strerror(errno));
    }
    return S_ISLNK(st.st_mode)   ? Kind::Link
           : S_ISDIR(st.st_mode) ? Kind::Directory
           : S_ISREG(st.st_mode) ? Kind::File
                                 : Kind::Other;
#else
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(in / name, ec);
    return !fs::exists(st)           ? Kind::None
           : fs::is_symlink(st)      ? Kind::Link
           : fs::is_directory(st)    ? Kind::Directory
           : fs::is_regular_file(st) ? Kind::File
                                     : Kind::Other;
#endif
}

// Makes the directory `name` in `in` if nothing is there; whether it did.
bool makeFolder(const Folder& in, const std::string& name) {
#if BW_PROJECT_POSIX
    int err = injected("mkdirat", in, name);
    if (!err && ::mkdirat(in.fd, name.c_str(), 0777) != 0)
        err = errno;
    if (err) {
        if (err != EEXIST)
            fail("can't create " + (in / name).string() + ": " + std::strerror(err));
        return false;
    }
    return true;
#else
    if (kindAt(in, name) != Kind::None)
        return false;
    std::error_code ec;
    const bool made = fs::create_directory(in / name, ec);
    if (ec)
        fail("can't create " + (in / name).string() + ": " + ec.message());
    return made;
#endif
}

// Sets `id` to the device and inode of the directory `name` in `in`, a
// link not followed ({0, 0} for anything but a directory, and on other
// systems than POSIX), so a directory the save made can be told from one
// put in its place later. Gives the errno looking failed with, or 0.
int folderId(const Folder& in, const std::string& name, FileId& id) {
    id = {};
#if BW_PROJECT_POSIX
    if (const int err = injected("fstatat", in, name))
        return err;
    struct stat st {};
    if (::fstatat(in.fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno;
    if (S_ISDIR(st.st_mode))
        id = {std::uint64_t(st.st_dev), std::uint64_t(st.st_ino)};
#else
    (void)in;
    (void)name;
#endif
    return 0;
}

// The directory `name` in `in`; a link there, or anything but a directory,
// is refused.
Folder openFolder(const Folder& in, const std::string& name) {
#if BW_PROJECT_POSIX
    Descriptor fd(::openat(in.fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() >= 0)
        return Folder(in / name, std::move(fd));
    const int err = errno;
    const Kind kind = kindAt(in, name);
    if (kind == Kind::Link)
        fail((in / name).string() + " is a symbolic link");
    if (kind != Kind::Directory)
        fail((in / name).string() + " is not a directory");
    fail("can't read " + (in / name).string() + ": " + std::strerror(err));
#else
    const Kind kind = kindAt(in, name);
    if (kind == Kind::Link)
        fail((in / name).string() + " is a symbolic link");
    if (kind != Kind::Directory)
        fail((in / name).string() + " is not a directory");
    return Folder(in / name);
#endif
}

// The directory `name` in `in`, a link to one followed: one on the way to a
// project, which is its owner's to link.
Folder enterFolder(const Folder& in, const std::string& name) {
#if BW_PROJECT_POSIX
    Descriptor fd(::openat(in.fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.get() < 0)
        fail("can't read " + (in / name).string() + ": " + std::strerror(errno));
    return Folder(in / name, std::move(fd));
#else
    if (kindAt(in, name) == Kind::None || !fs::is_directory(in / name))
        fail((in / name).string() + " is not a directory");
    return Folder(in / name);
#endif
}

// The names in `in`, but `.` and `..`, listed through its descriptor
// (POSIX).
std::vector<std::string> namesIn(const Folder& in) {
    std::vector<std::string> out;
#if BW_PROJECT_POSIX
    Descriptor fd(::dup(in.fd));  // closedir closes it, and `in` keeps its own
    DIR* d = fd.get() < 0 ? nullptr : ::fdopendir(fd.get());
    if (!d) {
        const int err = errno;
        fail("can't read " + in.path.string() + ": " + std::strerror(err));
    }
    fd.release();
    const struct Closer {
        DIR* d;
        ~Closer() { ::closedir(d); }
    } closer{d};
    ::rewinddir(d);  // the offset is the one `in`'s descriptor has
    int err = 0;
    for (;;) {
        errno = 0;
        const dirent* e = ::readdir(d);
        if (!e) {
            err = errno;
            break;
        }
        const std::string_view name = e->d_name;
        if (name != "." && name != "..")
            out.emplace_back(name);
    }
    if (err)
        fail("can't read " + in.path.string() + ": " + std::strerror(err));
#else
    std::error_code ec;
    for (fs::directory_iterator it(in.path, ec), end; !ec && it != end; it.increment(ec))
        out.push_back(it->path().filename().string());
    if (ec)
        fail("can't read " + in.path.string() + ": " + ec.message());
#endif
    return out;
}

// The bytes of the plain file `name` in `in`, a link not followed and a
// FIFO not waited on.
std::string readIn(const Folder& in, const std::string& name) {
#if BW_PROJECT_POSIX
    Descriptor fd(::openat(in.fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0)
        fail("can't read " + (in / name).string() + ": " + std::strerror(errno));
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0)
        fail("can't read " + (in / name).string() + ": " + std::strerror(errno));
    return readOpened(std::move(fd), in / name, std::size_t(st.st_size));
#else
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(in / name, ec);
    if (ec)
        fail("can't read " + (in / name).string() + ": " + ec.message());
    return readWhole(in / name, std::size_t(size));
#endif
}

// A hidden name no one is likely to have taken, `kind` saying what it holds
// ("tmp" or "old"). It never holds the name of the file it stands beside, so
// a name as long as the disk takes has one too.
std::string hiddenName(const char* kind) {
    static unsigned counter = 0;
    static const unsigned long salt = std::random_device{}();
    char name[kTmpNameBytes];
    std::snprintf(name, sizeof name, ".bw-%s-%lx-%lx-%x", kind, salt, processId(), counter++);
    return name;
}

// Renames `from` to `to` in `in`.
std::error_code renameIn(const Folder& in, const std::string& from, const std::string& to) {
#if BW_PROJECT_POSIX
    if (::renameat(in.fd, from.c_str(), in.fd, to.c_str()) != 0)
        return {errno, std::generic_category()};
    return {};
#else
    std::error_code ec;
    fs::rename(in / from, in / to, ec);
    return ec;
#endif
}

// Removes `name` from `in`: a file, or an empty directory if `folder`.
std::error_code removeIn(const Folder& in, const std::string& name, bool folder) {
#if BW_PROJECT_POSIX
    if (const int err = injected("unlinkat", in, name))
        return {err, std::generic_category()};
    if (::unlinkat(in.fd, name.c_str(), folder ? AT_REMOVEDIR : 0) != 0)
        return {errno, std::generic_category()};
    return {};
#else
    if (folder && kindAt(in, name) != Kind::Directory)
        return std::make_error_code(std::errc::not_a_directory);
    std::error_code ec;
    fs::remove(in / name, ec);
    return ec;
#endif
}

// How a rename that mustn't replace anything went: `error` if the file
// isn't at its new name alone, and `twice` if it is left under both names.
struct Renamed {
    std::error_code error;
    bool twice = false;
};

// Renames `from` to `to` in `in` unless something is at `to`
// (`file_exists` if so), never replacing what is there however it got
// there. On POSIX systems that takes renameat2's RENAME_NOREPLACE or a hard
// link; a disk with neither refuses the rename. Elsewhere `to` is looked at
// first. Not for a directory.
Renamed renameFresh(const Folder& in, const std::string& from, const std::string& to) {
#if BW_PROJECT_POSIX && defined(RENAME_NOREPLACE)
    {
        int err = injected("renameat2", in, to);
        if (!err && ::renameat2(in.fd, from.c_str(), in.fd, to.c_str(), RENAME_NOREPLACE) != 0)
            err = errno;
        if (!err)
            return {};
        if (err != EINVAL && err != ENOSYS)
            return {{err, std::generic_category()}};
    }
#endif
#if BW_PROJECT_POSIX
    // where renameat2 can't refuse (or there is none): a second name for the
    // file, which link refuses where something is (and never follows a link
    // there), then the first name removed, or the second if that can't be
    if (const int e = injected("noreplace", in, to))
        return {{e, std::generic_category()}};
    int err = injected("linkat", in, to);
    if (!err && ::linkat(in.fd, from.c_str(), in.fd, to.c_str(), 0) != 0)
        err = errno;
    if (err)
        return {{err, std::generic_category()}};
    if (const std::error_code e = removeIn(in, from, false))
        return {e, bool(removeIn(in, to, false))};
    return {};
#else
    if (kindAt(in, to) != Kind::None)
        return {std::make_error_code(std::errc::file_exists)};
    return {renameIn(in, from, to)};
#endif
}

struct Temporary {
    std::string name;      // in the folder it was written in
    FileId id;             // taken from the descriptor it was written through
    bool created = false;  // whether the file at `name` is this one
};

// Writes `bytes` to a new hidden file in `in`, for renaming over `name`
// there, created exclusively under a name no one can have put there first
// (and never through a link). Fills in `tmp` (the caller's record of it,
// which nothing after the file is made can fail to fill): its name before
// the file is made, and once it is that it was, and its identity, which is
// that file's whatever is put at `name` afterwards. A name already in `tmp`
// is tried first, so one chosen beforehand lets a write that finds it free
// allocate nothing. On a failure nothing is left, or `tmp` still says it
// was made.
void writeTemporary(const Folder& in, const std::string& name, const std::string& bytes, Temporary& tmp) {
    tmp.id = {};
    tmp.created = false;
    const auto give_up = [&](const std::string& why) {
        std::string left;
        if (tmp.created) {
            if (const std::error_code e = removeIn(in, tmp.name, false))
                left = "; and its temporary " + (in / tmp.name).string() + " couldn't be removed: " + e.message();
            else
                tmp.created = false;
        }
        fail("can't write " + (in / name).string() + ": " + why + left);
    };
    for (int attempt = 0;; ++attempt) {
        if (attempt > 0 || tmp.name.empty())
            tmp.name = hiddenName("tmp");
#if BW_PROJECT_POSIX
        int fd = -1;
        int err = injected("openat", in, tmp.name);
        if (!err && (fd = ::openat(in.fd, tmp.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666)) < 0)
            err = errno;
        if (fd < 0) {
            if (err == EEXIST && attempt < 100)
                continue;
            give_up(std::strerror(err));
        }
        tmp.created = true;
        const char* data = bytes.data();
        std::size_t left = bytes.size();
        while (left && !err) {
            const ssize_t n = ::write(fd, data, left);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                err = n < 0 ? errno : EIO;
                break;
            }
            data += n;
            left -= std::size_t(n);
        }
        struct stat st {};
        if (!err && ::fstat(fd, &st) != 0)
            err = errno;
        tmp.id = {std::uint64_t(st.st_dev), std::uint64_t(st.st_ino)};
        if (::close(fd) != 0 && !err)
            err = errno;
        if (err)
            give_up(std::strerror(err));
#else
        if (kindAt(in, tmp.name) != Kind::None) {
            if (attempt < 100)
                continue;
            give_up("no free temporary name");
        }
        const fs::path at = in / tmp.name;
        tmp.created = true;
        std::ofstream out(at, std::ios::binary);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.close();
        if (!out)
            give_up("write failed");
#endif
        break;
    }
}

} // namespace

// ---- .cells ----

std::string encodeCells(const Canvas& cells) {
    const std::uint32_t w = std::uint32_t(cells.width()), h = std::uint32_t(cells.height());
    if (w > kMaxSide || h > kMaxSide || std::size_t(w) * h > kMaxLayerCells)
        fail("a layer of " + std::to_string(w) + "x" + std::to_string(h) + " cells is over the limits");
    std::string raw;
    raw.reserve(std::size_t(w) * h * kCellSize);
    for (int y = 0; y < cells.height(); ++y)
        for (int x = 0; x < cells.width(); ++x) {
            const Cell& c = cells.at(x, y);
            put32(raw, std::uint32_t(c.ch));
            putColour(raw, c.fg);
            putColour(raw, c.bg);
            raw += char(c.attrs);
            raw += char(c.transparent ? kFlagTransparent : 0);
        }
    std::string out(kMagic, 4);
    put16(out, kCellsVersion);
    put16(out, 0);
    put32(out, w);
    put32(out, h);
    std::string frame(ZSTD_compressBound(raw.size()), '\0');
    const std::size_t n = ZSTD_compress(frame.data(), frame.size(), raw.data(), raw.size(), kZstdLevel);
    if (ZSTD_isError(n))
        fail(std::string("can't compress layer cells: ") + ZSTD_getErrorName(n));
    frame.resize(n);
    return out + frame;
}

namespace {

// The cells a .cells file says it holds, from its header; nothing for one
// decodeCells would refuse before it got that far.
std::optional<std::size_t> cellsCount(std::string_view data) {
    if (data.size() < kHeaderSize || data.substr(0, 4) != std::string_view(kMagic, 4))
        return std::nullopt;
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    const std::uint32_t w = get32(p + 8), h = get32(p + 12);
    if (w > kMaxSide || h > kMaxSide || std::size_t(w) * h > kMaxLayerCells)
        return std::nullopt;
    return std::size_t(w) * h;
}

void checkCells(std::size_t layers, std::size_t cells) {
    if (layers > kMaxLayers)
        fail("the project has over " + std::to_string(kMaxLayers) + " layers");
    if (cells > kMaxTotalCells)
        fail("the project's layers hold over " + std::to_string(kMaxTotalCells) + " cells");
}

} // namespace

Canvas decodeCells(std::string_view data) {
    if (data.size() < kHeaderSize || data.substr(0, 4) != std::string_view(kMagic, 4))
        fail("layer cells: not a .cells file");
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (get16(p + 4) != kCellsVersion)
        fail("layer cells: format version " + std::to_string(get16(p + 4)) + ", this build reads " +
             std::to_string(kCellsVersion));
    if (get16(p + 6) != 0)
        fail("layer cells: the reserved header field is set");
    const std::uint32_t w = get32(p + 8), h = get32(p + 12);
    if (w > kMaxSide || h > kMaxSide || std::size_t(w) * h > kMaxLayerCells)
        fail("layer cells: " + std::to_string(w) + "x" + std::to_string(h) + " is over the limits");
    const std::size_t count = std::size_t(w) * h, expected = count * kCellSize;
    const std::string_view frame = data.substr(kHeaderSize);
    const std::size_t frameSize = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    if (ZSTD_isError(frameSize))
        fail(std::string("layer cells: ") + ZSTD_getErrorName(frameSize));
    if (frameSize != frame.size())
        fail("layer cells: bytes after the cells");
    const unsigned long long content = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR)
        fail("layer cells: the frame doesn't say its size");
    if (content != expected)
        fail("layer cells: the frame holds " + std::to_string(content / kCellSize) + " cells, the header says " +
             std::to_string(count));
    std::string raw(expected, '\0');
    const std::size_t got = ZSTD_decompress(raw.data(), raw.size(), frame.data(), frame.size());
    if (ZSTD_isError(got))
        fail(std::string("layer cells: ") + ZSTD_getErrorName(got));
    if (got != expected)
        fail("layer cells: the frame is short");

    Canvas out(static_cast<int>(w), static_cast<int>(h));
    const auto* r = reinterpret_cast<const unsigned char*>(raw.data());
    for (std::size_t i = 0; i < count; ++i, r += kCellSize) {
        const std::uint32_t cp = get32(r);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            fail("layer cells: cell " + std::to_string(i) + " is not a Unicode character");
        if (r[13] & ~kFlagTransparent)
            fail("layer cells: cell " + std::to_string(i) + " has an unknown flag");
        Cell& c = out.at(int(i % w), int(i / w));
        c.ch = char32_t(cp);
        c.fg = getColour(r + 4, i);
        c.bg = getColour(r + 8, i);
        c.attrs = r[12];
        c.transparent = (r[13] & kFlagTransparent) != 0;
    }
    return out;
}

// ---- projects ----

Project fromAnsi(const AnsiDocument& from, Opened* opened) {
    AnsiDocument doc = from;
    const Opened o = openAsEditor(doc);
    if (opened)
        *opened = o;
    Project p;
    p.layers = LayerStack::fromCanvas(doc.canvas);
    p.ice = doc.ice;
    if (doc.sauce)
        p.sauce = normalSauce(*doc.sauce, doc.canvas.width(), doc.canvas.height(), doc.ice);
    return p;
}

AnsiDocument toAnsi(const Project& project) {
    CompositeOptions o;
    o.ice = project.ice;
    AnsiDocument doc;
    doc.canvas = flatten(project.layers, o);
    doc.ice = project.ice;
    // as the editor saves a document (Document::encode)
    for (int y = 0; y < doc.canvas.height(); ++y)
        for (int x = 0; x < doc.canvas.width(); ++x)
            if (Cell& c = doc.canvas.at(x, y); !c.transparent)
                c = iceCell(c, project.ice);
    const int w = doc.canvas.width(), h = doc.canvas.height();
    doc.sauce = project.sauce ? normalSauce(*project.sauce, w, h, project.ice) : [&] {
        Sauce s = Sauce::fresh(w, h);
        s.setIce(project.ice);
        return s;
    }();
    return doc;
}

AnsiOutput takeSnapshot(Project& project) {
    int last = 0;
    for (const Snapshot& s : project.snapshots)
        last = std::max(last, s.round);
    if (last >= 999999999)
        fail("no rounds left after r" + std::to_string(last));
    AnsiOutput out = writeAnsi(toAnsi(project));
    project.snapshots.push_back({last + 1, out.bytes});
    return out;
}

bool samePath(std::string_view a, std::string_view b) { return foldedPath(a) == foldedPath(b); }

bool safeEntryPath(std::string_view path) {
    if (path.empty() || path.size() > kMaxPathBytes)
        return false;
    for (char c : path)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == '\\' || c == ':')
            return false;
    for (std::string_view p : parts(path))
        if (p.empty() || p == "." || p == ".." || p[0] == '.' || p.size() > kMaxNameBytes)
            return false;
    return true;
}

Entries encode(const Project& project) {
    const LayerStack& stack = project.layers;
    if (stack.width() < 1 || stack.width() > kMaxColumns || stack.height() < 1 || stack.height() > kMaxRows)
        fail("a canvas of " + std::to_string(stack.width()) + "x" + std::to_string(stack.height()) +
             " is over the limits");
    if (project.ircBudget.profile.empty() || project.ircBudget.bytes < 1 || project.ircBudget.bytes > kMaxBudget)
        fail("an IRC budget of " + std::to_string(project.ircBudget.bytes) + " bytes is out of range");
    std::size_t cells = 0;
    for (const Layer& l : stack.layers())
        cells += std::size_t(l.width()) * std::size_t(l.height());
    checkCells(stack.size(), cells);
    if (project.sauce)
        checkSauce(*project.sauce);
    checkCount(project.files.size());
    Entries out;
    std::size_t fonts = 0;  // distinct font files written
    for (const auto& [path, bytes] : project.files) {
        if (!safeEntryPath(path))
            fail("\"" + path + "\" is not a path a project can hold");
        if (ownedPath(path))
            fail("\"" + path + "\" is the project's own file");
        if (bytes.size() > kMaxEntryBytes)  // before it is copied
            fail(path + " is over " + std::to_string(kMaxEntryBytes >> 20) + " MiB");
        out.emplace(path, bytes);
    }

    QJsonArray order;
    for (const Layer& l : stack.layers()) {
        order.append(qint64(l.props.id));
        if (l.props.text) {
            const std::string what = layerPath(l.props.id, false);
            if (l.props.kind != LayerKind::Text)
                fail(what + ": a " + std::string(layerKindName(l.props.kind)) + " layer has a text source");
            if (const auto problem = textSourceProblem(*l.props.text, {&project.fonts, nullptr}))
                fail(what + " " + sourceKey(*l.props.text) + ": " + *problem);
            // each font file the layers use, once
            if (l.props.text->face.kind == FaceRef::Kind::Tdf) {
                const FontFilePtr font = project.fonts.file(l.props.text->face.file);
                if (font->bytes.size() > kMaxTdfBytes)  // before it is copied
                    fail(fontPath(font->sha256) + " is over " + std::to_string(kMaxTdfBytes >> 20) + " MiB");
                if (out.emplace(fontPath(font->sha256), font->bytes).second && ++fonts > kMaxFontFiles)
                    fail("the project's text layers use over " + std::to_string(kMaxFontFiles) + " font files");
            }
        }
        const std::string json = toJson(layerJson(l));
        checkJsonSize(layerPath(l.props.id, false), json.size());
        out[layerPath(l.props.id, false)] = json;
        out[layerPath(l.props.id, true)] = encodeCells(l.cells);
    }
    const QJsonObject manifest{
        {"format", "blockwright"},
        {"version", qstr(std::to_string(kFormatMajor) + "." + std::to_string(kFormatMinor))},
        {"width", stack.width()},
        {"height", stack.height()},
        {"palette", "cga"},
        {"ice", project.ice},
        {"ircBudget", QJsonObject{{"profile", qstr(project.ircBudget.profile)},
                                  {"bytes", qint64(project.ircBudget.bytes)}}},
        {"sauce", project.sauce ? QJsonValue(sauceJson(*project.sauce)) : QJsonValue(QJsonValue::Null)},
        {"layers", order},
        {"nextLayerId", qint64(stack.nextId())},
    };
    out["manifest.json"] = toJson(manifest);
    checkJsonSize("manifest.json", out["manifest.json"].size());
    if (!project.notes.empty())
        out["notes.md"] = project.notes;
    std::set<int> rounds;
    for (const Snapshot& s : project.snapshots) {
        if (s.round < 1 || s.round > 999999999 || !rounds.insert(s.round).second)
            fail("snapshot round " + std::to_string(s.round) + " is out of range or repeated");
        out[snapshotPath(s.round)] = s.ans;
    }
    // what decode refuses, refused before anything is written
    checkEntries(out);
    checkPaths(out);
    return out;
}

Project decode(const Entries& entries) {
    checkEntries(entries);
    checkPaths(entries);
    const auto manifestIt = entries.find("manifest.json");
    if (manifestIt == entries.end())
        fail("not a project: no manifest.json");
    const Reader m(parseObject(manifestIt->second, "manifest.json"), "manifest.json");
    if (m.string("format") != "blockwright")
        fail("manifest.json: not a blockwright project");
    const std::string version = m.string("version");
    if (const auto [major, minor] = parseVersion(version); major != kFormatMajor) {
        (void)minor;
        fail("a project in format " + version + ": this build reads " + std::to_string(kFormatMajor) + ".x");
    }

    Project p;
    const int w = int(m.integer("width", 1, kMaxColumns));
    const int h = int(m.integer("height", 1, kMaxRows));
    if (m.string("palette") != "cga")
        fail("manifest.json: palette \"" + m.string("palette") + "\": this build has only cga");
    p.ice = m.boolean("ice");
    {
        const Reader b(m.object("ircBudget"), "manifest.json ircBudget");
        p.ircBudget.profile = b.string("profile");
        if (p.ircBudget.profile.empty())
            fail("manifest.json: the IRC budget has no profile");
        p.ircBudget.bytes = std::size_t(b.integer("bytes", 1, kMaxBudget));
    }
    if (const QJsonValue s = m.value("sauce"); !s.isNull()) {
        if (!s.isObject())
            fail("manifest.json: \"sauce\" is not an object or null");
        Sauce sauce = sauceFromJson(Reader(s.toObject(), "manifest.json sauce"));
        p.sauce = normalSauce(sauce, w, h, p.ice);
    }

    // the font copies, counted, then each checked against its name, before
    // the layers that use them
    if (std::size_t(std::count_if(entries.begin(), entries.end(),
                                  [](const auto& e) { return fontFile(e.first).has_value(); })) > kMaxFontFiles)
        fail("the project holds over " + std::to_string(kMaxFontFiles) + " font files");
    for (const auto& [path, bytes] : entries) {
        const auto sha = fontFile(path);
        if (!sha)
            continue;
        if (bytes.size() > kMaxTdfBytes)
            fail(path + " is over " + std::to_string(kMaxTdfBytes >> 20) + " MiB");
        if (sha256Hex(bytes) != *sha)
            fail(path + ": its bytes hash to " + sha256Hex(bytes) + ", not its name");
        try {
            p.fonts.add(loadFontFile(bytes));
        } catch (const TdfError& e) {
            fail(path + ": " + e.what());
        }
    }

    p.layers = LayerStack(w, h);
    // the cells every layer will hold, from the .cells headers, before any
    // is decoded
    const QJsonArray layers = m.array("layers");
    checkCells(std::size_t(layers.size()), 0);
    std::size_t cells = 0;
    for (const QJsonValue& v : layers) {
        const auto c = entries.find(layerPath(LayerId(m.integerOf(v, "layers", 1, UINT32_MAX)), true));
        if (c != entries.end())
            cells += cellsCount(c->second).value_or(0);
    }
    checkCells(std::size_t(layers.size()), cells);
    std::set<LayerId> listed;
    for (const QJsonValue& v : layers) {
        const LayerId id = LayerId(m.integerOf(v, "layers", 1, UINT32_MAX));
        if (!listed.insert(id).second)
            fail("manifest.json lists layer " + std::to_string(id) + " twice");
        const auto json = entries.find(layerPath(id, false));
        const auto cells = entries.find(layerPath(id, true));
        if (json == entries.end() || cells == entries.end())
            fail("layer " + std::to_string(id) + " has no " + layerPath(id, json != entries.end()));
        p.layers.add(layerFromEntries(id, json->second, cells->second, p.fonts));
    }
    const LayerId next = LayerId(m.integer("nextLayerId", 0, UINT32_MAX));
    if (next != 0 && !listed.empty() && next <= *listed.rbegin())
        fail("manifest.json: nextLayerId " + std::to_string(next) + " is not past every layer id");
    p.layers.reserveIds(next);

    for (const auto& [path, bytes] : entries) {
        if (path == "manifest.json")
            continue;
        if (path == "notes.md") {
            p.notes = bytes;
        } else if (const auto lf = layerFile(path)) {
            if (!listed.count(lf->first))
                fail(path + " is a layer manifest.json doesn't list");
        } else if (const auto round = snapshotFile(path)) {
            p.snapshots.push_back({*round, bytes});
        } else if (fontFile(path)) {
            // read above
        } else {
            p.files.emplace(path, bytes);
        }
    }
    std::sort(p.snapshots.begin(), p.snapshots.end(),
              [](const Snapshot& a, const Snapshot& b) { return a.round < b.round; });
    return p;
}

bool isProject(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec))
        return fs::is_regular_file(path / "manifest.json", ec);
    if (!fs::is_regular_file(path, ec))
        return false;
    std::ifstream in(path, std::ios::binary);
    char sig[4] = {};
    in.read(sig, 4);
    return in.gcount() == 4 && std::memcmp(sig, "PK\x03\x04", 4) == 0;
}

Project read(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec))
        return decode(readDirectory(path));
    if (!fs::exists(path, ec))
        fail(path.string() + " doesn't exist");
    return decode(readZip(path));
}

void writeZip(const Project& project, const fs::path& path) {
    const Entries entries = encode(project);
    std::error_code ec;
    if (fs::is_directory(path, ec))
        fail(path.string() + " is a directory");
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!z)
        fail("can't write " + path.string() + ": " + zipErrorText(err));
    // discarded however the save leaves, unless it is closed
    struct Discard {
        zip_t* z;
        ~Discard() {
            if (z)
                zip_discard(z);
        }
    } discard{z};
    const auto abandon = [&](const std::string& what) {
        fail("can't write " + path.string() + ": " + what + ": " + zip_strerror(z));
    };
    for (const auto& [name, bytes] : entries) {
        zip_source_t* src = zip_source_buffer(z, bytes.data(), bytes.size(), 0);
        if (!src)
            abandon(name);
        const zip_int64_t i = zip_file_add(z, name.c_str(), src, ZIP_FL_ENC_UTF_8);
        if (i < 0) {
            zip_source_free(src);
            abandon(name);
        }
        const bool stored = name.ends_with(".cells") || name.ends_with(".png");
        if (zip_set_file_compression(z, zip_uint64_t(i), stored ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0) != 0 ||
            zip_file_set_dostime(z, zip_uint64_t(i), kDosTime, kDosDate, 0) != 0)
            abandon(name);
    }
    if (zip_close(z) != 0)
        abandon("closing");
    discard.z = nullptr;
}

void writeDirectory(const Project& project, const fs::path& path) {
    const Entries entries = encode(project);
    // an empty path is no directory; `path / name` would write into the
    // working one, past the check that it holds a project
    if (path.empty())
        fail("can't write a project to an empty path");
    // the failure a rollback that left something, and ran out of memory
    // saying what, ends in: made before anything is changed, and thrown as a
    // copy, which allocates nothing
    const ProjectError unnameable("can't write " + path.string() +
                                  ": the save failed, and some of what it changed couldn't be put back as it was "
                                  "and couldn't be named (it ran out of memory)");
#ifdef PATH_MAX
    // each file, and the temporary one beside it, as long a path as the
    // system opens
    for (const auto& [name, bytes] : entries)
        if ((path / fs::path(name)).native().size() + kTmpNameBytes >= PATH_MAX)
            fail("can't write " + (path / fs::path(name)).string() + ": the path is too long");
#endif
    std::error_code ec;
    const bool existed = fs::exists(path, ec) || fs::is_symlink(path, ec);
    // the directory there, opened once and looked at through that, so the
    // one found to hold a project is the one the save works in
    std::optional<Folder> found;
    if (existed) {
        if (!fs::is_directory(path, ec))
            fail(path.string() + " is not a directory");
        found.emplace(openRoot(path));
        if (kindAt(*found, "manifest.json") != Kind::File)
            for (const std::string& name : namesIn(*found))
                if (!hiddenPart(name))
                    fail(path.string() + " is not empty and holds no project");
    }
    // Every entry is written to a temporary beside it first, each directory
    // on the way made or found, never a link, and only once all of them are
    // written are they renamed into place, the manifest last. Then each file
    // the project no longer holds is renamed aside to a hidden name, and
    // only once all of them are aside is the save done and are they removed.
    // A failure at any point before that puts the directory back as it found
    // it: the files set aside renamed back, each file a rename replaced given
    // its old bytes again, each one it added removed, the temporaries
    // removed, then every directory the save made, the project's own and its
    // parents included (last made first, so each is empty once what the save
    // put in it is gone; one that isn't, someone else's file in it, stays,
    // and one that was there before, or was put where the save's was, is
    // never the save's). So a failed save
    // over a project leaves the project that was there, and a failed first
    // save isn't refused next time as "not a project". Each directory in the
    // project is held open as the save first reaches it (POSIX), so all of
    // this happens in the directories the save found or made, wherever they
    // are moved and whatever link is put in their place meanwhile.
    std::map<std::string, Folder> folders;  // by path in the project; "" is the project's
    // how a rollback's step on a record failed, kept in the record, so
    // taking the step allocates nothing and every step is taken before any
    // failure is named
    struct Undo {
        std::error_code error;
        std::exception_ptr failure;  // what it threw, if it threw
        bool twice = false;          // a file left under both names
        explicit operator bool() const { return error || failure; }
    };
    struct Made {
        const Folder* in;
        std::string name;
        // as made, so one put in its place later is never taken for it;
        // nothing if looking at it failed, when it is removed only if empty
        std::optional<FileId> id;
        bool made = false;  // whether the save made it (else it is never removed)
        Undo undo = {};
    };
    // makes the directory `name` in `in` if nothing is there, recorded in
    // `into` before it is made, so it is the save's from the moment it is,
    // with nothing that can fail between, then known by its identity;
    // whether it made it
    const auto make = [](std::vector<Made>& into, const Folder& in, const std::string& name) {
        into.push_back({&in, name, std::nullopt});
        Made& d = into.back();
        if (!makeFolder(in, name)) {
            into.pop_back();
            return false;
        }
        d.made = true;
        FileId id;
        if (const int err = folderId(in, name, id))
            fail("can't read " + (in / name).string() + ": " + std::strerror(err));
        d.id = id;
        return true;
    };
    std::deque<Folder> above;  // the directories on the way to the project's, as the save opened them
    std::vector<Made> madeDirs;  // the project's own and its parents, each in the one above it
    std::vector<Made> madeIn;    // inside the project
    // the folder `rel` in the project, opened through the ones on its way
    // (made where `create` says, and nothing is there)
    const auto folder = [&](const std::string& rel, bool create) -> const Folder& {
        const Folder* in = &folders.at("");
        std::string at;
        for (const std::string_view part : parts(rel)) {
            at += (at.empty() ? "" : "/") + std::string(part);
            auto it = folders.find(at);
            if (it == folders.end()) {
                const std::string name(part);
                if (create)
                    make(madeIn, *in, name);
                it = folders.emplace(at, openFolder(*in, name)).first;
            }
            in = &it->second;
        }
        return *in;
    };
    // `name` in the project as the folder holding it and its name there
    const auto split = [](const std::string& name) {
        const std::size_t slash = name.rfind('/');
        return slash == std::string::npos ? std::pair<std::string, std::string>{"", name}
                                          : std::pair{name.substr(0, slash), name.substr(slash + 1)};
    };

    struct Staged {
        const Folder* in = nullptr;
        std::string name;                // in `in`
        Temporary tmp;                   // in `in`
        std::optional<std::string> old;  // the bytes of the file at `name`, if one was there
        Temporary back;                  // the old bytes, written by a rollback; named beforehand
        bool renamed = false;
        Undo undo;
    };
    std::vector<Staged> staged;
    struct Aside {
        const Folder* in;
        std::string name, aside;  // in `in`
        // left under both names by a move aside that couldn't remove the
        // first: only `aside` is the save's
        bool both = false;
        Undo undo = {};
    };
    std::vector<Aside> asides;
    // what the save writes, so the stale scan never takes it for another file
#if BW_PROJECT_POSIX
    std::set<FileId> written;
#endif
    const auto stage = [&](const std::string& name, const std::string& bytes) {
        const auto [dir, file] = split(name);
        Staged s;
        s.in = &folder(dir, true);
        s.name = file;
        // a file is replaced only once its bytes are read, so a failure can
        // give them back; one that can't be read refuses the save. A
        // directory there makes the rename fail, so it is never replaced.
        const Kind kind = kindAt(*s.in, file);
        if (kind == Kind::Link)
            fail((*s.in / file).string() + " is a symbolic link");
        if (kind == Kind::Other)
            fail((*s.in / file).string() + " is not a file");
        if (kind == Kind::File) {
            s.old = readIn(*s.in, file);
            s.back.name = hiddenName("tmp");
        }
        // recorded before its temporary is made, which then can't go
        // unrecorded
        staged.push_back(std::move(s));
        Staged& t = staged.back();
        writeTemporary(*t.in, file, bytes, t.tmp);
#if BW_PROJECT_POSIX
        written.insert(t.tmp.id);
#endif
    };
#if BW_PROJECT_POSIX
    // a descriptor held from before the first file is replaced, which a
    // rollback lets go of first: putting a file back takes one, one at a
    // time, so a save that runs out of them (the scan holds open every
    // directory it looks in) can still put back what it replaced
    std::optional<Folder> spare;
#endif
    // the directory `d` the save made, removed if it is still that one
    // (POSIX: by identity) and empty; why it couldn't be, if it is there
    const auto unmake = [](const Made& d) -> std::error_code {
        if (!d.made)
            return {};
        if (d.id) {
            FileId id;
            if (const int err = folderId(*d.in, d.name, id))
                return err == ENOENT ? std::error_code{} : std::error_code{err, std::generic_category()};
            if (id != *d.id)  // not the one it made
                return {};
        }
        const std::error_code e = removeIn(*d.in, d.name, true);
        return e == std::errc::no_such_file_or_directory ? std::error_code{} : e;
    };
    // what couldn't be put back: named, and how many more couldn't be
    struct Unrestored {
        std::string named;
        std::size_t unnamed = 0;
        bool any() const { return !named.empty() || unnamed; }
    };
    // Every step is taken, whatever the others did, and how one failed is
    // kept in its record; a rollback with nothing failing allocates
    // nothing. Only then is each failure named, a second time if the first
    // runs out of memory, and one that still can't be is counted instead.
    const auto rollBack = [&]() noexcept {
#if BW_PROJECT_POSIX
        spare.reset();
#endif
        const auto take = [](Undo& undo, const auto& step) {
            try {
                step();
            } catch (...) {
                undo.failure = std::current_exception();
            }
        };
        const auto gone = [](std::error_code e) {
            return e == std::errc::no_such_file_or_directory ? std::error_code{} : e;
        };
        for (auto a = asides.rbegin(); a != asides.rend(); ++a)
            take(a->undo, [&] {
                if (a->both) {
                    a->undo.error = gone(removeIn(*a->in, a->aside, false));
                    return;
                }
                const Renamed r = renameFresh(*a->in, a->aside, a->name);
                a->undo.error = r.error;
                a->undo.twice = r.twice;
            });
        for (auto s = staged.rbegin(); s != staged.rend(); ++s)
            take(s->undo, [&] {
                if (!s->renamed) {
                    if (s->tmp.created)
                        s->undo.error = gone(removeIn(*s->in, s->tmp.name, false));
                } else if (s->old) {
                    writeTemporary(*s->in, s->name, *s->old, s->back);
                    s->undo.error = renameIn(*s->in, s->back.name, s->name);
                } else {
                    s->undo.error = gone(removeIn(*s->in, s->name, false));
                }
            });
        for (std::vector<Made>* list : {&madeIn, &madeDirs})
            for (auto d = list->rbegin(); d != list->rend(); ++d)
                take(d->undo, [&] { d->undo.error = unmake(*d); });

        Unrestored out;
        const auto why = [](const Undo& undo) -> std::string {
            if (undo.error)
                return undo.error.message();
            try {
                std::rethrow_exception(undo.failure);
            } catch (const std::exception& e) {
                return e.what();
            } catch (...) {
                return "an unknown error";
            }
        };
        const auto report = [&](const Undo& undo, const auto& line) {
            if (!undo)
                return;
            for (int attempt = 0; attempt < 2; ++attempt)
                try {
                    out.named += (out.named.empty() ? "" : "; ") + line();
                    return;
                } catch (...) {
                }
            ++out.unnamed;
        };
        for (auto a = asides.rbegin(); a != asides.rend(); ++a)
            report(a->undo, [&] {
                if (a->both)
                    return (*a->in / a->aside).string() + ": " + why(a->undo) + " (a second name for " + a->name +
                           " beside it)";
                return (*a->in / a->name).string() + ": " + why(a->undo) + " (it is " + a->aside + " beside it" +
                       (a->undo.twice ? ", and under its own name" : "") + ")";
            });
        for (auto s = staged.rbegin(); s != staged.rend(); ++s)
            report(s->undo, [&] {
                if (!s->renamed)
                    return (*s->in / s->tmp.name).string() + ": " + why(s->undo);
                if (s->old && s->undo.error)  // written beside it, not renamed over it
                    return (*s->in / s->name).string() + ": " + why(s->undo) + " (its old bytes are " + s->back.name +
                           " beside it)";
                if (s->old && s->back.created)  // the write failed, its temporary not removed
                    return (*s->in / s->name).string() + ": " + why(s->undo) + " (some of its old bytes are " +
                           s->back.name + " beside it)";
                return (*s->in / s->name).string() + ": " + why(s->undo);
            });
        for (const std::vector<Made>* list : {&madeIn, &madeDirs})
            for (auto d = list->rbegin(); d != list->rend(); ++d)
                report(d->undo, [&] { return (*d->in / d->name).string() + ": " + why(d->undo) + " (a directory the save made)"; });
        return out;
    };

    // what the project no longer holds, looked for in each directory
    // through the descriptor the save holds on it (POSIX), as what is found
    // is then set aside through it. On a case-insensitive disk an entry may
    // be listed under another spelling, and is the file just written. A disk
    // that folds some other way than foldedPath may list one under a name no
    // entry folds to, so a file that is one the save wrote is kept too: a
    // gap in the folding can only keep a file, never remove one. A link is
    // never a file the save wrote, whatever it points at or its name is (an
    // entry's own, or one that folds to it), so that is asked first.
    std::map<QString, std::string> folded;
    for (const auto& [name, bytes] : entries)
        folded.emplace(foldedPath(name), name);
    // whether `file` in `in`, `name` in the project, is a file the save wrote
    const auto wrote = [&](const Folder& in, const std::string& file, const std::string& name) {
        const auto it = folded.find(foldedPath(name));
#if BW_PROJECT_POSIX
        struct stat st {};
        if (::fstatat(in.fd, file.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(st.st_mode))
            return false;
        if (written.count({std::uint64_t(st.st_dev), std::uint64_t(st.st_ino)}))
            return true;
        if (it == folded.end())
            return false;
        // the file at the entry it folds to, a link there followed
        const auto [dir, entry] = split(it->second);
        struct stat at {};
        return ::fstatat(folder(dir, false).fd, entry.c_str(), &at, 0) == 0 && at.st_dev == st.st_dev &&
               at.st_ino == st.st_ino;
#else
        std::error_code t;
        if (kindAt(in, file) == Kind::Link)
            return false;
        if (it != folded.end() && fs::equivalent(in / file, path / fs::path(it->second), t))
            return true;
        return std::any_of(staged.begin(), staged.end(),
                           [&](const Staged& s) { return fs::equivalent(in / file, *s.in / s.name, t); });
#endif
    };
    struct Found {
        const Folder* in;
        std::string name;  // in `in`
    };
    std::vector<std::string> dirs;  // in the project
    try {
        // `path` and the directories on the way to it, one at a time as
        // spelled, so the save knows which it made: one already there (before
        // the save, or made by someone else meanwhile, or reached through a
        // `..`) is not the save's, and a failure leaves it. Each is made in
        // the one above it as the save holds that open, one the save made is
        // opened without following a link, and the rollback removes each
        // through the one above it, so it removes them from wherever they
        // went and only ever an empty directory.
        if (found) {
            folders.emplace("", std::move(*found));
        } else {
            std::vector<std::string> names;
            for (const fs::path& part : path.relative_path())
                if (!part.empty())
                    names.push_back(part.string());
            above.push_back(openRoot(path.has_root_directory() ? path.root_path() : fs::path(".")));
            above.back().path = path.root_path();
            for (std::size_t i = 0; i < names.size(); ++i) {
                const Folder& in = above.back();
                const bool fresh = make(madeDirs, in, names[i]);
                Folder next = fresh ? openFolder(in, names[i]) : enterFolder(in, names[i]);
                if (i + 1 < names.size())
                    above.push_back(std::move(next));
                else
                    folders.emplace("", std::move(next));
            }
        }
        for (const auto& [name, bytes] : entries)
            if (name != "manifest.json")
                stage(name, bytes);
        stage("manifest.json", entries.at("manifest.json"));
#if BW_PROJECT_POSIX
        Descriptor fd(::dup(folders.at("").fd));
        if (fd.get() < 0)
            fail("can't write " + path.string() + ": " + std::strerror(errno));
        spare.emplace(path, std::move(fd));
#endif
        for (Staged& s : staged) {
            if (const std::error_code e = renameIn(*s.in, s.tmp.name, s.name))
                fail("can't write " + (*s.in / s.name).string() + ": " + e.message());
            s.renamed = true;
            if (testing::directoryStep)
                testing::directoryStep("wrote", *s.in / s.name);
        }

        if (testing::directoryStep)
            testing::directoryStep("scan", path);
        std::vector<Found> stale;
        // `rel` in the project and every directory in it but hidden ones,
        // each listed and looked at through the save's descriptor on it; a
        // hidden name is never the project's, and is left alone
        const std::function<void(const std::string&)> scan = [&](const std::string& rel) {
            const Folder& in = folder(rel, false);
            for (const std::string& file : namesIn(in)) {
                if (hiddenPart(file))
                    continue;
                const std::string name = rel.empty() ? file : rel + "/" + file;
                const Kind kind = kindAt(in, file);
                if (kind == Kind::Directory) {
                    dirs.push_back(name);
                    scan(name);
                } else if (kind == Kind::Link ||
                           (kind != Kind::None && !entries.count(name) && !wrote(in, file, name))) {
                    stale.push_back({&in, file});
                }
            }
        };
        scan("");
        // aside under a hidden name, which a read skips, so that until the
        // save is done each can go back, and once it is none is left under
        // its own name (a layer file the manifest doesn't list would make
        // the project unreadable). Each is recorded in full before it is
        // renamed, the room for the record taken first, so once the rename
        // is done nothing can fail before the rollback knows of it; one left
        // under both names too, whose second name the rollback then removes
        // (or names).
        asides.reserve(stale.size());
        for (const auto& [at, file] : stale) {
            const Folder& in = *at;
            for (int attempt = 0;; ++attempt) {
                Aside next{&in, file, hiddenName("old")};
                const Renamed r = renameFresh(in, file, next.aside);
                if (!r.error || r.twice) {
                    next.both = r.twice;
                    asides.push_back(std::move(next));  // moved into the room reserved: nothing to fail
                }
                if (!r.error)
                    break;
                if (r.twice || r.error != std::errc::file_exists || attempt >= 100)
                    fail("can't remove " + (in / file).string() + ": " + r.error.message());
            }
        }
    } catch (...) {
        // whatever failed, the save is rolled back; a rollback that left
        // anything turns the failure into a ProjectError that names what it
        // was and everything left, and otherwise it goes on as it was
        // (std::bad_alloc stays std::bad_alloc)
        const Unrestored left = rollBack();
        if (!left.any())
            throw;
        try {
            std::string what;
            try {
                throw;
            } catch (const ProjectError& e) {
                what = e.what();
            } catch (const std::exception& e) {
                what = "can't write " + path.string() + ": " + e.what();
            } catch (...) {
                what = "can't write " + path.string() + ": an unknown error";
            }
            if (!left.named.empty())
                what += "; and these couldn't be put back as they were: " + left.named;
            if (left.unnamed)
                what += "; and " + std::to_string(left.unnamed) + (left.named.empty() ? "" : " more") +
                        (left.unnamed == 1 ? " change" : " changes") +
                        " the save made couldn't be put back, and couldn't be named (it ran out of memory)";
            fail(what);
        } catch (const ProjectError&) {
            throw;
        } catch (...) {
            throw unnameable;
        }
    }

    // done: what was set aside goes, then the directories that leaves
    // empty, deepest first so a parent is empty once its children are gone.
    // One that can't go stays, hidden, where a read doesn't look; the save
    // is done whatever fails here, running out of memory included.
    try {
        for (const Aside& a : asides)
            removeIn(*a.in, a.aside, false);
        std::sort(dirs.begin(), dirs.end(), std::greater<>());
        for (const std::string& name : dirs) {
            const auto [dir, file] = split(name);
            try {
                removeIn(folder(dir, false), file, true);
            } catch (const ProjectError&) {
            }
        }
    } catch (...) {
    }
}

} // namespace bw::project
