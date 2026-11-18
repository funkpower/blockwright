#include "blockwright/library.h"

#include "blockwright/cp437.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/links.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/regular_file.h"
#include "blockwright/text.h"

#include "style_json.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <optional>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#define BW_LIBRARY_POSIX 1
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#define BW_LIBRARY_POSIX 0
#include <cerrno>
#include <cstring>
#include <system_error>
#include <windows.h>
#else
#define BW_LIBRARY_POSIX 0
#endif

#ifndef BW_LIBRARY_FROM_BINDIR
#define BW_LIBRARY_FROM_BINDIR "../share/blockwright/library"
#endif

namespace fs = std::filesystem;

namespace bw::library {

std::string readRegularFile(const fs::path& p, std::size_t limit) {
    try {
        return bw::readRegular(p, limit);
    } catch (const RegularFileError& e) {
        switch (e.reason) {
        case RegularFileError::Reason::Missing:
            throw LibraryError("can't be read");
        case RegularFileError::Reason::NotRegular:
            throw LibraryError("is not a regular file");
        default:
            throw LibraryError(e.what());
        }
    }
}

namespace {

// A stamp is read as its sidecar says (CP437 unless "encoding": "utf-8" and
// a byte order mark), never by detectEncoding, so a stamp's cells don't hang
// on what its bytes happen to look like.
ParseOptions stampParse(TextEncoding encoding) { return {.encoding = encoding}; }

constexpr std::string_view kBom = "\xEF\xBB\xBF";

constexpr const char* kCategory = "category";
constexpr const char* kTags = "tags";
constexpr const char* kIrcBytes = "ircBytes";
constexpr const char* kEncoding = "encoding";
constexpr const char* kUtf8Name = "utf-8";
constexpr const char* kInsets = "insets";

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

// A junction is a link too (links.h).
bool isLink(const fs::path& p) { return bw::isLink(p); }

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
    const fs::file_status st = linkStatus(p, ec);
    if (fs::is_symlink(st))
        throw LibraryError(p.string() + " is a symbolic link; a stamp is never written through one");
    if (fs::exists(st) && !fs::is_regular_file(st))
        throw LibraryError(p.string() + " is not a regular file");
}

// `bytes` written to a new temporary file beside `target`, renamed over
// it by commit() and removed if it never is, including when the
// constructor itself throws. Paths stay std::filesystem paths throughout,
// so a name that isn't UTF-8 is written as itself (a QString would turn its
// bytes into U+FFFD). Replacing a file keeps its permission bits (0777; not
// set-ID or sticky); a new one gets `mode` when given (its 0777 bits, POSIX
// only), else 0666 less the umask.
class Staged {
public:
    Staged(const fs::path& target, const std::string& bytes, std::optional<fs::perms> mode = {})
        : target_(target), mode_(mode) {
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
        moved();
    }

    // Moves the temporary to the target only if nothing sits there, as one
    // step: a file made there at any moment is never replaced. Throws
    // LibraryError ("already exists") when one is; the temporary is then
    // removed with the Staged.
    void commitNoReplace() {
#if BW_LIBRARY_POSIX
        int err = 0;
        if (fault("rename")) {
            err = EINVAL;
        } else {
#if defined(__linux__) && defined(RENAME_NOREPLACE)
            if (::renameat2(AT_FDCWD, temp_.c_str(), AT_FDCWD, target_.c_str(), RENAME_NOREPLACE) == 0) {
                moved();
                return;
            }
            err = errno;
#elif defined(__APPLE__)
            if (::renamex_np(temp_.c_str(), target_.c_str(), RENAME_EXCL) == 0) {
                moved();
                return;
            }
            err = errno;
#else
            err = ENOSYS;
#endif
        }
        // A file system without the exclusive rename: a hard link fails
        // with EEXIST as atomically, then the temporary's name goes.
        if (err == EINVAL || err == ENOSYS || err == ENOTSUP) {
            if (::link(temp_.c_str(), target_.c_str()) == 0) {
                // Published. The temporary's name is a second link to the
                // preset now; if it won't go, discard() tries again, as for
                // any temporary left over.
                if (!fault("unlink") && ::unlink(temp_.c_str()) == 0)
                    moved();
                return;
            }
            err = errno;
        }
        if (err == EEXIST)
            throw LibraryError(target_.string() + " already exists; nothing was written over it");
        throw LibraryError(target_.string() + ": write failed: " + std::strerror(err));
#elif defined(_WIN32)
        // MoveFileExW without MOVEFILE_REPLACE_EXISTING refuses a target
        // that exists. (The Windows port, #139, builds this branch.)
        if (::MoveFileExW(temp_.c_str(), target_.c_str(), MOVEFILE_WRITE_THROUGH)) {
            moved();
            return;
        }
        const DWORD e = ::GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)
            throw LibraryError(target_.string() + " already exists; nothing was written over it");
        throw LibraryError(target_.string() + ": write failed");
#else
#error "Staged::commitNoReplace needs an exclusive rename on this platform"
#endif
    }

private:
#if BW_LIBRARY_POSIX
    // Whether testing::textStylePublishFault fails `step`.
    static bool fault(std::string_view step) {
        if (!testing::textStylePublishFault)
            return false;
        try {
            testing::textStylePublishFault(step);
        } catch (const LibraryError&) {
            return true;
        }
        return false;
    }
#endif

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
            const bool chmodded = replacing || mode_;
            const int fd = ::open(temp_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, chmodded ? 0600 : 0666);
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
            if (chmodded && ::fchmod(fd_, (replacing ? old.st_mode : static_cast<mode_t>(*mode_)) & 0777) != 0)
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
#elif defined(_WIN32)
            // CREATE_NEW takes the name only if nothing has it, as O_EXCL
            // does; the file is flushed to the disk before it is renamed.
            const HANDLE h = ::CreateFileW(temp_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                const DWORD e = ::GetLastError();
                if ((e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) && attempt < 100)
                    continue;
                temp_.clear();
                throw LibraryError(target_.string() + ": " + std::system_category().message(int(e)));
            }
            created_ = true;
            const auto failed = [&](std::string why) {
                ::CloseHandle(h);
                throw LibraryError(target_.string() + ": write failed: " + why);
            };
            const char* data = bytes.data();
            std::size_t left = bytes.size();
            while (left) {
                DWORD n = 0;
                if (!::WriteFile(h, data, DWORD(std::min<std::size_t>(left, 1u << 30)), &n, nullptr) || n == 0)
                    failed(std::system_category().message(int(::GetLastError())));
                data += n;
                left -= n;
            }
            if (!::FlushFileBuffers(h))
                failed(std::system_category().message(int(::GetLastError())));
            if (testing::writeStampFault) {
                try {
                    testing::writeStampFault("fsync");
                } catch (const LibraryError&) {
                    failed(std::strerror(ENOSPC));
                }
            }
            if (!::CloseHandle(h))
                throw LibraryError(target_.string() + ": write failed: " +
                                   std::system_category().message(int(::GetLastError())));
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

    // The temporary's name is gone: renamed to the target, or unlinked.
    void moved() {
        temp_.clear();
        created_ = false;
    }

    // Closes the descriptor and removes the temporary, if its name is still
    // there.
    void discard() noexcept {
#if BW_LIBRARY_POSIX
        if (fd_ >= 0)
            ::close(std::exchange(fd_, -1));
#endif
        if (created_) {
            std::error_code ec;
            fs::remove(temp_, ec);
            temp_.clear();
            created_ = false;
        }
    }

    static long processId() {
#if BW_LIBRARY_POSIX
        return long(::getpid());
#elif defined(_WIN32)
        return long(::GetCurrentProcessId());
#else
        return 0;
#endif
    }

