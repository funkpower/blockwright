#include "blockwright/library.h"

#include "blockwright/cp437.h"
#include "blockwright/irc.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#define BW_LIBRARY_POSIX 1
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define BW_LIBRARY_POSIX 0
#endif

#ifndef BW_LIBRARY_FROM_BINDIR
#define BW_LIBRARY_FROM_BINDIR "../share/blockwright/library"
#endif

namespace fs = std::filesystem;

namespace bw::library {

std::string readRegularFile(const fs::path& p) {
#if BW_LIBRARY_POSIX
    // The handle is checked, not the path: open without blocking (a FIFO
    // with no writer would block), refuse anything fstat doesn't call a
    // regular file, then read it blocking.
    int fd;
    do
        fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0)
        throw LibraryError(errno == ENOENT ? "can't be read" : std::string("can't be read: ") + std::strerror(errno));
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    struct stat st {};
    if (::fstat(fd, &st) != 0)
        throw LibraryError(std::string("can't be read: ") + std::strerror(errno));
    if (!S_ISREG(st.st_mode))
        throw LibraryError("is not a regular file");
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
        throw LibraryError(std::string("can't be read: ") + std::strerror(errno));
    std::string out;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw LibraryError(std::string("read failed: ") + std::strerror(errno));
        if (n == 0)
            break;
        out.append(buf, std::size_t(n));
    }
    return out;
#else
    // No FIFOs to block on; the path checks the callers make suffice.
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw LibraryError("can't be read");
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        throw LibraryError("read failed");
    return ss.str();
#endif
}

namespace {

constexpr const char* kCategory = "category";
constexpr const char* kTags = "tags";
constexpr const char* kIrcBytes = "ircBytes";

std::string readAll(const fs::path& p) { return readRegularFile(p); }

fs::path fspath(const QString& s) { return fs::path(QFile::encodeName(s).toStdString()); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

bool blankText(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c == ' ' || c == '\t'; });
}

void checkText(const std::string& s, const char* what) {
    if (s.empty() || blankText(s))
        throw LibraryError(std::string(what) + " is empty");
    if (std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; }))
        throw LibraryError(std::string(what) + " holds a control character");
}

// A tag also can't hold a comma: `library list` joins tags with one, and
// the Save to Library dialog splits on it.
void checkTag(const std::string& s, const char* what) {
    checkText(s, what);
    if (s.find(',') != std::string::npos)
        throw LibraryError(std::string(what) + " holds a comma");
}

// A name is CP437 bytes, so 0x01-0x1F and 0x7F are the pictographs (☺ ♥ ⌂)
// and fine; only NUL (SAUCE's padding) is not.
void checkName(const std::string& name) {
    if (name.empty() || blankText(name))
        throw LibraryError("the name is empty");
    if (name.find('\0') != std::string::npos)
        throw LibraryError("the name holds a NUL");
    if (name.size() > Sauce::kTitleLen)
        throw LibraryError("the name is longer than SAUCE's " + std::to_string(Sauce::kTitleLen) + " bytes");
    if (name.back() == ' ')
        throw LibraryError("the name ends in a space, which SAUCE drops");
}

bool allBlank(const Canvas& cv) {
    for (int y = 0; y < cv.height(); ++y)
        for (int x = 0; x < cv.width(); ++x)
            if (!isBlank(cv.at(x, y)))
                return false;
    return true;
}

bool isLink(const fs::path& p) {
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
}

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}

// Whether `p` is a regular file (through a link). A FIFO, socket or device
// is never opened: reading one could block.
bool regular(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(fs::status(p, ec));
}

// Refuses a path something other than a regular file (or nothing) sits on.
void checkWritable(const fs::path& p) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (fs::is_symlink(st))
        throw LibraryError(p.string() + " is a symbolic link; a stamp is never written through one");
    if (fs::exists(st) && !fs::is_regular_file(st))
        throw LibraryError(p.string() + " is not a regular file");
}

