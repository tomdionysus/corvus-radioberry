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
#   --seed-search N      try fitter seeds 1..N until timing is met
#                        (default: the recorded seed for each variant)
set -eu

QUARTUS_VERSION=23.1std.1.993
IMAGE=radioberry-quartus:23.1std.1

usage() {
    sed -n '4,17p' "$0" | sed 's/^# \{0,1\}//'
}

# Fitter seed that meets timing for each variant with Quartus 23.1std.1.
seed_for() {
    case "$1" in
        CL025) echo 4 ;;
        CL016) echo 5 ;;
    esac
}

installers=""
quartus=""
accept=0
only=""
search=0
inner=0
while [ $# -gt 0 ]; do
    case "$1" in
        --installers) installers=$2; shift 2 ;;
        --accept-quartus-eula) accept=1; shift ;;
        --quartus) quartus=$2; shift 2 ;;
        --only) only=$2; shift 2 ;;
        --seed-search) search=$2; shift 2 ;;
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

# Timing check: every negative-slack path must end in a synchroniser stage
# (sync_chain), which the upstream SDC leaves unconstrained. On the CL016
# only, the AD9866 TX data outputs may miss their output delay by up to
# 3.5 ns; no fitter seed meets it and the image ships with the violation
# documented (gateware/PROVENANCE.md, Timing).
timing_ok() {
    awk -F';' -v type="$2" '
        /^; -[0-9]/ {
            if ($4 ~ /sync_chain\[/) next
            if (type == "CL016" && $4 ~ /^ *rffe_ad9866_tx(sync|\[)/ && $2 + 0 >= -3.5) next
            bad = 1
        }
        END { exit bad }' "$1"
}

# Compile with Quartus on PATH; runs on the host or inside the container.
compile() {
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    tar -xJf "$archive" -C "$work"
    src=$(ls -d "$work"/Hermes-Lite2-*)
    mkdir -p build-gateware
    failed=""
    for type in $variants; do
        lower=$(echo "$type" | tr 'A-Z' 'a-z')
        project="$src/gateware/variants/radioberry_pio_$lower"
        log="build-gateware/radioberry_pio_$lower.log"
        report="build-gateware/radioberry_pio_$lower.timing.txt"
        if [ "$search" -gt 0 ]; then seeds=$(seq 1 "$search"); else seeds=$(seed_for "$type"); fi
        met=""
        for seed in $seeds; do
            echo "== compiling radioberry_pio_$lower, fitter seed $seed"
            rm -rf "$project/build" "$project/db" "$project/incremental_db" "$project/timing.txt"
            (cd "$project" && quartus_sh -t "$top/tools/gateware/compile.tcl" "$seed") \
                >"$log" 2>&1 || { tail -40 "$log" >&2; exit 1; }
            # A project file Quartus could not read is only a warning to it.
            if grep -q -E "couldn't read file|Can't open file" "$log"; then
                grep -E "couldn't read file|Can't open file" "$log" >&2
                echo "radioberry_pio_$lower: a project file is missing" >&2
                exit 1
            fi
            (cd "$project" && quartus_sta -t "$top/tools/gateware/timing-report.tcl") >>"$log" 2>&1
            cp "$project/timing.txt" "$report"
            awk -F';' '/^; -[0-9]/ { printf "   slack %s to %s\n", $2, $4 }' "$report" | sort -u | head -5
            if timing_ok "$report" "$type"; then
                met=$seed
                break
            fi
        done
        if [ -z "$met" ]; then
            awk -F';' '/^; -[0-9]/ && $4 !~ /sync_chain\[/ { printf "   failing: slack %s to %s\n", $2, $4 }' \
                "$report" | sort -u >&2
            echo "radioberry_pio_$lower: timing not met, not written (see $report)" >&2
            failed="$failed $type"
            continue
        fi
        echo "   timing accepted with seed $met"
        echo "$met" >"build-gateware/radioberry_pio_$lower.seed"
        mkdir -p "gateware/rpi-5/$type"
        cp "$project/build/radioberry.rbf" "gateware/rpi-5/$type/radioberry.rbf"
    done
    echo "$failed" >build-gateware/failed
}

if [ "$inner" = 1 ]; then
    compile
    exit 0
fi
rm -f build-gateware/failed

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
        # Stage only the two files as the build context (hard links where
        # possible), not the whole directory they sit in.
        context=$(mktemp -d)
        for f in "QuartusLiteSetup-$QUARTUS_VERSION-linux.run" "cyclone10lp-$QUARTUS_VERSION.qdz"; do
            ln "$installers/$f" "$context/$f" 2>/dev/null || cp "$installers/$f" "$context/$f"
        done
        docker build --platform linux/amd64 -t "$IMAGE" -f tools/gateware/Dockerfile \
            --build-arg ACCEPT_QUARTUS_EULA=1 --build-arg QUARTUS_VERSION="$QUARTUS_VERSION" \
            "$context" || { rm -rf "$context"; exit 1; }
        rm -rf "$context"
    fi
    docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$top:/src" -w /src "$IMAGE" sh tools/build-gateware.sh --inner ${only:+--only "$only"} \
        --seed-search "$search"
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
    lower=$(echo "$type" | tr 'A-Z' 'a-z')
    seed=$(cat "build-gateware/radioberry_pio_$lower.seed" 2>/dev/null || echo "?")
    echo "$f  seed $seed  $s bytes  $h"
    awk -v path="\`rpi-5/$type/radioberry.rbf\`" -v seed="$seed" -v size="$s" -v hash="$h" '
        index($0, "| " path " |") == 1 {
            n = split($0, cell, "|")
            printf "| %s | %s | %s | %s | %s |\n", path, substr(cell[3], 2, length(cell[3]) - 2), seed, size, hash
            next
        }
        { print }' gateware/PROVENANCE.md >gateware/PROVENANCE.md.new
    mv gateware/PROVENANCE.md.new gateware/PROVENANCE.md
done

failed=$(cat build-gateware/failed 2>/dev/null || true)
if [ -n "$failed" ]; then
    echo "timing not met for:$failed" >&2
    exit 1
fi
