#include "blockwright/pipeline.h"

#include "blockwright/ansi.h"
#include "blockwright/library.h"
#include "blockwright/links.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <unistd.h>
#include <utility>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#endif
#include <variant>

#include <QString>

namespace bw::pipeline {

namespace fs = std::filesystem;

namespace {

bool lowerOrDigit(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

char asciiLower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = asciiLower(c);
    return out;
}

// One code point from valid UTF-8 at s[i], moving i past it; nullopt for
// a malformed, overlong or surrogate sequence.
std::optional<char32_t> nextUtf8(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
    if (!len || i + len > s.size())
        return std::nullopt;
    char32_t cp = len == 1 ? b : len == 2 ? b & 0x1F : len == 3 ? b & 0x0F : b & 0x07;
    for (int k = 1; k < len; ++k) {
        const auto c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80)
            return std::nullopt;
        cp = (cp << 6) | (c & 0x3F);
    }
    static constexpr char32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return std::nullopt;
    i += len;
    return cp;
}

// A tag lowercased by Unicode's case mapping, as Python's str.lower() does
// (not only ASCII), but for Python's word-final sigma rule, which QString
// doesn't apply. Throws PipelineError for text that isn't UTF-8, which
// QString would otherwise replace without a word.
std::string lowerTag(std::string_view s) {
    for (std::size_t i = 0; i < s.size();)
        if (!nextUtf8(s, i))
            throw PipelineError("a tag is not UTF-8");
    return QString::fromUtf8(s.data(), qsizetype(s.size())).toLower().toStdString();
}

// A tag as the ascii script's tag command stores it, w.lstrip("#").lower():
// its leading '#'s dropped, so its "#tag" search finds it, then lowercased.
// Empty for a tag that was only '#'s.
std::string foldTag(std::string_view s) {
    while (!s.empty() && s.front() == '#')
        s.remove_prefix(1);
    return lowerTag(s);
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// A string as Python's json.dumps writes it with ensure_ascii.
void appendJsonString(std::string& out, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto u = [&](unsigned v) {
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4)
            out += kHex[(v >> shift) & 0xF];
    };
    out += '"';
    for (std::size_t i = 0; i < s.size();) {
        const std::optional<char32_t> cp = nextUtf8(s, i);
        if (!cp)
            throw PipelineError("a tag or key is not UTF-8");
        switch (*cp) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (*cp >= 0x20 && *cp <= 0x7E) {
                out += char(*cp);
            } else if (*cp < 0x10000) {
                u(*cp);
            } else {
                const char32_t v = *cp - 0x10000;
                u(0xD800 + (v >> 10));
                u(0xDC00 + (v & 0x3FF));
            }
        }
    }
    out += '"';
}

using Value = std::variant<std::vector<std::string>, std::int64_t>;

// The two index shapes: one object of string keys to a list of strings or
// an integer. Anything else, or a key twice, throws.
class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::map<std::string, Value> object() {
        std::map<std::string, Value> out;
        ws();
        expect('{');
        ws();
        if (peek() == '}') {
            ++i_;
        } else {
            for (;;) {
                ws();
                std::string key = string();
                ws();
                expect(':');
                ws();
                Value v = value();
                if (!out.emplace(std::move(key), std::move(v)).second)
                    fail("a key is there twice");
                ws();
                if (peek() == ',') {
                    ++i_;
                    continue;
                }
                expect('}');
                break;
            }
        }
        ws();
        if (i_ != s_.size())
            fail("something follows the object");
        return out;
    }

