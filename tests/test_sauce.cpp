#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/sauce.h"

#include <catch2/catch_test_macros.hpp>

using namespace bw;

namespace {

std::string_view record(const std::string& file) { return std::string_view(file).substr(file.size() - 128); }

std::uint32_t le32(std::string_view s, size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | static_cast<unsigned char>(s[at + i]);
    return v;
}

} // namespace

TEST_CASE("SAUCE text fields are cut to 35/20/20 bytes and space-padded", "[sauce]") {
    Sauce s = Sauce::fresh(80, 25);
    s.title = std::string(50, 'T');
    s.author = "funkpower-with-a-very-long-name";
    s.group = "g";
    const std::string enc = encodeSauce(s, 0);
    REQUIRE(enc.size() == 129);  // EOF byte + record
    std::string_view rec = std::string_view(enc).substr(1);
    CHECK(rec.substr(0, 7) == "SAUCE00");
    CHECK(rec.substr(7, 35) == std::string(35, 'T'));
    CHECK(rec.substr(42, 20) == "funkpower-with-a-ver");
    CHECK(rec.substr(62, 20) == "g" + std::string(19, ' '));
    CHECK(rec.substr(106, 22) == std::string("IBM VGA") + std::string(15, '\0'));

    const std::string file = "x" + enc;
    auto back = splitSauce(file);
    REQUIRE(back.sauce);
    CHECK(back.sauce->title == std::string(35, 'T'));
    CHECK(back.sauce->author == "funkpower-with-a-ver");
    CHECK(back.sauce->group == "g");
    CHECK(back.content == "x");
}

TEST_CASE("SAUCE fields read back exactly", "[sauce]") {
    Sauce s;
    s.title = "Durin's Gate";
    s.author = "funkpower";
    s.group = "FPW";
    s.date = "20261021";
    s.tinfo1 = 120;
    s.tinfo2 = 64;
    s.flags = 0;
    const std::string file = "art" + encodeSauce(s, 3);
    auto r = splitSauce(file);
    REQUIRE(r.sauce);
    CHECK(r.sauce->title == "Durin's Gate");
    CHECK(r.sauce->author == "funkpower");
    CHECK(r.sauce->group == "FPW");
    CHECK(r.sauce->date == "20261021");
    CHECK(r.sauce->fileSize == 3);
    CHECK(r.sauce->dataType == 1);
    CHECK(r.sauce->fileType == 1);
    CHECK(r.sauce->tinfo1 == 120);
    CHECK(r.sauce->tinfo2 == 64);
    CHECK(r.sauce->font == "IBM VGA");
}

TEST_CASE("SAUCE flags: iCE, letter spacing, aspect ratio", "[sauce]") {
    Sauce s;
    s.flags = 0;
    s.setIce(true);
    CHECK(s.flags == 0x01);
    s.setLetterSpacing(2);  // 9 px
    CHECK(s.flags == 0x05);
    s.setAspectRatio(1);  // legacy
    CHECK(s.flags == 0x0D);
    CHECK(s.ice());
    CHECK(s.letterSpacing() == 2);
    CHECK(s.aspectRatio() == 1);
    s.setIce(false);
    s.setLetterSpacing(1);
    s.setAspectRatio(2);
    CHECK(s.flags == 0x12);
    CHECK_FALSE(s.ice());

    const std::string file = "x" + encodeSauce(s, 1);
    CHECK(static_cast<unsigned char>(record(file)[105]) == 0x12);
    CHECK(splitSauce(file).sauce->flags == 0x12);
}

TEST_CASE("SAUCE COMNT block is written and read", "[sauce]") {
    Sauce s = Sauce::fresh(80, 1);
    s.comments = {"first line", std::string(70, 'c')};
    const std::string enc = encodeSauce(s, 4);
    // EOF + "COMNT" + 2 * 64 + record
    REQUIRE(enc.size() == 1 + 5 + 128 + 128);
    CHECK(enc.substr(1, 5) == "COMNT");
    CHECK(enc.substr(6, 64) == "first line" + std::string(54, ' '));
    CHECK(static_cast<unsigned char>(enc[1 + 5 + 128 + 104]) == 2);

    const std::string file = "body" + enc;
    auto r = splitSauce(file);
    REQUIRE(r.sauce);
    REQUIRE(r.sauce->comments.size() == 2);
    CHECK(r.sauce->comments[0] == "first line");
    CHECK(r.sauce->comments[1] == std::string(64, 'c'));
    CHECK(r.content == "body");
}

