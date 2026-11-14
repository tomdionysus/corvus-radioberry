# Raspberry Pi 4

## Requirements

Raspberry Pi 4 Model B, Raspberry Pi OS (Debian 13 trixie, 64-bit), a
6.12 or 6.18 `rpt-rpi-v8` kernel. Same packages and overlay as the Pi 5.
[Kernels](kernels.md) has the differences between the series.

The RadioBerry uses the Pi's PCM (I2S) block on GPIO 18 to 21, so leave
`dtparam=i2s=on` and audio HAT overlays out of `config.txt`.

## Capacity

The I2S link carries about 1.54 million RX values per second: for example
six receivers at 96 ksps, four at 192 ksps or two at 384 ksps. Samples
move by DMA; piHPSDR at 192 ksps used at most 13% CPU.

The common driver parameters are in [Kernel driver](driver.md).
