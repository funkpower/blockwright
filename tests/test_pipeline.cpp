// The pipeline helper (pipeline.h): slugs and tags, the ascii library's
// index files, Publish to Library's plan and writes, and Send to WeeChat's
// ssh command. The command is run for real by test_app_publish.cpp, and here
// under each shell installed for a connection that drops.

#include "fs_quirks.h"
#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/pipeline.h"
#include "blockwright/project.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>

extern char** environ;
#endif

using namespace bw;
using namespace bw::pipeline;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary);
    out << bytes;
}

// A scratch library with an empty funkpower/ folder, removed afterwards.
struct Library {
    fs::path root;
    explicit Library(const std::string& name) {
        root = fs::path(BW_TEST_TMP) / ("pipeline-" + name);
        fs::remove_all(root);
        fs::create_directories(root / "funkpower");
    }
    ~Library() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// A small piece: "hi" in red on blue, then a row of green blocks.
std::string piece(int blocks = 4) {
    AnsiDocument doc;
    doc.canvas = Canvas(80, 2);
    doc.canvas.at(0, 0).ch = U'h';
    doc.canvas.at(1, 0).ch = U'i';
    for (int x = 0; x < 2; ++x) {
        doc.canvas.at(x, 0).fg = Color::indexed(4);
        doc.canvas.at(x, 0).bg = Color::indexed(1);
    }
    for (int x = 0; x < blocks; ++x) {
        doc.canvas.at(x, 1).ch = U'\u2588';
        doc.canvas.at(x, 1).fg = Color::indexed(x % 2 ? 2 : 10);
    }
    return writeAnsi(doc).bytes;
}

// The indexes as Python writes them (json.dump(d, indent=1), keys sorted).
const std::string kTags =
    "{\n"
    " \"funkpower/old\": [\n"
    "  \"funkpower\",\n"
    "  \"lrh\"\n"
    " ],\n"
    " \"ircart/x\": [\n"
    "  \"caf\\u00e9\",\n"
    "  \"q\\\"uote\\\\\"\n"
    " ]\n"
    "}\n";
const std::string kDates =
    "{\n"
    " \"funkpower/old\": 1629882333,\n"
    " \"ircart/x\": 1790869689\n"
    "}";

PublishRequest request(const Library& lib, std::string slug = "new-piece") {
    PublishRequest r;
    r.library = lib.root;
    r.slug = std::move(slug);
    r.ans = piece();
    r.now = 1800000000;
    return r;
}

// What publish() says when it refuses `plan`; empty when it publishes.
std::string refusal(const PublishPlan& plan) {
    try {
        publish(plan);
        return {};
    } catch (const PipelineError& e) {
        return e.what();
    }
}

} // namespace

TEST_CASE("Slugs are lower case, digits, '-' and '_'", "[pipeline]") {
    CHECK(validSlug("neon-skull"));
    CHECK(validSlug("black_man"));
    CHECK(validSlug("911ansi"));
    CHECK(validSlug(std::string(64, 'a')));
    CHECK_FALSE(validSlug(std::string(65, 'a')));
    CHECK_FALSE(validSlug(""));
    CHECK_FALSE(validSlug("-lead"));
    CHECK_FALSE(validSlug("_lead"));
    CHECK_FALSE(validSlug("Upper"));
    CHECK_FALSE(validSlug("a b"));
    CHECK_FALSE(validSlug("a/b"));
    CHECK_FALSE(validSlug("../x"));
    CHECK_FALSE(validSlug("caf\xc3\xa9"));

    CHECK(slugFrom("Neon Skull (r3)") == "neon-skull-r3");
    CHECK(slugFrom("--a__b--") == "a__b");
    CHECK(slugFrom("caf\xc3\xa9 au lait") == "caf-au-lait");
    CHECK(slugFrom("\xe2\x98\x85") == "");
    CHECK(slugFrom(std::string(70, 'x')) == std::string(64, 'x'));
    CHECK(validSlug(slugFrom("Neon Skull (r3)")));
}

TEST_CASE("Tags as typed are split, lowercased, deduplicated and sorted", "[pipeline]") {
    CHECK(parseTags("") == std::vector<std::string>{});
    CHECK(parseTags(" LRH, comic,,lrh  orthodox ") == std::vector<std::string>{"comic", "lrh", "orthodox"});
    CHECK(parseTags("caf\xc3\xa9") == std::vector<std::string>{"caf\xc3\xa9"});
    CHECK_THROWS_AS(parseTags("a\tb\x01"), PipelineError);
}

TEST_CASE("Tags are lowercased beyond ASCII, as the ascii script's str.lower() does", "[pipeline]") {
    CHECK(parseTags("CAF\xc3\x89,caf\xc3\xa9") == std::vector<std::string>{"caf\xc3\xa9"});
    CHECK(parseTags("\xce\x94\xce\x9f\xce\x9c") == std::vector<std::string>{"\xce\xb4\xce\xbf\xce\xbc"});
    CHECK_THROWS_AS(parseTags("caf\xc3"), PipelineError);
    CHECK_THROWS_AS(parseTags("\xed\xa0\x80"), PipelineError);
}

TEST_CASE("A tag typed with a leading '#' is stored without it, as the ascii script's tag command stores it",
          "[pipeline]") {
    // w.lstrip("#").lower(): the script's "#neon" search is for the exact
    // tag "neon", so a stored "#neon" would never be found by it.
    CHECK(parseTags("#Neon #skull") == std::vector<std::string>{"neon", "skull"});
    CHECK(parseTags("##neon, neon,#NEON") == std::vector<std::string>{"neon"});
    // Only leading ones go; a tag that was only '#'s is none.
    CHECK(parseTags("c#, # ,##") == std::vector<std::string>{"c#"});
    CHECK(parseTags("#\xc3\x89t\xc3\xa9") == std::vector<std::string>{"\xc3\xa9t\xc3\xa9"});
    CHECK_THROWS_AS(parseTags("#caf\xc3"), PipelineError);
}

TEST_CASE("Republishing drops the leading '#' of the piece's tags already in the index", "[pipeline]") {
    Library lib("hash");
    writeFile(lib.root / ".tags.json", "{\n \"funkpower/old\": [\n  \"#\",\n  \"#Neon\",\n  \"funkpower\"\n"
                                       " ],\n \"ircart/x\": [\n  \"#keep\"\n ]\n}\n");
    writeFile(lib.root / "funkpower" / "old.ans", "stale");
    PublishRequest r = request(lib, "old");
    r.tags = parseTags("#neon #skull");
    const PublishPlan plan = planPublish(r);
    CHECK(plan.tags == std::vector<std::string>{"funkpower", "neon", "skull"});
    // Another piece's entry is the script's own and is left as it is.
    CHECK(parseTagIndex(plan.tagsJson).at("ircart/x") == std::vector<std::string>{"#keep"});
}

TEST_CASE("Republishing folds the piece's tags already in the index to lower case too", "[pipeline]") {
    Library lib("case");
    writeFile(lib.root / ".tags.json", "{\n \"funkpower/old\": [\n  \"CAF\\u00c9\",\n  \"Comic\",\n  \"funkpower\"\n"
                                       " ],\n \"funkpower/other\": [\n  \"LRH\"\n ]\n}\n");
    writeFile(lib.root / "funkpower" / "old.ans", "stale");
    PublishRequest r = request(lib, "old");
    r.tags = parseTags("caf\xc3\xa9, comic");
    const PublishPlan plan = planPublish(r);
    CHECK(plan.tags == std::vector<std::string>{"caf\xc3\xa9", "comic", "funkpower"});
    // Only the piece's own entry is rewritten.
    CHECK(parseTagIndex(plan.tagsJson).at("funkpower/other") == std::vector<std::string>{"LRH"});
}

