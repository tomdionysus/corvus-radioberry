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

# The report's path rows: "; slack ; from ; to ; launch clock ; latch clock ;".
# Prints the worst row of each latch clock, in the order the clocks appear.
worst() {
    awk -F';' -v kind="$1" '
        function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
        function leaf(s,    n, a) { n = split(s, a, "|"); return a[n] }
        $1 == "" && NF >= 6 && trim($2) ~ /^-?[0-9]+\.[0-9]+$/ {
            s = trim($2) + 0; c = trim($6)
            if (!(c in slack)) order[++clocks] = c
            if (!(c in slack) || s < slack[c]) {
                slack[c] = s; from[c] = leaf(trim($3)); to[c] = leaf(trim($4))
            }
        }
        END {
            for (i = 1; i <= clocks; i++) {
                c = order[i]
                name = length(c) > 40 ? substr(c, length(c) - 39) : c
                printf "%-5s %-40s %7.3f  %s -> %s\n", kind, name, slack[c], from[c], to[c]
            }
        }' "$2"
}

worst setup "$w/worst.txt"
worst hold "$w/worst-hold.txt"
# The worst strict setup slack of each TX bus pin.
if [ -f "$w/worst-txbus.txt" ]; then
    awk -F';' '
        function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
        $1 == "" && NF >= 6 && trim($2) ~ /^-?[0-9]+\.[0-9]+$/ {
            s = trim($2) + 0; port = trim($4)
            if (!(port in slack) || s < slack[port]) slack[port] = s
        }
        END {
            for (port in slack) {
                note = (port == "ad_tx[4]") ? "  (relaxed in the .sdc)" : ""
                printf "strict %-39s %7.3f  TX bus setup%s\n", port, slack[port], note
            }
        }' "$w/worst-txbus.txt" | LC_ALL=C sort
fi
