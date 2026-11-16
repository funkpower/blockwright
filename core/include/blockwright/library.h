#pragma once
// The Library's stamps (PLAN §12): fixed pieces of art to drop into a scene.
//
// A stamp is two files side by side:
//   <slug>.ans   the art, with a SAUCE record whose title is the stamp's
//                name. Its blank cells (isBlank: a space or NUL on black
//                without blink) are its transparent area, as transparent
//                paste reads them; black inside the part is drawn as █.
//   <slug>.json  the sidecar: {"category": "...", "tags": ["...", ...],
//                "ircBytes": N}, where N is the IRC bytes the stamp's widest
//                row costs through the IRC encoder (widestRowBytes). Nothing
//                else is allowed in it, so a typo in a key is caught. The
//                category is not empty; tags may be none, but none is empty
//                or holds a comma; neither holds a control character.
// A sidecar whose ircBytes disagrees with the .ans is refused, and so is a
// sidecar that repeats a key, and a stamp whose cells are all blank (it
// would draw nothing).
//
// A stamp is CP437, but for one in the user library whose cells CP437 can't
// hold (a wide glyph, a cluster, a glyph with no CP437 byte or whose byte
// .ans reads as a control code; #336): that is written as UTF-8, starting
// with a byte order mark, and its sidecar carries a fourth key,
// "encoding": "utf-8". It is read as UTF-8 only when the .ans starts with
// the mark and the sidecar says so; a sidecar that says so over an .ans with
// no mark is refused, and so is an .ans that starts with the mark under a
// sidecar that doesn't (every CP437 stamp Blockwright writes starts with
// ESC, so that is a UTF-8 stamp that lost its key). A build from before #336
// refuses the fourth key, so it lists such a stamp as a problem rather than
// placing it misread. The bundled library is CP437 only
// (test_library_bundled).
//
// A resizable stamp (#345 slice 2, stamp_slice.h) has a fifth key,
// "insets": [left, right, top, bottom] for one that resizes both ways (a
// frame) or [left, right] for one that resizes in width only (a band),
// whole numbers of cells that leave at least a column (and a row) between
// them to repeat; a stamp holding a wide character can't have them. A
// build from before it refuses the key, so it lists such a stamp as a
// problem. A sidecar is at most kMaxSidecarBytes, refused from its size
// before it is read.
//
// A text style (PLAN §15) is one file, <slug>.style.json:
//   {"kind": "text-style", "name": "...", "category": "...",
//    "tags": ["...", ...], "ice": "required", "effects": [...]}
// "kind" says what the file is and is always "text-style"; the effects are
// in the form a project's layer style holds them (project.h, PLAN §6) and
// checked as a project's are (textStyleProblem: effects and settings it
// knows, at most one of each kind, ramps that exist). "ice" is optional
// and, when there, "required": the look needs a bright background, which
// only iCE colours give (with them off the Fill and Glow ladders stop short
// of the rung that needs one). The name and category are as a stamp's
// category is, the tags as a stamp's. Nothing else is allowed, at the top
// or in an effect, and no top-level key twice. A file is a text style by
// its name (isTextStyleName) and by its kind, so a scan never takes one for
// a stamp's sidecar: no stamp is named <x>.style.ans.
//
// A frame style (#345, frame.h) is one file, <slug>.frame.json, in the
// form frameStyleFromJson reads; its "id" is the file's slug, but the
// Frame tool knows it by its path, so two libraries never clash. As with a
// text style, no stamp is named <x>.frame.ans.
//
// Libraries are directories scanned recursively: the bundled read-only one
// at <prefix>/share/blockwright/library/ and the user's in the app data
// directory.

#include "blockwright/ansi.h"
#include "blockwright/frame.h"
#include "blockwright/selection.h"
#include "blockwright/stamp_slice.h"
#include "blockwright/text_style.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw::library {

class LibraryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
// A removal with Removal::Trash that the trash refused (none, or it
// couldn't take the file): nothing was removed. A caller may then ask the
// user and remove it again with Removal::Delete.
class TrashUnavailable : public LibraryError {
public:
    using LibraryError::LibraryError;
};

