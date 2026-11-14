#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the Docker image radioberry-quartus:23.1std.1: Quartus Prime Lite
# 23.1std.1 with Cyclone 10 LP support, which tools/build.sh compiles in.
# Needs an x86-64 Docker host (or Docker with amd64 emulation) and about
# 10 GB of disk. See docs/toolchain.md.
#
#   tools/quartus-image.sh --installers DIR --accept-quartus-eula
#
# DIR holds QuartusLiteSetup-23.1std.1.993-linux.run and
# cyclone10lp-23.1std.1.993.qdz from the Altera download centre.
# --accept-quartus-eula states that you have read and accepted the Quartus
# Prime licence agreement; without it nothing is installed.
set -eu

QUARTUS_VERSION=23.1std.1.993
IMAGE=radioberry-quartus:23.1std.1
top=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    sed -n '4,15p' "$0" | sed 's/^# \{0,1\}//'
}

installers=""
accept=0
while [ $# -gt 0 ]; do
    case "$1" in
        --installers) installers=$2; shift 2 ;;
        --accept-quartus-eula) accept=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
[ -n "$installers" ] || { usage >&2; exit 2; }
command -v docker >/dev/null || { echo "docker not found" >&2; exit 1; }
if docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "$IMAGE already exists"
    exit 0
fi
if [ "$accept" != 1 ]; then
    echo "Building the image installs Quartus Prime Lite, which requires accepting" >&2
    echo "its licence agreement. Read it, then rerun with --accept-quartus-eula." >&2
    exit 1
fi
files="QuartusLiteSetup-$QUARTUS_VERSION-linux.run cyclone10lp-$QUARTUS_VERSION.qdz"
for f in $files; do
    [ -f "$installers/$f" ] || { echo "missing $installers/$f" >&2; exit 1; }
done
# Only the two files form the build context (hard links where possible).
context=$(mktemp -d)
trap 'rm -rf "$context"' EXIT
for f in $files; do
    ln "$installers/$f" "$context/$f" 2>/dev/null || cp "$installers/$f" "$context/$f"
done
docker build --platform linux/amd64 -t "$IMAGE" -f "$top/tools/quartus.Dockerfile" \
    --build-arg ACCEPT_QUARTUS_EULA=1 --build-arg QUARTUS_VERSION="$QUARTUS_VERSION" "$context"
echo "built $IMAGE"
