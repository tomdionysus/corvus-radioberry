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
#   tools/sim.sh i2seq      rbv_i2s against the version released in v0.6.0
#   tools/sim.sh rxref [RATE N [fold]]
#                           one Hermes-Lite 2 receiver, the reference for a
#                           reworked receive chain: RATE 40, 20, 10 or 5
#                           (48 to 384 ksps, default 5), N output samples
#                           (default 200), written to build/sim/rxref-RATE.hex;
#                           with "fold", the folded filter (rtl/core/firfold.sv,
#                           the arithmetic of the images' filter, one receiver),
#                           to build/sim/rxref-fold-RATE.hex
#   tools/sim.sh firsharedfold [RATE N [LAST]]
#                           the images' receive filter (rtl/core/firsharedfold.sv)
#                           against six receivers with firfold.sv's filter
#                           (the same sums), value for value: N outputs
#                           (default 100) of receivers 0..LAST (default 5)
#   tools/sim.sh cicprune [N]
#                           the pruned CICs against the exact ones of
#                           takeover stage 2a, N outputs a decimation
#   tools/sim.sh sincoseq [N]
#                           rtl/core/sincos.sv against the core's sincos.v,
#                           every clock
#   tools/sim.sh txciceq [N]
#                           rtl/core/tx_cic.sv against the core's
#                           CicInterpM5.v, every clock
#   tools/sim.sh txfireq [N]
#                           rtl/core/tx_fir.sv against the core's
#                           FirInterp8_1024.v, every clock
#   tools/sim.sh cwshape [HOLD]
#                           the CW envelope, straight line against
#                           rtl/core/cw_shape.sv's raised cosine
#   tools/sim.sh failsafe   every keying path stops when the host does
#                           (the rpi4-i2s transmit build, real core)
#   tools/sim.sh mixeq [N]
#                           rtl/core/mix2.sv against the core's mix2.v,
#                           every clock
#   tools/sim.sh corecmp [REV [CLOCKS]]
#                           rtl/core/rbv_core.sv against the Hermes-Lite 2
#                           based core top at REV (default b8b7fc9), the
#                           driver's command translation between, every
#                           output on every clock
#   tools/sim.sh radioeq [REV [CLOCKS [TOL]]]
#                           the Hermes-Lite 2 based core top at revision
#                           NEW (default b8b7fc9) against its version at
#                           git revision REV, every output on every clock;
#                           CLOCKS for each receive rate (default 120000);
#                           receive words within TOL (24-bit units);
#                           REF_PRUNED=1 for a REV from 19c1441 on;
#                           TX_NEW=1 against a REV before the new
#                           up-converter (DAC words not compared,
#                           PureSignal off)
# With VERILATOR=1 it compiles the testbench with the Verilator installed
# on this machine (VJOBS compile jobs); with SIM_LOCAL=1 it runs the Icarus
# installed on this machine instead of
# Docker; build/simlib/ must then hold the Quartus simulation models that
# rxref, firsharedfold and radioeq use (copied from a machine with the
# Quartus image).
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
image=rbv2-sim:trixie
[ -n "${SIM_LOCAL:-}${VERILATOR:-}" ] || docker image inspect "$image" >/dev/null 2>&1 ||
    docker build -t "$image" -f "$top/tools/sim.Dockerfile" "$top/tools"
