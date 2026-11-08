# corvus-radioberry

A self-contained software stack for the Radioberry 2.x HF transceiver HAT
on a Raspberry Pi 5: Linux kernel driver, a radio daemon speaking
OpenHPSDR Protocol 1 and Protocol 2 (DSP by liquid-dsp), platform
diagnostics, and control of the AURSINC PA5W V2 amplifier board, with the
FPGA gateware built from its GPL source. Every component is built
from this repository against the current Raspberry Pi OS kernel (6.18 and
newer) with no dependency on out-of-tree kernel patches.

The Radioberry and the upstream software this project builds on are the
work of Johan Maas, PA3GSB. He offers a ready-to-run Raspberry Pi 5 image
of his software for EUR 5 at https://pa3gsb.gumroad.com/l/radioberry. If
the Radioberry is useful to you, buying it supports his work.

The code is C++20, with C only where the kernel or a shared ABI header
requires it. Every component carries Catch2 unit tests with the aim of
complete coverage; the kernel module's pure logic is compiled into a
userspace test binary for the same purpose. All dependencies come from
Debian trixie packages.

## Status (2026-11-08)

| Component | Directory | State |
|---|---|---|
| Platform probe `rbprobe` | `tools/rbprobe/` | working |
| Receive stream tool `rbcat` | `tools/rbcat/` | working |
| FPGA gateware, Hermes-Lite 2 Radioberry PIO variants 75.3 | `gateware/` | CL025 built and verified on hardware; CL016 built, untested, misses AD9866 TX output timing (`gateware/PROVENANCE.md`) |
| Kernel driver `radioberry.ko`, overlay, ABI header | `driver/` | working: loads the gateware at boot, receive streaming verified; transmit path not yet exercised |
| Radio daemon `radioberryd` (OpenHPSDR Protocol 1 and 2, PA control) | `daemon/` | not started |
| Command and protocol tools (`rbctl`, `rbload`, `p1probe`, `p2probe`) | `tools/` | not started |

There is no daemon yet, so no SDR client can use the radio. The design of
the whole stack is in `docs/plan-v1.md`; the open work, in order, is in
`TODO/ACTIVE.md`.

## Target hardware and host

- Raspberry Pi 5 running Raspberry Pi OS (Debian 13 trixie, 64-bit),
  kernel 6.18.60+rpt-rpi-2712 or newer. The stock in-tree `rp1-pio` driver
  is used as it is.
- Radioberry 2.x HAT (AD9866 front end, Intel Cyclone 10 LP 10CL025 or
  10CL016), 0 to 30 MHz.
- AURSINC "Radioberry Preamp Board V2" (PCB marked PA5W V2) stacked on the
  Radioberry: 1.5 to 3.5 W output, manual BIAS1/BIAS2 trimmers, INA199A1
  current sense on the PA_CUR header, no I2C devices.

Details: `docs/platform.md` (development host), `docs/pa.md` (amplifier),
`gateware/README.md` (FPGA interface), `docs/history.md` (the upstream
projects this replaces).

## Requirements on the Pi

```
sudo apt install build-essential cmake device-tree-compiler \
    linux-headers-rpi-2712 libliquid-dev libgpiod-dev libi2c-dev i2c-tools \
    libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev libsystemd-dev \
    catch2 lcov clang-format-19 rsync
```

`/boot/firmware/config.txt` needs:

```
dtparam=i2c_arm=on
dtoverlay=radioberry
```

The `radioberry` overlay is built and installed by this repository. It
enables `spi0` and `rp1_pio`, creates the `sdr,radioberry` device on
`spi0` chip select 0 (SPI mode 3, 12.5 MHz) with the FPGA configuration
GPIOs, and disables the spidev nodes.

## Gateware

The FPGA runs the Hermes-Lite 2 `radioberry_pio_cl025` or
`radioberry_pio_cl016` variant, version 75.3. The bitfiles in
`gateware/rpi-5/<type>/radioberry.rbf` are built here from the unmodified
source in `gateware/source/` with Quartus Prime Lite on an x86-64 machine
(`tools/build-gateware.sh`; toolchain and licence in `gateware/QUARTUS.md`)
and committed, so the Pi only installs them. `gateware/PROVENANCE.md`
records the source commit, fitter seeds, hashes and timing results.

## Build and install

On the Pi, from the repository root:

```
cmake -S . -B build -DFPGATYPE=CL025   # or CL016
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
sudo reboot                            # first install of the overlay only
rbprobe
```

`cmake --install` places:

