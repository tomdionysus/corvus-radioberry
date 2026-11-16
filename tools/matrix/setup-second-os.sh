#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs the root of a Raspberry Pi OS Bookworm image on a spare
# partition of a Pi that runs trixie, so the Pi can boot either: trixie
# normally, Bookworm by tryboot. Run as root on trixie.
#
#   setup-second-os.sh IMAGE.img.xz PART NAME
#     e.g. setup-second-os.sh 2026-10-06-raspios-bookworm-arm64.img.xz /dev/nvme0n1p3 bookworm
#
# What it sets up on PART:
# - the image's root file system, grown to the partition;
# - its own boot files in /boot/firmware/k-NAME/ on the shared FAT
#   partition (kernel, initramfs, DTBs, overlays, cmdline with this root and
#   panic=10), and $FW/tryboot.txt.NAME, copied to tryboot.txt;
# - /boot/firmware inside it is a bind mount of /boot/fwbw on its own root,
#   so its kernel updates never touch trixie's boot files; the FAT
#   partition is mounted at /mnt/fw;
# - trixie's SSH host keys and root's authorized_keys, ssh enabled, the
#   image's rename-user banner removed;
# - Wi-Fi: trixie's NetworkManager connection, NetworkManager's Wi-Fi and
#   the rfkill state unblocked (the image keeps both off until its
#   first-boot wizard sets a country);
# - the image's user pi renamed to trixie's first user (same uid, password
#   hash and groups), desktop autologin as that user, the wizard bypassed;
# - a persistent journal;
# - rb-guard: in a tryboot boot, reboots (back to trixie) when /run/rb-keep
#   has not been touched for 30 minutes. Touch it over SSH while working.
set -eu
img=$1
part=$2
name=$3
fw=/boot/firmware
k=$fw/k-$name
m=/mnt/second-os
b=/mnt/second-os-boot
user=$(getent passwd 1000 | cut -d: -f1)
raw=/root/second-os.img
xz -dc "$img" >"$raw"
loop=$(losetup -fP --show "$raw")
dd if="${loop}p2" of="$part" bs=4M conv=fsync status=none
e2fsck -fy "$part" || true
resize2fs "$part"
tune2fs -U random "$part"
mkdir -p "$m" "$b"
mount "$part" "$m"
mount -o ro "${loop}p1" "$b"

rm -rf "$m/boot/firmware" "$m/boot/fwbw"
mkdir -p "$m/boot/fwbw" "$m/boot/firmware" "$m/mnt/fw"
cp -r "$b/." "$m/boot/fwbw/"
rm -rf "$k"
mkdir -p "$k"
cp "$b"/kernel*.img "$b"/initramfs* "$b"/bcm27*.dtb "$k/"
cp -r "$b/overlays" "$k/"
[ -f /usr/share/radioberry/overlays/radioberry.dtbo ] &&
    cp /usr/share/radioberry/overlays/radioberry.dtbo "$k/overlays/"
puuid=$(blkid -s PARTUUID -o value "$part")
fwuuid=$(findmnt -n -o PARTUUID "$fw")
regdom=$(grep -o 'cfg80211.ieee80211_regdom=[A-Z]*' "$fw/cmdline.txt" || true)
echo "console=serial0,115200 console=tty1 root=PARTUUID=$puuid rootfstype=ext4 fsck.repair=yes rootwait $regdom panic=10" >"$k/cmdline.txt"
cat >"$m/etc/fstab" <<END
proc /proc proc defaults 0 0
PARTUUID=$puuid / ext4 defaults,noatime 0 1
PARTUUID=$fwuuid /mnt/fw vfat defaults,nofail 0 2
/boot/fwbw /boot/firmware none bind 0 0
END