private:
    [[noreturn]] void fail(const std::string& why) const {
        throw PipelineError("not an index the ascii script writes (" + why + " at byte " + std::to_string(i_) + ")");
    }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t'))
            ++i_;
    }
    void expect(char c) {
        if (peek() != c || i_ >= s_.size())
            fail(std::string("expected '") + c + "'");
        ++i_;
    }
    unsigned hex4() {
        if (i_ + 4 > s_.size())
            fail("a short \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= unsigned(c - 'A' + 10);
            else
                fail("a bad \\u escape");
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            if (i_ >= s_.size())
                fail("an unterminated string");
            const auto c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                ++i_;
                return out;
            }
            if (c < 0x20)
                fail("a control character in a string");
            if (c >= 0x80) {
                const std::optional<char32_t> cp = nextUtf8(s_, i_);
                if (!cp)
                    fail("a string that isn't UTF-8");
                appendUtf8(out, *cp);
                continue;
            }
            ++i_;
            if (c != '\\') {
                out += char(c);
                continue;
            }
            const char e = peek();
            ++i_;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                char32_t cp = hex4();
                if (cp >= 0xDC00 && cp <= 0xDFFF)
                    fail("a lone surrogate");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (s_.substr(i_, 2) != "\\u")
                        fail("a lone surrogate");
                    i_ += 2;
                    const char32_t lo = hex4();
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        fail("a lone surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: fail("a bad escape");
            }
        }
    }
    Value value() {
        if (peek() == '[') {
            ++i_;
            std::vector<std::string> list;
            ws();
            if (peek() == ']') {
                ++i_;
                return list;
            }
            for (;;) {
                ws();
                if (peek() != '"')
                    fail("a list item that isn't a string");
                list.push_back(string());
                ws();
                if (peek() == ',') {
                    ++i_;
                    continue;
                }
                expect(']');
                return list;
            }
        }
        const bool negative = peek() == '-';
        if (negative)
            ++i_;
        if (peek() < '0' || peek() > '9')
            fail("a value that is neither a list nor an integer");
        if (peek() == '0' && i_ + 1 < s_.size() && s_[i_ + 1] >= '0' && s_[i_ + 1] <= '9')
            fail("a number with a leading zero");
        // The limit is checked before each digit is added, so nothing wraps.
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        std::int64_t v = 0;
        while (peek() >= '0' && peek() <= '9') {
            const int digit = s_[i_] - '0';
            if (v > (kMax - digit) / 10)
                fail("a number too large");
            v = v * 10 + digit;
            ++i_;
        }
        if (peek() == '.' || peek() == 'e' || peek() == 'E')
            fail("a number that isn't an integer");
        return negative ? -v : v;
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

template <class T> std::map<std::string, T> parseIndex(std::string_view json, const char* what) {
    std::map<std::string, T> out;
    for (auto& [k, v] : Parser(json).object()) {
        if (!std::holds_alternative<T>(v))
            throw PipelineError(std::string("not an index the ascii script writes (") + what + " has a value of " +
                                "another kind, under \"" + k + "\")");
        out.emplace(k, std::get<T>(std::move(v)));
    }
    return out;
}

template <class T, class Write> std::string indexJson(const std::map<std::string, T>& index, bool newline, Write write) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : index) {
        out += first ? "\n " : ",\n ";
        first = false;
        appendJsonString(out, k);
        out += ": ";
        write(out, v);
    }
    out += first ? "}" : "\n}";
    if (newline)
        out += '\n';
    return out;
}

// A file that is there and isn't a regular file (a link to one included)
// is never written over.
void checkTarget(const fs::path& p) {
    std::error_code ec;
    const fs::file_status st = linkStatus(p, ec);
    if (ec && st.type() != fs::file_type::not_found)
        throw PipelineError(p.string() + ": " + ec.message());
    if (st.type() != fs::file_type::not_found && st.type() != fs::file_type::regular)
        throw PipelineError(p.string() + ": is not a regular file, so it is not written over");
}

bool writeAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        bytes.remove_prefix(std::size_t(n));
    }
    return true;
}

