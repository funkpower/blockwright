// The pipeline helper (pipeline.h): slugs and tags, the ascii library's
// index files, Publish to Library's plan and writes, and Send to WeeChat's
// ssh command. The command is run for real by test_app_publish.cpp, and here
// under each shell installed for a connection that drops.

#include "fs_quirks.h"
#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/irc.h"
#include "blockwright/pipeline.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
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
        const std::size_t longest = ircLines(r.ans)[1].size();
        r.budget = longest - 1;
        try {
            planPublish(r);
            FAIL("no error");
        } catch (const PipelineError& e) {
            CHECK(std::string(e.what()).find("line 2 is " + std::to_string(longest) + " bytes") != std::string::npos);
        }
        r.budget = longest;
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
    CHECK(cmd.find("-eq 1234 ]") < cmd.find("mv -f"));
    CHECK(cmd.find("pgrep -x weechat") != std::string::npos);
    CHECK(cmd.find("timeout 5") != std::string::npos);
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
// connection drops. SIGPIPE is the default action in the shell, as sshd
// leaves it, and HOME is `home`, so the command's '~' is under it. Returns
// the wait status.
int runDropped(const fs::path& shell, const std::string& command, const fs::path& sent, const fs::path& home) {
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
        if (in < 0 || ::dup2(in, 0) < 0 || ::dup2(out[1], 1) < 0 || ::dup2(out[1], 2) < 0)
            ::_exit(127);
        ::execve(shell.c_str(), argv.data(), env.data());
        ::_exit(127);
    }
    ::close(out[1]);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return status;
}

} // namespace

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
