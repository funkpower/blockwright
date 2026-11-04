#include "loss.h"

#include "blockwright/cp437.h"
#include "blockwright/glyph.h"

#include <algorithm>
#include <optional>

namespace bwcli {

namespace {

using bw::Loss;
using bw::Unwritable;

// A control or bidi override is shown by its code point alone, never raw;
// a glyph not written '?' says what it is written as.
std::string where(const Unwritable& u) {
    const std::optional<char32_t> cp = u.ch.codePoint();
    const std::string glyph = cp && bw::unprintableCodePoint(*cp) ? "" : " '" + bw::quotedGlyph(u.ch) + "'";
    return "(first: " + bw::codePointNames(u.ch) + glyph + " at column " + std::to_string(u.x + 1) + ", row " +
           std::to_string(u.y + 1) + (u.written == U'?' ? "" : ", written as " + bw::quotedGlyph(u.written)) + ")";
}

struct Warnings {
    const std::vector<Unwritable>& lost;
    std::vector<std::string> out;

    template <class Which>
    void add(Which which, const std::string& one, const std::string& many, const std::string& after = {}) {
        const int n = int(std::count_if(lost.begin(), lost.end(), which));
        if (n == 0)
            return;
        const Unwritable& u = *std::find_if(lost.begin(), lost.end(), which);
        out.push_back(std::to_string(n) + (n == 1 ? one : many) + " " + where(u) + after);
    }
};

auto is(Loss loss) {
    return [loss](const Unwritable& u) { return u.loss == loss; };
}

// A UTF-8 writer's losses, `holder` naming what it writes: a control
// character, the same before a continuation (a lead, whose continuation is
// written as a space), and a lead a terminal wouldn't draw over both cells.
void utf8Warnings(Warnings& w, const std::string& holder) {
    w.add([](const Unwritable& u) { return u.loss == Loss::Control && !u.lead; },
          " cell holds a control character " + holder + " can't hold and is written as '?'",
          " cells hold control characters " + holder + " can't hold and are written as '?'");
    w.add([](const Unwritable& u) { return u.loss == Loss::Control && u.lead; },
          " cell holds a control character " + holder +
              " can't hold before a continuation and is written as '?' and a space",
          " cells hold control characters " + holder +
              " can't hold before a continuation and are written as '?' and a space");
    w.add(is(Loss::Wide),
          " cell holds a glyph a terminal doesn't draw two columns wide before a continuation and is "
          "written as '?' and a space",
          " cells hold glyphs a terminal doesn't draw two columns wide before a continuation and are "
          "written as '?' and a space");
}

} // namespace

std::vector<std::string> unwritableWarnings(const bw::AnsiOutput& res) {
    Warnings w{res.unwritable, {}};
    if (res.encoding == bw::TextEncoding::Utf8) {
        utf8Warnings(w, "a UTF-8 .ans");
    } else {
        w.add(is(Loss::NoCode), " glyph has no CP437 code and is written as '?'",
              " glyphs have no CP437 code and are written as '?'");
        w.add(is(Loss::Wide), " wide glyph is written as '?' and a space", " wide glyphs are written as '?' and a space");
        w.add(is(Loss::MarksDropped), " glyph loses its combining marks", " glyphs lose their combining marks");
        const int controls = int(std::count_if(res.unwritable.begin(), res.unwritable.end(), is(Loss::Control)));
        const std::string names = bw::ansControlNames(res.unwritable);
        w.add(is(Loss::Control), " glyph has a CP437 byte .ans reads as a control code (" + names + ") and is written as '?'",
              " glyphs have CP437 bytes .ans reads as control codes (" + names + ") and are written as '?'",
              std::string("; XBIN and BIN keep ") + (controls == 1 ? "it" : "them"));
    }
    return w.out;
}

std::vector<std::string> ircUnwritableWarnings(const std::vector<Unwritable>& lost) {
    Warnings w{lost, {}};
    utf8Warnings(w, "IRC text");
    return w.out;
}

} // namespace bwcli
