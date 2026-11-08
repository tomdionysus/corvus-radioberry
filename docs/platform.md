# Platform: the development Raspberry Pi 5

This note describes the machine corvus-radioberry is developed and tested
on, as found over SSH on 2026-11-08 and updated the same day. Everything
here was read from the live system. Host-specific identifiers (addresses,
names, serials) are deliberately omitted.

## Access

- One dedicated Pi 5, reachable over SSH as root, with the Radioberry and
  the PA board attached. It is a development box: reboots and configuration
  changes need no coordination.
- Network is Wi-Fi only; the Ethernet port has no link. Any Protocol 1 or 2
  traffic to a client on the LAN crosses Wi-Fi, which matters for UDP
  jitter measurements.
- A desktop session with VNC (labwc, wayvnc and the RealVNC service) runs
  for the ordinary user account.

## Hardware

| Item | Value |
|---|---|
| Board | Raspberry Pi 5 Model B Rev 1.0, 8 GB |
| Storage | Micron 2550 NVMe (DRAM-less) on PCIe, about 1 TB root, 512 MB boot |
| Bootloader | 2026-10-30 release, BOOT_ORDER 0xf416, BOOT_UART=1, NET_INSTALL_AT_POWER_ON=1 |
| Cooling | PWM fan driver loaded, CPU 28 C idle, no throttling flags |
| HAT EEPROM | none detected (no `/proc/device-tree/hat`), as expected for the Radioberry |

The Radioberry 2.x card and the PA board are stacked on the 40-pin header.
See `pa.md` for the amplifier. The device tree overlay has created the
Radioberry's SPI control device, so the overlay side is live.

**The FPGA is an Intel Cyclone 10 LP 10CL025.** Established on 2026-11-08
by bit-banging the upstream Pi 5 gateware loader (`loadgateware3.c`,
libgpiod 2) with both upstream rpi-5 GPIO-mode images: the CL016 image
left CONF_DONE (GPIO 22) low, the CL025 image configured with CONF_DONE
and nSTATUS (GPIO 26) both high. `rbprobe --identify` repeats the test.

### Known gateware images

`rbprobe` identifies images by hash. The driver needs this project's
75.3 PIO build (`gateware/PROVENANCE.md`): CL025 SHA-256
a25f30f4bc9ba3c3f7fbe3406f4a4a27da99be7e90c5272d1b6c05ee0acecedc, 372888
bytes. The upstream Radioberry-2.x images below are the 73.3 GPIO-mode
gateware (8-bit RX bus, TX over SPI chip select 1) and do not work with
it.

| Image (Radioberry-2.x, commit 269e402) | Family | FPGA | Size | SHA-256 |
|---|---|---|---|---|
| `SBC/rpi-5/archive/releases/dev/CL025/radioberry.rbf` | rpi-5 GPIO | 10CL025 | 334915 | fe6a4e8acb67fbf80aa679affc8f2e0e0abe15908938cb2aed10804b461105cd |
| `SBC/rpi-5/archive/releases/dev/CL016/radioberry.rbf` | rpi-5 GPIO | 10CL016 | 275438 | 5f146a8c2113a888bd6b7d08d8ac9f62b796e79dd950ecd47b31b5ec1ebe9e48 |
| `SBC/rpi-4/releases/dev/CL016/radioberry.rbf` | rpi-4 GPIO | 10CL016 | 276541 | aa5c36f42456bc5dee622a694db2b36aa31dadfbca2372e00e1cd952e7217da3 |

On the development Pi, `/lib/firmware/radioberry.rbf` is this project's
CL025 PIO build (2026-11-08).

## Operating system

| Item | Value |
|---|---|
| Distribution | Debian 13 (trixie), Raspberry Pi OS 64-bit |
| Kernel | 6.18.60+rpt-rpi-2712, built 2026-11-03 |
| Also installed | 6.18.50+rpt-rpi-2712 (older image kernel) |
| Headers | linux-headers for both kernels, plus the rpi-2712 and rpi-v8 metapackages |
| Toolchain | gcc 14.2, g++ 14.2, device-tree-compiler; cmake 3.31 available from apt, not yet installed |
| Libraries | libfftw3-dev, libgtk-3-dev, libgpiod-dev 2.2.1, i2c-tools 4.4; libliquid-dev 1.7.0 available from apt, not yet installed |

The system was cloned from SD to the NVMe with a script; comments it
appended to `config.txt` record the partition identifiers.

## Boot configuration (`/boot/firmware/config.txt`)

Relevant lines:

```
dtparam=i2c_arm=on
dtparam=spi=off
dtparam=audio=on
dtoverlay=vc4-kms-v3d
[pi5]
dtoverlay=nospi10
[all]
dtoverlay=radioberry
```

Notes:

- `dtparam=i2c_arm=on` was enabled on 2026-11-08 (`raspi-config nonint
  do_i2c 0`, which also adds `i2c-dev` to `/etc/modules`) and the Pi
  rebooted. `/dev/i2c-1` now exists. `i2cdetect -y 1` finds no devices at
  all: no PA ADC at 0x34, no bias pot at 0x2C, no filter board at 0x20 to
  0x22. Buses 13 and 14 are the HDMI DDC links and are irrelevant.
- `dtparam=spi=off` is overridden by this project's radioberry overlay,
  which enables `spi0` and puts the `sdr,radioberry` device at `spi0.0`
  (12.5 MHz, mode 3) with the five FPGA configuration GPIOs. `gpioinfo`
  shows `spi0 CS0` on GPIO 8 and `spi0 CS1` on GPIO 7, and `pinctrl` GPIO
  9, 10, 11 in SPI0 alt function.
