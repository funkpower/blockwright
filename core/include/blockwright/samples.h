#pragma once
// The sample projects (PLAN §12): layered pieces that come with the app,
// opened from Help > Samples and the start screen as untitled copies.
//
// Each is a directory project, samples/<name>/ in the source, installed as
// it is to <datadir>/blockwright/samples/<name>/. Each one's notes.md says
// what it is and how it is built:
//
//   # <title>                      the name the app lists it by
//
//   <summary>                      the first paragraph: one line in the list
//
//   ## Layers                      one row a layer, top first:
//   | Layer | Kind | Mode | Locks | What it does |
//   |---|---|---|---|---|
//   | title | text | normal | - | ... |
//
//   ## Variants                    other looks the same layers make:
//   | Variant | Changes |
//   |---|---|
//   | Day | show day sky; hide stars |
//
// A variant's changes are separated by ';', each one of
//   show <layer>, hide <layer>,
//   raise <layer> above <layer>, lower <layer> below <layer>
// by layer name. A name may hold spaces, and the word a change splits at:
// "raise fog above above day sky" splits at the " above " whose two sides
// both name layers of the stack, and a change that could split two ways is
// refused. The piece as saved is the variant "As saved", with no changes;
// the table lists only the others.

#include "blockwright/layer.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw::samples {

class SampleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A row of the notes' layer table, as written.
struct LayerRow {
    std::string name, kind, mode, locks, does;
    bool operator==(const LayerRow&) const = default;
};

struct Variant {
    std::string name;
    std::vector<std::string> changes;  // each trimmed, none empty
    bool operator==(const Variant&) const = default;
};

struct Notes {
    std::string title;    // the first "# " heading, trimmed; empty when none
    std::string summary;  // the first paragraph after it, its lines joined with spaces
    std::vector<LayerRow> layers;    // top first
    std::vector<Variant> variants;   // the table's, without "As saved"
    bool operator==(const Notes&) const = default;
};

// The notes as above. A table's header row and its |---| row are skipped;
// a row with fewer cells than the header has them empty. Throws SampleError
// when a table's header has fewer columns than the format's (five for
// Layers, two for Variants), so list() shows that sample as one that can't
// be read.
Notes parseNotes(std::string_view markdown);

// The variant's changes made to the stack, in order. Throws SampleError for
// a change it can't read, a layer name the stack doesn't hold, or holds
// twice, or a raise or lower that splits two ways, leaving the stack as the
// changes before it left it.
void applyVariant(LayerStack& stack, const Variant& variant);

struct Sample {
    std::filesystem::path path;  // the .bwp file or project directory
    std::string title;           // the notes' title, or the file's name without .bwp
    std::string summary;
    std::string problem;         // why it can't be read; empty when it can
};

// The samples in `dir`: each .bwp file and each project directory in it,
// by file name (so a "01-" prefix orders them), hidden ones skipped. A
// sample that can't be read is listed with its problem. A directory that
// isn't there, or can't be read, has none.
std::vector<Sample> list(const std::filesystem::path& dir);

// Where the bundled samples are: $BLOCKWRIGHT_SAMPLES when it is set and
// not empty, else <datadir>/blockwright/samples/ as found from the directory
// holding the executable (library::realBinDir), as the bundled Library is.
std::filesystem::path bundledDir(const std::filesystem::path& binDir);

} // namespace bw::samples
