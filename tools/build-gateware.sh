#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the Radioberry PIO gateware (CL025 and CL016) from
# gateware/source/ with Quartus Prime Lite, writes
# gateware/rpi-5/<type>/radioberry.rbf and updates gateware/PROVENANCE.md.
# See gateware/QUARTUS.md.
#
#   tools/build-gateware.sh --installers DIR --accept-quartus-eula
#       build inside Docker; DIR holds the Quartus Lite installer and the
#       Cyclone 10 LP .qdz; the flag states that you accept the Quartus
#       Prime licence agreement
#   tools/build-gateware.sh --quartus DIR
#       use an installed Quartus (DIR is its quartus/ directory)
#   --only CL025|CL016   build one variant
set -eu

QUARTUS_VERSION=23.1std.1.993
IMAGE=radioberry-quartus:23.1std.1

usage() {
    sed -n '4,15p' "$0" | sed 's/^# \{0,1\}//'
}

installers=""
quartus=""
accept=0
only=""
inner=0
while [ $# -gt 0 ]; do
    case "$1" in
        --installers) installers=$2; shift 2 ;;
        --accept-quartus-eula) accept=1; shift ;;
        --quartus) quartus=$2; shift 2 ;;
        --only) only=$2; shift 2 ;;
        --inner) inner=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top"
archive=$(ls gateware/source/Hermes-Lite2-*-radioberry-pio.tar.xz)
variants="CL025 CL016"
[ -n "$only" ] && variants=$only

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

size() {
    wc -c <"$1" | tr -d ' '
}

# Compile with Quartus on PATH; runs on the host or inside the container.
compile() {
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    tar -xJf "$archive" -C "$work"
    src=$(ls -d "$work"/Hermes-Lite2-*)
    for type in $variants; do
        lower=$(echo "$type" | tr 'A-Z' 'a-z')
        project="$src/gateware/variants/radioberry_pio_$lower"
        echo "== compiling radioberry_pio_$lower"
        (cd "$project" && quartus_sh --flow compile radioberry -c radioberry >quartus.log 2>&1) || {
            tail -40 "$project/quartus.log" >&2
            exit 1
        }
        mkdir -p "gateware/rpi-5/$type"
        cp "$project/build/radioberry.rbf" "gateware/rpi-5/$type/radioberry.rbf"
    done
}

if [ "$inner" = 1 ]; then
    compile
    exit 0
fi

if [ -n "$quartus" ]; then
    PATH="$quartus/bin:$PATH"
    export PATH
    command -v quartus_sh >/dev/null || { echo "no quartus_sh in $quartus/bin" >&2; exit 1; }
    compile
elif [ -n "$installers" ]; then
    command -v docker >/dev/null || { echo "docker not found" >&2; exit 1; }
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        if [ "$accept" != 1 ]; then
            echo "Building the Quartus image installs Quartus Prime Lite, which requires" >&2
            echo "accepting its licence agreement. Read it, then rerun with" >&2
            echo "--accept-quartus-eula. See gateware/QUARTUS.md." >&2
            exit 1
        fi
        for f in "QuartusLiteSetup-$QUARTUS_VERSION-linux.run" "cyclone10lp-$QUARTUS_VERSION.qdz"; do
            [ -f "$installers/$f" ] || { echo "missing $installers/$f" >&2; exit 1; }
        done
        docker build --platform linux/amd64 -t "$IMAGE" -f tools/gateware/Dockerfile \
            --build-arg ACCEPT_QUARTUS_EULA=1 --build-arg QUARTUS_VERSION="$QUARTUS_VERSION" \
            "$installers"
    fi
    docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$top:/src" -w /src "$IMAGE" sh tools/build-gateware.sh --inner ${only:+--only "$only"}
else
    usage >&2
    exit 2
fi

# Record what was built.
for type in CL025 CL016; do
    f="gateware/rpi-5/$type/radioberry.rbf"
    [ -f "$f" ] || continue
    s=$(size "$f")
    h=$(sha256 "$f")
    echo "$f  $s bytes  $h"
    awk -v path="\`rpi-5/$type/radioberry.rbf\`" -v size="$s" -v hash="$h" '
        index($0, "| " path " |") == 1 {
            n = split($0, cell, "|")
            printf "| %s | %s | %s | %s |\n", path, substr(cell[3], 2, length(cell[3]) - 2), size, hash
            next
        }
        { print }' gateware/PROVENANCE.md >gateware/PROVENANCE.md.new
    mv gateware/PROVENANCE.md.new gateware/PROVENANCE.md
done