// The keys of the top-level object of `json`, which is valid JSON (Qt has
// parsed it), decoded. QJsonDocument keeps only the last of a repeated key,
// so repeats are found here.
std::vector<QString> topLevelKeys(std::string_view json) {
    std::vector<QString> keys;
    int depth = 0;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
        } else if (c == '"') {
            const std::size_t start = i;
            for (++i; i < json.size() && json[i] != '"'; ++i)
                if (json[i] == '\\')
                    ++i;
            if (depth != 1)
                continue;
            std::size_t j = i + 1;
            while (j < json.size() && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r'))
                ++j;
            if (j < json.size() && json[j] == ':') {
                const std::string raw = "[" + std::string(json.substr(start, i - start + 1)) + "]";
                keys.push_back(QJsonDocument::fromJson(QByteArray::fromStdString(raw)).array().at(0).toString());
            }
        }
    }
    return keys;
}

// `bytes` written to a new temporary file beside `target`, renamed over
// it by commit() and removed if it never is, including when the
// constructor itself throws. Paths stay std::filesystem paths throughout,
// so a name that isn't UTF-8 is written as itself (a QString would turn its
// bytes into U+FFFD). Replacing a file keeps its permission bits (0777; not
// set-ID or sticky); a new one gets 0666 less the umask.
class Staged {
public:
    Staged(const fs::path& target, const std::string& bytes) : target_(target) {
        try {
            write(bytes);
        } catch (...) {
            // The destructor doesn't run for a constructor that throws.
            discard();
            throw;
        }
    }
    Staged(const Staged&) = delete;
    Staged& operator=(const Staged&) = delete;
    ~Staged() { discard(); }

    void commit() {
        std::error_code ec;
        fs::rename(temp_, target_, ec);
        if (ec)
            throw LibraryError(target_.string() + ": write failed: " + ec.message());
        committed_ = true;
    }

private:
    void write(const std::string& bytes) {
        static unsigned counter = 0;
        const fs::path dir = target_.parent_path();
        for (int attempt = 0;; ++attempt) {
            // The path is recorded before the file exists, so once open()
            // succeeds nothing that can throw stands between it and cleanup.
            temp_ = dir / ("." + target_.filename().string() + ".tmp-" + std::to_string(processId()) + "-" +
                           std::to_string(counter++));
#if BW_LIBRARY_POSIX
            // A replacement keeps the old file's permission bits (0777 only:
            // set-user-ID, set-group-ID and sticky are deliberately not kept),
            // applied once the bytes are written. Until then the temporary is
            // 0600, never wider than the file it replaces.
            struct stat old {};
            const bool replacing = ::lstat(target_.c_str(), &old) == 0 && S_ISREG(old.st_mode);
            const int fd = ::open(temp_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, replacing ? 0600 : 0666);
            if (fd < 0) {
                if (errno == EEXIST && attempt < 100)
                    continue;
                temp_.clear();
                throw LibraryError(target_.string() + ": " + std::strerror(errno));
            }
            fd_ = fd;
            created_ = true;
            const char* data = bytes.data();
            std::size_t left = bytes.size();
            while (left) {
                const ssize_t n = ::write(fd_, data, left);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    throw LibraryError(target_.string() + ": write failed: " +
                                       std::strerror(n < 0 ? errno : EIO));
                data += n;
                left -= std::size_t(n);
            }
            if (replacing && ::fchmod(fd_, old.st_mode & 0777) != 0)
                throw LibraryError(target_.string() + ": " + std::strerror(errno));
            // Always close, and report the first error.
            int err = ::fsync(fd_) != 0 ? errno : 0;
            if (!err && testing::writeStampFault) {
                try {
                    testing::writeStampFault("fsync");
                } catch (const LibraryError&) {
                    err = ENOSPC;
                }
            }
            if (::close(std::exchange(fd_, -1)) != 0 && !err)
                err = errno;
            if (err)
                throw LibraryError(target_.string() + ": write failed: " + std::strerror(err));
#else
            std::error_code ec;
            if (fs::exists(fs::symlink_status(temp_, ec))) {
                if (attempt < 100)
                    continue;
                temp_.clear();
                throw LibraryError(target_.string() + ": no free temporary name");
            }
            std::ofstream out(temp_, std::ios::binary);
            created_ = true;  // removing a file that wasn't made is harmless
            if (!out)
                throw LibraryError(target_.string() + ": can't be written");
            out.write(bytes.data(), std::streamsize(bytes.size()));
            out.close();
            if (!out)
                throw LibraryError(target_.string() + ": write failed");
#endif
            return;
        }
    }

