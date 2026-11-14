#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Prints the worst setup and hold slack of every clock of a built project:
#   tools/worst.sh PROJECT
# For a project with an AD9866 TX bus it also prints each TX pin's setup
# slack under the strict constraint. ad_tx[4] (pin 105) is relaxed in the
# project's constraints, so its "strict" line is the only check of it;
# a negative value there is known (docs/hardware-discovery.md section 4,
# "Pin 105"; docs/review.md finding 1.3) and does not fail anything.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
p=$1
w="$top/build/work-$p"
rm -f "$w/worst.txt" "$w/worst-hold.txt" "$w/worst-txbus.txt"
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$top:/src" \
    -w "/src/build/work-$p" radioberry-quartus:23.1std.1 quartus_sta -t /src/tools/quartus/worst.tcl "$p" >/dev/null 2>&1
python3 - "$w/worst.txt" "$w/worst-hold.txt" "$w/worst-txbus.txt" <<PY
import os, re, sys
row = re.compile(r"; (-?\d+\.\d+)\s+;\s*(\S+)\s*; (\S+)\s*; (\S+)\s*; (\S+)")
for kind, path in zip(("setup", "hold"), sys.argv[1:3]):
    w = {}
    for l in open(path):
        m = row.match(l)
        if m:
            s, c = float(m.group(1)), m.group(5)
            if c not in w or s < w[c][0]: w[c] = (s, m.group(2).split("|")[-1], m.group(3).split("|")[-1])
    for c, v in w.items(): print("%-5s %-40s %7.3f  %s -> %s" % (kind, c[-40:], v[0], v[1], v[2]))
if os.path.exists(sys.argv[3]):
    w = {}
    for l in open(sys.argv[3]):
        m = row.match(l)
        if m:
            s, port = float(m.group(1)), m.group(3)
            if port not in w or s < w[port]: w[port] = s
    for port in sorted(w): print("strict %-39s %7.3f  TX bus setup%s" % (port, w[port], "  (relaxed in the .sdc)" if port == "ad_tx[4]" else ""))
PY
