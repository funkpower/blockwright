#!/usr/bin/env bash
# cli_rlimit_test.sh under translated messages: cli_rlimit_locale_test.sh
# <cli_rlimit_test.sh> <blockwright-cli> <setrlimit shim> <scratch dir>. The
# probe's EPERM must be recognised whatever language bash speaks. Skips (77)
# where no UTF-8 locale besides C is installed or bash has no German
# messages, and passes the inner test's own skip through.
set -uo pipefail
script=$1 cli=$2 shim=$3 tmp=$4

loc=$(locale -a 2>/dev/null | grep -i 'utf-*8$' | grep -v -i -e '^C\.' -e '^POSIX' | head -n 1)
if [ -z "$loc" ]; then echo "skip: no UTF-8 locale besides C is installed"; exit 77; fi
plain=$(LC_ALL=C bash -c 'cd /nonexistent-bw-dir' 2>&1)
german=$(LANGUAGE=de LC_ALL=$loc bash -c 'cd /nonexistent-bw-dir' 2>&1)
if [ "$plain" = "$german" ]; then echo "skip: bash has no German messages under $loc"; exit 77; fi

echo "LANGUAGE=de LC_ALL=$loc"
LANGUAGE=de LC_ALL=$loc bash "$script" "$cli" "$shim" "$tmp"
