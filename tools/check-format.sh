#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Checks C and C++ formatting against .clang-format with clang-format 19.
# With --fix, rewrites the files instead. The kernel module and the ABI
# header it shares with userspace follow the kernel's own style and are not
# checked here, nor is the link test's measurement module
# (gateware/host/kmod/), nor vendored third-party code (*/ft8_lib/).
set -eu

cd "$(git rev-parse --show-toplevel)"

clang_format=""
for candidate in clang-format-19 clang-format; do
    if command -v "$candidate" >/dev/null 2>&1; then
        clang_format=$candidate
        break
    fi
done
if [ -z "$clang_format" ]; then
    echo "clang-format not found (apt install clang-format-19)" >&2
    exit 1
fi
if ! "$clang_format" --version | grep -q 'version 19\.'; then
    echo "warning: $("$clang_format" --version) is not clang-format 19" >&2
fi

files=$(git ls-files -co --exclude-standard -- '*.c' '*.h' '*.cpp' '*.hpp' |
    grep -v -e '^driver/kernel/' -e '^driver/include/' -e '/ft8_lib/' \
        -e '^gateware/host/kmod/' || true)
if [ -z "$files" ]; then
    echo "format: no files"
    exit 0
fi

if [ "${1:-}" = "--fix" ]; then
    # shellcheck disable=SC2086
    "$clang_format" -i $files
    echo "format: rewritten"
else
    # shellcheck disable=SC2086
    "$clang_format" --dry-run --Werror $files
    echo "format: ok"
fi
