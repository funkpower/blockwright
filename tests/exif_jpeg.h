#pragma once
// Hand-built EXIF for JPEG tests (#422): APP1 segments spliced into a
// picture the tests made, so the replay's header reading and the editor's
// Qt reader can be given the same bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exif_jpeg {

// One IFD entry: tag, type, count and the first 16 bits of the value.
struct Entry {
    unsigned tag = 0x0112;
    unsigned type = 3;  // SHORT
    std::uint32_t count = 1;
    unsigned value = 1;
};

// A TIFF header in either byte order whose IFD0 at offset 8 holds `ifd0`,
// linked to an IFD1 holding `ifd1` when that isn't empty, or to `next`.
inline std::string tiff(bool little, const std::vector<Entry>& ifd0, const std::vector<Entry>& ifd1 = {},
                        std::uint32_t first = 8, std::uint32_t next = 0) {
    std::string b = little ? "II" : "MM";
    const auto u16 = [&](unsigned v) {
        const char lo = char(v & 0xFF), hi = char((v >> 8) & 0xFF);
        b += little ? std::string{lo, hi} : std::string{hi, lo};
    };
    const auto u32 = [&](std::uint32_t v) {
        if (little) {
            u16(v & 0xFFFF);
            u16(v >> 16);
        } else {
            u16(v >> 16);
            u16(v & 0xFFFF);
        }
    };
    const auto ifd = [&](const std::vector<Entry>& entries, std::uint32_t link) {
        u16(unsigned(entries.size()));
        for (const Entry& e : entries) {
            u16(e.tag);
            u16(e.type);
            u32(e.count);
            u16(e.value);
            u16(0);
        }
        u32(link);
    };
    u16(0x2A);
    u32(first);
    const std::uint32_t second = std::uint32_t(8 + 2 + 12 * ifd0.size() + 4);
    ifd(ifd0, ifd1.empty() ? next : second);
    if (!ifd1.empty())
        ifd(ifd1, 0);
    return b;
}

// "Exif\0\0" and a TIFF header whose IFD0 holds only Orientation.
inline std::string exif(bool little, unsigned orientation) {
    return std::string("Exif\0\0", 6) + tiff(little, {Entry{0x0112, 3, 1, orientation}});
}

// An APP1 segment carrying `payload`.
inline std::string app1(std::string_view payload) {
    const std::size_t len = payload.size() + 2;
    return std::string{char(0xFF), char(0xE1), char((len >> 8) & 0xFF), char(len & 0xFF)} + std::string(payload);
}

// `jpeg` with `segments` right after its SOI marker.
inline std::string afterSoi(std::string jpeg, std::string_view segments) {
    return jpeg.insert(2, segments);
}

// `jpeg` with `segments` right after its first SOF0 segment.
inline std::string afterSof(std::string jpeg, std::string_view segments) {
    const std::size_t sof = jpeg.find("\xFF\xC0");
    if (sof == std::string::npos || sof + 4 > jpeg.size())
        return jpeg;
    const std::size_t len = (std::size_t(static_cast<unsigned char>(jpeg[sof + 2])) << 8) |
                            static_cast<unsigned char>(jpeg[sof + 3]);
    return jpeg.insert(sof + 2 + len, segments);
}

// A case of EXIF and whether the editor turns the picture a quarter.
struct Case {
    const char* what;
    std::string (*make)(const std::string& jpeg);
    bool turned;
};

// EXIF Qt's JPEG reader does and doesn't turn by, read the way it reads it:
// its APP1 segments joined, IFDs followed, Orientation counted only as one
// SHORT from 1 to 8.
inline std::vector<Case> oddCases() {
    using std::string;
    return {
        {"cut short", [](const string& j) { return afterSoi(j, app1(exif(false, 6).substr(0, 20))); }, false},
        {"no byte order", [](const string& j) {
             string e = exif(false, 6);
             e[6] = e[7] = 'X';
             return afterSoi(j, app1(e));
         }, false},
        {"IFD0 past the end", [](const string& j) {
             return afterSoi(j, app1(string("Exif\0\0", 6) + tiff(false, {Entry{}}, {}, 0xFFFFFFF0u)));
         }, false},
        {"a LONG", [](const string& j) {
             return afterSoi(j, app1(string("Exif\0\0", 6) + tiff(true, {Entry{0x0112, 4, 1, 6}})));
         }, false},
        {"two values", [](const string& j) {
             return afterSoi(j, app1(string("Exif\0\0", 6) + tiff(false, {Entry{0x0112, 3, 2, 6}})));
         }, false},
        {"orientation 0", [](const string& j) { return afterSoi(j, app1(exif(false, 0))); }, false},
        {"orientation 9", [](const string& j) { return afterSoi(j, app1(exif(true, 9))); }, false},
        {"an IFD linked to itself", [](const string& j) {
             return afterSoi(j, app1(string("Exif\0\0", 6) + tiff(false, {Entry{0x0100, 3, 1, 4}}, {}, 8, 8)));
         }, false},
        {"65535 entries promised", [](const string& j) {
             string e = exif(false, 6);
             e[14] = e[15] = char(0xFF);
             e[17] = 0;  // the one entry there is tag 0x0100, not Orientation
             return afterSoi(j, app1(e.substr(0, 30)));
         }, false},
        {"XMP before the EXIF", [](const string& j) {
             return afterSoi(j, app1(string("http://ns.adobe.com/xap/1.0/\0<x/>", 33)) + app1(exif(false, 6)));
         }, false},
        {"in IFD1", [](const string& j) {
             return afterSoi(j, app1(string("Exif\0\0", 6) + tiff(true, {Entry{0x0100, 3, 1, 4}}, {Entry{0x0112, 3, 1, 6}})));
         }, true},
        {"split over two APP1s", [](const string& j) {
             const string e = exif(false, 8);
             return afterSoi(j, app1(e.substr(0, 20)) + app1(e.substr(20)));
         }, true},
        {"EXIF then XMP", [](const string& j) {
             return afterSoi(j, app1(exif(true, 6)) + app1(string("http://ns.adobe.com/xap/1.0/\0<x/>", 33)));
         }, true},
        {"after the start of frame", [](const string& j) { return afterSof(j, app1(exif(false, 6))); }, true},
        {"IFD0 linked back to an earlier IFD", [](const string& j) {
             // IFD0 at 26 (no Orientation) links back to an IFD at 8 holding 6;
             // Qt's reader refuses to seek backwards
             string t = tiff(false, {Entry{0x0112, 3, 1, 6}});
             t.replace(4, 4, string("\0\0\0\x1a", 4));
             t += string("\0\x01\x01\0\0\x03\0\0\0\x01\0\x04\0\0\0\0\0\x08", 18);
             return afterSoi(j, app1(string("Exif\0\0", 6) + t));
         }, false},
        {"IFD0 inside the TIFF header", [](const string& j) {
             // IFD0 at 4: its count is read from the offset's own bytes (4),
             // and the entry at 18 is Orientation 6; Qt's reader refuses an
             // IFD that starts before where it stands (8)
             string t = string("II\x2a\0\x04\0\0\0", 8) + string(10, '\0') + string("\x12\x01\x03\0\x01\0\0\0\x06\0\0\0", 12) +
                        string(28, '\0');
             return afterSoi(j, app1(string("Exif\0\0", 6) + t));
         }, false},
    };
}

}  // namespace exif_jpeg
