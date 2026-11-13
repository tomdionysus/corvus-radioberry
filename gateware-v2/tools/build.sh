#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compiles a gateware-v2 project with Quartus Prime Lite in the Docker
# image the repository's gateware build already uses
# (gateware/QUARTUS.md), and copies the result to
# gateware-v2/build/<project>/.
#
#   tools/build.sh linktest
#   SEED=4 tools/build.sh rpi4-i2s
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
image=radioberry-quartus:23.1std.1
project=${1:-linktest}
[ -f "$top/projects/$project/$project.qsf" ] || { echo "no project $project" >&2; exit 2; }
docker image inspect "$image" >/dev/null 2>&1 ||
    { echo "Quartus image $image not found: see gateware/QUARTUS.md" >&2; exit 1; }

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
# Fitter seed: SEED=n tools/build.sh PROJECT (default: the project's own).
if [ -n "${SEED:-}" ]; then
    echo "set_global_assignment -name SEED $SEED" >>"$work/$project.qsf"
fi
# The Hermes-Lite 2 source, unmodified, for projects that use the radio core.
mkdir -p "$work/hl2"
tar -xJf "$top"/../gateware/source/Hermes-Lite2-*-radioberry-pio.tar.xz -C "$work/hl2"
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
echo "built $out/$project.rbf"