TEST_CASE("The indexes read and write back byte for byte as Python writes them", "[pipeline]") {
    const TagIndex tags = parseTagIndex(kTags);
    REQUIRE(tags.size() == 2);
    CHECK(tags.at("funkpower/old") == std::vector<std::string>{"funkpower", "lrh"});
    CHECK(tags.at("ircart/x") == std::vector<std::string>{"caf\xc3\xa9", "q\"uote\\"});
    CHECK(tagIndexJson(tags, true) == kTags);

    const DateIndex dates = parseDateIndex(kDates);
    REQUIRE(dates.size() == 2);
    CHECK(dates.at("funkpower/old") == 1629882333);
    CHECK(dateIndexJson(dates, false) == kDates);

    // Python's escapes: the short ones, \u00XX for other controls and DEL,
    // a surrogate pair past the BMP; '/' as it is.
    TagIndex odd{{"k", {"a\nb\tc\r\b\f\x01/\x7f", "\xf0\x9f\x98\x80", "\xe2\x98\x85"}}};
    const std::string json = tagIndexJson(odd, false);
    CHECK(json ==
          "{\n \"k\": [\n  \"a\\nb\\tc\\r\\b\\f\\u0001/\\u007f\",\n  \"\\ud83d\\ude00\",\n  \"\\u2605\"\n ]\n}");
    CHECK(parseTagIndex(json) == odd);

    CHECK(tagIndexJson({}, false) == "{}");
    CHECK(tagIndexJson({{"k", {}}}, false) == "{\n \"k\": []\n}");
    CHECK(parseTagIndex("{}").empty());
    CHECK(parseTagIndex(" {\"a\":[ ],\"b\" : [\"x\"]} ").at("b") == std::vector<std::string>{"x"});
    CHECK(parseDateIndex("{\"a\": -5}").at("a") == -5);
}

TEST_CASE("An index the ascii script would misread is refused", "[pipeline]") {
    for (const char* bad : {"", "[]", "{", "{\"a\": [\"x\"]", "{\"a\": [\"x\"]} x", "{\"a\": \"x\"}",
                            "{\"a\": [1]}", "{\"a\": [\"x\"], \"a\": []}", "{a: []}", "{\"a\": [\"\\ud83d\"]}",
                            "{\"a\": [\"\\q\"]}", "{\"a\": [\"\x01\"]}", "{\"a\": [\"\xff\"]}"})
        CHECK_THROWS_AS(parseTagIndex(bad), PipelineError);
    for (const char* bad : {"{\"a\": 1.5}", "{\"a\": \"1\"}", "{\"a\": []}", "{\"a\": 1e3}",
                            "{\"a\": 99999999999999999999}", "{\"a\": 1, \"a\": 2}"})
        CHECK_THROWS_AS(parseDateIndex(bad), PipelineError);
}

TEST_CASE("A date too large for 64 bits is refused and never wrapped", "[pipeline]") {
    // 2^64 wraps to 0 if the limit is checked after the arithmetic.
    CHECK_THROWS_AS(parseDateIndex(R"({"funkpower/old":18446744073709551616})"), PipelineError);
    CHECK_THROWS_AS(parseDateIndex(R"({"funkpower/old":18446744073709551626})"), PipelineError);
    CHECK_THROWS_AS(parseDateIndex(R"({"a":9223372036854775808})"), PipelineError);
    CHECK_THROWS_AS(parseDateIndex(R"({"a":-9223372036854775808})"), PipelineError);
    CHECK(parseDateIndex(R"({"a":9223372036854775807})").at("a") == 9223372036854775807);
    CHECK(parseDateIndex(R"({"a":-9223372036854775807})").at("a") == -9223372036854775807);
}

TEST_CASE("Publish to Library: a new piece gets its files, its tags and today's date", "[pipeline]") {
    Library lib("new");
    writeFile(lib.root / ".tags.json", kTags);
    writeFile(lib.root / ".dates.json", kDates);
    PublishRequest r = request(lib);
    r.tags = parseTags("lrh, comic");
    const PublishPlan plan = planPublish(r);
    CHECK(plan.key == "funkpower/new-piece");
    CHECK(plan.ansPath == lib.root / "funkpower" / "new-piece.ans");
    CHECK(plan.txtPath == lib.root / "funkpower" / "new-piece.txt");
    CHECK(plan.tags == std::vector<std::string>{"comic", "funkpower", "lrh"});
    CHECK_FALSE(plan.replaces);
    CHECK(plan.dated);
    CHECK(plan.ans == r.ans);
    // The .txt is what `cli export --irc` makes of the .ans.
    CHECK(plan.txt == irc::joinLines(irc::exportCanvas(parseAnsi(r.ans).canvas)));
    CHECK(plan.txt == irc::joinLines(ircLines(r.ans)));
    // Nothing is written until publish().
    CHECK_FALSE(fs::exists(plan.ansPath));
    CHECK(bwtest::readFile(lib.root / ".tags.json") == kTags);

    publish(plan);
    CHECK(bwtest::readFile(plan.ansPath) == r.ans);
    CHECK(bwtest::readFile(plan.txtPath) == plan.txt);
    // Only the new entry is added; the rest of each file is as it was,
    // trailing newline (tags) and none (dates) included.
    CHECK(bwtest::readFile(lib.root / ".tags.json") ==
          "{\n"
          " \"funkpower/new-piece\": [\n"
          "  \"comic\",\n"
          "  \"funkpower\",\n"
          "  \"lrh\"\n"
          " ],\n" +
              kTags.substr(2));
    CHECK(bwtest::readFile(lib.root / ".dates.json") ==
          "{\n \"funkpower/new-piece\": 1800000000,\n" + kDates.substr(2));
    // No temporary files are left behind.
    int files = 0;
    for (const auto& e : fs::recursive_directory_iterator(lib.root))
        files += e.is_regular_file();
    CHECK(files == 4);
}

TEST_CASE("Publish to Library over a piece keeps its date and adds to its tags", "[pipeline]") {
    Library lib("again");
    writeFile(lib.root / ".tags.json", kTags);
    writeFile(lib.root / ".dates.json", kDates);
    writeFile(lib.root / "funkpower" / "old.ans", "stale");
    PublishRequest r = request(lib, "old");
    r.tags = {"comic"};
    const PublishPlan plan = planPublish(r);
    CHECK(plan.replaces);
    CHECK_FALSE(plan.dated);
    CHECK(plan.tags == std::vector<std::string>{"comic", "funkpower", "lrh"});
    CHECK(plan.datesJson == kDates);
    publish(plan);
    CHECK(bwtest::readFile(lib.root / "funkpower" / "old.ans") == r.ans);
    CHECK(parseTagIndex(bwtest::readFile(lib.root / ".tags.json")).at("funkpower/old") == plan.tags);
}

TEST_CASE("Publish to Library starts the indexes when there are none", "[pipeline]") {
    Library lib("fresh");
    const PublishPlan plan = planPublish(request(lib));
    CHECK(plan.tags == std::vector<std::string>{"funkpower"});
    CHECK(plan.tagsJson == "{\n \"funkpower/new-piece\": [\n  \"funkpower\"\n ]\n}");
    CHECK(plan.datesJson == "{\n \"funkpower/new-piece\": 1800000000\n}");
    publish(plan);
    CHECK(fs::exists(lib.root / ".tags.json"));
    CHECK(fs::exists(lib.root / ".dates.json"));
}

