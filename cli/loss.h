#pragma once
// The warnings for the cells a write couldn't hold, as blockwright-cli
// prints them after "blockwright-cli: warning: <file>: ".

#include "blockwright/ansi.h"
#include "blockwright/project.h"

#include <string>
#include <vector>

namespace bwcli {

// One warning per kind of loss in what writeAnsi wrote, with a count and
// the first cell of that kind.
std::vector<std::string> unwritableWarnings(const bw::AnsiOutput& res);

// The same for IRC text (bw::irc::unwritable): UTF-8, so worded as the
// UTF-8 .ans warnings are.
std::vector<std::string> ircUnwritableWarnings(const std::vector<bw::Unwritable>& lost);

// What a UTF-8 .ans read dropped (bw::ReadLoss): "3 combining code points
// past 16 dropped at 4,0", "12 clusters read as their base: too many
// distinct clusters".
std::vector<std::string> readWarnings(const bw::ReadLoss& loss);

// What a project read changed (bw::project::ReadNotes), printed after
// "note: <file>: " since the read goes on: "cleared attribute bits no
// earlier build wrote from 3 cells", the app's status-bar wording (#370).
std::vector<std::string> projectReadNotes(const bw::project::ReadNotes& notes);

} // namespace bwcli