// A temporary file beside `target` holding `bytes`, removed unless renamed.
// It is created exclusively, so a file or link already at a name is never
// written through: that name is skipped.
class Staged {
public:
    Staged(const fs::path& target, const std::string& bytes) : target_(target) {
        int fd = -1;
        for (int n = 0; fd < 0; ++n) {
            fs::path temp = target;
            temp += ".bw-" + std::to_string(::getpid()) + "-" + std::to_string(n) + ".tmp";
            do
#ifdef _WIN32
                // _O_EXCL is CREATE_NEW, which no link at the name is followed through
                fd = ::_wopen(temp.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                              _S_IREAD | _S_IWRITE);
#else
                fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
#endif
            while (fd < 0 && errno == EINTR);
            if (fd >= 0)
                temp_ = temp;
            else if (errno != EEXIST)
                throw PipelineError(target.string() + ": can't be written: " + std::strerror(errno));
            else if (n == 100)
                throw PipelineError(target.string() + ": no free temporary name");
        }
        const bool wrote = writeAll(fd, bytes);
        if (::close(fd) != 0 || !wrote) {
            discard();
            throw PipelineError(target.string() + ": write failed");
        }
    }
    Staged(const Staged&) = delete;
    Staged& operator=(const Staged&) = delete;
    ~Staged() { discard(); }

    void commit() {
        std::error_code ec;
        fs::rename(temp_, target_, ec);
        if (ec)
            throw PipelineError(target_.string() + ": " + ec.message());
        temp_.clear();
    }

private:
    void discard() {
        if (temp_.empty())
            return;
        std::error_code ec;
        fs::remove(temp_, ec);
        temp_.clear();
    }

    fs::path target_, temp_;
};

// A file's bytes, or none if it isn't there.
std::optional<std::string> readIfThere(const fs::path& p) {
    checkTarget(p);
    std::error_code ec;
    if (fs::symlink_status(p, ec).type() == fs::file_type::not_found)
        return std::nullopt;
    try {
        return library::readRegularFile(p);
    } catch (const library::LibraryError& e) {
        throw PipelineError(p.string() + ": " + e.what());
    }
}

// The library as a plan for `slug` sees it. The ascii script names a piece
// by its path without the extension, case folded, over .txt, .ans and .asc
// files that don't start with a dot.
LibraryState readState(const PublishPlan& plan, const std::string& slug) {
    LibraryState st;
    const fs::path dir = plan.ansPath.parent_path();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path name = it->path().filename();
        const std::string file = name.string();
        const std::string ext = asciiLower(name.extension().string());
        if (file.empty() || file.front() == '.' || (ext != ".txt" && ext != ".ans" && ext != ".asc"))
            continue;
        if (asciiLower(name.stem().string()) == slug)
            st.names.push_back(file);
    }
    if (ec)
        throw PipelineError(dir.string() + ": " + ec.message());
    std::sort(st.names.begin(), st.names.end());
    st.ans = readIfThere(plan.ansPath);
    st.txt = readIfThere(plan.txtPath);
    st.tags = readIfThere(plan.tagsPath);
    st.dates = readIfThere(plan.datesPath);
    return st;
}

// The first file that differs between two states of the library.
fs::path firstChange(const PublishPlan& plan, const LibraryState& was, const LibraryState& now) {
    if (was.ans != now.ans)
        return plan.ansPath;
    if (was.txt != now.txt)
        return plan.txtPath;
    if (was.tags != now.tags)
        return plan.tagsPath;
    if (was.dates != now.dates)
        return plan.datesPath;
    const fs::path dir = plan.ansPath.parent_path();
    for (const std::string& n : now.names)
        if (!std::binary_search(was.names.begin(), was.names.end(), n))
            return dir / n;
    for (const std::string& n : was.names)
        if (!std::binary_search(now.names.begin(), now.names.end(), n))
            return dir / n;
    return dir;
}

} // namespace

bool validSlug(std::string_view slug) {
    if (slug.empty() || slug.size() > 64 || !lowerOrDigit(slug.front()))
        return false;
    return std::all_of(slug.begin(), slug.end(), [](char c) { return lowerOrDigit(c) || c == '-' || c == '_'; });
}

