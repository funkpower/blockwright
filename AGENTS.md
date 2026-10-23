# Blockwright — agent rules

Native ANSI/IRC art editor in C++20 / Qt 6. `PLAN.md` is the design; read it before changing anything structural, and update it in the same PR when a decision changes.

## Build

- CMake + Ninja, Qt 6 (system Qt on Linux). No Rust toolchain.
- Layout: `core/` (no QtWidgets), `render/`, `app/`, `cli/`, `tests/` (Catch2). Tests must not need a display: run with `QT_QPA_PLATFORM=offscreen`.
- Build trees and caches go under `~/.cache/dw/<name>-build`, never inside the repo, `/tmp` or `~/projects`.
- On the dev machine build with `nice -n 10` and at most 8 jobs.
- `ctest` must pass before a PR opens. A PR that adds behaviour adds tests for it.

## Golden files

- Golden files are funkpower's own pieces, copied into `tests/data/`. The maintainer's private notes say where they come from and which reference tools the lint and the `.ans`/SAUCE writer follow. Never copy pieces by anyone else into the repo.
- `tests/data/golden-irc/` is the IRC encoder's fixed oracle (the `irc_oracle` test and `test_irc.cpp` check against it). Change a file there only in a PR that means to change the encoder's output, and say why in the PR.

## Git and the forge

- Remote: `https://git.pumpin.online/funkpower/blockwright.git`. Push with `GIT_ASKPASS=$PWD/scripts/git-askpass`. Commits are made as funkpower (picked up from the remote URL); never as a real name.
- Work on a branch, open a PR with `scripts/forge pr-create <branch> <title> <body-file>`; the PR body is the report. Never push to `main`; PRs are squash-merged through `scripts/forge pr-merge`.
- No Co-Authored-By or "Generated with" trailers. Never force-push except `--force-with-lease` on your own branch.
- Never print the forge token or any `pass` entry.

## Public repo

The repo, its history, issues and PRs are public. Tracked files, commit messages and PR or issue text carry no real names, personal host names, home-directory paths (write `~/…`), email addresses, tokens or other secrets, and nothing about anyone's employer or private infrastructure. People are "funkpower" or "the maintainer"; machines are "the dev machine". PRs are squash-merged, so the PR title and the first paragraph of its body become the commit on `main`.

## Review

Every PR gets an adversarial Codex review before it merges. When Codex is unavailable (out of credits or its session limit), a Claude reviewer stands in for it, one that didn't write the change, and says in its comment that it is the fallback. A later round may go back to Codex once it is available; either the stand-in or Codex may then re-check the stand-in's findings. Findings need a failing test or an exact repro; the author fixes, the same reviewer re-checks. Codex can't run the build, so the PR body states what was built and tested and how.

## Dev install on the dev machine

After every merge to `main`, `scripts/install-dev` (on the dev machine) builds `main` and installs it so the maintainer can try it:

- `~/.local/opt/blockwright-dev/` (the build), `~/.local/bin/blockwright-dev` and `~/.local/bin/blockwright-cli-dev`
- `~/.local/share/applications/blockwright-dev.desktop` named "Blockwright (dev)" with the commit in its comment
- `blockwright-dev --version` prints the short commit
