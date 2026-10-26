#pragma once
// Projects (PLAN §6): a layer stack with its format, notes, snapshots and the
// files kept with it, saved as one zip file or as a directory of the same
// layout (for git and other tools):
//
//   manifest.json      "format": "blockwright", "version": "1.1", the canvas
//                      size, palette, iCE, IRC budget, SAUCE fields (font and
//                      letter spacing among them), the layer order (ids,
//                      bottom first) and the next layer id
//   layers/<id>.json   the layer's properties (LayerProperties, its size;
//                      a live text layer's source, text.h)
//   layers/<id>.cells  its cells (encodeCells: a versioned header, zstd)
//   notes.md           brief, plan, notes; absent when empty
//   snapshots/r<N>.ans saved rounds, each the composite flattened to .ans
//   refs/, scripts/<id>/, thumbnail.png and anything else: kept as bytes
//
// Version: "major.minor". A reader refuses a major it doesn't know rather
// than drop data on re-save. Within a major, a reader ignores the keys and
// files it doesn't know; the files are kept (Project::files) and saved back,
// the keys are not, so a minor version may only add what an older reader
// can drop without changing what the project shows.
//
// Every layer kind round-trips: cells, pixels and text layers by their
// cells; generator and reference layers keep their cells (a generator's
// cached output) and their files under scripts/<id>/ and refs/. A live text
// layer keeps its source (1.1) beside its cells, its drawn text as saved: a
// 1.0 reader drops the source and shows the same cells. A source this build
// can't draw (a face it doesn't have, a setting out of range) or one on a
// layer that isn't a text layer is refused, by the reader and the writer.

#include "blockwright/ansi.h"
#include "blockwright/irc.h"
#include "blockwright/layer.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw::project {

inline constexpr int kFormatMajor = 1;
inline constexpr int kFormatMinor = 1;

// Limits a reader holds a file to, and a writer checks before it writes
// anything: no more than kMaxEntries entries, none over kMaxEntryBytes, all
// of them together under kMaxTotalBytes; no more than kMaxLayers layers,
// none over kMaxLayerCells cells or 65,535 a side, all of them together no
// more than kMaxTotalCells cells (about 256 MiB decoded, checked from the
// .cells headers before any layer is decoded); manifest.json and each
// layers/<id>.json no more than kMaxJsonBytes, checked before it is parsed;
// a canvas within ansi.h's kMaxColumns by kMaxRows; SAUCE text within the
// record's fields (Sauce::kTitleLen and the rest, a date of 8).
inline constexpr std::size_t kMaxEntries = 10'000;
inline constexpr std::size_t kMaxEntryBytes = std::size_t{256} << 20;
inline constexpr std::size_t kMaxTotalBytes = std::size_t{1} << 30;
inline constexpr std::size_t kMaxLayers = 1024;
inline constexpr std::size_t kMaxLayerCells = std::size_t{kMaxColumns} * kMaxRows;
inline constexpr std::size_t kMaxTotalCells = 8 * kMaxLayerCells;
inline constexpr std::size_t kMaxJsonBytes = std::size_t{1} << 20;

class ProjectError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The byte budget an IRC line is held to (PLAN §3): a named preset, or a
// number worked out for a nick mask and channel ("custom").
struct IrcBudget {
    std::string profile = "efnet-weechat";
    std::size_t bytes = irc::kEfnetWeechatBudget;

    bool operator==(const IrcBudget&) const = default;
};

// A saved round: snapshots/r<round>.ans, the .ans bytes as written.
struct Snapshot {
    int round = 0;
    std::string ans;

    bool operator==(const Snapshot&) const = default;
};

struct Project {
    LayerStack layers;
    bool ice = true;
    // The SAUCE fields flatten writes: everything an .ans save keeps (title,
    // author, group, date, comments, font, TInfo3 and TInfo4, the flags but
    // iCE: letter spacing, aspect ratio and the reserved bits). The size and
    // iCE come from the canvas and `ice`; data and file type are ANSi;
    // FileSize is written as the content's. Without one, flatten writes a
    // fresh record.
    std::optional<Sauce> sauce;
    IrcBudget ircBudget;
    std::string notes;               // notes.md
    std::vector<Snapshot> snapshots;  // by round, first first
    // Every other entry by its path in the project ('/'-separated, relative):
    // refs/…, scripts/<id>/…, thumbnail.png, and whatever a newer minor
    // version added. Kept byte for byte.
    std::map<std::string, std::string> files;

    bool operator==(const Project&) const = default;
};

