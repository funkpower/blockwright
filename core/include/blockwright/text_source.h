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
// bundledFace). Other sources (TheDraw fonts, kept in the project) are kinds
// to come, each with the fields that name its font.
struct FaceRef {
    enum class Kind : std::uint8_t { Pixel };
    Kind kind = Kind::Pixel;
    std::string name = "block";  // a pixel face's name

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

// Stable names for the project format (layers/<id>.json): pixel; as-typed,
// capitals; left, centre, right.
std::string_view faceKindName(FaceRef::Kind kind);
std::optional<FaceRef::Kind> faceKindFromName(std::string_view name);
std::string_view textCaseName(TextCase c);
std::optional<TextCase> textCaseFromName(std::string_view name);
std::string_view textAlignName(TextAlign a);
std::optional<TextAlign> textAlignFromName(std::string_view name);

} // namespace bw