    fs::path target_, temp_;
    std::optional<fs::perms> mode_;
#if BW_LIBRARY_POSIX
    int fd_ = -1;
#endif
    bool created_ = false;  // temp_ exists (and is ours to remove)
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

SentRow widestSentRow(const Canvas& canvas, std::size_t budget) {
    SentRow widest;
    for (const std::string& line : irc::encodeCanvas(canvas))
        if (const std::size_t sent = irc::txtSentBytes(line, budget); sent > widest.sent)
            widest = {sent, line.size()};
    return widest;
}

std::vector<Problem> budgetProblems(const ScanResult& scanned, std::size_t budget) {
    std::vector<Problem> problems;
    const std::string over = ", over the " + std::to_string(budget) + "-byte budget";
    // `written` is the widest row as written; the art's rows are measured
    // again as the ascii script sends them
    const auto check = [&](const fs::path& path, const std::string& what, std::size_t written, const Canvas& art) {
        if (const SentRow sent = widestSentRow(art, budget); sent.sent > budget)
            problems.push_back({path, what + " is " + std::to_string(sent.sent) +
                                          " IRC bytes as WeeChat's ascii script sends it (" +
                                          std::to_string(sent.written) + " as written)" + over});
        else if (written > budget)
            problems.push_back({path, what + " is " + std::to_string(written) + " IRC bytes" + over});
    };
    for (const Stamp& s : scanned.stamps)
        check(s.path, "the widest row", s.meta.ircBytes, s.art.canvas);
    for (const TextStyleEntry& t : scanned.styles)
        for (const bool ice : {true, false}) {
            if (!ice && t.iceRequired)
                continue;
            const std::string colours = std::string("iCE colours ") + (ice ? "on" : "off");
            try {
                const Canvas sample = textStyleSample(t.style, ice);
                check(t.path, "the sample's widest row with " + colours, widestRowBytes(sample), sample);
            } catch (const std::exception& e) {
                problems.push_back({t.path, "the sample can't be drawn with " + colours + ": " + e.what()});
            }
        }
    return problems;
}

namespace {

// A JSON value as compact text; QJsonDocument does the escaping.
std::string jsonText(const QJsonValue& v) {
    const QByteArray a = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return a.mid(1, a.size() - 2).toStdString();
}

std::string quoted(const std::string& s) { return jsonText(QString::fromStdString(s)); }

std::string tagsJson(const std::vector<std::string>& tags) {
    std::string out = "[";
    for (std::size_t i = 0; i < tags.size(); ++i)
        out += (i ? ", " : "") + quoted(tags[i]);
    return out + "]";
}

// The top-level object of `json`, refused (as `what`) unless it is one
// whose keys are all `known` and no object in it, at any depth, holds a key
// twice, as a project file's are refused (project::repeatedKey).
QJsonObject knownObject(std::string_view json, const std::string& what, std::initializer_list<const char*> known) {
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray(json.data(), static_cast<qsizetype>(json.size())), &err);
    if (err.error != QJsonParseError::NoError)
        throw LibraryError(what + " is not JSON: " + err.errorString().toStdString() + " at offset " +
                           std::to_string(err.offset));
    if (!doc.isObject())
        throw LibraryError(what + " is not a JSON object");
    if (const auto key = project::repeatedKey(json))
        throw LibraryError(what + " has the key \"" + key->toStdString() + "\" more than once");
    const QJsonObject o = doc.object();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (std::none_of(known.begin(), known.end(), [&](const char* k) { return it.key() == QLatin1String(k); }))
            throw LibraryError(what + " has an unknown key \"" + it.key().toStdString() + "\"");
    return o;
}

} // namespace

std::string sidecarJson(const StampMeta& meta) {
    std::string insets;
    if (const auto& sl = meta.slice) {
        if (!sl->width)
            throw LibraryError("a stamp's sidecar can't hold a slice that resizes in height only");
        insets = ",\n  \"insets\": [" + std::to_string(sl->left) + ", " + std::to_string(sl->right) +
                 (sl->height ? ", " + std::to_string(sl->top) + ", " + std::to_string(sl->bottom) : std::string()) +
                 "]";
    }
    // Written by hand so the key order is fixed and diffs stay small.
    return "{\n  \"category\": " + quoted(meta.category) + ",\n  \"tags\": " + tagsJson(meta.tags) +
           ",\n  \"ircBytes\": " + std::to_string(meta.ircBytes) + insets +
           (meta.encoding == TextEncoding::Utf8 ? ",\n  \"encoding\": \"" + std::string(kUtf8Name) + "\"" : "") +
           "\n}\n";
}

StampMeta parseSidecar(std::string_view json) {
    const QJsonObject o = knownObject(json, "sidecar", {kCategory, kTags, kIrcBytes, kEncoding, kInsets});
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
    // Absent for CP437: only a UTF-8 stamp's sidecar names its encoding.
    if (o.contains(QLatin1String(kEncoding))) {
        if (o.value(QLatin1String(kEncoding)).toString() != QLatin1String(kUtf8Name))
            throw LibraryError("sidecar \"encoding\" is not \"utf-8\" (a CP437 stamp's sidecar has no \"encoding\")");
        m.encoding = TextEncoding::Utf8;
    }
    // Absent for a stamp of a fixed size. Each number is bounded before it
    // is made an int; readStamp checks them against the art.
    if (o.contains(QLatin1String(kInsets))) {
        const QJsonValue v = o.value(QLatin1String(kInsets));
        const QJsonArray a = v.toArray();
        if (!v.isArray() || (a.size() != 2 && a.size() != 4))
            throw LibraryError("sidecar \"insets\" is not [left, right] or [left, right, top, bottom]");
        int n[4] = {0, 0, 0, 0};
        for (qsizetype i = 0; i < a.size(); ++i) {
            const double d = a[i].toDouble(-1);
            if (!a[i].isDouble() || d < 0 || d != std::floor(d) || d > kMaxRows)
                throw LibraryError("sidecar \"insets\" holds something other than a whole number of cells from 0 to " +
                                   std::to_string(kMaxRows));
            n[i] = static_cast<int>(d);
        }
        m.slice = StampSlice{n[0], n[1], n[2], n[3], true, a.size() == 4};
    }
    return m;
}

fs::path sidecarPath(const fs::path& ans) {
    fs::path p = ans;
    p.replace_extension(".json");
    return p;
}

bool isArtName(const fs::path& p) { return lowered(p.extension().string()) == ".ans"; }
bool isSidecarName(const fs::path& p) { return lowered(p.extension().string()) == ".json"; }

namespace {

// The sidecar `ans` pairs with among `listed`, the sidecar names (sorted)
// beside it with its stem: the exact <stem>.json, else the first case
// variant. Pairing goes by what the folder lists, not by a lookup: on a
// case-insensitive file system (macOS's and Windows's default) a lookup of
// <stem>.json finds <stem>.JSON too, under a name the folder doesn't have.
fs::path chooseSidecar(const fs::path& ans, const std::vector<fs::path>& listed) {
    const fs::path exact = sidecarPath(ans);
    for (const fs::path& p : listed)
        if (p.filename() == exact.filename())
            return p;
    return listed.empty() ? exact : listed.front();
}

} // namespace

fs::path pairedSidecar(const fs::path& ans) {
    std::vector<fs::path> found;
    std::error_code ec;
    const fs::path dir = ans.has_parent_path() ? ans.parent_path() : fs::path(".");
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.stem() == ans.stem() && isSidecarName(p))
            found.push_back(ans.has_parent_path() ? p : p.filename());
    }
    std::sort(found.begin(), found.end());
    return chooseSidecar(ans, found);
}

// ---- Text styles ----

namespace {

constexpr const char* kKind = "kind";
constexpr const char* kName = "name";
constexpr const char* kIce = "ice";
constexpr const char* kEffects = "effects";
constexpr const char* kTextStyleKind = "text-style";
constexpr const char* kIceRequired = "required";

std::string textField(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString())
        throw LibraryError(std::string("text style \"") + key + "\" is missing or not a string");
    std::string s = v.toString().toStdString();
    checkText(s, (std::string("text style \"") + key + "\"").c_str());
    return s;
}

} // namespace

