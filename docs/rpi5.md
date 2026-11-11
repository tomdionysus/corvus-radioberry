# Raspberry Pi 5

## Requirements

Raspberry Pi OS (Debian 13 trixie, 64-bit), kernel 6.18.60+rpt-rpi-2712
or newer. The stock `rp1-pio` driver is used; no kernel patches.

## Capacity

Up to six receivers at 192 ksps or three at 384 ksps (about 2.37 million
RX values per second).

## Driver parameters

| Parameter | Default | Meaning |
|---|---|---|
| `rx_clkdiv` | 2 | RX PIO clock divider |
| `tx_clkdiv` | 8 | TX PIO clock divider |

The common parameters are in [Kernel driver](driver.md).