| What | Where |
|---|---|
| kernel module | `/lib/modules/$(uname -r)/updates/radioberry.ko` (then `depmod`) |
| overlay | `/boot/firmware/overlays/radioberry.dtbo` |
| gateware for `FPGATYPE` | `/lib/firmware/radioberry.rbf` |
| both gateware images | `/usr/local/share/radioberry/gateware/rpi-5/<type>/` |
| ABI header | `/usr/local/include/radioberry_ioctl.h` |
| `rbprobe`, `rbcat` | `/usr/local/bin/` |

The module is built when headers for the running kernel are installed
(`-DKERNEL_RELEASE=` picks another kernel, `-DRADIOBERRY_BUILD_MODULE=OFF`
skips it). `CMakePresets.json` has `default`, `asan` (AddressSanitizer and
UndefinedBehaviorSanitizer) and `coverage` presets;
`cmake --build --preset coverage --target coverage` writes an lcov report
to `build-coverage/coverage/html`.

## Using it

- `rbprobe` reports the Pi, kernel, overlay, driver, installed gateware,
  FPGA type and gateware identity, PA board (I2C scan) and daemon state.
  `--json` and `--yaml` give the same content. Exit status 0 means the
  radio can run, 1 that it cannot (each blocker is listed under
  `findings`), 2 a usage error. Run it as root to see device holders and
  the I2C bus.
- `rbprobe --identify` finds out which FPGA the board carries by loading
  each known rpi-5 gateware image in turn until one configures the chip.
  It reprograms the FPGA, so it refuses to run while the driver is loaded.
  It looks for `.rbf` files in `/usr/local/share/radioberry/gateware`,
  `/usr/share/radioberry/gateware` and `/lib/firmware`; `--gateware-dir`
  replaces that list.
- The driver loads the gateware when it binds, at boot, and publishes the
  gateware identity in `/sys/bus/spi/devices/spi0.0/` (`gateware_version`,
  `fpga_type`, `receivers`, `transmitters`). Module parameters:
  `load_gateware` (default 1), `rx_clkdiv` (2), `tx_clkdiv` (8),
  `rx_ring_kib` (512), `tx_ring_kib` (64).
- `/dev/radioberry` is the interface for the daemon and tools: exclusive
  open, whole RX frames from `read()`, TX samples to `write()`, gateware
  commands, identity and statistics by ioctl. `driver/include/radioberry_ioctl.h`
  documents it. Closing the device sends the gateware a command that
  clears run, PTT, the TX permission bit, PA enable and drive.
- `rbcat` receives from `/dev/radioberry` without the daemon: it sets the
  sample rate (`--rate 48|96|192|384`), receiver count (`--receivers`),
  frequencies (`--freq HZ[,HZ...]`) and LNA gain (`--lna`, -12 to 48 dB),
  streams for `--seconds` (or until SIGINT or SIGTERM) after a settling
  time (`--settle`, 0.5 s), and reports frame rate, RMS and peak level
  per receiver, and the driver's resync, dropped-word and overrun counts
  over the measurement and over the settling time, as text, `--json` or
  `--yaml`. `--output iq24` writes the frames to stdout as read (24-bit
  big-endian I and Q per receiver), `--output iq16` the top 16 bits of
  each value as little-endian int16; the report then goes to stderr.
  `--interval S` prints a progress line every S seconds. `--tx-zeros`
  also writes zero TX samples, keeping about 50 ms queued in the driver,
  and reports the TX word rate, underruns and the lowest queue level; the
  gateware drains and discards them because PTT is clear. It never sets
  PTT or the TX permission bit. Exit status 0 means a clean stream (no
  faults after settling, frame rate within 1%, and with `--tx-zeros` a
  TX word rate within 1% of 48000 per second, no underruns and a queue
  that never ran empty), 1 stream faults or a device error, 2 a usage
  error.
- Receive capacity over the PIO bus is about 2.37 million values per
  second: up to six receivers at 192 ksps or three at 384 ksps.

## Repository layout

```
docs/      design (plan-v1.md), development host, amplifier, upstream survey
gateware/  gateware source, built bitfiles per FPGA, provenance, toolchain
driver/    kernel module, device tree overlay, ABI header, logic tests
daemon/    radioberryd (not started)
tools/     rbprobe, rbcat, shared library, CI and check scripts, gateware build
TODO/      ACTIVE.md (open work) and COMPLETED.md (done work)
```

Development happens on `develop`, one branch per TODO item squash-merged
into it; `main` receives `develop` only at version tags.
`tools/ci-pi.sh` is the merge gate: on the Pi it runs the format check
(`tools/check-format.sh`, clang-format 19), the SPDX check
(`tools/check-spdx.sh`), the release, sanitizer and coverage builds with
their tests, and prints the coverage summary; from a workstation,
`tools/ci-pi.sh --remote [user@]host` copies the working tree to the Pi
and runs it there. Working conventions are in `CONTRIBUTING.md`.

