# SPDX-License-Identifier: GPL-2.0-or-later
# Simulator image for the gateware testbenches (tools/sim.sh): Icarus
# Verilog and Verilator from Debian.
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends iverilog verilator make g++ \
    && rm -rf /var/lib/apt/lists/*
