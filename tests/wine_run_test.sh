#!/usr/bin/env bash
# wine_run_test.sh <wine-run> <wineserver> <scratch dir>: a program run
# through wine-run hands back its output as soon as it exits (#259): when no
# session is running, when several start at once, and when one starts just as
# the session's last program exits, while the server is shutting down, and
# when a server started by hand without services is up (#273). A
# program that starts a session starts wine's services, which hold its output
# open until the session ends, and ctest would wait on them. The scratch dir
# holds a wine prefix of its own, so stopping its server touches no other test.
set -uo pipefail
run=$1 server=$2 tmp=$3
export BW_WINEPREFIX=$tmp/prefix WINEPREFIX=$tmp/prefix
mkdir -p "$tmp/prefix"
stop() { "$server" -k >/dev/null 2>&1; "$server" -w >/dev/null 2>&1; }
trap stop EXIT
fails=0

# probe <name>: runs `cmd /c echo <name>` with its output and errors on a pipe
# (which ends only when every process holding its write end has exited), in
# the background; check <name> then checks what it wrote.
probe() {
  (timeout 240 "$run" cmd /c echo "$1" 2>&1 | tr -d '\r' >"$tmp/out-$1"; echo "${PIPESTATUS[0]}" >"$tmp/status-$1") &
}
# The server outlives the programs by 60 s: had a service held a pipe, that
# pipe would have ended with the session, and no server would be running.
running() { "$server" -p60 </dev/null >/dev/null 2>&1; [ $? = 2 ]; }
check() {
  local status out
  status=$(<"$tmp/status-$1") out=$(<"$tmp/out-$1")
  if [ "$status" != 0 ]; then
    echo "FAIL $1: exit $status: $out"; fails=$((fails + 1))
  elif [ "$out" != "$1" ]; then
    echo "FAIL $1: output $out"; fails=$((fails + 1))
  elif [ "$2" != yes ]; then
    echo "FAIL $1: the output ended only when the session did"; fails=$((fails + 1))
  else
    echo "ok   $1"
  fi
}

# several at once, with no session running
for wave in 1 2; do
  stop
  for i in 1 2 3 4; do probe "cold$wave-$i"; done
  wait
  up=no; running && up=yes
  for i in 1 2 3 4; do check "cold$wave-$i" $up; done
done

# one just after the session's last program exits
for delay in 0 0.01 0.02 0.03 0.04 0.05 0.06 0.08 0.1; do
  stop
  "$run" cmd /c echo first >/dev/null 2>&1
  sleep "$delay"
  probe "after-$delay"
  wait
  up=no; running && up=yes
  check "after-$delay" $up
done

# one after a server was started by hand with `wineserver -p`, which has no
# services and stays up until it is killed (#273): the output has to end
# while that server is still up, so it is waited for here, 60 s at most,
# before the session is stopped
stop
"$server" -p </dev/null >/dev/null 2>&1
probe bare
for _ in $(seq 600); do
  [ -e "$tmp/status-bare" ] && break
  sleep 0.1
done
up=no; [ -e "$tmp/status-bare" ] && up=yes
stop
wait
check bare $up

# nothing that outlives wine-run keeps its lock
if ls -l /proc/[0-9]*/fd 2>/dev/null | grep -qF "$tmp/prefix/.wine-run.lock"; then
  echo "FAIL a process still holds wine-run's lock"; fails=$((fails + 1))
fi
[ "$fails" = 0 ] || { echo "$fails failure(s)"; exit 1; }