    // Closes the descriptor and removes the temporary, unless committed.
    void discard() noexcept {
#if BW_LIBRARY_POSIX
        if (fd_ >= 0)
            ::close(std::exchange(fd_, -1));
#endif
        if (created_ && !committed_) {
            std::error_code ec;
            fs::remove(temp_, ec);
            temp_.clear();
            created_ = false;
        }
    }

    static long processId() {
#if BW_LIBRARY_POSIX
        return long(::getpid());
#else
        return 0;
#endif
    }

    fs::path target_, temp_;
    int fd_ = -1;
    bool created_ = false;  // temp_ exists (and is ours to remove)
    bool committed_ = false;
};

fs::path envPath(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
}

} // namespace

std::string Stamp::displayName() const { return cp437ToUtf8(name); }

CellBlock Stamp::block() const {
    const Canvas& cv = art.canvas;
    CellBlock b(cv.width(), cv.height());
    for (int y = 0; y < cv.height(); ++y)
        for (int x = 0; x < cv.width(); ++x)
            b.at(x, y) = cv.at(x, y);
    return b;
}

std::size_t widestRowBytes(const Canvas& canvas) {
    std::size_t widest = 0;
    for (const std::string& line : irc::encodeCanvas(canvas))
        widest = std::max(widest, line.size());
    return widest;
}

std::string sidecarJson(const StampMeta& meta) {
    // Written by hand so the key order is fixed and diffs stay small.
    const auto quoted = [](const std::string& s) {
        // QJsonDocument does the escaping.
        const QByteArray a = QJsonDocument(QJsonArray{QString::fromStdString(s)}).toJson(QJsonDocument::Compact);
        return a.mid(1, a.size() - 2).toStdString();
    };
    std::string out = "{\n  \"category\": " + quoted(meta.category) + ",\n  \"tags\": [";
    for (std::size_t i = 0; i < meta.tags.size(); ++i)
        out += (i ? ", " : "") + quoted(meta.tags[i]);
    out += "],\n  \"ircBytes\": " + std::to_string(meta.ircBytes) + "\n}\n";
    return out;
}

StampMeta parseSidecar(std::string_view json) {
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray(json.data(), static_cast<qsizetype>(json.size())), &err);
    if (err.error != QJsonParseError::NoError)
        throw LibraryError("sidecar is not JSON: " + err.errorString().toStdString() + " at offset " +
                           std::to_string(err.offset));
    if (!doc.isObject())
        throw LibraryError("sidecar is not a JSON object");
    {
        std::vector<QString> keys = topLevelKeys(json);
        std::sort(keys.begin(), keys.end());
        if (const auto d = std::adjacent_find(keys.begin(), keys.end()); d != keys.end())
            throw LibraryError("sidecar has the key \"" + d->toStdString() + "\" more than once");
    }
    const QJsonObject o = doc.object();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.key() != QLatin1String(kCategory) && it.key() != QLatin1String(kTags) &&
            it.key() != QLatin1String(kIrcBytes))
            throw LibraryError("sidecar has an unknown key \"" + it.key().toStdString() + "\"");
    StampMeta m;
    const QJsonValue cat = o.value(QLatin1String(kCategory));
    if (!cat.isString())
        throw LibraryError("sidecar \"category\" is missing or not a string");
    m.category = cat.toString().toStdString();
    checkText(m.category, "sidecar \"category\"");
    const QJsonValue tags = o.value(QLatin1String(kTags));
    if (!tags.isArray())
        throw LibraryError("sidecar \"tags\" is missing or not an array");
    for (const QJsonValue& t : tags.toArray()) {
        if (!t.isString())
            throw LibraryError("sidecar \"tags\" holds something other than a string");
        m.tags.push_back(t.toString().toStdString());
        checkTag(m.tags.back(), "a sidecar tag");
    }
    const QJsonValue bytes = o.value(QLatin1String(kIrcBytes));
    const double d = bytes.toDouble(-1);
    if (!bytes.isDouble() || d < 0 || d != std::floor(d) || d > 1e15)
        throw LibraryError("sidecar \"ircBytes\" is missing or not a whole number of bytes");
    m.ircBytes = static_cast<std::size_t>(d);
    return m;
}

