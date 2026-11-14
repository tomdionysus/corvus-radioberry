#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs a gateware testbench with Icarus Verilog in Docker (the image is
# built from tools/sim.Dockerfile on first use). Prints PASS and exits 0 when
# every check passes; the log is in build/sim/<name>.log.
#   tools/sim.sh rpi5       the Raspberry Pi 5 image
#   tools/sim.sh rpi4       the Raspberry Pi 4 image, receive
#   tools/sim.sh rpi4tx     the Raspberry Pi 4 image, transmit
#   tools/sim.sh linktest   the link test image (the default)
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
    rpi5)
        files="rtl/common/rbv_crc8.sv rtl/common/rbv_spi.sv rtl/common/rbv_adseq.sv"
        files="$files rtl/common/rbv_afifo.sv rtl/linktest/rbt_adser.sv"
        files="$files rtl/rpi5/rbv_pio_rx.sv rtl/rpi5/rbv_pio_tx.sv rtl/rpi5/rbv_rpi5_top.sv"
        files="$files sim/rbt_pll_sim.sv sim/radio_stub.sv sim/pio_sm_model.sv sim/rbv_rpi5_tb.sv"
        tb=rbv_rpi5_tb ;;
    *) echo "unknown testbench $which" >&2; exit 2 ;;
esac
mkdir -p "$top/build/sim"
docker run --rm -v "$top:/src" -w /src "$image" sh -c \
    "iverilog -g2012 -Wall -DSINE_HEX=\\\"rtl/linktest/data/sin1024.hex\\\" -s $tb -o /tmp/$tb.vvp $files && vvp -n /tmp/$tb.vvp" |
    tee "$top/build/sim/$which.log"
grep -q '^PASS$' "$top/build/sim/$which.log"
