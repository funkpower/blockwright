#pragma once
// Projects (PLAN §6): a layer stack with its format, notes, snapshots and the
// files kept with it, saved as one zip file or as a directory of the same
// layout (for git and other tools):
//
//   manifest.json      "format": "blockwright", "version": "1.3", the canvas
//                      size, palette, iCE, IRC budget, the flat .ans form's
//                      text encoding (only when UTF-8), SAUCE fields (font and
//                      letter spacing among them), the layer order (ids,
//                      bottom first) and the next layer id
//   layers/<id>.json   the layer's properties (LayerProperties, its size;
//                      a live text layer's source, text.h; a text style,
//                      text_style.h)
//   layers/<id>.cells  its cells (encodeCells: a versioned header, zstd)
//   fonts/<sha256>.tdf a TheDraw font file a text layer is set in, named by
//                      the sha256 of its bytes (lowercase hex)
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
//
// A source set in a TheDraw font (1.2) is the layer's "tdfSource" rather
// than its "source", and its font file is copied in once under fonts/ for
// every layer that uses it, so the project opens, and its text is edited,
// with the project's copy wherever it goes. A 1.1 reader, which refuses a
// face kind it doesn't know under "source", drops the unknown key and shows
// the saved cells, and keeps the copies as files it doesn't know. A copy
// whose bytes don't hash to its name, that doesn't read as a TheDraw file,
// or that a layer names and the project lacks is refused by the reader.
//
// A text style (1.3) is kept as its effects in the stack's order. A live
// text layer's style is drawn into its cells, so a 1.2 reader drops it and
// shows the same cells. A cells or pixels layer's style is drawn when the
// stack is composited, so a reader that dropped it would show the layer
// unstyled: a project with one is written as 2.0, which a 1.x reader
// refuses, and a 1.x project holding one is refused. A style this build
// can't draw (more than one effect of a kind, a ramp it doesn't have, a
// setting out of range, over TextStyle::kMaxEffects effects, checked before
// any is read) or one on a layer that can't have one is refused, by the
// reader and the writer.
//
// A reference layer's picture (1.5; 2.1 in a styled project) is the file
// refs/<sha256>.png or .jpg, named by the sha256 of its bytes and written
// once however many layers show it, and the layer's "reference" key: the
// file's path, the picture's place and size in canvas pixels and its
// opacity. A reader refuses a file whose bytes don't hash to its name or
// don't start as its type says, a path the project lacks, and the key on a
// layer that isn't a reference layer. A 1.3 reader knows the kind, which
// the compositor skips, so it shows the same art; it drops the key and
// keeps the picture as a file it doesn't know, so a project it saves opens
// here with an empty reference layer and the picture under refs/.

#include "blockwright/ansi.h"
#include "blockwright/font_store.h"
#include "blockwright/irc.h"
#include "blockwright/layer.h"
#include "blockwright/target.h"

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
inline constexpr int kFormatMinor = 3;
// A live whole-cell big text source, skipped by older readers as saved cells.
inline constexpr int kBigTextFormatMinor = 4;
// The version of a project with a cells or pixels layer that has a style.
inline constexpr int kStyledFormatMajor = 2;
inline constexpr int kStyledFormatMinor = 0;
// The minor versions of a project with a reference layer that has a picture
// (refs/<sha256>.png or .jpg, the layer's "reference" key; #178): 1.5, or
// 2.1 when it is styled as well. 1.5 implies 1.4's big text sources.
inline constexpr int kReferenceFormatMinor = 5;
inline constexpr int kReferenceStyledFormatMinor = 1;
// A live Big rainbow source (the layer's "bigRainbowSource"): 1.6, or 2.2
// when styled as well. Each implies the minors before it.
inline constexpr int kRainbowFormatMinor = 6;
inline constexpr int kRainbowStyledFormatMinor = 2;
// A layer with cells of the ramp gradient's dither (the layer's "dither",
// Cell::dithered): 1.7, or 2.3 when styled as well. Each implies the minors
// before it. An older reader drops the marks, so the lint calls those cells
// specks again.
inline constexpr int kDitherFormatMinor = 7;
inline constexpr int kDitherStyledFormatMinor = 3;
// A Glow whose brightness isn't its width (the effect's "brightness", #184)
// on a live text layer, whose saved cells already show it: 1.8, or 2.4 when
// styled as well. Each implies the minors before it. An older reader drops
// the key and lights the ring next to the letters at the width's rung.
inline constexpr int kGlowFormatMinor = 8;
inline constexpr int kGlowStyledFormatMinor = 4;
// Any Glow on a cells or pixels layer, which the compositor draws, switched
// off too and whatever its brightness: a 2.x reader would drop the key and
// show the ring next to the letters at the width's rung, and leave a counter
// to fade darker inside the inner ring. 3.0, which a 2.x reader refuses. It
// implies every 2.x minor. A 2.x file from before 3.0 with a Glow there and
// no brightness (or the width's) still opens, its counters now filled.
inline constexpr int kGlowCellsFormatMajor = 3;
inline constexpr int kGlowCellsFormatMinor = 0;
// A layer that holds a wide pair or a cluster (PLAN §16), whose .cells is
// version 2: an older reader would refuse the layer or, reading it, show
// other columns, so the project is 4.0, which a 3.x reader refuses. It
// implies every 3.x and 2.x minor.
inline constexpr int kWideFormatMajor = 4;
inline constexpr int kWideFormatMinor = 0;
// A layer that holds a cell with bold, italic or underline (attr::Formatting,
// #350): a 4.x reader would keep the bits but neither draw nor export them,
// so the project is 5.0, which a 4.x reader refuses. It implies every 4.x
// minor and everything before. No build before 5.0 wrote a bit but blink, so
// a reader clears every other bit in a project below 5.0 and counts the
// cells (ReadNotes), and refuses a bit outside attr::Known in 5.0 or later.
inline constexpr int kFormattingFormatMajor = 5;
inline constexpr int kFormattingFormatMinor = 0;

