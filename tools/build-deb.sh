#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the Debian packages (radioberry, radioberry-dkms, radioberry-dev)
# on a Raspberry Pi 5 running Raspberry Pi OS (trixie, arm64). The version
# comes from `git describe`: v0.2.0 gives 0.2.0, and a build 5 commits later
# 0.2.0+git5.gabc1234, which sorts after 0.2.0 and before 0.2.1. The
# packages are written to build-deb/.
#
#   tools/build-deb.sh [dpkg-buildpackage options]
set -eu

top=$(git rev-parse --show-toplevel)
cd "$top"

describe=$(git describe --tags --match 'v[0-9]*' --dirty)
version=$(echo "${describe#v}" | sed -E 's/-([0-9]+)-g([0-9a-f]+)/+git\1.g\2/; s/-dirty$/.dirty/')

cat >debian/changelog <<END
radioberry ($version) trixie; urgency=medium

  * Built from $describe.

 -- tomdionysus <tomdionysus@users.noreply.github.com>  $(date -R)
END

dpkg-buildpackage -us -uc -b "$@"

mkdir -p build-deb
for f in ../radioberry*_"$version"_*.deb ../radioberry_"$version"_*.buildinfo \
    ../radioberry_"$version"_*.changes; do
    [ -e "$f" ] && mv "$f" build-deb/
done
ls -l build-deb/*"$version"*