// What the sidecar holds.
struct StampMeta {
    std::string category;           // UTF-8, not empty
    std::vector<std::string> tags;  // UTF-8, none empty or holding ','  
    std::size_t ircBytes = 0;       // the widest row's IRC bytes
    // The .ans's text: "encoding": "utf-8" in the sidecar, nothing for CP437.
    TextEncoding encoding = TextEncoding::Cp437;
    // "insets": how the stamp resizes (stamp_slice.h); none for a stamp of
    // a fixed size.
    std::optional<StampSlice> slice;

    bool operator==(const StampMeta&) const = default;
};

struct Stamp {
    std::filesystem::path path;  // the .ans
    std::string name;            // the SAUCE title: CP437 bytes, not empty
    StampMeta meta;
    AnsiDocument art;

    // The name as UTF-8, for showing and searching.
    std::string displayName() const;
    // The art as a block to float: every cell, blank ones included.
    CellBlock block() const;
};

// The largest sidecar read: a sidecar is a few lines, and one this large is
// not one.
inline constexpr std::size_t kMaxSidecarBytes = std::size_t(1) << 20;

// The IRC bytes the widest row of `canvas` costs, as irc::encodeCanvas
// encodes the rows (trailing black spaces dropped).
std::size_t widestRowBytes(const Canvas& canvas);

// The row of `canvas` that WeeChat's ascii script sends in the most bytes
// at `budget` (irc::txtSentBytes, which the row meter and Export hold to
// the budget), and that row's bytes as written (#364). The sidecar's
// ircBytes stays the written size (widestRowBytes).
struct SentRow {
    std::size_t sent = 0;
    std::size_t written = 0;
};
SentRow widestSentRow(const Canvas& canvas, std::size_t budget);

// The sidecar as JSON text (keys in a fixed order, two-space indent, a
// trailing newline), and back. parseSidecar throws LibraryError for
// anything but an object with exactly the three keys, of the right types,
// "encoding" when it is "utf-8" (written only for a UTF-8 stamp, so a
// CP437 stamp's sidecar is as it always was) and "insets" when it is two
// or four whole numbers from 0 to kMaxRows (written only for a resizable
// stamp; readStamp checks them against the art). sidecarJson can't write a
// slice that resizes in height only and throws LibraryError for one.
std::string sidecarJson(const StampMeta& meta);
StampMeta parseSidecar(std::string_view json);

// The sidecar beside an .ans: the same path with ".json".
std::filesystem::path sidecarPath(const std::filesystem::path& ans);

// Stamp file names, with the extension's ASCII case folded (X.ANS is art,
// X.Json a sidecar), the same rule stampPathFor uses for its slots.
bool isArtName(const std::filesystem::path& p);
bool isSidecarName(const std::filesystem::path& p);
// The sidecar an .ans pairs with, as its folder lists it: <stem>.json if
// listed, else the first listed <stem>.<json in any case>, else <stem>.json.
// readStamp reads it, writeStamp writes over it and scan pairs by it.
std::filesystem::path pairedSidecar(const std::filesystem::path& ans);

// The whole of the regular file at `p` (through a link). On POSIX the
// opened handle is what is checked: the file is opened without blocking and
// refused unless fstat says it is a regular file, so a FIFO swapped in after
// any earlier path check can't block the reader. Throws LibraryError ("can't
// be read", "is not a regular file", "read failed", and "is larger than N
// bytes" for one past `limit`, found before it is read or as it grows).
std::string readRegularFile(const std::filesystem::path& p,
                            std::size_t limit = std::numeric_limits<std::size_t>::max());

// Reads and checks the stamp at `ans` and its sidecar. Throws LibraryError
// saying what is wrong: an unreadable or unparsable file, no SAUCE or an
// empty title, every cell blank, a bad sidecar, an ircBytes that
// disagrees with the art, or insets that can't cut it (sliceProblem).
Stamp readStamp(const std::filesystem::path& ans);

// The two files of a new stamp.
struct StampFiles {
    std::string ans, json;
};

