# Raspberry Pi 4

## Requirements

Raspberry Pi 4 Model B, Raspberry Pi OS (Debian 13 trixie, 64-bit), a
6.12 or 6.18 `rpt-rpi-v8` kernel. Same packages, overlay and gateware as
the Pi 5. [Kernels](kernels.md) has the differences between the series.

## Tested kernels

6.12.25, 6.12.34, 6.12.47, 6.12.62, 6.12.75, 6.18.29, 6.18.33, 6.18.34,
6.18.39, 6.18.50, 6.18.60 (`+rpt-rpi-v8`; 6.12.75 with one accepted
anomaly). The tests: [Kernels](kernels.md#tested-kernels).

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
