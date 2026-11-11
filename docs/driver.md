# Kernel driver

`radioberry.ko` (`driver/`) binds to the overlay's `sdr,radioberry`
device, loads the gateware, and gives userspace one device,
`/dev/radioberry`. The design is in [Design](design.md); the gateware
interface it drives is in
[gateware/README.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/gateware/README.md).

## Transports

The streams reach the gateware one of two ways, chosen when the driver
binds, from the SoC; everything else is shared:

| Board | Transport | sysfs `transport` | Details |
|---|---|---|---|
| Raspberry Pi 4 (BCM2711) | CPU-clocked kernel thread | `cpu` | [Raspberry Pi 4](rpi4.md) |
| Raspberry Pi 5 (BCM2712) | RP1 PIO with DMA | `rp1-pio` | [Raspberry Pi 5](rpi5.md) |

The driver reports its transport and RX limit in `struct radioberry_info`
(`transport`, `rx_values_max`); `radioberryd` runs no more receivers than
the limit carries.

## Overlay

`dtoverlay=radioberry` ([driver/radioberry.dts](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/radioberry.dts),
built and installed by this repository) is the same file on both boards:
it uses only labels both boards' device trees define. It enables `spi0`,
creates the `sdr,radioberry` device on `spi0` chip select 0 (SPI mode 3,
12.5 MHz) with the five FPGA configuration GPIOs and the stream GPIOs,
and disables the spidev nodes. `dtoverlay=radioberry,spi_speed=<Hz>` sets
another SPI clock; the gateware's timing constraints allow up to
15.625 MHz. `tools/check-overlay.sh` checks that it applies to both
boards' trees.

## `/dev/radioberry`

- Exclusive open (EBUSY while another program holds it).
- `read()` returns whole RX frames: 24-bit big-endian I and Q per
  receiver. `poll()` reports a readable frame; a ring overrun gives
  `POLLERR`.
- `write()` takes 16-bit I/Q TX samples.
- ioctls send gateware commands and read the identity and statistics.
  [driver/include/radioberry_ioctl.h](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/include/radioberry_ioctl.h)
  documents the ABI.
- Closing the device sends the gateware a command that clears run, PTT,
  the TX permission bit, PA enable and drive.
- Receive moves only while the last command sent had run set. With run
  clear (from boot, while closed, and while `radioberryd` has no client
  streaming) the RX clock stands still, so the bus and the driver's thread
  are idle. When run is set again the stream restarts and the parser
  settles to whole frames, as after a receiver-count change.

## Gateware loading and sysfs

The driver loads `/lib/firmware/radioberry.rbf` when it binds, at boot,
and publishes the gateware identity in `/sys/bus/spi/devices/spi0.0/`:
`gateware_version`, `fpga_type`, `receivers`, `transmitters`, and its
`transport`.

Writing 1 to `reload` there reloads the gateware, re-reads the identity
and restarts the streams (about 5.5 s). It returns EBUSY while
`/dev/radioberry` is open, statistics restart from zero, and if the
reload fails, opening the device gives ENODEV until a reload succeeds.

## Receive lock-up workaround

The gateware can lock up its receive stream at a receiver-count change
([receive lock-up](fault-rx-lockup.md)). The driver holds its RX clock still from
just before a receiver-count command until `rx_clock_hold_us` after it.
Should the stream stop anyway, the driver logs "RX stream stalled" after
200 ms without RX words while run is set, sysfs `rx_stalled` reads 1 and
`rx_stalls` counts the events. Writing 1 to `reload` recovers.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `load_gateware` | 1 | load the gateware at bind; 0 keeps the FPGA as it is |
| `rx_ring_kib` | 512 | RX ring size, KiB, rounded up to a power of two |
| `tx_ring_kib` | 64 | TX ring size, KiB, rounded up to a power of two |
| `rx_clock_hold_us` | 200 | RX clock hold across a receiver-count change; 0 disables it |
| `debug` | 0 | 1: rate-limited diagnostics (each command with its reply, RX resyncs and dropped words per period, RX overruns, TX underruns); 2: every command and receiver-count change without rate limiting |

Each board's transport has its own parameters ([Raspberry Pi 4](rpi4.md),
[Raspberry Pi 5](rpi5.md)). `rx_clock_hold_us` and `debug` are writable at
run time in `/sys/module/radioberry/parameters/`.

The module reloads with `rmmod radioberry; modprobe radioberry` (add
`load_gateware=0` to keep the FPGA as it is). Stop `radioberry.service`
first: it holds the device.

## Under a debug kernel

The driver has been run under KASAN, lockdep, DMA API checking, kmemleak
and UBSAN with no reports ([Driver under a debug kernel](kernel-debug.md)).
