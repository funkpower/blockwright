#pragma once
// The pipeline actions (PLAN §9). Publish to Library writes a piece into the
// ascii library: <library>/funkpower/<slug>.ans and .txt, and its entries in
// the .tags.json and .dates.json indexes the WeeChat ascii script keeps at
// the library's root. Send to WeeChat copies a round to the IRC host's wip
// folder over ssh and sends "*/ascii reload" to WeeChat's fifo.

#include "blockwright/irc.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw::pipeline {

class PipelineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The library folder a piece goes into, and the tag every piece there has.
inline constexpr std::string_view kArtist = "funkpower";

// A piece's name in the library and in wip: 1-64 of a-z, 0-9, '-' and '_',
// starting with a letter or a digit.
bool validSlug(std::string_view slug);
// A slug suggested from a file's base name: ASCII letters lowercased, digits,
// '-' and '_' kept, every other run as one '-'; anything but a letter or a
// digit trimmed from the start, cut to 64, '-' trimmed from the end. May be
// empty.
std::string slugFrom(std::string_view name);

// Tags as typed: split on commas and spaces, lowercased by Unicode's case
// mapping as the ascii script's str.lower() does when it reads them (all but
// its word-final sigma rule: a final Σ becomes σ, not ς), repeats and empty
// ones dropped, sorted. Throws PipelineError for a control character or text
// that isn't UTF-8.
std::vector<std::string> parseTags(std::string_view text);

// The library's indexes: an object of piece keys ("funkpower/<slug>", the
// path under the library without its extension) to a sorted list of tags,
// or to the time the piece joined the library in seconds since the epoch.
using TagIndex = std::map<std::string, std::vector<std::string>>;
using DateIndex = std::map<std::string, std::int64_t>;

// Parses an index. Throws PipelineError for anything but one object whose
// values are all lists of strings (tags) or all integers (dates), with no
// key twice: a file the ascii script would misread is left alone.
TagIndex parseTagIndex(std::string_view json);
DateIndex parseDateIndex(std::string_view json);
// An index as the ascii script writes it, Python's json.dump(indent=1) with
// the keys sorted: one space of indent a level, ", " never, ": " after a
// key, non-ASCII as \uXXXX. `newline` ends the file with a '\n'.
std::string tagIndexJson(const TagIndex& index, bool newline);
std::string dateIndexJson(const DateIndex& index, bool newline);

// The IRC lines of a piece's .ans bytes, as `blockwright-cli export --irc`
// makes them from the file: parsed, then irc::exportCanvas.
std::vector<std::string> ircLines(std::string_view ans);

struct PublishRequest {
    std::filesystem::path library;  // the ascii library's root
    std::string slug;
    std::vector<std::string> tags;  // besides kArtist, from parseTags
    std::string ans;                // the .ans bytes, as Save writes them
    std::int64_t now = 0;           // the date a piece new to the index gets
    std::size_t budget = irc::kEfnetWeechatBudget;
};

// What the library held when a publish was worked out: the bytes of each
// file it writes (none for one that wasn't there) and the names under
// funkpower/ that the ascii script takes for the piece.
struct LibraryState {
    std::optional<std::string> ans, txt, tags, dates;
    std::vector<std::string> names;  // sorted
    bool operator==(const LibraryState&) const = default;
};

// What publish writes: every file's path and whole new contents.
struct PublishPlan {
    std::string key;  // "funkpower/<slug>"
    std::filesystem::path ansPath, txtPath, tagsPath, datesPath;
    std::string ans, txt, tagsJson, datesJson;
    std::vector<std::string> tags;  // the piece's tags after publishing
    bool replaces = false;          // <slug>.ans or <slug>.txt is there already
    bool dated = false;             // the piece is new to .dates.json
    LibraryState basis;             // the library the plan was worked out from
};

// Works out the publish without writing anything. The piece's tags are the
// ones it has (lowercased as parseTags does), kArtist and the request's; a
// piece already in .dates.json keeps its date (when it joined the library).
// Every other index entry is kept as it is, and each index keeps whether it
// ended in a newline; one that isn't there yet is started. Throws
// PipelineError for a bad slug, a library without a funkpower/ folder, an
// index that can't be read or parsed or isn't laid out byte for byte as
// tagIndexJson/dateIndexJson would write it back, another file under
// funkpower/ that the ascii script would take for the same piece (the same
// name in another case or with another extension), an .ans that doesn't
// parse, or an IRC line over the budget.
PublishPlan planPublish(const PublishRequest& request);

// Writes the plan's files: each to a temporary file beside it (created
// exclusively, so nothing already there is written through), then all
// renamed into place, the .ans and .txt before the indexes. Refuses a path
// that is there and isn't a regular file (a symbolic link included), and a
// plan whose library has changed since it was worked out (its basis), so
// another publish or an edit made while the user was asked is never
// written over: nothing is written, and the error names the file that
// changed. Throws PipelineError saying which files were written if one
// fails.
void publish(const PublishPlan& plan);

// Where Send to WeeChat copies a round.
struct WeechatTarget {
    std::string host;                    // an ssh destination
    std::string wipDir = "~/ascii/wip";  // on that host
};

// An ssh destination ([user@]host, with an optional ssh config alias):
// letters, digits and ". _ - @", not starting with '-'.
bool validHost(std::string_view host);
// A folder on the host: "~", "~/..." or "/...", of letters, digits and
// ". _ - /", with no ".." part. The remote shell expands the '~'.
bool validRemoteDir(std::string_view dir);
// A round's name in wip: "<slug>-r<round>.ans".
std::string wipName(std::string_view slug, int round);

// What the remote command prints last: kReloaded once WeeChat's fifo took
// the reload, kNoFifo when no live WeeChat has one (the file is copied
// either way; WeeChat then needs "/fifo enable" and "/ascii reload").
inline constexpr std::string_view kReloaded = "RELOADED";
inline constexpr std::string_view kNoFifo = "NOFIFO";

// The shell command run on the host: it reads the .ans, `size` bytes, on
// stdin into <dir>/<name> through a temporary file of its own (mktemp,
// removed by a trap if anything fails or the connection drops). Anything
// but `size` bytes is an upload cut short: the command says how much
// arrived and exits 1, leaving <dir>/<name> as it was. Then it writes the
// reload to the fifo of the running WeeChat (weechat_fifo_<pid> in
// $XDG_RUNTIME_DIR, or /run/user/<uid>, /weechat), under a 5 second
// timeout: a fifo left by a WeeChat that has exited has no reader and
// would block forever.
std::string remoteCommand(std::string_view dir, std::string_view name, std::size_t size);
// ssh's arguments: batch mode (it never asks for a password), a connect
// timeout, the host and remoteCommand for an .ans of `size` bytes. Throws
// PipelineError for a bad host, folder, slug or a round under 1.
std::vector<std::string> sshArguments(const WeechatTarget& target, std::string_view slug, int round,
                                      std::size_t size);

} // namespace bw::pipeline
