# Raspberry Pi 4

## Requirements

Raspberry Pi 4 Model B, Raspberry Pi OS (Debian 13 trixie, 64-bit), a
6.12 or 6.18 `rpt-rpi-v8` kernel. Same packages, overlay and gateware as
the Pi 5. [Kernels](kernels.md) has the differences between the series.

## Enable SMI

The Pi 4 has no PIO. With the `radioberry-smi` overlay, its Secondary
Memory Interface reads the RadioBerry's RX bus with DMA, and receive
costs about 1% of the CPU; without it, a kernel thread clocks the bus and
holds a whole core from one receiver at 96 ksps
([CPU cost of receive](finding-rpi4-cpu.md)). In `/boot/firmware/config.txt`,
after `dtoverlay=radioberry`:

```
[pi4]
dtoverlay=radioberry-smi
[all]
```

Reboot. `cat /sys/bus/spi/devices/spi0.0/transport` reads `smi`, and
`rbprobe` says so. If SMI cannot start, the driver logs why and clocks
the bus with the CPU instead (`transport` reads `cpu`).

The overlay switches GPIO 6, 18 to 21 and 25 to their SMI function and
leaves SPI, I2C and the TX pins as they are. TX is clocked by the CPU on
either transport.

## Capacity

| Transport | RX values per second | For example |
|---|---|---|
| `smi` | 1 000 000 | One receiver at 384 ksps, two at 192, five at 96 |
| `cpu` | 192 000 | Two receivers at 48 ksps or one at 96 |

The daemon runs no more receivers than these allow.

Measured with `smi`, all four cores together: 0.5 to 1.4% for receive
at one receiver at 48 to 384 ksps, and piHPSDR at one receiver at 192
ksps left every core below 30%
([CPU cost of receive](finding-rpi4-cpu.md#the-fix)).

## Tested kernels

6.12.25, 6.12.34, 6.12.47, 6.12.62, 6.12.75, 6.18.29, 6.18.33, 6.18.34,
6.18.39, 6.18.50, 6.18.60 (`+rpt-rpi-v8`; 6.12.75 with one accepted
anomaly). The tests: [Kernels](kernels.md#tested-kernels).

## Driver parameters

| Parameter | Default | Meaning |
|---|---|---|
| `bb_rx_values_max` | 192000 | `cpu` transport: RX values per second offered to clients; higher values cost a whole CPU core |
| `bb_settle_reads` | 0 | extra pin reads after each bus clock pulse |

The common parameters are in [Kernel driver](driver.md).