TEST_CASE("A COMNT count with no COMNT block leaves the content alone", "[sauce]") {
    Sauce s = Sauce::fresh(80, 1);
    std::string enc = encodeSauce(s, 2);
    enc[1 + 104] = 3;  // claims three comments
    const std::string file = "ab" + enc;
    auto r = splitSauce(file);
    REQUIRE(r.sauce);
    CHECK(r.sauce->comments.empty());
    CHECK(r.content == "ab");
}

TEST_CASE("A file whose SAUCE FileSize is 0 still loads its content", "[sauce]") {
    Sauce s = Sauce::fresh(10, 1);
    std::string file = "\x1b[0;31;40mhello\x1b[0m\r\n";
    file += encodeSauce(s, 0);  // FileSize 0, the way Moebius broke on it
    REQUIRE(le32(record(file), 90) == 0);
    AnsiDocument doc = parseAnsi(file);
    REQUIRE(doc.canvas.width() == 10);
    REQUIRE(doc.canvas.height() == 1);
    CHECK(doc.canvas.at(0, 0).ch == U'h');
    CHECK(doc.canvas.at(4, 0).ch == U'o');
    CHECK(doc.canvas.at(0, 0).fg.index == Red);
}

TEST_CASE("SAUCE FileSize 0 with a large TInfo2 loads the content at the SAUCE height", "[sauce]") {
    Sauce s = Sauce::fresh(10, 300);
    std::string file = "\x1b[0;31;40mhello\x1b[0m\r\nworld";
    file += encodeSauce(s, 0);
    REQUIRE(le32(record(file), 90) == 0);
    AnsiDocument doc = parseAnsi(file);
    REQUIRE(doc.canvas.width() == 10);
    REQUIRE(doc.canvas.height() == 300);
    CHECK(doc.canvas.at(0, 0).ch == U'h');
    CHECK(doc.canvas.at(0, 0).fg.index == Red);
    CHECK(doc.canvas.at(4, 1).ch == U'd');
    CHECK(doc.canvas.at(0, 299) == Cell{});
}

TEST_CASE("A wrong FileSize doesn't cut the content", "[sauce]") {
    std::string file = "abcdef\x1b[0m\r\n" + encodeSauce(Sauce::fresh(80, 1), 2);
    AnsiDocument doc = parseAnsi(file);
    CHECK(doc.canvas.at(5, 0).ch == U'f');
}

TEST_CASE("Written FileSize is the content length before the EOF byte", "[sauce]") {
    AnsiDocument doc = bwtest::parse("\x1b[1;33mAB\r\nC");
    doc.sauce = Sauce::fresh(80, 2);
    doc.sauce->fileSize = 999999;  // stale value from a read must not survive
    doc.sauce->comments = {"note"};
    const std::string out = writeAnsi(doc).bytes;
    const size_t eof = out.find('\x1a');
    REQUIRE(eof != std::string::npos);
    CHECK(le32(record(out), 90) == eof);
    auto r = splitSauce(out);
    CHECK(r.content.size() == eof);
    CHECK(r.sauce->fileSize == eof);
    CHECK(r.sauce->comments == std::vector<std::string>{"note"});
}

TEST_CASE("Content stops at the first EOF byte", "[sauce]") {
    const std::string file("art\x1a" "junk after eof", 18);
    auto r = splitSauce(file);
    CHECK_FALSE(r.sauce);
    CHECK(r.content == "art");
}

TEST_CASE("Without SAUCE there is no record and width defaults to 80", "[sauce]") {
    AnsiDocument doc = parseAnsi("hi");
    CHECK_FALSE(doc.sauce);
    CHECK(doc.ice);
    CHECK(doc.canvas.width() == 80);
}

