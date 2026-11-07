// The row encoder counts bytes as WeeChat's ascii script sends them (#411).

#include "portable_random.h"
#include "util.h"

#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/mirc.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>

using namespace bw;
using irc::MircCell;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kNoBudget = std::numeric_limits<std::size_t>::max();

// The size the script sends a line in before any cut: what the encoder
// predicts.
std::size_t sent(const std::string& line) { return irc::txtSentBytes(line, kNoBudget); }

// A row as the export hands it to the encoder (encodeCells): trailing black
// spaces dropped unless underlined.
std::vector<MircCell> exportRow(const Canvas& canvas, int y) {
    std::vector<Cell> row;
    for (int x = 0; x < canvas.width(); ++x)
        row.push_back(canvas.at(x, y));
    std::vector<MircCell> cells = irc::mircRow(row);
    while (!cells.empty() && cells.back().ch == U' ' && cells.back().bg == 1 && !attr::showsOnBlank(cells.back().attrs))
        cells.pop_back();
    return cells;
}

struct Checked {
    std::uint64_t sent = 0, writtenMinSent = 0;
};

// The encoder's line sends no more than the line with the fewest bytes as
// written, and its own count of the bytes sent is the script's.
Checked check(std::span<const MircCell> cells, bool swapShades, bool solidEnd) {
    using irc::detail::Objective;
    const auto bySent = irc::detail::encodeMircRowWith(cells, swapShades, solidEnd, Objective::Sent);
    const auto byWritten = irc::detail::encodeMircRowWith(cells, swapShades, solidEnd, Objective::Written);
    CHECK(bySent.line == irc::encodeMircRow(cells, swapShades, solidEnd));
    CHECK(bySent.written == bySent.line.size());
    CHECK(byWritten.written == byWritten.line.size());
    CHECK(bySent.sent == sent(bySent.line));
    CHECK(byWritten.sent == sent(byWritten.line));
    CHECK(sent(bySent.line) <= sent(byWritten.line));
    // at every budget: the script sends a line over its budget in its own
    // shortest encoding where that is shorter, so the sizes step at each
    // line's own size
    const std::size_t a = sent(bySent.line), b = sent(byWritten.line);
    for (const std::size_t budget : {std::size_t(0), a ? a - 1 : 0, a, b ? b - 1 : 0, b, irc::kEfnetWeechatBudget}) {
        INFO("budget " << budget);
        CHECK(irc::txtSentBytes(bySent.line, budget) <= irc::txtSentBytes(byWritten.line, budget));
    }
    // of lines that send the same, the one written shortest
    if (sent(bySent.line) == sent(byWritten.line))
        CHECK(bySent.written == byWritten.written);
    return {sent(bySent.line), sent(byWritten.line)};
}

} // namespace

TEST_CASE("the encoder sends a golden row in no more bytes than the shortest written row") {
    std::vector<fs::path> pieces;
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden"))
        if (e.path().extension() == ".ans")
            pieces.push_back(e.path());
    std::sort(pieces.begin(), pieces.end());
    REQUIRE(pieces.size() >= 12);
    std::uint64_t total = 0, writtenMin = 0;
    for (const auto& p : pieces) {
        const Canvas canvas = parseAnsi(bwtest::readFile(p)).canvas;
        for (int y = 0; y < canvas.height(); ++y) {
            INFO(p.filename().string() << " row " << y + 1);
            const Checked c = check(exportRow(canvas, y), false, false);
            total += c.sent;
            writtenMin += c.writtenMinSent;
        }
    }
    // the goldens send fewer bytes in all
    CHECK(total < writtenMin);
}

TEST_CASE("the encoder sends a random row in no more bytes than the shortest written row") {
    // blocks, shades, a letter, a digit and a comma (which a colour number
    // can swallow), formatted or not, in colours of one and two digits; now
    // and then a wide character, a combining mark in a cell of its own or a
    // control, which the script joins or drops
    constexpr char32_t kGlyphs[] = {U' ', U'█', U'▀', U'▄', U'▌', U'▐', U'░', U'▒', U'▓', U'a', U'1', U',', U' '};
    constexpr char32_t kRare[] = {U'中', U'\u0301', U'\x01', U'\t'};
    constexpr std::uint8_t kColours[] = {0, 1, 2, 3, 4, 7, 10, 12, 15, 52, 98};
    constexpr std::uint8_t kAttrs[] = {attr::Bold, attr::Italic, attr::Underline};
    std::mt19937 rng(411);
    int formatted = 0;
    for (int n = 0; n < 20000; ++n) {
        const int width = 1 + int(bwtest::below(rng, 24));
        // one colour in three of the row's few, so runs and repeats occur
        const int palette = 2 + int(bwtest::below(rng, 4));
        std::vector<std::uint8_t> colours;
        for (int i = 0; i < palette; ++i)
            colours.push_back(kColours[bwtest::below(rng, std::size(kColours))]);
        const bool withAttrs = n % 2 == 1;
        std::vector<MircCell> row;
        for (int x = 0; x < width; ++x) {
            MircCell c;
            c.ch = bwtest::below(rng, 40) == 0 ? kRare[bwtest::below(rng, std::size(kRare))]
                                               : kGlyphs[bwtest::below(rng, std::size(kGlyphs))];
            c.fg = colours[bwtest::below(rng, colours.size())];
            c.bg = colours[bwtest::below(rng, colours.size())];
            if (withAttrs && bwtest::below(rng, 3) == 0)
                for (const std::uint8_t a : kAttrs)
                    if (bwtest::below(rng, 2))
                        c.attrs |= a;
            formatted += c.attrs ? 1 : 0;
            row.push_back(c);
            if (c.ch == Glyph(U'中')) {
                // its continuation, in the lead's colours or others
                MircCell cont = c;
                cont.ch = Glyph::continuation();
                if (bwtest::below(rng, 2)) {
                    cont.fg = colours[bwtest::below(rng, colours.size())];
                    cont.bg = colours[bwtest::below(rng, colours.size())];
                }
                row.push_back(cont);
                ++x;
            }
        }
        const bool swapShades = bwtest::below(rng, 2), solidEnd = bwtest::below(rng, 2);
        INFO("row " << n);
        check(row, swapShades, solidEnd);
    }
    CHECK(formatted > 10000);
}