TEST_CASE("Publish to Library refuses what it can't publish cleanly", "[pipeline]") {
    Library lib("refuse");
    SECTION("a bad slug") {
        CHECK_THROWS_AS(planPublish(request(lib, "Bad Name")), PipelineError);
    }
    SECTION("a folder that isn't the library") {
        fs::remove(lib.root / "funkpower");
        CHECK_THROWS_AS(planPublish(request(lib)), PipelineError);
    }
    SECTION("an index it can't parse is left alone") {
        writeFile(lib.root / ".tags.json", "{\"a\": [\"x\"],}");
        CHECK_THROWS_AS(planPublish(request(lib)), PipelineError);
    }
    SECTION("another file the ascii script takes for the same piece") {
        writeFile(lib.root / "funkpower" / "New-Piece.ANS", "x");
        CHECK_THROWS_AS(planPublish(request(lib)), PipelineError);
    }
    SECTION("the same piece with another extension") {
        writeFile(lib.root / "funkpower" / "new-piece.asc", "x");
        CHECK_THROWS_AS(planPublish(request(lib)), PipelineError);
    }
    SECTION("a line over the budget") {
        PublishRequest r = request(lib);
        r.ans = piece(80);
        // The budget is the line's as the ascii script sends it, which writes
        // the colour codes again with two digits: a budget the line fits as
        // written is not enough.
        const std::string line = ircLines(r.ans)[1];
        const std::size_t sent = irc::txtSentBytes(line);
        REQUIRE(sent > line.size());
        r.budget = line.size();
        try {
            planPublish(r);
            FAIL("no error");
        } catch (const PipelineError& e) {
            CHECK(std::string(e.what()).find("line 2 is " + std::to_string(irc::txtSentBytes(line, r.budget)) +
                                             " bytes") != std::string::npos);
        }
        r.budget = sent - 1;
        CHECK_THROWS_AS(planPublish(r), PipelineError);
        r.budget = sent;
        CHECK_NOTHROW(planPublish(r));
    }
    SECTION("a link at a temporary name is skipped, never written through") {
        const PublishPlan plan = planPublish(request(lib));
        writeFile(lib.root / "sentinel", "keep me");
        fs::path temp = plan.ansPath;
        temp += ".bw-" + std::to_string(::getpid()) + "-0.tmp";
        if (!bwtest::symlinksWork())
            SKIP("the symbolic-link tests don't run here: no right to make links (fs_quirks.h)");
        fs::create_symlink(lib.root / "sentinel", temp);
        publish(plan);
        CHECK(bwtest::readFile(lib.root / "sentinel") == "keep me");
        CHECK(fs::is_symlink(temp));
        CHECK(bwtest::readFile(plan.ansPath) == plan.ans);
    }
    SECTION("a link in place of a file") {
        const PublishPlan plan = planPublish(request(lib));
        if (!bwtest::symlinksWork())
            SKIP("the symbolic-link tests don't run here: no right to make links (fs_quirks.h)");
        fs::create_symlink(lib.root / "elsewhere.txt", plan.txtPath);
        CHECK_THROWS_AS(publish(plan), PipelineError);
        CHECK_FALSE(fs::exists(lib.root / "elsewhere.txt"));
        CHECK_FALSE(fs::exists(plan.ansPath));
    }
}

TEST_CASE("Publish to Library refuses an index it couldn't write back in the same layout", "[pipeline]") {
    Library lib("layout");
    fs::path file;
    std::string index;
    SECTION("CRLF line ends") {
        file = lib.root / ".dates.json";
        index = "{\r\n \"funkpower/old\": 100\r\n}\r\n";
    }
    SECTION("another indent") {
        file = lib.root / ".tags.json";
        index = "{\n  \"funkpower/old\": [\n    \"funkpower\"\n  ]\n}\n";
    }
    SECTION("keys out of order") {
        file = lib.root / ".dates.json";
        index = "{\n \"funkpower/zz\": 1,\n \"funkpower/old\": 100\n}";
    }
    SECTION("trailing white space") {
        file = lib.root / ".tags.json";
        index = "{\n \"funkpower/old\": [\n  \"funkpower\"\n ]\n}\n\n";
    }
    SECTION("an upper-case escape") {
        file = lib.root / ".tags.json";
        index = "{\n \"funkpower/old\": [\n  \"caf\\u00E9\"\n ]\n}";
    }
    writeFile(lib.root / "funkpower" / "old.ans", "stale");
    writeFile(file, index);
    try {
        planPublish(request(lib, "old"));
        FAIL("no error");
    } catch (const PipelineError& e) {
        CHECK(std::string(e.what()).find(file.string() + " is not laid out as the ascii script writes it") == 0);
    }
    CHECK(bwtest::readFile(file) == index);
}

TEST_CASE("Publish to Library refuses a plan the library has moved on from", "[pipeline]") {
    // The user answers the confirmation at leisure; whatever changed in the
    // meantime would be written over by the plan's old view of the library.
    Library lib("stale");
    writeFile(lib.root / ".tags.json", kTags);
    writeFile(lib.root / ".dates.json", kDates);
    const std::string changed = " changed after the publish was worked out";
    SECTION("two plans published one after the other") {
        const PublishPlan a = planPublish(request(lib, "piece-a"));
        const PublishPlan b = planPublish(request(lib, "piece-b"));
        publish(a);
        CHECK(refusal(b) == (lib.root / ".tags.json").string() + changed +
                                ", so nothing was written; publish again to include the change");
        CHECK(parseTagIndex(bwtest::readFile(lib.root / ".tags.json")).count("funkpower/piece-a"));
        CHECK(parseDateIndex(bwtest::readFile(lib.root / ".dates.json")).count("funkpower/piece-a"));
        CHECK_FALSE(fs::exists(b.ansPath));
        CHECK_FALSE(fs::exists(b.txtPath));
    }
    SECTION("the piece edited while the publish waits") {
        writeFile(lib.root / "funkpower" / "old.ans", "stale");
        const PublishPlan plan = planPublish(request(lib, "old"));
        writeFile(lib.root / "funkpower" / "old.ans", "edited");
        CHECK(refusal(plan).find((lib.root / "funkpower" / "old.ans").string() + changed) == 0);
        CHECK(bwtest::readFile(lib.root / "funkpower" / "old.ans") == "edited");
        CHECK(bwtest::readFile(lib.root / ".tags.json") == kTags);
    }
    SECTION("a name the ascii script takes for the piece made while it waits") {
        const PublishPlan plan = planPublish(request(lib));
        writeFile(lib.root / "funkpower" / "new-piece.asc", "x");
        CHECK(refusal(plan).find((lib.root / "funkpower" / "new-piece.asc").string() + changed) == 0);
        CHECK_FALSE(fs::exists(plan.ansPath));
        CHECK(bwtest::readFile(lib.root / ".tags.json") == kTags);
    }
    SECTION("an index removed while it waits") {
        const PublishPlan plan = planPublish(request(lib));
        fs::remove(lib.root / ".dates.json");
        CHECK(refusal(plan).find((lib.root / ".dates.json").string() + changed) == 0);
        CHECK_FALSE(fs::exists(lib.root / ".dates.json"));
    }
    SECTION("a plan of an unchanged library still publishes") {
        const PublishPlan plan = planPublish(request(lib));
        CHECK(refusal(plan).empty());
        // and, once published, is out of date itself
        CHECK(refusal(plan).find((plan.ansPath).string() + changed) == 0);
    }
}

TEST_CASE("Send to WeeChat: hosts, folders and the wip name", "[pipeline]") {
    CHECK(validHost("irc-host"));
    CHECK(validHost("art@irc.example.org"));
    CHECK_FALSE(validHost(""));
    CHECK_FALSE(validHost("-oProxyCommand=x"));
    CHECK_FALSE(validHost("a b"));
    CHECK_FALSE(validHost("a;b"));
    CHECK_FALSE(validHost("a$(b)"));

    CHECK(validRemoteDir("~/ascii/wip"));
    CHECK(validRemoteDir("~"));
    CHECK(validRemoteDir("/srv/art_1/wip-2"));
    CHECK_FALSE(validRemoteDir(""));
    CHECK_FALSE(validRemoteDir("ascii/wip"));
    CHECK_FALSE(validRemoteDir("~user/wip"));
    CHECK_FALSE(validRemoteDir("~/a b"));
    CHECK_FALSE(validRemoteDir("~/$(x)"));
    CHECK_FALSE(validRemoteDir("~/a/../b"));
    CHECK_FALSE(validRemoteDir("~/'x'"));

    CHECK(wipName("neon-skull", 3) == "neon-skull-r3.ans");
}

