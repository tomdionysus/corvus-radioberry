#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compiles a gateware project with Quartus Prime Lite in the Docker image
# radioberry-quartus:23.1std.1 (tools/quartus-image.sh), copies the reports
# to build/<project>/ and, for a radio image, the bitfile to
# images/<project>/CL025/radioberry-<project>.rbf.
#
#   tools/build.sh rpi5-pio            the Raspberry Pi 5 image
#   tools/build.sh rpi4-i2s            the Raspberry Pi 4 image
#   tools/build.sh linktest            the link test image (build/ only)
#   SEED=4 tools/build.sh rpi5-pio     another fitter seed
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
image=radioberry-quartus:23.1std.1
project=${1:-}
[ -n "$project" ] && [ -f "$top/projects/$project/$project.qsf" ] ||
    { sed -n '4,13p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
docker image inspect "$image" >/dev/null 2>&1 ||
    { echo "Docker image $image not found: build it with tools/quartus-image.sh" >&2; exit 1; }

out="$top/build/$project"
# Quartus rewrites project files and fills the project directory, so it
# works on a copy with the same depth below the top (the project refers
# to ../../rtl).
work="$top/build/work-$project"
# Emptied in place, not removed: Docker Desktop's file sharing can keep a
# removed directory's old handle, and the next container then starts in
# a directory that no longer exists.
mkdir -p "$out" "$work"
find "$work" -mindepth 1 -delete
cp "$top/projects/$project/$project.qsf" "$top/projects/$project/$project.sdc" "$work/"
if [ -n "${SEED:-}" ]; then
    echo "set_global_assignment -name SEED $SEED" >>"$work/$project.qsf"
fi
# The Hermes-Lite 2 radio core, unmodified, for the projects that use it.
mkdir -p "$work/hl2"
tar -xJf "$top"/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$work/hl2"
echo "compiling $project (a few minutes)"
if ! docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$top:/src" -w "/src/build/work-$project" "$image" \
    quartus_sh --flow compile "$project" >"$out/compile.log" 2>&1; then
    tail -40 "$out/compile.log" >&2
    exit 1
fi
cp "$work/build/$project.rbf" "$out/$project.rbf"
for r in fit.summary sta.summary pin map.rpt sta.rpt fit.rpt; do
    cp "$work/build/$project.$r" "$out/$project.$r"
done
echo "reports in $out (timing: $project.sta.summary; tools/worst.sh $project)"

case "$project" in
    rpi4-i2s | rpi5-pio)
        dest="$top/images/$project/CL025/radioberry-$project.rbf"
        mkdir -p "$(dirname "$dest")"
        cp "$out/$project.rbf" "$dest"
        if command -v sha256sum >/dev/null 2>&1; then
            hash=$(sha256sum "$dest" | cut -d' ' -f1)
        else
            hash=$(shasum -a 256 "$dest" | cut -d' ' -f1)
        fi
        echo "image ${dest#"$top"/}: $(wc -c <"$dest" | tr -d ' ') bytes, sha256 $hash"
        ;;
esac
