#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The merge gate. Builds and tests everything on a Raspberry Pi 5:
# format and SPDX checks, the release build with its tests, the ASan and
# UBSan build with the same tests, and the coverage build with a summary.
#
#   tools/ci-pi.sh                  run here (on the Pi)
#   tools/ci-pi.sh --remote HOST    copy this working tree to HOST over SSH
#                                   and run there; HOST is [user@]address
#
# RADIOBERRY_CI_HOST may stand in for --remote HOST. The remote copy lives
# in ~/radioberry-ci on the host and is reused between runs.
set -eu

usage() {
    sed -n '4,13p' "$0" | sed 's/^# \{0,1\}//'
}

remote=${RADIOBERRY_CI_HOST:-}
while [ $# -gt 0 ]; do
    case "$1" in
        --remote)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            remote=$2
            shift 2
            ;;
        --local)
            remote=""
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

top=$(git rev-parse --show-toplevel)
cd "$top"

if [ -n "$remote" ]; then
    dest=radioberry-ci
    echo "== copying working tree to $remote:$dest"
    ssh -o BatchMode=yes "$remote" "mkdir -p $dest"
    # Tracked and untracked-but-not-ignored files, plus .git for the version.
    # Owned by the remote user, so git there trusts the copy.
    git ls-files -co --exclude-standard -z |
        rsync -a --no-owner --no-group --delete --from0 --files-from=- ./ "$remote:$dest/"
    rsync -a --no-owner --no-group --delete .git/ "$remote:$dest/.git/"
    exec ssh -o BatchMode=yes "$remote" \
        "cd $dest && git update-index -q --refresh >/dev/null 2>&1; sh tools/ci-pi.sh --local"
fi

jobs=$(nproc 2>/dev/null || echo 2)
step() { echo; echo "== $*"; }

step "format"
sh tools/check-format.sh

step "SPDX headers"
sh tools/check-spdx.sh

step "release build and tests"
cmake --preset default >/dev/null
cmake --build --preset default -j "$jobs"
ctest --preset default

step "ASan and UBSan build and tests"
cmake --preset asan >/dev/null
cmake --build --preset asan -j "$jobs"
ctest --preset asan

step "coverage"
cmake --preset coverage >/dev/null
cmake --build --preset coverage -j "$jobs"
cmake --build --preset coverage --target coverage

echo
echo "== ci-pi: all checks passed ($(git describe --tags --always --dirty 2>/dev/null || echo unknown))"
