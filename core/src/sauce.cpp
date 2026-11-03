#include "blockwright/sauce.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace bw {

namespace {

std::uint16_t le16(std::string_view s, size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(s[at]) |
                                      (static_cast<unsigned char>(s[at + 1]) << 8));
}

std::uint32_t le32(std::string_view s, size_t at) {
    return static_cast<std::uint32_t>(le16(s, at)) | (static_cast<std::uint32_t>(le16(s, at + 2)) << 16);
}

void put16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>(v >> 8);
}

void put32(std::string& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v & 0xFFFF));
    put16(out, static_cast<std::uint16_t>(v >> 16));
}

// A text field without its trailing padding (spaces, and the NULs some
// editors pad with instead).
std::string field(std::string_view rec, size_t at, size_t len) {
    std::string_view f = rec.substr(at, len);
    size_t end = f.find_last_not_of(std::string_view(" \0", 2));
    return std::string(end == std::string_view::npos ? std::string_view{} : f.substr(0, end + 1));
}

void putField(std::string& out, std::string_view value, size_t len, char pad) {
    std::string f(value.substr(0, len));
    f.resize(len, pad);
    out += f;
}

} // namespace

Sauce Sauce::fresh(int width, int height) {
    Sauce s;
    std::time_t now = std::time(nullptr);
    char buf[9] = {};
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(buf, sizeof buf, "%Y%m%d", &tm);
    s.date = buf;
    s.tinfo1 = static_cast<std::uint16_t>(width);
    s.tinfo2 = static_cast<std::uint16_t>(height);
    s.setIce(true);
    return s;
}

bool isSauceDate(std::string_view date) {
    if (date.size() != 8 || !std::all_of(date.begin(), date.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    const auto number = [&](std::size_t at, std::size_t len) {
        unsigned v = 0;
        for (std::size_t i = at; i < at + len; ++i)
            v = v * 10 + unsigned(date[i] - '0');
        return v;
    };
    // the calendar's years start at 0001, as QDate's (the app's SAUCE dialog) do
    const unsigned year = number(0, 4);
    const std::chrono::year_month_day day{std::chrono::year(int(year)), std::chrono::month(number(4, 2)),
                                          std::chrono::day(number(6, 2))};
    return year != 0 && day.ok();
}

std::optional<std::string> sauceDateFromEpoch(std::string_view seconds) {
    // 9999-12-31 23:59:59 UTC; more significant digits than that has can't
    // be a time that fits CCYYMMDD, and stopping there keeps the sum from
    // overflowing
    constexpr long long kLast = 253402300799LL;
    if (seconds.empty() || !std::all_of(seconds.begin(), seconds.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {};
    // leading zeros add nothing to the value
    seconds.remove_prefix(std::min(seconds.find_first_not_of('0'), seconds.size()));
    if (seconds.size() > 12)
        return {};
    long long s = 0;
    for (const char c : seconds)
        s = s * 10 + (c - '0');
    if (s > kLast)
        return {};
    const std::chrono::year_month_day day{
        std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds(std::chrono::seconds(s)))};
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d%02u%02u", int(day.year()), unsigned(day.month()), unsigned(day.day()));
    return std::string(buf);
}

SauceSplit splitSauce(std::string_view file) {
    SauceSplit out;
    std::string_view content = file;
    if (file.size() >= Sauce::kRecordSize) {
        std::string_view rec = file.substr(file.size() - Sauce::kRecordSize);
        if (rec.substr(0, 5) == "SAUCE") {
            Sauce s;
            s.title = field(rec, 7, Sauce::kTitleLen);
            s.author = field(rec, 42, Sauce::kAuthorLen);
            s.group = field(rec, 62, Sauce::kGroupLen);
            s.date = field(rec, 82, 8);
            s.fileSize = le32(rec, 90);
            s.dataType = static_cast<std::uint8_t>(rec[94]);
            s.fileType = static_cast<std::uint8_t>(rec[95]);
            s.tinfo1 = le16(rec, 96);
            s.tinfo2 = le16(rec, 98);
            s.tinfo3 = le16(rec, 100);
            s.tinfo4 = le16(rec, 102);
            const auto ncomments = static_cast<unsigned char>(rec[104]);
            s.flags = static_cast<std::uint8_t>(rec[105]);
            s.font = field(rec, 106, Sauce::kFontLen);
            content = file.substr(0, file.size() - Sauce::kRecordSize);
            const size_t block = 5 + Sauce::kCommentLen * ncomments;
            if (ncomments && content.size() >= block && content.substr(content.size() - block, 5) == "COMNT") {
                std::string_view lines = content.substr(content.size() - block + 5);
                for (size_t i = 0; i < ncomments; ++i)
                    s.comments.push_back(field(lines, i * Sauce::kCommentLen, Sauce::kCommentLen));
                content = content.substr(0, content.size() - block);
            }
            out.sauce = std::move(s);
        }
    }
    out.body = content;
    const size_t eof = content.find('\x1a');
    out.content = eof == std::string_view::npos ? content : content.substr(0, eof);
    return out;
}

std::string encodeSauce(const Sauce& s, std::uint32_t contentSize) {
    std::string out = "\x1a";
    const size_t ncomments = std::min(s.comments.size(), Sauce::kMaxComments);
    if (ncomments) {
        out += "COMNT";
        for (size_t i = 0; i < ncomments; ++i)
            putField(out, s.comments[i], Sauce::kCommentLen, ' ');
    }
    out += "SAUCE00";
    putField(out, s.title, Sauce::kTitleLen, ' ');
    putField(out, s.author, Sauce::kAuthorLen, ' ');
    putField(out, s.group, Sauce::kGroupLen, ' ');
    putField(out, s.date, 8, ' ');
    put32(out, contentSize);
    out += static_cast<char>(s.dataType);
    out += static_cast<char>(s.fileType);
    put16(out, s.tinfo1);
    put16(out, s.tinfo2);
    put16(out, s.tinfo3);
    put16(out, s.tinfo4);
    out += static_cast<char>(ncomments);
    out += static_cast<char>(s.flags);
    putField(out, s.font, Sauce::kFontLen, '\0');
    return out;
}

} // namespace bw
