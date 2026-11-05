#pragma once
// Writing a run's outputs all or none: `blockwright-cli replay` and the
// window's screenshots (#295).

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bwcli {

// Open files a run keeps beside its outputs: stdio and Qt's own.
inline constexpr std::size_t kSpareOpenFiles = 64;

// Room for `files` more open files: each output's QSaveFile stays open until
// all are written. Raises the soft limit as far as the hard one allows and
// returns the limit in force when that still leaves too little room (the old
// one when it can't be raised), so the run is refused before any file is
// opened rather than failing part way through.
std::optional<unsigned long long> tooFewOpenFiles(std::size_t files);

// Writes every file (path, bytes) whole, or none of them, each as save
// writes one (through a symlink, keeping the file's permissions, refusing a
// file that isn't writable): a target that is a directory, or a project
// where a flat file goes (as save refuses one), is refused first; then every
// file is written and flushed to its own QSaveFile, and only once all are
// written is each committed into place. A failure drops the files not yet
// committed and removes every file it committed that wasn't there. The
// caller checks the open-file limit (tooFewOpenFiles) first. `beforeCommit`,
// for tests, runs once all are written and before any is committed. Throws
// Failure.
void saveAll(const std::vector<std::pair<std::string, std::string>>& files,
             const std::function<void()>& beforeCommit = {});

} // namespace bwcli
