#include "blockwright/project.h"

#include "blockwright/composite.h"
#include "blockwright/cp437.h"

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
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#define BW_PROJECT_POSIX 1
#include <cerrno>
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
constexpr std::size_t kTmpNameBytes = 64;      // writeFileWhole's temporary name, at most

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

bool ownedPath(std::string_view path) {
    return path == "manifest.json" || path == "notes.md" || layerFile(path) || snapshotFile(path);
}

std::string layerPath(LayerId id, bool cells) { return "layers/" + std::to_string(id) + (cells ? ".cells" : ".json"); }
std::string snapshotPath(int round) { return "snapshots/r" + std::to_string(round) + ".ans"; }

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

QJsonObject layerJson(const Layer& l) {
    const LayerProperties& p = l.props;
    return QJsonObject{
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
}

Layer layerFromEntries(LayerId id, const std::string& json, const std::string& cells) {
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

// Reads the `size` bytes of the file a directory walk found at `p`. What is
// there now may not be what the walk saw, so it is opened without following
// a link or waiting on a FIFO, and read only if it is a plain file of that
// size.
std::string readWhole(const fs::path& p, std::size_t size) {
    std::string out(size, '\0');
#if BW_PROJECT_POSIX
    const int fd = ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        fail(errno == ELOOP ? p.string() + " is a symbolic link" : "can't read " + p.string() + ": " + std::strerror(errno));
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        fail(p.string() + " is not a file");
    }
    if (std::uint64_t(st.st_size) != size) {
        ::close(fd);
        fail(p.string() + " changed while it was read");
    }
    if (testing::directoryStep)
        testing::directoryStep("reading", p);
    std::size_t got = 0;
    while (got < size) {
        const ssize_t n = ::read(fd, out.data() + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += std::size_t(n);
    }
    char extra;
    ssize_t more;
    do
        more = ::read(fd, &extra, 1);
    while (more < 0 && errno == EINTR);
    ::close(fd);
    if (got != size || more != 0)
        fail(p.string() + " changed while it was read");
#else
    std::ifstream in(p, std::ios::binary);
    if (!in)
        fail("can't read " + p.string());
    in.read(out.data(), std::streamsize(size));
    if (std::size_t(in.gcount()) != size || in.peek() != std::char_traits<char>::eof())
        fail(p.string() + " changed while it was read");
#endif
    return out;
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

Entries readZip(const fs::path& path) {
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &err);
    if (!z) {
        zip_error_t e;
        zip_error_init_with_code(&e, err);
        const std::string msg = zip_error_strerror(&e);
        zip_error_fini(&e);
        fail(path.string() + " is not a project zip: " + msg);
    }
    struct Closer {
        zip_t* z;
        ~Closer() { zip_discard(z); }
    } closer{z};
    Entries out;
    std::uint64_t total = 0;
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
        zip_file_t* f = zip_fopen_index(z, zip_uint64_t(i), 0);
        if (!f)
            fail(path.string() + ": can't open " + name + ": " + zip_strerror(z));
        std::string bytes(std::size_t(st.size), '\0');
        const zip_int64_t got = st.size ? zip_fread(f, bytes.data(), st.size) : 0;
        char extra;
        const zip_int64_t more = zip_fread(f, &extra, 1);
        zip_fclose(f);
        if (got != zip_int64_t(st.size) || more != 0)
            fail(path.string() + ": " + name + " is damaged");
        out.emplace(std::move(name), std::move(bytes));
    }
    return out;
}

Entries readDirectory(const fs::path& root) {
    Entries out;
    std::uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec), end;
    if (ec)
        fail("can't read " + root.string() + ": " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec)
            fail("can't read " + root.string() + ": " + ec.message());
        const fs::directory_entry& e = *it;
        const std::string name = e.path().lexically_relative(root).generic_string();
        if (e.is_symlink(ec))
            fail(root.string() + ": " + name + " is a symbolic link");
        const bool directory = e.is_directory(ec);
        if (!directory && !e.is_regular_file(ec))  // hidden or not
            fail(root.string() + ": " + name + " is not a file");
        if (hiddenPath(name)) {
            if (directory)
                it.disable_recursion_pending();
            continue;
        }
        if (directory)
            continue;
        takeEntry(out, name);
        const std::uint64_t size = e.file_size(ec);
        if (ec)
            fail("can't read " + e.path().string() + ": " + ec.message());
        checkSize(name, size, total);
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

// Refuses a symbolic link at `path`; whether something is there.
bool notLink(const fs::path& path) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(path, ec);
    if (fs::is_symlink(st))
        fail(path.string() + " is a symbolic link");
    return fs::exists(st);
}