// Limits a reader holds a file to, and a writer checks before it writes
// anything: no more than kMaxEntries entries, none over kMaxEntryBytes, all
// of them together under kMaxTotalBytes; no more than kMaxLayers layers,
// none over kMaxLayerCells cells or 65,535 a side, all of them together no
// more than kMaxTotalCells cells (about 256 MiB decoded, checked from the
// .cells headers before any layer is decoded); manifest.json and each
// layers/<id>.json no more than kMaxJsonBytes, checked before it is parsed;
// no more than kMaxFontFiles font copies (fonts/<sha256>.tdf), each within
// tdf.h's kMaxTdfBytes, counted before any is read (each can make up to
// kMaxTdfCells cells of glyphs); a canvas within ansi.h's kMaxColumns by
// kMaxRows; SAUCE text within the record's fields (Sauce::kTitleLen and the
// rest, a date of 8); no more than kMaxTableCodePoints code points in the
// layers' cluster tables together, counted from what each .cells frame
// declares before any is decoded (encodeCells).
inline constexpr std::size_t kMaxEntries = 10'000;
inline constexpr std::size_t kMaxEntryBytes = std::size_t{256} << 20;
inline constexpr std::size_t kMaxTotalBytes = std::size_t{1} << 30;
inline constexpr std::size_t kMaxLayers = 1024;
inline constexpr std::size_t kMaxLayerCells = std::size_t{kMaxColumns} * kMaxRows;
inline constexpr std::size_t kMaxTotalCells = 8 * kMaxLayerCells;
inline constexpr std::size_t kMaxJsonBytes = std::size_t{1} << 20;
inline constexpr std::size_t kMaxFontFiles = 16;
inline constexpr std::size_t kMaxTableCodePoints = 4'000'000;

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
    // The text encoding of the project's flat .ans form (PLAN §8): what the
    // .ans it was made from was read as (fromAnsi), or what the app last
    // saved it in. The app's Save As .ans and snapshots (takeSnapshot) write
    // it unless told otherwise; the CLI's convert doesn't follow it (its
    // .ans is CP437 unless an option says). The manifest's
    // "ansEncoding", "utf-8" or "cp437", written only when UTF-8; a project
    // without it is CP437.
    TextEncoding ansEncoding = TextEncoding::Cp437;
    // What the project is drawn for (#350): ANSI (an .ans, 16 colours) or
    // IRC (mIRC's 99 colours too). The manifest's "target", "irc" or "ansi",
    // written only for IRC; a project without it, or with a value this build
    // doesn't know, is ANSI. Like ansEncoding it is a preference, not art.
    Target target = Target::Ansi;
    // The mIRC colours IRC text of the project carries under the IRC target
    // (#496): all 99, or 0-15 only. The manifest's "ircColours", written only
    // as 16; a project without it, or with any other number, is 99, and a
    // value that isn't a number is refused. A preference like target: kept
    // under ANSI and Micron, where it does nothing.
    IrcColours ircColours = IrcColours::Mirc99;
    // The 16 colours its indexed cells show (PLAN §8, Palettes): nothing for
    // the standard kPalette. The manifest's "palette": "cga" for none, or a
    // list of 16 "#rrggbb" strings, written only for a palette other than
    // the standard one. A build before #497 refuses a list ("this build has
    // only cga"), as it should, since the cells' indices then mean other
    // colours; so the list needs no version of its own.
    std::optional<Palette> palette;
    IrcBudget ircBudget;
    std::string notes;               // notes.md
    std::vector<Snapshot> snapshots;  // by round, first first
    // Every other entry by its path in the project ('/'-separated, relative):
    // refs/…, scripts/<id>/…, thumbnail.png, and whatever a newer minor
    // version added. Kept byte for byte.
    std::map<std::string, std::string> files;
    // The TheDraw font files its text layers may be set in (fonts/…): what
    // a layer's TheDraw face is drawn from. A save writes the ones a layer
    // uses; a file none uses isn't written.
    FontStore fonts;

    bool operator==(const Project&) const = default;
};

