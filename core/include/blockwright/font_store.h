#pragma once
// TheDraw font files for text layers (PLAN §15): a store of .tdf files keyed
// by the sha256 of their bytes, which a FaceRef of kind Tdf names, and the
// fonts folder's catalogue.
//
// A text layer set in a TheDraw font names the file by its hash and the font
// in it by its index and name. Its face is looked up in the project's own
// copies of the files first (project.h: fonts/<sha256>.tdf), then in the
// fonts folder; the copies are what let a project open, and its text be
// edited, where the folder doesn't have the font.

#include "blockwright/face.h"
#include "blockwright/tdf.h"
#include "blockwright/text_source.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bw {

// The sha256 of `bytes`, in 64 lowercase hex digits.
std::string sha256Hex(std::string_view bytes);
// Whether `s` is 64 lowercase hex digits.
bool isSha256Hex(std::string_view s);

// A .tdf file as read: its bytes, their hash and its fonts' names, in file
// order. The fonts' faces (tdfFace: confusables screened) are made the
// first time one is asked for, all of the file's at once, so a file no
// layer or picker draws from costs its bytes alone; readTdf's kMaxTdfCells
// bounds what they hold.
struct FontFile {
    // A file of `bytes` holding the fonts `names`. loadFontFile makes one
    // from bytes it has read; this one checks nothing.
    FontFile(std::string bytes, std::vector<std::string> names);
    FontFile(const FontFile&) = delete;
    FontFile& operator=(const FontFile&) = delete;

    std::string sha256;
    std::string bytes;
    std::vector<std::string> names;  // each font's name, as face(i)->name()

    // The face of font `i`, made with the rest of the file's on the first
    // call (safe from any thread); null past the fonts, or when the bytes
    // don't read as names says.
    const Face* face(std::size_t i) const;
    // Whether the faces have been made. Tests only.
    bool facesMade() const;

private:
    mutable std::once_flag made_;
    mutable std::atomic<bool> madeFlag_{false};
    mutable std::vector<std::unique_ptr<Face>> faces_;
};
using FontFilePtr = std::shared_ptr<const FontFile>;

// The file of `bytes`, read (readTdf) for its fonts' names; their faces are
// made when asked for. Throws TdfError when the bytes are over
// kMaxTdfBytes, when readTdf throws, or when the file holds no font.
FontFilePtr loadFontFile(std::string bytes);

// Font files by hash. Copies share the files.
class FontStore {
public:
    // Adds the file (a file already held is kept); returns it as held.
    FontFilePtr add(FontFilePtr file);
    // The file with this hash, or null.
    FontFilePtr file(std::string_view sha256) const;
    // The face a ref of kind Tdf names: the file's font at its index, when
    // that font has the ref's name; otherwise (or for another kind) null.
    const Face* face(const FaceRef& ref) const;
    const std::map<std::string, FontFilePtr, std::less<>>& files() const { return files_; }
    bool empty() const { return files_.empty(); }

    // Equal when they hold the same files (the hash names the bytes).
    bool operator==(const FontStore& other) const;

private:
    std::map<std::string, FontFilePtr, std::less<>> files_;
};

// Where a TheDraw face is looked up: the project's copies first, then the
// fonts folder, then files a caller holds on to (the Text tool keeps the
// file of the face new text starts in). Any may be missing.
struct FontSources {
    const FontStore* project = nullptr;
    const FontStore* folder = nullptr;
    const FontStore* held = nullptr;

    // The face from the first that has it, or null.
    const Face* face(const FaceRef& ref) const;
    // The file from the first that has it, or null.
    FontFilePtr file(std::string_view sha256) const;
};

// A face a picker offers: what a text source names, what the list shows,
// and the face itself.
struct FaceEntry {
    FaceRef ref;
    std::string displayName;
    const Face* face = nullptr;
};

// The fonts folder, scanned: every .tdf file under it (the extension in any
// case, subfolders too) read as readTdfFile reads it, its fonts screened. A file
// that can't be read is listed in `rejected` with the reason, never thrown.
struct FontCatalogue {
    struct Font {
        FaceRef ref;
        std::string displayName;  // "NAME (file.tdf)"
        std::filesystem::path file;
        const Face* face = nullptr;
    };
    struct Rejected {
        std::filesystem::path file;
        std::string reason;
    };

    std::filesystem::path folder;
    FontStore store;
    std::vector<Font> fonts;  // by file path, then file order
    std::vector<Rejected> rejected;

    std::vector<FaceEntry> entries() const;
};

// The catalogue of `folder`; an empty one when the folder is empty or isn't
// a directory, with a rejected entry for the folder when it can't be read.
FontCatalogue scanFontsFolder(const std::filesystem::path& folder);

} // namespace bw