- The overlay also disables `spidev0` and `spidev1` and enables `rp1_pio`.

## Radioberry software state

Installed from this repository by `cmake --install` (2026-11-08):

| Path | State |
|---|---|
| `/boot/firmware/overlays/radioberry.dtbo` | this project's overlay, active |
| `/lib/modules/<kernel>/updates/radioberry.ko` | loads at boot; `/dev/radioberry` present; gateware identity under `/sys/bus/spi/devices/spi0.0/` |
| `/lib/firmware/radioberry.rbf` | this project's CL025 PIO build, gateware 75.3 |
| `/usr/local/include/radioberry_ioctl.h` | ABI header |
| `/usr/local/bin/rbprobe`, `/usr/local/share/radioberry/gateware/` | probe and both gateware images |

Not from this repository:

| Path | State |
|---|---|
| `/usr/local/bin/pihpsdr` | built 2026-11-07 from the g0orx submodule |
| `/usr/local/lib/libwdsp.so`, `/usr/local/include/wdsp.h` | built from a g0orx/wdsp checkout (49084f5, 2022-01-17) |

The upstream init script, its `radioberry.service`, overlay and archive
are kept in the root account's home directory, out of the system paths.
No radioberry daemon or service is installed yet.

## Kernel facts that decide whether the upstream driver can build

- `rp1_pio` is an in-tree module in this kernel
  (`drivers/misc/rp1-pio.ko.xz`, author Phil Elwell) and is loaded. It
  created `pio0` with 38 ops and 2 interrupts.
- The kernel API header is
  `/usr/src/linux-headers-6.18.60+rpt-common-rpi/include/linux/pio_rp1.h`
  (reached through the `build` symlink), with the ioctl argument structs in
  `include/uapi/misc/rp1_pio_if.h`. `RP1_PIO_ORIGIN_ANY` and the
  `PIO_DIR_FROM_SM` / `PIO_DIR_TO_SM` enums the driver uses are present.
- The installed headers' `Module.symvers` exports 51 `rp1_pio_*` symbols.
  `rp1_pio_open`, `rp1_pio_sm_claim`, `rp1_pio_sm_config_xfer`,
  `rp1_pio_sm_xfer_data` (with the same eight-argument signature the driver
  calls), `rp1_pio_sm_drain_tx`, `rp1_pio_gpio_set_function` and
  `rp1_pio_gpio_set_pulls` are all there. The `KBUILD_EXTRA_SYMBOLS` line in
  the upstream Makefile is therefore unnecessary and must be removed, since
  its target does not exist.
- **`rp1_pio_sm_buffer_virt` is not exported by this kernel and is not
  declared in its headers.** The upstream rpi-5 driver calls it in both the
  RX and TX stream code to write straight into the PIO DMA buffers. That
  function came from a patched rp1-pio tree (the one the upstream Makefile
  refers to). This is the concrete reason the upstream module cannot be
  built against the stock 6.18 kernel as-is, and the likely reason it
  worked on an older Linux. A port must either copy through the `data`
  argument of `rp1_pio_sm_xfer_data` or carry the rp1-pio patch.
- Stock behaviour, read from `drivers/misc/rp1-pio.c` on the rpi-6.18.y
  branch: `rp1_pio_sm_config_xfer` allocates up to four coherent bounce
  buffers of `buf_size` bytes (multiple of 4, non-cyclic count at most 4).
  `rp1_pio_sm_xfer_data` with `dma_addr` 0 uses those buffers and, for
  FROM_SM, runs a kernel `memcpy` into the caller's `data` buffer before
  invoking the callback from the dmaengine completion. The upstream driver's
  24576-byte RX and 16384-byte TX transfers with two buffers fit inside
  those limits, so replacing the two `buffer_virt` uses with a kernel buffer
  passed as `data` is the minimal change.

## Devices and buses seen

- GPIO chips: `gpiochip0` (RP1, 54 lines), `gpiochip10` to `13` (BCM2712
  internal). `/dev/gpiochip4` is a symlink to `gpiochip0`.
- GPIO 4, 5, 6, 12, 18 to 27 are all unclaimed inputs with no alt function.
  These are the pins the Radioberry driver takes over for the RX and TX
  PIO streams and for FPGA configuration.
- No `/dev/spidev*` (disabled by the overlay). `/dev/i2c-1` present since
  2026-11-08, with nothing on the bus.
- PCIe: NVMe on one bus, RP1 south bridge on the other.
- USB: nothing attached beyond the root hubs.
- Power rails measured by the PMIC are nominal; no undervoltage flag.

## Working practice on this machine

- All builds run on the Pi itself over SSH; there is no cross toolchain.
- The only git checkout on the box is the wdsp one used to build piHPSDR.
  New source trees go under the root or the ordinary user's home.
- `tools/ci-pi.sh --remote` keeps its copy of the tree in the root
  account's home directory on the Pi. A blobless clone of the Hermes-Lite 2
  repository, used to locate the gateware source, is also kept there.
- No radioberry service runs, so nothing needs stopping before driver
  work. Once a driver loads, `/dev/radioberry` is exclusive, so only one
  daemon may open it.
- Reboots and config changes are fine at any time; the desktop session is
  not in use for anything that must survive them.