fs::path sidecarPath(const fs::path& ans) {
    fs::path p = ans;
    p.replace_extension(".json");
    return p;
}

bool isArtName(const fs::path& p) { return lowered(p.extension().string()) == ".ans"; }
bool isSidecarName(const fs::path& p) { return lowered(p.extension().string()) == ".json"; }

fs::path pairedSidecar(const fs::path& ans) {
    const fs::path exact = sidecarPath(ans);
    if (present(exact))
        return exact;
    // A case variant of the extension (X.JSON beside X.ANS), the first by
    // name if there are several.
    std::vector<fs::path> found;
    std::error_code ec;
    const fs::path dir = ans.has_parent_path() ? ans.parent_path() : fs::path(".");
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.stem() == ans.stem() && isSidecarName(p))
            found.push_back(p);
    }
    if (found.empty())
        return exact;
    std::sort(found.begin(), found.end());
    return found.front();
}

Stamp readStamp(const fs::path& ans) {
    Stamp s;
    s.path = ans;
    std::error_code ec;
    if (!fs::exists(fs::status(ans, ec)))
        throw LibraryError(".ans can't be read");
    if (!regular(ans))
        throw LibraryError(".ans is not a regular file");
    std::string data;
    try {
        data = readAll(ans);
    } catch (const LibraryError& e) {
        throw LibraryError(std::string(".ans ") + e.what());
    }
    try {
        s.art = parseAnsi(data);
    } catch (const AnsiError& e) {
        throw LibraryError(std::string(".ans does not parse: ") + e.what());
    }
    if (!s.art.sauce)
        throw LibraryError(".ans has no SAUCE record (the title is the stamp's name)");
    s.name = s.art.sauce->title;
    if (s.name.empty() || blankText(s.name))
        throw LibraryError(".ans has an empty SAUCE title (the title is the stamp's name)");
    try {
        checkName(s.name);
    } catch (const LibraryError& e) {
        throw LibraryError(std::string(".ans SAUCE title: ") + e.what());
    }
    if (allBlank(s.art.canvas))
        throw LibraryError(".ans has no drawn cell: every cell is blank, so placing it writes nothing");

    const fs::path side = pairedSidecar(ans);
    if (!fs::exists(fs::status(side, ec)))
        throw LibraryError("no sidecar " + side.filename().string());
    if (!regular(side))
        throw LibraryError("sidecar " + side.filename().string() + " is not a regular file");
    std::string json;
    try {
        json = readAll(side);
    } catch (const LibraryError& e) {
        throw LibraryError("sidecar " + std::string(e.what()));
    }
    s.meta = parseSidecar(json);
    std::size_t actual = 0;
    try {
        actual = widestRowBytes(s.art.canvas);
    } catch (const irc::IrcError& e) {
        throw LibraryError(std::string("the IRC encoder failed: ") + e.what());
    }
    if (actual != s.meta.ircBytes)
        throw LibraryError("sidecar says the widest row is " + std::to_string(s.meta.ircBytes) +
                           " IRC bytes; the art's is " + std::to_string(actual));
    return s;
}

namespace {
StampFiles encodeChecked(const std::string& name, const std::string& category, const std::vector<std::string>& tags,
                         const CellBlock& cells, bool ice);
} // namespace