// A stamp of `cells` (at most kMaxColumns x kMaxRows) named `name`: CP437
// bytes, 1-35, no NUL, not ending in a space (0x01-0x1F are glyphs here,
// ☺ ♥ …; readStamp holds a SAUCE title to the same rules). The cells are
// written as they show with iCE colours `ice` (iceCell), as the editor
// saves a document, under a fresh SAUCE record titled `name`, in CP437 when
// CP437 holds every cell as it is (nothing in AnsiOutput::unwritable and no
// cluster, not even one CP437 composes to a byte: é as e + U+0301 would
// read back as U+00E9), so such a stamp is byte for byte what it was before
// #336. Otherwise in UTF-8 with a byte order mark and "encoding": "utf-8" in
// the sidecar (see the top), so wide glyphs and clusters read back as they
// were. Throws LibraryError for a bad name, category or tag, an empty, too
// large or all-blank block, cells neither can hold (glyphs written as '?',
// blink lost), or any failure of the .ans writer, parser or IRC encoder.
// With `slice` the stamp is resizable and its sidecar carries the insets;
// a slice that can't cut the cells (sliceProblem), or that resizes in
// height only, is refused.
StampFiles encodeStamp(const std::string& name, const std::string& category,
                       const std::vector<std::string>& tags, const CellBlock& cells, bool ice,
                       const std::optional<StampSlice>& slice = std::nullopt);

// Why encodeStamp refuses what writeAnsi wrote, in English with each count
// in the singular or the plural: the cells it lost a glyph of (those with
// no CP437 code, those whose byte .ans reads as a control code, the
// clusters written as their base, then the wide glyphs, counted: "3 wide
// glyphs written as ?"), else the blinking cells that lost their blink.
// For a UTF-8 output the glyphs are those it can't write: a control
// character, a wide glyph with no continuation, a lone mark, a lead whose
// glyph isn't two columns. Empty when nothing was lost.
std::string stampLossMessage(const AnsiOutput& out);

// Writes both files (the sidecar at pairedSidecar(ans)), replacing a stamp
// already there, under their exact native names (a name that isn't UTF-8
// stays as it is) and, for a file replaced, its permission bits: 0777 only,
// set-user-ID, set-group-ID and sticky are not kept (a new file gets 0666
// less the umask). Both are written to
// temporary files first, so a failure while writing leaves the old pair
// untouched; then the sidecar and the art are renamed into place, and if
// the art's rename fails the old sidecar is put back (the error says so if
// that fails too). Refuses (LibraryError) when either path is a symbolic
// link, rather than writing through it to a file outside the library, or
// anything else that isn't a regular file.
void writeStamp(const std::filesystem::path& ans, const StampFiles& files);

namespace testing {
// Called by writeStamp at "fsync" (after writing each temporary file; a
// throw is taken as fsync failing with ENOSPC), "art" (just before the
// art's rename) and "restore" (before putting the old sidecar back); a hook
// that throws LibraryError makes that step fail. Tests only.
inline std::function<void(std::string_view step)> writeStampFault;
// Called by writeTextStyle with the preset's path once its temporary is
// written, just before it is moved into place. Tests only.
inline std::function<void(const std::filesystem::path&)> beforeTextStylePublish;
// Called by writeTextStyle as it moves the preset into place, at "rename"
// (a throw is taken as the exclusive rename failing with EINVAL, so the
// hard-link fallback runs) and "unlink" (a throw is taken as removing the
// temporary's name after the link failing with EIO). POSIX only. Tests only.
inline std::function<void(std::string_view step)> textStylePublishFault;
// When set, called instead of QFile::moveToTrash; false is a trash that
// refused. Tests only.
inline std::function<bool(const std::filesystem::path&)> moveToTrash;
} // namespace testing

// A file name for a stamp called `name` (CP437 bytes): lower-case ASCII
// letters, digits and '-', at most 48 characters. A name with no ASCII
// letter or digit is named by its glyphs' code points instead ("♥" is
// "u2665", "☺ ♥" "u263a-u2665"); "stamp" only if nothing is left.
std::string slug(std::string_view name);

