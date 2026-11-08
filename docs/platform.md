# Platform: the development Raspberry Pi 5

This note describes the machine corvus-radioberry is developed and tested
on. Status 2026-11-08, read from the live system. Host-specific
identifiers (addresses, names, serials) are deliberately omitted.

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
See `pa.md` for the amplifier.

**The FPGA is an Intel Cyclone 10 LP 10CL025**: CL016 images leave
CONF_DONE (GPIO 22) low, CL025 images configure it, and the driver reads
FPGA type 10CL025 from the gateware.

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
| Toolchain | gcc 14.2, cmake 3.31, device-tree-compiler, clang-format 19, lcov 2.0, rsync |
| Libraries | everything in the README requirements (catch2 3.7.1, libyaml-cpp-dev 0.8, nlohmann-json3-dev 3.11, libspdlog-dev 1.15, libliquid-dev 1.7.0, libgpiod-dev 2.2.1, libi2c-dev), plus libfftw3-dev and libgtk-3-dev for piHPSDR |

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

## Kernel facts the driver depends on

- `rp1_pio` is an in-tree module (`drivers/misc/rp1-pio.ko.xz`), loaded at
  boot, instance `pio0`. Its kernel API is
  `include/linux/pio_rp1.h` with argument structs in
  `include/uapi/misc/rp1_pio_if.h` in the kernel headers.
- It exports 51 `rp1_pio_*` symbols, but not `rp1_pio_sm_buffer_virt`,
  which the upstream driver needed from a patched rp1-pio.
- Read from `drivers/misc/rp1-pio.c` on the rpi-6.18.y branch:
  `rp1_pio_sm_config_xfer()` allocates up to four coherent bounce buffers
  per state machine and direction; `rp1_pio_sm_xfer_data()` with
  `dma_addr` 0 never blocks, returns `-EBUSY` when all buffers are in
  flight, allocates with `GFP_KERNEL` (so it must be called from process
  context), copies FROM_SM data into the caller's buffer and then calls
  the caller's callback from the DMA completion (softirq). Cyclic DMA
  exists only through the character device's ioctls.
- `rp1_pio_add_program()` takes programs written for offset 0; the RP1
  firmware relocates jump targets to the load offset.
- `rp1_pio_close()` disables and unclaims the client's state machines and
  frees their DMA.

## Devices and buses seen

- GPIO chips: `gpiochip0` (RP1, 54 lines), `gpiochip10` to `13` (BCM2712
  internal). `/dev/gpiochip4` is a symlink to `gpiochip0`.
- With the driver bound, GPIO 4, 5, 6, 12, 18 to 21 and 25 belong to the
  RP1 PIO (stream pins) and GPIO 13, 22, 24, 26, 27 are held by the driver
  (FPGA configuration).
- No `/dev/spidev*` (disabled by the overlay). `/dev/i2c-1` present since
  2026-11-08, with nothing on the bus.
- PCIe: NVMe on one bus, RP1 south bridge on the other.
- USB: nothing attached beyond the root hubs.
- Power rails measured by the PMIC are nominal; no undervoltage flag.

## Working practice on this machine

- All builds run on the Pi itself over SSH; there is no cross toolchain.
  The gateware is built elsewhere (x86-64, `gateware/QUARTUS.md`).
- `tools/ci-pi.sh --remote` keeps its copy of the tree in the root
  account's home directory and builds there; `cmake --install` from that
  copy is how the installed files above got there.
- Reference clones (Hermes-Lite 2 and the Raspberry Pi kernel's
  `drivers/misc`, both blobless or sparse) are kept in the root account's
  home directory for reading upstream sources.
- `/dev/radioberry` is exclusive. The module can be reloaded with
  `rmmod radioberry; modprobe radioberry` (add `load_gateware=0` to keep
  the FPGA as it is).
- Reboots and config changes are fine at any time; the desktop session is
  not in use for anything that must survive them.