// A flat file as a project: one Background layer (LayerStack::fromCanvas),
// its SAUCE and iCE, opened as the editor opens it (openAsEditor, whose
// result goes to `opened`), so toAnsi writes it back as writeAnsi does.
Project fromAnsi(const AnsiDocument& doc, Opened* opened = nullptr);
// The project flattened through the compositor (bw::flatten) with its iCE
// and SAUCE fields, each cell as it shows with the project's iCE (iceCell),
// as the editor saves: what exports and snapshots write with writeAnsi.
AnsiDocument toAnsi(const Project& project);

// Flattens the project to .ans and keeps it as the next round (one past the
// last, r1 for the first). Returns what writeAnsi wrote, so a caller can
// report a lossy snapshot (glyphs written as '?', blink dropped).
AnsiOutput takeSnapshot(Project& project);

// A layer's cells as layers/<id>.cells holds them: "BWCL", the format
// version (u16, 1), reserved (u16, 0), width and height (u32), then one zstd
// frame of width × height 14-byte cells, row by row: the code point (u32),
// foreground and background (u8 kind, 0 a palette index then the index and
// two zeros, 1 RGB then r, g, b), attributes (u8) and flags (u8, bit 0
// transparent). All little-endian. decodeCells throws ProjectError for
// anything else: another magic or version, a size over the limits, a frame
// that isn't exactly the cells, a code point past U+10FFFF, an unknown
// colour kind or flag.
std::string encodeCells(const Canvas& cells);
Canvas decodeCells(std::string_view data);

// The project as its entries, path to bytes, and back. decode throws
// ProjectError for a manifest it can't read (not JSON, another format, an
// unknown major version, a missing or mistyped key, a size over the limits),
// a layer it lists without both files or whose files disagree with it or
// each other, a layers/<id>.json or .cells it doesn't list, two layers with
// one id, a next layer id at or below one in use, entries or layers over
// the limits, or two paths a disk may take for one: equal once case-folded
// and Unicode-normalised ("refs/A" and "refs/a", "é" composed and
// decomposed, a directory spelled two ways), or one a file and the other a
// directory ("refs/x" and "refs/x/y"); so a directory save never writes one
// file over another. encode
// throws it for a path in `files` that is unsafe or that the format owns
// (manifest.json, notes.md, layers/<n>.json and .cells, snapshots/r<N>.ans),
// and for a project whose entries decode would refuse that way.
using Entries = std::map<std::string, std::string>;
Entries encode(const Project& project);
Project decode(const Entries& entries);

// Whether a path in a project is one a reader takes and a writer writes:
// relative and '/'-separated, no empty, ".", ".." or hidden (leading '.')
// part, none over 255 bytes (longer than a disk takes) and no more than 1024
// bytes in all (so a directory save has room for where it goes), no
// backslash, colon or control character.
bool safeEntryPath(std::string_view path);

// Whether a case-insensitive or Unicode-normalising disk may take `a` and
// `b` for one path: both compared as NFD of the full case fold of NFD, so
// "ß", "ẞ" and "ss" are one, as are "é" composed and decomposed. A project
// holding two such paths is refused, and a directory save doesn't remove a
// file it finds under another such spelling of one it wrote.
bool samePath(std::string_view a, std::string_view b);

// Whether `path` is a project: a directory with a manifest.json, or a file
// that starts as a zip does (read says whether the zip is a project, so a
// damaged one is reported as such rather than read as something else).
bool isProject(const std::filesystem::path& path);

// Reads a project from a zip file or a directory. Hidden entries in either
// are skipped; in a directory, whatever they are (a link, a pipe, a
// directory it can't open), and nothing under a hidden directory is looked
// at. Anything else that isn't a safe path (safeEntryPath), an entry
// repeated or over the limits is refused, as is any symbolic link or other
// entry that isn't a file or directory (in a zip, by its UNIX mode, hidden
// or not), and everything decode refuses.
Project read(const std::filesystem::path& path);

// Both writers encode the project first, so one decode would refuse is
// refused before the destination is touched.
//
// Writes the project as one zip file, replacing `path` whole (libzip writes
// a temporary file beside it and renames it over). Entries are stored with
// a fixed time, so a project writes the same bytes every time; .cells and
// .png entries are stored, the rest deflated.
void writeZip(const Project& project, const std::filesystem::path& path);