TEST_CASE("Send to WeeChat's ssh arguments never prompt and carry one command", "[pipeline]") {
    const auto args = sshArguments({"irc-host", "~/ascii/wip"}, "neon-skull", 2, 1234);
    REQUIRE(args.size() == 6);
    CHECK(args[0] == "-o");
    CHECK(args[1] == "BatchMode=yes");
    CHECK(args[2] == "-o");
    CHECK(args[3] == "ConnectTimeout=10");
    CHECK(args[4] == "irc-host");
    CHECK(args[5] == remoteCommand("~/ascii/wip", "neon-skull-r2.ans", 1234));
    const std::string& cmd = args[5];
    CHECK(cmd.find("d=~/ascii/wip;") != std::string::npos);
    CHECK(cmd.find("\"$d/neon-skull-r2.ans\"") != std::string::npos);
    // The length is checked before the rename.
    CHECK(cmd.find("-eq 1234 ]") != std::string::npos);
    CHECK(cmd.find("-eq 1234 ]") < cmd.find("mv -f \"$a\""));
    CHECK(cmd.find("pgrep -x weechat") != std::string::npos);
    CHECK(cmd.find("timeout -s KILL 5") != std::string::npos);
    CHECK(cmd.find("core.weechat */ascii reload") != std::string::npos);

    CHECK_THROWS_AS(sshArguments({"-x", "~/w"}, "a", 1, 1), PipelineError);
    CHECK_THROWS_AS(sshArguments({"h", "w"}, "a", 1, 1), PipelineError);
    CHECK_THROWS_AS(sshArguments({"h", "~/w"}, "A", 1, 1), PipelineError);
    CHECK_THROWS_AS(sshArguments({"h", "~/w"}, "a", 0, 1), PipelineError);
}

// Send to WeeChat runs its command in a POSIX sh on the far side.
#ifndef _WIN32
namespace {

// `name` as found on PATH, or empty.
fs::path onPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "";
    while (!rest.empty()) {
        const std::size_t colon = std::min(rest.find(':'), rest.size());
        const fs::path p = fs::path(rest.substr(0, colon)) / name;
        if (::access(p.c_str(), X_OK) == 0)
            return p;
        rest.remove_prefix(std::min(colon + 1, rest.size()));
    }
    return {};
}

// Runs `command` under `shell` (with `sh` first for busybox) as sshd runs a
// login shell's -c, after the client has gone: stdin holds `sent` and ends,
// and stdout and stderr are a pipe with no reader, as they are once the
// connection drops (or /dev/null, when not `dropped`). SIGPIPE is the
// default action in the shell, as sshd leaves it, and HOME is `home`, so
// the command's '~' is under it. Not `dropped`, stdout and stderr go to
// `output` when it is given. Returns the wait status.
int runDropped(const fs::path& shell, const std::string& command, const fs::path& sent, const fs::path& home,
               bool dropped = true, const fs::path& output = {}) {
    // The child's environment is built before the fork: this one's, with HOME
    // replaced.
    const std::string homeVar = "HOME=" + home.string();
    std::vector<char*> env;
    for (char** e = environ; *e; ++e)
        if (std::string_view(*e).substr(0, 5) != "HOME=")
            env.push_back(*e);
    env.push_back(const_cast<char*>(homeVar.c_str()));
    env.push_back(nullptr);
    const std::string argv0 = shell.filename().string();
    std::vector<char*> argv{const_cast<char*>(argv0.c_str())};
    if (shell.filename() == "busybox")
        argv.push_back(const_cast<char*>("sh"));
    argv.push_back(const_cast<char*>("-c"));
    argv.push_back(const_cast<char*>(command.c_str()));
    argv.push_back(nullptr);
    int out[2];
    REQUIRE(::pipe(out) == 0);
    ::close(out[0]);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        std::signal(SIGPIPE, SIG_DFL);
        const int in = ::open(sent.c_str(), O_RDONLY);
        const int to = dropped          ? out[1]
                       : output.empty() ? ::open("/dev/null", O_WRONLY)
                                        : ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in < 0 || to < 0 || ::dup2(in, 0) < 0 || ::dup2(to, 1) < 0 || ::dup2(to, 2) < 0)
            ::_exit(127);
        ::execve(shell.c_str(), argv.data(), env.data());
        ::_exit(127);
    }
    ::close(out[1]);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return status;
}

// The names in `dir`, sorted.
std::vector<std::string> listed(const fs::path& dir) {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(dir))
        out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("Send to WeeChat's command puts the .txt beside the .ans, or removes an earlier round's, in any sh",
          "[pipeline][utf8]") {
    // #336 slice 3, #350 slice 4: the .ans and the .txt arrive as one
    // upload, the .ans first, and are split by the sizes the command names.
    const fs::path root = fs::path(BW_TEST_TMP) / "pipeline-text";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string ans = piece(), txt = "\x03" "77\xE2\x98\x85 \xE2\x96\x9A\n";
    writeFile(root / "both", ans + txt);
    writeFile(root / "ans", ans);
    writeFile(root / "short", ans + txt.substr(0, 3));
    int ran = 0;
    for (const std::string name : {"sh", "bash", "dash", "zsh", "ksh", "mksh", "busybox"}) {
        const fs::path shell = onPath(name);
        if (shell.empty())
            continue;
        ++ran;
        INFO(shell.string());
        const std::string dir = "~/wip-" + name;
        const fs::path wip = root / ("wip-" + name);
        const std::string both = remoteCommand(dir, "neon-skull-r1.ans", ans.size(), txt.size());
        int status = runDropped(shell, both, root / "both", root, false);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(listed(wip) == std::vector<std::string>{"neon-skull-r1.ans", "neon-skull-r1.txt"});
        CHECK(bwtest::readFile(wip / "neon-skull-r1.ans") == ans);
        CHECK(bwtest::readFile(wip / "neon-skull-r1.txt") == txt);

        // An upload cut short leaves both as they were.
        writeFile(wip / "neon-skull-r1.ans", "old ans");
        writeFile(wip / "neon-skull-r1.txt", "old txt");
        status = runDropped(shell, both, root / "short", root, false);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 1);
        CHECK(listed(wip) == std::vector<std::string>{"neon-skull-r1.ans", "neon-skull-r1.txt"});
        CHECK(bwtest::readFile(wip / "neon-skull-r1.ans") == "old ans");
        CHECK(bwtest::readFile(wip / "neon-skull-r1.txt") == "old txt");

        // A connection that drops leaves no temporary file either.
        status = runDropped(shell, both, root / "short", root);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 1);
        CHECK(listed(wip) == std::vector<std::string>{"neon-skull-r1.ans", "neon-skull-r1.txt"});

        // The same round sent again with no .txt loses the old one, which
        // the ascii script would play instead of the new .ans.
        status = runDropped(shell, remoteCommand(dir, "neon-skull-r1.ans", ans.size()), root / "ans", root, false);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(listed(wip) == std::vector<std::string>{"neon-skull-r1.ans"});
        CHECK(bwtest::readFile(wip / "neon-skull-r1.ans") == ans);
    }
    CHECK(ran > 0);
    fs::remove_all(root);
}