// Where Save to Library writes a stamp called `name` in `dir`: whichever of
// <slug>.ans, <slug>-2.ans, <slug>-3.ans, … holds the stamp whose SAUCE
// title is exactly `name` (exists is true: saving replaces it); else the
// lowest of them that nothing (no .ans or .json of any kind) sits on. A
// name never replaces a stamp with another title and is never saved twice.
// A `dir` that doesn't exist is empty; one that can't be listed throws
// LibraryError rather than being taken as empty. File names are compared
// with ASCII case folded, so on a case-insensitive volume OAK-TREE.ans is
// <slug>.ans (a same-titled one is returned as it is named).
struct StampTarget {
    std::filesystem::path path;
    bool exists = false;
};
StampTarget stampPathFor(const std::filesystem::path& dir, const std::string& name);

// A text style entry (see the top).
struct TextStyleEntry {
    std::filesystem::path path;     // the .style.json; empty when parsed from text
    std::string name;               // UTF-8, not empty
    std::string category;           // UTF-8, not empty
    std::vector<std::string> tags;  // as a stamp's
    bool iceRequired = false;       // "ice": "required"
    TextStyle style;

    bool operator==(const TextStyleEntry&) const = default;
};

// Whether `p` is named as a text style: it ends in ".style.json", ASCII
// case folded.
bool isTextStyleName(const std::filesystem::path& p);

// The entry as JSON text (keys in a fixed order, two-space indent, a
// trailing newline; the path isn't written), and back. parseTextStyleEntry
// throws LibraryError saying what is wrong.
std::string textStyleEntryJson(const TextStyleEntry& entry);
TextStyleEntry parseTextStyleEntry(std::string_view json);
// The entry in the regular file at `p` (through a link), its path `p`.
TextStyleEntry readTextStyleEntry(const std::filesystem::path& p);

// The text a style's thumbnail shows, "Aa" in the block face, drawn with
// `style` and iCE colours `ice` (renderStyledText).
Canvas textStyleSample(const TextStyle& style, bool ice);

// Where Save as Preset writes a text style called `name` (UTF-8) in `dir`:
// the lowest of <slug>.style.json, <slug>-2.style.json, … that nothing sits
// on, compared with ASCII case folded. A preset never replaces another file.
// A `dir` that doesn't exist is empty; one that can't be listed throws
// LibraryError.
std::filesystem::path textStylePathFor(const std::filesystem::path& dir, const std::string& name);

// Writes the entry (its path ignored) to textStylePathFor(dir, entry.name),
// making the folder if need be, through a temporary file moved into place
// only if nothing sits there (an exclusive rename, or a hard link), and
// returns the path. Throws LibraryError for an entry parseTextStyleEntry
// would refuse (the bytes are read back before anything is written), a file
// made at the path meanwhile ("already exists"; it is left as it was) or a
// write that fails.
std::filesystem::path writeTextStyle(const std::filesystem::path& dir, const TextStyleEntry& entry);

// A frame style entry (see the top).
struct FrameStyleEntry {
    std::filesystem::path path;  // the .frame.json
    FrameStyle style;
};

// Whether `p` is named as a frame style: it ends in ".frame.json", ASCII
// case folded.
bool isFrameStyleName(const std::filesystem::path& p);
// The style in the regular file at `p` (through a link). Throws
// LibraryError saying what is wrong; a file past kMaxFrameStyleBytes is
// refused without being read. The scan lists any frame style it can't read,
// running out of memory included, as a problem.
FrameStyleEntry readFrameStyleEntry(const std::filesystem::path& p);
// Where Selection > Save as Frame Style writes a style called `name`
// (UTF-8) in `dir`: as textStylePathFor, with ".frame.json".
std::filesystem::path frameStylePathFor(const std::filesystem::path& dir, const std::string& name);
// Writes `style` to frameStylePathFor(dir, style.name), its "id" the file's
// slug, as writeTextStyle writes a text style (never over another file),
// and returns the path. Throws LibraryError for an empty name or one
// holding a control character, a style frameStyleFromJson would refuse,
// a file made at the path meanwhile, or a write that fails.
std::filesystem::path writeFrameStyle(const std::filesystem::path& dir, const FrameStyle& style);

struct Problem {
    std::filesystem::path path;
    std::string message;
};