StampFiles encodeStamp(const std::string& name, const std::string& category, const std::vector<std::string>& tags,
                       const CellBlock& cells, bool ice) {
    checkName(name);
    checkText(category, "the category");
    for (const std::string& t : tags)
        checkTag(t, "a tag");
    if (cells.empty())
        throw LibraryError("nothing is selected");
    if (cells.width > kMaxColumns || cells.height > kMaxRows)
        throw LibraryError("the stamp is " + std::to_string(cells.width) + " x " + std::to_string(cells.height) +
                           " cells; a stamp is at most " + std::to_string(kMaxColumns) + " x " +
                           std::to_string(kMaxRows));
    try {
        return encodeChecked(name, category, tags, cells, ice);
    } catch (const AnsiError& e) {
        throw LibraryError(std::string("the art can't be written as .ans: ") + e.what());
    } catch (const irc::IrcError& e) {
        throw LibraryError(std::string("the IRC encoder failed: ") + e.what());
    }
}

namespace {

StampFiles encodeChecked(const std::string& name, const std::string& category, const std::vector<std::string>& tags,
                         const CellBlock& cells, bool ice) {
    AnsiDocument doc;
    doc.canvas = Canvas(cells.width, cells.height);
    for (int y = 0; y < cells.height; ++y)
        for (int x = 0; x < cells.width; ++x) {
            const Cell& c = cells.at(x, y);
            doc.canvas.at(x, y) = c.transparent ? Cell{} : iceCell(c, ice);
        }
    doc.ice = ice;
    doc.sauce = Sauce::fresh(cells.width, cells.height);
    doc.sauce->title = name;
    doc.sauce->setIce(ice);
    const AnsiOutput out = writeAnsi(doc);
    if (!out.unwritable.empty())
        throw LibraryError(std::to_string(out.unwritable.size()) +
                           " cell(s) hold a glyph with no CP437 code, which .ans can't store");
    if (out.blinkDropped)
        throw LibraryError(std::to_string(out.blinkDropped) + " blinking cell(s) would lose their blink");

    // Check and count the art as it reads back, which is what readStamp
    // checks the sidecar against.
    const Canvas back = parseAnsi(out.bytes).canvas;
    if (allBlank(back))
        throw LibraryError("every cell is blank, so the stamp would draw nothing");
    StampMeta meta{category, tags, widestRowBytes(back)};
    return {out.bytes, sidecarJson(meta)};
}

} // namespace

void writeStamp(const fs::path& ans, const StampFiles& files) {
    // Over the sidecar already there, in whatever case its extension has.
    const fs::path json = pairedSidecar(ans);
    for (const fs::path& p : {ans, json})
        checkWritable(p);
    std::error_code ec;
    if (ans.has_parent_path()) {
        fs::create_directories(ans.parent_path(), ec);
        if (ec)
            throw LibraryError(ans.parent_path().string() + ": " + ec.message());
    }
    // Both files are written to temporaries first, so a failure there (a
    // full disk, a sidecar that can't be written) leaves the old pair as it
    // was. Then the sidecar is renamed into place and the art after it; if
    // that last rename fails the old sidecar is put back.
    std::optional<std::string> oldJson;
    if (present(json)) {
        try {
            oldJson = readAll(json);
        } catch (const LibraryError& e) {
            throw LibraryError(json.string() + ": " + e.what());
        }
    }
    Staged a(ans, files.ans);
    Staged j(json, files.json);
    j.commit();
    try {
        if (testing::writeStampFault)
            testing::writeStampFault("art");
        a.commit();
    } catch (const LibraryError& e) {
        std::string why = e.what();
        try {
            if (testing::writeStampFault)
                testing::writeStampFault("restore");
            if (oldJson)
                Staged(json, *oldJson).commit();
            else if (!fs::remove(json, ec) && ec)
                throw LibraryError(json.string() + ": " + ec.message());
        } catch (const LibraryError& r) {
            why += "; the old sidecar could not be put back either (" + std::string(r.what()) +
                   "), so the new sidecar no longer matches the old art";
        }
        throw LibraryError(why);
    }
}

