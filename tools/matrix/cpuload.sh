#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# cpuload.sh PROTO RATE RECEIVERS SECONDS
# Streams with pNprobe on the Pi itself and reports per-core busy % and
# per-thread CPU % (driver kthread, radioberryd threads, the probe) over
# the measurement window. Receive only.
P=$1; R=$2; N=$3; S=$4
HZ=$(getconf CLK_TCK)
KT=$(pgrep -x radioberry | head -1)
D=$(pgrep -x radioberryd | head -1)

cores() { awk '/^cpu[0-9]/ {b=$2+$3+$4+$7+$8+$9; t=b+$5+$6; print $1, b, t}' /proc/stat; }
threads() {
  for t in /proc/$KT/task/* /proc/$D/task/* $(for p in $(pgrep -x p${P}probe); do echo /proc/$p/task/*; done); do
    [ -r $t/stat ] || continue
    awk -v id="$(basename $t)" '{ s=$0; sub(/^[^(]*\(/,"",s); n=s; sub(/\).*/,"",n); sub(/^[^)]*\) /,"",s); split(s,f," "); print id, n, f[12]+f[13] }' $t/stat
  done
}

p${P}probe --rate $R --receivers $N --seconds $S --settle 1 > /tmp/probe.out 2>&1 &
PP=$!
sleep 1.5
cores > /tmp/c0; threads > /tmp/t0; date +%s.%N > /tmp/w0
sleep $((S - 2))
cores > /tmp/c1; threads > /tmp/t1; date +%s.%N > /tmp/w1
wait $PP; RC=$?
W=$(echo "$(cat /tmp/w1) - $(cat /tmp/w0)" | bc)
echo "== P$P ${N}x${R} ksps, window ${W}s, probe exit $RC"
join /tmp/c0 /tmp/c1 | awk '{ printf "%s %.0f%%  ", $1, 100*($4-$2)/($5-$3) } END { print "" }'
awk 'NR==FNR { a[$1]=$3; n[$1]=$2; next } ($1 in a) { d=$3-a[$1]; if (d>0) printf "  %-16s %5.1f%%\n", $2, 100*d/'"$HZ"'/'"$W"' }' /tmp/t0 /tmp/t1
grep -iE "packet|rate|lost|break|pass|finding" /tmp/probe.out | head -8
