#!/usr/bin/env bash
# install-dev path guards: install_dev_test.sh <scripts/install-dev> <scratch dir>
#
# Runs against a throwaway HOME, with rm, git, cmake, tar, mv, ln and mkdir
# replaced by stubs that log and fail, so a guard that lets a path through
# can't delete or build anything: the stub log shows it instead.
set -uo pipefail
script=$1 tmp=$2
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

home=$tmp/home
mkdir -p "$home/.cache/dw" "$home/.local/opt" "$tmp/stubs" "$tmp/outside"
echo keep > "$home/canary"
echo keep > "$tmp/outside/canary"
ln -s / "$home/.cache/dw/root-link"
log=$tmp/stub.log
for cmd in rm git cmake tar mv ln mkdir nice; do
  printf '#!/bin/sh\necho "%s $*" >> "%s"\nexit 97\n' "$cmd" "$log" > "$tmp/stubs/$cmd"
  chmod +x "$tmp/stubs/$cmd"
done

# refused <name> <args...>: install-dev exits 2 with a refusal and runs nothing
refused() {
  local name=$1; shift
  : > "$log"
  local out st
  out=$(HOME=$home PATH=$tmp/stubs:$PATH bash "$script" "$@" 2>&1); st=$?
  if [[ $st -ne 2 ]]; then bad "$name: exit $st, wanted 2: $out"; return; fi
  if ! grep -q 'refusing' <<<"$out"; then bad "$name: no refusal message: $out"; return; fi
  if [[ -s $log ]]; then bad "$name: ran commands: $(tr '\n' ';' < "$log")"; return; fi
  ok "$name"
}

# accepted <name> <args...>: --check-paths passes the guard and runs nothing
accepted() {
  local name=$1; shift
  : > "$log"
  local out st
  out=$(HOME=$home PATH=$tmp/stubs:$PATH bash "$script" --check-paths "$@" 2>&1); st=$?
  if [[ $st -ne 0 ]]; then bad "$name: exit $st, wanted 0: $out"; return; fi
  if [[ -s $log ]]; then bad "$name: ran commands: $(tr '\n' ';' < "$log")"; return; fi
  ok "$name"
}

refused "--build-dir ''" --build-dir ''
refused "--build-dir /" --build-dir /
refused "--build-dir \$HOME" --build-dir "$home"
refused "--build-dir ~/.cache/dw itself" --build-dir "$home/.cache/dw"
refused "--build-dir outside ~/.cache/dw" --build-dir "$tmp/outside"
refused "--build-dir climbing out with .." --build-dir "$home/.cache/dw/../.."
refused "--build-dir through a symlink to /" --build-dir "$home/.cache/dw/root-link"
refused "--prefix ''" --prefix ''
refused "--prefix /" --prefix /
refused "--prefix \$HOME" --prefix "$home"
refused "--prefix ~/.local/opt itself" --prefix "$home/.local/opt"
refused "--prefix outside ~/.local/opt" --prefix "$tmp/outside"
refused "--bin-dir ''" --bin-dir ''
refused "--bin-dir /" --bin-dir /
refused "--bin-dir \$HOME" --bin-dir "$home"
refused "--bin-dir \$HOME/" --bin-dir "$home/"
refused "--bin-dir outside \$HOME" --bin-dir "$tmp/outside"
refused "--bin-dir climbing out with .." --bin-dir "$home/.local/bin/../../.."
refused "--bin-dir through a symlink to /" --bin-dir "$home/.cache/dw/root-link/bin"
accepted "default paths" 
accepted "build dir and prefix inside their roots" --build-dir "$home/.cache/dw/x-build" --prefix "$home/.local/opt/x"
accepted "bin dir below \$HOME" --bin-dir "$home/.local/bin"

# A symlinked root, or a symlink on the way to it from $HOME, is refused even
# when the path given looks inside it: with ~/.local/opt -> $HOME,
# --prefix ~/.local/opt/projects would resolve to ~/projects.
symlinked() { # <name> <dir to replace with a symlink to $HOME> <args...>
  local name=$1 dir=$2; shift 2
  mv "$dir" "$dir.real"
  ln -s "$home" "$dir"
  refused "$name" "$@"
  rm "$dir" && mv "$dir.real" "$dir"
}
mkdir -p "$home/projects" "$home/.ssh"
symlinked "~/.local/opt a symlink" "$home/.local/opt" --prefix "$home/.local/opt/projects"
symlinked "~/.local a symlink" "$home/.local" --prefix "$home/.local/opt/projects"
symlinked "~/.cache/dw a symlink" "$home/.cache/dw" --build-dir "$home/.cache/dw/.ssh"
symlinked "~/.cache a symlink" "$home/.cache" --build-dir "$home/.cache/dw/.ssh"
symlinked "~/.local/opt a symlink, default paths" "$home/.local/opt"

if [[ -f $home/canary && -f $tmp/outside/canary && -d $home/.cache/dw && -d $home/.local/opt ]]; then
  ok "nothing was deleted"
else
  bad "something was deleted"
fi

rm -f "$home/.cache/dw/root-link"
echo "$fails failure(s)"
[[ $fails -eq 0 ]]