bool isTextStyleName(const fs::path& p) {
    const std::string name = lowered(p.filename().string());
    constexpr std::string_view suffix = ".style.json";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string textStyleEntryJson(const TextStyleEntry& entry) {
    // By hand, as the sidecar is: the entry's keys in a fixed order, each
    // effect on a line of its own with its kind and switch first.
    std::string out = "{\n  \"kind\": " + quoted(kTextStyleKind) + ",\n  \"name\": " + quoted(entry.name) +
                      ",\n  \"category\": " + quoted(entry.category) + ",\n  \"tags\": " + tagsJson(entry.tags);
    if (entry.iceRequired)
        out += ",\n  \"ice\": " + quoted(kIceRequired);
    out += ",\n  \"effects\": [";
    const QJsonArray effects = project::textStyleJson(entry.style).value(QLatin1String(kEffects)).toArray();
    for (qsizetype i = 0; i < effects.size(); ++i) {
        const QJsonObject e = effects[i].toObject();
        out += std::string(i ? "," : "") + "\n    {\"kind\": " + jsonText(e.value(QLatin1String(kKind))) +
               ", \"enabled\": " + jsonText(e.value(QLatin1String("enabled")));
        for (auto it = e.begin(); it != e.end(); ++it)
            if (it.key() != QLatin1String(kKind) && it.key() != QLatin1String("enabled"))
                out += ", " + quoted(it.key().toStdString()) + ": " + jsonText(it.value());
        out += "}";
    }
    out += effects.isEmpty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

TextStyleEntry parseTextStyleEntry(std::string_view json) {
    // The kind first: a file without one (a stamp's sidecar given the
    // suffix, say) is told what a .style.json holds, not about its keys.
    const QJsonDocument peek = QJsonDocument::fromJson(QByteArray(json.data(), static_cast<qsizetype>(json.size())));
    if (peek.isObject() && !peek.object().contains(QLatin1String(kKind)))
        throw LibraryError("text style has no \"kind\" (a .style.json holds \"kind\": \"text-style\")");
    const QJsonObject o =
        knownObject(json, "text style", {kKind, kName, kCategory, kTags, kIce, kEffects});
    const QJsonValue kind = o.value(QLatin1String(kKind));
    if (kind != QJsonValue(QLatin1String(kTextStyleKind)))
        throw LibraryError("text style \"kind\" is " + jsonText(kind) + ", not \"text-style\"");
    TextStyleEntry entry;
    entry.name = textField(o, kName);
    entry.category = textField(o, kCategory);
    const QJsonValue tags = o.value(QLatin1String(kTags));
    if (!tags.isArray())
        throw LibraryError("text style \"tags\" is missing or not an array");
    for (const QJsonValue& t : tags.toArray()) {
        if (!t.isString())
            throw LibraryError("text style \"tags\" holds something other than a string");
        entry.tags.push_back(t.toString().toStdString());
        checkTag(entry.tags.back(), "a text style tag");
    }
    if (o.contains(QLatin1String(kIce))) {
        const QJsonValue ice = o.value(QLatin1String(kIce));
        if (ice != QJsonValue(QLatin1String(kIceRequired)))
            throw LibraryError("text style \"ice\" is " + jsonText(ice) + "; the only value it takes is \"required\"");
        entry.iceRequired = true;
    }
    const QJsonValue effects = o.value(QLatin1String(kEffects));
    if (!effects.isArray())
        throw LibraryError("text style \"effects\" is missing or not a list");
    if (effects.toArray().isEmpty())
        throw LibraryError("text style has no effects, so it draws nothing a plain layer doesn't");
    try {
        // "text" + " style …" in the reader's messages
        entry.style = project::textStyleFromJson(QJsonObject{{QLatin1String(kEffects), effects}}, "text");
    } catch (const project::ProjectError& e) {
        throw LibraryError(e.what());
    }
    // A flat Fill's ramp draws nothing, so the project reader lets it be;
    // a preset keeps it for when the Fill is switched to a gradient, so it
    // must be one this build has, switched on or off.
    for (const Effect& e : entry.style.effects)
        if (const auto* fill = std::get_if<FillEffect>(&e.settings); fill && !findRamp(fill->ramp))
            throw LibraryError("text style: fill ramp \"" + fill->ramp + "\" is not a named ramp");
    // The project reader skips keys it doesn't know (a newer minor
    // version's); an entry, like a sidecar, refuses them, so a typo is
    // caught: every key must be one the effect is written with.
    const QJsonArray in = effects.toArray();
    const QJsonArray out = project::textStyleJson(entry.style).value(QLatin1String(kEffects)).toArray();
    for (qsizetype i = 0; i < in.size(); ++i) {
        const QJsonObject e = in[i].toObject(), known = out[i].toObject();
        for (auto it = e.begin(); it != e.end(); ++it)
            if (!known.contains(it.key()))
                throw LibraryError("text style effect " + std::to_string(i + 1) + " (" +
                                   known.value(QLatin1String(kKind)).toString().toStdString() +
                                   ") has an unknown key \"" + it.key().toStdString() + "\"");
    }
    return entry;
}

TextStyleEntry readTextStyleEntry(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(fs::status(p, ec)))
        throw LibraryError(".style.json can't be read");
    if (!regular(p))
        throw LibraryError(".style.json is not a regular file");
    std::string json;
    try {
        json = readAll(p);
    } catch (const LibraryError& e) {
        throw LibraryError(std::string(".style.json ") + e.what());
    }
    TextStyleEntry entry = parseTextStyleEntry(json);
    entry.path = p;
    return entry;
}

Canvas textStyleSample(const TextStyle& style, bool ice) {
    TextSource source;
    source.text = U"Aa";
    return renderStyledText(source, style, blockFace(), ice).cells;
}

namespace {

// The lowest of <slug>.<suffix>, <slug>-2.<suffix>, … in `dir` that nothing
// sits on, compared with ASCII case folded.
fs::path freePathFor(const fs::path& dir, const std::string& name, std::string_view suffix) {
    const std::string base = slug(name);
    const auto nameOf = [&base, suffix](long n) {
        return base + (n > 1 ? "-" + std::to_string(n) : std::string()) + std::string(suffix);
    };
    std::set<std::string> taken;  // lowered file names
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return dir / nameOf(1);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec))
        taken.insert(lowered(it->path().filename().string()));
    if (ec)
        throw LibraryError(dir.string() + " can't be listed: " + ec.message());
    for (long n = 1;; ++n) {
        const std::string file = nameOf(n);
        if (taken.count(lowered(file)) || present(dir / file))
            continue;
        return dir / file;
    }
}

constexpr std::string_view kFrameSuffix = ".frame.json";

} // namespace

fs::path textStylePathFor(const fs::path& dir, const std::string& name) {
    return freePathFor(dir, name, ".style.json");
}


fs::path writeTextStyle(const fs::path& dir, const TextStyleEntry& entry) {
    refuseInsideBundled(dir);
    const std::string json = textStyleEntryJson(entry);
    parseTextStyleEntry(json);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        throw LibraryError(dir.string() + ": " + ec.message());
    const fs::path p = textStylePathFor(dir, entry.name);
    Staged staged(p, json);
    if (testing::beforeTextStylePublish)
        testing::beforeTextStylePublish(p);
    staged.commitNoReplace();
    return p;
}

bool isFrameStyleName(const fs::path& p) {
    const std::string name = lowered(p.filename().string());
    return name.size() > kFrameSuffix.size() &&
           name.compare(name.size() - kFrameSuffix.size(), kFrameSuffix.size(), kFrameSuffix) == 0;
}

FrameStyleEntry readFrameStyleEntry(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(fs::status(p, ec)))
        throw LibraryError(".frame.json can't be read");
    if (!regular(p))
        throw LibraryError(".frame.json is not a regular file");
    std::string json;
    try {
        json = readRegularFile(p, kMaxFrameStyleBytes);
    } catch (const LibraryError& e) {
        throw LibraryError(std::string(".frame.json ") + e.what());
    }
    std::string why;
    std::optional<FrameStyle> style = frameStyleFromJson(json, &why);
    if (!style)
        throw LibraryError("frame style: " + why);
    return {p, std::move(*style)};
}

fs::path frameStylePathFor(const fs::path& dir, const std::string& name) {
    return freePathFor(dir, name, kFrameSuffix);
}

fs::path writeFrameStyle(const fs::path& dir, const FrameStyle& style) {
    refuseInsideBundled(dir);
    checkText(style.name, "frame style name");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        throw LibraryError(dir.string() + ": " + ec.message());
    const fs::path p = frameStylePathFor(dir, style.name);
    FrameStyle named = style;
    const std::string file = p.filename().string();
    named.id = file.substr(0, file.size() - kFrameSuffix.size());
    const std::string json = frameStyleJson(named);
    std::string why;
    if (!frameStyleFromJson(json, &why))
        throw LibraryError("frame style: " + why);
    Staged staged(p, json);
    staged.commitNoReplace();
    return p;
}