TEST_CASE("Send to WeeChat's command puts the round back as it was when the pair fails half way, in any sh",
          "[pipeline][utf8]") {
    // Codex's finding 2 on #362: with the .ans or the .txt in place and the
    // other not, a failure or a HUP must leave both as they were, since the
    // ascii script plays a .txt before an .ans. mv and rm on PATH are
    // wrappers that count their calls in the folder they are in and fail, or
    // raise HUP in the shell that ran them, on the call the test names.
    const fs::path root = fs::path(BW_TEST_TMP) / "pipeline-pair";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path realMv = onPath("mv"), realRm = onPath("rm");
    REQUIRE_FALSE(realMv.empty());
    REQUIRE_FALSE(realRm.empty());
    const std::string ans = piece(), txt = "\x03" "77\xE2\x98\x85\n";
    writeFile(root / "both", ans + txt);
    writeFile(root / "ans", ans);
    // `when` runs with the call's number in $n and the real command in $real
    const auto wrapper = [&](const std::string& dir, const std::string& tool, const fs::path& real,
                             const std::string& when) {
        const fs::path bin = root / dir;
        fs::create_directories(bin);
        writeFile(bin / tool, "#!/bin/sh\nc=\"$(dirname \"$0\")/" + tool + ".calls\"\n"
                                  "n=$(($(cat \"$c\" 2>/dev/null || echo 0) + 1)); echo $n > \"$c\"\nreal=" +
                                  real.string() + "\n" + when + "\nexec \"$real\" \"$@\"\n");
        fs::permissions(bin / tool, fs::perms::owner_all);
        return bin;
    };
    const fs::path failSecondMv = wrapper("fail-mv-2", "mv", realMv, "[ $n -ne 2 ] || exit 1");
    const fs::path hupAfterMv = wrapper("hup-mv-1", "mv", realMv,
                                        "if [ $n -eq 1 ]; then \"$real\" \"$@\" || exit; kill -HUP $PPID; exit 0; fi");
    const fs::path failRmText = wrapper("fail-rm-txt", "rm", realRm,
                                        "for f; do case $f in */neon-skull-r1.txt) exit 1 ;; esac; done");
    const std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    struct Case {
        const char* name;
        fs::path bin;
        const char* tool;
        bool text;  // the send carries a .txt
        bool seeded;  // the round has an .ans and a .txt already
    };
    const std::vector<Case> cases{
        {"the .txt's rename fails", failSecondMv, "mv", true, true},
        {"HUP between the renames", hupAfterMv, "mv", true, true},
        {"the .txt's rename fails in a new round", failSecondMv, "mv", true, false},
        {"HUP after the .ans of an .ans-only send", hupAfterMv, "mv", false, true},
        {"removing the earlier .txt fails", failRmText, "rm", false, true},
    };
    int ran = 0;
    for (const std::string name : {"sh", "bash", "dash", "zsh", "ksh", "mksh", "busybox"}) {
        const fs::path shell = onPath(name);
        if (shell.empty())
            continue;
        INFO(shell.string());
        for (const Case& c : cases) {
            INFO(c.name);
            const std::string dir = "~/wip-" + name;
            const fs::path wip = root / ("wip-" + name);
            fs::remove_all(wip);
            fs::create_directories(wip);
            if (c.seeded) {
                writeFile(wip / "neon-skull-r1.ans", "old ans");
                writeFile(wip / "neon-skull-r1.txt", "old txt");
            }
            const fs::path calls = c.bin / (std::string(c.tool) + ".calls");
            fs::remove(calls);
            const std::string cmd = c.text ? remoteCommand(dir, "neon-skull-r1.ans", ans.size(), txt.size())
                                           : remoteCommand(dir, "neon-skull-r1.ans", ans.size());
            ::setenv("PATH", (c.bin.string() + ":" + oldPath).c_str(), 1);
            const int status = runDropped(shell, cmd, root / (c.text ? "both" : "ans"), root, false);
            ::setenv("PATH", oldPath.c_str(), 1);
            if (!fs::exists(calls)) {
                // busybox built with its applets preferred never runs PATH's
                WARN(shell.string() + " ran its own " + c.tool + ", not PATH's");
                continue;
            }
            ++ran;
            CHECK(WIFEXITED(status));
            CHECK(WEXITSTATUS(status) == 1);
            if (c.seeded) {
                CHECK(listed(wip) == std::vector<std::string>{"neon-skull-r1.ans", "neon-skull-r1.txt"});
                CHECK(bwtest::readFile(wip / "neon-skull-r1.ans") == "old ans");
                CHECK(bwtest::readFile(wip / "neon-skull-r1.txt") == "old txt");
            } else {
                CHECK(listed(wip) == std::vector<std::string>{});
            }
        }
    }
    CHECK(ran > 0);
    fs::remove_all(root);
}

TEST_CASE("Send to WeeChat's rollback runs once, and keeps a backup it couldn't put back, in any sh",
          "[pipeline][utf8]") {
    // Codex's re-check of #362: a HUP while the rollback put the .ans back
    // ran the cleanup again, which saw no rollback pending and removed both
    // backups. The .txt's rename (mv's second call) fails; on the third, the
    // .ans put back, the shell gets a HUP, and that mv then fails (Codex's
    // repro) or does its work.
    const fs::path root = fs::path(BW_TEST_TMP) / "pipeline-rollback";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path realMv = onPath("mv");
    REQUIRE_FALSE(realMv.empty());
    const std::string ans = piece(), txt = "\x03" "77\xE2\x98\x85\n";
    writeFile(root / "both", ans + txt);
    const auto wrapper = [&](const std::string& dir, const std::string& third) {
        const fs::path bin = root / dir;
        fs::create_directories(bin);
        writeFile(bin / "mv", "#!/bin/sh\nc=\"$(dirname \"$0\")/mv.calls\"\n"
                              "n=$(($(cat \"$c\" 2>/dev/null || echo 0) + 1)); echo $n > \"$c\"\n"
                              "[ $n -ne 2 ] || exit 1\nif [ $n -eq 3 ]; then kill -HUP $PPID; " +
                                  third + "fi\nexec " + realMv.string() + " \"$@\"\n");
        fs::permissions(bin / "mv", fs::perms::owner_all);
        return bin;
    };
    const fs::path hupThenFail = wrapper("hup-fail", "exit 1; "), hupThenMove = wrapper("hup-move", "");
    const std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    int ran = 0;
    for (const std::string name : {"sh", "bash", "dash", "zsh", "ksh", "mksh", "busybox"}) {
        const fs::path shell = onPath(name);
        if (shell.empty())
            continue;
        INFO(shell.string());
        for (const fs::path& bin : {hupThenFail, hupThenMove}) {
            const bool fails = bin == hupThenFail;
            INFO((fails ? "the .ans can't be put back" : "the .ans is put back"));
            const std::string dir = "~/wip-" + name;
            const fs::path wip = root / ("wip-" + name);
            fs::remove_all(wip);
            fs::create_directories(wip);
            writeFile(wip / "neon-skull-r1.ans", "old ans");
            writeFile(wip / "neon-skull-r1.txt", "old txt");
            fs::remove(bin / "mv.calls");
            ::setenv("PATH", (bin.string() + ":" + oldPath).c_str(), 1);
            const int status = runDropped(shell, remoteCommand(dir, "neon-skull-r1.ans", ans.size(), txt.size()),
                                          root / "both", root, false, root / "said");
            ::setenv("PATH", oldPath.c_str(), 1);
            if (!fs::exists(bin / "mv.calls")) {
                WARN(shell.string() + " ran its own mv, not PATH's");
                continue;
            }
            ++ran;
            CHECK(WIFEXITED(status));
            CHECK(WEXITSTATUS(status) == 1);
            CHECK(bwtest::readFile(wip / "neon-skull-r1.txt") == "old txt");
            std::vector<std::string> names = listed(wip);
            if (!fails) {
                CHECK(names == std::vector<std::string>{"neon-skull-r1.ans", "neon-skull-r1.txt"});
                CHECK(bwtest::readFile(wip / "neon-skull-r1.ans") == "old ans");
                continue;
            }
            // The new .ans stays, and the old one is kept beside it, named in
            // the message.
            CHECK(bwtest::readFile(wip / "neon-skull-r1.ans") == ans);
            REQUIRE(names.size() == 3);
            CHECK(names[0].rfind(".neon-skull-r1.ans.", 0) == 0);
            CHECK(bwtest::readFile(wip / names[0]) == "old ans");
            const std::string said = bwtest::readFile(root / "said");
            CHECK(said.find("wip-" + name + "/" + names[0]) != std::string::npos);
        }
    }
    CHECK(ran > 0);
    fs::remove_all(root);
}

