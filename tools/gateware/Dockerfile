# SPDX-License-Identifier: GPL-2.0-or-later
#
# Quartus Prime Lite 23.1std.1 for building the Radioberry gateware.
# Built by tools/build-gateware.sh with a directory holding only the Quartus
# installer and the Cyclone 10 LP device file as the build context. The
# licence agreement is accepted only when ACCEPT_QUARTUS_EULA=1 is passed,
# which the script does only when run with --accept-quartus-eula.
FROM ubuntu:22.04

ARG ACCEPT_QUARTUS_EULA=0
ARG QUARTUS_VERSION=23.1std.1.993

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates xz-utils make locales \
        libglib2.0-0 libfontconfig1 libfreetype6 libx11-6 libxext6 libxrender1 \
        libxtst6 libxi6 libsm6 libice6 libncurses5 libtinfo5 \
    && rm -rf /var/lib/apt/lists/*

COPY QuartusLiteSetup-${QUARTUS_VERSION}-linux.run cyclone10lp-${QUARTUS_VERSION}.qdz /tmp/quartus/
RUN test "$ACCEPT_QUARTUS_EULA" = 1 || { echo "Quartus licence agreement not accepted" >&2; exit 1; } \
    && chmod +x /tmp/quartus/QuartusLiteSetup-${QUARTUS_VERSION}-linux.run \
    && /tmp/quartus/QuartusLiteSetup-${QUARTUS_VERSION}-linux.run --mode unattended \
        --unattendedmodeui none --installdir /opt/altera --accept_eula 1 \
    && rm -rf /tmp/quartus /opt/altera/uninstall /opt/altera/logs

ENV PATH=/opt/altera/quartus/bin:$PATH