TEST_CASE("Writing a document without SAUCE adds a fresh record", "[sauce]") {
    AnsiDocument doc = bwtest::parse("hi", 30);
    const std::string out = writeAnsi(doc).bytes;
    auto r = splitSauce(out);
    REQUIRE(r.sauce);
    CHECK(r.sauce->tinfo1 == 30);
    CHECK(r.sauce->tinfo2 == 1);
    CHECK(r.sauce->ice());
    CHECK(r.sauce->date.size() == 8);
    CHECK(r.sauce->font == "IBM VGA");
}

TEST_CASE("A FileSize larger than the file doesn't cut the content", "[sauce]") {
    const std::string body = "\x1b[0;31;40mhello\x1b[0m\r\n";
    const std::string file = body + encodeSauce(Sauce::fresh(80, 1), 1'000'000);
    auto r = splitSauce(file);
    REQUIRE(r.sauce);
    CHECK(r.sauce->fileSize == 1'000'000);
    CHECK(r.content == body);
    CHECK(parseAnsi(file).canvas.at(4, 0).ch == U'o');
}

TEST_CASE("A SAUCE record with no EOF byte before it is still read", "[sauce]") {
    Sauce s = Sauce::fresh(12, 1);
    s.title = "no eof";
    const std::string body = "abc\x1b[0m\r\n";
    const std::string rec = encodeSauce(s, static_cast<std::uint32_t>(body.size()));
    REQUIRE(rec[0] == '\x1a');
    const std::string file = body + rec.substr(1);
    auto r = splitSauce(file);
    REQUIRE(r.sauce);
    CHECK(r.sauce->title == "no eof");
    CHECK(r.content == body);
    AnsiDocument doc = parseAnsi(file);
    CHECK(doc.canvas.width() == 12);
    CHECK(doc.canvas.at(2, 0).ch == U'c');
}

TEST_CASE("SAUCE TInfo2 is the minimum canvas height", "[sauce]") {
    auto withHeight = [](const std::string& body, int height) {
        Sauce s = Sauce::fresh(10, height);
        return body + encodeSauce(s, static_cast<std::uint32_t>(body.size()));
    };
    SECTION("blank rows below the content are kept") {
        AnsiDocument doc = parseAnsi(withHeight("A\r\n", 5));
        CHECK(doc.canvas.height() == 5);
        CHECK(doc.canvas.at(0, 4) == Cell{});
        // and written back out
        CHECK(parseAnsi(writeAnsi(doc).bytes).canvas == doc.canvas);
    }
    SECTION("drawn content extends it") {
        CHECK(parseAnsi(withHeight("1\r\n2\r\n3\r\n", 2)).canvas.height() == 3);
    }
    SECTION("0 means not set") {
        CHECK(parseAnsi(withHeight("1\r\n2\r\n", 0)).canvas.height() == 2);
    }
    SECTION("bounded by the row limit") {
        CHECK(parseAnsi(withHeight("A", 65535)).canvas.height() == kMaxRows);
    }
    SECTION("ignored for non-character SAUCE") {
        Sauce s = Sauce::fresh(10, 9);
        s.dataType = 5;  // BinaryText: TInfo fields mean something else
        const std::string body = "A";
        CHECK(parseAnsi(body + encodeSauce(s, 1)).canvas.height() == 1);
    }
}

TEST_CASE("SAUCE text fields, comments and every flag round-trip through an .ans", "[sauce]") {
    // #7: what the SAUCE dialog and the iCE and 8/9 px toggles set comes back
    // from writeAnsi and parseAnsi as it was
    for (const std::uint8_t flags : {0x00, 0x01, 0x02, 0x04, 0x05, 0x08, 0x0B, 0x10, 0x14, 0x15}) {
        INFO("flags " << int(flags));
        AnsiDocument doc = bwtest::parse("\x1b[1;5;33;41mA");
        doc.sauce = Sauce::fresh(80, 1);
        doc.sauce->title = std::string(35, 'T');
        doc.sauce->author = "funkpower";
        doc.sauce->group = std::string(20, 'g');
        doc.sauce->date = "19961231";
        doc.sauce->comments = {std::string(64, 'c'), "", "\x01\xb0\xdb \xfe"};
        doc.sauce->flags = flags;
        doc.ice = flags & 0x01;
        if (!doc.ice)  // what an iCE-off editor saves: blink, not a bright background
            doc.canvas.at(0, 0) = iceCell(doc.canvas.at(0, 0), false);
        const AnsiOutput out = writeAnsi(doc);
        CHECK_FALSE(out.lossy());
        const AnsiDocument back = parseAnsi(out.bytes);
        REQUIRE(back.sauce);
        CHECK(back.sauce->title == doc.sauce->title);
        CHECK(back.sauce->author == "funkpower");
        CHECK(back.sauce->group == doc.sauce->group);
        CHECK(back.sauce->date == "19961231");
        CHECK(back.sauce->comments == doc.sauce->comments);
        CHECK(back.sauce->flags == flags);
        CHECK(back.sauce->ice() == doc.ice);
        CHECK(back.sauce->letterSpacing() == doc.sauce->letterSpacing());
        CHECK(back.sauce->aspectRatio() == doc.sauce->aspectRatio());
        CHECK(back.ice == doc.ice);
        CHECK(back.canvas.at(0, 0) == doc.canvas.at(0, 0));
    }
}