TEST_CASE("Send to WeeChat's command removes its temporary file when the connection drops, in any sh",
          "[pipeline]") {
    // The "only N of M bytes" message has no reader, so the shell gets
    // SIGPIPE. bash runs the EXIT trap for a signal that kills it; dash, zsh
    // and busybox ash don't, and only the PIPE trap removes the file. The
    // folder is a "~/..." one, as Send to WeeChat's is, with HOME at the
    // scratch root: the command doesn't quote it, and sshArguments only lets
    // through folders it needn't (the build tree's path may have spaces).
    const fs::path root = fs::path(BW_TEST_TMP) / "pipeline-dropped";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path sent = root / "sent";
    writeFile(sent, std::string(20, 'x'));
    int ran = 0;
    for (const std::string name : {"sh", "bash", "dash", "zsh", "ksh", "mksh", "busybox"}) {
        const fs::path shell = onPath(name);
        if (shell.empty())
            continue;
        ++ran;
        INFO(shell.string());
        const std::string dir = "~/wip-" + name;
        REQUIRE(validRemoteDir(dir));
        const fs::path wip = root / ("wip-" + name);
        const int status = runDropped(shell, remoteCommand(dir, "neon-skull-r1.ans", 1234), sent, root);
        // It exits 1, as it does for an upload cut short, not by the signal.
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 1);
        REQUIRE(fs::is_directory(wip));
        std::vector<std::string> left;
        for (const auto& e : fs::directory_iterator(wip))
            left.push_back(e.path().filename().string());
        CHECK(left == std::vector<std::string>{});
    }
    CHECK(ran > 0);
    fs::remove_all(root);
}
#endif

TEST_CASE("A .txt line is held to the budget as WeeChat's ascii script sends it", "[pipeline][utf8]") {
    // Each row is what the ascii script (1.32.2, 3f16708) does
    // with the line as a .txt at 407 bytes. Derived by loading it with
    // python3 -I and a stub weechat module, then running text_to_irc on the
    // line and a '\n' and fit_lines on the result: the size of the line it
    // sends when fit_lines cuts nothing, and when it cuts, the shorter of
    // compact's line and encode_irc_min's, each with fit's reset after a
    // trailing space. Not from txtSentBytes.
    const std::string star = "\xE2\x98\x85", block = "\xE2\x96\x88";
    const auto stars = [&](int n) {  // ★ in mIRC 4 and 5 by turns, on 1
        std::string s = "\x03" "4,1" + star;
        for (int i = 1; i < n; ++i)
            s += (i % 2 ? "\x03" "5" : "\x03" "4") + star;
        return s;
    };
    std::string solid = "\x03" "0,4 ";  // a space on 4 and a 4 █ on 5 by turns
    for (int i = 1; i < 140; ++i)
        solid += i % 2 ? "\x03" "4,5" + block : std::string("\x03" "0,4 ");
    std::string pairs = "\x03" "3,2";
    for (int i = 0; i < 200; ++i)
        pairs += "ab";
    struct Row {
        const char* name;
        std::string line;
        std::size_t written, sent;
        bool cut;
    };
    const std::vector<Row> rows{
        {"Codex's repro: 80 stars", stars(80), 402, 483, true},
        {"68 stars", stars(68), 342, 411, true},
        {"67 stars", stars(67), 337, 405, false},
        {"spaces and blocks, sent as encode_irc_min has them", solid, 840, 147, false},
        {"a trailing space on a colour", pairs + " ", 405, 408, true},
        {"a digit after a reset", "\x03" "4,1x\x03\x02\x02" "1" + std::string(398, 'y'), 407, 409, true},
    };
    for (const Row& r : rows) {
        INFO(r.name);
        REQUIRE(r.line.size() == r.written);
        CHECK(irc::txtSentBytes(r.line, irc::kEfnetWeechatBudget) == r.sent);
        CHECK(irc::txtOverBudget({r.line}, irc::kEfnetWeechatBudget).empty() == !r.cut);
        if (r.cut)
            CHECK_THROWS_AS(requireBudget({r.line}, irc::kEfnetWeechatBudget), PipelineError);
        else
            CHECK_NOTHROW(requireBudget({r.line}, irc::kEfnetWeechatBudget));
    }
    // The repro and the last two fit as written, which is what the budget
    // used to measure.
    CHECK(irc::overBudget({stars(80), pairs + " ", rows.back().line}, irc::kEfnetWeechatBudget).empty());
}

TEST_CASE("A .txt line's formatting is costed as the ascii script sends it", "[pipeline][utf8]") {
    // Codex's re-check of #362: the script keeps bold, italic, underline,
    // reverse, strikethrough and monospace, writing each again where it
    // changes, and leaves a line with any of them to encode_irc, never
    // encode_irc_min. Recorded as the rows of the test above, from the
    // script (1.32.2, 3f16708). Not from txtSentBytes.
    std::string solid = "\x03" "0,4 ";  // as the test above has it
    for (int i = 1; i < 140; ++i)
        solid += i % 2 ? "\x03" "4,5\xE2\x96\x88" : std::string("\x03" "0,4 ");
    struct Row {
        std::string name, line;
        std::size_t written, sent;
        bool cut;
    };
    std::vector<Row> rows;
    for (const auto& [code, name] : std::vector<std::pair<std::string, std::string>>{
             {"\x02", "bold"}, {"\x1d", "italic"}, {"\x1f", "underline"}, {"\x16", "reverse"},
             {"\x1e", "strikethrough"}, {"\x11", "monospace"}}) {
        rows.push_back({name + ", 400 x", code + "\x03" "4,1" + std::string(400, 'x'), 405, 407, false});
        rows.push_back({name + ", 401 x", code + "\x03" "4,1" + std::string(401, 'x'), 406, 408, true});
    }
    rows.push_back({"bold on spaces and blocks: no shortest encoding", "\x02" + solid, 841, 1121, true});
    rows.push_back({"bold cancelled: the shortest encoding again", "\x02\x02" + solid, 842, 147, false});
    rows.push_back({"underlined trailing spaces are sent",
                    "\x1f\x03" "4" + std::string(395, 'x') + "    ", 402, 404, false});
    rows.push_back({"plain trailing spaces are not", "\x03" "4" + std::string(395, 'x') + "    ", 401, 398, false});
    rows.push_back({"a reset ends bold", "\x02\x03" "4,1x\x0f" + std::string(401, 'y'), 408, 411, true});
    rows.push_back({"a tab is spaces to the next stop", "\x03" "4,1" + std::string(396, 'x') + "\ty", 402, 407, false});
    rows.push_back({"other controls are dropped", "\x03" "4,1" + std::string(401, 'x') + "\x01\x01", 407, 407, false});
    rows.push_back({"a ^D colour is the nearest mIRC one",
                    "\x04" "FF0000,000000" + std::string(401, 'x'), 415, 407, false});
    rows.push_back({"a bare ^D resets the colours", "\x03" "4,1x\x04" + std::string(400, 'y'), 406, 408, true});
    for (const Row& r : rows) {
        INFO(r.name);
        REQUIRE(r.line.size() == r.written);
        CHECK(irc::txtSentBytes(r.line, irc::kEfnetWeechatBudget) == r.sent);
        if (r.cut)
            CHECK_THROWS_AS(requireBudget({r.line}, irc::kEfnetWeechatBudget), PipelineError);
        else
            CHECK_NOTHROW(requireBudget({r.line}, irc::kEfnetWeechatBudget));
    }
}

