#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Arms a one-time tryboot (Raspberry Pi firmware) of the running kernel
# with a private initramfs that checks or shrinks the root ext4 before it
# is mounted. The system's own initramfs configuration is not touched, and
# any other boot is the normal one. panic=10 makes a failed initramfs
# reboot (into the normal boot) instead of stopping at a shell.
#
#   shrink-root.sh dry  DEV          check only: e2fsck -n and the minimum size
#   shrink-root.sh real DEV SIZE     e2fsck -fy, resize2fs DEV SIZE (e.g. 830G)
#
# Then: reboot "0 tryboot", and read /run/rbshrink.log after it boots.
# Run as root on the Pi; DEV is the root partition (e.g. /dev/nvme0n1p2,
# /dev/mmcblk0p2).
set -eu
mode=$1
dev=$2
size=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
rel=$(uname -r)
case $rel in
    *-rpi-2712) img=kernel_2712.img ;;
    *-rpi-v8) img=kernel8.img ;;
    *) echo "unknown kernel flavour: $rel" >&2; exit 2 ;;
esac
fw=/boot/firmware
conf=/root/rbshrink-conf
rm -rf "$conf"
cp -a /etc/initramfs-tools "$conf"
install -m 755 "$here/rbshrink-premount" "$conf/scripts/local-premount/rbshrink"
cat >"$conf/hooks/rbshrink" <<'END'
#!/bin/sh
PREREQ=""
prereqs() { echo "$PREREQ"; }
case $1 in prereqs) prereqs; exit 0 ;; esac
. /usr/share/initramfs-tools/hook-functions
copy_exec /sbin/resize2fs /sbin
copy_exec /sbin/e2fsck /sbin
END
chmod 755 "$conf/hooks/rbshrink"
sed -i 's/^MODULES=.*/MODULES=most/' "$conf/initramfs.conf"
dir=$fw/k-rbs
rm -rf "$dir"
mkdir -p "$dir/overlays"
mkinitramfs -d "$conf" -o "$dir/initrd.img" "$rel"
cp "/boot/vmlinuz-$rel" "$dir/$img"
cp "/usr/lib/linux-image-$rel/broadcom/"bcm27*.dtb "$dir/"
cp "/usr/lib/linux-image-$rel/overlays/"* "$dir/overlays/"
[ -f /usr/share/radioberry/overlays/radioberry.dtbo ] &&
    cp /usr/share/radioberry/overlays/radioberry.dtbo "$dir/overlays/"
printf '%s panic=10 rbshrink=%s rbdev=%s%s\n' "$(cat $fw/cmdline.txt)" "$mode" "$dev" \
    "${size:+ rbsize=$size}" >"$dir/cmdline.txt"
{
    grep -vE '^(kernel=|os_prefix=|auto_initramfs=|initramfs )' $fw/config.txt
    printf '[all]\nos_prefix=k-rbs/\nkernel=%s\ninitramfs initrd.img followkernel\n' "$img"
} >$fw/tryboot.txt
echo "armed: $(cat "$dir/cmdline.txt" | grep -o 'rbshrink=.*')"
echo "now: reboot \"0 tryboot\"; afterwards: cat /run/rbshrink.log; rm -rf $dir"
