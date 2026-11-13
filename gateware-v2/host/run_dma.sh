#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run_dma.sh DIV RXPH RXOFF SECONDS [TXPOS] [PERIOD_WORDS]
# One two-way I2S run with rbpcm_dma. Counts are taken between a point 1 s
# after the start and the end, so the start of the stream is left out.
# Prints one JSON line. Run as root in the directory holding linktest.py
# and kmod/rbpcm_dma.ko, with GPIO 18 to 21 in their PCM function.
set -eu
div=$1; rxph=$2; rxoff=$3; secs=$4; txpos=${5:-0}; pw=${6:-4096}
L="python3 linktest.py"
stat() { awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8, $7, $8}' /proc/stat; }
kv() { awk -v k="$1" '$1==k{print $2}' /sys/kernel/debug/rbpcm_dma/stats; }

$L i2s-start "$div" "$rxph" 1 "$rxoff"
sleep 0.2
insmod kmod/rbpcm_dma.ko txpos="$txpos" period_words="$pw"
sleep 1
fa=$($L i2s-stat)
ra=$(kv rx_words); ba=$(kv rx_breaks); ca=$(kv callback_ns_total); xa=$(kv tx_words_consumed)
ea=$(kv rx_fifo_errors); ta=$(kv tx_fifo_errors); sa=$(stat); t0=$(date +%s.%N)
sleep "$secs"
fb=$($L i2s-stat)
rb=$(kv rx_words); bb=$(kv rx_breaks); cb=$(kv callback_ns_total); xb=$(kv tx_words_consumed)
eb=$(kv rx_fifo_errors); tb=$(kv tx_fifo_errors); mx=$(kv callback_ns_max); sb=$(stat); t1=$(date +%s.%N)
rmmod rbpcm_dma
$L i2s-stop
python3 - "$fa" "$fb" <<PY
import json, sys
fa, fb = json.loads(sys.argv[1]), json.loads(sys.argv[2])
dt = $t1 - $t0
sa = "$sa".split(); sb = "$sb".split()
jiff = int(sb[0]) - int(sa[0])
print(json.dumps({
  "div": $div, "bit_clock_mhz": round(122.88 / $div, 3), "rxph": $rxph, "rxoff": $rxoff,
  "txpos": $txpos, "period_words": $pw, "seconds": round(dt, 1),
  "to_pi": {"words": $rb - $ra, "breaks": $bb - $ba, "fifo_errors": $eb - $ea},
  "to_fpga": {"words": fb["rx_words"] - fa["rx_words"], "errors": fb["rx_errors"] - fa["rx_errors"],
              "pi_fifo_errors": $tb - $ta},
  "callback_pct_of_one_core": round(($cb - $ca) / 1e9 / dt * 100, 2),
  "callback_max_us": round($mx / 1e3, 1),
  "irq_softirq_pct_of_all_cores": round((int(sb[1]) + int(sb[2]) - int(sa[1]) - int(sa[2])) / jiff * 100, 2),
}))
PY
