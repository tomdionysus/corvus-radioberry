#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Kernel test for the radioberry packages on a Pi 4 or Pi 5, on whichever
# OS is running (trixie or bookworm), receive only.
#
#   kt.sh stage RELEASE   install RELEASE (e.g. 6.12.25+rpt-rpi-2712) and its
#                         headers, build the driver with DKMS, stage the
#                         kernel with its own initramfs, DTBs and overlays in
#                         $FW/k-RELEASE/ and arm a one-time tryboot into it
#   kt.sh run OUT         after booting it: probe, receive, CPU, rbcat and the
#                         driver harness; results to OUT
#
# Environment:
#   FW       the firmware's FAT partition as mounted here
#            (trixie: /boot/firmware; bookworm on partition 3: /mnt/fw)
#   BASE     config the tryboot config is made from
#            (trixie: $FW/config.txt; bookworm: $FW/tryboot.txt.bw)
#   CMDLINE  kernel command line file (trixie: $FW/cmdline.txt;
#            bookworm: $FW/k-bw/cmdline.txt)
#   TOOLS    directory with ft8rx and rbdrivertest built for this OS
#   DIAL     FT8 dial frequency in Hz (default 7074000, 40 m; 14074000 by day)
# Nothing here sets PTT or the TX permission bit.
set -u
FW=${FW:-/boot/firmware}
BASE=${BASE:-$FW/config.txt}
CMDLINE=${CMDLINE:-$FW/cmdline.txt}
TOOLS=${TOOLS:-/root/rbtools}
cmd=$1
case "$cmd" in
stage)
    rel=$2
    flavour=${rel#*-rpi-}
    case $flavour in
        2712) img=kernel_2712.img; ird=initramfs_2712 ;;
        v8) img=kernel8.img; ird=initramfs8 ;;
        *) echo "unknown flavour $flavour"; exit 2 ;;
    esac
    dir=$FW/k-$rel
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q "linux-image-$rel" "linux-headers-$rel" \
        > "/root/kt-install-$rel.log" 2>&1
    echo "apt exit $?"
    ver=$(dpkg-query -W -f '${Version}' radioberry-dkms | sed 's/-[^-]*$//')
    dkms install "radioberry/$ver" -k "$rel" > "/root/kt-dkms-$rel.log" 2>&1
    echo "dkms exit $?"
    ls -l "/lib/modules/$rel/updates/dkms/radioberry.ko"* || { grep -m3 -E "error" /var/lib/dkms/radioberry/*/build/make.log; exit 1; }
    rm -rf "$dir"
    mkdir -p "$dir/overlays"
    cp "/boot/vmlinuz-$rel" "$dir/$img"
    [ -f "/boot/initrd.img-$rel" ] && cp "/boot/initrd.img-$rel" "$dir/$ird"
    cp "/usr/lib/linux-image-$rel/broadcom/"bcm27*.dtb "$dir/"
    cp "/usr/lib/linux-image-$rel/overlays/"* "$dir/overlays/"
    cp /usr/share/radioberry/overlays/radioberry.dtbo "$dir/overlays/"
    cp "$CMDLINE" "$dir/cmdline.txt"
    {
        grep -vE '^(kernel=|os_prefix=|auto_initramfs=|initramfs )' "$BASE"
        printf '[all]\nos_prefix=k-%s/\nkernel=%s\nauto_initramfs=1\n' "$rel" "$img"
    } > "$FW/tryboot.txt"
    tail -4 "$FW/tryboot.txt"
    ;;
run)
    out=$2
    {
        echo "kernel $(uname -r)"
        echo "os $(. /etc/os-release; echo "$VERSION_CODENAME")"
        echo "module $(modinfo -n radioberry) version $(cat /sys/module/radioberry/version)"
        for i in $(seq 1 30); do systemctl is-active -q radioberry && break; sleep 1; done
        echo "service $(systemctl is-active radioberry)"
        echo "transport $(cat /sys/bus/spi/devices/spi0.0/transport)"
        dmesg | grep -E "radioberry spi0.0" | tail -3
        rbprobe > /root/kt-rbprobe.txt 2>&1; echo "rbprobe exit $?"
        grep -E "radio can run" /root/kt-rbprobe.txt | head -2
        sh /root/cpuload.sh 1 192 1 20 > "$out.cpu192" 2>&1
        grep -E "^cpu|  radioberry |lost_packets" "$out.cpu192"
        for p in 1 2; do
            "$TOOLS/ft8rx" --protocol $p --dial "${DIAL:-7074000}" --slots 4 > "$out.ft8rx-p$p" 2>&1
            echo "ft8rx P$p: $(tail -1 "$out.ft8rx-p$p")"
        done
        journalctl -u radioberry -b --no-pager | grep -iE "warn|error|fault|stall|full|drop|refused" |
            grep -v "transmit allowed" | head -5
        systemctl stop radioberry
        timeout 40 rbcat --seconds 10 --tx-zeros > /root/kt-rbcat.txt 2>&1
        grep -E "^  frames |frame_rate|rx_resyncs|rx_dropped|tx_word_rate|tx_underruns|ptt_set|tx_permission_set" /root/kt-rbcat.txt | head -9
        tail -1 /root/kt-rbcat.txt
        timeout 1200 "$TOOLS/rbdrivertest" > /root/kt-harness.txt 2>&1
        echo "harness exit $?: $(grep -E "test cases|FAILED" /root/kt-harness.txt | tail -1)"
        cp /root/kt-harness.txt "$out.harness"
        systemctl start radioberry
        dmesg | grep -iE "radioberry|rp1-pio|dw_axi|bcm2835-i2s|dma" | grep -iE "error|fail|warn|stall|bug" | head -5
        echo "throttled $(vcgencmd get_throttled 2>/dev/null)"
    } > "$out" 2>&1
    cat "$out"
    ;;
esac
