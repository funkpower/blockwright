#pragma once
// `blockwright-cli add-generator` and `run-generators`: a generator's
// directory read into the files a project keeps under scripts/<id>/
// (generator.h), and the project's generator layers run again.

#include "blockwright/generator.h"
#include "blockwright/project.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bwcli {

// At most this many files and bytes in a generator's directory.
inline constexpr std::size_t kMaxGeneratorFiles = 1000;
inline constexpr std::size_t kMaxGeneratorBytes = std::size_t{64} << 20;

// The files under `dir`, by their path from it ('/'-separated), as
// generator::add takes them. Hidden files and directories (a leading '.',
// such as .git) are skipped; a symbolic link or anything else that isn't a
// plain file or directory is refused, as a project directory refuses one,
// and so is a directory over the limits above. Throws Failure.
std::map<std::string, std::string> readGeneratorDir(const std::filesystem::path& dir);

// Readies this program to run generators: puts SIGCHLD back to its default
// action (an ignored SIGCHLD, which generator::run refuses, survives exec,
// so a parent can hand one down), and makes SIGINT, SIGTERM and SIGHUP
// (each unless ignored) kill the generator running
// (generator::killRunning) before they end this program as they would have.
// A generator leads a process group of its own, so the terminal's ^C no
// longer reaches it.
// On Windows a console's Ctrl+C, Ctrl+Break and close, a logoff and a
// shutdown kill it before the default handler ends this program; the
// generator's job object goes with this program however it ends.
void prepareToRunGenerators();

// The ids `wanted` names, each a generator layer of the stack, or every
// visible generator layer when it is empty, bottom first. A hidden layer
// runs only when `wanted` names it (#440); with `wanted` empty the hidden
// ones go in `hidden`, bottom first, when it is given. Throws Failure for an
// id that isn't one.
std::vector<bw::LayerId> generatorsToRun(const bw::LayerStack& stack, const std::vector<bw::LayerId>& wanted,
                                         std::vector<bw::LayerId>* hidden = nullptr);

} // namespace bwcli
