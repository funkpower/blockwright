// Golden round trips over real pieces in tests/data/golden/.
//
// parse -> write must give the file back byte for byte. Pieces listed in
// golden/NONCANONICAL (name, then why) can't, because the source isn't in the
// writer's canonical form; for those the cell grid must survive a second
// round trip and the second write must equal the first.

#include "util.h"

#include "blockwright/ansi.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <sstream>

using namespace bw;
namespace fs = std::filesystem;

namespace {

fs::path goldenDir() { return bwtest::dataDir() / "golden"; }

std::map<std::string, std::string> nonCanonical() {
    std::map<std::string, std::string> out;
    std::istringstream in(bwtest::readFile(goldenDir() / "NONCANONICAL"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t sp = line.find(' ');
        out[line.substr(0, sp)] = sp == std::string::npos ? "" : line.substr(sp + 1);
    }
    return out;
}

std::vector<fs::path> pieces() {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(goldenDir()))
        if (e.path().extension() == ".ans")
            out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// First differing byte, for a readable failure.
std::string firstDiff(const std::string& a, const std::string& b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i])
        ++i;
    std::ostringstream ss;
    ss << "sizes " << a.size() << " vs " << b.size() << ", first difference at byte " << i;
    return ss.str();
}

} // namespace

TEST_CASE("Golden set has at least 10 pieces", "[golden]") {
    CHECK(pieces().size() >= 10);
    for (const auto& [name, why] : nonCanonical()) {
        INFO(name);
        CHECK(fs::exists(goldenDir() / name));
        CHECK_FALSE(why.empty());
    }
}

TEST_CASE("Golden pieces round-trip", "[golden]") {
    const auto nc = nonCanonical();
    for (const auto& path : pieces()) {
        const std::string name = path.filename().string();
        DYNAMIC_SECTION(name) {
            const std::string src = bwtest::readFile(path);
            const AnsiDocument doc = parseAnsi(src);
            const std::string out = writeAnsi(doc).bytes;
            if (!nc.count(name)) {
                INFO(firstDiff(src, out));
                CHECK(out == src);
            } else {
                // listed as non-canonical: it must really differ, or the list is stale
                CHECK(out != src);
                const AnsiDocument again = parseAnsi(out);
                CHECK(again.canvas == doc.canvas);
                CHECK(again.ice == doc.ice);
                CHECK(writeAnsi(again).bytes == out);
            }
        }
    }
}

TEST_CASE("A real piece with its SAUCE FileSize zeroed loads the same grid", "[golden][sauce]") {
    const std::string src = bwtest::readFile(goldenDir() / "durin-gate.ans");
    std::string zeroed = src;
    const size_t at = zeroed.size() - 128 + 90;
    REQUIRE(zeroed.compare(zeroed.size() - 128, 5, "SAUCE") == 0);
    zeroed.replace(at, 4, std::string(4, '\0'));
    const AnsiDocument a = parseAnsi(src), b = parseAnsi(zeroed);
    REQUIRE(b.canvas.height() == a.canvas.height());
    CHECK(b.canvas == a.canvas);
    CHECK(b.sauce->fileSize == 0);
    // and writing it puts the real FileSize back
    CHECK(writeAnsi(b).bytes == src);
}