// A file's identity, the same under every name it has: device and inode,
// or {0, 0} where the system has none.
using FileId = std::pair<std::uint64_t, std::uint64_t>;

// Writes `bytes` as `path` whole: a new hidden file beside it, created
// exclusively under a name no one can have put there first (and never
// through a link), then renamed over `path`, which must not be a link. The
// temporary name doesn't hold `path`'s, so a name as long as the disk takes
// is written as any other. Returns the identity of the file written, taken
// from the descriptor it was written through before the rename, so it is
// that file's whatever is put at `path` afterwards.
FileId writeFileWhole(const fs::path& path, const std::string& bytes) {
    static unsigned counter = 0;
    static const unsigned long salt = std::random_device{}();
    notLink(path);
    fs::path tmp;
    bool created = false;
    FileId id{};
    const auto give_up = [&](const std::string& why) {
        if (created) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
        }
        fail("can't write " + path.string() + ": " + why);
    };
    for (int attempt = 0;; ++attempt) {
        char name[kTmpNameBytes];
        std::snprintf(name, sizeof name, ".bw-tmp-%lx-%lx-%x", salt, processId(), counter++);
        tmp = path.parent_path() / name;
#if BW_PROJECT_POSIX
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (fd < 0) {
            if (errno == EEXIST && attempt < 100)
                continue;
            give_up(std::strerror(errno));
        }
        created = true;
        const char* data = bytes.data();
        std::size_t left = bytes.size();
        int err = 0;
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
        id = {std::uint64_t(st.st_dev), std::uint64_t(st.st_ino)};
        if (::close(fd) != 0 && !err)
            err = errno;
        if (err)
            give_up(std::strerror(err));
#else
        std::error_code ec;
        if (fs::exists(fs::symlink_status(tmp, ec))) {
            if (attempt < 100)
                continue;
            give_up("no free temporary name");
        }
        created = true;
        std::ofstream out(tmp, std::ios::binary);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.close();
        if (!out)
            give_up("write failed");
#endif
        break;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
        give_up(ec.message());
    if (testing::directoryStep)
        testing::directoryStep("wrote", path);
    return id;
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
        p.layers.add(layerFromEntries(id, json->second, cells->second));
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
    if (!z) {
        zip_error_t e;
        zip_error_init_with_code(&e, err);
        const std::string msg = zip_error_strerror(&e);
        zip_error_fini(&e);
        fail("can't write " + path.string() + ": " + msg);
    }
    const auto abandon = [&](const std::string& what) {
        const std::string msg = what + ": " + zip_strerror(z);
        zip_discard(z);
        fail("can't write " + path.string() + ": " + msg);
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
}

void writeDirectory(const Project& project, const fs::path& path) {
    const Entries entries = encode(project);
    // an empty path is no directory; `path / name` would write into the
    // working one, past the check that it holds a project
    if (path.empty())
        fail("can't write a project to an empty path");
#ifdef PATH_MAX
    // each file, and the temporary one beside it, as long a path as the
    // system opens
    for (const auto& [name, bytes] : entries)
        if ((path / fs::path(name)).native().size() + kTmpNameBytes >= PATH_MAX)
            fail("can't write " + (path / fs::path(name)).string() + ": the path is too long");
#endif
    std::error_code ec;
    const bool existed = fs::exists(path, ec) || fs::is_symlink(path, ec);
    bool first = true;  // no project there yet
    if (existed) {
        if (!fs::is_directory(path, ec))
            fail(path.string() + " is not a directory");
        first = !fs::is_regular_file(path / "manifest.json", ec);
        if (first)
            for (const fs::directory_entry& e : fs::directory_iterator(path, ec))
                if (!hiddenPart(e.path().filename().string()))
                    fail(path.string() + " is not empty and holds no project");
        if (ec)
            fail("can't read " + path.string() + ": " + ec.message());
    }
    // each directory on the way made or found, never a link; what a first
    // save makes is kept so a failure can take it away again (in the order
    // made, so a directory comes after its parent), and what any save writes
    // so the stale scan never takes it for another file
    std::vector<fs::path> madeFiles, madeDirs;
#if BW_PROJECT_POSIX
    std::set<FileId> written;
#endif
    const auto write = [&](const std::string& name, const std::string& bytes) {
        fs::path at = path;
        const std::vector<std::string_view> names = parts(name);
        for (std::size_t i = 0; i + 1 < names.size(); ++i) {
            at /= fs::path(std::string(names[i]));
            std::error_code e;
            if (!notLink(at)) {
                if (fs::create_directory(at, e))
                    madeDirs.push_back(at);
                else if (e)
                    fail("can't create " + at.string() + ": " + e.message());
            }
            if (!fs::is_directory(fs::symlink_status(at, e)))
                fail(at.string() + " is not a directory");
        }
        [[maybe_unused]] const FileId id = writeFileWhole(path / fs::path(name), bytes);
        madeFiles.push_back(path / fs::path(name));
#if BW_PROJECT_POSIX
        written.insert(id);
#endif
    };
    try {
        // `path` and the directories on the way to it, one at a time as
        // spelled, so the save knows which it made: one already there (before
        // the save, or made by someone else meanwhile, or reached through a
        // `..`) is not the save's, and a failure leaves it
        if (!existed) {
            fs::path at;
            for (const fs::path& part : path) {
                at /= part;
                if (fs::create_directory(at, ec))
                    madeDirs.push_back(at);
                else if (ec)
                    fail("can't create " + at.string() + ": " + ec.message());
            }
        }
        for (const auto& [name, bytes] : entries)
            if (name != "manifest.json")
                write(name, bytes);
        write("manifest.json", entries.at("manifest.json"));
    } catch (...) {
        // a first save that fails leaves the directory as it found it (or
        // none), so the next one isn't refused as "not a project"
        if (first) {
            std::error_code ignored;
            for (const fs::path& f : madeFiles)
                fs::remove(f, ignored);
            // last made first, so each is empty once what the save put in it
            // is gone; one that isn't (someone else's file in it) stays
            for (auto d = madeDirs.rbegin(); d != madeDirs.rend(); ++d)
                fs::remove(*d, ignored);
        }
        throw;
    }

    if (testing::directoryStep)
        testing::directoryStep("scan", path);

    // what the project no longer holds, and the directories that leaves
    // empty; on a case-insensitive disk an entry may be listed under another
    // spelling, and is the file just written. A disk that folds some other
    // way than foldedPath may list one under a name no entry folds to, so a
    // file that is one the save wrote is kept too: a gap in the folding can
    // only keep a file, never remove one. A link is never a file the save
    // wrote, whatever it points at or its name is (an entry's own, or one
    // that folds to it), so that is asked first (fs::equivalent follows
    // links).
    std::map<QString, std::string> folded;
    for (const auto& [name, bytes] : entries)
        folded.emplace(foldedPath(name), name);
    const auto link = [](const fs::directory_entry& e) {
        std::error_code t;
        return fs::is_symlink(fs::symlink_status(e.path(), t));
    };
    const auto wrote = [&](const std::string& name, const fs::directory_entry& e) {
        std::error_code t;
#if BW_PROJECT_POSIX
        struct stat st {};
        if (::lstat(e.path().c_str(), &st) != 0 || S_ISLNK(st.st_mode))
            return false;
        if (written.count({std::uint64_t(st.st_dev), std::uint64_t(st.st_ino)}))
            return true;
#else
        if (e.is_symlink(t))
            return false;
#endif
        const auto it = folded.find(foldedPath(name));
        if (it != folded.end() && fs::equivalent(e.path(), path / fs::path(it->second), t))
            return true;
#if BW_PROJECT_POSIX
        return false;
#else
        return std::any_of(madeFiles.begin(), madeFiles.end(), [&](const fs::path& f) {
            return fs::equivalent(e.path(), f, t);
        });
#endif
    };
    std::vector<fs::path> stale, dirs;
    fs::recursive_directory_iterator it(path, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        const std::string name = e.path().lexically_relative(path).generic_string();
        std::error_code t;
        if (hiddenPath(name)) {
            if (e.is_directory(t) && !e.is_symlink(t))
                it.disable_recursion_pending();
            continue;
        }
        if (e.is_directory(t) && !e.is_symlink(t))
            dirs.push_back(e.path());
        else if (link(e) || (!entries.count(name) && !wrote(name, e)))
            stale.push_back(e.path());
    }
    if (ec)
        fail("can't read " + path.string() + ": " + ec.message());
    for (const fs::path& f : stale)
        if (!fs::remove(f, ec) && ec)
            fail("can't remove " + f.string() + ": " + ec.message());
    // deepest first, so a parent is empty once its children are gone
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native() > b.native(); });
    for (const fs::path& d : dirs)
        if (fs::is_empty(d, ec))
            fs::remove(d, ec);
}

} // namespace bw::project