// Writes the project as a directory: each entry written whole (a new
// temporary file, created exclusively, renamed over it), manifest.json last,
// then every other file under the directory that isn't hidden removed (never
// one the save wrote, whatever name the disk lists it under; a symbolic link
// is never one, so one is removed even at an entry's own name), and the
// directories that leaves empty. Refuses an empty path before writing
// anything, a directory that holds anything but hidden files and has no
// manifest.json, so a save never empties a directory that isn't a project,
// and a symbolic link where an entry or a directory on its way goes. A path
// longer than the system opens, the directory's own included, is refused
// before anything is written. Every entry is written to a temporary first
// and renamed into place only once all are written, the manifest last;
// files the project no longer holds are then renamed aside under hidden
// names and removed only once all are aside.
//
// What a save promises, on POSIX systems:
// - It never writes, follows a link or deletes outside the project
//   directory, but for removing, on a failure, the directories it made on
//   the way to it. `path` is followed as given, links and all, once, up to the
//   first directory the save makes; past that every directory is opened
//   once, as the save first reaches it (never through a link), and
//   everything after, the rollback's included, goes through what it opened.
//   So it all happens in the directories the save found or made, wherever
//   they are moved and whatever is linked in their place meanwhile.
// - It never deletes a file it didn't make or that isn't a stale project
//   entry (a file under the directory, not hidden, that the project doesn't
//   hold). A hidden name it didn't make is never touched, so a set-aside
//   file it couldn't remove once it was done stays, hidden, where a read
//   doesn't look. A rename never replaces a file unless it is the one the
//   project's entry replaces: renameat2's RENAME_NOREPLACE, else a hard
//   link then an unlink, and on a disk with neither the rename, and so the
//   save, is refused. A directory the save made is removed only while it is
//   empty and still the one it made (its device and inode as made, looked
//   at just before; one put in its place in between, or before the save
//   could look at the one it made, can be removed only if it is empty).
// - A save that fails for any reason, I/O errors and running out of
//   descriptors or memory included, leaves the directory as it found it:
//   each change (a directory made, a temporary, a file set aside, a second
//   name a file is left under when the move aside can't remove its first)
//   is recorded before it is made, or with nothing that can fail between,
//   so none can be missed, and what was set aside goes back, each file
//   replaced gets its old bytes again, and what the save added is removed,
//   the directories it made too, its parents included. A file is replaced only once its old bytes are read; one that
//   can't be read, or that isn't a file, refuses the save before anything is
//   replaced. A descriptor is held from before the first file is replaced
//   and let go of first by a rollback, which needs one at a time. Only a
//   step of the rollback itself failing (writing a file's old bytes back, a
//   set-aside file that can't go back without replacing something, a
//   second name, a temporary or a directory the save made that can't be
//   looked at or removed) leaves anything otherwise, and the error then names each such
//   file or directory and where a file's bytes are: a ProjectError, whatever
//   the save failed with (std::bad_alloc, say, is named in it). A failure
//   the rollback leaves nothing after is thrown as it was.
// - Running out of memory, at any allocation, changes none of this. Every
//   descriptor and libzip archive or entry the save (or a read) opens is
//   owned from the call that returns it, so none is left open. The rollback
//   allocates nothing unless a step fails, takes every step whatever the
//   others did, and only then names what it left, each as far as memory
//   lets: one it can't name is counted in the error instead.
// Out of scope: anyone else writing in the project directory while the save
// runs, another save included (saves aren't locked against each other), and
// systems other than POSIX, where all of this is done by path, looking
// before each step, and what changes between the look and the step isn't
// seen.
void writeDirectory(const Project& project, const std::filesystem::path& path);

namespace testing {
// Called at each step below with the path it is about. Tests only.
// - "read", every platform: a directory read has taken the size of the file
//   `at` from its walk and is about to open it.
// - "reading", POSIX systems only: the read has opened `at`, found it a
//   plain file of the size the walk saw, and is about to read it. The other
//   systems' reader checks the size only as it reads, and has no such step.
// - "wrote", every platform: inside a directory save's write of the file
//   `at`, straight after the rename that puts it in place, before the save
//   looks at `at` again.
// - "scan", every platform: a directory save has written the manifest and
//   is about to look in the directory `at` for files the project no longer
//   holds.
inline std::function<void(std::string_view step, const std::filesystem::path& at)> directoryStep;
// Called before some system calls a directory save makes, POSIX systems
// only, with the call and the path it acts on. Tests only. A result other
// than 0 is an errno the call fails with, and the call isn't made.
// - "renameat2", Linux: renaming a file to `at` unless something is there.
// - "noreplace": renameat2 can't refuse to replace there (or the system
//   has none), and the save is about to move a file to `at` without
//   replacing what is there.
// - "linkat": that move giving the file the second name `at`; if this
//   fails, the move is refused.
// - "unlinkat": removing the file or empty directory `at`, the old name of
//   that move and, where that fails, its second name included.
// - "fstatat": looking at the directory `at` the save made, for its device
//   and inode: just after making it, and before removing it on a failure.
// - "mkdirat": making the directory `at`, which may already be there.
// - "openat": creating the temporary file `at`, the save's or, on a
//   failure, the one holding a replaced file's old bytes.
inline std::function<int(std::string_view call, const std::filesystem::path& at)> systemCall;
} // namespace testing

} // namespace bw::project