struct ScanResult {
    std::vector<Stamp> stamps;            // sorted by path
    std::vector<TextStyleEntry> styles;  // sorted by path
    std::vector<FrameStyleEntry> frames;  // sorted by path
    // Sorted by path: stamps that failed readStamp, text styles that failed
    // readTextStyleEntry, frame styles that failed readFrameStyleEntry,
    // stamps named <x>.style.ans or <x>.frame.ans, sidecars with no .ans,
    // broken symbolic links, links to folders (not followed), folders that
    // can't be read, and .ans or .json entries that aren't regular files
    // (a FIFO is never opened), or that can't be read at all.
    std::vector<Problem> problems;
};

// The rows of a scanned library over `budget` IRC bytes (library check
// holds them to irc::kEfnetWeechatBudget), in order: each stamp whose
// widest row is over, then each text style whose thumbnail sample (with
// iCE colours, and unless the style needs them without) can't be drawn or
// has a row over. A row is over when WeeChat's ascii script sends it in
// more bytes than `budget` (widestSentRow, as Export measures it), or when
// it is written in more (a stamp's sidecar ircBytes, a sample's
// widestRowBytes); each is one problem (#364).
std::vector<Problem> budgetProblems(const ScanResult& scanned, std::size_t budget);

// Every stamp under `dir`, recursively: each art file (isArtName) with its
// pairedSidecar; every text style, each .style.json no stamp pairs
// with (a stamp named <x>.style.ans is a problem); and every frame style,
// each .frame.json, likewise. A symbolic link to a file is read
// through; one to a folder is not followed, so that a link isn't followed by
// accident (not as a security boundary: a folder swapped for a link while
// the scan runs is followed, and whoever can do that can already write
// stamps there). A missing directory is an empty library, not a problem.
ScanResult scan(const std::filesystem::path& dir);

// The categories of `stamps`, sorted, each once.
std::vector<std::string> categories(const std::vector<Stamp>& stamps);

// Whether `stamp` matches a search: every whitespace-separated term of
// `query` is in its name or one of its tags, ignoring ASCII case. An empty
// query matches everything.
bool matches(const Stamp& stamp, std::string_view query);
// The same for a text style, by its name and tags.
bool matches(const TextStyleEntry& style, std::string_view query);

// Collections (#464, PLAN §12): the user's own named sets of stamps.
//
// A collection is a folder at the top of the user library,
// <library>/<collectionSlug(name)>/, whose stamps (scanned recursively, as
// any library folder is) carry its name as their sidecar category. The
// folder holds a one-line marker file, kCollectionMarker, with the name in
// UTF-8, so a collection keeps its name exactly ("My Trees", not the folder's
// "my-trees") while it holds no stamp; the marker is a dot file with no .json
// or .ans extension, which the scan (this build's and older ones) never
// reads. A folder with no readable marker is still a collection: its name is
// the category its stamps share if they all share one, else the folder's
// own name. Stamps at the top of the library belong to no collection and
// work as they always did.
//
// Every function here takes the library to change (the user library, or a
// folder the CLI's --dir names) and changes nothing outside it, so a
// collection named like a bundled category never touches the bundled
// stamps. Each that changes anything refuses a library that is, is inside
// or holds the running program's bundled library (bundledLibraryDir(),
// compared with links resolved): bundled stamps are never changed. Each
// refuses (LibraryError) a name that isn't a category's (empty, blank, a
// control character), isn't UTF-8 or is longer than kMaxCollectionName
// bytes, a name already taken, a folder that can't be listed, and a stamp
// outside `library`, before writing anything; a write that fails leaves the
// old files (writeStamp's rules).

inline constexpr const char* kCollectionMarker = ".collection";
inline constexpr std::size_t kMaxCollectionName = 128;  // bytes of UTF-8

struct Collection {
    std::string name;            // UTF-8, as a category is
    std::filesystem::path dir;   // <library>/<slug>
    std::size_t stamps = 0;      // stamps in it that read cleanly
};