// A flat file as a project: one Background layer (LayerStack::fromCanvas),
// its SAUCE, iCE and text encoding (ansEncoding), opened as the editor opens it (openAsEditor, whose
// result goes to `opened`), so toAnsi writes it back as writeAnsi does.
Project fromAnsi(const AnsiDocument& doc, Opened* opened = nullptr);
// The project flattened through the compositor (bw::flatten) with its iCE
// and SAUCE fields (and ansEncoding as the document's encoding), each cell as it shows with the project's iCE (iceCell),
// as the editor saves: what exports and snapshots write with writeAnsi.
AnsiDocument toAnsi(const Project& project);

// Flattens the project to .ans and keeps it as the next round (one past the
// last, r1 for the first), in `encoding`, or with none in the project's
// ansEncoding, so a UTF-8 piece's rounds keep its wide glyphs and clusters
// (#336). Returns what writeAnsi wrote, so a caller can report a lossy
// snapshot (glyphs written as '?', blink dropped).
AnsiOutput takeSnapshot(Project& project, std::optional<TextEncoding> encoding = std::nullopt);

// Why the writer would refuse a layer's reference image (a picture that is
// not a PNG or JPEG, or too big; a place, size or opacity out of range; one
// on a layer that is not a reference layer), or nullopt when it would not.
std::optional<std::string> layerReferenceProblem(const LayerProperties& props);

// A layer's cells as layers/<id>.cells holds them: "BWCL", the format
// version (u16), reserved (u16, 0), width and height (u32), then one zstd
// frame of width × height 14-byte cells, row by row: the code point (u32),
// foreground and background (u8 kind, 0 a palette index then the index and
// two zeros, 1 RGB then r, g, b), attributes (u8) and flags (u8, bit 0
// transparent). All little-endian. Version 1 is written for a layer of code
// points only, byte for byte as before wide glyphs (PLAN §16). Version 2 is
// written for a layer that holds a continuation or a cluster: flags bit 1
// marks a continuation (code point field 0, the colours, attributes and
// transparency of the lead to its left, never in column 0 or right of
// another continuation; its dither mark, kept in the layer's JSON, is the
// lead's too) and bit 2 a cluster (the code point
// field indexes the table); after the cells, in the same frame, the cluster
// table: u32 count, then for each u8 n (2-16) and n u32 code points, in the
// order the cells first use them, no two alike, each used, each one a
// cluster may hold (clusterMayHold). decodeCells throws ProjectError for
// anything else: another magic or version, a size over the limits, a frame
// that isn't exactly the cells (and table), a code point past U+10FFFF, an
// unknown colour kind, flag or attribute bit (outside attr::Known), a version 2 layer with no continuation or
// cluster, a table that could hold over kMaxTableCodePoints code points
// (checked before decoding), and a pool that can't take the table (out of
// ids or memory; never its lossy cap, PoolCap::Uncapped). encodeCells
// refuses a canvas it would write so (a stray continuation, one in other
// values than its lead's, a table over the limit).
// The zstd frame a .cells file holds its cells in: compressed (zstd at level
// 3, what a save writes), or raw blocks, the cells as they are, in a frame
// that depends on no zstd version (a frame header with the content size, then
// blocks of at most 128 KiB, the last one flagged; bw-make-samples writes the
// samples so, the same bytes on any machine). decodeCells reads either.
enum class CellsFrame { Compressed, Raw };
std::string encodeCells(const Canvas& cells, CellsFrame frame = CellsFrame::Compressed);
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
// file over another, or a font copy it refuses (above). decode interns the
// layers' clusters only once all of that has passed, so a project it refuses
// adds nothing to the cluster pool. encode
// throws it for a path in `files` that is unsafe or that the format owns
// (manifest.json, notes.md, layers/<n>.json and .cells, snapshots/r<N>.ans,
// fonts/<sha256>.tdf), for a text source in a TheDraw font `fonts` doesn't
// hold, and for a project whose entries decode would refuse that way.
using Entries = std::map<std::string, std::string>;
Entries encode(const Project& project, CellsFrame cells = CellsFrame::Compressed);
// What decode changed to open a project, for a notice: the cells of a
// project before 5.0 whose stray attribute bits (all but blink) it cleared.
struct ReadNotes {
    int strayAttributes = 0;
};
Project decode(const Entries& entries, ReadNotes* notes = nullptr);

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
// damaged one is reported as such rather than read as something else). On
// POSIX systems the first bytes are read through a handle opened without
// waiting on a FIFO and checked by fstat, so a FIFO, device or socket at the
// path (which can't be opened) is not a project and is never waited on.
bool isProject(const std::filesystem::path& path);