TEST_CASE("a bold or italic space keeps the foreground the script last wrote") {
    // the script writes no code in front of a bold space on the background
    // already set, so the 4 written there is not sent: the █ after it needs
    // one of its own
    const std::vector<MircCell> row = {{U'▀', 3, 2}, {U' ', 4, 2, attr::Bold}, {U'▀', 4, 2, attr::Bold}};
    const auto e = irc::detail::encodeMircRowWith(row, false, false, irc::detail::Objective::Sent);
    CHECK(e.sent == sent(e.line));
    // an underlined space shows its foreground: the script writes its code
    const std::vector<MircCell> under = {{U'▀', 3, 2}, {U' ', 4, 2, attr::Underline}, {U'▀', 4, 2, attr::Underline}};
    const auto u = irc::detail::encodeMircRowWith(under, false, false, irc::detail::Objective::Sent);
    CHECK(u.sent == sent(u.line));
}

TEST_CASE("a row that sent 390 bytes before #401 fits the budget as sent (#411)") {
    // 16 times: a space 15 on 6, a space 15 on 7, ▐ 7 on 3, a space 15 on 7,
    // ▐ 3 on 1. Shortest as written (352 bytes) it sent 416, over 407; the
    // encoder before #401 wrote 354 and sent 390.
    const Canvas canvas = parseAnsi(bwtest::readFile(bwtest::dataDir() / "irc-sent" / "regress-401.ans")).canvas;
    REQUIRE(canvas.height() >= 1);
    const auto lines = irc::exportCanvas(canvas);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].size() == 352);
    CHECK(irc::txtSentBytes(lines[0]) == 386);
    CHECK(irc::txtOverBudget(lines, irc::kEfnetWeechatBudget).empty());
    const auto written = irc::detail::encodeMircRowWith(exportRow(canvas, 0), false, false, irc::detail::Objective::Written);
    CHECK(written.line.size() == 352);
    CHECK(irc::txtSentBytes(written.line) == 416);
}

TEST_CASE("a formatted row sends no more than its line shortest as written") {
    // the search by bytes as sent prunes the entry that leads to the 49-byte
    // line here (a bold space leaves the script's foreground behind the
    // encoder's) and finds one of 50; the encoder keeps the written line
    const std::vector<MircCell> row = {
        {U'░', 3, 10}, {U'▓', 3, 10, attr::Bold}, {U'1', 3, 3}, {U',', 3, 3}, {U' ', 10, 10}, {U' ', 3, 3, attr::Bold},
        {U'▐', 3, 3, attr::Bold}, {U'1', 10, 10}, {U'▄', 3, 10}, {U'▄', 3, 3}, {U'▓', 3, 3}};
    const auto written = irc::detail::encodeMircRowWith(row, false, true, irc::detail::Objective::Written);
    const auto line = irc::encodeMircRow(row, false, true);
    CHECK(sent(written.line) == 49);
    CHECK(sent(line) == 49);
    CHECK(irc::detail::encodeMircRowWith(row, false, true, irc::detail::Objective::Sent).sent == 49);
}

TEST_CASE("a line over its budget sends no more than the line shortest as written") {
    // as written out the encoder's line would send 408 bytes and the line
    // shortest as written 409, but over 407 the script sends that one in its
    // own shortest encoding, 405 bytes: the encoder keeps it
    std::vector<MircCell> row(387, MircCell{U'a', 0, 4});
    row.push_back({U'▒', 1, 1});
    row.push_back({U'▀', 13, 1});
    row.push_back({U' ', 0, 1});
    const auto written = irc::detail::encodeMircRowWith(row, false, true, irc::detail::Objective::Written);
    REQUIRE(sent(written.line) == 409);
    REQUIRE(irc::txtSentBytes(written.line, irc::kEfnetWeechatBudget) == 405);
    const std::string line = irc::encodeMircRow(row, false, true);
    CHECK(irc::txtSentBytes(line, irc::kEfnetWeechatBudget) <= 405);
    CHECK(irc::txtOverBudget({line}, irc::kEfnetWeechatBudget).empty());
}

TEST_CASE("a row with a mark or a control in a cell of its own is written shortest and measured as sent") {
    // the script joins the mark to the █ before it, in the █'s colours, and
    // drops the control: the count as sent can't follow either, so the
    // encoder writes the line shortest as written and measures it
    const std::vector<MircCell> marked = {{U',', 2, 4}, {U' ', 1, 2}, {U'█', 2, 3}, {U'́', 3, 1}, {U'1', 3, 2}};
    const auto m = irc::detail::encodeMircRowWith(marked, false, false, irc::detail::Objective::Sent);
    const auto mw = irc::detail::encodeMircRowWith(marked, false, false, irc::detail::Objective::Written);
    CHECK(m.line == mw.line);
    CHECK(m.sent == sent(m.line));
    CHECK(mw.sent == sent(mw.line));
    CHECK(sent(m.line) == 21);
    const std::vector<MircCell> control = {{U'\x01', 4, 1}};
    const auto c = irc::detail::encodeMircRowWith(control, false, false, irc::detail::Objective::Sent);
    CHECK(c.line == "\x03" "4,1\x01");
    CHECK(c.sent == 0);
}