// The folder name for a collection called `name` (UTF-8), made as slug()
// makes a stamp's: "My Trees" is my-trees, "♥" u2665. A slug Windows
// reserves as a device (con, prn, aux, nul, com1-com9, lpt1-lpt9) gets a
// '-' after it ("con-"), which no other name's slug ends in.
std::string collectionSlug(std::string_view name);

// Every collection in `library`, sorted by name: each folder at its top
// (not a link to one: the scan doesn't follow those either). A library that
// doesn't exist has none; one that can't be listed throws LibraryError. A
// folder named by its own name (no marker, no shared category) has each
// control character and each byte that isn't UTF-8 in it as U+FFFD, so a
// name never holds a tab or newline.
std::vector<Collection> collections(const std::filesystem::path& library);
// The collection called exactly `name`, if there is one. Two folders going
// by the name (a folder with no marker whose stamps share another
// collection's name) is refused (LibraryError naming both), not settled by
// picking one.
std::optional<Collection> findCollection(const std::filesystem::path& library, const std::string& name);
// The collection whose folder holds `path`, if any.
std::optional<Collection> collectionOf(const std::filesystem::path& library, const std::filesystem::path& path);

// Refuses (LibraryError) a library that is, is inside or holds
// bundledLibraryDir(), links resolved. Every function below that changes
// anything calls it first; a caller may too, before its own checks.
void refuseBundled(const std::filesystem::path& library);

// Makes an empty collection: its folder and marker, and `library` if need
// be. Refused when a collection has the name already or something sits on
// its folder's name.
Collection createCollection(const std::filesystem::path& library, const std::string& name);

// How a stamp or a collection goes: to the system trash
// (QFile::moveToTrash), or deleted outright. Trash never falls back to
// deleting: when the trash refuses (there is none, or it can't take the
// file), TrashUnavailable is thrown and nothing is removed; Delete is the
// caller's explicit choice, after its own confirmation.
enum class Removal { Trash, Delete };
struct Removed {
    std::size_t stamps = 0;  // the stamps that went
    bool trashed = false;    // moved to the trash rather than deleted
};

// Removes the collection called `name`, its folder and everything in it.
// Either way it first checks that every folder in it, and the library
// holding it, can be listed and written (POSIX access(); not checked on
// Windows), so a permission that would stop it halfway refuses it before
// anything goes. Trash then moves the folder in one call (on Linux and the
// BSDs one rename into the trash when the trash is on the same volume, so
// all of it goes or none), and a trash that reports success with the
// folder still here is TrashUnavailable. What the check can't see (an
// immutable file, a change made meanwhile) can still stop Delete, or a
// trash that copies, partway; the error then names the path.
Removed deleteCollection(const std::filesystem::path& library, const std::string& name, Removal how = Removal::Trash);

// Renames a collection: its folder to collectionSlug(to) (kept when the slug
// is the same), its marker, and the category of every stamp in it. Refused
// when no collection has the name `from`, one has `to` already, or
// something else sits on the new folder's name. Every stamp is read and its
// new sidecar made before anything moves; a sidecar that can't be written
// afterwards keeps its old category, and the error names it.
Collection renameCollection(const std::filesystem::path& library, const std::string& from, const std::string& to);

// Adds a stamp of `cells` named `name` to the collection (encodeStamp, with
// the collection's name as the category), at stampPathFor in its folder, and
// returns the .ans path. Refused when the collection doesn't exist or holds
// a stamp with that title already (anywhere in its folder, whatever the
// file is called), unless `replace`, which writes over that stamp.
std::filesystem::path addStamp(const std::filesystem::path& library, const std::string& collection,
                               const std::string& name, const std::vector<std::string>& tags, const CellBlock& cells,
                               bool ice, const std::optional<StampSlice>& slice = std::nullopt, bool replace = false);

// Adds the art file `ans` (isArtName) to the collection as a copy. A stamp
// (an .ans with a sidecar beside it, bundled or the user's) is copied as it
// is, its art byte for byte and its sidecar with the collection's name as
// the category; it must read cleanly (readStamp). Any other .ans is
// imported: read as the CLI reads one (its encoding detected), named by its
// SAUCE title when that is a stamp's name, else by its file name (in CP437,
// what CP437 can't hold dropped, cut to SAUCE's 35 bytes), and written by
// encodeStamp with no tags and the piece's own iCE colours. Refused as
// addStamp refuses, and for anything encodeStamp refuses (all blank, past
// the size limits, glyphs a stamp can't hold).
struct Added {
    std::filesystem::path path;  // the new .ans
    std::string name;            // its title, CP437
};
Added addStampFile(const std::filesystem::path& library, const std::string& collection, const std::filesystem::path& ans);