std::string slugFrom(std::string_view name) {
    std::string out;
    for (const char raw : name) {
        const char c = asciiLower(raw);
        if (lowerOrDigit(c) || c == '-' || c == '_') {
            if (!out.empty() || lowerOrDigit(c))
                out += c;
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    if (out.size() > 64)
        out.resize(64);
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out;
}

std::vector<std::string> parseTags(std::string_view text) {
    std::set<std::string> tags;
    std::string cur;
    const auto flush = [&] {
        if (std::string t = foldTag(std::exchange(cur, {})); !t.empty())
            tags.insert(std::move(t));
    };
    for (const char c : text) {
        if (c == ',' || c == ' ') {
            flush();
        } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            throw PipelineError("a tag holds a control character");
        } else {
            cur += c;
        }
    }
    flush();
    return {tags.begin(), tags.end()};
}

TagIndex parseTagIndex(std::string_view json) { return parseIndex<std::vector<std::string>>(json, ".tags.json"); }

DateIndex parseDateIndex(std::string_view json) { return parseIndex<std::int64_t>(json, ".dates.json"); }

std::string tagIndexJson(const TagIndex& index, bool newline) {
    return indexJson(index, newline, [](std::string& out, const std::vector<std::string>& tags) {
        if (tags.empty()) {
            out += "[]";
            return;
        }
        out += "[";
        for (std::size_t i = 0; i < tags.size(); ++i) {
            out += i ? ",\n  " : "\n  ";
            appendJsonString(out, tags[i]);
        }
        out += "\n ]";
    });
}

std::string dateIndexJson(const DateIndex& index, bool newline) {
    return indexJson(index, newline, [](std::string& out, std::int64_t v) { out += std::to_string(v); });
}

// `ans` is the .ans Publish writes (writeAnsi), which is CP437.
std::vector<std::string> ircLines(std::string_view ans) {
    return irc::exportCanvas(parseAnsi(ans, {.encoding = TextEncoding::Cp437}).canvas);
}

PublishPlan planPublish(const PublishRequest& r) {
    if (!validSlug(r.slug))
        throw PipelineError("\"" + r.slug + "\" is not a piece name: use 1-64 of a-z, 0-9, '-' and '_', "
                            "starting with a letter or a digit");
    const fs::path dir = r.library / std::string(kArtist);
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        throw PipelineError(r.library.string() + ": has no " + std::string(kArtist) +
                            " folder, so it isn't the ascii library");

    PublishPlan plan;
    plan.key = std::string(kArtist) + "/" + r.slug;
    plan.ansPath = dir / (r.slug + ".ans");
    plan.txtPath = dir / (r.slug + ".txt");
    plan.tagsPath = r.library / ".tags.json";
    plan.datesPath = r.library / ".dates.json";

    plan.basis = readState(plan, r.slug);
    for (const std::string& file : plan.basis.names) {
        if (file == r.slug + ".ans" || file == r.slug + ".txt")
            plan.replaces = true;
        else
            throw PipelineError((dir / file).string() + " is there already, and the ascii script would take it "
                                "for the same piece; rename or remove it first");
    }

    std::vector<std::string> lines;
    try {
        lines = ircLines(r.ans);
    } catch (const std::exception& e) {
        throw PipelineError(std::string("the piece can't be read back as IRC text: ") + e.what());
    }
    const auto over = irc::overBudget(lines, r.budget);
    if (!over.empty()) {
        std::string why = "over the " + std::to_string(r.budget) + "-byte IRC budget, so it would be cut on IRC:";
        for (std::size_t i = 0; i < over.size(); ++i)
            why += (i ? ", line " : " line ") + std::to_string(over[i].line) + " is " +
                   std::to_string(over[i].bytes) + " bytes";
        throw PipelineError(why);
    }
    plan.ans = r.ans;
    plan.txt = irc::joinLines(lines);

    const std::optional<std::string>& tagsFile = plan.basis.tags;
    const std::optional<std::string>& datesFile = plan.basis.dates;
    TagIndex tags;
    DateIndex dates;
    try {
        if (tagsFile)
            tags = parseTagIndex(*tagsFile);
    } catch (const PipelineError& e) {
        throw PipelineError(plan.tagsPath.string() + ": " + e.what());
    }
    try {
        if (datesFile)
            dates = parseDateIndex(*datesFile);
    } catch (const PipelineError& e) {
        throw PipelineError(plan.datesPath.string() + ": " + e.what());
    }
    // An index is written back whole, so one laid out any other way (CRLF,
    // another indent, keys out of order) would be rewritten throughout, not
    // only at this piece's entry. It is refused instead.
    const auto endsInNewline = [](const std::optional<std::string>& f) {
        return f && !f->empty() && f->back() == '\n';
    };
    const auto checkLayout = [](const fs::path& path, const std::optional<std::string>& was,
                                const std::string& rewritten) {
        if (was && *was != rewritten)
            throw PipelineError(path.string() + " is not laid out as the ascii script writes it (json.dump with "
                                "indent=1, keys sorted), so publishing would rewrite all of it; it was left alone");
    };
    checkLayout(plan.tagsPath, tagsFile, tagIndexJson(tags, endsInNewline(tagsFile)));
    checkLayout(plan.datesPath, datesFile, dateIndexJson(dates, endsInNewline(datesFile)));

    // The piece's tags already there are folded the same way, so a tag
    // typed again in another case isn't kept twice.
    std::set<std::string> mine(r.tags.begin(), r.tags.end());
    if (const auto it = tags.find(plan.key); it != tags.end())
        for (const std::string& t : it->second)
            if (std::string f = foldTag(t); !f.empty())
                mine.insert(std::move(f));
    mine.insert(std::string(kArtist));
    plan.tags.assign(mine.begin(), mine.end());
    tags[plan.key] = plan.tags;
    plan.dated = dates.emplace(plan.key, r.now).second;

    plan.tagsJson = tagIndexJson(tags, endsInNewline(tagsFile));
    plan.datesJson = dateIndexJson(dates, endsInNewline(datesFile));
    return plan;
}

void publish(const PublishPlan& plan) {
    const std::pair<const fs::path*, const std::string*> files[] = {
        {&plan.ansPath, &plan.ans},
        {&plan.txtPath, &plan.txt},
        {&plan.tagsPath, &plan.tagsJson},
        {&plan.datesPath, &plan.datesJson},
    };
    // The user may take any time to confirm; a library that has moved on
    // since (another publish, an edit, a new clashing name) is not written
    // over with the plan's old view of it. Reading the state also refuses a
    // target that isn't a regular file.
    const LibraryState now = readState(plan, plan.ansPath.stem().string());
    if (now != plan.basis)
        throw PipelineError(firstChange(plan, plan.basis, now).string() +
                            " changed after the publish was worked out, so nothing was written; "
                            "publish again to include the change");
    // Every file is written beside its target first, so a full disk leaves
    // the library as it was; then they are renamed into place.
    std::vector<std::unique_ptr<Staged>> staged;
    for (const auto& [path, bytes] : files)
        staged.push_back(std::make_unique<Staged>(*path, *bytes));
    std::string written;
    for (std::size_t i = 0; i < staged.size(); ++i) {
        try {
            staged[i]->commit();
        } catch (const PipelineError& e) {
            throw PipelineError(std::string(e.what()) +
                                (written.empty() ? "; nothing was written" : "; written before it:" + written));
        }
        written += " " + files[i].first->filename().string();
    }
}

bool validHost(std::string_view host) {
    if (host.empty() || host.size() > 255 || host.front() == '-')
        return false;
    return std::all_of(host.begin(), host.end(), [](char c) {
        return lowerOrDigit(asciiLower(c)) || c == '.' || c == '_' || c == '-' || c == '@';
    });
}

bool validRemoteDir(std::string_view dir) {
    if (dir.empty() || dir.size() > 1024)
        return false;
    std::string_view rest = dir;
    if (rest.front() == '~') {
        rest.remove_prefix(1);
        if (!rest.empty() && rest.front() != '/')
            return false;
    } else if (rest.front() != '/') {
        return false;
    }
    if (!std::all_of(rest.begin(), rest.end(), [](char c) {
            return lowerOrDigit(asciiLower(c)) || c == '.' || c == '_' || c == '-' || c == '/';
        }))
        return false;
    std::size_t at = 0;
    while (at <= rest.size()) {
        const std::size_t slash = std::min(rest.find('/', at), rest.size());
        if (rest.substr(at, slash - at) == "..")
            return false;
        at = slash + 1;
    }
    return true;
}

std::string wipName(std::string_view slug, int round) {
    return std::string(slug) + "-r" + std::to_string(round) + ".ans";
}

std::string remoteCommand(std::string_view dir, std::string_view name, std::size_t size) {
    const std::string d(dir), n(name), z = std::to_string(size);
    // The temporary file is mktemp's own (created exclusively, a dot name
    // the ascii script skips), and the traps remove it if anything fails or
    // the connection drops. cat stops at the end of its input whether the
    // upload finished or the connection dropped, so the length decides
    // whether the round is renamed into place. It gets the mode `cat >`
    // would have given it. PIPE is trapped because the client gone leaves
    // the "only N bytes" message no reader, and dash, zsh and busybox ash
    // run no EXIT trap for a signal that kills them. The signal trap removes
    // the file itself: bash 5.3 on macOS runs no EXIT trap for the `exit` in
    // it. BSD wc pads the count with spaces, which $(( )) drops.
    return "set -e; d=" + d + "; mkdir -p \"$d\"; t=; trap '[ -z \"$t\" ] || rm -f \"$t\"' EXIT; "
           "trap '[ -z \"$t\" ] || rm -f \"$t\"; exit 1' HUP INT TERM PIPE; t=$(mktemp \"$d/." + n + ".XXXXXX\"); cat > \"$t\"; "
           "got=$(($(wc -c < \"$t\"))); [ $got -eq " + z + " ] || { echo \"only $got of " + z + " bytes arrived, so $d/" + n +
           " was left as it was\" >&2; exit 1; }; "
           "chmod \"$(umask -S | tr -d x)\" \"$t\"; mv -f \"$t\" \"$d/" + n + "\"; t=; "
           "f=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/weechat/weechat_fifo_$(pgrep -x weechat | head -n 1)\"; "
           "if [ -p \"$f\" ] && timeout 5 sh -c 'echo \"core.weechat */ascii reload\" > \"$1\"' sh \"$f\"; "
           "then echo " +
           std::string(kReloaded) + "; else echo " + std::string(kNoFifo) + "; fi";
}

std::vector<std::string> sshArguments(const WeechatTarget& target, std::string_view slug, int round,
                                      std::size_t size) {
    if (!validHost(target.host))
        throw PipelineError("\"" + target.host + "\" is not an ssh host: use letters, digits and . _ - @");
    if (!validRemoteDir(target.wipDir))
        throw PipelineError("\"" + target.wipDir + "\" is not a folder on the host: start it with ~/ or /, "
                            "and use letters, digits and . _ - /");
    if (!validSlug(slug))
        throw PipelineError("\"" + std::string(slug) + "\" is not a piece name");
    if (round < 1)
        throw PipelineError("a round is 1 or more");
    return {"-o", "BatchMode=yes", "-o", "ConnectTimeout=10", target.host,
            remoteCommand(target.wipDir, wipName(slug, round), size)};
}

} // namespace bw::pipeline
