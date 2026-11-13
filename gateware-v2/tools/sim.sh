#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs the gateware-v2 testbenches with Icarus Verilog in Docker.
#   tools/sim.sh            run the link test testbench
#   tools/sim.sh linktest   the same, by name
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
image=rbv2-sim:trixie
docker image inspect "$image" >/dev/null 2>&1 ||
    docker build -t "$image" -f "$top/tools/sim.Dockerfile" "$top/tools"
which=${1:-linktest}
case "$which" in
    linktest)
        files="rtl/linktest/rbt_spi.sv rtl/linktest/rbt_adser.sv rtl/linktest/rbt_fcount.sv"
        files="$files rtl/linktest/rbt_bist.sv rtl/linktest/rbt_i2s.sv rtl/linktest/rbt_rxcap.sv rtl/linktest/rbt_arm.sv"
        files="$files rtl/linktest/rbt_txtone.sv rtl/linktest/rbt_top.sv"
        files="$files sim/rbt_pll_sim.sv sim/rbt_top_tb.sv"
        tb=rbt_top_tb ;;
    rpi4)
        files="rtl/common/rbv_crc8.sv rtl/common/rbv_spi.sv rtl/common/rbv_adseq.sv"
        files="$files rtl/common/rbv_afifo.sv rtl/common/rbv_i2s.sv rtl/linktest/rbt_adser.sv"
        files="$files rtl/rpi4/rbv_rpi4_top.sv sim/rbt_pll_sim.sv sim/radio_stub.sv sim/rbv_rpi4_tb.sv"
        tb=rbv_rpi4_tb ;;
    rpi4tx)
        files="rtl/common/rbv_crc8.sv rtl/common/rbv_spi.sv rtl/common/rbv_adseq.sv"
        files="$files rtl/common/rbv_afifo.sv rtl/common/rbv_i2s.sv rtl/linktest/rbt_adser.sv"
        files="$files rtl/rpi4/rbv_rpi4_top.sv sim/rbt_pll_sim.sv sim/radio_stub.sv sim/rbv_rpi4tx_tb.sv"
        tb=rbv_rpi4tx_tb ;;
    *) echo "unknown testbench $which" >&2; exit 2 ;;
esac
mkdir -p "$top/build/sim"
docker run --rm -v "$top:/src" -w /src "$image" sh -c \
    "iverilog -g2012 -Wall -DSINE_HEX=\\\"rtl/linktest/data/sin1024.hex\\\" -s $tb -o /tmp/$tb.vvp $files && vvp -n /tmp/$tb.vvp" |
    tee "$top/build/sim/$which.log"
grep -q '^PASS$' "$top/build/sim/$which.log"
