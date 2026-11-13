#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The merge gate. Builds and tests everything on a Raspberry Pi 5:
# format and SPDX checks, the release build with its tests, the ASan and
# UBSan build and the TSan build with the same tests, the coverage build
# with a summary,
# where the ABI header is installed the userspace build against it, and
# where /dev/radioberry is present and free the driver's hardware tests.
#
#   tools/ci-pi.sh                  run here (on the Pi)
#   tools/ci-pi.sh --remote HOST    copy this working tree to HOST over SSH
#                                   and run there; HOST is [user@]address
#   tools/ci-pi.sh --local          run here even if RADIOBERRY_CI_HOST is set
#
# RADIOBERRY_CI_HOST may stand in for --remote HOST. The remote copy lives
# in ~/radioberry-ci on the host and is reused between runs.
set -eu

usage() {
    sed -n '4,16p' "$0" | sed 's/^# \{0,1\}//'
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

kernel_sources="driver/kernel/radioberry.c driver/kernel/rb_logic.h driver/kernel/rb_pio.h
    driver/kernel/rb_bitbang.h driver/kernel/rb_smi.h driver/include/radioberry_ioctl.h
    tools/rbsmi/kmod/rbsmi_dma.c"
kbuild=/lib/modules/$(uname -r)/build
checkpatch=${CHECKPATCH:-$kbuild/scripts/checkpatch.pl}
if [ -f "$checkpatch" ]; then
    step "checkpatch"
    # shellcheck disable=SC2086
    perl "$checkpatch" --no-tree --file --quiet --show-types $kernel_sources
    echo "checkpatch: ok"
else
    step "checkpatch: skipped, set CHECKPATCH to the kernel's scripts/checkpatch.pl"
fi

if command -v sparse >/dev/null && [ -f "$kbuild/Makefile" ]; then
    step "sparse"
    rm -rf build-sparse && mkdir -p build-sparse && cp driver/kernel/* build-sparse/
    out=$(make -s -C "$kbuild" M="$top/build-sparse" RB_INCLUDE="$top/driver/include" C=2 \
        modules 2>&1)
    if [ -n "$out" ]; then
        echo "$out"
        exit 1
    fi
    echo "sparse: ok"
else
    step "sparse: skipped, needs sparse and the running kernel's headers"
fi

step "release build and tests"
cmake --preset default >/dev/null
cmake --build --preset default -j "$jobs"
ctest --preset default

if command -v dtmerge >/dev/null && [ -f build/driver/radioberry.dtbo ] &&
    [ -f /boot/firmware/bcm2711-rpi-4-b.dtb ]; then
    step "overlays on the Pi 4 and Pi 5 device trees"
    sh tools/check-overlay.sh build/driver/radioberry.dtbo build/driver/radioberry-smi.dtbo
else
    step "overlay check: skipped, needs dtmerge and the boards' trees in /boot/firmware"
fi

if command -v run-clang-tidy-19 >/dev/null; then
    step "clang-tidy"
    # Sources only: the tests' Catch2 macros and the vendored ft8_lib are
    # not held to it.
    run-clang-tidy-19 -p build -j "$jobs" -quiet \
        '^(?!.*ft8_lib)(?!.*/tests/).*/(daemon|tools|examples|driver)/.*\.cpp$' >build/clang-tidy.log 2>&1 ||
        { grep -E "(warning|error):" build/clang-tidy.log | sort -u; exit 1; }
    echo "clang-tidy: ok"
else
    step "clang-tidy: skipped, run-clang-tidy-19 not installed"
fi

# Each sanitizer needs an address space it supports: GCC's AddressSanitizer
# does not run under the Pi 4's 39-bit one, ThreadSanitizer not under the
# Pi 5's 47-bit one. So the Pi 5 runs ASan and the Pi 4 TSan.
sanitizer_runs() {
    probe=$(mktemp -d)
    printf 'int main(void) { return 0; }\n' >"$probe/a.c"
    ok=1
    if cc "-fsanitize=$1" -o "$probe/a" "$probe/a.c" 2>/dev/null && "$probe/a" 2>/dev/null; then
        ok=0
    fi
    rm -rf "$probe"
    return $ok
}

if sanitizer_runs address; then
    step "ASan and UBSan build and tests"
    cmake --preset asan >/dev/null
    cmake --build --preset asan -j "$jobs"
    ctest --preset asan
else
    step "ASan and UBSan: skipped, AddressSanitizer cannot run under this kernel ($(uname -r))"
fi

if sanitizer_runs thread; then
    step "ThreadSanitizer build and tests"
    cmake --preset tsan >/dev/null
    cmake --build --preset tsan -j "$jobs"
    ctest --preset tsan
else
    step "ThreadSanitizer: skipped, it cannot run under this kernel ($(uname -r))"
fi

step "coverage"
# Test discovery runs the binaries while building; clear stale counters first.
[ -d build-coverage ] && find build-coverage -name '*.gcda' -delete
cmake --preset coverage >/dev/null
cmake --build --preset coverage -j "$jobs"
cmake --build --preset coverage --target coverage

# cmake --install puts the header in /usr/local/include, radioberry-dev in
# /usr/include.
installed_abi=/usr/local/include/radioberry_ioctl.h
[ -f "$installed_abi" ] || installed_abi=/usr/include/radioberry_ioctl.h
if [ -f "$installed_abi" ]; then
    step "userspace against the installed ABI header"
    cmake -S . -B build-abi -DRADIOBERRY_ABI_INCLUDE_DIR="$(dirname "$installed_abi")" \
        -DRADIOBERRY_BUILD_MODULE=OFF -DRADIOBERRY_BUILD_TESTS=OFF >/dev/null
    cmake --build build-abi -j "$jobs" --target rbprobe rbcat rbctl
    # No userspace compile may see the source tree's copy.
    python3 - build-abi/compile_commands.json <<'PY'
import json, sys
bad = [e["file"] for e in json.load(open(sys.argv[1]))
       if "/driver/" not in e["file"] and "driver/include" in e["command"]]
if bad:
    sys.exit("source-tree ABI header on the include path of: " + ", ".join(bad))
print("no userspace source sees driver/include")
PY
else
    step "userspace against the installed ABI header: skipped, $installed_abi absent"
fi

# Receive only: the hardware tests never set PTT or the TX permission bit.
if [ -c /dev/radioberry ]; then
    if python3 -c 'import os; os.close(os.open("/dev/radioberry", os.O_RDWR))' 2>/dev/null; then
        step "driver hardware tests on /dev/radioberry"
        build/tools/driver-tests/rbdrivertest
    else
        step "driver hardware tests: skipped, /dev/radioberry is busy"
    fi
else
    step "driver hardware tests: skipped, no /dev/radioberry"
fi

echo
echo "== ci-pi: all checks passed ($(git describe --tags --always --dirty 2>/dev/null || echo unknown))"
