# Kernel driver

`radioberry.ko` binds to the overlay's `sdr,radioberry` device, loads the
gateware, and gives userspace one device, `/dev/radioberry`. Building it
and how it works: [driver/README.md](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/README.md).

## Boards

The driver picks the link to the FPGA from the SoC when it binds;
everything else is shared:

| Board | Link | Gateware image | sysfs `transport` | Details |
|---|---|---|---|---|
| Raspberry Pi 4 (BCM2711) | I2S into the PCM block, with DMA | `radioberry-rpi4-i2s.rbf` | `i2s` | [Raspberry Pi 4](rpi4.md) |
| Raspberry Pi 5 (BCM2712) | RP1 PIO, with DMA | `radioberry-rpi5-pio.rbf` | `rp1-pio` | [Raspberry Pi 5](rpi5.md) |

The driver reports its transport and RX limit in `struct radioberry_info`
(`transport`, `rx_values_max`); `radioberryd` runs no more receivers than
the limit carries.

## Overlay

`dtoverlay=radioberry` ([driver/radioberry.dts](https://github.com/tomdionysus/corvus-radioberry/blob/main/driver/radioberry.dts))
is the same file on both boards. It enables `spi0`, creates the
`sdr,radioberry` device on `spi0` chip select 0 (SPI mode 3, 12.5 MHz)
with the five FPGA configuration GPIOs, and disables the spidev nodes.
`dtoverlay=radioberry,spi_speed=<Hz>` sets another SPI clock, up to
15.625 MHz. On a Pi 4 the driver uses the PCM block (GPIO 18 to 21)
itself, so `dtparam=i2s=on` and audio overlays that claim it must be off.

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
  the TX permission bit, PA allowed and drive, then sets one receiver at
  48 ksps.
- Receive moves only while the last command sent had run set. With run
  clear (from boot, while closed, and while `radioberryd` has no client
  streaming) the link stands still, so the bus and the driver are idle.
- Transmit is keyed only while the client keeps sending commands with PTT
  and the permission bit; the gateware drops keying by itself if the
  driver stops renewing it for a second.

## Gateware loading and sysfs

The driver loads the board's image from `/lib/firmware` when it binds, at
boot, and publishes in `/sys/bus/spi/devices/spi0.0/`:

| Attribute | Meaning |
|---|---|
| `gateware_version` | the Hermes-Lite 2 core version clients are shown |
| `fpga_type`, `receivers`, `transmitters` | from the image |
| `transport` | `i2s` or `rp1-pio` |
| `reload` | write 1 to reload the gateware (EBUSY while the device is open) |
| `rx_stalled`, `rx_stalls` | 1 while no RX words have arrived for 200 ms with run set; how often that happened |
| `counters` | the image's link and transmit counters |
| `tx_loopback` | 1 puts the AD9866 in digital loopback, PA lines held low, for a transmit self-test |

After a reload the statistics restart from zero; if it fails, opening
the device gives ENODEV until a reload succeeds.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `load_gateware` | 1 | load the gateware at bind; 0 keeps the FPGA as it is |
| `rx_ring_kib` | 512 | RX ring size, KiB, rounded up to a power of two |
| `tx_ring_kib` | 64 | TX ring size, KiB, rounded up to a power of two |
| `debug` | 0 | 1: rate-limited diagnostics (each command with its reply, RX resyncs and dropped words per period, RX overruns, TX underruns); 2: every command and receiver-count change without rate limiting |

The Pi 5's own parameters are in [Raspberry Pi 5](rpi5.md). `debug` is
writable at run time in `/sys/module/radioberry/parameters/`.

The module reloads with `rmmod radioberry; modprobe radioberry` (add
`load_gateware=0` to keep the FPGA as it is). Stop `radioberry.service`
first: it holds the device.
