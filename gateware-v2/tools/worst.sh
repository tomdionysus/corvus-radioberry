#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Prints the worst setup slack of every clock of a built project:
#   tools/worst.sh PROJECT
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
p=$1
rm -f "$top/build/work-$p/worst.txt"
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$top:/src" \
    -w "/src/build/work-$p" radioberry-quartus:23.1std.1 quartus_sta -t /src/tools/quartus/worst.tcl "$p" >/dev/null 2>&1
python3 - "$top/build/work-$p/worst.txt" <<PY
import re, sys
w = {}
for l in open(sys.argv[1]):
    m = re.match(r"; (-?\d+\.\d+) ; (\S+)\s*; (\S+)\s*; (\S+)\s*; (\S+)", l)
    if m:
        s, c = float(m.group(1)), m.group(5)
        if c not in w or s < w[c][0]: w[c] = (s, m.group(2).split("|")[-1], m.group(3).split("|")[-1])
for c, v in w.items(): print("%-40s %7.3f  %s -> %s" % (c[-40:], v[0], v[1], v[2]))
PY