// Removes the stamp at `ans` (its art and its sidecar) from `library`; a
// stamp that doesn't read cleanly, or has no sidecar, can be removed too.
// Refused for anything outside `library` (a bundled stamp), not named as
// art, or not there, or in a folder a permission stops it leaving
// (requireRemovable). The sidecar goes first, then the art. When either
// can't (TrashUnavailable, or Delete's folder check) nothing is removed: a
// sidecar already gone is written back from a copy read beforehand, with
// its permission bits and modification time (the trash keeps the copy it
// took). A sidecar that can't be read beforehand couldn't be written back,
// so Trash refuses it (TrashUnavailable) before anything goes; Delete
// takes it. Only if a write-back fails does the art stay without its
// sidecar, and the error says so.
Removed removeStamp(const std::filesystem::path& library, const std::filesystem::path& ans, Removal how = Removal::Trash);
// What removeStamp checks before it removes anything (LibraryError when
// `ans` isn't a stamp it would take), so a caller can say what would go.
// It can't know beforehand whether the trash will take the files, or
// whether a sidecar can be read for a write-back: removeStamp may still
// refuse those (TrashUnavailable), removing nothing.
void requireRemovable(const std::filesystem::path& library, const std::filesystem::path& ans);

// A change to a stamp: each field left empty is kept.
struct StampEdit {
    std::optional<std::string> name;               // CP437, encodeStamp's rules
    std::optional<std::string> category;           // UTF-8
    std::optional<std::vector<std::string>> tags;  // UTF-8
};
// Renames, re-files or re-tags the stamp at `ans` in `library` and returns
// where it is now. The file stays where it is unless the category moves it:
// into the folder of the collection the new category names, or, for a stamp
// leaving a collection for a category no collection has, to the top of the
// library. A new name rewrites only the SAUCE title (the art's bytes before
// it are kept); a stamp moved is written at stampPathFor in its new folder
// and its old files removed. Refused for a stamp outside `library`, one that
// doesn't read cleanly, a name or category encodeStamp would refuse, and a
// title another stamp has in the destination collection (anywhere in its
// folder) or, at the top of the library, among the stamps there.
std::filesystem::path editStamp(const std::filesystem::path& library, const std::filesystem::path& ans,
                                const StampEdit& edit);

// Where the libraries are. BLOCKWRIGHT_BUNDLED_LIBRARY and
// BLOCKWRIGHT_USER_LIBRARY override them (tests, and running a build tree
// against the repo's library/). The bundled library is found from the
// directory holding the executable (`binDir`) as installed:
// <prefix>/share/blockwright/library/. The user library is
// QStandardPaths::GenericDataLocation + "/Blockwright/library", so the GUI
// and the CLI (whose application names differ) find the same one. On Linux
// and macOS that is the GUI's AppDataLocation + "/library"; on Windows it is
// under AppData/Local (AppLocalDataLocation), not the roaming AppData.
std::filesystem::path bundledLibraryDir(const std::filesystem::path& binDir);
// The running program's bundled library: BLOCKWRIGHT_BUNDLED_LIBRARY, else
// bundledLibraryDir(realBinDir(...)) of the QCoreApplication's executable.
// Empty with neither (no override and no QCoreApplication).
std::filesystem::path bundledLibraryDir();
// The directory of the real executable `exe`, with symlinks resolved, so a
// binary started through a link elsewhere (the dev install's ~/.local/bin
// links) still finds its own prefix. When `exe` can't be resolved (renamed
// or deleted while running) it is `fallback`, the directory it was started from.
std::filesystem::path realBinDir(const std::filesystem::path& exe, const std::filesystem::path& fallback);
std::filesystem::path userLibraryDir();

} // namespace bw::library