StampTarget stampPathFor(const fs::path& dir, const std::string& name) {
    const std::string base = slug(name);
    // The slots of this slug: <slug> is 1, <slug>-<n> is n (n >= 2). Names
    // are compared with ASCII case folded, so on a case-insensitive volume
    // OAK-TREE.ans is the same slot as oak-tree.ans (on a case-sensitive one
    // that only reserves a few more names than needed).
    const auto slotOf = [&base](std::string stem) -> long {
        stem = lowered(stem);
        if (stem == base)
            return 1;
        if (stem.size() <= base.size() + 1 || stem.compare(0, base.size(), base) != 0 || stem[base.size()] != '-')
            return 0;
        const std::string digits = stem.substr(base.size() + 1);
        if (digits.size() > 6 || digits[0] == '0' ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return 0;
        const long n = std::stol(digits);
        return n >= 2 ? n : 0;
    };
    const auto pathOf = [&](long n) { return dir / (base + (n > 1 ? "-" + std::to_string(n) : std::string()) + ".ans"); };
    // One pass over the directory: the stamp with this title if there is
    // one, else the lowest slot nothing (of any kind, in any case) sits on.
    // A folder that can't be listed is refused, not taken as empty: that
    // would let a save replace another title without asking. Only a folder
    // that doesn't exist yet is empty.
    std::set<long> taken;
    std::map<long, std::vector<fs::path>> arts;  // each slot's .ans files, as named
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return {pathOf(1), false};
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const bool art = isArtName(p);
        if (!art && !isSidecarName(p))
            continue;
        const long n = slotOf(p.stem().string());
        if (!n)
            continue;
        taken.insert(n);
        if (art)
            arts[n].push_back(p);
    }
    if (ec)
        throw LibraryError(dir.string() + " can't be listed: " + ec.message());
    for (auto& [n, paths] : arts) {
        std::sort(paths.begin(), paths.end());
        for (const fs::path& ans : paths) {
            if (isLink(ans) || !regular(ans))
                continue;
            try {
                const std::optional<Sauce> s = splitSauce(readAll(ans)).sauce;
                // The file as it is named, so a replace writes over it.
                if (s && s->title == name)
                    return {ans, true};
            } catch (const LibraryError&) {
            }
        }
    }
    for (long n = 1;; ++n) {
        if (taken.count(n))
            continue;
        // Nothing listed here, but a name the listing didn't show as this
        // one (case folding past ASCII, a file just made) may still be it.
        const fs::path ans = pathOf(n);
        if (present(ans) || present(sidecarPath(ans)))
            continue;
        return {ans, false};
    }
}

std::string slug(std::string_view name) {
    constexpr std::size_t kMax = 48;
    std::string out;
    bool dash = false;
    for (const char c : name) {
        const char l = lower(c);
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) {
            if (dash && !out.empty())
                out += '-';
            dash = false;
            out += l;
        } else {
            dash = true;
        }
        if (out.size() >= kMax)
            break;
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    if (!out.empty())
        return out;
    // No ASCII letter or digit: the glyphs' code points (♥ is u2665).
    for (const char c : name) {
        if (c == ' ')
            continue;
        char buf[16];
        std::snprintf(buf, sizeof buf, "u%04x", unsigned(cp437ToUnicode(static_cast<std::uint8_t>(c))));
        const std::string part = (out.empty() ? "" : "-") + std::string(buf);
        if (out.size() + part.size() > kMax)
            break;
        out += part;
    }
    return out.empty() ? "stamp" : out;
}