// Reads a project from a zip file or a directory. Hidden entries in either
// are skipped; in a directory, whatever they are (a link, a pipe, a
// directory it can't open), and nothing under a hidden directory is looked
// at. Anything else that isn't a safe path (safeEntryPath), an entry
// repeated or over the limits is refused, as is any symbolic link or other
// entry that isn't a file or directory (in a zip, by its UNIX mode, hidden
// or not), and everything decode refuses. On POSIX systems a zip is opened
// once, without waiting on a FIFO, refused unless it is a regular file (a
// FIFO, device, socket or directory put at the path after the check for a
// directory: fstat says so, or for a socket the open fails), and read by
// libzip through that same descriptor, which stays close-on-exec.
Project read(const std::filesystem::path& path, ReadNotes* notes = nullptr);

// Both writers encode the project first, so one decode would refuse is
// refused before the destination is touched.
//
// Writes the project as one zip file, replacing `path` whole (libzip writes
// a temporary file beside it and renames it over). Entries are stored with
// a fixed time, so a project writes the same bytes every time; .cells and
// .png entries are stored, the rest deflated.
void writeZip(const Project& project, const std::filesystem::path& path);

// The bytes writeZip writes for the project, made in memory: for a caller
// that writes several files and wants each whole before it writes any.
std::string zipBytes(const Project& project);

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
void writeDirectory(const Project& project, const std::filesystem::path& path,
                    CellsFrame cells = CellsFrame::Compressed);

namespace testing {
// Called at each step below with the path it is about. Tests only.
// - "read", every platform: a directory read has taken the size of the file
//   `at` from its walk and is about to open it.
// - "reading", POSIX systems and Windows: the read has opened `at`, found it
//   a plain file of the size the walk saw, and is about to read it. The
//   other systems' reader checks the size only as it reads, and has no such
//   step.
// - "wrote", every platform: inside a directory save's write of the file
//   `at`, straight after the rename that puts it in place, before the save
//   looks at `at` again.
// - "scan", every platform: a directory save has written the manifest and
//   is about to look in the directory `at` for files the project no longer
//   holds.
// - "sniff", POSIX systems: isProject has found `at` not to be a directory
//   and is about to open it to read its first bytes.
// - "zip", POSIX systems: a read has found `at` not to be a directory and is
//   about to open it as a zip.
// - "zip opened", POSIX systems: that read has opened `at`, found it a
//   regular file by its handle, and is about to give the handle to libzip.
// - "zip reading", every platform: a read has opened the archive `at` and
//   is about to read its entries.
inline std::function<void(std::string_view step, const std::filesystem::path& at)> directoryStep;
// Called before some system calls a directory save makes, POSIX systems
// only, with the call and the path it acts on. Tests only. A result other
// than 0 is an errno the call fails with, and the call isn't made.
// - "renameat2", Linux and macOS: renaming a file to `at` unless something
//   is there (renameat2, or renameatx_np on macOS).
// - "noreplace": renameat2 can't refuse to replace there (or the system
//   has none, as on FreeBSD), and the save is about to move a file to `at`
//   without replacing what is there.
// - "linkat": that move giving the file the second name `at`; if this
//   fails, the move is refused.
// - "unlinkat": removing the file or empty directory `at`, the old name of
//   that move and, where that fails, its second name included.
// - "fstatat": looking at the directory `at` the save made, for its device
//   and inode: just after making it, and before removing it on a failure.
// - "mkdirat": making the directory `at`, which may already be there.
// - "openat": creating the temporary file `at`, the save's or, on a
//   failure, the one holding a replaced file's old bytes.
// - "open to read": opening the file `at`, which the save is about to
//   replace, to read its old bytes; a link there is refused by the open.
inline std::function<int(std::string_view call, const std::filesystem::path& at)> systemCall;
// The call above that moves a file to a name where nothing may be
// replaced: "renameat2" where the system has such a rename, else "linkat"
// (empty off POSIX systems). Tests only.
std::string_view noReplaceMove();
// Windows only: the longest path, in UTF-16 units with its terminator, a
// directory save writes; 0 for the system's own. Tests only.
inline std::size_t pathLimit = 0;
} // namespace testing

#ifdef _WIN32
// The longest path, in UTF-16 units with its terminator, a directory save
// writes: testing::pathLimit when it is set, else MAX_PATH, or 32767 where
// long paths are on.
std::size_t maxPathChars();
#endif

} // namespace bw::project
