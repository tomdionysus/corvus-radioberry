#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# run_i2s.sh DIV RXPH MODE RXOFF TXPOS SECONDS: one two-way I2S run.
# Prints the host result and the FPGA checker delta over the middle of the run.
div=$1; rxph=$2; mode=$3; rxoff=$4; txpos=$5; secs=$6
python3 linktest.py i2s-start $div $rxph $mode $rxoff
chrt -f 90 taskset -c 3 ./pcmpoll $secs $mode $txpos > host.json &
sleep 1
a=$(python3 linktest.py i2s-stat)
sleep $((secs - 2))
b=$(python3 linktest.py i2s-stat)
wait
python3 linktest.py i2s-stop
python3 - "$a" "$b" <<PY
import json,sys
a,b=json.loads(sys.argv[1]),json.loads(sys.argv[2]); h=json.load(open("host.json"))
print(json.dumps({"div":$div,"rxph":$rxph,"rxoff":$rxoff,"txpos":$txpos,"mode":$mode,
 "bit_clock_hz":h["bit_clock_hz"],
 "to_pi":{"words":h["rx_words"],"link_errors":h["link_errors"],"breaks":h["sequence_breaks"],"rx_overruns":h["rx_overruns"]},
 "to_fpga":{"words":b["rx_words"]-a["rx_words"],"errors":b["rx_errors"]-a["rx_errors"],"tx_underruns":h["tx_underruns"],
  "last":b["rx_last"],"first_err_got":b["first_error_got"],"first_err_want":b["first_error_want"]}}))
PY