ScanResult scan(const fs::path& dir) {
    ScanResult r;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return r;
    std::vector<fs::path> ans;
    std::set<fs::path> jsons;
    // A walk of our own rather than recursive_directory_iterator, so a
    // folder that can't be read is reported instead of skipped or ending
    // the scan. Links to folders are not followed, so there are no loops.
    // That rule is there so a link isn't followed by accident; it is not a
    // security boundary. A folder swapped for a link between being listed
    // and being opened is followed, but whoever can do that inside the
    // user's library can already write stamps there.
    std::vector<fs::path> todo{dir};
    while (!todo.empty()) {
        const fs::path d = todo.back();
        todo.pop_back();
        std::error_code e;
        fs::directory_iterator it(d, e);
        if (e) {
            r.problems.push_back({d, "folder can't be read (" + e.message() + "), skipped"});
            continue;
        }
        for (const fs::directory_iterator end; it != end; it.increment(e)) {
            const fs::path& p = it->path();
            const bool stampFile = isArtName(p) || isSidecarName(p);
            std::error_code e2;
            const fs::file_status own = it->symlink_status(e2);
            if (e2) {
                // In a folder that can be listed but not searched.
                if (stampFile)
                    r.problems.push_back({p, "can't be read (" + e2.message() + "), skipped"});
                continue;
            }
            if (fs::is_symlink(own)) {
                // Links to files are read through; links to folders are not
                // followed, and both kinds of skip are reported.
                const fs::file_status target = fs::status(p, e2);
                if (!fs::exists(target)) {
                    r.problems.push_back({p, "broken symbolic link, skipped"});
                    continue;
                }
                if (fs::is_directory(target)) {
                    r.problems.push_back({p, "symbolic link to a folder, not followed"});
                    continue;
                }
            } else if (fs::is_directory(own)) {
                todo.push_back(p);
                continue;
            }
            if (!regular(p)) {
                if (stampFile)
                    r.problems.push_back({p, "not a regular file, skipped"});
                continue;
            }
            if (isArtName(p))
                ans.push_back(p);
            else if (isSidecarName(p))
                jsons.insert(p);
        }
        if (e)
            r.problems.push_back({d, "folder could not be read to the end (" + e.message() + ")"});
    }

    std::sort(ans.begin(), ans.end());
    for (const fs::path& p : ans) {
        jsons.erase(pairedSidecar(p));
        try {
            r.stamps.push_back(readStamp(p));
        } catch (const LibraryError& e) {
            r.problems.push_back({p, e.what()});
        }
    }
    for (const fs::path& j : jsons) {
        fs::path a = j;
        a.replace_extension(".ans");
        r.problems.push_back({j, "sidecar with no " + a.filename().string()});
    }
    std::sort(r.problems.begin(), r.problems.end(),
              [](const Problem& a, const Problem& b) { return a.path < b.path; });
    return r;
}

std::vector<std::string> categories(const std::vector<Stamp>& stamps) {
    std::set<std::string> s;
    for (const Stamp& st : stamps)
        s.insert(st.meta.category);
    return {s.begin(), s.end()};
}

bool matches(const Stamp& stamp, std::string_view query) {
    const std::string name = lowered(stamp.displayName());
    std::vector<std::string> tags;
    for (const std::string& t : stamp.meta.tags)
        tags.push_back(lowered(t));
    std::istringstream in{std::string(query)};
    std::string term;
    while (in >> term) {
        term = lowered(term);
        if (name.find(term) == std::string::npos &&
            std::none_of(tags.begin(), tags.end(), [&](const std::string& t) { return t.find(term) != std::string::npos; }))
            return false;
    }
    return true;
}

fs::path realBinDir(const fs::path& exe, const fs::path& fallback) {
    std::error_code ec;
    const fs::path real = fs::canonical(exe, ec);
    if (ec || !real.has_parent_path())
        return fallback;
    return real.parent_path();
}

fs::path bundledLibraryDir(const fs::path& binDir) {
    if (fs::path p = envPath("BLOCKWRIGHT_BUNDLED_LIBRARY"); !p.empty())
        return p;
    return (binDir / BW_LIBRARY_FROM_BINDIR).lexically_normal();
}

fs::path userLibraryDir() {
    if (fs::path p = envPath("BLOCKWRIGHT_USER_LIBRARY"); !p.empty())
        return p;
    // GenericDataLocation/Blockwright/library, shared by the GUI and the CLI
    // (whose application names differ). On Linux and macOS that is the GUI's
    // AppDataLocation + /library; on Windows it is under AppData/Local (the
    // GUI's AppLocalDataLocation), not the roaming AppDataLocation.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return fspath(base) / "Blockwright" / "library";
}

} // namespace bw::library