TEST_CASE("A .txt line is held to the budget whatever Unicode the ascii script's Python has", "[pipeline][utf8]") {
    // Codex's re-checks of #362: the script joins a code point to the
    // character before it only if its Python's unicodedata makes it a mark
    // (Mn or Me), and Blockwright's width table is Unicode 18.0. Each row was
    // recorded from the script (1.32.2, 3f16708) with its
    // unicodedata giving every category from the UnicodeData.txt of 14.0,
    // 15.0, 15.1, 16.0, 17.0 and 18.0 in turn, and natively under Python
    // 3.11, 3.12, 3.13 and 3.14 (14.0, 15.0, 15.1 and 16.0), which agreed: the
    // size it sends whole, or the shorter of its two encodings when it cuts,
    // under each. Not from txtSentBytes.
    const std::string m14 = "\xE0\xA2\x98", m15 = "\xE0\xBB\x8E", m16 = "\xE0\xA2\x97", m17 = "\xE1\xAB\x8F",
                      m18 = "\xD7\x88", block = "\xE2\x96\x88";
    // U+1171E, Mn until Unicode 15.1 and Mc from 16.0: a mark only the older
    // Pythons join
    const std::string was = "\xF0\x91\x9C\x9E";
    const auto repeat = [](const std::string& s, int n) {
        std::string out;
        for (int i = 0; i < n; ++i)
            out += s;
        return out;
    };
    // x in 4 with a mark in 5 after each: one cell in 4 where the script's
    // Python has the mark, two cells and a colour code each where it hasn't
    const auto marked = [&](const std::string& mark, int n) {
        return "\x03" "4,1" + repeat("x\x03" "5" + mark + "\x03" "4", n);
    };
    struct Row {
        const char* name;
        std::string line;
        std::size_t written;
        std::array<std::size_t, 6> sent;  // under 14.0, 15.0, 15.1, 16.0, 17.0, 18.0
        std::array<bool, 6> cut;
    };
    const std::vector<Row> rows{
        {"Codex's row: blocks with a 17.0 mark, swapped for spaces only before 17.0",
         "\x03" "4,4" + repeat(block + m17, 67), 406, {276, 276, 276, 276, 408, 408},
         {false, false, false, false, true, true}},
        {"Codex's row: a 17.0 mark in another colour, cut before 17.0", marked(m17, 41), 332,
         {413, 413, 413, 413, 170, 170}, {true, true, true, true, false, false}},
        {"a 16.0 mark in another colour", marked(m16, 41), 332, {413, 413, 413, 170, 170, 170},
         {true, true, true, false, false, false}},
        {"a 15.0 mark in another colour", marked(m15, 41), 332, {413, 170, 170, 170, 170, 170},
         {true, false, false, false, false, false}},
        {"a 14.0 mark, which every Python since 3.11 has", marked(m14, 41), 332, {170, 170, 170, 170, 170, 170},
         {false, false, false, false, false, false}},
        {"15.0 and 17.0 marks, sent whole under each",
         marked(m15, 20) + "\x03" "4,4" + repeat(block + m17, 30), 348, {389, 272, 272, 272, 272, 272},
         {false, false, false, false, false, false}},
        {"16.0 and 18.0 marks in the base's colours", "\x03" "4,1" + repeat("x" + m16 + "y" + m18, 50), 354,
         {356, 356, 356, 356, 356, 356}, {false, false, false, false, false, false}},
        {"Codex's row: blocks with U+1171E, swapped for spaces only from 16.0",
         "\x03" "4,4" + repeat(block + was, 57) + "xxx", 406, {408, 408, 408, 296, 296, 296},
         {true, true, true, false, false, false}},
        {"blocks with U+1171E, a pair fewer: whole under each", "\x03" "4,4" + repeat(block + was, 56) + "xxx", 399,
         {401, 401, 401, 401, 401, 401}, {false, false, false, false, false, false}},
        {"U+1171E in another colour, cut from 16.0", marked(was, 41), 373, {211, 211, 211, 454, 454, 454},
         {false, false, false, true, true, true}},
    };
    for (const Row& r : rows) {
        INFO(r.name);
        REQUIRE(r.line.size() == r.written);
        const std::size_t worst = *std::max_element(r.sent.begin(), r.sent.end());
        const bool cut = std::find(r.cut.begin(), r.cut.end(), true) != r.cut.end();
        CHECK(irc::txtSentBytes(r.line, irc::kEfnetWeechatBudget) == worst);
        if (!cut) {
            CHECK_NOTHROW(requireBudget({r.line}, irc::kEfnetWeechatBudget));
            continue;
        }
        try {
            requireBudget({r.line}, irc::kEfnetWeechatBudget);
            FAIL("accepted a line a Python of another Unicode cuts");
        } catch (const PipelineError& e) {
            INFO(e.what());
            CHECK(std::string(e.what()).find("line 1 is " + std::to_string(worst) + " bytes where ") !=
                  std::string::npos);
        }
    }
    // The refusal says which Python cuts the line.
    const auto refusal = [](const std::string& line) {
        try {
            requireBudget({"", line}, irc::kEfnetWeechatBudget);
        } catch (const PipelineError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    const std::string over = "over the 407-byte IRC budget as WeeChat's ascii script sends it, so it would be cut on IRC: ";
    CHECK(refusal(marked(m17, 41)) == over + "line 2 is 413 bytes where the script's Python has Unicode 16.0 or older, "
                                             "which sends U+1ACF as a character of its own");
    CHECK(refusal("\x03" "4,4" + repeat(block + m17, 67)) ==
          over + "line 2 is 408 bytes where the script's Python has Unicode 17.0 or later, which joins U+1ACF to the "
                 "character before it");
    CHECK(refusal("\x03" "4,4" + repeat(block + was, 57) + "xxx") ==
          over + "line 2 is 408 bytes where the script's Python has Unicode 15.1 or older, which joins U+1171E to the "
                 "character before it");
    CHECK(refusal(marked(was, 41)) == over + "line 2 is 454 bytes where the script's Python has Unicode 16.0 or later, "
                                             "which sends U+1171E as a character of its own");
    // Each mark keeps its own versions: U+1171E joins until 15.1, U+0ECE from
    // 15.0, so 15.0 and 15.1 join both and the 14.0 receiver alone sends
    // U+0ECE apart.
    CHECK(refusal(marked(m15, 41) + repeat(block + was, 2)).ends_with(
        "line 2 is 430 bytes where the script's Python has Unicode 14.0 or older, which sends U+0ECE as a character "
        "of its own"));
    // A line over under every Unicode needs no word on it.
    CHECK(refusal("\x03" "4,1" + repeat(block + m17, 140)).ends_with("line 2 is 846 bytes"));
}

TEST_CASE("Publish to Library takes the .txt from the lines it is given, and holds them to the budget",
          "[pipeline][utf8]") {
    // #336 slice 3, #350 slice 4: an IRC-target or UTF-8 document's .txt
    // comes from its canvas, not from the CP437 .ans read back.
    Library lib("lines");
    PublishRequest r = request(lib);
    r.lines = std::vector<std::string>{"\x03" "77\xE2\x98\x85", std::string(irc::kEfnetWeechatBudget, 'x')};
    const PublishPlan plan = planPublish(r);
    CHECK(plan.ans == r.ans);
    CHECK(plan.txt == irc::joinLines(*r.lines));
    CHECK(plan.txt != irc::joinLines(ircLines(r.ans)));
    publish(plan);
    CHECK(bwtest::readFile(plan.txtPath) == plan.txt);
    CHECK(bwtest::readFile(plan.ansPath) == r.ans);

    // The budget is the given lines', whatever the .ans would make.
    r.lines->push_back(std::string(irc::kEfnetWeechatBudget + 1, 'x'));
    REQUIRE(irc::overBudget(ircLines(r.ans), r.budget).empty());
    try {
        planPublish(r);
        FAIL("published a line over the budget");
    } catch (const PipelineError& e) {
        CHECK(std::string(e.what()) == "over the 407-byte IRC budget as WeeChat's ascii script sends it, so it would be cut on IRC: "
                                    "line 3 is 408 bytes");
    }
    CHECK_THROWS_AS(requireBudget(*r.lines, r.budget), PipelineError);
    CHECK_NOTHROW(requireBudget(*r.lines, r.budget + 1));
}

TEST_CASE("Publish and Send hold a .txt to the tighter of the document's budget and 407", "[pipeline]") {
    // #393, the maintainer's ruling: a tighter document budget is never
    // loosened, and the ascii library's 407 always holds.
    CHECK(sendBudget(300) == SendBudget{300, BudgetSource::Document});
    CHECK(sendBudget(406) == SendBudget{406, BudgetSource::Document});
    CHECK(sendBudget(irc::kMinBudget) == SendBudget{irc::kMinBudget, BudgetSource::Document});
    CHECK(sendBudget(407) == SendBudget{407, BudgetSource::Library});
    CHECK(sendBudget(450) == SendBudget{407, BudgetSource::Library});
    CHECK(sendBudget(irc::kMaxBudget) == SendBudget{407, BudgetSource::Library});
    CHECK(sendBudget(project::IrcBudget{}.bytes) == SendBudget{});

    // The refusal names the budget it enforced, and whose it is.
    const std::vector<std::string> lines{"ok", std::string(400, 'x')};
    REQUIRE(irc::txtSentBytes(lines[1]) == 400);
    try {
        requireBudget(lines, 350, BudgetSource::Document);
        FAIL("no error");
    } catch (const PipelineError& e) {
        CHECK(std::string(e.what()) ==
              "over the document's own 350-byte IRC budget as WeeChat's ascii script sends it: line 2 is 400 bytes");
    }
    CHECK_NOTHROW(requireBudget(lines, 400, BudgetSource::Document));

    Library lib("send-budget");
    PublishRequest r = request(lib);
    r.lines = lines;
    const SendBudget tight = sendBudget(350);
    r.budget = tight.bytes;
    r.budgetSource = tight.source;
    try {
        planPublish(r);
        FAIL("published a line over the document's budget");
    } catch (const PipelineError& e) {
        CHECK(std::string(e.what()) ==
              "over the document's own 350-byte IRC budget as WeeChat's ascii script sends it: line 2 is 400 bytes");
    }
    r.lines = std::vector<std::string>{"ok", std::string(450, 'x')};
    const SendBudget loose = sendBudget(450);
    r.budget = loose.bytes;
    r.budgetSource = loose.source;
    try {
        planPublish(r);
        FAIL("published a line over 407");
    } catch (const PipelineError& e) {
        CHECK(std::string(e.what()) == "over the 407-byte IRC budget as WeeChat's ascii script sends it, so it "
                                       "would be cut on IRC: line 2 is 450 bytes");
    }
}

TEST_CASE("Send to WeeChat's arguments carry the .txt's size and its name", "[pipeline][utf8]") {
    CHECK(wipTextName("neon-skull", 3) == "neon-skull-r3.txt");
    const auto args = sshArguments({"irc-host", "~/ascii/wip"}, "neon-skull", 2, 1234, 56);
    REQUIRE(args.size() == 6);
    CHECK(args[5] == remoteCommand("~/ascii/wip", "neon-skull-r2.ans", 1234, 56));
    const std::string& cmd = args[5];
    // One length for the two together, checked before anything is renamed;
    // both are backed up first, and the .txt, which the ascii script plays,
    // goes into place after the .ans; WeeChat is reloaded once both are.
    const std::string ansIn = "mv -f \"$a\" \"$d/neon-skull-r2.ans\"", textIn = "mv -f \"$x\" \"$d/neon-skull-r2.txt\"";
    CHECK(cmd.find("-eq 1290 ]") != std::string::npos);
    CHECK(cmd.find("-eq 1290 ]") < cmd.find(ansIn));
    CHECK(cmd.find("tail -c +1235 ") != std::string::npos);
    CHECK(cmd.find("head -c 1234 ") != std::string::npos);
    CHECK(cmd.find("cp -p \"$d/neon-skull-r2.ans\" \"$ba\"") < cmd.find("r=1;"));
    CHECK(cmd.find("cp -p \"$d/neon-skull-r2.txt\" \"$bx\"") < cmd.find("r=1;"));
    CHECK(cmd.find("r=1;") < cmd.find(ansIn));
    CHECK(cmd.find(ansIn) < cmd.find(textIn));
    CHECK(cmd.find(textIn) < cmd.find("r=; c;"));
    CHECK(cmd.find("r=; c;") < cmd.find("pgrep -x weechat"));
    CHECK(cmd.find("core.weechat */ascii reload") != std::string::npos);
    // With no .txt, an earlier one of the round is removed after the .ans is
    // in place, and put back if that fails.
    const std::string ansOnly = sshArguments({"irc-host", "~/ascii/wip"}, "neon-skull", 2, 1234)[5];
    CHECK(ansOnly == remoteCommand("~/ascii/wip", "neon-skull-r2.ans", 1234));
    CHECK(ansOnly.rfind("rm -f \"$d/neon-skull-r2.txt\"; r=; c;") > ansOnly.find(ansIn));
    CHECK(ansOnly.find("cp -p \"$d/neon-skull-r2.txt\" \"$bx\"") < ansOnly.find(ansIn));
    CHECK(ansOnly.find("r=; c;") < ansOnly.find("pgrep -x weechat"));
    CHECK(ansOnly.find("tail -c") == std::string::npos);
}

// Publish writes the .ans as CP437 (writeAnsi's default), so ircLines reads it
// as CP437 even when its bytes would pass for UTF-8: changing it to detect
// the encoding fails here (#302).
TEST_CASE("ircLines reads the published .ans as CP437, never as UTF-8", "[pipeline][utf8]") {
    AnsiDocument d;
    d.canvas = Canvas(3, 1);
    d.canvas.at(0, 0).ch = U'α';
    d.canvas.at(1, 0).ch = U'░';
    d.canvas.at(2, 0).ch = U'▒';
    const std::string ans = writeAnsi(d).bytes;
    // E0 B0 B1, which is also UTF-8 for one glyph (U+0C31)
    REQUIRE(ans.find("\xE0\xB0\xB1") != std::string::npos);
    REQUIRE(detectEncoding(std::string_view(ans).substr(0, ans.find("\x1A"))) == TextEncoding::Utf8);
    const std::vector<std::string> lines = ircLines(ans);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("α░▒") != std::string::npos);
    CHECK(lines[0].find("\xE0\xB0\xB1") == std::string::npos);
    // UTF-8 bytes for █ send Γûê
    const std::vector<std::string> block = ircLines("\xE2\x96\x88\r\n");
    REQUIRE(block.size() == 1);
    CHECK(block[0].find("Γûê") != std::string::npos);
    CHECK(block[0].find("█") == std::string::npos);
}
