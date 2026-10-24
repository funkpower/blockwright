#pragma once
// `blockwright-cli lint`: the whole lint (lint_rules.h) of a piece, as text
// or JSON.

#include "blockwright/ansi.h"
#include "blockwright/lint_rules.h"

#include <optional>
#include <string>
#include <vector>

namespace bwcli {

struct LintCliOptions {
    // The ramps the ladder check runs under; empty: every named ramp.
    std::vector<bw::Ramp> ramps;
    std::optional<bw::lint::Box> box;
    bool json = false;
};

// The lint of the document with its own iCE colours. Throws Failure when
// the box is off the canvas, as shade does.
bw::lint::LintReport lintDocument(const bw::AnsiDocument& doc, const LintCliOptions& o);

// One line per finding, "x,y kind ramp message" ('-' for no ramp), then a
// summary line: the count by kind, the noise and whether iCE is on.
std::string lintText(const bw::lint::LintReport& r);

// {"findings":[{"kind","ramp","x","y","x2","y2","message"}],"noise":N,"ice":bool},
// "ramp" null for a finding of no ramp; one line.
std::string lintJson(const bw::lint::LintReport& r);

} // namespace bwcli