namespace {

// The checks readStamp and parseStamp share: `data` is the .ans and
// `readSidecar` gives its sidecar, called once, before the art when the
// .ans starts with a byte order mark and after it otherwise.
template <typename ReadSidecar> Stamp stampFrom(const std::string& data, ReadSidecar readSidecar) {
    Stamp s;
    // A stamp that starts with a byte order mark is UTF-8, and its sidecar
    // must say so: it is read first, and one that doesn't is refused (no
    // CP437 stamp Blockwright writes starts with the mark; they start with
    // ESC). Any other stamp is read as CP437, as it always was, and its
    // sidecar checked after the art.
    const bool bom = data.starts_with(kBom);
    std::optional<StampMeta> meta;
    if (bom) {
        meta = readSidecar();
        if (meta->encoding != TextEncoding::Utf8)
            throw LibraryError(".ans starts with a byte order mark, but its sidecar has no \"encoding\": \"utf-8\"");
    }
    const TextEncoding encoding = bom ? TextEncoding::Utf8 : TextEncoding::Cp437;
    try {
        s.art = parseAnsi(data, stampParse(encoding));
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

    s.meta = meta ? *meta : readSidecar();
    if (s.meta.encoding == TextEncoding::Utf8 && !bom)
        throw LibraryError("sidecar says the .ans is UTF-8, but it doesn't start with a byte order mark");
    std::size_t actual = 0;
    try {
        actual = widestRowBytes(s.art.canvas);
    } catch (const irc::IrcError& e) {
        throw LibraryError(std::string("the IRC encoder failed: ") + e.what());
    }
    if (actual != s.meta.ircBytes)
        throw LibraryError("sidecar says the widest row is " + std::to_string(s.meta.ircBytes) +
                           " IRC bytes; the art's is " + std::to_string(actual));
    if (s.meta.slice)
        if (const std::string why = sliceProblem(s.block(), *s.meta.slice); !why.empty())
            throw LibraryError("sidecar \"insets\" can't cut the art: " + why);
    return s;
}

} // namespace

Stamp readStamp(const fs::path& ans) {
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
    Stamp s = stampFrom(data, [&] {
        const fs::path side = pairedSidecar(ans);
        if (!fs::exists(fs::status(side, ec)))
            throw LibraryError("no sidecar " + side.filename().string());
        if (!regular(side))
            throw LibraryError("sidecar " + side.filename().string() + " is not a regular file");
        std::string json;
        try {
            json = readRegularFile(side, kMaxSidecarBytes);
        } catch (const LibraryError& e) {
            throw LibraryError("sidecar " + std::string(e.what()));
        }
        return parseSidecar(json);
    });
    s.path = ans;
    return s;
}

Stamp parseStamp(const StampFiles& files) {
    return stampFrom(files.ans, [&] {
        if (files.json.size() > kMaxSidecarBytes)
            throw LibraryError("sidecar is larger than " + std::to_string(kMaxSidecarBytes) + " bytes");
        return parseSidecar(files.json);
    });
}

namespace {
StampFiles encodeChecked(const std::string& name, const std::string& category, const std::vector<std::string>& tags,
                         const CellBlock& cells, bool ice, const std::optional<StampSlice>& slice);
} // namespace

std::string stampText(const std::string& text) {
    return QString::fromStdString(text).trimmed().toStdString();
}

std::vector<std::string> stampTags(const std::vector<std::string>& tags) {
    std::vector<std::string> out;
    for (const std::string& t : tags)
        if (std::string s = stampText(t); !s.empty() && std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(std::move(s));
    return out;
}

StampFiles encodeStamp(const std::string& name, const std::string& category, const std::vector<std::string>& tags,
                       const CellBlock& cells, bool ice, const std::optional<StampSlice>& slice) {
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
    if (slice) {
        if (const std::string why = sliceProblem(cells, *slice); !why.empty())
            throw LibraryError("the stamp can't be resizable: " + why);
        if (!slice->width)
            throw LibraryError("a stamp resizes in width and height, or in width only");
    }
    try {
        return encodeChecked(name, category, tags, cells, ice, slice);
    } catch (const AnsiError& e) {
        throw LibraryError(std::string("the art can't be written as .ans: ") + e.what());
    } catch (const irc::IrcError& e) {
        throw LibraryError(std::string("the IRC encoder failed: ") + e.what());
    }
}

std::string stampLossMessage(const AnsiOutput& out) {
    if (!out.unwritable.empty()) {
        const auto count = [&](Loss loss) {
            return int(std::count_if(out.unwritable.begin(), out.unwritable.end(),
                                     [&](const Unwritable& u) { return u.loss == loss; }));
        };
        const auto cells = [](int n) { return std::to_string(n) + (n == 1 ? " cell holds" : " cells hold"); };
        if (out.encoding == TextEncoding::Utf8) {
            // What even a UTF-8 stamp can't hold (writeAnsi would write each as '?').
            const int controls = count(Loss::Control), loneWide = count(Loss::LoneWide),
                      marks = count(Loss::LoneMark), wide = count(Loss::Wide);
            std::string why;
            const auto add = [&](int n, const char* one, const char* many) {
                if (n)
                    why += (why.empty() ? "" : "; ") + std::to_string(n) + (n == 1 ? one : many);
            };
            add(controls, " cell holds a control character", " cells hold a control character");
            add(loneWide, " wide glyph has no cell for its right half", " wide glyphs have no cell for their right half");
            add(marks, " mark has no glyph of its own to sit on", " marks have no glyph of their own to sit on");
            add(wide, " wide glyph isn't two columns wide", " wide glyphs aren't two columns wide");
            return why + ", which a stamp can't store";
        }
        const int uncoded = count(Loss::NoCode), controls = count(Loss::Control), marks = count(Loss::MarksDropped),
                  wide = count(Loss::Wide);
        std::string why;
        const auto add = [&](const std::string& clause) { why += (why.empty() ? "" : " and ") + clause; };
        if (uncoded)
            add(cells(uncoded) + " a glyph with no CP437 code");
        if (controls)
            add(cells(controls) + " a glyph whose CP437 byte .ans reads as a control code (" +
                ansControlNames(out.unwritable) + ")");
        if (marks)
            add(cells(marks) + " a glyph whose marks CP437 drops");
        if (!why.empty())
            why += ", which .ans can't store";
        if (wide)
            why += (why.empty() ? "" : "; ") + std::to_string(wide) +
                   (wide == 1 ? " wide glyph written as ?" : " wide glyphs written as ?");
        return why;
    }
    if (out.blinkDropped)
        return std::to_string(out.blinkDropped) +
               (out.blinkDropped == 1 ? " blinking cell would lose its blink" : " blinking cells would lose their blink");
    return {};
}

namespace {

StampFiles encodeChecked(const std::string& name, const std::string& category, const std::vector<std::string>& tags,
                         const CellBlock& cells, bool ice, const std::optional<StampSlice>& slice) {
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
    AnsiOutput out = writeAnsi(doc);
    // CP437 when it holds every cell as it is; a cluster CP437 composes to
    // one byte (e + U+0301 as é) would read back as another glyph.
    bool cluster = false;
    for (int y = 0; y < cells.height; ++y)
        for (int x = 0; x < cells.width; ++x)
            cluster = cluster || doc.canvas.at(x, y).ch.isCluster();
    if (!out.unwritable.empty() || cluster)
        out = writeAnsi(doc, TextEncoding::Utf8, true);
    if (const std::string why = stampLossMessage(out); !why.empty())
        throw LibraryError(why);

    // Check and count the art as it reads back, which is what readStamp
    // checks the sidecar against.
    const Canvas back = parseAnsi(out.bytes, stampParse(out.encoding)).canvas;
    if (allBlank(back))
        throw LibraryError("every cell is blank, so the stamp would draw nothing");
    StampMeta meta{category, tags, widestRowBytes(back), out.encoding, slice};
    return {out.bytes, sidecarJson(meta)};
}

} // namespace

void writeStamp(const fs::path& ans, const StampFiles& files) {
    // Every stamp write in a library comes here: the folder it lands in is
    // checked, whatever the caller checked above it.
    refuseInsideBundled(ans.has_parent_path() ? ans.parent_path() : fs::path("."));
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

namespace {

// The slug of a name given as its code points: ASCII letters and digits,
// lower-cased, runs of anything else as one '-'; for a name with neither,
// the code points ("u2665"), spaces skipped. Both slug() and
// collectionSlug() are this, so a glyph slugs alike in either.
std::string slugOf(const std::vector<char32_t>& cps) {
    constexpr std::size_t kMax = 48;
    std::string out;
    bool dash = false;
    for (const char32_t cp : cps) {
        const char l = cp < 0x80 ? lower(static_cast<char>(cp)) : '\0';
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
    for (const char32_t cp : cps) {
        if (cp == U' ')
            continue;
        char buf[16];
        std::snprintf(buf, sizeof buf, "u%04x", unsigned(cp));
        const std::string part = (out.empty() ? "" : "-") + std::string(buf);
        if (out.size() + part.size() > kMax)
            break;
        out += part;
    }
    return out.empty() ? "stamp" : out;
}

} // namespace

std::string slug(std::string_view name) {
    std::vector<char32_t> cps;
    cps.reserve(name.size());
    for (const char c : name)
        cps.push_back(cp437ToUnicode(static_cast<std::uint8_t>(c)));
    return slugOf(cps);
}

ScanResult scan(const fs::path& dir) {
    if (testing::scanned)
        testing::scanned(dir);
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
            const fs::file_status own = linkStatus(*it, e2);  // a junction is a link (links.h)
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
    // pairs as pairedSidecar does, from this listing
    std::map<std::pair<fs::path, fs::path>, std::vector<fs::path>> sidecars;
    for (const fs::path& j : jsons)
        sidecars[{j.parent_path(), j.stem()}].push_back(j);
    for (const fs::path& p : ans) {
        // Its sidecar's name would be a text style's, which is read as one.
        if (isTextStyleName(sidecarPath(p))) {
            r.problems.push_back({p, "a stamp can't be named " + p.filename().string() + ": its sidecar " +
                                         sidecarPath(p).filename().string() + " would be a text style's file"});
            continue;
        }
        if (isFrameStyleName(sidecarPath(p))) {
            r.problems.push_back({p, "a stamp can't be named " + p.filename().string() + ": its sidecar " +
                                         sidecarPath(p).filename().string() + " would be a frame style's file"});
            continue;
        }
        if (const auto it = sidecars.find({p.parent_path(), p.stem()}); it != sidecars.end())
            jsons.erase(chooseSidecar(p, it->second));
        try {
            r.stamps.push_back(readStamp(p));
        } catch (const LibraryError& e) {
            r.problems.push_back({p, e.what()});
        } catch (const std::bad_alloc&) {
            r.problems.push_back({p, "out of memory reading it"});
        }
    }
    for (const fs::path& j : jsons) {
        if (isTextStyleName(j)) {
            try {
                r.styles.push_back(readTextStyleEntry(j));
            } catch (const LibraryError& e) {
                r.problems.push_back({j, e.what()});
            }
            continue;
        }
        if (isFrameStyleName(j)) {
            try {
                r.frames.push_back(readFrameStyleEntry(j));
            } catch (const LibraryError& e) {
                r.problems.push_back({j, e.what()});
            } catch (const std::bad_alloc&) {
                r.problems.push_back({j, "frame style: out of memory reading it"});
            }
            continue;
        }
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

namespace {

// Every whitespace-separated term of `query` is in `name` or one of `tags`,
// ignoring ASCII case.
bool matchesTerms(const std::string& name, const std::vector<std::string>& tags, std::string_view query) {
    const std::string lname = lowered(name);
    std::vector<std::string> ltags;
    for (const std::string& t : tags)
        ltags.push_back(lowered(t));
    std::istringstream in{std::string(query)};
    std::string term;
    while (in >> term) {
        term = lowered(term);
        if (lname.find(term) == std::string::npos &&
            std::none_of(ltags.begin(), ltags.end(), [&](const std::string& t) { return t.find(term) != std::string::npos; }))
            return false;
    }
    return true;
}

} // namespace

bool matches(const Stamp& stamp, std::string_view query) {
    return matchesTerms(stamp.displayName(), stamp.meta.tags, query);
}

bool matches(const TextStyleEntry& style, std::string_view query) {
    return matchesTerms(style.name, style.tags, query);
}

// Collections (#464).

namespace {

std::string q(const std::string& s) { return "\"" + s + "\""; }

void checkCollectionName(const std::string& name) {
    checkText(name, "the collection name");
    for (std::size_t i = 0; i < name.size();)
        if (!nextUtf8(name, i))
            throw LibraryError("the collection name isn't UTF-8");
    if (name.size() > kMaxCollectionName)
        throw LibraryError("the collection name is longer than " + std::to_string(kMaxCollectionName) + " bytes");
}

// A folder's own name as a collection's: each control character and each
// byte that isn't UTF-8 as U+FFFD, so `library list` never prints a tab or
// newline in a name.
std::string folderName(const std::string& raw) {
    static const std::string kReplacement = "\xEF\xBF\xBD";
    std::string out;
    for (std::size_t i = 0; i < raw.size();) {
        const std::size_t at = i;
        const std::optional<char32_t> cp = nextUtf8(raw, i);
        if (i == at)
            ++i;
        if (!cp || *cp < 0x20 || *cp == 0x7F)
            out += kReplacement;
        else
            out.append(raw, at, i - at);
    }
    return out;
}

fs::path canonicalish(const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::weakly_canonical(p, ec);
    if (ec)
        r = fs::absolute(p, ec).lexically_normal();
    return r;
}

// `inner` is `outer` or inside it, both already canonical.
bool within(const fs::path& inner, const fs::path& outer) {
    const fs::path rel = inner.lexically_relative(outer);
    return !rel.empty() && *rel.begin() != "..";
}

} // namespace

#ifdef BW_TEST_HOOKS
std::function<bool(const fs::path&, const fs::path&)>& sameFolderHook() {
    static std::function<bool(const fs::path&, const fs::path&)> hook;
    return hook;
}
#endif

namespace {

// The same folder, by file identity (device and inode; volume and file index
// on Windows), so a bind mount or a second export of it counts. False when
// either is missing or can't be asked.
bool sameFolder(const fs::path& a, const fs::path& b) {
#ifdef BW_TEST_HOOKS
    if (sameFolderHook() && sameFolderHook()(a, b))
        return true;
#endif
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

// `folder` or one of its ancestors is the same folder as `other`: 0 for
// `folder` itself, 1 for an ancestor, -1 for none.
int sameFolderUp(const fs::path& folder, const fs::path& other) {
    int depth = 0;
    for (fs::path p = folder;; p = p.parent_path(), depth = 1) {
        if (sameFolder(p, other))
            return depth;
        if (!p.has_relative_path())
            return -1;
    }
}

// The folders of the bundled tree `b`, `b` first, links not followed; at
// most kMaxBundledFolders (a bundled library holds a few dozen).
constexpr std::size_t kMaxBundledFolders = 4096;
std::vector<fs::path> bundledFolders(const fs::path& b) {
    std::vector<fs::path> out{b};
    std::error_code ec;
    fs::recursive_directory_iterator it(b, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator() && out.size() < kMaxBundledFolders; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_symlink(e2) && it->is_directory(e2))
            out.push_back(it->path());
    }
    return out;
}

// How `folder` or one of its ancestors is a folder of the bundled tree, by
// file identity: "is" for `folder` itself as the root, "is inside" for any
// other match, nullptr for none.
const char* insideByIdentity(const fs::path& folder, const std::vector<fs::path>& folders) {
    bool self = true;
    for (fs::path p = folder;; p = p.parent_path(), self = false) {
        for (std::size_t i = 0; i < folders.size(); ++i)
            if (sameFolder(p, folders[i]))
                return self && i == 0 ? "is" : "is inside";
        if (!p.has_relative_path())
            return nullptr;
    }
}

} // namespace

void refuseBundled(const fs::path& library) {
    const fs::path bundled = bundledLibraryDir();
    if (bundled.empty())
        return;
    const fs::path lib = canonicalish(library), b = canonicalish(bundled);
    // The paths first (they cover a library or bundled folder that doesn't
    // exist yet), then file identity against every folder of the bundled
    // tree, for a second path to it or to a folder in it (a bind mount).
    const char* how = lib == b ? "is" : within(lib, b) ? "is inside" : within(b, lib) ? "holds" : nullptr;
    if (!how) {
        how = insideByIdentity(lib, bundledFolders(b));
        if (!how && sameFolderUp(b.parent_path(), lib) >= 0)
            how = "holds";
    }
    if (how)
        throw LibraryError(library.string() + " " + how + " the bundled library (" + bundled.string() +
                           "); bundled stamps are never changed");
}

// Refuses a write in `folder`: the bundled library or a folder inside it, by
// path or by file identity (a bind mount or a second export of one). A folder
// that only holds the bundled library is not refused here; a write in it
// leaves the bundled stamps alone.
void refuseInsideBundled(const fs::path& folder) {
    const fs::path bundled = bundledLibraryDir();
    if (bundled.empty())
        return;
    const fs::path f = canonicalish(folder), b = canonicalish(bundled);
    const char* how = f == b ? "is" : within(f, b) ? "is inside" : insideByIdentity(f, bundledFolders(b));
    if (how)
        throw LibraryError(folder.string() + " " + how + " the bundled library (" + bundled.string() +
                           "); bundled stamps are never changed");
}

static void refuseBundledBelow(const fs::path& folder) {
    const fs::path bundled = bundledLibraryDir();
    if (bundled.empty())
        return;
    const std::vector<fs::path> folders = bundledFolders(canonicalish(bundled));
    const fs::path above = canonicalish(bundled).parent_path();
    std::error_code ec;
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_symlink(e2) || !it->is_directory(e2))
            continue;
        const fs::path& d = it->path();
        bool hit = sameFolderUp(above, d) >= 0;  // a bind of the bundled folder's parent or an ancestor
        for (std::size_t i = 0; !hit && i < folders.size(); ++i)
            hit = sameFolder(d, folders[i]);
        if (hit)
            throw LibraryError(d.string() + " is inside the bundled library (" + bundled.string() +
                               ") or holds it; bundled stamps are never changed");
    }
}

namespace {

QString qpath(const fs::path& p) {
#ifdef _WIN32
    return QString::fromStdWString(p.native());
#else
    return QFile::decodeName(QByteArray(p.native().data(), qsizetype(p.native().size())));
#endif
}

bool realDir(const fs::path& p) {
    std::error_code ec;
    const fs::file_status st = linkStatus(p, ec);
    return !ec && fs::is_directory(st) && !fs::is_symlink(st);
}

// The marker's name, if it holds one a collection can have.
std::optional<std::string> markerName(const fs::path& dir) {
    const fs::path m = dir / kCollectionMarker;
    if (!regular(m))
        return std::nullopt;
    try {
        std::string s = readRegularFile(m, 4096);
        if (s.ends_with('\n'))
            s.pop_back();
        if (s.ends_with('\r'))
            s.pop_back();
        checkCollectionName(s);
        return s;
    } catch (const LibraryError&) {
        return std::nullopt;
    }
}

// A folder that can't be read is still listed, with no stamps, so one bad
// folder doesn't hide the others; scan() reports it as a problem.
Collection collectionAt(const fs::path& dir) {
    ScanResult r;
    try {
        r = scan(dir);
    } catch (const LibraryError&) {
    }
    Collection c{{}, dir, r.stamps.size()};
    if (const std::optional<std::string> m = markerName(dir)) {
        c.name = *m;
    } else {
        std::set<std::string> cats;
        for (const Stamp& s : r.stamps)
            cats.insert(s.meta.category);
        c.name = cats.size() == 1 ? *cats.begin() : folderName(dir.filename().string());
    }
    return c;
}

void writeMarker(const fs::path& dir, const std::string& name) {
    refuseInsideBundled(dir);
    const fs::path m = dir / kCollectionMarker;
    checkWritable(m);
    Staged(m, name + "\n").commit();
}

// The stamp titled `name` in `dir` (in its subfolders too when `deep`),
// other than the file `except`, if any.
std::optional<fs::path> titledIn(const fs::path& dir, const std::string& name, bool deep, const fs::path& except) {
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return std::nullopt;
    for (const Stamp& s : scan(dir).stamps) {
        if (s.name != name || (!deep && s.path.parent_path() != dir))
            continue;
        if (!except.empty() && fs::equivalent(s.path, except, ec))
            continue;
        return s.path;
    }
    return std::nullopt;
}

Collection requireCollection(const fs::path& library, const std::string& name) {
    if (std::optional<Collection> c = findCollection(library, name))
        return *c;
    throw LibraryError("no collection named " + q(name) + " in " + library.string());
}

// `p` with its folder's links resolved (not a link `p` itself is), so a
// path into the library through another spelling is still in it.
fs::path resolvedParent(const fs::path& p) {
    std::error_code ec;
    const fs::path parent = p.has_parent_path() ? p.parent_path() : fs::path(".");
    fs::path r = fs::weakly_canonical(parent, ec);
    if (ec)
        r = fs::absolute(parent, ec).lexically_normal();
    return r / p.filename();
}

// `p` relative to `library` when it is inside it, else nothing.
std::optional<fs::path> insideLibrary(const fs::path& library, const fs::path& p) {
    std::error_code ec;
    fs::path base = fs::weakly_canonical(library, ec);
    if (ec)
        base = fs::absolute(library, ec).lexically_normal();
    const fs::path rel = resolvedParent(p).lexically_relative(base);
    if (rel.empty() || rel == "." || *rel.begin() == "..")
        return std::nullopt;
    return rel;
}

fs::path requireInside(const fs::path& library, const fs::path& p, const char* what) {
    if (std::optional<fs::path> rel = insideLibrary(library, p))
        return *rel;
    throw LibraryError(p.string() + " isn't in the library " + library.string() + "; only the stamps there can be " +
                       what);
}

// Refuses `p` when a permission would stop deleting it: the folder holding
// it, and every folder in it, must be listable and writable. POSIX only.
void checkDeletable(const fs::path& p) {
#if BW_LIBRARY_POSIX
    const auto check = [](const fs::path& dir, int mode) {
        if (::access(dir.c_str(), mode) != 0)
            throw LibraryError(dir.string() + " can't be emptied: " + std::strerror(errno) + "; nothing was removed");
    };
    check(p.has_parent_path() ? p.parent_path() : fs::path("."), W_OK | X_OK);
    if (!realDir(p))
        return;
    check(p, R_OK | W_OK | X_OK);
    std::error_code ec;
    fs::recursive_directory_iterator it(p, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec))
        if (realDir(it->path()))
            check(it->path(), R_OK | W_OK | X_OK);
    if (ec)
        throw LibraryError(p.string() + " can't be listed: " + ec.message() + "; nothing was removed");
#else
    (void)p;
#endif
}

// `p` as the trash records it: the .trashinfo's Path=, where a restore puts
// it back. The freedesktop trash spec takes a relative Path= as relative to
// the folder the trash sits in ($XDG_DATA_HOME for the home trash), so a
// relative `p` (`cli library remove lib/x.ans`) would restore to the wrong
// place: `p` is made absolute. Its folder is resolved on disk, not
// lexically: `..` after a link is taken where the link leads, as the library
// code takes it, and Qt cuts a top directory's trash Path= by the length of
// the mount root, which lands right only on a resolved path. Its own name is
// kept, so a link is recorded as the link.
fs::path trashPath(const fs::path& p) {
    fs::path q = p;
    if (q.filename().empty())
        q = q.parent_path();
    std::error_code ec;
    const fs::path dir = fs::canonical(q.has_parent_path() ? q.parent_path() : fs::path("."), ec);
    return ec ? fs::absolute(q, ec) : dir / q.filename();
}

// Moves `p` (a file, or a folder and everything in it) to the trash in one
// call, or deletes it. Whether it went to the trash. A trash that refuses
// throws TrashUnavailable, having removed nothing: never a delete instead.
// Both first check that `p` could be deleted (checkDeletable): a trash that
// can't rename it may copy it and then remove it (Qt does, across volumes),
// and that removal must not stop halfway.
bool discard(const fs::path& p, Removal how) {
    refuseInsideBundled(p.has_parent_path() ? p.parent_path() : fs::path("."));
    if (realDir(p)) {
        refuseBundled(p);
        refuseBundledBelow(p);
    }
    checkDeletable(p);
    if (how == Removal::Trash) {
        const fs::path where = trashPath(p);
        if (!(testing::moveToTrash ? testing::moveToTrash(where) : QFile::moveToTrash(qpath(where))))
            throw TrashUnavailable(p.string() + " can't be moved to the trash (there is none, or it refused); " +
                                   "nothing was removed");
        // Qt reports success when it copied `p` to the trash but couldn't
        // remove it.
        std::error_code ec;
        if (fs::exists(linkStatus(p, ec)))
            throw TrashUnavailable(p.string() + " was copied to the trash but can't be removed from here; " +
                                   "nothing was removed");
        return true;
    }
    std::error_code ec;
    if (realDir(p))
        fs::remove_all(p, ec);
    else
        fs::remove(p, ec);
    if (ec)
        throw LibraryError(p.string() + " can't be removed: " + ec.message());
    return false;
}

// Renames the folder `from` to `to` only if nothing sits on `to`.
void renameDirNoReplace(const fs::path& from, const fs::path& to) {
    refuseBundled(from);
    refuseBundledBelow(from);
    refuseInsideBundled(to.has_parent_path() ? to.parent_path() : fs::path("."));
#if BW_LIBRARY_POSIX && defined(__linux__) && defined(RENAME_NOREPLACE)
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
        return;
    if (errno == EEXIST)
        throw LibraryError(to.string() + " already exists");
    if (errno != EINVAL && errno != ENOSYS)
        throw LibraryError(from.string() + " can't be renamed: " + std::strerror(errno));
#elif BW_LIBRARY_POSIX && defined(__APPLE__)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0)
        return;
    if (errno == EEXIST)
        throw LibraryError(to.string() + " already exists");
    if (errno != ENOTSUP && errno != EINVAL)
        throw LibraryError(from.string() + " can't be renamed: " + std::strerror(errno));
#endif
    // No exclusive rename here: checked, then renamed.
    if (present(to))
        throw LibraryError(to.string() + " already exists");
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec)
        throw LibraryError(from.string() + " can't be renamed: " + ec.message());
}

// The art `bytes` with its SAUCE title made `name`: the text before the
// record kept as it is, the record (comments and all) rewritten. It must
// read back with the new title and the same widest row.
std::string retitled(const std::string& bytes, const std::string& name, TextEncoding encoding,
                     std::size_t ircBytes) {
    const SauceSplit split = splitSauce(bytes);
    if (!split.sauce)
        throw LibraryError(".ans has no SAUCE record (the title is the stamp's name)");
    Sauce s = *split.sauce;
    s.title = name;
    std::string out(split.content);
    out += encodeSauce(s, std::uint32_t(split.content.size()));
    try {
        const AnsiDocument back = parseAnsi(out, stampParse(encoding));
        if (!back.sauce || back.sauce->title != name || widestRowBytes(back.canvas) != ircBytes)
            throw LibraryError("the stamp doesn't read back the same with its new title");
    } catch (const AnsiError& e) {
        throw LibraryError(std::string("the retitled .ans does not parse: ") + e.what());
    } catch (const irc::IrcError& e) {
        throw LibraryError(std::string("the IRC encoder failed: ") + e.what());
    }
    return out;
}

// An imported piece's name: its SAUCE title when that is a stamp's name,
// else its file name in CP437 (what CP437 has no glyph for dropped), cut to
// SAUCE's 35 bytes.
std::string importName(const AnsiDocument& doc, const fs::path& ans) {
    if (doc.sauce) {
        try {
            checkName(doc.sauce->title);
            return doc.sauce->title;
        } catch (const LibraryError&) {
        }
    }
    const std::string stem = ans.stem().string();
    std::string name;
    for (std::size_t i = 0; i < stem.size();) {
        const std::optional<char32_t> cp = nextUtf8(stem, i);
        if (!cp || *cp == 0)
            continue;
        if (const std::optional<std::uint8_t> b = unicodeToCp437(*cp); b && *b)
            name += char(*b);
    }
    const auto trim = [&] {
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
    };
    trim();
    if (name.size() > Sauce::kTitleLen)
        name.resize(Sauce::kTitleLen);
    trim();
    return name.empty() ? "stamp" : name;
}

constexpr std::size_t kMaxImportBytes = std::size_t(32) << 20;

} // namespace

std::string collectionSlug(std::string_view name) {
    std::vector<char32_t> cps;
    for (std::size_t i = 0; i < name.size();)
        if (const std::optional<char32_t> cp = nextUtf8(name, i))
            cps.push_back(*cp);
    std::string s = slugOf(cps);
    // Windows's device names, which no folder there may have.
    static const std::set<std::string> kReserved = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                                    "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                                    "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    if (kReserved.count(s))
        s += '-';
    return s;
}

std::vector<Collection> collections(const fs::path& library) {
    std::vector<Collection> out;
    std::error_code ec;
    fs::directory_iterator it(library, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return out;
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec))
        if (realDir(it->path()))
            out.push_back(collectionAt(it->path()));
    if (ec)
        throw LibraryError(library.string() + " can't be listed: " + ec.message());
    std::sort(out.begin(), out.end(), [](const Collection& a, const Collection& b) {
        return a.name != b.name ? a.name < b.name : a.dir < b.dir;
    });
    return out;
}

std::optional<Collection> findCollection(const fs::path& library, const std::string& name) {
    std::vector<Collection> found;
    for (Collection& c : collections(library))
        if (c.name == name)
            found.push_back(std::move(c));
    if (found.empty())
        return std::nullopt;
    if (found.size() > 1) {
        std::string dirs;
        for (const Collection& c : found)
            dirs += (dirs.empty() ? "" : ", ") + c.dir.string();
        throw LibraryError("more than one folder goes by the collection name " + q(name) + ": " + dirs +
                           "; give one of them another name (in its " + kCollectionMarker + " file) first");
    }
    return std::move(found.front());
}

std::optional<Collection> collectionOf(const fs::path& library, const fs::path& path) {
    const std::optional<fs::path> rel = insideLibrary(library, path);
    if (!rel || std::distance(rel->begin(), rel->end()) < 2)
        return std::nullopt;
    const fs::path dir = library / *rel->begin();
    if (!realDir(dir))
        return std::nullopt;
    return collectionAt(dir);
}

Collection createCollection(const fs::path& library, const std::string& name) {
    checkCollectionName(name);
    refuseBundled(library);
    std::error_code ec;
    fs::create_directories(library, ec);
    if (ec)
        throw LibraryError(library.string() + ": " + ec.message());
    if (findCollection(library, name))
        throw LibraryError("a collection named " + q(name) + " already exists");
    const fs::path dir = library / collectionSlug(name);
    if (present(dir))
        throw LibraryError(dir.string() + " already exists" +
                           (realDir(dir) ? " as the collection " + q(collectionAt(dir).name) : std::string()) +
                           "; the collection " + q(name) + " would need that folder");
    if (!fs::create_directory(dir, ec) || ec)
        throw LibraryError(dir.string() + (ec ? ": " + ec.message() : std::string(" already exists")));
    try {
        writeMarker(dir, name);
    } catch (...) {
        fs::remove_all(dir, ec);
        throw;
    }
    return {name, dir, 0};
}

Removed deleteCollection(const fs::path& library, const std::string& name, Removal how) {
    refuseBundled(library);
    const Collection c = requireCollection(library, name);
    refuseBundled(c.dir);
    refuseBundledBelow(c.dir);
    if (!realDir(c.dir))
        throw LibraryError(c.dir.string() + " is no longer a folder");
    return {c.stamps, discard(c.dir, how)};
}

Collection renameCollection(const fs::path& library, const std::string& from, const std::string& to) {
    checkCollectionName(to);
    refuseBundled(library);
    const Collection c = requireCollection(library, from);
    refuseBundled(c.dir);
    refuseBundledBelow(c.dir);
    if (from == to)
        return c;
    if (findCollection(library, to))
        throw LibraryError("a collection named " + q(to) + " already exists");
    const fs::path dir = library / collectionSlug(to);
    const bool move = dir.filename() != c.dir.filename();
    if (move && present(dir))
        throw LibraryError(dir.string() + " already exists; the collection " + q(to) + " would need that folder");
    // Every new sidecar is made before anything moves, so a stamp that can't
    // take the new category refuses the rename.
    const ScanResult r = scan(c.dir);
    std::vector<std::pair<fs::path, std::string>> sidecars;  // relative to the folder
    for (const Stamp& s : r.stamps) {
        StampMeta meta = s.meta;
        meta.category = to;
        const fs::path side = pairedSidecar(s.path);
        checkWritable(side);
        sidecars.emplace_back(side.lexically_relative(c.dir), sidecarJson(meta));
    }
    if (move)
        renameDirNoReplace(c.dir, dir);
    std::string failed;
    std::size_t failures = 0;
    for (const auto& [rel, json] : sidecars) {
        try {
            checkWritable(dir / rel);
            Staged(dir / rel, json).commit();
        } catch (const LibraryError& e) {
            failed += std::string(failures++ ? "; " : "") + e.what();
        }
    }
    // The marker last: until it is written the name is the old one, and
    // the folder's stamps are what a collection with no marker goes by.
    try {
        writeMarker(dir, to);
    } catch (const LibraryError& e) {
        failed += std::string(failures ? "; " : "") + "the marker: " + e.what();
        ++failures;
    }
    if (failures)
        throw LibraryError("the collection " + q(from) + " was moved to " + dir.string() +
                           ", but not everything in it could be renamed " + q(to) + ": " + failed);
    return {to, dir, r.stamps.size()};
}

std::filesystem::path addStamp(const fs::path& library, const std::string& collection, const std::string& name,
                               const std::vector<std::string>& tags, const CellBlock& cells, bool ice,
                               const std::optional<StampSlice>& slice, bool replace) {
    refuseBundled(library);
    const Collection c = requireCollection(library, collection);
    refuseBundled(c.dir);
    const StampFiles files = encodeStamp(name, c.name, tags, cells, ice, slice);
    // The title anywhere in the collection, a stamp renamed in place included.
    const std::optional<fs::path> held = titledIn(c.dir, name, true, {});
    const StampTarget t = stampPathFor(c.dir, name);
    if ((held || t.exists) && !replace)
        throw LibraryError("the collection " + q(c.name) + " already holds a stamp named " +
                           q(cp437ToUtf8(name)) + " (" + (held ? *held : t.path).string() + ")");
    const fs::path dest = held ? *held : t.path;
    writeStamp(dest, files);
    return dest;
}

namespace {

// What addStampFile writes into collection `c` for `ans`: its title and files.
std::pair<std::string, StampFiles> stampFromFile(const Collection& c, const fs::path& ans) {
    std::string name;
    StampFiles files;
    if (present(pairedSidecar(ans))) {
        // A stamp: copied as it is, into the collection's category.
        const Stamp s = readStamp(ans);
        StampMeta meta = s.meta;
        meta.category = c.name;
        try {
            files = {readAll(ans), sidecarJson(meta)};
        } catch (const LibraryError& e) {
            throw LibraryError(std::string(".ans ") + e.what());
        }
        name = s.name;
    } else {
        std::string data;
        try {
            data = readRegularFile(ans, kMaxImportBytes);
        } catch (const LibraryError& e) {
            throw LibraryError(std::string(".ans ") + e.what());
        }
        AnsiDocument doc;
        try {
            doc = parseAnsi(data);
        } catch (const AnsiError& e) {
            throw LibraryError(std::string(".ans does not parse: ") + e.what());
        }
        name = importName(doc, ans);
        // The canvas is 80 wide however little is drawn on it: the blank
        // columns to the right and rows below go.
        int w = 0, h = 0;
        for (int y = 0; y < doc.canvas.height(); ++y)
            for (int x = 0; x < doc.canvas.width(); ++x)
                if (!isBlank(doc.canvas.at(x, y))) {
                    w = std::max(w, x + 1);
                    h = y + 1;
                }
        if (!w)
            throw LibraryError(".ans has nothing drawn in it");
        CellBlock block(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                block.at(x, y) = doc.canvas.at(x, y);
        files = encodeStamp(name, c.name, {}, block, doc.ice);
    }
    return {name, files};
}

// addStampFile's adds into one collection, which is found and has its
// titles read once: each stamp added puts its title in too, so a batch
// refuses a title it added itself, as a fresh scan would.
class Adder {
public:
    Adder(const fs::path& library, const std::string& collection) {
        refuseBundled(library);
        c_ = requireCollection(library, collection);
        refuseBundled(c_.dir);
        // titledIn's scan, kept: the first stamp holding each title
        for (Stamp& s : scan(c_.dir).stamps)
            titles_.emplace(std::move(s.name), std::move(s.path));
    }
    Added add(const fs::path& ans) {
        if (!isArtName(ans))
            throw LibraryError("not an .ans file");
        auto [name, files] = stampFromFile(c_, ans);
        const auto held = titles_.find(name);
        const StampTarget t = stampPathFor(c_.dir, name);
        if (held != titles_.end() || t.exists)
            throw LibraryError("the collection " + q(c_.name) + " already holds a stamp named " +
                               q(cp437ToUtf8(name)) + " (" +
                               (held != titles_.end() ? held->second : t.path).string() + ")");
        writeStamp(t.path, files);
        titles_.emplace(name, t.path);
        return {t.path, name};
    }

private:
    Collection c_;
    std::map<std::string, fs::path> titles_;
};

} // namespace

Added addStampFile(const fs::path& library, const std::string& collection, const fs::path& ans) {
    if (!isArtName(ans))
        throw LibraryError("not an .ans file");
    return Adder(library, collection).add(ans);
}

std::vector<fs::path> folderArt(const fs::path& dir) {
    std::vector<fs::path> found;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        std::error_code dirEc;
        // a dot-prefixed file is hidden, and on a Mac an AppleDouble ._x.ans
        // sits beside x.ans: neither is art
        if (it->path().filename().string().rfind('.', 0) != 0 && isArtName(it->path()) &&
            !fs::is_directory(it->path(), dirEc))
            found.push_back(it->path());
    }
    if (ec)
        throw LibraryError("can't be listed: " + ec.message());
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<AddOutcome> addStamps(const fs::path& library, const std::string& collection,
                                  const std::vector<fs::path>& files, const std::function<bool(std::size_t)>& next) {
    Adder adder(library, collection);
    std::vector<AddOutcome> out;
    out.reserve(files.size());
    for (const fs::path& f : files) {
        if (next && !next(out.size()))
            break;
        AddOutcome o{f, std::nullopt, {}};
        try {
            o.added = adder.add(f);
        } catch (const LibraryError& e) {
            o.error = e.what();
        }
        out.push_back(std::move(o));
    }
    return out;
}

void requireRemovable(const fs::path& library, const fs::path& ans) {
    if (!isArtName(ans))
        throw LibraryError(ans.string() + " isn't a stamp's .ans");
    refuseBundled(library);
    requireInside(library, ans, "removed");
    refuseBundled(ans.has_parent_path() ? ans.parent_path() : fs::path("."));
    std::error_code ec;
    const fs::file_status st = linkStatus(ans, ec);
    if (!fs::exists(st))
        throw LibraryError(ans.string() + " doesn't exist");
    if (fs::is_directory(st))
        throw LibraryError(ans.string() + " is a folder, not a stamp");
    // The art and its sidecar share the folder, so this covers both.
    checkDeletable(ans);
}

Removed removeStamp(const fs::path& library, const fs::path& ans, Removal how) {
    requireRemovable(library, ans);
    const fs::path side = pairedSidecar(ans);
    if (!present(side) || realDir(side))
        return {1, discard(ans, how)};
    // The sidecar goes first, kept here (its bytes, or where a link to it
    // points): when the art then can't go, the sidecar is put back, so a
    // refusal leaves the stamp whole. An art file left without its sidecar
    // is still a stamp remove takes.
    std::error_code ec;
    std::optional<std::string> kept;
    std::optional<fs::path> link;
    std::optional<fs::perms> mode;
    std::optional<fs::file_time_type> mtime;
    const fs::file_status sideStatus = fs::symlink_status(side, ec);
    if (fs::is_symlink(sideStatus)) {
        if (fs::path to = fs::read_symlink(side, ec); !ec)
            link = to;
    } else {
        try {
            kept = readAll(side);
        } catch (const LibraryError& e) {
            // The trash would take a sidecar that can't come back, and an
            // art file the trash then refuses would be left without it.
            if (how == Removal::Trash)
                throw TrashUnavailable(side.string() + " " + e.what() +
                                       ", so it couldn't be put back if the art can't go to the trash; " +
                                       "nothing was removed");
        }
        // Put back as it was: its permission bits and modification time.
        if (!ec)
            mode = sideStatus.permissions() & fs::perms::all;
        if (const fs::file_time_type t = fs::last_write_time(side, ec); !ec)
            mtime = t;
    }
    discard(side, how);
    try {
        return {1, discard(ans, how)};
    } catch (const LibraryError& e) {
        std::string why = e.what();
        const std::string gone = how == Removal::Trash ? "moved to the trash" : "deleted";
        try {
            if (link) {
                fs::create_symlink(*link, side, ec);
                if (ec)
                    throw LibraryError(ec.message());
            } else if (kept) {
                Staged(side, *kept, mode).commitNoReplace();
                if (mtime)
                    fs::last_write_time(side, *mtime, ec);
            } else {
                throw LibraryError("it couldn't be read beforehand");
            }
        } catch (const LibraryError& r) {
            const std::string nothing = "; nothing was removed";
            if (why.size() >= nothing.size() && why.compare(why.size() - nothing.size(), nothing.size(), nothing) == 0)
                why.erase(why.size() - nothing.size());
            throw LibraryError(why + "; its sidecar " + side.string() + ", already " + gone +
                               ", couldn't be put back (" + r.what() + "), so only the art is still here");
        }
        // The trash's copy stays: Qt can't take an entry back out of it.
        why += " (its sidecar, already " + gone + ", was put back" +
               (how == Removal::Trash ? "; the trash keeps a copy of it" : "") + ")";
        if (dynamic_cast<const TrashUnavailable*>(&e))
            throw TrashUnavailable(why);
        throw LibraryError(why);
    }
}

std::filesystem::path editStamp(const fs::path& library, const fs::path& ans, const StampEdit& edit) {
    refuseBundled(library);
    requireInside(library, ans, "changed");
    refuseBundled(ans.has_parent_path() ? ans.parent_path() : fs::path("."));
    const Stamp s = readStamp(ans);
    const std::string name = edit.name.value_or(s.name);
    checkName(name);
    StampMeta meta = s.meta;
    if (edit.category) {
        checkText(*edit.category, "the category");
        meta.category = *edit.category;
    }
    if (edit.tags) {
        for (const std::string& t : *edit.tags)
            checkTag(t, "a tag");
        meta.tags = *edit.tags;
    }
    std::string art;
    try {
        art = readAll(ans);
    } catch (const LibraryError& e) {
        throw LibraryError(std::string(".ans ") + e.what());
    }
    if (name != s.name)
        art = retitled(art, name, meta.encoding, meta.ircBytes);
    const StampFiles files{art, sidecarJson(meta)};

    const fs::path here = ans.has_parent_path() ? ans.parent_path() : fs::path(".");
    fs::path dest = here;
    if (meta.category != s.meta.category) {
        const std::optional<Collection> from = collectionOf(library, ans);
        if (const std::optional<Collection> to = findCollection(library, meta.category)) {
            if (!from || from->dir != to->dir)
                dest = to->dir;
        } else if (from) {
            dest = library;
        }
    }
    refuseBundled(dest);
    std::error_code ec;
    const bool moving = !fs::equivalent(dest, here, ec);
    const StampTarget t = stampPathFor(dest, name);
    // Another stamp with the title: anywhere in the destination collection
    // (one renamed in place keeps its old file name), or among the stamps
    // at the top of the library.
    const std::optional<Collection> into = collectionOf(library, dest / "x");
    const std::optional<fs::path> held = into ? titledIn(into->dir, name, true, ans) : titledIn(dest, name, false, ans);
    const auto clash = [&] {
        return LibraryError(dest.string() + " already holds a stamp named " + q(cp437ToUtf8(name)) + " (" +
                            (held ? *held : t.path).string() + ")");
    };
    // A stamp that keeps its title and folder makes no new clash.
    if (held && (moving || name != s.name))
        throw clash();
    if (!moving) {
        if (t.exists && !fs::equivalent(t.path, ans, ec))
            throw clash();
        writeStamp(ans, files);
        return ans;
    }
    if (t.exists)
        throw clash();
    const fs::path oldSide = pairedSidecar(ans);
    writeStamp(t.path, files);
    std::string why;
    for (const fs::path& p : {ans, oldSide})
        if (present(p) && !fs::remove(p, ec) && ec)
            why += (why.empty() ? "" : "; ") + p.string() + ": " + ec.message();
    if (!why.empty())
        throw LibraryError("the stamp was written to " + t.path.string() + ", but its old files can't be removed: " +
                           why);
    return t.path;
}

fs::path realBinDir(const fs::path& exe, const fs::path& fallback) {
    // libc++ resolves an empty path to the working directory; it names no
    // executable
    if (exe.empty())
        return fallback;
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

fs::path bundledLibraryDir() {
    if (fs::path p = envPath("BLOCKWRIGHT_BUNDLED_LIBRARY"); !p.empty())
        return p;
    if (!QCoreApplication::instance())
        return {};
    return bundledLibraryDir(realBinDir(fspath(QCoreApplication::applicationFilePath()),
                                        fspath(QCoreApplication::applicationDirPath())));
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