echo "$(hostname)-$name" >"$m/etc/hostname"
sed -i "s/^127.0.1.1.*/127.0.1.1\t$(hostname)-$name/" "$m/etc/hosts"
cp -a /etc/ssh/ssh_host_* "$m/etc/ssh/"
rm -f "$m/etc/ssh/sshd_config.d/rename_user.conf"
mkdir -p "$m/root/.ssh"
cp /root/.ssh/authorized_keys "$m/root/.ssh/"
chmod 700 "$m/root/.ssh"
ln -sf /lib/systemd/system/ssh.service "$m/etc/systemd/system/multi-user.target.wants/ssh.service"
chroot "$m" systemctl disable regenerate_ssh_host_keys >/dev/null 2>&1 || true

for c in /run/NetworkManager/system-connections/*.nmconnection /etc/NetworkManager/system-connections/*.nmconnection; do
    [ -f "$c" ] || continue
    grep -q '^type=wifi' "$c" || continue
    install -m 600 "$c" "$m/etc/NetworkManager/system-connections/wlan0.nmconnection"
    break
done
sed -i 's/^WirelessEnabled=.*/WirelessEnabled=true/' "$m/var/lib/NetworkManager/NetworkManager.state"
for f in "$m"/var/lib/systemd/rfkill/*wlan*; do
    [ -f "$f" ] && echo 0 >"$f"
done

if [ -n "$user" ] && chroot "$m" id pi >/dev/null 2>&1; then
    chroot "$m" groupmod -n "$user" pi
    chroot "$m" usermod -l "$user" -d "/home/$user" -m pi
fi
if [ -n "$user" ]; then
    chroot "$m" usermod -p "$(getent shadow "$user" | cut -d: -f2)" "$user"
    for g in $(id -nG "$user"); do
        chroot "$m" usermod -aG "$g" "$user" 2>/dev/null || true
    done
    sed -i "s/^autologin-user=.*/autologin-user=$user/" "$m/etc/lightdm/lightdm.conf" 2>/dev/null || true
    sed -i "s/\bpi\b/$user/" "$m/etc/sudoers.d/010_pi-nopasswd" 2>/dev/null || true
fi
rm -f "$m/etc/sudoers.d/010_wiz-nopasswd" "$m/etc/xdg/autostart/piwiz.desktop"
chroot "$m" systemctl mask userconfig.service >/dev/null 2>&1 || true
mkdir -p "$m/var/log/journal"

cat >"$m/usr/local/sbin/rb-guard" <<'END'
#!/bin/sh
# In a tryboot boot, reboot (back to the normal boot) when /run/rb-keep has
# not been touched for 30 minutes.
t=$(od -An -tx1 /proc/device-tree/chosen/bootloader/tryboot 2>/dev/null | tr -d ' \n')
[ "$t" = 00000001 ] || exit 0
[ -e /run/rb-keep ] || touch /run/rb-keep
age=$(($(date +%s) - $(stat -c %Y /run/rb-keep)))
if [ $age -gt 1800 ]; then
    logger "rb-guard: no keepalive for ${age}s, rebooting"
    systemctl reboot
fi
END
chmod 755 "$m/usr/local/sbin/rb-guard"
printf '[Unit]\nDescription=Reboot a tryboot boot that lost contact\n[Service]\nType=oneshot\nExecStart=/usr/local/sbin/rb-guard\n' \
    >"$m/etc/systemd/system/rb-guard.service"
printf '[Unit]\nDescription=rb-guard every minute\n[Timer]\nOnBootSec=1min\nOnUnitActiveSec=1min\n[Install]\nWantedBy=timers.target\n' \
    >"$m/etc/systemd/system/rb-guard.timer"
ln -sf /etc/systemd/system/rb-guard.timer "$m/etc/systemd/system/timers.target.wants/rb-guard.timer"

umount "$b" "$m"
losetup -d "$loop"
rm -f "$raw"
{
    grep -vE '^(kernel=|os_prefix=|auto_initramfs=|initramfs )' "$fw/config.txt"
    printf '[all]\nos_prefix=k-%s/\nauto_initramfs=1\n' "$name"
} >"$fw/tryboot.txt.$name"
cp "$fw/tryboot.txt.$name" "$fw/tryboot.txt"
echo "ready: reboot \"0 tryboot\" boots $name from $part; a normal reboot returns to trixie"
