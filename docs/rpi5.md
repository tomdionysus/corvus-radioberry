# Raspberry Pi 5

## Requirements

Raspberry Pi OS (Debian 13 trixie, 64-bit), a 6.12 or 6.18
`rpt-rpi-2712` kernel. The stock `rp1-pio` driver is used; no kernel
patches. How the driver handles the two series: [Kernels](kernels.md).

## Tested kernels

6.12.25, 6.12.34, 6.12.47, 6.12.62, 6.12.75, 6.18.29, 6.18.33, 6.18.34,
6.18.39, 6.18.50, 6.18.60 (`+rpt-rpi-2712`). The tests:
[Kernels](kernels.md#tested-kernels).

## Capacity

Up to six receivers at 192 ksps or three at 384 ksps (about 2.37 million
RX values per second).

## Driver parameters

| Parameter | Default | Meaning |
|---|---|---|
| `rx_clkdiv` | 2 | RX PIO clock divider |
| `tx_clkdiv` | 8 | TX PIO clock divider |

The common parameters are in [Kernel driver](driver.md).
