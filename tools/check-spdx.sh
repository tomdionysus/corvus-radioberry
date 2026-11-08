#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fails if a source file lacks the SPDX line in its first two lines (the
# second allows for a shebang). Run from anywhere inside the repository.
# Every file is GPL-2.0-or-later except vendored third-party code under a
# */ft8_lib/ directory, which keeps its MIT licence.
set -eu

cd "$(git rev-parse --show-toplevel)"

missing=0
for f in $(git ls-files -co --exclude-standard -- \
        '*.c' '*.h' '*.cpp' '*.hpp' '*.in' '*.cmake' '*.sh' '*.py' '*.dts' \
        'CMakeLists.txt' '*/CMakeLists.txt' 'Kbuild' '*/Kbuild' 'Makefile' '*/Makefile'); do
    [ -f "$f" ] || continue
    licence='GPL-2.0-or-later'
    case "$f" in
        */ft8_lib/*) licence='MIT' ;;
    esac
    if ! head -n 2 "$f" | grep -q "SPDX-License-Identifier: $licence"; then
        echo "missing SPDX header: $f"
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    exit 1
fi
echo "SPDX headers: ok"
