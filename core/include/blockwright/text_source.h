#pragma once
// A text layer's source (PLAN §15): the text and the Text tool's settings,
// kept in the layer's properties (LayerProperties::text) and redrawn into
// its cells (text.h) whenever one changes. Only the types live here, so
// layer.h can hold one without the faces.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bw {

// The face a text layer is set in. A pixel face is named (face.h's
// bundledFace). A TheDraw font is the font at `index` in the .tdf file whose
// bytes hash to `file` (sha256, 64 lowercase hex digits), and `name` is that
// font's name, checked against it (font_store.h).
struct FaceRef {
    enum class Kind : std::uint8_t { Pixel, Tdf };
    Kind kind = Kind::Pixel;
    std::string name = "block";  // the face's or font's name
    std::string file;            // Tdf: the file's sha256
    int index = 0;               // Tdf: the font's place in the file

    bool operator==(const FaceRef&) const = default;
};

// Case: the text as typed, or in capitals (a-z as A-Z).
enum class TextCase : std::uint8_t { AsTyped, Capitals };
// Each line's place in the text's box: its left edge, middle (an odd
// column left over goes on the right) or right edge.
enum class TextAlign : std::uint8_t { Left, Centre, Right };

struct TextSource {
    std::u32string text;  // at most kMaxLength characters
    FaceRef face;
    TextCase textCase = TextCase::AsTyped;
    // Blank columns between letters on top of the face's own, and blank cell
    // rows between lines; 0 to kMaxSpacing.
    int letterSpacing = 0;
    int lineSpacing = 0;
    TextAlign align = TextAlign::Left;
    // Wrap width in columns, 0 to kMaxWrapWidth; 0 doesn't wrap.
    int wrapWidth = 0;
    // The letters' colour, a palette index: what they show until a text
    // style's Fill colours them.
    std::uint8_t colour = 7;

    static constexpr std::size_t kMaxLength = 4096;
    static constexpr int kMaxSpacing = 64;
    static constexpr int kMaxWrapWidth = 65535;

    bool operator==(const TextSource&) const = default;
};

// Stable names for the project format (layers/<id>.json): pixel, tdf; as-typed,
// capitals; left, centre, right.
std::string_view faceKindName(FaceRef::Kind kind);
std::optional<FaceRef::Kind> faceKindFromName(std::string_view name);
std::string_view textCaseName(TextCase c);
std::optional<TextCase> textCaseFromName(std::string_view name);
std::string_view textAlignName(TextAlign a);
std::optional<TextAlign> textAlignFromName(std::string_view name);

} // namespace bw
