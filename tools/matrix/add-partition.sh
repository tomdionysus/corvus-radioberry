#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# After shrink-root.sh has shrunk the root ext4: shrinks the root partition
# (partition 2, GPT or MBR) to ROOT_GIB and appends a partition of NEW_GIB
# right after it, without rebooting. Refuses unless the file system fits in
# the new partition 2 with at least 1 GiB to spare. Saves the old table to
# /root/sfdisk-before.txt.
#
#   add-partition.sh DISK ROOT_GIB NEW_GIB     e.g. /dev/nvme0n1 831 64
set -eu
disk=$1
root_gib=$2
new_gib=$3
case $disk in
    *[0-9]) p2=${disk}p2 ;;
    *) p2=${disk}2 ;;
esac
start=$(sfdisk -d "$disk" | awk -v p="$p2" '$1 == p { sub(/,/, "", $4); print $4 }')
[ -n "$start" ] || { echo "no $p2 in the table" >&2; exit 1; }
p2size=$((root_gib * 2097152))
p3start=$((start + p2size))
p3start=$(((p3start + 2047) / 2048 * 2048))
p3size=$((new_gib * 2097152))
bc=$(tune2fs -l "$p2" | awk -F: '/^Block count/ { gsub(/ /, "", $2); print $2 }')
bs=$(tune2fs -l "$p2" | awk -F: '/^Block size/ { gsub(/ /, "", $2); print $2 }')
fs=$((bc * bs / 512))
echo "file system $fs sectors, new partition 2 $p2size sectors"
[ "$fs" -le $((p2size - 2097152)) ] || { echo "refused: the file system does not fit" >&2; exit 1; }
sfdisk -d "$disk" >/root/sfdisk-before.txt
printf '%s,%s\n' "$start" "$p2size" | sfdisk --no-reread --no-tell-kernel -N 2 "$disk"
label=$(sfdisk -d "$disk" | awk '/^label:/ { print $2 }')
if [ "$label" = gpt ]; then
    type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
else
    type=83
fi
printf '%s,%s,%s\n' "$p3start" "$p3size" "$type" | sfdisk --no-reread --no-tell-kernel --append "$disk"
partx -u --nr 2 "$disk"
partx -a --nr 3 "$disk" || true
lsblk "$disk"