## Licence

This repository is licensed under the GNU General Public License,
version 2 or (at your option) any later version. See `LICENSE`. Every
source file carries an SPDX header:

```
// SPDX-License-Identifier: GPL-2.0-or-later
```

GPL-2.0-or-later is required by the kernel module, which links against
`EXPORT_SYMBOL_GPL` symbols of the Raspberry Pi `rp1-pio` driver, and is
compatible with every upstream component listed below. The repository is
private; the licence governs any future distribution.

## Third-party works and attributions

| Work | Author / holder | Licence | Use here | Reference |
|---|---|---|---|---|
| Radioberry 2.x driver, daemon, overlays, gateware loader | Johan Maas, PA3GSB | Unlicense (public domain) as stated in each source file; no repository-level LICENSE file | ABI, pin map and behaviour reference; code may be reused freely | https://github.com/pa3gsb/Radioberry-2.x |
| Hermes-Lite 2 gateware, Radioberry variants (`radioberry_pio_cl025`, `radioberry_pio_cl016`) | Steve Haynal, KF7O; Johan Maas, PA3GSB | GPL-2.0-or-later (file headers) | Source in `gateware/source/`, unmodified; built here and the bitfiles distributed under `gateware/rpi-5/` | https://github.com/softerhardware/Hermes-Lite2 |
| openHPSDR DSP cores used by Hermes-Lite 2 (CIC, FIR, CORDIC, receiver) | Alex Shovkoplyas VE3NEA; Phil Harman VK6APH; Darrell Harmon; Cathy Moss; Steve Haynal KF7O | LGPL-2.0-or-later ("GNU Library General Public License", file headers) | Contained in the redistributed bitfiles | https://github.com/softerhardware/Hermes-Lite2/tree/master/gateware/rtl/radio_openhpsdr1 |
| Hermes-Lite 2 protocol description | softerhardware project | project wiki | Protocol 1 dialect, PA and bias commands, ADC fields | https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol |
| openHPSDR Ethernet Protocol v4.x (Protocol 2 specification) and Protocol 1 firmware sources | OpenHPSDR project, TAPR; Phil Harman VK6PH | GPL-2.0 (repository LICENSE) | Protocol 2 implementation reference | https://github.com/TAPR/OpenHPSDR-Firmware (Protocol 2/Documentation) |
| Raspberry Pi Linux kernel, `rp1-pio` driver | Raspberry Pi Ltd; Phil Elwell | GPL-2.0 | Kernel API the driver is built on | https://github.com/raspberrypi/linux |
| liquid-dsp | Joseph Gaeddert | MIT | DSP library linked by the daemon | https://github.com/jgaeddert/liquid-dsp |
| piHPSDR | John Melton G0ORX; Christoph van Wullen DL1YCF | GPL-2.0 | Reference client for both protocols; not linked | https://github.com/dl1ycf/pihpsdr |
| wdsp | Warren Pratt NR0V | GPL-2.0 | Not used; listed because the upstream stack depends on it | https://github.com/g0orx/wdsp |
| Radioberry HAT PA30 (preamp and ADC HAT design) | hish8612; JG1TWP; PA3GSB | no licence file in repository | Hardware reference for the PA family | https://github.com/hish8612/Radioberry_hat_pa30 |
| HL2-PA30 (30 W amplifier design) | JI1UDD | no licence file in repository | Hardware reference for the PA family | https://github.com/ji1udd/HL2-PA30 |
| AURSINC Radioberry Preamp Board V2 (PA5W V2) | AURSINC (commercial) | none published; test notes by BG7IYN | The amplifier board in use | https://www.aliexpress.com/item/1005008233825206.html , https://jumbo5566.github.io/pro/rb/2024/04/17/rb016_025.html |
| AD9866 datasheet | Analog Devices | datasheet terms | Front-end reference | https://www.analog.com/en/products/ad9866.html |
| Quartus Prime Lite Edition and its generated IP (PLL, FIFO megafunctions) | Altera (formerly Intel) | Quartus Prime licence agreement, accepted by whoever builds | Toolchain for the gateware build (`gateware/QUARTUS.md`); not redistributed; its generated IP is inside the bitfiles | https://www.altera.com/downloads |

Code taken from an upstream work keeps that work's licence header and
copyright line in addition to the SPDX line above. "Hermes", "Hermes-Lite",
"openHPSDR" and "Radioberry" are the names of their respective projects.
