#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Checks that the overlay applies to the Raspberry Pi 4 and Pi 5 device
# trees, as the firmware would apply it, and that the driver's node and its
# configuration GPIOs land on each board's own SPI and GPIO blocks. With the
# Pi 4's SMI overlay as well, checks that it applies on the Pi 4 after the
# first and gives the driver's node the SMI phandle and the "smi" pin state.
# Needs dtmerge and fdtget (Raspberry Pi OS) and the boards' trees in
# /boot/firmware.
#
#   tools/check-overlay.sh build/driver/radioberry.dtbo [build/driver/radioberry-smi.dtbo]
set -eu

dtbo=${1:?usage: check-overlay.sh OVERLAY.dtbo [SMI-OVERLAY.dtbo]}
smi_dtbo=${2:-}
firmware=${RADIOBERRY_DTB_DIR:-/boot/firmware}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# board tree, SPI controller path, GPIO controller path
check() {
    base=$firmware/$1.dtb
    merged=$tmp/$1.dtb
    if ! dtmerge "$base" "$merged" "$dtbo" >"$tmp/log" 2>&1; then
        echo "$1: overlay does not apply"
        cat "$tmp/log"
        exit 1
    fi
    node=$2/radioberry@0
    [ "$(fdtget "$merged" "$node" compatible)" = "sdr,radioberry" ] ||
        { echo "$1: no sdr,radioberry at $node"; exit 1; }
    [ "$(fdtget "$merged" "$2" status)" = "okay" ] || { echo "$1: $2 not enabled"; exit 1; }
    gpio=$(fdtget -t u "$merged" "$3" phandle)
    for prop in nconfig-gpios dclk-gpios data0-gpios conf-done-gpios nstatus-gpios \
        tx-clk-gpios tx-data-gpios rx-clk-gpios tx-ready-gpios rx-data-gpios rx-ready-gpios; do
        ref=$(fdtget -t u "$merged" "$node" "$prop" | cut -d' ' -f1)
        [ "$ref" = "$gpio" ] || { echo "$1: $prop is not on $3"; exit 1; }
    done
    echo "$1: ok"
}

# board tree (already merged with the first overlay), SPI path, SMI path,
# GPIO path
check_smi() {
    merged=$tmp/$1.dtb
    both=$tmp/$1-smi.dtb
    if ! dtmerge "$merged" "$both" "$smi_dtbo" >"$tmp/log" 2>&1; then
        echo "$1: SMI overlay does not apply"
        cat "$tmp/log"
        exit 1
    fi
    node=$2/radioberry@0
    smi=$(fdtget -t u "$both" "$3" phandle)
    [ "$(fdtget -t u "$both" "$node" brcm,smi)" = "$smi" ] ||
        { echo "$1: brcm,smi is not $3"; exit 1; }
    [ "$(fdtget "$both" "$node" pinctrl-names)" = "smi" ] ||
        { echo "$1: no \"smi\" pin state"; exit 1; }
    group_path=$4/radioberry_smi_pins
    pins=$(fdtget -t u "$both" "$node" pinctrl-0)
    group=$(fdtget -t u "$both" "$group_path" phandle)
    [ "$pins" = "$group" ] || { echo "$1: the \"smi\" state is not $group_path"; exit 1; }
    [ "$(fdtget -t u "$both" "$group_path" brcm,pins)" = "6 18 19 20 21 25" ] ||
        { echo "$1: the SMI pins are not 6, 18 to 21 and 25"; exit 1; }
    [ "$(fdtget "$both" "$3" status)" = "disabled" ] ||
        { echo "$1: $3 is enabled, so bcm2835_smi would bind it"; exit 1; }
    echo "$1 with the SMI overlay: ok"
}

check bcm2711-rpi-4-b /soc/spi@7e204000 /soc/gpio@7e200000
check bcm2712-rpi-5-b /axi/pcie@1000120000/rp1/spi@50000 /axi/pcie@1000120000/rp1/gpio@d0000
if [ -n "$smi_dtbo" ]; then
    check_smi bcm2711-rpi-4-b /soc/spi@7e204000 /soc/smi@7e600000 /soc/gpio@7e200000
fi