TEST_CASE("iceCell: blink and a bright background are one bit", "[sauce]") {
    const Cell bright{U'A', Color::indexed(Yellow), Color::indexed(LightRed), 0, false};
    const Cell blink{U'A', Color::indexed(Yellow), Color::indexed(Red), attr::Blink, false};
    const Cell plain{U'A', Color::indexed(Yellow), Color::indexed(Red), 0, false};
    // iCE on: blink shows (and saves) as the bright background
    CHECK(iceCell(bright, true) == bright);
    CHECK(iceCell(blink, true) == bright);
    CHECK(iceCell(plain, true) == plain);
    // iCE off: a bright background shows (and saves) as blink on the dark one
    CHECK(iceCell(bright, false) == blink);
    CHECK(iceCell(blink, false) == blink);
    CHECK(iceCell(plain, false) == plain);
    // the foreground's bright bit is not iCE's business
    const Cell boldFg{U'A', Color::indexed(White), Color::indexed(Black), 0, false};
    CHECK(iceCell(boldFg, false) == boldFg);

    // written through iceCell, iCE off is SGR 5 with the flag off, and iCE on
    // a bright background with nothing dropped
    AnsiDocument doc = bwtest::parse("\x1b[33;101mA");
    REQUIRE(doc.canvas.at(0, 0).bg == Color::indexed(LightRed));
    doc.ice = false;
    doc.canvas.at(0, 0) = iceCell(doc.canvas.at(0, 0), false);
    AnsiOutput out = writeAnsi(doc);
    CHECK(out.bytes.find("\x1b[0;5;33;41mA") != std::string::npos);
    AnsiDocument back = parseAnsi(out.bytes);
    CHECK_FALSE(back.ice);
    CHECK(back.canvas.at(0, 0) == Cell{U'A', Color::indexed(Brown), Color::indexed(Red), attr::Blink, false});

    back.ice = true;
    back.canvas.at(0, 0) = iceCell(back.canvas.at(0, 0), true);
    out = writeAnsi(back);
    CHECK(out.blinkDropped == 0);
    back = parseAnsi(out.bytes);
    CHECK(back.ice);
    CHECK(back.canvas.at(0, 0) == Cell{U'A', Color::indexed(Brown), Color::indexed(LightRed), 0, false});
}

TEST_CASE("A trailing blinking space is written, not trimmed as black", "[sauce]") {
    // iCE off, a bright-black space is a blinking space on black: trimming it
    // as a blank would lose DarkGrey once iCE is on again
    AnsiDocument doc = bwtest::parse("A");
    doc.ice = false;
    doc.canvas.at(1, 0) = iceCell(Cell{U' ', Color::indexed(Grey), Color::indexed(DarkGrey), 0, false}, false);
    REQUIRE(doc.canvas.at(1, 0) == Cell{U' ', Color::indexed(Grey), Color::indexed(Black), attr::Blink, false});
    const AnsiOutput out = writeAnsi(doc);
    CHECK_FALSE(out.lossy());
    const AnsiDocument back = parseAnsi(out.bytes);
    CHECK_FALSE(back.ice);
    CHECK(back.canvas.at(1, 0) == doc.canvas.at(1, 0));
    CHECK(iceCell(back.canvas.at(1, 0), true).bg == Color::indexed(DarkGrey));
}
