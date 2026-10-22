#pragma once
// SAUCE 00 records (https://www.acid.org/info/sauce/sauce.htm): the 128-byte
// trailer after the art, with an optional COMNT block before it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bw {

struct Sauce {
    static constexpr int kRecordSize = 128;
    static constexpr size_t kTitleLen = 35, kAuthorLen = 20, kGroupLen = 20;
    static constexpr size_t kCommentLen = 64, kMaxComments = 255, kFontLen = 22;

    // Text fields hold raw CP437 bytes without their space/NUL padding.
    std::string title, author, group;
    std::string date;              // CCYYMMDD
    std::uint32_t fileSize = 0;    // as read; written as the real content length
    std::uint8_t dataType = 1;     // 1 = character
    std::uint8_t fileType = 1;     // 1 = ANSi
    std::uint16_t tinfo1 = 0, tinfo2 = 0, tinfo3 = 0, tinfo4 = 0;  // width, height for ANSi
    std::uint8_t flags = 0;
    std::string font = "IBM VGA";  // TInfoS
    std::vector<std::string> comments;

    // TFlags bit 0: iCE colours (blink = bright background).
    bool ice() const { return flags & 0x01; }
    void setIce(bool on) { flags = static_cast<std::uint8_t>(on ? flags | 0x01 : flags & ~0x01); }
    // TFlags bits 1-2: 0 none, 1 = 8 px, 2 = 9 px letter spacing.
    int letterSpacing() const { return (flags >> 1) & 3; }
    void setLetterSpacing(int v) { flags = static_cast<std::uint8_t>((flags & ~0x06) | ((v & 3) << 1)); }
    // TFlags bits 3-4: 0 none, 1 = legacy (stretched) aspect, 2 = square pixels.
    int aspectRatio() const { return (flags >> 3) & 3; }
    void setAspectRatio(int v) { flags = static_cast<std::uint8_t>((flags & ~0x18) | ((v & 3) << 3)); }

    bool isCharacterArt() const { return dataType == 1; }

    // A record for new art: today's date, ANSi, iCE on, IBM VGA.
    static Sauce fresh(int width, int height);
};

struct SauceSplit {
    std::string_view content;    // the art: before the SAUCE/COMNT and the first EOF (0x1A)
    std::optional<Sauce> sauce;
};

// Splits a file into its content and SAUCE record. The content ends at the
// first 0x1A before the record; FileSize is read but never used to cut the
// content, so a file whose FileSize is 0 (or wrong) still loads.
SauceSplit splitSauce(std::string_view file);

// "\x1A" + optional COMNT block + the 128-byte record. Text fields are cut to
// their SAUCE lengths and space-padded; FileSize is set to contentSize.
std::string encodeSauce(const Sauce& s, std::uint32_t contentSize);

} // namespace bw
