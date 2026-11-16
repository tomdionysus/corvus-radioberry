#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs kt.sh on HOST for each RELEASE: stage, tryboot, run, remove.
#   kmatrix.sh HOST OUTDIR RELEASE...
# Environment FW, BASE, CMDLINE, TOOLS are passed to kt.sh (see there).
# Each kernel is staged from the boot before it and entered by tryboot,
# so a Bookworm system on another partition stays on Bookworm throughout;
# HOSTNAME_EXPECT, when set, must match on every boot or the run stops.
H=$1; OUT=$2
shift 2
S=$(dirname "$0")
E="FW=${FW:-/boot/firmware} BASE=${BASE:-} CMDLINE=${CMDLINE:-} TOOLS=${TOOLS:-/root/rbtools}"
scp -q "$S/kt.sh" "$S/cpuload.sh" "$H:/root/"
on() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$H" "$@"; }
wait_up() {
    i=0
    until on 'test $(cut -d. -f1 /proc/uptime) -lt 900 && touch /run/rb-keep' 2>/dev/null; do
        sleep 10
        i=$((i + 1))
        [ $i -gt 60 ] && return 1
    done
    return 0
}
for rel in "$@"; do
    echo "=== $rel $(date +%T)"
    on "env $E sh /root/kt.sh stage $rel" || { echo "STAGE FAILED $rel"; continue; }
    on '(sleep 1; reboot "0 tryboot") >/dev/null 2>&1 &'
    sleep 40
    if ! wait_up; then echo "BOOT TIMEOUT $rel"; break; fi
    got=$(on uname -r)
    host=$(on hostname)
    if [ -n "${HOSTNAME_EXPECT:-}" ] && [ "$host" != "$HOSTNAME_EXPECT" ]; then
        echo "BOOTED $host, NOT $HOSTNAME_EXPECT: stopping"; break
    fi
    if [ "$got" != "$rel" ]; then
        echo "BOOTED $got, NOT $rel"
    else
        on "sleep 15; touch /run/rb-keep; mkdir -p $OUT; env $E sh /root/kt.sh run $OUT/$rel.txt"
    fi
    on "rm -rf ${FW:-/boot/firmware}/k-$rel"
done
echo "=== done $(date +%T)"