which=${1:-linktest}
run_dir=.
plusargs=
defines=
case "$which" in
    i2seq)
        mkdir -p "$top/build/sim"
        git -C "$top" show v0.6.0:gateware/rtl/common/rbv_i2s.sv |
            sed 's/^module rbv_i2s (/module rbv_i2s_ref (/' >"$top/build/sim/rbv_i2s_ref.sv"
        files="rtl/common/rbv_i2s.sv build/sim/rbv_i2s_ref.sv sim/rbv_i2s_eq_tb.sv"
        tb=rbv_i2s_eq_tb ;;
    rxref)
        # The Hermes-Lite 2 core as the images build it, and Quartus's
        # simulation models of its memories (altera_mf.v, from the Quartus
        # image; not part of this repository).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl/radio_openhpsdr1
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        if [ ! -f "$top/build/simlib/altera_mf.v" ]; then
            mkdir -p "$top/build/simlib"
            docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
                -v "$top/build/simlib:/out" --entrypoint sh radioberry-quartus:23.1std.1 \
                -c 'cp /opt/altera/quartus/eda/sim_lib/altera_mf.v /out/'
        fi
        files="$core/cic_integrator.v $core/cic_comb.v $core/cic.v $core/varcic.v"
        files="$files $core/firram36.v $core/firrom/firromH.v $core/receiver_nco.v"
        rate=${2:-5}
        name=rxref-$rate
        if [ "${4:-}" = fold ]; then
            files="$files rtl/core/firfold.sv"
            name=rxref-fold-$rate
            fold="-DFIR_FOLD -DFIR_FOLD_SIM"
        else
            files="$files $core/firfilt.v"
        fi
        files="$files build/simlib/altera_mf.v sim/rx_ref_tb.sv"
        tb=rx_ref_tb
        # The coefficient files are read from the directory the run starts in.
        run_dir=$core/firrom
        defines="-DUSE_ALTSYNCRAM=1 ${fold:-}"
        plusargs="+RATE=$rate +N=${3:-200} +OUT=/src/build/sim/$name.hex +FIRIN=/src/build/sim/$name.firin"
        which=$name ;;
    firsharedfold)
        # The images' filter against six receivers with firfold.sv's filter
        # (see rxref for the files).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl/radio_openhpsdr1
        if [ ! -d "$top/$core" ] || [ ! -f "$top/build/simlib/altera_mf.v" ]; then
            echo "run tools/sim.sh rxref once first (it unpacks the core)" >&2
            exit 2
        fi
        files="$core/cic_integrator.v $core/cic_comb.v $core/cic.v $core/varcic.v"
        files="$files $core/firram36.v $core/firrom/firromH.v $core/receiver_nco.v"
        files="$files rtl/core/firfold.sv rtl/core/firsharedfold.sv"
        defines="-DUSE_ALTSYNCRAM=1 -DFIR_SHARED_SIM -DFIR_FOLD_SIM -DREF_FOLD"
        files="$files rtl/core/fir_sdpram.sv rtl/core/rx_cic.sv build/simlib/altera_mf.v sim/fir_shared_tb.sv"
        tb=fir_shared_tb
        run_dir=$core/firrom
        rate=${2:-5}
        plusargs="+RATE=$rate +N=${3:-100}${4:+ +LAST=$4}"
        which=$which-$rate-${4:-all} ;;
    radioeq|corecmp)
        # radioeq: the Hermes-Lite 2 based core top (rtl/core/radio.sv, at
        # git revision NEW, default b8b7fc9, its last) against its version
        # at REV (default: the end of stage 1 of the core takeover).
        # corecmp: rtl/core/rbv_core.sv against radio.sv at REV (default
        # b8b7fc9) with the driver's command translation
        # (sim/core_cmp_tb.sv). Both with the core's modules as the images
        # build them and Quartus's models of its memories and multipliers
        # (altera_mf.v, 220model.v).
        if [ "$which" = corecmp ]; then rev=${2:-b8b7fc9}; else rev=${2:-bfeb533}; fi
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        # Verilator builds use this tree's memory models, not Quartus's.
        for lib in altera_mf.v 220model.v; do
            if [ -z "${VERILATOR:-}" ] && [ ! -f "$top/build/simlib/$lib" ]; then
                mkdir -p "$top/build/simlib"
                docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
                    -v "$top/build/simlib:/out" --entrypoint sh radioberry-quartus:23.1std.1 \
                    -c "cp /opt/altera/quartus/eda/sim_lib/$lib /out/"
            fi
        done
        mkdir -p "$top/build/sim/$which-run"
        # The reference keeps the transmit CIC interpolator it was proven
        # with (tx_cic.sv at bbf2960, renamed tx_cic_ref), so a change to
        # rtl/core/tx_cic.sv is compared, not shared.
        git -C "$top" show "$rev:gateware/rtl/core/radio.sv" |
            sed -e 's/^module radio (/module radio_ref (/' -e 's/^tx_cic in2 (/tx_cic_ref in2 (/' \
                >"$top/build/sim/radio_ref.sv"
        git -C "$top" show bbf2960:gateware/rtl/core/tx_cic.sv |
            sed 's/^module tx_cic (/module tx_cic_ref (/' >"$top/build/sim/tx_cic_ref.sv"
        git -C "$top" show "${NEW:-b8b7fc9}:gateware/rtl/core/radio.sv" >"$top/build/sim/radio_new.sv"
        # The coefficient and sine tables are read from the directory the
        # run starts in.
        cp "$top/$core"/radio_openhpsdr1/firrom/*.mif "$top/$core"/nco/*.txt "$top/build/sim/$which-run/"
        r=$core/radio_openhpsdr1
        files="$r/cic_integrator.v $r/cic_comb.v $r/cic.v $r/varcic.v $r/cordic.v $r/cpl_cordic.v"
        files="$files $r/CicInterpM5.v $r/FirInterp8_1024.v $r/firram36I_1024.v"
        files="$files $r/firrom/firromI_1024.v"
        # The mixers are the tree's (rtl/core/mix2.sv, sincos.sv; the same
        # values on the same clocks as the core's, tools/sim.sh mixeq) for
        # both designs.
        files="$files rtl/core/mix2.sv rtl/core/sincos.sv"
        files="$files rtl/core/rx_cic.sv rtl/core/rx_cic_int.sv rtl/core/rx_varcomb.sv"
        files="$files rtl/core/fir_sdpram.sv rtl/core/firsharedfold.sv rtl/core/rx_sender.sv"
        files="$files rtl/core/tx_cic.sv rtl/core/tx_fir.sv rtl/core/tx_upconv.sv"
        files="$files rtl/core/cw_shape.sv build/sim/tx_cic_ref.sv build/sim/radio_ref.sv"
        files="$files build/simlib/altera_mf.v build/simlib/220model.v"
        if [ "$which" = corecmp ]; then
            files="$files rtl/core/rbv_core.sv sim/core_cmp_tb.sv"
            tb=core_cmp_tb
            defines="-DUSE_ALTSYNCRAM=1 -DFIR_SHARED_SIM"
            plusargs="${3:++PHASE=$3}"
        else
            files="$files build/sim/radio_new.sv sim/radio_eq_tb.sv"
            tb=radio_eq_tb
            # A reference from 19c1441 on has the pruned receivers.
            defines="-DUSE_ALTSYNCRAM=1 -DFIR_SHARED_SIM ${REF_PRUNED:+-DREF_PRUNED}"
            plusargs="${3:++PHASE=$3}${4:+ +RX_TOL=$4}${TX_NEW:+ +TX_NEW}"
        fi
        run_dir=build/sim/$which-run ;;
    cicprune)
        # The pruned CICs (rtl/core/rx_cic_int.sv, rx_varcomb.sv) against
        # the exact ones at 12c6864 (takeover stage 2a), six receivers, each
        # decimation; N outputs a decimation (default 400).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl/radio_openhpsdr1
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        mkdir -p "$top/build/sim"
        git -C "$top" show 12c6864:gateware/rtl/core/rx_cic_int.sv |
            sed 's/^module rx_cic_int /module rx_cic_int_x /' >"$top/build/sim/rx_cic_int_x.sv"
        git -C "$top" show 12c6864:gateware/rtl/core/rx_varcomb.sv |
            sed 's/^module rx_varcomb /module rx_varcomb_x /' >"$top/build/sim/rx_varcomb_x.sv"
        files="$core/cic.v $core/cic_integrator.v $core/cic_comb.v rtl/core/fir_sdpram.sv"
        files="$files build/sim/rx_cic_int_x.sv build/sim/rx_varcomb_x.sv"
        files="$files rtl/core/rx_cic_int.sv rtl/core/rx_varcomb.sv sim/cic_prune_tb.sv"
        tb=cic_prune_tb
        defines="-DFIR_SHARED_SIM"
        plusargs="${2:++N=$2}" ;;
    sincoseq)
        # rtl/core/sincos.sv against the core's sincos.v (renamed
        # sincos_ref), with Quartus's model of its multipliers (220model.v);
        # N clocks of each kind of angle (default 200000).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        mkdir -p "$top/build/sim/sincoseq-run"
        sed 's/^module sincos (/module sincos_ref (/' "$top/$core/nco/sincos.v" >"$top/build/sim/sincos_ref.v"
        # The sine tables are read from the directory the run starts in.
        cp "$top/$core"/nco/*.txt "$top/build/sim/sincoseq-run/"
        files="$core/nco/coarserom.v $core/nco/finerom.v $core/primitives/intel/multipliers.v"
        files="$files build/sim/sincos_ref.v rtl/core/sincos.sv build/simlib/220model.v"
        files="$files sim/sincos_eq_tb.sv"
        tb=sincos_eq_tb
        run_dir=build/sim/sincoseq-run
        plusargs="${2:++N=$2}" ;;
    txciceq)
        # rtl/core/tx_cic.sv against the core's CicInterpM5.v, every clock;
        # N input samples of each kind (default 2000).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl/radio_openhpsdr1
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        files="$core/CicInterpM5.v rtl/core/fir_sdpram.sv rtl/core/tx_cic.sv sim/tx_cic_eq_tb.sv"
        tb=tx_cic_eq_tb
        defines="-DFIR_SHARED_SIM"
        plusargs="${2:++N=$2}" ;;
    txfireq)
        # rtl/core/tx_fir.sv against the core's FirInterp8_1024.v with
        # Quartus's memory models, every clock; N input samples of each
        # kind (default 2000).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl/radio_openhpsdr1
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        mkdir -p "$top/build/sim/txfireq-run"
        # The coefficient table is read from the directory the run starts in.
        cp "$top/$core"/firrom/coefI8_1024.mif "$top/build/sim/txfireq-run/"
        files="$core/FirInterp8_1024.v $core/firram36I_1024.v $core/firrom/firromI_1024.v"
        files="$files rtl/core/fir_sdpram.sv rtl/core/tx_fir.sv build/simlib/altera_mf.v"
        files="$files sim/tx_fir_eq_tb.sv"
        tb=tx_fir_eq_tb
        run_dir=build/sim/txfireq-run
        defines="-DUSE_ALTSYNCRAM=1 -DFIR_SHARED_SIM"
        plusargs="${2:++N=$2}" ;;
    txupconv)
        # The transmit up-converter: the core's cpl_cordic.v and its cut to
        # the DAC against rtl/core/tx_upconv.sv without and with dither; the
        # DAC words of six cases to build/sim/txupconv-run/OUT_<case>.txt
        # for spectral analysis; N clocks a case (default 262144).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        mkdir -p "$top/build/sim/txupconv-run"
        # The sine tables are read from the directory the run starts in.
        cp "$top/$core"/nco/*.txt "$top/build/sim/txupconv-run/"
        files="$core/radio_openhpsdr1/cpl_cordic.v rtl/core/sincos.sv rtl/core/tx_upconv.sv"
        files="$files sim/tx_upconv_tb.sv"
        tb=tx_upconv_tb
        run_dir=build/sim/txupconv-run
        plusargs="${2:++N=$2}" ;;
    cwshape)
        # The CW keying envelope: the core's straight line against
        # rtl/core/cw_shape.sv's raised cosine, one element, both written
        # to build/sim/cwshape-run/OUT.txt for the click spectra.
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        mkdir -p "$top/build/sim/cwshape-run"
        cp "$top/$core"/nco/*.txt "$top/build/sim/cwshape-run/"
        files="rtl/core/sincos.sv rtl/core/cw_shape.sv sim/cw_shape_tb.sv"
        tb=cw_shape_tb
        run_dir=build/sim/cwshape-run
        plusargs="${2:++HOLD=$2}" ;;
    mixeq)
        # rtl/core/mix2.sv with rtl/core/sincos.sv against the core's mix2.v,
        # nco2.v and sincos.v (renamed mix2_ref, nco2_ref, sincos_ref), with
        # Quartus's model of their multipliers (220model.v); N clocks of
        # 76.8 MHz (default 200000).
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        mkdir -p "$top/build/sim/mixeq-run"
        sed -e 's/^module mix2 (/module mix2_ref (/' -e 's/^nco2 #(/nco2_ref #(/' \
            "$top/$core/nco/mix2.v" >"$top/build/sim/mix2_ref.v"
        sed -e 's/^module nco2 (/module nco2_ref (/' -e 's/^sincos #(/sincos_ref #(/' \
            "$top/$core/nco/nco2.v" >"$top/build/sim/nco2_ref.v"
        sed 's/^module sincos (/module sincos_ref (/' "$top/$core/nco/sincos.v" >"$top/build/sim/sincos_ref.v"
        cp "$top/$core"/nco/*.txt "$top/build/sim/mixeq-run/"
        files="$core/nco/coarserom.v $core/nco/finerom.v $core/primitives/intel/multipliers.v"
        files="$files build/sim/mix2_ref.v build/sim/nco2_ref.v build/sim/sincos_ref.v"
        files="$files rtl/core/sincos.sv rtl/core/mix2.sv build/simlib/220model.v sim/mix2_eq_tb.sv"
        tb=mix2_eq_tb
        run_dir=build/sim/mixeq-run
        plusargs="${2:++N=$2}" ;;
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
    failsafe)
        # The keying fail-safe: the rpi4-i2s transmit build with the real
        # radio core; every keying path stops when the host does.
        core=build/hl2-sim/Hermes-Lite2-3e467fd/gateware/rtl
        if [ ! -d "$top/$core" ]; then
            mkdir -p "$top/build/hl2-sim"
            tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$top/build/hl2-sim"
            patch -s -p1 -d "$top/build/hl2-sim" <"$top/source/hl2-conformance.patch"
        fi
        mkdir -p "$top/build/sim/failsafe-run"
        cp "$top/$core"/radio_openhpsdr1/firrom/*.mif "$top/$core"/nco/*.txt "$top/build/sim/failsafe-run/"
        files="rtl/common/rbv_crc8.sv rtl/common/rbv_spi.sv rtl/common/rbv_adseq.sv"
        files="$files rtl/common/rbv_afifo.sv rtl/common/rbv_i2s.sv rtl/linktest/rbt_adser.sv"
        files="$files rtl/rpi4/rbv_rpi4_top.sv sim/rbt_pll_sim.sv"
        files="$files rtl/core/mix2.sv rtl/core/sincos.sv rtl/core/rx_cic_int.sv rtl/core/rx_varcomb.sv"
        files="$files rtl/core/fir_sdpram.sv rtl/core/firsharedfold.sv rtl/core/rx_sender.sv"
        files="$files rtl/core/tx_cic.sv rtl/core/tx_fir.sv rtl/core/tx_upconv.sv rtl/core/cw_shape.sv"
        files="$files rtl/core/rbv_core.sv build/simlib/altera_mf.v build/simlib/220model.v"
        files="$files sim/keying_failsafe_tb.sv"
        tb=keying_failsafe_tb
        run_dir=build/sim/failsafe-run
        defines="-DUSE_ALTSYNCRAM=1 -DFIR_SHARED_SIM" ;;
    rpi5)
        files="rtl/common/rbv_crc8.sv rtl/common/rbv_spi.sv rtl/common/rbv_adseq.sv"
        files="$files rtl/common/rbv_afifo.sv rtl/linktest/rbt_adser.sv"
        files="$files rtl/rpi5/rbv_pio_rx.sv rtl/rpi5/rbv_pio_tx.sv rtl/rpi5/rbv_rpi5_top.sv"
        files="$files sim/rbt_pll_sim.sv sim/radio_stub.sv sim/pio_sm_model.sv sim/rbv_rpi5_tb.sv"
        tb=rbv_rpi5_tb ;;
    *) echo "unknown testbench $which" >&2; exit 2 ;;
esac
mkdir -p "$top/build/sim"
if [ -n "${VERILATOR:-}" ]; then
    # Verilator installed on this machine, compiled (--binary --timing):
    # about a thousand times faster than Icarus on the core comparisons.
    # Memories use this tree's own models (fir_sdpram), not Quartus's.
    vfiles=""
    for f in $files; do
        case "$f" in build/simlib/*) ;; *) vfiles="$vfiles $f" ;; esac
    done
    vdefines=""
    for d in $defines; do
        case "$d" in -DUSE_ALTSYNCRAM*) ;; *) vdefines="$vdefines $d" ;; esac
    done
    (cd "$top" &&
        verilator --binary --timing -j "${VJOBS:-1}" -Wno-fatal -Wno-lint -Wno-style \
            -DSINE_HEX=\"rtl/linktest/data/sin1024.hex\" $vdefines --top-module $tb \
            --Mdir "build/sim/vl-$tb" -o "$tb" $vfiles >"build/sim/vl-$tb.log" 2>&1 &&
        cd "$run_dir" && "$top/build/sim/vl-$tb/$tb" $plusargs) |
        tee "$top/build/sim/$which.log"
elif [ -n "${SIM_LOCAL:-}" ]; then
    # Icarus installed on this machine (a build Pi), no Docker.
    (cd "$top" &&
        iverilog -g2012 -Wall -DSINE_HEX=\"rtl/linktest/data/sin1024.hex\" $defines -s $tb \
            -o "build/sim/$tb.vvp" $files &&
        cd "$run_dir" && vvp -n "$top/build/sim/$tb.vvp" $plusargs) |
        tee "$top/build/sim/$which.log"
else
    docker run --rm -v "$top:/src" -w /src "$image" sh -c \
        "iverilog -g2012 -Wall -DSINE_HEX=\\\"rtl/linktest/data/sin1024.hex\\\" $defines -s $tb -o /tmp/$tb.vvp $files &&
         cd $run_dir && vvp -n /tmp/$tb.vvp $plusargs" |
        tee "$top/build/sim/$which.log"
fi
grep -q '^PASS$' "$top/build/sim/$which.log"
