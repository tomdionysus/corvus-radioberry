# Raspberry Pi 4

## Requirements

Raspberry Pi 4 Model B, Raspberry Pi OS (Debian 13 trixie, 64-bit),
kernel 6.18.50+rpt-rpi-v8 or newer. Same packages, overlay and gateware as
the Pi 5.

## Capacity

Two receivers at 48 ksps or one at 96: enough for FT8 receive and
transmit. The Pi 4 has no PIO, so a kernel thread clocks the RadioBerry's
buses and uses part of one CPU core while streaming.

## Driver parameters

| Parameter | Default | Meaning |
|---|---|---|
| `bb_rx_values_max` | 192000 | RX values per second offered to clients; higher values cost a whole CPU core |
| `bb_settle_reads` | 0 | extra pin reads after each bus clock pulse |

The common parameters are in [Kernel driver](driver.md).
