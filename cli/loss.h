#pragma once
// The warnings for the cells a write couldn't hold, as blockwright-cli
// prints them after "blockwright-cli: warning: <file>: ".

#include "blockwright/ansi.h"

#include <string>
#include <vector>

namespace bwcli {

// One warning per kind of loss in what writeAnsi wrote, with a count and
// the first cell of that kind.
std::vector<std::string> unwritableWarnings(const bw::AnsiOutput& res);

// The same for IRC text (bw::irc::unwritable): UTF-8, so worded as the
// UTF-8 .ans warnings are.
std::vector<std::string> ircUnwritableWarnings(const std::vector<bw::Unwritable>& lost);

} // namespace bwcli
